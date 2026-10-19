#include "lights.h"
#include "coaps.h"
#include "knob_events.h"

#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"

#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr const char *TAG = "lights";
constexpr const char *HOST = CONFIG_LIGHTS_TRADFRI_HOST;
constexpr const char *CODE = CONFIG_LIGHTS_TRADFRI_CODE;
constexpr const char *IGNORE = CONFIG_LIGHTS_IGNORE;
constexpr const char *IGNORE_ROOMS = CONFIG_LIGHTS_IGNORE_ROOMS;
constexpr int PORT = 5684;

// TRADFRI's resources and attributes, as in pytradfri's const.py.
constexpr const char *DEVICES = "15001";
constexpr const char *GROUPS = "15004";
constexpr const char *PAIR = "15011/9063";
constexpr const char *PAIRING_IDENTITY = "Client_identity";
constexpr const char *ATTR_NAME = "9001";
constexpr const char *ATTR_REACHABLE = "9019";
constexpr const char *ATTR_LIGHT = "3311";
constexpr const char *ATTR_PLUG = "3312";
constexpr const char *ATTR_ON = "5850";
constexpr const char *ATTR_DIMMER = "5851";
constexpr const char *ATTR_TRANSITION = "5712";
constexpr const char *ATTR_PSK = "9091";
constexpr int MAX_DIMMER = 254;

constexpr int MAX_LIGHTS = 32;
constexpr size_t REPLY_CAP = 8192;
constexpr uint32_t STACK = 6144;
constexpr UBaseType_t PRIORITY = 3;
constexpr BaseType_t CORE = 0;
// The session is kept for a minute after the last request, so a few turns of
// the knob don't each pay for a handshake.
constexpr uint32_t IDLE_CLOSE_MS = 60000;
// A room command is a Zigbee broadcast. Zigbee allows about 8 broadcasts in
// any 9 s across the whole network and silently drops the rest, though the
// gateway still acknowledges them. So the first few detents of a turn go out
// at once, and after that a command goes at most once per COMMAND_GAP_MS,
// its latest level each time. Other traffic counts against the same budget,
// so the burst stays well under it.
constexpr int BURST = 2;
constexpr uint32_t COMMAND_GAP_MS = 1500;
// No fade: an IKEA bulb ignores every command that arrives while it's still
// fading, so a fade swallows the next detents.
constexpr int FADE = 0;
// A room this small is reached light by light, which costs no broadcast.
constexpr int SMALL_ROOM = 3;
// Once the knob rests this long the lights are read back, and any that missed
// the level get it again one by one, this far apart: the gateway answers 5.03
// to per-light commands much faster than ten a second.
constexpr uint32_t SETTLE_MS = 1000;
constexpr uint32_t CORRECTION_GAP_MS = 100;
constexpr int CORRECTIONS = 2;

constexpr const char *NVS_NAMESPACE = "lights";

enum Work : uint32_t { START = 1, REFRESH = 2, COMMAND = 4, VERIFY = 8 };
// The latest thing asked for wins, as one value: a level 0..100 for all at
// one brightness (0 is off), or on with each light at its own brightness.
constexpr int ON_OWN_LEVELS = -1;

// A bulb, or an outlet with a lamp on it, which only switches.
struct Light {
  int id;
  bool outlet, reachable, on;
  int dimmer;
};

// What one command reaches: a whole room by its id, or one light by its
// index in s_lights, when its room also holds an ignored light.
struct Target {
  bool group;
  int id;
};

struct Credentials {
  char identity[32];
  char psk[48];
};

TaskHandle_t s_task;
portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
LightsState s_state = {HOST[0] ? LightsStatus::Connecting : LightsStatus::Unset,
                       false, 0, 0};
std::atomic<int> s_wanted{0};
// Bumped by every request from the UI, so a slow answer to an older one
// doesn't show over what was asked for since.
std::atomic<uint32_t> s_asked;

// The task's own.
CoapsSession *s_session;
char *s_reply;
bool s_mapped;
Light s_lights[MAX_LIGHTS];
int s_light_count;
int s_ignored[MAX_LIGHTS];
int s_ignored_count;
Target s_targets[MAX_LIGHTS * 2];
int s_target_count;
// When the budget is back to full, less a burst: a command may go once the
// clock reaches it. Each command moves it a gap on.
TickType_t s_budget_at;
int s_corrections_left;
// The UI shows what it asked for; a read only replaces that when it's news:
// a plain refresh, or lights that still missed after every correction.
bool s_news;

// ─── State ──────────────────────────────────────────────────────────────────

void post(const LightsState &state) {
  esp_event_post(APP_EVENT, APP_EVENT_LIGHTS_CHANGED, &state, sizeof(state), 0);
}

void set_state(const LightsState &state) {
  taskENTER_CRITICAL(&s_lock);
  s_state = state;
  taskEXIT_CRITICAL(&s_lock);
  post(state);
}

int dimmer_of(int level) {
  return std::max(1, (level * MAX_DIMMER + 50) / 100);
}

int percent(int dimmer) {
  return std::clamp((dimmer * 100 + MAX_DIMMER / 2) / MAX_DIMMER, 1, 100);
}

LightsState gathered() {
  LightsState state = {LightsStatus::Ready, false, 0, s_light_count};
  int on = 0, dimmed = 0, sum = 0;
  for (int i = 0; i < s_light_count; i++) {
    const Light &l = s_lights[i];
    if (!l.reachable || !l.on)
      continue;
    on++;
    if (!l.outlet) {
      dimmed++;
      sum += l.dimmer;
    }
  }
  state.on = on > 0;
  state.level = !on ? 0 : dimmed ? percent(sum / dimmed) : 100;
  return state;
}

// Exactly what the UI asked for, so its number doesn't step by the rounding
// between percent and the bulbs' 0..254. Switched on, each light is at its
// own level, which only the lights can tell.
LightsState as_asked(int wanted) {
  if (wanted == ON_OWN_LEVELS)
    return gathered();
  return {LightsStatus::Ready, wanted > 0, wanted, s_light_count};
}

void set_offline() {
  LightsState state = lights_state();
  state.status = LightsStatus::Offline;
  set_state(state);
}

// ─── Gateway ────────────────────────────────────────────────────────────────

bool load_credentials(Credentials *c) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK)
    return false;
  size_t id_len = sizeof(c->identity), psk_len = sizeof(c->psk);
  bool ok = nvs_get_str(nvs, "identity", c->identity, &id_len) == ESP_OK &&
            nvs_get_str(nvs, "psk", c->psk, &psk_len) == ESP_OK;
  nvs_close(nvs);
  return ok;
}

void save_credentials(const Credentials *c) {
  nvs_handle_t nvs;
  if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK)
    return;
  nvs_set_str(nvs, "identity", c->identity);
  nvs_set_str(nvs, "psk", c->psk);
  nvs_commit(nvs);
  nvs_close(nvs);
}

CoapsOpen open_with(const char *identity, const char *key) {
  return coaps_open(HOST, PORT, identity,
                    reinterpret_cast<const uint8_t *>(key), strlen(key),
                    &s_session);
}

// The gateway hands out a key for a new identity to whoever knows the code
// on its label. The code is used for nothing else.
bool pair(Credentials *c) {
  if (!CODE[0]) {
    ESP_LOGW(TAG, "Not paired, and no CONFIG_LIGHTS_TRADFRI_CODE to pair with");
    return false;
  }
  snprintf(c->identity, sizeof(c->identity), "knob-%08lx",
           static_cast<unsigned long>(esp_random()));
  if (open_with(PAIRING_IDENTITY, CODE) != CoapsOpen::Ok)
    return false;
  char body[64];
  snprintf(body, sizeof(body), R"({"9090":"%s"})", c->identity);
  int code = coaps_request(s_session, CoapMethod::Post, PAIR, body, s_reply,
                           REPLY_CAP);
  coaps_close(s_session);
  s_session = nullptr;
  cJSON *answer = code == 201 ? cJSON_Parse(s_reply) : nullptr;
  cJSON *psk = answer ? cJSON_GetObjectItem(answer, ATTR_PSK) : nullptr;
  bool ok = cJSON_IsString(psk) && strlen(psk->valuestring) < sizeof(c->psk);
  if (ok) {
    snprintf(c->psk, sizeof(c->psk), "%s", psk->valuestring);
    save_credentials(c);
    ESP_LOGI(TAG, "Paired with the gateway as %s", c->identity);
  } else {
    ESP_LOGW(TAG, "Pairing failed (%d): %s", code, s_reply);
  }
  cJSON_Delete(answer);
  return ok;
}

bool connect() {
  if (s_session)
    return true;
  Credentials c = {};
  bool known = load_credentials(&c);
  if (!known && !pair(&c))
    return false;
  CoapsOpen opened = open_with(c.identity, c.psk);
  // A gateway that was reset has forgotten the knob.
  if (opened == CoapsOpen::Refused && known && pair(&c))
    opened = open_with(c.identity, c.psk);
  return opened == CoapsOpen::Ok;
}

void disconnect() {
  coaps_close(s_session);
  s_session = nullptr;
}

cJSON *get(const char *path) {
  int code = coaps_request(s_session, CoapMethod::Get, path, nullptr, s_reply,
                           REPLY_CAP);
  if (code != 205) {
    ESP_LOGW(TAG, "GET %s: %d", path, code);
    return nullptr;
  }
  return cJSON_Parse(s_reply);
}

bool put(const char *path, const char *body) {
  int code =
      coaps_request(s_session, CoapMethod::Put, path, body, s_reply, REPLY_CAP);
  if (code != 204)
    ESP_LOGW(TAG, "PUT %s: %d", path, code);
  return code != 0;
}

int int_of(cJSON *obj, const char *key, int fallback) {
  cJSON *item = obj ? cJSON_GetObjectItem(obj, key) : nullptr;
  return cJSON_IsNumber(item) ? item->valueint : fallback;
}

cJSON *control_of(cJSON *device, const char *kind) {
  return cJSON_GetArrayItem(cJSON_GetObjectItem(device, kind), 0);
}

void read_light(cJSON *device, Light *l) {
  cJSON *control = control_of(device, l->outlet ? ATTR_PLUG : ATTR_LIGHT);
  l->reachable = int_of(device, ATTR_REACHABLE, 1) == 1;
  l->on = int_of(control, ATTR_ON, 0) == 1;
  l->dimmer = l->outlet ? MAX_DIMMER : int_of(control, ATTR_DIMMER, 0);
}

// ─── Which lights ───────────────────────────────────────────────────────────

bool contains(const char *name, const char *part) {
  if (!part[0] || !name)
    return false;
  size_t n = strlen(part);
  for (const char *at = name; *at; at++)
    if (strncasecmp(at, part, n) == 0)
      return true;
  return false;
}

const char *name_of(cJSON *item) {
  cJSON *name = cJSON_GetObjectItem(item, ATTR_NAME);
  return cJSON_IsString(name) ? name->valuestring : "";
}

cJSON *members_of(cJSON *group) {
  return cJSON_GetObjectItem(
      cJSON_GetObjectItem(cJSON_GetObjectItem(group, "9018"), "15002"), "9003");
}

int light_index(int id) {
  for (int i = 0; i < s_light_count; i++)
    if (s_lights[i].id == id)
      return i;
  return -1;
}

bool is_ignored(int id) {
  return std::find(s_ignored, s_ignored + s_ignored_count, id) !=
         s_ignored + s_ignored_count;
}

template <typename Fn> bool for_each_id(const char *root, Fn fn) {
  cJSON *ids = get(root);
  if (!cJSON_IsArray(ids)) {
    cJSON_Delete(ids);
    return false;
  }
  bool ok = true;
  cJSON *id;
  cJSON_ArrayForEach(id, ids) {
    char path[24];
    snprintf(path, sizeof(path), "%s/%d", root, id->valueint);
    cJSON *item = get(path);
    if (!item) {
      ok = false;
      break;
    }
    fn(id->valueint, item);
    cJSON_Delete(item);
  }
  cJSON_Delete(ids);
  return ok;
}

bool find_lights() {
  s_light_count = s_ignored_count = 0;
  return for_each_id(DEVICES, [](int id, cJSON *device) {
    bool outlet = control_of(device, ATTR_PLUG) != nullptr;
    if (!outlet && !control_of(device, ATTR_LIGHT))
      return;
    const char *text = name_of(device);
    if (contains(text, IGNORE) || s_light_count == MAX_LIGHTS) {
      s_ignored[s_ignored_count++] = id;
      ESP_LOGI(TAG, "Leaving %s alone", text);
      return;
    }
    Light &l = s_lights[s_light_count++];
    l.id = id;
    l.outlet = outlet;
    read_light(device, &l);
  });
}

void leave_alone(int id) {
  int i = light_index(id);
  if (i < 0 || s_ignored_count == MAX_LIGHTS)
    return;
  s_ignored[s_ignored_count++] = id;
  std::copy(s_lights + i + 1, s_lights + s_light_count, s_lights + i);
  s_light_count--;
}

bool leave_rooms() {
  if (!IGNORE_ROOMS[0])
    return true;
  return for_each_id(GROUPS, [](int, cJSON *group) {
    if (!contains(name_of(group), IGNORE_ROOMS))
      return;
    ESP_LOGI(TAG, "Leaving %s alone", name_of(group));
    cJSON *m;
    cJSON_ArrayForEach(m, members_of(group)) leave_alone(m->valueint);
  });
}

// A big room is switched whole, in one broadcast, so its lights change
// together. A small room, a room with an ignored light in it, and any light
// in no room are switched light by light.
bool find_targets() {
  bool covered[MAX_LIGHTS] = {};
  s_target_count = 0;
  bool ok = for_each_id(GROUPS, [&](int id, cJSON *group) {
    cJSON *members = members_of(group);
    bool has_ignored = false;
    int lights = 0;
    cJSON *m;
    cJSON_ArrayForEach(m, members) {
      has_ignored |= is_ignored(m->valueint);
      lights += light_index(m->valueint) >= 0;
    }

    if (has_ignored || lights <= SMALL_ROOM)
      return;
    s_targets[s_target_count++] = {true, id};
    cJSON_ArrayForEach(m, members) {
      int i = light_index(m->valueint);
      if (i >= 0)
        covered[i] = true;
    }
  });
  for (int i = 0; i < s_light_count; i++)
    if (!covered[i])
      s_targets[s_target_count++] = {false, i};
  return ok;
}

bool map_lights() {
  s_mapped = find_lights() && leave_rooms() && find_targets();
  if (s_mapped)
    ESP_LOGI(TAG, "%d lights, %d commands to reach them, %d left alone",
             s_light_count, s_target_count, s_ignored_count);
  return s_mapped;
}

bool refresh() {
  for (int i = 0; i < s_light_count; i++) {
    char path[24];
    snprintf(path, sizeof(path), "%s/%d", DEVICES, s_lights[i].id);
    cJSON *device = get(path);
    if (!device)
      return false;
    read_light(device, &s_lights[i]);
    cJSON_Delete(device);
  }

  return true;
}

// What a command sets: `bulb` for a bulb or a room, `outlet` for an outlet,
// which takes no brightness. A brightness alone also switches a bulb on.
struct Attrs {
  char bulb[48], outlet[16];
};

Attrs attrs_for(int wanted) {
  Attrs a;
  bool on = wanted != 0;
  snprintf(a.outlet, sizeof(a.outlet), R"("%s":%d)", ATTR_ON, on ? 1 : 0);
  if (wanted > 0)
    snprintf(a.bulb, sizeof(a.bulb), R"("%s":%d,"%s":%d)", ATTR_DIMMER,
             dimmer_of(wanted), ATTR_TRANSITION, FADE);
  else
    snprintf(a.bulb, sizeof(a.bulb), "%s", a.outlet);
  return a;
}

bool send_light(const Light &l, const Attrs &a) {
  char path[24], body[80];
  snprintf(path, sizeof(path), "%s/%d", DEVICES, l.id);
  snprintf(body, sizeof(body), R"({"%s":[{%s}]})",
           l.outlet ? ATTR_PLUG : ATTR_LIGHT, l.outlet ? a.outlet : a.bulb);
  return put(path, body);
}

bool send_all(const Attrs &a) {
  for (int i = 0; i < s_target_count; i++) {
    const Target &t = s_targets[i];
    if (!t.group) {
      if (!send_light(s_lights[t.id], a))
        return false;
      continue;
    }
    char path[24], body[56];
    snprintf(path, sizeof(path), "%s/%d", GROUPS, t.id);
    snprintf(body, sizeof(body), "{%s}", a.bulb);
    if (!put(path, body))
      return false;
  }
  return true;
}

void assume(Light &l, int wanted) {
  l.on = wanted != 0;
  if (wanted > 0 && !l.outlet)
    l.dimmer = dimmer_of(wanted);
}

bool shows(const Light &l, int wanted) {
  if (!l.reachable)
    return true;
  if (wanted == 0 || wanted == ON_OWN_LEVELS)
    return l.on == (wanted != 0);
  return l.on && (l.outlet || std::abs(l.dimmer - dimmer_of(wanted)) <= 2);
}

bool all_show(int wanted) {
  return std::all_of(s_lights, s_lights + s_light_count,
                     [&](const Light &l) { return shows(l, wanted); });
}

bool command(int wanted) {
  TickType_t now = xTaskGetTickCount();
  TickType_t from =
      static_cast<int32_t>(s_budget_at - now) > 0 ? s_budget_at : now;
  s_budget_at = from + pdMS_TO_TICKS(COMMAND_GAP_MS);
  if (!send_all(attrs_for(wanted)))
    return false;
  for (int i = 0; i < s_light_count; i++)
    assume(s_lights[i], wanted);
  return true;
}

// Broadcasts reach most bulbs but not always all, so a miss is sent again to
// that bulb alone, which the bulb acknowledges. Not as a room command: by
// the end of a turn the broadcast budget is spent, and it would be lost too.
bool correct(int wanted) {
  Attrs a = attrs_for(wanted);
  for (int i = 0; i < s_light_count; i++) {
    Light &l = s_lights[i];
    if (shows(l, wanted))
      continue;
    if (!send_light(l, a))
      return false;
    assume(l, wanted);
    vTaskDelay(pdMS_TO_TICKS(CORRECTION_GAP_MS));
  }
  return true;
}

// Switched on, each light comes back at its own last brightness, which only
// the read after it settles can tell.
bool run(uint32_t work) {
  if (!connect())
    return false;
  bool mapped_now = !s_mapped;
  if (mapped_now && !map_lights())
    return false;
  int wanted = s_wanted;
  s_news = false;
  if (work & COMMAND) {
    s_corrections_left = CORRECTIONS;
    return command(wanted);
  }
  if (work & VERIFY) {
    if (!refresh())
      return false;
    if (all_show(wanted)) {
      s_corrections_left = 0;
      return true;
    }
    if (s_corrections_left == 0) {
      s_news = true;
      return true;
    }
    s_corrections_left--;
    return correct(wanted);
  }
  s_news = true;
  return mapped_now || refresh();
}

// Sends a command at once while the burst lasts. After that it's held back
// until the budget allows, gathering whatever else is asked meanwhile.
uint32_t paced(uint32_t work) {
  if (!(work & COMMAND))
    return work;
  TickType_t earliest =
      s_budget_at - pdMS_TO_TICKS(COMMAND_GAP_MS * (BURST - 1));
  auto wait = static_cast<int32_t>(earliest - xTaskGetTickCount());
  if (wait <= 0)
    return work;
  vTaskDelay(static_cast<TickType_t>(wait));
  uint32_t more = 0;
  xTaskNotifyWait(0, UINT32_MAX, &more, 0);
  return work | more;
}

TickType_t next_wait() {
  if (s_corrections_left > 0)
    return pdMS_TO_TICKS(SETTLE_MS);
  return s_session ? pdMS_TO_TICKS(IDLE_CLOSE_MS) : portMAX_DELAY;
}

void task(void *) {
  s_reply = static_cast<char *>(
      heap_caps_malloc(REPLY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  for (;;) {
    uint32_t work = 0;
    if (!xTaskNotifyWait(0, UINT32_MAX, &work, next_wait())) {
      if (s_corrections_left > 0) {
        work = VERIFY;
      } else {
        disconnect();
        continue;
      }
    }
    work = paced(work);
    if (work & COMMAND)
      work &= ~VERIFY;
    uint32_t asked = s_asked;
    // A session the gateway dropped fails its first request; one retry on a
    // new one tells that apart from a gateway that's gone.
    bool ok = run(work);
    if (!ok) {
      disconnect();
      ok = run(work);
    }
    if (!ok) {
      s_corrections_left = 0;
      disconnect();
      set_offline();
    } else if (asked == s_asked) {
      set_state(s_news ? gathered() : as_asked(s_wanted));
    }
  }
}

void ask(uint32_t work) {
  if (s_task && HOST[0])
    xTaskNotify(s_task, work, eSetBits);
}

// The UI shows what it asked for itself; the gateway's state follows once the
// lights have caught up with the last request.
void want(int wanted) {
  s_asked++;
  s_wanted = wanted;
  ask(COMMAND);
}

} // namespace

void lights_init() {}

// The task saves its key to flash, which a task with its stack in PSRAM may
// not do, so the stack is internal. It's made once Wi-Fi is up, after Wi-Fi
// has taken the internal buffers it needs.
void lights_start() {
  if (HOST[0] && !s_task)
    xTaskCreatePinnedToCore(task, "lights", STACK, nullptr, PRIORITY, &s_task,
                            CORE);
  ask(START);
}

void lights_refresh() { ask(REFRESH); }

void lights_set_on(bool on) { want(on ? ON_OWN_LEVELS : 0); }

void lights_set_level(int level) { want(std::clamp(level, 0, 100)); }

LightsState lights_state() {
  taskENTER_CRITICAL(&s_lock);
  LightsState state = s_state;
  taskEXIT_CRITICAL(&s_lock);
  return state;
}

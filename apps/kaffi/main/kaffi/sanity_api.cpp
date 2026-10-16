#include "kaffi/sanity_api.h"
#include "app_config.h"
#include "kaffi/prefs.h"

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "art_decoder.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

static constexpr const char *TAG = "sanity";

ESP_EVENT_DECLARE_BASE(APP_EVENT);

// ─── Our project (coffee events) — compiled in via Kconfig, safe fallbacks
#ifndef CONFIG_KAFFI_SANITY_PROJECT_ID
#define CONFIG_KAFFI_SANITY_PROJECT_ID "j7764jbe"
#endif
#ifndef CONFIG_KAFFI_SANITY_DATASET
#define CONFIG_KAFFI_SANITY_DATASET "production"
#endif
#ifndef CONFIG_KAFFI_SANITY_API_VERSION
#define CONFIG_KAFFI_SANITY_API_VERSION "2024-01-01"
#endif
#ifndef CONFIG_KAFFI_SANITY_TOKEN
#define CONFIG_KAFFI_SANITY_TOKEN ""
#endif

// ─── Employee directory (people, read-only)
#ifndef CONFIG_KAFFI_DIR_PROJECT_ID
#define CONFIG_KAFFI_DIR_PROJECT_ID "r3dzy7he"
#endif
#ifndef CONFIG_KAFFI_DIR_DATASET
#define CONFIG_KAFFI_DIR_DATASET "production"
#endif
#ifndef CONFIG_KAFFI_DIR_API_VERSION
#define CONFIG_KAFFI_DIR_API_VERSION "2026-02-01"
#endif
#ifndef CONFIG_KAFFI_DIR_TOKEN
#define CONFIG_KAFFI_DIR_TOKEN ""
#endif

static constexpr int RESP_BUF_SIZE = 98304; // holds one page of events
static constexpr int EVENTS_PAGE_SIZE = 400;
static constexpr int EVENTS_MAX_PAGES = 25;

// Streak bitmaps: bit N = "person was active N days ago" (local days). 366
// bits covers the full yearly event window.
static constexpr int STREAK_DAYS = 366;
static constexpr int STREAK_WORDS = (STREAK_DAYS + 31) / 32;
using DayBits = uint32_t[STREAK_WORDS];

// ─── HTTP response accumulator

struct Response {
  char *buf;
  int len;
  int cap;
};

static esp_err_t on_http_event(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data &&
      evt->data_len > 0) {
    auto *r = static_cast<Response *>(evt->user_data);
    int space = r->cap - r->len - 1;
    if (space > 0) {
      int n = evt->data_len < space ? evt->data_len : space;
      memcpy(r->buf + r->len, evt->data, n);
      r->len += n;
      r->buf[r->len] = '\0';
    }
  }
  return ESP_OK;
}

// ─── Minimal JSON extraction (operates on a null-terminated object substring)

static const char *skip_ws(const char *p) {
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

static const char *json_find_key(const char *json, const char *key) {
  char pat[64];
  snprintf(pat, sizeof(pat), "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p)
    return nullptr;
  p = skip_ws(p + strlen(pat));
  if (*p != ':')
    return nullptr;
  return skip_ws(p + 1);
}

static int hex_val(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

// Copy a JSON string value, decoding standard escapes and \uXXXX (to UTF-8).
static bool json_str(const char *json, const char *key, char *out,
                     size_t out_len) {
  const char *p = json_find_key(json, key);
  if (!p || *p != '"')
    return false;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < out_len) {
    if (*p != '\\') {
      out[i++] = *p++;
      continue;
    }
    p++;
    if (!*p)
      break;
    char esc = *p++;
    switch (esc) {
    case 'n':
      out[i++] = '\n';
      break;
    case 't':
      out[i++] = '\t';
      break;
    case 'r':
    case 'b':
    case 'f':
      break; // dropped — never meaningful in names/URLs
    case 'u': {
      if (!p[0] || !p[1] || !p[2] || !p[3])
        break; // truncated escape at end of buffer
      int h0 = hex_val(p[0]), h1 = hex_val(p[1]);
      int h2 = hex_val(p[2]), h3 = hex_val(p[3]);
      if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
        break;
      int cp = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
      p += 4;
      if (cp >= 0xD800 && cp <= 0xDFFF)
        break;
      if (cp < 0x80) {
        out[i++] = static_cast<char>(cp);
      } else if (cp < 0x800 && i + 2 < out_len) {
        out[i++] = static_cast<char>(0xC0 | (cp >> 6));
        out[i++] = static_cast<char>(0x80 | (cp & 0x3F));
      } else if (i + 3 < out_len) {
        out[i++] = static_cast<char>(0xE0 | (cp >> 12));
        out[i++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[i++] = static_cast<char>(0x80 | (cp & 0x3F));
      }
      break;
    }
    default:
      out[i++] = esc;
      break;
    }
  }
  out[i] = '\0';
  return true;
}

static bool json_int(const char *json, const char *key, int *out) {
  const char *p = json_find_key(json, key);
  if (!p)
    return false;
  if (strncmp(p, "null", 4) == 0) {
    *out = 0;
    return true;
  }
  *out = atoi(p);
  return true;
}

// ─── ASCII folding for Latin-1 names (device font is ASCII-only)

static const char *fold_cp(int cp) {
  switch (cp) {
  case 0xC6:
    return "AE";
  case 0xE6:
    return "ae";
  case 0xDF:
    return "ss";
  default:
    break;
  }
  static char one[2];
  char c = '?';
  if (cp >= 0xC0 && cp <= 0xC5)
    c = 'A';
  else if (cp == 0xC7)
    c = 'C';
  else if (cp >= 0xC8 && cp <= 0xCB)
    c = 'E';
  else if (cp >= 0xCC && cp <= 0xCF)
    c = 'I';
  else if (cp == 0xD0)
    c = 'D';
  else if (cp == 0xD1)
    c = 'N';
  else if ((cp >= 0xD2 && cp <= 0xD6) || cp == 0xD8)
    c = 'O';
  else if (cp >= 0xD9 && cp <= 0xDC)
    c = 'U';
  else if (cp == 0xDD)
    c = 'Y';
  else if (cp >= 0xE0 && cp <= 0xE5)
    c = 'a';
  else if (cp == 0xE7)
    c = 'c';
  else if (cp >= 0xE8 && cp <= 0xEB)
    c = 'e';
  else if (cp >= 0xEC && cp <= 0xEF)
    c = 'i';
  else if (cp == 0xF0)
    c = 'd';
  else if (cp == 0xF1)
    c = 'n';
  else if ((cp >= 0xF2 && cp <= 0xF6) || cp == 0xF8)
    c = 'o';
  else if (cp >= 0xF9 && cp <= 0xFC)
    c = 'u';
  else if (cp == 0xFD || cp == 0xFF)
    c = 'y';
  one[0] = c;
  one[1] = '\0';
  return one;
}

// Rewrite a UTF-8 string in place, folding Latin-1 letters to ASCII and
// dropping other non-ASCII (so ø→o, æ→ae, å→a, é→e …).
static void ascii_fold(char *s) {
  const unsigned char *p = reinterpret_cast<unsigned char *>(s);
  char *o = s;
  while (*p) {
    if (*p < 0x80) {
      *o++ = static_cast<char>(*p++);
    } else if ((*p & 0xE0) == 0xC0 && p[1]) {
      int cp = ((*p & 0x1F) << 6) | (p[1] & 0x3F);
      p += 2;
      for (const char *r = fold_cp(cp); *r; r++)
        *o++ = *r;
    } else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
      p += 3; // drop 3-byte sequences (smart quotes, emoji, …)
    } else {
      p++;
    }
  }
  *o = '\0';
}

// ─── Object-array walker: copy the next {...} into `obj`, advance *pp.
// Returns false when no more objects.
static bool next_object(const char **pp, char *obj, size_t obj_cap) {
  const char *p = *pp;
  while (*p && *p != '{' && *p != ']')
    p++;
  if (*p != '{')
    return false;
  const char *start = p;
  int depth = 0;
  bool in_str = false;
  for (; *p; p++) {
    char c = *p;
    if (in_str) {
      if (c == '\\') {
        if (p[1])
          p++;
      } else if (c == '"') {
        in_str = false;
      }
      continue;
    }
    if (c == '"')
      in_str = true;
    else if (c == '{')
      depth++;
    else if (c == '}') {
      if (--depth == 0) {
        p++;
        break;
      }
    }
  }
  auto len = static_cast<size_t>(p - start);
  *pp = p;
  if (len >= obj_cap) {
    obj[0] = '\0'; // never leave the previous object behind
    return true;   // skip oversized but keep walking
  }
  memcpy(obj, start, len);
  obj[len] = '\0';
  return true;
}

// Parse the directory people list into Person[] (id, name folded, image).
static int parse_people(const char *json, Person *arr, int max) {
  const char *res = strstr(json, "\"result\"");
  if (!res)
    return 0;
  const char *p = strchr(res, '[');
  if (!p)
    return 0;
  p++;

  int count = 0;
  char obj[640];
  while (count < max && next_object(&p, obj, sizeof(obj))) {
    if (obj[0] == '\0')
      continue;
    Person *person = &arr[count];
    memset(person, 0, sizeof(*person));
    if (json_str(obj, "_id", person->id, sizeof(person->id)) &&
        json_str(obj, "name", person->name, sizeof(person->name))) {
      ascii_fold(person->name);
      json_str(obj, "image", person->image_url, sizeof(person->image_url));
      count++;
    }
    if (*p == ']')
      break;
  }
  return count;
}

// ─── Streaks (calendar day math)

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
static long days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  long era = (y >= 0 ? y : y - 399) / 400;
  auto yoe = static_cast<unsigned>(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<long>(doe) - 719468;
}

// Local calendar day number for a UTC instant (-1 if the clock isn't synced).
static long local_daynum(time_t t) {
  struct tm lt;
  localtime_r(&t, &lt);
  if (lt.tm_year + 1900 < 2023)
    return -1;
  return days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

// Parse an ISO-8601 UTC timestamp ("2026-08-10T07:31:22Z") into a time_t.
static bool parse_iso_utc(const char *iso, time_t *out) {
  int y, mo, d, h, mi, s;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6)
    return false;
  *out = days_from_civil(y, mo, d) * 86400L + h * 3600 + mi * 60 + s;
  return true;
}

// Consecutive brew days ending today (or yesterday — a day you haven't
// brewed yet doesn't break the run). Quiet Saturdays and Sundays are skipped
// rather than breaking the streak: nobody brews at the office on weekends.
static int compute_streak(const uint32_t *days, long today) {
  int streak = 0;
  for (int d = (days[0] & 1u) ? 0 : 1; d < STREAK_DAYS; d++) {
    bool active = (days[d / 32] >> (d % 32)) & 1u;
    int wd = static_cast<int>((today - d + 4) % 7); // 0 = Sunday, 6 = Saturday
    if (active)
      streak++;
    else if (wd != 0 && wd != 6)
      break;
  }
  return streak;
}

// Bucket coffee events onto people by personId (the directory _id), marking
// each person's brew-day bitmap (indexed like st->people) for streaks.
// Returns the number of event objects in the page and writes the last
// event's _id into `last_id` (the cursor for the next page).
static int aggregate_events(const char *json, KaffiState *st, DayBits *days,
                            long today, char *last_id, size_t last_id_len) {
  const char *res = strstr(json, "\"result\"");
  if (!res)
    return 0;
  const char *p = strchr(res, '[');
  if (!p)
    return 0;
  p++;

  int seen = 0;
  char obj[360];
  while (next_object(&p, obj, sizeof(obj))) {
    seen++;
    if (obj[0]) {
      char pid[KAFFI_ID_LEN] = {};
      char kind[8] = {};
      char at[32] = {};
      int qty = 0;
      json_str(obj, "_id", last_id, last_id_len);
      if (json_str(obj, "personId", pid, sizeof(pid))) {
        json_str(obj, "kind", kind, sizeof(kind));
        json_str(obj, "occurredAt", at, sizeof(at));
        json_int(obj, "quantity", &qty);

        for (int i = 0; i < st->count; i++) {
          if (strcmp(st->people[i].id, pid) != 0)
            continue;
          Person *pe = &st->people[i];
          if (strcmp(kind, "brew") == 0) {
            pe->pots += qty;
            pe->credit += kaffi_brew_credit(qty);
            // Only brews feed the streak — it rewards making coffee, not
            // drinking it.
            time_t t;
            if (today >= 0 && at[0] && parse_iso_utc(at, &t)) {
              long ago = today - local_daynum(t);
              if (ago >= 0 && ago < STREAK_DAYS)
                days[i][ago / 32] |= 1u << (ago % 32);
            }
          } else if (strcmp(kind, "cup") == 0) {
            pe->cups += qty;
          }
          break;
        }
      }
    }
    if (*p == ']')
      break;
  }
  return seen;
}

// Alphabetical, always — a stable order is what makes the knob predictable
// ("my face is about 8 clicks in"), so never re-sort by activity.
static void sort_people(KaffiState *st) {
  for (int i = 1; i < st->count; i++) {
    Person key = st->people[i];
    int j = i - 1;
    while (j >= 0 && strcmp(st->people[j].name, key.name) > 0) {
      st->people[j + 1] = st->people[j];
      j--;
    }
    st->people[j + 1] = key;
  }
}

// ─── URL encoding (percent-encode everything but RFC 3986 unreserved chars)

static void url_encode(const char *src, char *dst, size_t dst_cap) {
  static const char *hex = "0123456789ABCDEF";
  size_t j = 0;
  for (size_t i = 0; src[i] && j + 4 < dst_cap; i++) {
    auto c = static_cast<unsigned char>(src[i]);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[j++] = static_cast<char>(c);
    } else {
      dst[j++] = '%';
      dst[j++] = hex[c >> 4];
      dst[j++] = hex[c & 0x0F];
    }
  }
  dst[j] = '\0';
}

static void iso_now(char *out, size_t out_len) {
  time_t now = time(nullptr);
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  if (tm_utc.tm_year + 1900 < 2023) {
    out[0] = '\0';
    return;
  }
  strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
}

static void year_start_iso(char *out, size_t out_len) {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  if (lt.tm_year + 1900 < 2023) {
    out[0] = '\0';
    return;
  }
  lt.tm_mon = 0;
  lt.tm_mday = 1;
  lt.tm_hour = 0;
  lt.tm_min = 0;
  lt.tm_sec = 0;
  time_t jan1 = mktime(&lt);
  struct tm utc;
  gmtime_r(&jan1, &utc);
  strftime(out, out_len, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

// ─── Authenticated request (token chosen by caller for cross-project reads)

static int api_request(const char *url, esp_http_client_method_t method,
                       char *resp_buf, int resp_cap, const char *body,
                       const char *token) {
  Response resp = {resp_buf, 0, resp_cap};
  if (resp_buf)
    resp_buf[0] = '\0';

  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = method;
  cfg.timeout_ms = KAFFI_HTTP_TIMEOUT_MS;
  cfg.event_handler = on_http_event;
  cfg.user_data = resp_buf ? &resp : nullptr;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;
  cfg.buffer_size_tx = 2048;

  auto *client = esp_http_client_init(&cfg);
  if (!client) {
    ESP_LOGE(TAG, "http client init failed for %s", url);
    return -1;
  }

  char auth[600];
  snprintf(auth, sizeof(auth), "Bearer %s", token);
  esp_http_client_set_header(client, "Authorization", auth);

  if (body) {
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
  }

  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP error %d for %s", err, url);
    return -1;
  }
  return status;
}

// ─── Public API

void sanity_api_init() {
  if (CONFIG_KAFFI_SANITY_TOKEN[0] == '\0')
    ESP_LOGW(TAG, "No coffee-project token set — writes will fail");
  if (CONFIG_KAFFI_DIR_TOKEN[0] == '\0')
    ESP_LOGW(TAG, "No directory token set — people won't load");
  int office_idx = kaffi_office_get();
  ESP_LOGI(TAG, "Sanity ready (events=%s, directory=%s, office=%s)",
           CONFIG_KAFFI_SANITY_PROJECT_ID, CONFIG_KAFFI_DIR_PROJECT_ID,
           KAFFI_OFFICES[office_idx < 0 ? 0 : office_idx].name);
}

bool sanity_api_fetch_people() {
  auto *buf =
      static_cast<char *>(heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM));
  auto *st = static_cast<KaffiState *>(
      heap_caps_malloc(sizeof(KaffiState), MALLOC_CAP_SPIRAM));
  auto *days = static_cast<DayBits *>(
      heap_caps_calloc(KAFFI_MAX_PEOPLE, sizeof(DayBits), MALLOC_CAP_SPIRAM));
  if (!buf || !st || !days) {
    ESP_LOGE(TAG, "alloc failed");
    heap_caps_free(buf);
    heap_caps_free(st);
    heap_caps_free(days);
    return false;
  }
  st->count = 0;
  static char query[600];
  static char enc[2048];
  char url[2400];

  int office_idx = kaffi_office_get();
  const char *office = KAFFI_OFFICES[office_idx < 0 ? 0 : office_idx].name;

  // 1) People from the company directory (filtered to the office location).
  // `match` instead of `==` — the directory location is free text, so word
  // matching also catches people who typed a full address.
  snprintf(query, sizeof(query),
           "*[_type==\"person\"&&active==true&&location match \"%s\"]"
           "{_id,name,\"image\":photo.asset->url + "
           "\"?w=96&h=96&fit=crop&fm=jpg\"}",
           office);
  url_encode(query, enc, sizeof(enc));
  snprintf(url, sizeof(url),
           "https://%s.api.sanity.io/v%s/data/query/%s?query=%s",
           CONFIG_KAFFI_DIR_PROJECT_ID, CONFIG_KAFFI_DIR_API_VERSION,
           CONFIG_KAFFI_DIR_DATASET, enc);
  int s1 = api_request(url, HTTP_METHOD_GET, buf, RESP_BUF_SIZE, nullptr,
                       CONFIG_KAFFI_DIR_TOKEN);
  if (s1 != 200) {
    ESP_LOGW(TAG, "Directory HTTP %d", s1);
    heap_caps_free(days);
    heap_caps_free(st);
    heap_caps_free(buf);
    return false;
  }
  st->count = parse_people(buf, st->people, KAFFI_MAX_PEOPLE);

  // 2) This-year coffee events from our own project, merged onto people by
  // id. Paginated with an _id cursor (datasets are physically sorted by _id,
  // so `_id > $last | order(_id)` is the fastest pagination GROQ offers —
  // offset slices degrade linearly) and both filter terms are
  // attribute-vs-literal, so the query stays on the indexed fast path.
  char ys[40];
  year_start_iso(ys, sizeof(ys));
  long today = local_daynum(time(nullptr));
  char cursor[KAFFI_ID_LEN] = "";
  for (int page = 0; page < EVENTS_MAX_PAGES; page++) {
    // Events predating multi-office have no `office` — they were all Oslo.
    snprintf(query, sizeof(query),
             "*[_type==\"coffeeEvent\"&&occurredAt>=\"%s\"&&_id>\"%s\""
             "&&coalesce(office,\"Oslo\")==\"%s\"]"
             "|order(_id)[0...%d]{_id,personId,kind,quantity,occurredAt}",
             ys, cursor, office, EVENTS_PAGE_SIZE);
    url_encode(query, enc, sizeof(enc));
    snprintf(url, sizeof(url),
             "https://%s.api.sanity.io/v%s/data/query/%s?query=%s",
             CONFIG_KAFFI_SANITY_PROJECT_ID, CONFIG_KAFFI_SANITY_API_VERSION,
             CONFIG_KAFFI_SANITY_DATASET, enc);
    int s2 = api_request(url, HTTP_METHOD_GET, buf, RESP_BUF_SIZE, nullptr,
                         CONFIG_KAFFI_SANITY_TOKEN);
    if (s2 != 200) {
      ESP_LOGW(TAG, "Events HTTP %d (page %d)", s2, page);
      break;
    }
    if (aggregate_events(buf, st, days, today, cursor, sizeof(cursor)) <
        EVENTS_PAGE_SIZE)
      break;
  }

  if (today >= 0)
    for (int i = 0; i < st->count; i++) {
      st->people[i].streak = compute_streak(days[i], today);
      st->people[i].brewed_today = days[i][0] & 1u;
    }

  sort_people(st);

  ESP_LOGI(TAG, "Fetched %d people", st->count);
  esp_event_post(APP_EVENT, APP_EVENT_KAFFI_PEOPLE_UPDATE, st, sizeof(*st),
                 pdMS_TO_TICKS(100));

  heap_caps_free(days);
  heap_caps_free(st);
  heap_caps_free(buf);
  return true;
}

// Write a coffeeEvent referencing the directory person by id (+ denormalized
// name for studio readability).
void sanity_api_iso_now(char *out, size_t out_len) { iso_now(out, out_len); }

bool sanity_api_log(const char *person_id, const char *person_name, int kind,
                    int quantity, const char *occurred_at) {
  const char *kind_str = (kind == KAFFI_BREW) ? "brew" : "cup";

  char occ[32];
  if (occurred_at && occurred_at[0])
    snprintf(occ, sizeof(occ), "%s", occurred_at);
  else
    iso_now(occ, sizeof(occ));

  int office_idx = kaffi_office_get();
  const char *office = KAFFI_OFFICES[office_idx < 0 ? 0 : office_idx].name;

  char body[800];
  if (occ[0]) {
    snprintf(body, sizeof(body),
             "{\"mutations\":[{\"create\":{\"_type\":\"coffeeEvent\","
             "\"kind\":\"%s\",\"quantity\":%d,\"occurredAt\":\"%s\","
             "\"personId\":\"%s\",\"personName\":\"%s\","
             "\"office\":\"%s\"}}]}",
             kind_str, quantity, occ, person_id, person_name, office);
  } else {
    snprintf(body, sizeof(body),
             "{\"mutations\":[{\"create\":{\"_type\":\"coffeeEvent\","
             "\"kind\":\"%s\",\"quantity\":%d,"
             "\"personId\":\"%s\",\"personName\":\"%s\","
             "\"office\":\"%s\"}}]}",
             kind_str, quantity, person_id, person_name, office);
  }

  char url[256];
  snprintf(url, sizeof(url), "https://%s.api.sanity.io/v%s/data/mutate/%s",
           CONFIG_KAFFI_SANITY_PROJECT_ID, CONFIG_KAFFI_SANITY_API_VERSION,
           CONFIG_KAFFI_SANITY_DATASET);

  char respbuf[512];
  int status = api_request(url, HTTP_METHOD_POST, respbuf, sizeof(respbuf),
                           body, CONFIG_KAFFI_SANITY_TOKEN);
  if (status == 200) {
    ESP_LOGI(TAG, "Logged %s x%d for %s", kind_str, quantity, person_name);
    return true;
  }
  ESP_LOGW(TAG, "Log failed HTTP %d: %s", status, respbuf);
  return false;
}

bool sanity_api_fetch_firmware(char *version, size_t vlen, char *url,
                               size_t ulen) {
  version[0] = '\0';
  url[0] = '\0';

  static const char *QUERY =
      "*[_id==\"firmwareRelease\"][0]{version,\"url\":bin.asset->url}";
  char enc[200];
  url_encode(QUERY, enc, sizeof(enc));
  char req_url[400];
  snprintf(req_url, sizeof(req_url),
           "https://%s.api.sanity.io/v%s/data/query/%s?query=%s",
           CONFIG_KAFFI_SANITY_PROJECT_ID, CONFIG_KAFFI_SANITY_API_VERSION,
           CONFIG_KAFFI_SANITY_DATASET, enc);

  char resp[600];
  int status = api_request(req_url, HTTP_METHOD_GET, resp, sizeof(resp),
                           nullptr, CONFIG_KAFFI_SANITY_TOKEN);
  if (status != 200)
    return false;
  return json_str(resp, "version", version, vlen) &&
         json_str(resp, "url", url, ulen);
}

const char *sanity_api_token() { return CONFIG_KAFFI_SANITY_TOKEN; }

// ─── Avatar fetch + decode (public image CDN, no auth)

struct JpegResp {
  uint8_t *buf;
  int len;
  int cap;
};

static esp_err_t on_jpeg_event(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data &&
      evt->data_len > 0) {
    auto *r = static_cast<JpegResp *>(evt->user_data);
    int space = r->cap - r->len;
    if (space > 0) {
      int n = evt->data_len < space ? evt->data_len : space;
      memcpy(r->buf + r->len, evt->data, n);
      r->len += n;
    }
  }
  return ESP_OK;
}

bool sanity_api_fetch_avatar(const char *url, unsigned char **out_pixels,
                             int *w, int *h) {
  if (!url || !url[0])
    return false;

  static constexpr int JPEG_BUF_SIZE = 96 * 1024;
  auto *jbuf = static_cast<uint8_t *>(
      heap_caps_malloc(JPEG_BUF_SIZE, MALLOC_CAP_SPIRAM));
  if (!jbuf)
    return false;

  JpegResp resp = {jbuf, 0, JPEG_BUF_SIZE};
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = KAFFI_HTTP_TIMEOUT_MS;
  cfg.event_handler = on_jpeg_event;
  cfg.user_data = &resp;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.buffer_size = 2048;

  auto *client = esp_http_client_init(&cfg); // null on e.g. malformed URL
  if (!client) {
    heap_caps_free(jbuf);
    return false;
  }
  esp_err_t err = esp_http_client_perform(client);
  int status = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);

  bool ok = false;
  if (err == ESP_OK && status == 200 && resp.len > 0) {
    ok = art_decode_jpeg(jbuf, resp.len, out_pixels, w, h, 96);
  } else {
    ESP_LOGW(TAG, "Avatar fetch failed (err=%d status=%d len=%d)", err, status,
             resp.len);
  }

  heap_caps_free(jbuf);
  return ok;
}

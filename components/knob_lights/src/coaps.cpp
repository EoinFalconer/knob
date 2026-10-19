#include "coaps.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include "psa/crypto.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace {

constexpr const char *TAG = "coaps";

// CoAP's own retransmission, over a session that already survived the
// handshake: a lost datagram is resent after ACK_TIMEOUT_MS.
constexpr uint32_t ACK_TIMEOUT_MS = 2000;
constexpr int MAX_TIMEOUTS = 3;
constexpr uint32_t HANDSHAKE_MIN_MS = 1000;
constexpr uint32_t HANDSHAKE_MAX_MS = 8000;
constexpr size_t DATAGRAM = 1280;
constexpr size_t REQUEST = 512;
constexpr int MAX_BLOCKS = 64;

constexpr uint8_t VERSION = 1;
enum Type : uint8_t { CON = 0, NON = 1, ACK = 2, RST = 3 };
constexpr uint16_t OPT_URI_PATH = 11;
constexpr uint16_t OPT_BLOCK2 = 23;
constexpr uint8_t PAYLOAD_MARKER = 0xFF;
constexpr int TOKEN_LEN = 4;

// The only suite the gateway offers.
constexpr int SUITES[] = {MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8, 0};

struct Timer {
  int64_t start_us;
  uint32_t intermediate_ms, final_ms;
};

struct Reply {
  uint8_t type, code, tkl;
  uint16_t mid;
  uint8_t token[8];
  bool block2, more;
  uint32_t block_num;
  uint8_t szx;
  const uint8_t *payload;
  size_t payload_len;
};

} // namespace

struct CoapsSession {
  int fd;
  bool heard; // the gateway sent something back during the handshake
  bool open;
  uint16_t message_id;
  Timer timer;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  uint8_t datagram[DATAGRAM];
};

namespace {

int open_socket(const char *host, int port) {
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  char service[8];
  snprintf(service, sizeof(service), "%d", port);
  addrinfo *found = nullptr;
  if (getaddrinfo(host, service, &hints, &found) != 0 || !found) {
    ESP_LOGW(TAG, "%s didn't resolve", host);
    return -1;
  }
  int fd = socket(found->ai_family, found->ai_socktype, found->ai_protocol);
  if (fd >= 0 && connect(fd, found->ai_addr, found->ai_addrlen) != 0) {
    close(fd);
    fd = -1;
  }
  freeaddrinfo(found);
  return fd;
}

int bio_send(void *ctx, const unsigned char *buf, size_t len) {
  int n = send(static_cast<CoapsSession *>(ctx)->fd, buf, len, 0);
  return n < 0 ? MBEDTLS_ERR_NET_SEND_FAILED : n;
}

int bio_recv(void *ctx, unsigned char *buf, size_t len, uint32_t timeout_ms) {
  auto *s = static_cast<CoapsSession *>(ctx);
  fd_set ready;
  FD_ZERO(&ready);
  FD_SET(s->fd, &ready);
  timeval tv = {static_cast<time_t>(timeout_ms / 1000),
                static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
  int waited =
      select(s->fd + 1, &ready, nullptr, nullptr, timeout_ms ? &tv : nullptr);
  if (waited == 0)
    return MBEDTLS_ERR_SSL_TIMEOUT;
  int n = waited < 0 ? -1 : recv(s->fd, buf, len, 0);
  if (n < 0)
    return MBEDTLS_ERR_NET_RECV_FAILED;
  s->heard = true;
  return n;
}

void timer_set(void *ctx, uint32_t intermediate_ms, uint32_t final_ms) {
  auto *t = static_cast<Timer *>(ctx);
  *t = {esp_timer_get_time(), intermediate_ms, final_ms};
}

int timer_get(void *ctx) {
  auto *t = static_cast<Timer *>(ctx);
  if (t->final_ms == 0)
    return -1;
  int64_t ms = (esp_timer_get_time() - t->start_us) / 1000;
  if (std::cmp_greater_equal(ms, t->final_ms))
    return 2;
  return std::cmp_greater_equal(ms, t->intermediate_ms) ? 1 : 0;
}

int random_bytes(void *, unsigned char *out, size_t len) {
  esp_fill_random(out, len);
  return 0;
}

// ─── Messages ───────────────────────────────────────────────────────────────

// An option's delta or length: small values fit the nibble, larger ones
// spill into one or two extension bytes.
uint8_t nibble(uint32_t v, uint8_t *ext, size_t *ext_len) {
  if (v < 13)
    return static_cast<uint8_t>(v);
  if (v < 269) {
    ext[(*ext_len)++] = static_cast<uint8_t>(v - 13);
    return 13;
  }
  ext[(*ext_len)++] = static_cast<uint8_t>((v - 269) >> 8);
  ext[(*ext_len)++] = static_cast<uint8_t>(v - 269);
  return 14;
}

bool put_option(uint8_t *buf, size_t cap, size_t *at, uint16_t *last,
                uint16_t number, const uint8_t *value, size_t len) {
  uint8_t ext[4];
  size_t ext_len = 0;
  uint8_t delta = nibble(number - *last, ext, &ext_len);
  uint8_t length = nibble(static_cast<uint32_t>(len), ext, &ext_len);
  if (*at + 1 + ext_len + len > cap)
    return false;
  buf[(*at)++] = static_cast<uint8_t>(delta << 4 | length);
  memcpy(buf + *at, ext, ext_len);
  *at += ext_len;
  memcpy(buf + *at, value, len);
  *at += len;
  *last = number;
  return true;
}

// `block2` asks for that block of the response, < 0 for none. Returns the
// length, or 0 if it doesn't fit.
size_t build(uint8_t *buf, size_t cap, CoapMethod method, uint16_t mid,
             const uint8_t *token, const char *path, int32_t block2,
             const char *payload) {
  size_t at = 4 + TOKEN_LEN;
  buf[0] = static_cast<uint8_t>(VERSION << 6 | CON << 4 | TOKEN_LEN);
  buf[1] = static_cast<uint8_t>(method);
  buf[2] = static_cast<uint8_t>(mid >> 8);
  buf[3] = static_cast<uint8_t>(mid);
  memcpy(buf + 4, token, TOKEN_LEN);
  uint16_t last = 0;
  for (const char *seg = path; *seg;) {
    const char *end = strchr(seg, '/');
    size_t len = end ? static_cast<size_t>(end - seg) : strlen(seg);
    if (!put_option(buf, cap, &at, &last, OPT_URI_PATH,
                    reinterpret_cast<const uint8_t *>(seg), len))
      return 0;
    seg += len + (end ? 1 : 0);
  }
  if (block2 >= 0) {
    uint8_t value[3];
    size_t len = 0;
    for (int shift = 16; shift >= 0; shift -= 8)
      if (len || (block2 >> shift) & 0xFF)
        value[len++] = static_cast<uint8_t>(block2 >> shift);
    if (!put_option(buf, cap, &at, &last, OPT_BLOCK2, value, len))
      return 0;
  }
  if (payload && *payload) {
    size_t len = strlen(payload);
    if (at + 1 + len > cap)
      return 0;
    buf[at++] = PAYLOAD_MARKER;
    std::copy_n(payload, len, buf + at); // CoAP payloads end with the datagram
    at += len;
  }
  return at;
}

bool extended(const uint8_t *b, size_t n, size_t *i, uint8_t v, uint32_t *out) {
  if (v < 13) {
    *out = v;
    return true;
  }
  if (v == 13 && *i < n) {
    *out = 13 + b[(*i)++];
    return true;
  }
  if (v == 14 && *i + 1 < n) {
    *out = 269 + (b[*i] << 8 | b[*i + 1]);
    *i += 2;
    return true;
  }
  return false;
}

bool parse(const uint8_t *b, size_t n, Reply *r) {
  if (n < 4 || b[0] >> 6 != VERSION)
    return false;
  *r = {};
  r->type = (b[0] >> 4) & 3;
  r->tkl = b[0] & 0x0F;
  r->code = b[1];
  r->mid = static_cast<uint16_t>(b[2] << 8 | b[3]);
  size_t i = 4;
  if (r->tkl > sizeof(r->token) || i + r->tkl > n)
    return false;
  memcpy(r->token, b + i, r->tkl);
  i += r->tkl;
  uint32_t number = 0;
  while (i < n) {
    if (b[i] == PAYLOAD_MARKER) {
      r->payload = b + i + 1;
      r->payload_len = n - i - 1;
      break;
    }
    uint8_t head = b[i++];
    uint32_t delta, len;
    if (!extended(b, n, &i, head >> 4, &delta) ||
        !extended(b, n, &i, head & 0x0F, &len) || i + len > n)
      return false;
    number += delta;
    if (number == OPT_BLOCK2) {
      uint32_t v = 0;
      for (uint32_t k = 0; k < len; k++)
        v = v << 8 | b[i + k];
      r->block2 = true;
      r->block_num = v >> 4;
      r->more = v & 0x08;
      r->szx = v & 0x07;
    }
    i += len;
  }
  return true;
}

// ─── Exchange ───────────────────────────────────────────────────────────────

bool write_datagram(CoapsSession *s, const uint8_t *buf, size_t len) {
  int ret;
  do
    ret = mbedtls_ssl_write(&s->ssl, buf, len);
  while (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ);
  return std::cmp_equal(ret, len);
}

void acknowledge(CoapsSession *s, uint16_t mid) {
  const uint8_t ack[4] = {static_cast<uint8_t>(VERSION << 6 | ACK << 4), 0,
                          static_cast<uint8_t>(mid >> 8),
                          static_cast<uint8_t>(mid)};
  write_datagram(s, ack, sizeof(ack));
}

// Sends a confirmable request and waits for its response: piggybacked on the
// acknowledgement, or, after an empty one, sent on its own.
bool exchange(CoapsSession *s, const uint8_t *req, size_t len, uint16_t mid,
              const uint8_t *token, Reply *r) {
  if (!write_datagram(s, req, len))
    return false;
  bool acknowledged = false;
  for (int timeouts = 0; timeouts < MAX_TIMEOUTS;) {
    int n = mbedtls_ssl_read(&s->ssl, s->datagram, sizeof(s->datagram));
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
      continue;
    if (n == MBEDTLS_ERR_SSL_TIMEOUT) {
      // Once acknowledged, the gateway has the request and is still working.
      if (++timeouts < MAX_TIMEOUTS && !acknowledged &&
          !write_datagram(s, req, len))
        return false;
      continue;
    }
    if (n <= 0)
      return false;
    if (!parse(s->datagram, static_cast<size_t>(n), r))
      continue;
    bool by_mid = r->mid == mid;
    bool by_token =
        r->tkl == TOKEN_LEN && memcmp(r->token, token, TOKEN_LEN) == 0;
    if (r->type == RST && by_mid)
      return false;
    if (r->type == ACK && by_mid) {
      if (r->code != 0)
        return true;
      acknowledged = true;
      continue;
    }
    // A separate response, or a stray one left over from an earlier retry;
    // either way the gateway wants its acknowledgement.
    if (r->type == CON)
      acknowledge(s, r->mid);
    if ((r->type == CON || r->type == NON) && by_token)
      return true;
  }
  return false;
}

} // namespace

CoapsOpen coaps_open(const char *host, int port, const char *identity,
                     const uint8_t *psk, size_t psk_len, CoapsSession **out) {
  *out = nullptr;
#if defined(MBEDTLS_PSA_CRYPTO_C)
  psa_crypto_init();
#endif
  auto *s = static_cast<CoapsSession *>(
      heap_caps_calloc(1, sizeof(CoapsSession), MALLOC_CAP_SPIRAM));
  if (!s)
    return CoapsOpen::Unreachable;
  s->fd = open_socket(host, port);
  if (s->fd < 0) {
    heap_caps_free(s);
    return CoapsOpen::Unreachable;
  }
  mbedtls_ssl_init(&s->ssl);
  mbedtls_ssl_config_init(&s->conf);
  int ret = mbedtls_ssl_config_defaults(&s->conf, MBEDTLS_SSL_IS_CLIENT,
                                        MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret == 0) {

    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s->conf, random_bytes, nullptr);
    mbedtls_ssl_conf_ciphersuites(&s->conf, SUITES);
    mbedtls_ssl_conf_max_tls_version(&s->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_handshake_timeout(&s->conf, HANDSHAKE_MIN_MS,
                                       HANDSHAKE_MAX_MS);
    mbedtls_ssl_conf_read_timeout(&s->conf, ACK_TIMEOUT_MS);
    ret = mbedtls_ssl_conf_psk(
        &s->conf, psk, psk_len,
        reinterpret_cast<const unsigned char *>(identity), strlen(identity));
  }
  if (ret == 0)
    ret = mbedtls_ssl_setup(&s->ssl, &s->conf);
  if (ret == 0) {
    mbedtls_ssl_set_bio(&s->ssl, s, bio_send, nullptr, bio_recv);
    mbedtls_ssl_set_timer_cb(&s->ssl, &s->timer, timer_set, timer_get);
    do
      ret = mbedtls_ssl_handshake(&s->ssl);
    while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE);
  }
  if (ret != 0) {
    ESP_LOGW(TAG, "Handshake with %s failed: -0x%04x", host,
             static_cast<unsigned>(-ret));
    CoapsOpen why = s->heard ? CoapsOpen::Refused : CoapsOpen::Unreachable;
    coaps_close(s);
    return why;
  }
  s->open = true;
  s->message_id = static_cast<uint16_t>(esp_random());
  *out = s;
  return CoapsOpen::Ok;
}

void coaps_close(CoapsSession *s) {
  if (!s)
    return;
  if (s->open)
    mbedtls_ssl_close_notify(&s->ssl);
  mbedtls_ssl_free(&s->ssl);
  mbedtls_ssl_config_free(&s->conf);
  if (s->fd >= 0)
    close(s->fd);
  heap_caps_free(s);
}

int coaps_request(CoapsSession *s, CoapMethod method, const char *path,
                  const char *payload, char *out, size_t out_cap) {
  if (!s || out_cap == 0)
    return 0;
  out[0] = '\0';
  size_t got = 0;
  int32_t block2 = -1;
  for (int blocks = 0; blocks < MAX_BLOCKS; blocks++) {
    uint8_t req[REQUEST];
    uint8_t token[TOKEN_LEN];
    esp_fill_random(token, sizeof(token));
    uint16_t mid = s->message_id++;
    // Later blocks are asked for without the payload, which went with the
    // first.
    size_t len = build(req, sizeof(req), method, mid, token, path, block2,
                       block2 < 0 ? payload : nullptr);
    Reply r;
    if (!len || !exchange(s, req, len, mid, token, &r))
      return 0;
    size_t take = std::min(r.payload_len, out_cap - 1 - got);
    if (take)
      memcpy(out + got, r.payload, take);
    got += take;
    out[got] = '\0';
    if (!r.block2 || !r.more)
      return (r.code >> 5) * 100 + (r.code & 0x1F);
    block2 = static_cast<int32_t>((r.block_num + 1) << 4 | r.szx);
  }
  return 0;
}

#pragma once

#include <cstddef>
#include <cstdint>

// A CoAP client over DTLS with a pre-shared key, just what the TRADFRI gateway
// needs: one confirmable request at a time, with blockwise responses put back
// together (RFC 7252, RFC 7959).

struct CoapsSession;

enum class CoapsOpen : uint8_t {
  Ok,
  Unreachable, // no answer, or the host didn't resolve
  Refused,     // it answered but the handshake failed: the key is wrong
};

enum class CoapMethod : uint8_t { Get = 1, Post = 2, Put = 3 };

CoapsOpen coaps_open(const char *host, int port, const char *identity,
                     const uint8_t *psk, size_t psk_len, CoapsSession **out);
void coaps_close(CoapsSession *s);

// `path` is the URI path without a leading slash, like "15001/65536". The
// response payload lands in `out`, NUL-terminated and cut to fit. Returns the
// response code as class * 100 + detail (205 for 2.05 Content), or 0 when
// nothing usable came back; the session is then best closed.
int coaps_request(CoapsSession *s, CoapMethod method, const char *path,
                  const char *payload, char *out, size_t out_cap);

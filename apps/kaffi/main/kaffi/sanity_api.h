#pragma once

#include <cstddef>

/// Initialize the Sanity API module. Call after WiFi is connected.
void sanity_api_init();

/// Fetch the leaderboard (people + pots/cups) and post
/// APP_EVENT_KAFFI_PEOPLE_UPDATE with the result. Returns true on success.
bool sanity_api_fetch_people();

/// Create a coffeeEvent referencing a directory person by id (person_name is
/// denormalized for readability). `kind` is a KaffiKind (cup/brew).
/// `occurred_at` is an ISO-8601 UTC timestamp; pass null/empty to stamp now —
/// replayed events pass the time of the original tap.
bool sanity_api_log(const char *person_id, const char *person_name, int kind,
                    int quantity, const char *occurred_at);

/// Current UTC time as ISO-8601, or "" while the clock is still unset.
void sanity_api_iso_now(char *out, size_t out_len);

/// Fetch a JPEG avatar from `url` and decode it to an RGB565 buffer.
/// On success, *out_pixels is a PSRAM buffer the caller must free.
bool sanity_api_fetch_avatar(const char *url, unsigned char **out_pixels,
                             int *w, int *h);

/// Fetch the published firmwareRelease doc (version + binary URL) from the
/// events project. Returns false when missing or unreachable.
bool sanity_api_fetch_firmware(char *version, size_t vlen, char *url,
                               size_t ulen);

/// The bearer token for the events project — the OTA download needs it when
/// the dataset is private.
const char *sanity_api_token();

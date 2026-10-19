// Host stand-ins for what the kaffi UI asks of main.cpp.
#include "kaffi_sim.h"

#include "app_config.h"
#include "art_decoder.h"
#include "ui/ui.h"

#include <cstdio>
#include <string>
#include <vector>

// Photo URLs are file paths here. Fetches wait until a scenario delivers
// them, as the avatar task would between frames.
static std::vector<std::string> s_asked;

void kaffi_request_avatar(const char *url) { s_asked.emplace_back(url); }

int sim_deliver_avatars() {
  std::vector<std::string> asked;
  asked.swap(s_asked);
  for (const std::string &url : asked) {
    std::vector<uint8_t> jpeg(64 * 1024);
    FILE *f = std::fopen(url.c_str(), "rb");
    int len = 0;
    if (f) {
      len = static_cast<int>(std::fread(jpeg.data(), 1, jpeg.size(), f));
      std::fclose(f);
    }
    uint8_t *pixels = nullptr;
    int w = 0, h = 0;
    if (len > 0)
      art_decode_jpeg(jpeg.data(), len, &pixels, &w, &h, KAFFI_PHOTO_PX);
    ui_set_avatar(url.c_str(), pixels, w, h);
  }
  return static_cast<int>(asked.size());
}

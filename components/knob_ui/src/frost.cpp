#include "frost.h"
#include "hal_pins.h"

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

#include <algorithm>
#include <cstdint>
#include <initializer_list>

// The blur runs on a quarter-size copy: a box blur there, three passes each
// way, is close to a Gaussian of about 14 px on the full screen, at a
// sixteenth of the work. Upscaling it back is smooth because it's a blur.
namespace {

constexpr int SCALE = 4;
constexpr int W = LCD_H_RES / SCALE;
constexpr int H = LCD_V_RES / SCALE;
constexpr int BLUR_RADIUS = 3;
constexpr int BLUR_PASSES = 3;
// Tint, in 1/256: colors pushed away from gray, then darkened toward black
// with a small lift so a black screen frosts to near black, not pure black.
constexpr int SATURATION = 350;
constexpr int DARKEN = 112;
constexpr int LIFT = 10;

lv_draw_buf_t *s_snapshot;
uint16_t *s_pixels[2];
int s_back;        // the image being made; the other one may be on screen
bool s_pending;    // a blur is under way or done but not yet taken
uint8_t *s_planes; // r, g, b, then scratch: W * H each
lv_image_dsc_t s_images[2];
const lv_image_dsc_t *s_taken;

bool allocate(lv_obj_t *screen) {
  if (!s_snapshot)
    s_snapshot = lv_snapshot_create_draw_buf(screen, LV_COLOR_FORMAT_RGB565);
  for (uint16_t *&p : s_pixels)
    if (!p)
      p = static_cast<uint16_t *>(
          lv_malloc(sizeof(uint16_t) * LCD_H_RES * LCD_V_RES));
  if (!s_planes)
    s_planes = static_cast<uint8_t *>(lv_malloc(4 * W * H));
  return s_snapshot && s_pixels[0] && s_pixels[1] && s_planes;
}

// Reads the snapshot front to back, which is what PSRAM's cache likes.
void downsample(const lv_draw_buf_t *src, uint8_t *r, uint8_t *g, uint8_t *b) {
  uint16_t sr[W], sg[W], sb[W];
  for (int y = 0; y < H; y++) {
    std::fill_n(sr, W, 0);
    std::fill_n(sg, W, 0);
    std::fill_n(sb, W, 0);
    for (int dy = 0; dy < SCALE; dy++) {
      const auto *row = reinterpret_cast<const uint16_t *>(
          src->data + (y * SCALE + dy) * src->header.stride);
      for (int x = 0; x < LCD_H_RES; x++) {
        uint16_t p = row[x];
        sr[x / SCALE] += p >> 11;
        sg[x / SCALE] += (p >> 5) & 0x3F;
        sb[x / SCALE] += p & 0x1F;
      }
    }
    // 16 samples of 5 or 6 bits, back to 8 bits.
    for (int x = 0; x < W; x++) {
      r[y * W + x] = static_cast<uint8_t>(sr[x] >> 1);
      g[y * W + x] = static_cast<uint8_t>(sg[x] >> 2);
      b[y * W + x] = static_cast<uint8_t>(sb[x] >> 1);
    }
  }
}

// One box-blur pass along a line of `n` samples `step` apart, edges clamped.
void box_line(const uint8_t *src, uint8_t *dst, int n, int step) {
  auto at = [&](int i) { return src[std::clamp(i, 0, n - 1) * step]; };
  int sum = 0;
  for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; i++)
    sum += at(i);
  constexpr int SPAN = 2 * BLUR_RADIUS + 1;
  for (int i = 0; i < n; i++) {
    dst[i * step] = static_cast<uint8_t>(sum / SPAN);
    sum += at(i + BLUR_RADIUS + 1) - at(i - BLUR_RADIUS);
  }
}

void blur(uint8_t *plane, uint8_t *scratch) {
  for (int pass = 0; pass < BLUR_PASSES; pass++) {
    for (int y = 0; y < H; y++)
      box_line(plane + y * W, scratch + y * W, W, 1);
    for (int x = 0; x < W; x++)
      box_line(scratch + x, plane + x, H, W);
  }
}

uint8_t tint_channel(int c, int lum) {
  int saturated = lum + (c - lum) * SATURATION / 256;
  return static_cast<uint8_t>(
      std::clamp(saturated * DARKEN / 256 + LIFT, 0, 255));
}

void tint(uint8_t *r, uint8_t *g, uint8_t *b) {
  for (int i = 0; i < W * H; i++) {
    int lum = (r[i] * 77 + g[i] * 150 + b[i] * 29) >> 8;
    r[i] = tint_channel(r[i], lum);
    g[i] = tint_channel(g[i], lum);
    b[i] = tint_channel(b[i], lum);
  }
}

// Where a full-size row samples the small grid: the small row before it and
// the weight of the next one, in 1/256, centers aligned.
struct Tap {
  int i;
  int f;
};

Tap tap(int i, int n) {
  int pos = std::max(0, ((2 * i + 1) * 256 / (2 * SCALE)) - 128);
  int i0 = std::min(pos >> 8, n - 2);
  return {i0, std::min(pos - (i0 << 8), 256)};
}

// One channel along a row, in 1/256 of a level.
struct Ramp {
  const uint16_t *row;
  int32_t at, step;
  void start(int i) {
    int32_t span = row[i + 1] - row[i];
    at = row[i] + span / (2 * SCALE);
    step = span / SCALE;
  }
};

// An ordered dither hides the bands a smooth gradient otherwise shows in
// RGB565. Offsets are in 1/256 of a level, below the bits each channel drops.
struct Dither {
  int32_t rb[4], g[4];
  explicit Dither(int y) {
    static constexpr uint8_t BAYER[4][4] = {
        {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    for (int x = 0; x < 4; x++) {
      rb[x] = (BAYER[y & 3][x] >> 1) << 8;
      g[x] = (BAYER[y & 3][x] >> 2) << 8;
    }
  }
};

uint16_t pack(int32_t r, int32_t g, int32_t b, const Dither &d, int x) {
  int r5 = std::min<int32_t>(31, (r + d.rb[x & 3]) >> 11);
  int g6 = std::min<int32_t>(63, (g + d.g[x & 3]) >> 10);
  int b5 = std::min<int32_t>(31, (b + d.rb[x & 3]) >> 11);
  auto px = static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
  return static_cast<uint16_t>((px >> 8) | (px << 8));
}

// Bilinear back to full size, one direction at a time: each output row mixes
// two small rows once. Along the row, the SCALE pixels between two small
// pixel centers sit at fixed fractions, so each is the last plus a step. The
// outermost half-cells just repeat the edge.
void upscale(const uint8_t *r, const uint8_t *g, const uint8_t *b,
             uint16_t *pixels) {
  constexpr int HALF = SCALE / 2;
  uint16_t rows[3][W];
  const uint8_t *planes[3] = {r, g, b};
  for (int y = 0; y < LCD_V_RES; y++) {
    Tap ty = tap(y, H);
    for (int c = 0; c < 3; c++) {
      const uint8_t *top = planes[c] + ty.i * W;
      for (int x = 0; x < W; x++)
        rows[c][x] =
            static_cast<uint16_t>(top[x] * (256 - ty.f) + top[x + W] * ty.f);
    }
    uint16_t *out = pixels + y * LCD_H_RES;
    Dither dither(y);
    for (int x = 0; x < HALF; x++) {
      out[x] = pack(rows[0][0], rows[1][0], rows[2][0], dither, x);
      out[LCD_H_RES - 1 - x] = pack(rows[0][W - 1], rows[1][W - 1],
                                    rows[2][W - 1], dither, LCD_H_RES - 1 - x);
    }
    Ramp rr{rows[0], 0, 0}, rg{rows[1], 0, 0}, rb{rows[2], 0, 0};
    for (int i = 0; i < W - 1; i++) {
      rr.start(i);
      rg.start(i);
      rb.start(i);
      int x = HALF + i * SCALE;
      for (int k = 0; k < SCALE; k++, x++) {
        out[x] = pack(rr.at, rg.at, rb.at, dither, x);
        rr.at += rr.step;
        rg.at += rg.step;
        rb.at += rb.step;
      }
    }
  }
}

// The slow part: touches only its own buffers, never LVGL.
void blur_snapshot() {
  uint8_t *r = s_planes;
  uint8_t *g = r + W * H;
  uint8_t *b = g + W * H;
  uint8_t *scratch = b + W * H;
  downsample(s_snapshot, r, g, b);
  for (uint8_t *plane : {r, g, b})
    blur(plane, scratch);
  tint(r, g, b);
  upscale(r, g, b, s_pixels[s_back]);
}

#ifdef ESP_PLATFORM
// Core 0, beside Wi-Fi; LVGL runs on core 1. Low priority: it has a frame or
// more of slack while the finger travels the drag slop.
constexpr int WORKER_CORE = 0;
constexpr UBaseType_t WORKER_PRIO = 3;
constexpr uint32_t WORKER_STACK = 4096;

TaskHandle_t s_worker;
SemaphoreHandle_t s_done;

void worker(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    blur_snapshot();
    xSemaphoreGive(s_done);
  }
}

bool start_blur() {
  if (!s_worker) {
    s_done = xSemaphoreCreateBinary();
    if (!s_done || xTaskCreatePinnedToCore(worker, "frost", WORKER_STACK,
                                           nullptr, WORKER_PRIO, &s_worker,
                                           WORKER_CORE) != pdPASS) {
      s_worker = nullptr;
      blur_snapshot();
      return true;
    }
  }
  xTaskNotifyGive(s_worker);
  return false;
}

void wait_blur() { xSemaphoreTake(s_done, portMAX_DELAY); }
#else
// The simulator blurs at once.
bool start_blur() {
  blur_snapshot();
  return true;
}
void wait_blur() {}
#endif

bool s_blurring; // the worker has the buffers

void finish_blur() {
  if (s_blurring)
    wait_blur();
  s_blurring = false;
}

} // namespace

bool frost_begin(lv_obj_t *screen) {
  finish_blur();
  if (!allocate(screen) ||
      lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_RGB565,
                                   s_snapshot) != LV_RESULT_OK)
    return false;
  s_pending = true;
  s_blurring = !start_blur();
  return true;
}

const lv_image_dsc_t *frost_take() {
  if (!s_pending)
    return s_taken;
  finish_blur();
  s_pending = false;
  lv_image_dsc_t &image = s_images[s_back];
  image = {};
  image.header.magic = LV_IMAGE_HEADER_MAGIC;
  // In the panel's byte order, so drawing it is a plain copy.
  image.header.cf = LV_COLOR_FORMAT_RGB565_SWAPPED;
  image.header.w = LCD_H_RES;
  image.header.h = LCD_V_RES;
  image.header.stride = LCD_H_RES * 2;
  image.data_size = LCD_H_RES * LCD_V_RES * 2;
  image.data = reinterpret_cast<const uint8_t *>(s_pixels[s_back]);
  s_back ^= 1;
  s_taken = &image;
  return s_taken;
}

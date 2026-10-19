#include "art_decoder.h"

extern "C" {
#include "tjpgd/tjpgd.h"
}

#include "esp_heap_caps.h"
#include "esp_log.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

static constexpr const char *TAG = "art_dec";
static constexpr int TJPGD_WORK_SIZE = 32768;

// Combined TJPGD device struct with both stream and output info.
struct JpegDevice {
  // Stream input
  const uint8_t *data;
  int len;
  int pos;
  // RGB565 output
  uint16_t *pixels;
  int out_w;
  int out_h;
};

static size_t jpeg_input(JDEC *jd, uint8_t *buf, size_t ndata) {
  auto *dev = static_cast<JpegDevice *>(jd->device);
  int avail = dev->len - dev->pos;
  if (static_cast<int>(ndata) > avail)
    ndata = avail;
  if (buf)
    memcpy(buf, dev->data + dev->pos, ndata);
  dev->pos += ndata;
  return ndata;
}

// 4x4 Bayer thresholds. Truncating 8-bit channels to RGB565 turns a smooth
// sky into visible bands; nudging each pixel by its cell's threshold first
// spreads the error into a fine pattern the eye averages out.
static constexpr uint8_t BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static uint16_t dither_565(int r, int g, int b, int x, int y) {
  int t = BAYER[y & 3][x & 3];
  r = std::min(255, r + (t >> 1)) >> 3;
  g = std::min(255, g + (t >> 2)) >> 2;
  b = std::min(255, b + (t >> 1)) >> 3;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

static int jpeg_output(JDEC *jd, void *bitmap, JRECT *rect) {
  auto *dev = static_cast<JpegDevice *>(jd->device);
  const auto *src = static_cast<const uint8_t *>(bitmap);

  int w = rect->right - rect->left + 1;
  int stride = dev->out_w;

  for (int y = rect->top; y <= rect->bottom; y++) {
    if (y >= dev->out_h)
      break;
    uint16_t *dst = dev->pixels + y * stride + rect->left;
    int copy_w = std::min(w, stride - rect->left);
    for (int i = 0; i < copy_w; i++) {
      // TJpgDec writes each pixel as B, G, R.
      const uint8_t *p = src + i * 3;
      uint16_t rgb = dither_565(p[2], p[1], p[0], rect->left + i, y);
      // Byte-swap for LV_COLOR_FORMAT_RGB565_SWAPPED display
      dst[i] = static_cast<uint16_t>((rgb >> 8) | (rgb << 8));
    }
    src += w * 3;
  }
  return 1; // Continue decompression
}

bool art_decode_jpeg(const uint8_t *jpeg_data, int jpeg_len, uint8_t **out_buf,
                     int *out_w, int *out_h, int max_dim) {
  *out_buf = nullptr;
  *out_w = 0;
  *out_h = 0;

  if (!jpeg_data || jpeg_len < 4)
    return false;

  // Verify JPEG magic
  if (jpeg_data[0] != 0xFF || jpeg_data[1] != 0xD8)
    return false;

  // Allocate work buffer for TJPGD (large images need big pools — use PSRAM)
  void *work = heap_caps_malloc(TJPGD_WORK_SIZE, MALLOC_CAP_SPIRAM);
  if (!work)
    return false;

  JpegDevice dev = {};
  dev.data = jpeg_data;
  dev.len = jpeg_len;
  dev.pos = 0;
  dev.pixels = nullptr;

  JDEC jd = {};
  JRESULT rc = jd_prepare(&jd, jpeg_input, work, TJPGD_WORK_SIZE, &dev);
  if (rc != JDR_OK) {
    ESP_LOGW(TAG, "jd_prepare failed: %d", rc);
    free(work);
    return false;
  }

  // Pick scale factor so output fits within max_dim x max_dim
  // Scale: 0=1/1, 1=1/2, 2=1/4, 3=1/8
  uint8_t scale = 0;
  int sw = jd.width, sh = jd.height;
  while (scale < 3 && (sw > max_dim || sh > max_dim)) {
    scale++;
    sw = jd.width >> scale;
    sh = jd.height >> scale;
  }

  if (sw <= 0 || sh <= 0) {
    heap_caps_free(work);
    return false;
  }

  // Allocate output pixel buffer in PSRAM
  size_t buf_size = static_cast<size_t>(sw) * sh * sizeof(uint16_t);
  auto *pixels =
      static_cast<uint16_t *>(heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM));
  if (!pixels) {
    ESP_LOGW(TAG, "Failed to allocate %u bytes for decoded art",
             (unsigned)buf_size);
    heap_caps_free(work);
    return false;
  }
  memset(pixels, 0, buf_size);

  dev.pixels = pixels;
  dev.out_w = sw;
  dev.out_h = sh;

  // Reset stream position for decompression — jd_prepare consumed header,
  // but TJPGD tracks its own internal position via the input buffer,
  // so we don't reset dev.pos here.

  rc = jd_decomp(&jd, jpeg_output, scale);
  heap_caps_free(work);

  if (rc != JDR_OK) {
    ESP_LOGW(TAG, "jd_decomp failed: %d (scale=%d, %dx%d→%dx%d)", rc, scale,
             jd.width, jd.height, sw, sh);
    heap_caps_free(pixels);
    return false;
  }

  *out_buf = reinterpret_cast<uint8_t *>(pixels);
  *out_w = sw;
  *out_h = sh;

  ESP_LOGI(TAG, "Decoded %dx%d JPEG → %dx%d RGB565 (%u bytes)", jd.width,
           jd.height, sw, sh, (unsigned)buf_size);
  return true;
}

uint8_t *art_scale_rgb565(const uint8_t *src, int src_w, int src_h, int dst_w,
                          int dst_h) {
  if (!src || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
    return nullptr;
  auto *dst = static_cast<uint8_t *>(heap_caps_malloc(
      static_cast<size_t>(dst_w) * dst_h * 2, MALLOC_CAP_SPIRAM));
  if (!dst)
    return nullptr;

  for (int y = 0; y < dst_h; y++) {
    int sy0 = y * src_h / dst_h;
    int sy1 = (y + 1) * src_h / dst_h;
    if (sy1 <= sy0) // upscaling: sample at least one source row
      sy1 = sy0 + 1;
    for (int x = 0; x < dst_w; x++) {
      int sx0 = x * src_w / dst_w;
      int sx1 = (x + 1) * src_w / dst_w;
      if (sx1 <= sx0)
        sx1 = sx0 + 1;
      int r = 0, g = 0, b = 0, n = 0;
      for (int sy = sy0; sy < sy1; sy++) {
        for (int sx = sx0; sx < sx1; sx++) {
          const uint8_t *p =
              src + (static_cast<ptrdiff_t>(sy) * src_w + sx) * 2;
          auto v = static_cast<uint16_t>((p[0] << 8) | p[1]); // swapped
          r += (v >> 11) & 0x1F;
          g += (v >> 5) & 0x3F;
          b += v & 0x1F;
          n++;
        }
      }
      auto v =
          static_cast<uint16_t>(((r / n) << 11) | ((g / n) << 5) | (b / n));
      uint8_t *q = dst + (static_cast<ptrdiff_t>(y) * dst_w + x) * 2;
      q[0] = static_cast<uint8_t>(v >> 8);
      q[1] = static_cast<uint8_t>(v & 0xFF);
    }
  }
  return dst;
}

#include "ui/clock_art.h"

#include "esp_heap_caps.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace {

void set_header(ClockArt *art, int w, int h) {
  art->dsc = {};
  art->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  art->dsc.header.cf = LV_COLOR_FORMAT_A8;
  art->dsc.header.w = static_cast<uint32_t>(w);
  art->dsc.header.h = static_cast<uint32_t>(h);
  art->dsc.header.stride = static_cast<uint32_t>(w);
  art->dsc.data_size = static_cast<uint32_t>(w * h);
  art->dsc.data = art->pixels;
}

// PSRAM on purpose: several of these fall under malloc's 16 KB internal-RAM
// threshold, and together they starved Wi-Fi of its buffers at boot.
bool reserve(ClockArt *art, size_t bytes) {
  if (!art->pixels)
    art->pixels = static_cast<uint8_t *>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  return art->pixels != nullptr;
}

struct Glyph {
  lv_font_glyph_dsc_t dsc;
  int x, y; // ink top-left, relative to the pen start and the line top
};

// Lays the glyphs out like an LVGL label would, and returns the ink bounds.
int layout(const lv_font_t *font, const char *text, int tracking, Glyph *glyphs,
           lv_area_t *ink) {
  int baseline = lv_font_get_line_height(font) - font->base_line;
  int pen = 0;
  int n = 0;
  *ink = {INT16_MAX, INT16_MAX, INT16_MIN, INT16_MIN};
  for (const char *c = text; *c; c++) {
    Glyph &g = glyphs[n];
    if (!lv_font_get_glyph_dsc(font, &g.dsc, static_cast<uint8_t>(*c), 0))
      continue;
    g.x = pen + g.dsc.ofs_x;
    g.y = baseline - g.dsc.ofs_y - g.dsc.box_h;
    if (g.dsc.box_w > 0) {
      ink->x1 = std::min<int32_t>(ink->x1, g.x);
      ink->y1 = std::min<int32_t>(ink->y1, g.y);
      ink->x2 = std::max<int32_t>(ink->x2, g.x + g.dsc.box_w - 1);
      ink->y2 = std::max<int32_t>(ink->y2, g.y + g.dsc.box_h - 1);
      n++;
    }
    pen += g.dsc.adv_w + tracking;
  }
  return n;
}

// Alpha of a capsule (a segment from `a` to `b` thickened by `r`) at the
// center of pixel (x, y): one pixel of anti-aliasing across its edge.
float capsule(float x, float y, float ax, float ay, float bx, float by,
              float r) {
  float px = x - ax, py = y - ay;
  float dx = bx - ax, dy = by - ay;
  float len2 = dx * dx + dy * dy;
  float t =
      len2 > 0.0f ? std::clamp((px * dx + py * dy) / len2, 0.0f, 1.0f) : 0.0f;
  float ex = px - t * dx, ey = py - t * dy;
  float d2 = ex * ex + ey * ey;
  if (d2 >= (r + 0.5f) * (r + 0.5f))
    return 0.0f;
  return std::clamp(r + 0.5f - std::sqrt(d2), 0.0f, 1.0f);
}

struct Segment {
  float ax, ay, bx, by, r;
};

uint8_t coverage(float a) {
  return static_cast<uint8_t>(std::lround(a * 255.0f));
}

// Adds a capsule to the bitmap whose top-left pixel is (x0, y0) on screen.
// Each row only visits the pixels within the capsule's band, so a long thin
// hand costs about its length, not its bounding box. (dx, dy) is the unit
// direction from a to b.
void stamp(const Segment &s, float dx, float dy, ClockArt *art, int x0, int y0,
           int w, int h) {
  float reach = s.r + 1.0f;
  float left = std::min(s.ax, s.bx) - reach;
  float right = std::max(s.ax, s.bx) + reach;
  int top = std::max(
      0, static_cast<int>(std::floor(std::min(s.ay, s.by) - reach)) - y0);
  int bottom = std::min(
      h - 1, static_cast<int>(std::ceil(std::max(s.ay, s.by) + reach)) - y0);
  for (int y = top; y <= bottom; y++) {
    float fy = static_cast<float>(y0 + y) + 0.5f;
    float lo = left, hi = right;
    if (std::fabs(dy) > 1e-3f) {
      float mid = s.ax + (fy - s.ay) * dx / dy;
      float half = reach / std::fabs(dy);
      lo = std::max(lo, mid - half);
      hi = std::min(hi, mid + half);
    }
    int from = std::max(0, static_cast<int>(std::floor(lo)) - x0);
    int to = std::min(w - 1, static_cast<int>(std::ceil(hi)) - x0);
    uint8_t *row = art->pixels + y * w;
    for (int x = from; x <= to; x++) {
      float fx = static_cast<float>(x0 + x) + 0.5f;
      row[x] = std::max(row[x],
                        coverage(capsule(fx, fy, s.ax, s.ay, s.bx, s.by, s.r)));
    }
  }
}

// The widest the bitmap gets at any angle: the hand's span plus its round
// ends, the anti-aliasing and rounding to whole pixels.
int max_side(const HandShape &shape) {
  float from = 0.0f, to = 0.0f, r = 0.0f;
  for (int i = 0; i < shape.count; i++) {
    from = std::min(from, shape.parts[i].from);
    to = std::max(to, shape.parts[i].to);
    r = std::max(r, shape.parts[i].width / 2.0f);
  }
  return static_cast<int>(std::ceil(to - from + 2.0f * r)) + 6;
}

} // namespace

bool clock_art_text(ClockArt *art, const lv_font_t *font, const char *text,
                    int tracking) {
  Glyph glyphs[8];
  lv_area_t ink;
  int n = layout(font, text, tracking, glyphs, &ink);
  int w = lv_area_get_width(&ink);
  int h = lv_area_get_height(&ink);
  size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (n == 0 || !reserve(art, bytes))
    return false;
  memset(art->pixels, 0, bytes);

  for (int i = 0; i < n; i++) {
    Glyph &g = glyphs[i];
    lv_draw_buf_t *buf = lv_draw_buf_create(g.dsc.box_w, g.dsc.box_h,
                                            LV_COLOR_FORMAT_A8, LV_STRIDE_AUTO);
    if (!buf)
      return false;
    lv_font_get_glyph_bitmap(&g.dsc, buf);
    // Glyphs overlap when tracked tight; the darker ink wins.
    int box_w = g.dsc.box_w, box_h = g.dsc.box_h;
    for (int y = 0; y < box_h; y++) {
      const uint8_t *src = buf->data + y * buf->header.stride;
      uint8_t *dst = art->pixels + (g.y - ink.y1 + y) * w + (g.x - ink.x1);
      for (int x = 0; x < box_w; x++)
        dst[x] = std::max(dst[x], src[x]);
    }
    lv_draw_buf_destroy(buf);
  }
  set_header(art, w, h);
  return true;
}

bool clock_art_hand(ClockArt *art, const HandShape &shape, float degrees,
                    int cx, int cy, lv_point_t *origin) {
  int side = max_side(shape);
  if (!reserve(art, static_cast<size_t>(side) * static_cast<size_t>(side)))
    return false;

  float rad = degrees * std::numbers::pi_v<float> / 180.0f;
  float dx = std::sin(rad), dy = -std::cos(rad);
  Segment segs[2];
  float left = INFINITY, top = INFINITY, right = -INFINITY, bottom = -INFINITY;
  for (int i = 0; i < shape.count; i++) {
    const HandPart &p = shape.parts[i];
    float r = p.width / 2.0f;
    // The round ends sit inside the part's span, like a rounded rectangle.
    float a = std::min(p.from + r, (p.from + p.to) / 2.0f);
    float b = std::max(p.to - r, a);
    Segment &s = segs[i];
    s = {static_cast<float>(cx) + dx * a, static_cast<float>(cy) + dy * a,
         static_cast<float>(cx) + dx * b, static_cast<float>(cy) + dy * b, r};
    left = std::min({left, s.ax - r, s.bx - r});
    right = std::max({right, s.ax + r, s.bx + r});
    top = std::min({top, s.ay - r, s.by - r});
    bottom = std::max({bottom, s.ay + r, s.by + r});
  }
  // One pixel of anti-aliasing all round.
  int x0 = static_cast<int>(std::floor(left)) - 1;
  int y0 = static_cast<int>(std::floor(top)) - 1;
  int w = static_cast<int>(std::ceil(right)) + 1 - x0 + 1;
  int h = static_cast<int>(std::ceil(bottom)) + 1 - y0 + 1;
  memset(art->pixels, 0, static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int i = 0; i < shape.count; i++)
    stamp(segs[i], dx, dy, art, x0, y0, w, h);
  set_header(art, w, h);
  *origin = {x0, y0};
  return true;
}

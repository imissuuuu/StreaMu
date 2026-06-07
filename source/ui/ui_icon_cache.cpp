#include "ui_icon_cache.h"

#include <array>
#include <citro2d.h>
#include <cstdlib>
#include <vector>

namespace {

struct IconCacheEntry {
  C3D_Tex tex = {};
  C2D_Image image = {};
  Tex3DS_SubTexture subtex = {};
  int width = 0;
  int height = 0;
  bool loaded = false;
};

constexpr size_t kIconCount = 8;
std::array<IconCacheEntry, kIconCount> g_icon_cache = {};
bool g_icon_cache_initialized = false;
IconCacheEntry g_separator_cache = {};

unsigned int next_pow2(unsigned int v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return v + 1;
}

u32 morton_interleave(u32 x, u32 y) {
  static const u32 xlut[] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
  static const u32 ylut[] = {0x00, 0x02, 0x08, 0x0a, 0x20, 0x22, 0x28, 0x2a};
  return xlut[x & 7] | ylut[y & 7];
}

void rgba8_to_tiled(u8 *out, const u8 *rgba, int w, int h, int tex_w,
                    int tex_h) {
  (void)tex_h;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const int tile_x = x / 8;
      const int tile_y = y / 8;
      const int fine_x = x & 7;
      const int fine_y = y & 7;
      const int tile_offset = (tile_y * (tex_w / 8) + tile_x) * (8 * 8 * 4);
      const int pixel_offset = morton_interleave(fine_x, fine_y) * 4;
      const int dst = tile_offset + pixel_offset;
      const int src = (y * w + x) * 4;
      out[dst + 0] = rgba[src + 3];
      out[dst + 1] = rgba[src + 2];
      out[dst + 2] = rgba[src + 1];
      out[dst + 3] = rgba[src + 0];
    }
  }
}

void fill_rect(std::vector<u8> &rgba, int width, int height, int x, int y,
               int rect_w, int rect_h) {
  if (rect_w <= 0 || rect_h <= 0) {
    return;
  }
  const int x0 = x < 0 ? 0 : x;
  const int y0 = y < 0 ? 0 : y;
  const int x1 = (x + rect_w) > width ? width : (x + rect_w);
  const int y1 = (y + rect_h) > height ? height : (y + rect_h);
  for (int py = y0; py < y1; ++py) {
    for (int px = x0; px < x1; ++px) {
      const size_t idx = static_cast<size_t>((py * width + px) * 4);
      rgba[idx + 0] = 255;
      rgba[idx + 1] = 255;
      rgba[idx + 2] = 255;
      rgba[idx + 3] = 255;
    }
  }
}

void draw_tri_right_mask(std::vector<u8> &rgba, int width, int height,
                         int center_x, int center_y, int half_h) {
  const int w = half_h * 2;
  for (int i = 0; i < w; ++i) {
    int h = static_cast<int>(half_h * (1.0f - static_cast<float>(i) / w));
    if (h < 1) {
      h = 1;
    }
    fill_rect(rgba, width, height, center_x - half_h + i, center_y - h, 1,
              h * 2);
  }
}

void draw_tri_left_mask(std::vector<u8> &rgba, int width, int height,
                        int center_x, int center_y, int half_h) {
  const int w = half_h * 2;
  for (int i = 0; i < w; ++i) {
    int h = static_cast<int>(half_h * (static_cast<float>(i + 1) / w));
    if (h < 1) {
      h = 1;
    }
    fill_rect(rgba, width, height, center_x - half_h + i, center_y - h, 1,
              h * 2);
  }
}

bool build_icon_texture(IconCacheEntry &entry, int width, int height,
                        const std::vector<u8> &rgba) {
  int tex_w = static_cast<int>(next_pow2(static_cast<unsigned>(width)));
  int tex_h = static_cast<int>(next_pow2(static_cast<unsigned>(height)));
  if (tex_w < 8) {
    tex_w = 8;
  }
  if (tex_h < 8) {
    tex_h = 8;
  }
  if (!C3D_TexInit(&entry.tex, tex_w, tex_h, GPU_RGBA8)) {
    return false;
  }

  const size_t tiled_size = static_cast<size_t>(tex_w * tex_h * 4);
  u8 *tiled = static_cast<u8 *>(calloc(1, tiled_size));
  if (!tiled) {
    C3D_TexDelete(&entry.tex);
    entry.tex = {};
    return false;
  }

  rgba8_to_tiled(tiled, rgba.data(), width, height, tex_w, tex_h);
  C3D_TexUpload(&entry.tex, tiled);
  C3D_TexSetFilter(&entry.tex, GPU_NEAREST, GPU_NEAREST);
  free(tiled);

  entry.subtex.width = static_cast<u16>(width);
  entry.subtex.height = static_cast<u16>(height);
  entry.subtex.left = 0.0f;
  entry.subtex.top = 1.0f;
  entry.subtex.right = static_cast<float>(width) / static_cast<float>(tex_w);
  entry.subtex.bottom =
      1.0f - static_cast<float>(height) / static_cast<float>(tex_h);
  entry.image.tex = &entry.tex;
  entry.image.subtex = &entry.subtex;
  entry.width = width;
  entry.height = height;
  entry.loaded = true;
  return true;
}

bool build_loop_icon(IconCacheEntry &entry) {
  constexpr int width = 22;
  constexpr int height = 22;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);

  fill_rect(rgba, width, height, 0, 3, 20, 2);
  fill_rect(rgba, width, height, 20, 3, 2, 16);
  fill_rect(rgba, width, height, 2, 17, 20, 2);
  fill_rect(rgba, width, height, 0, 3, 2, 16);
  fill_rect(rgba, width, height, 16, 0, 2, 3);
  fill_rect(rgba, width, height, 18, 1, 2, 2);
  fill_rect(rgba, width, height, 4, 19, 2, 3);
  fill_rect(rgba, width, height, 2, 19, 2, 2);

  return build_icon_texture(entry, width, height, rgba);
}

bool build_prev_icon(IconCacheEntry &entry) {
  constexpr int width = 25;
  constexpr int height = 18;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);
  fill_rect(rgba, width, height, 0, 0, 3, 18);
  draw_tri_left_mask(rgba, width, height, 16, 9, 9);
  return build_icon_texture(entry, width, height, rgba);
}

bool build_play_icon(IconCacheEntry &entry) {
  constexpr int width = 24;
  constexpr int height = 24;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);
  draw_tri_right_mask(rgba, width, height, 12, 12, 12);
  return build_icon_texture(entry, width, height, rgba);
}

bool build_pause_icon(IconCacheEntry &entry) {
  constexpr int width = 18;
  constexpr int height = 20;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);
  fill_rect(rgba, width, height, 0, 0, 5, 20);
  fill_rect(rgba, width, height, 13, 0, 5, 20);
  return build_icon_texture(entry, width, height, rgba);
}

bool build_next_icon(IconCacheEntry &entry) {
  constexpr int width = 25;
  constexpr int height = 18;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);
  draw_tri_right_mask(rgba, width, height, 10, 9, 9);
  fill_rect(rgba, width, height, 22, 0, 3, 18);
  return build_icon_texture(entry, width, height, rgba);
}

bool build_shuffle_icon(IconCacheEntry &entry) {
  constexpr int width = 32;
  constexpr int height = 32;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);

  fill_rect(rgba, width, height, 25, 5, 1, 1);
  fill_rect(rgba, width, height, 25, 6, 2, 1);
  fill_rect(rgba, width, height, 25, 7, 3, 1);
  fill_rect(rgba, width, height, 2, 8, 7, 1);
  fill_rect(rgba, width, height, 20, 8, 9, 1);
  fill_rect(rgba, width, height, 2, 9, 8, 1);
  fill_rect(rgba, width, height, 19, 9, 11, 1);
  fill_rect(rgba, width, height, 2, 10, 9, 1);
  fill_rect(rgba, width, height, 18, 10, 12, 1);
  fill_rect(rgba, width, height, 2, 11, 10, 1);
  fill_rect(rgba, width, height, 17, 11, 12, 1);
  fill_rect(rgba, width, height, 9, 12, 4, 1);
  fill_rect(rgba, width, height, 16, 12, 4, 1);
  fill_rect(rgba, width, height, 25, 12, 3, 1);
  fill_rect(rgba, width, height, 10, 13, 4, 1);
  fill_rect(rgba, width, height, 15, 13, 4, 1);
  fill_rect(rgba, width, height, 25, 13, 2, 1);
  fill_rect(rgba, width, height, 11, 14, 7, 1);
  fill_rect(rgba, width, height, 25, 14, 1, 1);
  fill_rect(rgba, width, height, 12, 15, 5, 1);
  fill_rect(rgba, width, height, 12, 16, 5, 1);
  fill_rect(rgba, width, height, 11, 17, 7, 1);
  fill_rect(rgba, width, height, 25, 17, 1, 1);
  fill_rect(rgba, width, height, 10, 18, 4, 1);
  fill_rect(rgba, width, height, 15, 18, 4, 1);
  fill_rect(rgba, width, height, 25, 18, 2, 1);
  fill_rect(rgba, width, height, 9, 19, 4, 1);
  fill_rect(rgba, width, height, 16, 19, 4, 1);
  fill_rect(rgba, width, height, 25, 19, 3, 1);
  fill_rect(rgba, width, height, 2, 20, 10, 1);
  fill_rect(rgba, width, height, 17, 20, 12, 1);
  fill_rect(rgba, width, height, 2, 21, 9, 1);
  fill_rect(rgba, width, height, 18, 21, 12, 1);
  fill_rect(rgba, width, height, 2, 22, 8, 1);
  fill_rect(rgba, width, height, 19, 22, 11, 1);
  fill_rect(rgba, width, height, 2, 23, 7, 1);
  fill_rect(rgba, width, height, 20, 23, 9, 1);
  fill_rect(rgba, width, height, 25, 24, 3, 1);
  fill_rect(rgba, width, height, 25, 25, 2, 1);
  fill_rect(rgba, width, height, 25, 26, 1, 1);

  return build_icon_texture(entry, width, height, rgba);
}

bool build_home_icon(IconCacheEntry &entry) {
  constexpr int width = 32;
  constexpr int height = 32;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);

  fill_rect(rgba, width, height, 26, 2, 2, 2);
  fill_rect(rgba, width, height, 14, 4, 4, 2);
  fill_rect(rgba, width, height, 24, 4, 4, 2);
  fill_rect(rgba, width, height, 12, 6, 8, 2);
  fill_rect(rgba, width, height, 24, 6, 4, 2);
  fill_rect(rgba, width, height, 10, 8, 12, 2);
  fill_rect(rgba, width, height, 24, 8, 4, 2);
  fill_rect(rgba, width, height, 8, 10, 20, 2);
  fill_rect(rgba, width, height, 6, 12, 22, 2);
  fill_rect(rgba, width, height, 4, 14, 26, 2);
  fill_rect(rgba, width, height, 2, 16, 28, 2);
  fill_rect(rgba, width, height, 6, 18, 20, 2);
  fill_rect(rgba, width, height, 6, 20, 6, 10);
  fill_rect(rgba, width, height, 20, 20, 6, 10);

  return build_icon_texture(entry, width, height, rgba);
}

bool build_menu_icon(IconCacheEntry &entry) {
  constexpr int width = 32;
  constexpr int height = 32;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);
  fill_rect(rgba, width, height, 4, 6, 24, 4);
  fill_rect(rgba, width, height, 4, 14, 24, 4);
  fill_rect(rgba, width, height, 4, 22, 24, 4);
  return build_icon_texture(entry, width, height, rgba);
}

bool build_separator_texture(IconCacheEntry &entry) {
  constexpr int width = 320;
  constexpr int height = 1;
  std::vector<u8> rgba(static_cast<size_t>(width * height * 4), 0);
  for (int x = 0; x < width; x += 6) {
    fill_rect(rgba, width, height, x, 0, 3, 1);
  }
  return build_icon_texture(entry, width, height, rgba);
}

bool build_icon(UiIconId id, IconCacheEntry &entry) {
  switch (id) {
  case UiIconId::Loop:
    return build_loop_icon(entry);
  case UiIconId::Prev:
    return build_prev_icon(entry);
  case UiIconId::Play:
    return build_play_icon(entry);
  case UiIconId::Pause:
    return build_pause_icon(entry);
  case UiIconId::Next:
    return build_next_icon(entry);
  case UiIconId::Shuffle:
    return build_shuffle_icon(entry);
  case UiIconId::Home:
    return build_home_icon(entry);
  case UiIconId::Menu:
    return build_menu_icon(entry);
  }
  return false;
}

} // namespace

bool init_ui_icon_cache() {
  if (g_icon_cache_initialized) {
    return true;
  }

  for (size_t i = 0; i < g_icon_cache.size(); ++i) {
    if (!build_icon(static_cast<UiIconId>(i), g_icon_cache[i])) {
      cleanup_ui_icon_cache();
      return false;
    }
  }
  if (!build_separator_texture(g_separator_cache)) {
    cleanup_ui_icon_cache();
    return false;
  }

  g_icon_cache_initialized = true;
  return true;
}

void cleanup_ui_icon_cache() {
  for (auto &entry : g_icon_cache) {
    if (entry.loaded) {
      C3D_TexDelete(&entry.tex);
      entry.tex = {};
      entry.image = {};
      entry.subtex = {};
      entry.width = 0;
      entry.height = 0;
      entry.loaded = false;
    }
  }
  if (g_separator_cache.loaded) {
    C3D_TexDelete(&g_separator_cache.tex);
    g_separator_cache.tex = {};
    g_separator_cache.image = {};
    g_separator_cache.subtex = {};
    g_separator_cache.width = 0;
    g_separator_cache.height = 0;
    g_separator_cache.loaded = false;
  }
  g_icon_cache_initialized = false;
}

void draw_ui_icon(UiIconId id, float x, float y, u32 color) {
  const size_t index = static_cast<size_t>(id);
  if (index >= g_icon_cache.size()) {
    return;
  }
  const IconCacheEntry &entry = g_icon_cache[index];
  if (!entry.loaded) {
    return;
  }
  C2D_ImageTint tint;
  C2D_PlainImageTint(&tint, color, 1.0f);
  C2D_DrawImageAt(entry.image, x, y, 0.0f, &tint);
}

void draw_ui_icon_centered(UiIconId id, float center_x, float center_y,
                           u32 color) {
  const size_t index = static_cast<size_t>(id);
  if (index >= g_icon_cache.size()) {
    return;
  }
  const IconCacheEntry &entry = g_icon_cache[index];
  if (!entry.loaded) {
    return;
  }
  C2D_ImageTint tint;
  C2D_PlainImageTint(&tint, color, 1.0f);
  C2D_DrawImageAt(entry.image, center_x - entry.width * 0.5f,
                  center_y - entry.height * 0.5f, 0.0f, &tint);
}

void draw_ui_separator(float x, float y, float width, u32 color) {
  if (!g_separator_cache.loaded || width <= 0.0f) {
    return;
  }
  C2D_ImageTint tint;
  C2D_PlainImageTint(&tint, color, 1.0f);
  C2D_DrawImageAt(g_separator_cache.image, x, y, 0.0f, &tint,
                  width / static_cast<float>(g_separator_cache.width), 1.0f);
}

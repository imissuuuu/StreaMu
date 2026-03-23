#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

#include "wallpaper.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Round up to next power of 2
static unsigned int next_pow2(unsigned int v) {
    v--;
    v |= v >> 1; v |= v >> 2; v |= v >> 4;
    v |= v >> 8; v |= v >> 16;
    return v + 1;
}

// Morton (Z-order) index within 8x8 tile
static u32 morton_interleave(u32 x, u32 y) {
    static const u32 xlut[] = {0x00,0x01,0x04,0x05,0x10,0x11,0x14,0x15};
    static const u32 ylut[] = {0x00,0x02,0x08,0x0a,0x20,0x22,0x28,0x2a};
    return xlut[x & 7] | ylut[y & 7];
}

// Convert RGBA8 pixel array to 3DS GPU tile format
// Output: ABGR8888, 8x8 Morton-order tiles, Y-flipped
static void rgba8_to_tiled(u8* out, const u8* rgba, int w, int h, int tex_w, int tex_h) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            // No Y-flip needed (subtex top > bottom handles it at display time)
            int flip_y = y;

            // Tile coordinates
            int tile_x = x / 8;
            int tile_y = flip_y / 8;
            int fine_x = x & 7;
            int fine_y = flip_y & 7;

            // Output offset (4 bytes per pixel)
            int tile_offset = (tile_y * (tex_w / 8) + tile_x) * (8 * 8 * 4);
            int pixel_offset = morton_interleave(fine_x, fine_y) * 4;
            int dst = tile_offset + pixel_offset;

            // Source pixel (RGBA)
            int src = (y * w + x) * 4;

            // GPU format: ABGR8888
            out[dst + 0] = rgba[src + 3]; // A
            out[dst + 1] = rgba[src + 2]; // B
            out[dst + 2] = rgba[src + 1]; // G
            out[dst + 3] = rgba[src + 0]; // R
        }
    }
}

Wallpaper::~Wallpaper() {
    unload();
}

bool Wallpaper::load(const std::string& path) {
    unload();

    // Read file
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 4 * 1024 * 1024) { // 4MB limit
        fclose(f);
        return false;
    }

    u8* file_data = static_cast<u8*>(malloc(fsize));
    if (!file_data) { fclose(f); return false; }
    size_t read_bytes = fread(file_data, 1, fsize, f);
    fclose(f);
    if (static_cast<long>(read_bytes) != fsize) { free(file_data); return false; }

    // Decode with stb_image
    int w = 0, h = 0, channels = 0;
    u8* rgba = stbi_load_from_memory(file_data, (int)fsize, &w, &h, &channels, 4);
    free(file_data);

    if (!rgba) return false;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) {
        stbi_image_free(rgba);
        return false;
    }

    m_orig_w = w;
    m_orig_h = h;

    // Texture size (power of 2)
    int tex_w = (int)next_pow2((unsigned)w);
    int tex_h = (int)next_pow2((unsigned)h);

    // Initialize GPU texture
    if (!C3D_TexInit(&m_tex, tex_w, tex_h, GPU_RGBA8)) {
        stbi_image_free(rgba);
        return false;
    }

    // Convert to tile format
    size_t tiled_size = tex_w * tex_h * 4;
    u8* tiled = static_cast<u8*>(calloc(1, tiled_size)); // Zero-init for padding areas
    if (!tiled) {
        C3D_TexDelete(&m_tex);
        stbi_image_free(rgba);
        return false;
    }

    rgba8_to_tiled(tiled, rgba, w, h, tex_w, tex_h);
    stbi_image_free(rgba);

    // Upload
    C3D_TexUpload(&m_tex, tiled);
    C3D_TexSetFilter(&m_tex, GPU_LINEAR, GPU_LINEAR);
    free(tiled);

    // Build C2D_Image (display only actual image region)
    // Note: top > bottom required or image rotates 90 degrees (citro2d behavior)
    m_subtex.width  = (u16)w;
    m_subtex.height = (u16)h;
    m_subtex.left   = 0.0f;
    m_subtex.top    = 1.0f;
    m_subtex.right  = (float)w / (float)tex_w;
    m_subtex.bottom = 1.0f - (float)h / (float)tex_h;

    m_img.tex    = &m_tex;
    m_img.subtex = &m_subtex;

    m_loaded = true;
    return true;
}

void Wallpaper::unload() {
    if (m_loaded) {
        C3D_TexDelete(&m_tex);
        m_loaded = false;
    }
    m_orig_w = 0;
    m_orig_h = 0;
}

void Wallpaper::draw_fullscreen() {
    if (!m_loaded) return;

    // Scale to top screen 400x240
    float sx = 400.0f / (float)m_orig_w;
    float sy = 240.0f / (float)m_orig_h;
    C2D_DrawImageAt(m_img, 0, 0, 0, nullptr, sx, sy);
}

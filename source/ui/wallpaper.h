#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>

class Wallpaper {
public:
    ~Wallpaper();
    bool load(const std::string& path);
    bool load_from_memory(const uint8_t* data, size_t size); // Decode JPEG/PNG from memory
    void unload();
    bool is_loaded() const { return m_loaded; }
    void draw_fullscreen(); // Draw fullscreen on top screen (400x240)
    void draw_at(float x, float y, float size); // Draw scaled/centered into a square

private:
    bool load_from_pixels(uint8_t* rgba, int w, int h); // Build GPU texture from decoded pixels
    C3D_Tex m_tex{};
    Tex3DS_SubTexture m_subtex{};
    C2D_Image m_img{};
    bool m_loaded = false;
    int m_orig_w = 0;
    int m_orig_h = 0;
};

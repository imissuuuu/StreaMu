#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>

class Wallpaper {
public:
    ~Wallpaper();
    bool load(const std::string& path);
    void unload();
    bool is_loaded() const { return m_loaded; }
    void draw_fullscreen(); // Draw fullscreen on top screen (400x240)

private:
    C3D_Tex m_tex{};
    Tex3DS_SubTexture m_subtex{};
    C2D_Image m_img{};
    bool m_loaded = false;
    int m_orig_w = 0;
    int m_orig_h = 0;
};

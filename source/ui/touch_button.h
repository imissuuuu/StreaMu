#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>

// A simple touch button class applying UI/UX "Impeccable" principles
// (Clear touch targets, feedback, accessible contrast)
class TouchButton {
public:
    float x, y, width, height;
    std::string text;
    u32 bg_color;
    u32 text_color;
    bool is_pressed;

    TouchButton(float _x, float _y, float _w, float _h, const std::string& _text)
        : x(_x), y(_y), width(_w), height(_h), text(_text), 
          bg_color(C2D_Color32(100, 100, 255, 255)), 
          text_color(C2D_Color32(255, 255, 255, 255)),
          is_pressed(false) {}

    void set_colors(u32 bg, u32 fg) {
        bg_color = bg;
        text_color = fg;
    }

    // Call this in the update loop with the current touch position
    bool update(const touchPosition& touch) {
        bool hovering = (touch.px >= x && touch.px <= x + width &&
                         touch.py >= y && touch.py <= y + height);
        
        // Simple press detection (in a real app, we'd check touch down vs touch release)
        // For now, if hovering and a touch is active, consider it pressed.
        is_pressed = hovering && (touch.px > 0 || touch.py > 0);
        return is_pressed;
    }

    void draw(C2D_TextBuf buf, bool is_selected = false,
              u32 cursor_color = C2D_Color32(0, 0, 0, 255)) {
        // Draw background (darker if pressed for feedback)
        // User request: avoid turning blue on press. Keep bg_color as-is.
        u32 draw_bg = bg_color;
        // Overlay semi-transparent black to darken slightly
        C2D_DrawRectSolid(x, y, 0, width, height, draw_bg);
        if (is_pressed) {
            C2D_DrawRectSolid(x, y, 0, width, height, C2D_Color32(0, 0, 0, 60));
        }

        // Outline border cursor
        if (is_selected) {
            const float b = 3.0f; // border thickness
            C2D_DrawRectSolid(x, y, 0, width, b, cursor_color);             // top
            C2D_DrawRectSolid(x, y + height - b, 0, width, b, cursor_color); // bottom
            C2D_DrawRectSolid(x, y, 0, b, height, cursor_color);             // left
            C2D_DrawRectSolid(x + width - b, y, 0, b, height, cursor_color); // right
        }

        if (!text.empty()) {
            C2D_Text c2d_text;
            C2D_TextParse(&c2d_text, buf, text.c_str());
            C2D_TextOptimize(&c2d_text);

            float text_w, text_h;
            C2D_TextGetDimensions(&c2d_text, 0.5f, 0.5f, &text_w, &text_h);
            float text_x = x + (width - text_w) / 2.0f;
            float text_y = y + (height - text_h) / 2.0f;
            C2D_DrawText(&c2d_text, C2D_WithColor, text_x, text_y, 0, 0.5f, 0.5f, text_color);
        }
    }

private:
    // No longer caching C2D_Text because the global allocator buf is wiped every frame.
};

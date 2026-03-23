#pragma once
#include "ui_manager.h"
#include "ui_renderer.h"
#include "ui_constants.h"
#include <string>

namespace PlayBar {
    constexpr float BAR_Y  = 200.0f;
    constexpr float BAR_H  = 40.0f;
    // 5 zones: LOOP | PREV | PLAY/PAUSE | NEXT | SHUF
    constexpr float ZONE_1 = 55.0f;
    constexpr float ZONE_2 = 118.0f;
    constexpr float ZONE_3 = 202.0f;
    constexpr float ZONE_4 = 265.0f;

    inline void draw(const RenderContext& ctx, UIManager& ui_mgr) {
        C2D_TextBuf buf = ui_mgr.get_text_buf();
        C2D_Text text;

        u32 bar_bg = (ctx.config.mode == THEME_DARK)
                     ? C2D_Color32(55, 55, 55, 255)
                     : C2D_Color32(230, 230, 230, 255);
        u32 bar_text = ctx.theme->text_body;

        if (ctx.playing_id.empty()) return;

        C2D_DrawRectSolid(0, BAR_Y, 0, 320, BAR_H, bar_bg);

        bool has_queue = !ctx.play_queue.empty();
        u32 skip_color = has_queue ? bar_text : (ctx.theme->text_dim & 0x60FFFFFF);
        u32 dim_color  = ctx.theme->text_dim & 0x80FFFFFF;

        // Divider lines (4)
        C2D_DrawRectSolid(ZONE_1, BAR_Y, 0, 1, BAR_H, dim_color);
        C2D_DrawRectSolid(ZONE_2, BAR_Y, 0, 1, BAR_H, dim_color);
        C2D_DrawRectSolid(ZONE_3, BAR_Y, 0, 1, BAR_H, dim_color);
        C2D_DrawRectSolid(ZONE_4, BAR_Y, 0, 1, BAR_H, dim_color);

        float cy = BAR_Y + 20.0f; // vertical center of bar

        // === Helper: equilateral-looking triangle (width ≈ height) ===
        // half_h = half of total height. Width = half_h * 2 (1px columns)

        // ▶ right-pointing: flat base on left, point on right
        auto draw_tri_right = [&](float cx, float cy, int half_h, u32 c) {
            int w = half_h * 2;
            for (int i = 0; i < w; i++) {
                float h = half_h * (1.0f - (float)i / w);
                if (h < 1) h = 1;
                C2D_DrawRectSolid(cx - half_h + i, cy - h, 0, 1, h * 2, c);
            }
        };

        // ◀ left-pointing: point on left, flat base on right (exact mirror)
        auto draw_tri_left = [&](float cx, float cy, int half_h, u32 c) {
            int w = half_h * 2;
            for (int i = 0; i < w; i++) {
                float h = half_h * ((float)(i + 1) / w);
                if (h < 1) h = 1;
                C2D_DrawRectSolid(cx - half_h + i, cy - h, 0, 1, h * 2, c);
            }
        };

        // --- LOOP icon (0-55, center=27.5) ---
        {
            u32 lc = ctx.loop_mode == LOOP_OFF ? dim_color : bar_text;
            float lx = 27.5f;
            float w = 22.0f, h = 16.0f;
            float x0 = lx - w/2, y0 = cy - h/2;
            C2D_DrawRectSolid(x0, y0, 0, w - 2, 2, lc);       // top
            C2D_DrawRectSolid(x0 + w - 2, y0, 0, 2, h, lc);   // right
            C2D_DrawRectSolid(x0 + 2, y0 + h - 2, 0, w - 2, 2, lc); // bottom
            C2D_DrawRectSolid(x0, y0, 0, 2, h, lc);            // left
            // Arrow top-right →
            C2D_DrawRectSolid(x0 + w - 6, y0 - 3, 0, 2, 3, lc);
            C2D_DrawRectSolid(x0 + w - 4, y0 - 2, 0, 2, 2, lc);
            // Arrow bottom-left ←
            C2D_DrawRectSolid(x0 + 4, y0 + h, 0, 2, 3, lc);
            C2D_DrawRectSolid(x0 + 2, y0 + h, 0, 2, 2, lc);

            if (ctx.loop_mode == LOOP_ONE) {
                C2D_TextParse(&text, buf, "1");
                C2D_DrawText(&text, C2D_WithColor, lx - 3, cy - 7, 0,
                             FONT_SM, FONT_SM, lc);
            }
        }

        // --- PREV icon (55-118, center=86.5): |◀ ---
        {
            float px = 86.5f;
            C2D_DrawRectSolid(px - 13, cy - 9, 0, 3, 18, skip_color);
            draw_tri_left(px + 3, cy, 9, skip_color);
        }

        // --- PLAY/PAUSE icon (118-202, center=160) ---
        {
            float px = 160.0f;
            if (ctx.is_paused) {
                // ▶ large: half_h=12 → 24x24px
                draw_tri_right(px, cy, 12, bar_text);
            } else {
                // || large
                C2D_DrawRectSolid(px - 9, cy - 10, 0, 5, 20, bar_text);
                C2D_DrawRectSolid(px + 4, cy - 10, 0, 5, 20, bar_text);
            }
        }

        // --- NEXT icon (202-265, center=233.5): ▶| ---
        {
            float nx = 233.5f;
            draw_tri_right(nx - 3, cy, 9, skip_color);
            C2D_DrawRectSolid(nx + 10, cy - 9, 0, 3, 18, skip_color);
        }

        // --- SHUF icon (265-320, center=292.5) ---
        // 32x32 pixel art, 1px per dot
        {
            u32 sc = has_queue ? (ctx.shuffle_mode ? bar_text : dim_color) : dim_color;
            float x0 = 292.5f - 16;  // center 32px icon
            float y0 = cy - 16;
            // Arrow tip top-right
            C2D_DrawRectSolid(x0+25, y0+5,  0, 1, 1, sc);
            C2D_DrawRectSolid(x0+25, y0+6,  0, 2, 1, sc);
            C2D_DrawRectSolid(x0+25, y0+7,  0, 3, 1, sc);
            // Top arms
            C2D_DrawRectSolid(x0+2,  y0+8,  0, 7, 1, sc); C2D_DrawRectSolid(x0+20, y0+8,  0, 9, 1, sc);
            C2D_DrawRectSolid(x0+2,  y0+9,  0, 8, 1, sc); C2D_DrawRectSolid(x0+19, y0+9,  0, 11, 1, sc);
            C2D_DrawRectSolid(x0+2,  y0+10, 0, 9, 1, sc); C2D_DrawRectSolid(x0+18, y0+10, 0, 12, 1, sc);
            C2D_DrawRectSolid(x0+2,  y0+11, 0, 10, 1, sc); C2D_DrawRectSolid(x0+17, y0+11, 0, 12, 1, sc);
            // Cross upper
            C2D_DrawRectSolid(x0+9,  y0+12, 0, 4, 1, sc); C2D_DrawRectSolid(x0+16, y0+12, 0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+12, 0, 3, 1, sc);
            C2D_DrawRectSolid(x0+10, y0+13, 0, 4, 1, sc); C2D_DrawRectSolid(x0+15, y0+13, 0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+13, 0, 2, 1, sc);
            C2D_DrawRectSolid(x0+11, y0+14, 0, 7, 1, sc); C2D_DrawRectSolid(x0+25, y0+14, 0, 1, 1, sc);
            // Center
            C2D_DrawRectSolid(x0+12, y0+15, 0, 5, 1, sc);
            C2D_DrawRectSolid(x0+12, y0+16, 0, 5, 1, sc);
            // Cross lower
            C2D_DrawRectSolid(x0+11, y0+17, 0, 7, 1, sc); C2D_DrawRectSolid(x0+25, y0+17, 0, 1, 1, sc);
            C2D_DrawRectSolid(x0+10, y0+18, 0, 4, 1, sc); C2D_DrawRectSolid(x0+15, y0+18, 0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+18, 0, 2, 1, sc);
            C2D_DrawRectSolid(x0+9,  y0+19, 0, 4, 1, sc); C2D_DrawRectSolid(x0+16, y0+19, 0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+19, 0, 3, 1, sc);
            // Bottom arms
            C2D_DrawRectSolid(x0+2,  y0+20, 0, 10, 1, sc); C2D_DrawRectSolid(x0+17, y0+20, 0, 12, 1, sc);
            C2D_DrawRectSolid(x0+2,  y0+21, 0, 9, 1, sc); C2D_DrawRectSolid(x0+18, y0+21, 0, 12, 1, sc);
            C2D_DrawRectSolid(x0+2,  y0+22, 0, 8, 1, sc); C2D_DrawRectSolid(x0+19, y0+22, 0, 11, 1, sc);
            C2D_DrawRectSolid(x0+2,  y0+23, 0, 7, 1, sc); C2D_DrawRectSolid(x0+20, y0+23, 0, 9, 1, sc);
            // Arrow tip bottom-right
            C2D_DrawRectSolid(x0+25, y0+24, 0, 3, 1, sc);
            C2D_DrawRectSolid(x0+25, y0+25, 0, 2, 1, sc);
            C2D_DrawRectSolid(x0+25, y0+26, 0, 1, 1, sc);
        }
    }

    inline std::string handle_touch(int tx, int ty, const RenderContext& ctx) {
        if (ty < (int)BAR_Y || ctx.playing_id.empty()) return "";
        if (tx < (int)ZONE_1) return "toggle_loop";
        if (tx < (int)ZONE_2) return ctx.play_queue.empty() ? "" : "prev_track";
        if (tx < (int)ZONE_3) return "toggle_pause";
        if (tx < (int)ZONE_4) return ctx.play_queue.empty() ? "" : "next_track";
        return ctx.play_queue.empty() ? "" : "toggle_shuffle";
    }
}

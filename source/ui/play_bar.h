#pragma once
#include "ui_manager.h"
#include "ui_renderer.h"
#include "ui_constants.h"
#include <string>

namespace PlayBar {
    constexpr float BAR_Y  = 200.0f;
    constexpr float BAR_H  = 40.0f;
    constexpr float SEEK_H = 14.0f;  // Seek bar height at top of BAR
    // 5 zones: LOOP | PREV | PLAY/PAUSE | NEXT | SHUF (below seek bar)
    constexpr float ZONE_1 = 55.0f;
    constexpr float ZONE_2 = 118.0f;
    constexpr float ZONE_3 = 202.0f;
    constexpr float ZONE_4 = 265.0f;

    // Parse "M:SS" or "H:MM:SS" duration string to total seconds. Returns 0 on failure.
    inline int parse_duration_secs(const std::string& dur) {
        if (dur.empty() || dur == "?") return 0;
        int parts[3] = {0, 0, 0};
        int count = 0;
        size_t start = 0;
        for (size_t i = 0; i <= dur.size() && count < 3; ++i) {
            if (i == dur.size() || dur[i] == ':') {
                if (i > start) {
                    parts[count] = 0;
                    for (size_t j = start; j < i; ++j)
                        parts[count] = parts[count] * 10 + (dur[j] - '0');
                    ++count;
                }
                start = i + 1;
            }
        }
        if (count == 2) return parts[0] * 60 + parts[1];
        if (count == 3) return parts[0] * 3600 + parts[1] * 60 + parts[2];
        return 0;
    }

    inline void draw(const RenderContext& ctx, UIManager& ui_mgr) {
        C2D_TextBuf buf = ui_mgr.get_text_buf();
        C2D_Text text;

        u32 bar_bg = (ctx.config.mode == THEME_DARK)
                     ? C2D_Color32(55, 55, 55, 255)
                     : C2D_Color32(230, 230, 230, 255);
        u32 bar_text = ctx.theme->text_body;

        if (ctx.playing_id.empty()) return;

        C2D_DrawRectSolid(0, BAR_Y, 0, 320, BAR_H, bar_bg);

        // === Seek bar (top 8px of play bar) ===
        {
            int total_secs = parse_duration_secs(ctx.playing_duration);
            if (total_secs > 0) {
                // Check if user is currently dragging on the seek bar
                const TouchState& ts = ctx.touch_state;
                bool is_drag_seeking = ts.is_touching
                    && ts.start_y >= (int)BAR_Y
                    && ts.start_y < (int)(BAR_Y + SEEK_H);

                float ratio;
                int elapsed;
                if (is_drag_seeking) {
                    // Follow finger during drag
                    ratio = (float)ts.current_x / 320.0f;
                    if (ratio < 0.0f) ratio = 0.0f;
                    if (ratio > 1.0f) ratio = 1.0f;
                    elapsed = (int)(ratio * total_secs);
                } else {
                    u64 now = osGetTime();
                    u64 total_paused = ctx.pause_accumulated_ms;
                    if ((ctx.is_paused || ctx.is_buffering) && ctx.pause_started_at > 0)
                        total_paused += now - ctx.pause_started_at;
                    elapsed = (ctx.playback_start_time > 0)
                        ? (int)((now - ctx.playback_start_time - total_paused) / 1000ULL)
                        : 0;
                    if (elapsed < 0) elapsed = 0;
                    if (elapsed > total_secs) elapsed = total_secs;
                    ratio = (float)elapsed / (float)total_secs;
                }

                // Filled portion — clearly distinct from both bar_bg and text_body
                u32 seek_fill = (ctx.config.mode == THEME_DARK)
                    ? C2D_Color32(100, 100, 100, 255)
                    : C2D_Color32(145, 145, 145, 255);
                float fill_w = ratio * 320.0f;
                C2D_DrawRectSolid(0, BAR_Y, 0, fill_w, SEEK_H, seek_fill);

                // Playhead (4px wide, contrasting dot)
                u32 head_color = (ctx.config.mode == THEME_DARK)
                    ? C2D_Color32(30, 30, 30, 255)
                    : C2D_Color32(255, 255, 255, 255);
                float head_x = fill_w - 2.0f;
                if (head_x < 0.0f) head_x = 0.0f;
                C2D_DrawRectSolid(head_x, BAR_Y, 0, 4.0f, SEEK_H, head_color);

                // Font size: max that fits within SEEK_H=14px (30pt * 0.44 ≈ 13.2px)
                constexpr float FONT_TIME = 0.44f;

                // Elapsed time — left side
                char el_buf[16];
                int el_m = elapsed / 60, el_s = elapsed % 60;
                snprintf(el_buf, sizeof(el_buf), "%d:%02d", el_m, el_s);
                C2D_Text elapsed_text;
                C2D_TextParse(&elapsed_text, buf, el_buf);
                C2D_DrawText(&elapsed_text, C2D_WithColor, 2.0f, BAR_Y, 0, FONT_TIME, FONT_TIME, bar_text & 0xA0FFFFFF);

                // Total duration — right side (fixed string from ctx)
                if (!ctx.playing_duration.empty() && ctx.playing_duration != "?") {
                    C2D_Text dur_text;
                    float tw = 0, th = 0;
                    C2D_TextParse(&dur_text, buf, ctx.playing_duration.c_str());
                    C2D_TextGetDimensions(&dur_text, FONT_TIME, FONT_TIME, &tw, &th);
                    C2D_DrawText(&dur_text, C2D_WithColor, 320.0f - tw - 2.0f, BAR_Y, 0, FONT_TIME, FONT_TIME, bar_text & 0xA0FFFFFF);
                }
            }
        }

        bool has_queue = !ctx.play_queue.empty();
        u32 skip_color = has_queue ? bar_text : (ctx.theme->text_dim & 0x60FFFFFF);
        u32 dim_color  = ctx.theme->text_dim & 0x80FFFFFF;

        // Divider lines (4, below seek bar)
        C2D_DrawRectSolid(ZONE_1, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);
        C2D_DrawRectSolid(ZONE_2, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);
        C2D_DrawRectSolid(ZONE_3, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);
        C2D_DrawRectSolid(ZONE_4, BAR_Y + SEEK_H, 0, 1, BAR_H - SEEK_H, dim_color);

        float cy = BAR_Y + SEEK_H + (BAR_H - SEEK_H) / 2.0f; // center of button area

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
            u32 lc = (ctx.play_queue.empty() || ctx.loop_mode == LOOP_OFF) ? dim_color : bar_text;
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
        if (ty < (int)(BAR_Y + SEEK_H)) return ""; // seek bar handled by caller
        if (tx < (int)ZONE_1) return ctx.play_queue.empty() ? "" : "toggle_loop";
        if (tx < (int)ZONE_2) return ctx.play_queue.empty() ? "" : "prev_track";
        if (tx < (int)ZONE_3) return "toggle_pause";
        if (tx < (int)ZONE_4) return ctx.play_queue.empty() ? "" : "next_track";
        return ctx.play_queue.empty() ? "" : "toggle_shuffle";
    }
}

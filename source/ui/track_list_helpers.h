#pragma once
#include <3ds.h>
#include "ui_renderer.h"
#include "ui_constants.h"
#include "../app_context.h"

// Forward declarations
class UIManager;

// Shared drawing helper for track list (used by SearchScreen and PlaylistDetailScreen)
void draw_track_list_bottom(const RenderContext& ctx, UIManager& ui_mgr, bool show_views = true);

// PlayingScreen-style track list: 30px items, y=8..200, used by PlayingScreen and SearchScreen
void draw_playing_style_list(
    const std::vector<Track>& tracks,
    const RenderContext& ctx,
    UIManager& ui_mgr
);

// Shared DPad navigation for g_tracks list (wrap-around + horizontal scroll)
void navigate_track_list(AppContext& ctx, u32 kRepeat);

// Hamburger menu button
struct MenuBtnRect { int x, y, w, h; };

inline MenuBtnRect get_menu_btn_rect(const AppConfig& cfg) {
    int bx = (cfg.menu_btn_side == MENU_BTN_RIGHT) ? MENU_BTN_RX : MENU_BTN_LX;
    return { bx, MENU_BTN_Y, MENU_BTN_W, MENU_BTN_H };
}

// Draw menu button on bottom screen (lower-left or lower-right)
void draw_menu_button(const RenderContext& ctx, UIManager& ui_mgr);

// --- List restyle helpers ---

// Per-item background for normal (non-selected, non-playing) list items
// Slightly lighter than bg_bottom to distinguish "list" from "background"
inline void draw_item_bg(float x, float y, float w, float h, u32 bg_bottom) {
    u8 r = (bg_bottom >> 24) & 0xFF;
    u8 g = (bg_bottom >> 16) & 0xFF;
    u8 b = (bg_bottom >> 8)  & 0xFF;
    u8 a = bg_bottom & 0xFF;
    // Add flat value to avoid color shift (no proportional calc)
    int add = (r < 128) ? 18 : 8;  // Dark: +18, Light: +8
    r = (r + add > 255) ? 255 : r + add;
    g = (g + add > 255) ? 255 : g + add;
    b = (b + add > 255) ? 255 : b + add;
    u32 item_bg = (r << 24) | (g << 16) | (b << 8) | a;
    C2D_DrawRectSolid(x, y, 0, w, h, item_bg);
}

// Dashed horizontal line (citro2d has no dashed API, so we draw short rects)
inline void draw_dashed_line_h(float x, float y, float width, u32 color,
                                float dash_len = 3.0f, float gap_len = 3.0f) {
    for (float dx = 0; dx < width; dx += dash_len + gap_len) {
        float seg = dash_len;
        if (dx + seg > width) seg = width - dx;
        C2D_DrawRectSolid(x + dx, y, 0, seg, 1, color);
    }
}

// Left accent bar for selected items (3px wide, brightened accent)
inline void draw_selection_left_bar(float y, float h, u32 accent) {
    u8 r = (accent >> 24) & 0xFF;
    u8 g = (accent >> 16) & 0xFF;
    u8 b = (accent >> 8)  & 0xFF;
    u8 a = accent & 0xFF;
    r = r + (255 - r) / 4;
    g = g + (255 - g) / 4;
    b = b + (255 - b) / 4;
    u32 bright = (r << 24) | (g << 16) | (b << 8) | a;
    C2D_DrawRectSolid(0, y, 0, 3, h, bright);
}

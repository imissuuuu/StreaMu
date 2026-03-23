#pragma once
#include <3ds.h>
#include "ui_renderer.h"
#include "ui_constants.h"
#include "../app_context.h"

// Forward declarations
class UIManager;

// Shared drawing helper for track list (used by SearchScreen and PlaylistDetailScreen)
void draw_track_list_bottom(const RenderContext& ctx, UIManager& ui_mgr);

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

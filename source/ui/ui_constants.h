#pragma once

// === Layout Constants (Base-4 Grid) ===

// Top Screen (400x240)
constexpr float TOP_MARGIN_X     = 8.0f;
constexpr float TOP_SYS_Y        = 4.0f;
constexpr float TOP_TITLE_Y      = 21.0f;
constexpr float TOP_STATUS_Y     = 60.0f;
constexpr float TOP_PLAYING_Y    = 84.0f;
constexpr float TOP_PLAYING_LINE = 14.0f;
constexpr float TOP_OFFLINE_X    = 205.0f;
constexpr float TOP_ITEM_HEIGHT  = 28.0f;

// Bottom Screen (320x240)
constexpr float BTM_MARGIN_X         = 8.0f;
constexpr float BTM_LIST_START_Y     = 8.0f;
constexpr float BTM_ITEM_HEIGHT      = 20.0f;
constexpr float BTM_ITEM_HEIGHT_2ROW = 30.0f;
constexpr float BTM_META_OFFSET      = 13.0f;
constexpr float BTM_HOME_HEIGHT      = 28.0f;
constexpr int   BTM_MAX_VISIBLE      = 11;
constexpr int   BTM_MAX_VISIBLE_2ROW = 7;

// Popup
constexpr float POPUP_MARGIN_X   = 20.0f;
constexpr float POPUP_WIDTH      = 280.0f;
constexpr float POPUP_INNER_X    = 28.0f;
constexpr float POPUP_HEADER_H   = 30.0f;
constexpr float POPUP_ITEM_H     = 30.0f;
constexpr int   POPUP_MAX_H      = 240;

// Font Scales
constexpr float FONT_XS          = 0.45f;

// Scroll helper: decompose scroll_offset into integer item count + fractional pixel offset
// scroll_items : number of fully scrolled items (can be negative)
// y_shift      : fractional pixel amount [0, item_height). Use as y_base = START_Y - y_shift
inline void calc_scroll(float scroll_offset, float item_height,
                        int& scroll_items, float& y_shift) {
    if (item_height <= 0.0f) { scroll_items = 0; y_shift = 0.0f; return; }
    scroll_items = (int)(scroll_offset / item_height);
    // Floor for negative values
    if (scroll_offset < 0.0f && scroll_offset < (float)scroll_items * item_height)
        scroll_items--;
    y_shift = scroll_offset - (float)scroll_items * item_height;
    // y_shift is always [0, item_height)
}
constexpr float FONT_SM          = 0.50f;
constexpr float FONT_MD          = 0.55f;
constexpr float FONT_LG          = 0.65f;

// Now Playing window (bottom of top screen, fixed size)
constexpr float TOP_NOW_PLAYING_WIN_Y = 190.0f;
constexpr float TOP_NOW_PLAYING_WIN_W = 400.0f;
constexpr float TOP_NOW_PLAYING_WIN_H = 50.0f;
constexpr float TOP_NOW_PLAYING_PAD   = 2.0f;

// Home Screen 2-Column Layout
constexpr float HOME_LEFT_X       = 0.0f;
constexpr float HOME_RIGHT_X      = 160.0f;
constexpr float HOME_COL_W        = 160.0f;
constexpr float HOME_AREA_TOP     = 0.0f;
constexpr float HOME_AREA_BOTTOM  = 200.0f;   // PlayBar start position
constexpr float HOME_TILE_H       = 66.0f;    // 200/3 (right column items)
constexpr float HOME_QA_H         = 132.0f;   // 66*2 (Quick Access)
constexpr float HOME_TILE_FONT    = 0.70f;    // Tile label font
constexpr float HOME_QA_HEADER_FONT = 0.50f;  // QA header font
constexpr float HOME_QA_SLOT_FONT = 0.60f;    // QA slot font
constexpr float HOME_BORDER_W     = 2.0f;     // Border width
constexpr float HOME_QA_SLOT_H    = 22.0f;    // QA slot row height
constexpr float HOME_TILE_PAD     = 4.0f;     // Tile inner padding

// Hamburger menu button
constexpr int MENU_BTN_Y  = 206;
constexpr int MENU_BTN_W  = 40;
constexpr int MENU_BTN_H  = 30;
constexpr int MENU_BTN_LX = 4;    // Left-aligned x
constexpr int MENU_BTN_RX = 276;  // Right-aligned x (320 - 40 - 4)

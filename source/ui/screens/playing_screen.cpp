#include "playing_screen.h"
#include "../ui_constants.h"
#include "../track_list_helpers.h"
#include <time.h>

void PlayingScreen::on_enter(AppContext& ctx) {
    ctx.current_state   = STATE_PLAYING_UI;
    ctx.scroll_offset_y = 0.0f;
    // Align selected_index with currently playing track
    ctx.selected_index = 0;
    for (int i = 0; i < (int)ctx.playing_tracks.size(); ++i) {
        if (ctx.playing_tracks[i].id == ctx.playing_id) {
            ctx.selected_index = i;
            break;
        }
    }

    // Thumbnail fetch handled asynchronously in main loop
}

// ============================================================
// Top screen (unused; main.cpp calls renderer.draw_top_screen)
// ============================================================
void PlayingScreen::draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
    (void)ctx; (void)ui_mgr;
}

// ============================================================
// Bottom screen
// ============================================================
void PlayingScreen::draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    C2D_TextBuf buf = ui_mgr.get_text_buf();
    C2D_Text text;

    // --- Track list (y=LIST_START_Y..BAR_Y) ---
    if (!ctx.playing_tracks.empty()) {
        int scroll_items;
        float y_shift;
        calc_scroll(ctx.scroll_offset_y, ITEM_H, scroll_items, y_shift);
        float y_base = LIST_START_Y - y_shift;
        int first    = scroll_items;
        int visible  = (int)((LIST_END_Y - LIST_START_Y) / ITEM_H) + 1;

        for (int i = first; i < first + visible; ++i) {
            if (i < 0 || i >= (int)ctx.playing_tracks.size()) continue;
            float y_item = y_base + (i - first) * ITEM_H;
            if (y_item + ITEM_H < LIST_START_Y || y_item >= LIST_END_Y) continue;

            const Track& tr   = ctx.playing_tracks[i];
            bool is_playing   = (tr.id == ctx.playing_id);
            bool is_selected  = (i == ctx.selected_index);

            // BG: selected -> accent + left bar, playing -> accent tint
            if (is_selected) {
                C2D_DrawRectSolid(0, y_item, 0, 320, ITEM_H, ctx.theme->accent);
                draw_selection_left_bar(y_item, ITEM_H, ctx.theme->accent);
            } else if (is_playing) {
                C2D_DrawRectSolid(0, y_item, 0, 320, ITEM_H, ctx.theme->playing_bg);
            } else {
                draw_item_bg(0, y_item, 320, ITEM_H, ctx.theme->bg_bottom);
            }

            u32 title_color = (is_selected || is_playing) ? ctx.theme->accent_text
                                                          : ctx.theme->text_body;

            // Add ">>" indicator for playing track
            std::string display_title = is_playing ? (">> " + tr.title) : tr.title;
            C2D_TextParse(&text, buf, display_title.c_str());
            {
                float draw_x = BTM_MARGIN_X;
                if (is_selected) {
                    float text_w    = text.width * FONT_SM;
                    float display_w = 320.0f - BTM_MARGIN_X * 2;
                    float eff_scroll = (text_w > display_w)
                                       ? std::min((float)ctx.scroll_x, text_w - display_w)
                                       : 0.0f;
                    draw_x = BTM_MARGIN_X - eff_scroll;
                }
                C2D_DrawText(&text, C2D_WithColor, draw_x, y_item, 0,
                             FONT_SM, FONT_SM, title_color);
            }

            if (!tr.duration.empty()) {
                C2D_TextParse(&text, buf, tr.duration.c_str());
                C2D_DrawText(&text, C2D_WithColor, BTM_MARGIN_X + 8, y_item + BTM_META_OFFSET, 0,
                             FONT_XS, FONT_XS, title_color);
            }
            // Dashed separator
            if (!is_selected) {
                u32 sep = ctx.theme->text_dim & 0x14FFFFFF;
                draw_dashed_line_h(0, y_item + ITEM_H - 1, 320, sep);
            }
        }
    }

    // --- Play bar (shared component) ---
    PlayBar::draw(ctx, ui_mgr);

    // HOME button: bottom=play bar top(y=170), right=screen edge(x=280), 40x30
    C2D_DrawRectSolid(HOME_BTN_X, HOME_BTN_Y, 0, MENU_BTN_W, MENU_BTN_H,
                      ctx.theme->text_dim & 0xC0FFFFFF);
    // Home icon (16x16 grid scaled 2x → 32x32, centered in 40x30 button)
    {
        u32 ic = ctx.theme->text_dim;
        float ix = HOME_BTN_X + 4.0f;   // (40-32)/2
        float iy = HOME_BTN_Y - 1.0f;   // (30-32)/2
        // Row 1: chimney tip
        C2D_DrawRectSolid(ix+26, iy+2,  0, 2, 2, ic);
        // Row 2: roof peak + chimney
        C2D_DrawRectSolid(ix+14, iy+4,  0, 4, 2, ic);
        C2D_DrawRectSolid(ix+24, iy+4,  0, 4, 2, ic);
        // Row 3: roof wider + chimney
        C2D_DrawRectSolid(ix+12, iy+6,  0, 8, 2, ic);
        C2D_DrawRectSolid(ix+24, iy+6,  0, 4, 2, ic);
        // Row 4: roof wider + chimney
        C2D_DrawRectSolid(ix+10, iy+8,  0, 12, 2, ic);
        C2D_DrawRectSolid(ix+24, iy+8,  0, 4, 2, ic);
        // Row 5: roof connects chimney
        C2D_DrawRectSolid(ix+8,  iy+10, 0, 20, 2, ic);
        // Row 6
        C2D_DrawRectSolid(ix+6,  iy+12, 0, 22, 2, ic);
        // Row 7
        C2D_DrawRectSolid(ix+4,  iy+14, 0, 26, 2, ic);
        // Row 8: roof base
        C2D_DrawRectSolid(ix+2,  iy+16, 0, 28, 2, ic);
        // Row 9: wall top
        C2D_DrawRectSolid(ix+6,  iy+18, 0, 20, 2, ic);
        // Rows 10-14: left wall + right wall
        C2D_DrawRectSolid(ix+6,  iy+20, 0, 6, 10, ic);
        C2D_DrawRectSolid(ix+20, iy+20, 0, 6, 10, ic);
    }
}

// ============================================================
// Input handling
// ============================================================
std::string PlayingScreen::update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
    touchPosition touch;
    hidTouchRead(&touch);

    if (kDown & KEY_TOUCH) {
        ctx.touch_state.begin(touch.px, touch.py);
    }
    if ((kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        int delta = ctx.touch_state.update(touch.px, touch.py);
        // Only scroll when dragging within list area
        if (ctx.touch_state.is_dragging &&
            ctx.touch_state.start_y >= (int)LIST_START_Y &&
            ctx.touch_state.start_y < (int)PlayBar::BAR_Y &&
            !ctx.playing_tracks.empty()) {
            ctx.scroll_offset_y += (float)delta;
            int visible = (int)((LIST_END_Y - LIST_START_Y) / ITEM_H);
            float max_scroll = ((int)ctx.playing_tracks.size() - visible) * ITEM_H;
            if (ctx.scroll_offset_y < 0.0f) ctx.scroll_offset_y = 0.0f;
            if (max_scroll <= 0.0f) ctx.scroll_offset_y = 0.0f;
            else if (ctx.scroll_offset_y > max_scroll) ctx.scroll_offset_y = max_scroll;
        }
    }
    if (!(kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        bool was_tap = ctx.touch_state.end();
        if (was_tap) {
            int tx = ctx.touch_state.current_x;
            int ty = ctx.touch_state.current_y;

            // HOME button: x=HOME_BTN_X..320, y=HOME_BTN_Y..BAR_Y
            if (tx >= (int)HOME_BTN_X && ty >= (int)HOME_BTN_Y && ty < (int)PlayBar::BAR_Y) {
                return "trigger_home";
            } else if (ty >= (int)PlayBar::BAR_Y) {
                std::string bar_action = PlayBar::handle_touch(tx, ty, ctx);
                if (!bar_action.empty()) return bar_action;
            } else if (ty >= (int)LIST_START_Y && !ctx.playing_tracks.empty()) {
                // List item tap -> select and play
                int tapped = (int)((ctx.scroll_offset_y + ty - LIST_START_Y) / ITEM_H);
                if (tapped >= 0 && tapped < (int)ctx.playing_tracks.size()) {
                    ctx.selected_index = tapped;
                    return "play_selected_track";
                }
            }
        }
    }

    // D-pad cursor movement + scroll follow (same behavior as search/playlist screens)
    LightLock_Lock(&ctx.lock);
    if (!ctx.playing_tracks.empty()) {
        bool moved = false;
        if (kRepeat & KEY_DDOWN) {
            ctx.selected_index++;
            if (ctx.selected_index >= (int)ctx.playing_tracks.size())
                ctx.selected_index = 0;
            ctx.scroll_x = 0;
            moved = true;
        }
        if (kRepeat & KEY_DUP) {
            ctx.selected_index--;
            if (ctx.selected_index < 0)
                ctx.selected_index = (int)ctx.playing_tracks.size() - 1;
            ctx.scroll_x = 0;
            moved = true;
        }
        if (moved) {
            // Auto-scroll to keep selected_index visible
            int visible = (int)((LIST_END_Y - LIST_START_Y) / ITEM_H);
            int scroll_items = (int)(ctx.scroll_offset_y / ITEM_H);
            if (ctx.selected_index < scroll_items) {
                ctx.scroll_offset_y = ctx.selected_index * ITEM_H;
            } else if (ctx.selected_index >= scroll_items + visible) {
                ctx.scroll_offset_y = (ctx.selected_index - visible + 1) * ITEM_H;
            }
            float max_scroll = ((int)ctx.playing_tracks.size() - visible) * ITEM_H;
            if (ctx.scroll_offset_y < 0.0f) ctx.scroll_offset_y = 0.0f;
            if (max_scroll <= 0.0f) ctx.scroll_offset_y = 0.0f;
            else if (ctx.scroll_offset_y > max_scroll) ctx.scroll_offset_y = max_scroll;
        }
        if (kRepeat & KEY_DRIGHT) {
            ctx.scroll_x += 50;
        }
        if (kRepeat & KEY_DLEFT) {
            ctx.scroll_x -= 50;
            if (ctx.scroll_x < 0) ctx.scroll_x = 0;
        }
    }
    LightLock_Unlock(&ctx.lock);

    // A button -> play selected track
    if (kDown & KEY_A) {
        if (!ctx.playing_tracks.empty() &&
            ctx.selected_index >= 0 &&
            ctx.selected_index < (int)ctx.playing_tracks.size()) {
            return "play_selected_track";
        }
    }

    return "";
}

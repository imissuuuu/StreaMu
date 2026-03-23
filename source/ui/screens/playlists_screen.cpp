#include "playlists_screen.h"
#include "../track_list_helpers.h"
#include "../ui_manager.h"
#include "../ui_constants.h"

void PlaylistsScreen::on_enter(AppContext& ctx) {
    ctx.current_state  = STATE_PLAYLISTS;
    ctx.selected_index = 0;
    ctx.scroll_offset_y = 0.0f;
    ctx.touch_state.reset();
}

std::string PlaylistsScreen::update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
    LightLock_Lock(&ctx.lock);
    int playlist_count = (int)ctx.playlists.size();
    LightLock_Unlock(&ctx.lock);

    // --- Touch handling ---
    touchPosition touch;
    hidTouchRead(&touch);

    if (kDown & KEY_TOUCH) {
        ctx.touch_state.begin(touch.px, touch.py);
    }
    if ((kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        int delta_y = ctx.touch_state.update(touch.px, touch.py);
        if (ctx.touch_state.is_dragging) {
            ctx.scroll_offset_y += delta_y;
            float max_scroll = (playlist_count - BTM_MAX_VISIBLE_2ROW) * BTM_ITEM_HEIGHT_2ROW;
            if (max_scroll < 0) max_scroll = 0;
            if (ctx.scroll_offset_y < 0) ctx.scroll_offset_y = 0;
            if (ctx.scroll_offset_y > max_scroll) ctx.scroll_offset_y = max_scroll;
        }
    }
    if (!(kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        bool was_tap = ctx.touch_state.end();
        if (was_tap) {
            int tap_x = ctx.touch_state.current_x;
            int tap_y = ctx.touch_state.current_y;

            // Menu button hit test
            MenuBtnRect btn = get_menu_btn_rect(ctx.config);
            if (tap_x >= btn.x && tap_x <= btn.x + btn.w &&
                tap_y >= btn.y && tap_y <= btn.y + btn.h) {
                return "trigger_nav_menu";
            }

            // List item tap detection (must match draw_bottom start_idx calculation)
            int start_idx;
            if (ctx.scroll_offset_y > 0.0f) {
                start_idx = (int)(ctx.scroll_offset_y / BTM_ITEM_HEIGHT_2ROW);
            } else {
                start_idx = 0;
                if (ctx.selected_index > BTM_MAX_VISIBLE_2ROW - 2)
                    start_idx = ctx.selected_index - (BTM_MAX_VISIBLE_2ROW - 2);
            }
            float rel_y = tap_y - BTM_LIST_START_Y;
            if (rel_y >= 0) {
                int tapped_row = (int)(rel_y / BTM_ITEM_HEIGHT_2ROW);
                int tapped_index = start_idx + tapped_row;

                LightLock_Lock(&ctx.lock);
                if (tapped_index >= 0 && tapped_index < (int)ctx.playlists.size()) {
                    // Single tap to open
                    ctx.selected_index       = tapped_index;
                    ctx.selected_playlist_id = ctx.playlists[tapped_index].id;
                    ctx.g_tracks             = ctx.playlists[tapped_index].tracks;
                    LightLock_Unlock(&ctx.lock);
                    return "open_playlist_detail";
                }
                LightLock_Unlock(&ctx.lock);
            }
        }
    }

    // DPad navigation (resets scroll_offset_y)
    LightLock_Lock(&ctx.lock);
    if (!ctx.playlists.empty()) {
        if (kRepeat & KEY_DDOWN) {
            ctx.selected_index++;
            if (ctx.selected_index >= (int)ctx.playlists.size())
                ctx.selected_index = 0;
            ctx.scroll_offset_y = 0.0f;
        }
        if (kRepeat & KEY_DUP) {
            ctx.selected_index--;
            if (ctx.selected_index < 0)
                ctx.selected_index = (int)ctx.playlists.size() - 1;
            ctx.scroll_offset_y = 0.0f;
        }
        if (kDown & KEY_A) {
            ctx.selected_playlist_id = ctx.playlists[ctx.selected_index].id;
            ctx.g_tracks             = ctx.playlists[ctx.selected_index].tracks;
            LightLock_Unlock(&ctx.lock);
            return "open_playlist_detail";
        }
    }
    LightLock_Unlock(&ctx.lock);
    return "";
}

void PlaylistsScreen::draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
    // Top screen rendered by UIRenderer (common header)
}

void PlaylistsScreen::draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    C2D_Text text;

    int max_vis   = BTM_MAX_VISIBLE_2ROW;

    // Convert scroll_offset_y to start_idx
    int scroll_idx = (int)(ctx.scroll_offset_y / BTM_ITEM_HEIGHT_2ROW);
    int start_idx = scroll_idx;

    // When using DPad (scroll_offset_y==0), use traditional cursor follow
    if (ctx.scroll_offset_y == 0.0f) {
        start_idx = 0;
        if (ctx.selected_index > max_vis - 2)
            start_idx = ctx.selected_index - (max_vis - 2);
    }

    for (int i = start_idx;
         i < (int)ctx.playlists.size() && i < start_idx + max_vis; i++) {
        float y_pos = BTM_LIST_START_Y + (i - start_idx) * BTM_ITEM_HEIGHT_2ROW;

        if (i == ctx.selected_index) {
            C2D_DrawRectSolid(0, y_pos - 2, 0, 320,
                              BTM_ITEM_HEIGHT_2ROW, ctx.theme->accent);
        }

        std::string display_title = ctx.playlists[i].name;
        std::string meta_line =
            std::to_string(ctx.playlists[i].tracks.size()) + " tracks";

        u32 color = (i == ctx.selected_index) ? ctx.theme->accent_text
                                               : ctx.theme->text_body;
        C2D_TextParse(&text, ui_mgr.get_text_buf(), display_title.c_str());
        C2D_DrawText(&text, C2D_WithColor, BTM_MARGIN_X, y_pos, 0,
                     FONT_SM, FONT_SM, color);

        u32 meta_color = (i == ctx.selected_index) ? ctx.theme->accent_text
                                                    : ctx.theme->text_dim;
        C2D_TextParse(&text, ui_mgr.get_text_buf(), meta_line.c_str());
        C2D_DrawText(&text, C2D_WithColor,
                     BTM_MARGIN_X + 8, y_pos + BTM_META_OFFSET, 0,
                     FONT_XS, FONT_XS, meta_color);
    }

    if (ctx.playlists.empty()) {
        C2D_TextParse(&text, ui_mgr.get_text_buf(), "No playlists found.");
        C2D_DrawText(&text, C2D_WithColor,
                     BTM_MARGIN_X, BTM_LIST_START_Y + 12, 0,
                     FONT_SM, FONT_SM, ctx.theme->empty_text);
    }

    draw_menu_button(ctx, ui_mgr);
}

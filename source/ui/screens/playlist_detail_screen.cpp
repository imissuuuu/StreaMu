#include "playlist_detail_screen.h"
#include "../track_list_helpers.h"
#include "../ui_constants.h"

void PlaylistDetailScreen::on_enter(AppContext& ctx) {
    ctx.current_state  = STATE_PLAYLIST_DETAIL;
    ctx.selected_index = 0;  // 0 = MODE_BTN row
    ctx.scroll_x       = 0;
    ctx.scroll_offset_y = 0.0f;
    ctx.touch_state.reset();
    mode_btn_focus_ = 1; // Default: ORDER
    ctx.mode_btn_focus = mode_btn_focus_;

    // Insert virtual MODE_BTN entry at the top
    LightLock_Lock(&ctx.lock);
    // Skip if MODE_BTN already exists
    if (ctx.g_tracks.empty() || ctx.g_tracks[0].id != "MODE_BTN") {
        Track mode_btn;
        mode_btn.id    = "MODE_BTN";
        mode_btn.title = "";
        ctx.g_tracks.insert(ctx.g_tracks.begin(), mode_btn);
    }
    LightLock_Unlock(&ctx.lock);
}

std::string PlaylistDetailScreen::update(AppContext& ctx, u32 kDown,
                                         u32 kHeld, u32 kRepeat) {
    LightLock_Lock(&ctx.lock);
    int track_count = (int)ctx.g_tracks.size();
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
            float min_scroll = 0.0f;
            float max_scroll = std::max(0.0f, (float)(track_count - BTM_MAX_VISIBLE_2ROW) * BTM_ITEM_HEIGHT_2ROW);
            if (ctx.scroll_offset_y < min_scroll) ctx.scroll_offset_y = min_scroll;
            if (ctx.scroll_offset_y > max_scroll)  ctx.scroll_offset_y = max_scroll;
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

            // List item tap detection
            int btm_start;
            float y_base;
            if (ctx.scroll_offset_y != 0.0f) {
                int scroll_items; float y_shift;
                calc_scroll(ctx.scroll_offset_y, BTM_ITEM_HEIGHT_2ROW, scroll_items, y_shift);
                btm_start = scroll_items;
                if (btm_start < 0) btm_start = 0;
                y_base = BTM_LIST_START_Y - y_shift;
            } else {
                btm_start = 0;
                if (ctx.selected_index > BTM_MAX_VISIBLE_2ROW - 2)
                    btm_start = ctx.selected_index - (BTM_MAX_VISIBLE_2ROW - 2);
                y_base = BTM_LIST_START_Y;
            }

            float rel_y = (float)tap_y - y_base;
            if (rel_y >= 0) {
                int tapped_row = (int)(rel_y / BTM_ITEM_HEIGHT_2ROW);
                int tapped_index = btm_start + tapped_row;

                LightLock_Lock(&ctx.lock);
                if (tapped_index >= 0 && tapped_index < (int)ctx.g_tracks.size()) {
                    ctx.selected_index = tapped_index;
                    ctx.scroll_x = 0;
                    std::string sel_id = ctx.g_tracks[tapped_index].id;
                    LightLock_Unlock(&ctx.lock);

                    if (sel_id == "MODE_BTN") {
                        // MODE_BTN row tap: left half=SHUFFLE, right half=ORDER
                        if (tap_x < 160) {
                            return "start_shuffle_playback";
                        } else {
                            return "start_order_playback";
                        }
                    } else if (ctx.playing_id == sel_id) {
                        return "toggle_pause";
                    } else {
                        return "start_playback_from_playlist";
                    }
                }
                LightLock_Unlock(&ctx.lock);
            }
        }
    }

    // Left/right keys on MODE_BTN row -> switch focus
    if (ctx.selected_index == 0) {
        LightLock_Lock(&ctx.lock);
        bool is_mode_btn = !ctx.g_tracks.empty() && ctx.g_tracks[0].id == "MODE_BTN";
        LightLock_Unlock(&ctx.lock);
        if (is_mode_btn) {
            if (kRepeat & KEY_DLEFT) {
                mode_btn_focus_ = 0;
                ctx.mode_btn_focus = 0;
            }
            if (kRepeat & KEY_DRIGHT) {
                mode_btn_focus_ = 1;
                ctx.mode_btn_focus = 1;
            }
        }
    }

    // KEY_A: play track or toggle pause
    if (kDown & KEY_A) {
        LightLock_Lock(&ctx.lock);
        bool has_tracks    = !ctx.g_tracks.empty();
        std::string sel_id = has_tracks ? ctx.g_tracks[ctx.selected_index].id
                                        : "";
        LightLock_Unlock(&ctx.lock);

        if (has_tracks) {
            if (sel_id == "MODE_BTN") {
                return mode_btn_focus_ == 0 ? "start_shuffle_playback" : "start_order_playback";
            } else if (ctx.playing_id == sel_id) {
                return "toggle_pause";
            } else {
                return "start_playback_from_playlist";
            }
        }
    }

    // DPad navigation (resets scroll_offset_y)
    if (kRepeat & (KEY_DDOWN | KEY_DUP)) {
        ctx.scroll_offset_y = 0.0f;
    }
    navigate_track_list(ctx, kRepeat);

    return "";
}

void PlaylistDetailScreen::draw_top(const RenderContext& ctx,
                                    UIManager& ui_mgr) {
    // Top screen rendered by UIRenderer (common header)
}

void PlaylistDetailScreen::draw_bottom(const RenderContext& ctx,
                                       UIManager& ui_mgr) {
    draw_track_list_bottom(ctx, ui_mgr, false);
    draw_menu_button(ctx, ui_mgr);
}

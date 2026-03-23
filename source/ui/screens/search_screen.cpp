#include "search_screen.h"
#include "../track_list_helpers.h"
#include "../ui_constants.h"

void SearchScreen::on_enter(AppContext& ctx) {
    ctx.current_state = STATE_SEARCH;
    ctx.scroll_offset_y = 0.0f;
    ctx.touch_state.reset();
}

std::string SearchScreen::update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
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
                    if (ctx.playing_id == sel_id) {
                        return "toggle_pause";
                    } else {
                        return "start_playback";
                    }
                }
                LightLock_Unlock(&ctx.lock);
            }
        }
    }

    // KEY_A: play / toggle pause
    if (kDown & KEY_A) {
        LightLock_Lock(&ctx.lock);
        bool has_tracks   = !ctx.g_tracks.empty();
        std::string sel_id = has_tracks ? ctx.g_tracks[ctx.selected_index].id : "";
        LightLock_Unlock(&ctx.lock);

        if (has_tracks) {
            if (ctx.playing_id == sel_id) {
                return "toggle_pause";
            } else {
                return "start_playback";
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

void SearchScreen::draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
    // Top screen is rendered by UIRenderer (common header)
}

void SearchScreen::draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    draw_track_list_bottom(ctx, ui_mgr);
    draw_menu_button(ctx, ui_mgr);
}

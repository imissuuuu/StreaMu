#include "search_screen.h"
#include "../track_list_helpers.h"
#include "../ui_constants.h"
#include "../play_bar.h"

void SearchScreen::on_enter(AppContext& ctx) {
    ctx.current_state = STATE_SEARCH;
    ctx.scroll_offset_y = 0.0f;
    ctx.selected_index = 0;
    ctx.touch_state.reset();
}

std::string SearchScreen::update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
    touchPosition touch;
    hidTouchRead(&touch);

    if (kDown & KEY_TOUCH)
        ctx.touch_state.begin(touch.px, touch.py);

    if ((kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        int delta = ctx.touch_state.update(touch.px, touch.py);
        if (ctx.touch_state.is_dragging &&
            ctx.touch_state.start_y >= (int)BTM_LIST_START_Y &&
            ctx.touch_state.start_y < (int)PlayBar::BAR_Y &&
            !ctx.search_tracks.empty()) {
            ctx.scroll_offset_y += (float)delta;
            int visible = (int)((PlayBar::BAR_Y - BTM_LIST_START_Y) / BTM_ITEM_HEIGHT_2ROW);
            float max_scroll = ((int)ctx.search_tracks.size() - visible) * BTM_ITEM_HEIGHT_2ROW;
            if (ctx.scroll_offset_y < 0.0f) ctx.scroll_offset_y = 0.0f;
            if (max_scroll <= 0.0f) ctx.scroll_offset_y = 0.0f;
            else if (ctx.scroll_offset_y > max_scroll) ctx.scroll_offset_y = max_scroll;
        }
    }

    if (!(kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        int tx = ctx.touch_state.current_x;
        int ty = ctx.touch_state.current_y;
        bool from_seekbar = ctx.touch_state.start_y >= (int)PlayBar::BAR_Y
                         && ctx.touch_state.start_y < (int)(PlayBar::BAR_Y + PlayBar::SEEK_H);
        bool was_tap = ctx.touch_state.end();

        if (from_seekbar) {
            int total_secs = PlayBar::parse_duration_secs(ctx.playing_duration);
            if (total_secs > 0 && !ctx.playing_id.empty()) {
                float ratio = (float)tx / 320.0f;
                if (ratio < 0.03f) ratio = 0.0f;
                if (ratio > 0.97f) ratio = 1.0f;
                ctx.seek_target_seconds = (ratio >= 1.0f) ? total_secs - 1 : (int)(ratio * total_secs);
                return "seek";
            }
        } else if (was_tap) {
            // PlayBar area
            if (ty >= (int)PlayBar::BAR_Y) {
                // LOOP zone repurposed as nav menu button
                if (tx < (int)PlayBar::ZONE_1)
                    return "trigger_nav_menu";
                std::string bar_action = PlayBar::handle_touch(tx, ty, ctx);
                if (!bar_action.empty()) return bar_action;
            }

            // List item tap
            if (ty >= (int)BTM_LIST_START_Y && ty < (int)PlayBar::BAR_Y &&
                !ctx.search_tracks.empty()) {
                int tapped = (int)((ctx.scroll_offset_y + ty - BTM_LIST_START_Y)
                                   / BTM_ITEM_HEIGHT_2ROW);
                if (tapped >= 0 && tapped < (int)ctx.search_tracks.size()) {
                    ctx.selected_index = tapped;
                    ctx.scroll_x = 0;
                    if (ctx.playing_id == ctx.search_tracks[tapped].id)
                        return "toggle_pause";
                    return "start_playback";
                }
            }
        }
    }

    // D-pad navigation (mirrors PlayingScreen)
    LightLock_Lock(&ctx.lock);
    if (!ctx.search_tracks.empty()) {
        bool moved = false;
        if (kRepeat & KEY_DDOWN) {
            ctx.selected_index++;
            if (ctx.selected_index >= (int)ctx.search_tracks.size())
                ctx.selected_index = 0;
            ctx.scroll_x = 0;
            moved = true;
        }
        if (kRepeat & KEY_DUP) {
            ctx.selected_index--;
            if (ctx.selected_index < 0)
                ctx.selected_index = (int)ctx.search_tracks.size() - 1;
            ctx.scroll_x = 0;
            moved = true;
        }
        if (moved) {
            int visible = (int)((PlayBar::BAR_Y - BTM_LIST_START_Y) / BTM_ITEM_HEIGHT_2ROW);
            int scroll_items = (int)(ctx.scroll_offset_y / BTM_ITEM_HEIGHT_2ROW);
            if (ctx.selected_index < scroll_items)
                ctx.scroll_offset_y = ctx.selected_index * BTM_ITEM_HEIGHT_2ROW;
            else if (ctx.selected_index >= scroll_items + visible)
                ctx.scroll_offset_y = (ctx.selected_index - visible + 1) * BTM_ITEM_HEIGHT_2ROW;
            float max_scroll = ((int)ctx.search_tracks.size() - visible) * BTM_ITEM_HEIGHT_2ROW;
            if (ctx.scroll_offset_y < 0.0f) ctx.scroll_offset_y = 0.0f;
            if (max_scroll <= 0.0f) ctx.scroll_offset_y = 0.0f;
            else if (ctx.scroll_offset_y > max_scroll) ctx.scroll_offset_y = max_scroll;
        }
        if (kRepeat & KEY_DRIGHT) ctx.scroll_x += 50;
        if (kRepeat & KEY_DLEFT) {
            ctx.scroll_x -= 50;
            if (ctx.scroll_x < 0) ctx.scroll_x = 0;
        }
    }
    LightLock_Unlock(&ctx.lock);

    // KEY_A: play / toggle pause
    if (kDown & KEY_A) {
        LightLock_Lock(&ctx.lock);
        bool has = !ctx.search_tracks.empty() &&
                   ctx.selected_index < (int)ctx.search_tracks.size();
        std::string sel_id = has ? ctx.search_tracks[ctx.selected_index].id : "";
        LightLock_Unlock(&ctx.lock);
        if (has) {
            if (ctx.playing_id == sel_id) return "toggle_pause";
            return "start_playback";
        }
    }

    return "";
}

void SearchScreen::draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
    // Top screen is rendered by UIRenderer (common header)
}

void SearchScreen::draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    draw_playing_style_list(ctx.search_tracks, ctx, ui_mgr);
    PlayBar::draw(ctx, ui_mgr);

    // Replace LOOP zone with hamburger nav button
    {
        u32 bar_bg = (ctx.config.mode == THEME_DARK)
                     ? C2D_Color32(55, 55, 55, 255)
                     : C2D_Color32(230, 230, 230, 255);
        // Cover loop icon in control area (below seek bar)
        C2D_DrawRectSolid(0, PlayBar::BAR_Y + PlayBar::SEEK_H, 0,
                          (int)PlayBar::ZONE_1, PlayBar::BAR_H - PlayBar::SEEK_H, bar_bg);
        // Draw ☰ centered in the zone
        float cy = PlayBar::BAR_Y + PlayBar::SEEK_H + (PlayBar::BAR_H - PlayBar::SEEK_H) * 0.5f;
        float ix = PlayBar::ZONE_1 * 0.5f - 16.0f + 4.0f;  // center of zone - icon offset
        float iy = cy - 16.0f - 1.0f;
        u32 ic = ctx.theme->text_dim;
        C2D_DrawRectSolid(ix + 4, iy + 6,  0, 24, 4, ic);
        C2D_DrawRectSolid(ix + 4, iy + 14, 0, 24, 4, ic);
        C2D_DrawRectSolid(ix + 4, iy + 22, 0, 24, 4, ic);
    }
}

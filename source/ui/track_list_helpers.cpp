#include "track_list_helpers.h"
#include "ui_manager.h"
#include "ui_constants.h"

// ============================================================
// Shared helper: track list drawing for STATE_SEARCH / STATE_PLAYLIST_DETAIL
// ============================================================
void draw_track_list_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    C2D_Text text;
    int max_vis = BTM_MAX_VISIBLE_2ROW;

    // Scroll: prefer scroll_offset_y during touch scroll (sub-pixel), cursor follow for DPad
    int btm_start;
    float y_base = BTM_LIST_START_Y;
    if (ctx.scroll_offset_y != 0.0f) {
        int scroll_items; float y_shift;
        calc_scroll(ctx.scroll_offset_y, BTM_ITEM_HEIGHT_2ROW, scroll_items, y_shift);
        btm_start = scroll_items;
        if (btm_start < 0) btm_start = 0;
        y_base = BTM_LIST_START_Y - y_shift;
    } else {
        btm_start = 0;
        if (ctx.selected_index > max_vis - 2)
            btm_start = ctx.selected_index - (max_vis - 2);
    }

    int rendered = 0;
    for (int i = btm_start;
         i < (int)ctx.g_tracks.size() && rendered < max_vis + 1; i++) {

        float y_pos = y_base + rendered * BTM_ITEM_HEIGHT_2ROW;
        if (y_pos >= 240.0f) break;
        // Special rendering for MODE_BTN row
        if (ctx.g_tracks[i].id == "MODE_BTN" && ctx.current_state == STATE_PLAYLIST_DETAIL) {
            // Highlight only the focused side when selected
            if (i == ctx.selected_index) {
                if (ctx.mode_btn_focus == 0) {
                    C2D_DrawRectSolid(0, y_pos - 2, 0, 160,
                                      BTM_ITEM_HEIGHT_2ROW, ctx.theme->accent);
                } else {
                    C2D_DrawRectSolid(160, y_pos - 2, 0, 160,
                                      BTM_ITEM_HEIGHT_2ROW, ctx.theme->accent);
                }
            }
            // Center divider line
            C2D_DrawRectSolid(160, y_pos - 2, 0, 1, BTM_ITEM_HEIGHT_2ROW,
                              ctx.theme->text_dim & 0x80FFFFFF);

            float btn_text_y = y_pos + (BTM_ITEM_HEIGHT_2ROW - FONT_LG * 24.0f) / 2.0f - 2.0f;

            // SHUFFLE (left half, center=80)
            u32 shuf_color = (i == ctx.selected_index && ctx.mode_btn_focus == 0)
                             ? ctx.theme->accent_text : ctx.theme->text_body;
            C2D_TextParse(&text, ui_mgr.get_text_buf(), "SHUFFLE");
            float shuf_w = text.width * FONT_LG;
            C2D_DrawText(&text, C2D_WithColor, 80.0f - shuf_w / 2.0f, btn_text_y, 0,
                         FONT_LG, FONT_LG, shuf_color);

            // ORDER (right half, center=240)
            u32 order_color = (i == ctx.selected_index && ctx.mode_btn_focus == 1)
                              ? ctx.theme->accent_text : ctx.theme->text_body;
            C2D_TextParse(&text, ui_mgr.get_text_buf(), "ORDER");
            float order_w = text.width * FONT_LG;
            C2D_DrawText(&text, C2D_WithColor, 240.0f - order_w / 2.0f, btn_text_y, 0,
                         FONT_LG, FONT_LG, order_color);

            rendered++;
            continue;
        }

        bool is_playing = (ctx.g_tracks[i].id == ctx.playing_id &&
                           ctx.playing_id != "SEARCH_BTN");

        if (i == ctx.selected_index) {
            C2D_DrawRectSolid(0, y_pos - 2, 0, 320,
                              BTM_ITEM_HEIGHT_2ROW, ctx.theme->accent);
        } else if (is_playing) {
            C2D_DrawRectSolid(0, y_pos - 2, 0, 320,
                              BTM_ITEM_HEIGHT_2ROW, ctx.theme->playing_bg);
        }

        // Row 1: Title
        std::string display_title = ctx.g_tracks[i].title;
        if (is_playing) display_title = ">> " + display_title;

        u32   color  = (i == ctx.selected_index) ? ctx.theme->accent_text
                                                  : ctx.theme->text_body;
        C2D_TextParse(&text, ui_mgr.get_text_buf(), display_title.c_str());
        float draw_x = BTM_MARGIN_X;
        if (i == ctx.selected_index) {
            float text_w    = text.width * FONT_SM;
            float display_w = 320.0f - BTM_MARGIN_X * 2;
            float eff_scroll = (text_w > display_w)
                               ? std::min((float)ctx.scroll_x, text_w - display_w)
                               : 0.0f;
            draw_x = BTM_MARGIN_X - eff_scroll;
        }
        C2D_DrawText(&text, C2D_WithColor, draw_x, y_pos, 0,
                     FONT_SM, FONT_SM, color);

        // Row 2: Metadata
        {
            std::string meta;
            if (!ctx.g_tracks[i].duration.empty() &&
                ctx.g_tracks[i].duration != "?")
                meta += ctx.g_tracks[i].duration;
            if (!ctx.g_tracks[i].views.empty() &&
                ctx.g_tracks[i].views != "?") {
                if (!meta.empty()) meta += " \xC2\xB7 ";
                meta += ctx.g_tracks[i].views;
            }
            if (!ctx.g_tracks[i].upload_date.empty() &&
                ctx.g_tracks[i].upload_date != "?") {
                if (!meta.empty()) meta += " \xC2\xB7 ";
                meta += ctx.g_tracks[i].upload_date;
            }
            if (!meta.empty()) {
                u32 meta_color = (i == ctx.selected_index)
                                 ? ctx.theme->accent_text
                                 : ctx.theme->text_body;
                C2D_TextParse(&text, ui_mgr.get_text_buf(), meta.c_str());
                C2D_DrawText(&text, C2D_WithColor,
                             BTM_MARGIN_X + 8, y_pos + BTM_META_OFFSET, 0,
                             FONT_XS, FONT_XS, meta_color);
            }
        }
        rendered++;
    }

    bool effectively_empty = ctx.g_tracks.empty() ||
        (ctx.g_tracks.size() == 1 && ctx.g_tracks[0].id == "MODE_BTN");
    if (effectively_empty &&
        ctx.current_state == STATE_PLAYLIST_DETAIL) {
        C2D_TextParse(&text, ui_mgr.get_text_buf(),
                      "No tracks in this playlist.");
        C2D_DrawText(&text, C2D_WithColor,
                     BTM_MARGIN_X, BTM_LIST_START_Y + 12, 0,
                     FONT_SM, FONT_SM, ctx.theme->empty_text);
    }
}

// ============================================================
// draw_menu_button: Draw menu button on bottom screen (shared by all non-Home screens)
// ============================================================
void draw_menu_button(const RenderContext& ctx, UIManager& ui_mgr) {
    MenuBtnRect btn = get_menu_btn_rect(ctx.config);
    u32 bg = ctx.theme->text_dim & 0xC0FFFFFF;
    C2D_DrawRectSolid(btn.x, btn.y, 0, btn.w, btn.h, bg);
    // ☰ icon (16x16 grid scaled 2x → 32x32, centered in 40x30 button)
    u32 ic = ctx.theme->text_dim;
    float ix = btn.x + 4.0f;   // (40-32)/2 = 4
    float iy = btn.y - 1.0f;   // (30-32)/2 = -1
    C2D_DrawRectSolid(ix + 4, iy + 6,  0, 24, 4, ic);
    C2D_DrawRectSolid(ix + 4, iy + 14, 0, 24, 4, ic);
    C2D_DrawRectSolid(ix + 4, iy + 22, 0, 24, 4, ic);
}

// ============================================================
// Shared helper: DPad navigation for g_tracks list
// ============================================================
void navigate_track_list(AppContext& ctx, u32 kRepeat) {
    LightLock_Lock(&ctx.lock);
    if (!ctx.g_tracks.empty()) {
        if (kRepeat & KEY_DDOWN) {
            ctx.selected_index++;
            if (ctx.selected_index >= (int)ctx.g_tracks.size())
                ctx.selected_index = 0;
            ctx.scroll_x = 0;
        }
        if (kRepeat & KEY_DUP) {
            ctx.selected_index--;
            if (ctx.selected_index < 0)
                ctx.selected_index = (int)ctx.g_tracks.size() - 1;
            ctx.scroll_x = 0;
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
}

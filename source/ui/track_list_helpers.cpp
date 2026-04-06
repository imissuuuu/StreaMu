#include "track_list_helpers.h"
#include "ui_manager.h"
#include "ui_constants.h"
#include "play_bar.h"

// ============================================================
// Shared helper: track list drawing for STATE_SEARCH / STATE_PLAYLIST_DETAIL
// ============================================================
void draw_track_list_bottom(const RenderContext& ctx, UIManager& ui_mgr, bool show_views) {
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
            // PlayBar-style background for both halves
            u32 bar_bg = (ctx.config.mode == THEME_DARK)
                         ? C2D_Color32(55, 55, 55, 255)
                         : C2D_Color32(230, 230, 230, 255);
            C2D_DrawRectSolid(0, y_pos - 2, 0, 160, BTM_ITEM_HEIGHT_2ROW, bar_bg);
            C2D_DrawRectSolid(160, y_pos - 2, 0, 160, BTM_ITEM_HEIGHT_2ROW, bar_bg);

            // Highlight focused side when selected
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

            float btn_cy = y_pos - 2 + BTM_ITEM_HEIGHT_2ROW / 2.0f;

            // --- SHUFFLE icon (left half, center=80) ---
            // Original PlayBar 32x32 pixel art, no scaling
            {
                u32 sc = (i == ctx.selected_index && ctx.mode_btn_focus == 0)
                         ? ctx.theme->accent_text : ctx.theme->text_body;
                float x0 = 80.0f - 16;
                float y0 = btn_cy - 11;
                // Arrow tip top-right
                C2D_DrawRectSolid(x0+25, y0+0,  0, 1, 1, sc);
                C2D_DrawRectSolid(x0+25, y0+1,  0, 2, 1, sc);
                C2D_DrawRectSolid(x0+25, y0+2,  0, 3, 1, sc);
                // Top arms
                C2D_DrawRectSolid(x0+2,  y0+3,  0, 7, 1, sc); C2D_DrawRectSolid(x0+20, y0+3,  0, 9, 1, sc);
                C2D_DrawRectSolid(x0+2,  y0+4,  0, 8, 1, sc); C2D_DrawRectSolid(x0+19, y0+4,  0, 11, 1, sc);
                C2D_DrawRectSolid(x0+2,  y0+5,  0, 9, 1, sc); C2D_DrawRectSolid(x0+18, y0+5,  0, 12, 1, sc);
                C2D_DrawRectSolid(x0+2,  y0+6,  0, 10, 1, sc); C2D_DrawRectSolid(x0+17, y0+6,  0, 12, 1, sc);
                // Cross upper
                C2D_DrawRectSolid(x0+9,  y0+7,  0, 4, 1, sc); C2D_DrawRectSolid(x0+16, y0+7,  0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+7,  0, 3, 1, sc);
                C2D_DrawRectSolid(x0+10, y0+8,  0, 4, 1, sc); C2D_DrawRectSolid(x0+15, y0+8,  0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+8,  0, 2, 1, sc);
                C2D_DrawRectSolid(x0+11, y0+9,  0, 7, 1, sc); C2D_DrawRectSolid(x0+25, y0+9,  0, 1, 1, sc);
                // Center
                C2D_DrawRectSolid(x0+12, y0+10, 0, 5, 1, sc);
                C2D_DrawRectSolid(x0+12, y0+11, 0, 5, 1, sc);
                // Cross lower
                C2D_DrawRectSolid(x0+11, y0+12, 0, 7, 1, sc); C2D_DrawRectSolid(x0+25, y0+12, 0, 1, 1, sc);
                C2D_DrawRectSolid(x0+10, y0+13, 0, 4, 1, sc); C2D_DrawRectSolid(x0+15, y0+13, 0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+13, 0, 2, 1, sc);
                C2D_DrawRectSolid(x0+9,  y0+14, 0, 4, 1, sc); C2D_DrawRectSolid(x0+16, y0+14, 0, 4, 1, sc); C2D_DrawRectSolid(x0+25, y0+14, 0, 3, 1, sc);
                // Bottom arms
                C2D_DrawRectSolid(x0+2,  y0+15, 0, 10, 1, sc); C2D_DrawRectSolid(x0+17, y0+15, 0, 12, 1, sc);
                C2D_DrawRectSolid(x0+2,  y0+16, 0, 9, 1, sc); C2D_DrawRectSolid(x0+18, y0+16, 0, 12, 1, sc);
                C2D_DrawRectSolid(x0+2,  y0+17, 0, 8, 1, sc); C2D_DrawRectSolid(x0+19, y0+17, 0, 11, 1, sc);
                C2D_DrawRectSolid(x0+2,  y0+18, 0, 7, 1, sc); C2D_DrawRectSolid(x0+20, y0+18, 0, 9, 1, sc);
                // Arrow tip bottom-right
                C2D_DrawRectSolid(x0+25, y0+19, 0, 3, 1, sc);
                C2D_DrawRectSolid(x0+25, y0+20, 0, 2, 1, sc);
                C2D_DrawRectSolid(x0+25, y0+21, 0, 1, 1, sc);
            }

            // --- ORDER play icon (right half, center=240) ---
            // ▶ triangle, same size as PlayBar (half_h=12, 24px)
            {
                u32 oc = (i == ctx.selected_index && ctx.mode_btn_focus == 1)
                         ? ctx.theme->accent_text : ctx.theme->text_body;
                float px = 240.0f;
                int half_h = 12;
                int w = half_h * 2;
                for (int j = 0; j < w; j++) {
                    float h = half_h * (1.0f - (float)j / w);
                    if (h < 1) h = 1;
                    C2D_DrawRectSolid(px - half_h + j, btn_cy - h, 0, 1, h * 2, oc);
                }
            }

            rendered++;
            continue;
        }

        bool is_playing = (ctx.g_tracks[i].id == ctx.playing_id &&
                           ctx.playing_id != "SEARCH_BTN");

        if (i == ctx.selected_index) {
            C2D_DrawRectSolid(0, y_pos - 2, 0, 320,
                              BTM_ITEM_HEIGHT_2ROW, ctx.theme->accent);
            draw_selection_left_bar(y_pos - 2, BTM_ITEM_HEIGHT_2ROW, ctx.theme->accent);
        } else if (is_playing) {
            C2D_DrawRectSolid(0, y_pos - 2, 0, 320,
                              BTM_ITEM_HEIGHT_2ROW, ctx.theme->playing_bg);
        } else {
            draw_item_bg(0, y_pos - 2, 320, BTM_ITEM_HEIGHT_2ROW, ctx.theme->bg_bottom);
        }

        // Row 1: Title
        std::string display_title = ctx.g_tracks[i].title;
        if (is_playing) display_title = ">> " + display_title;

        u32   color  = (i == ctx.selected_index) ? ctx.theme->accent_text
                     : is_playing                ? ctx.theme->accent_text
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
            if (show_views && !ctx.g_tracks[i].views.empty() &&
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
                u32 meta_color = (i == ctx.selected_index || is_playing)
                                 ? ctx.theme->accent_text
                                 : ctx.theme->text_body;
                C2D_TextParse(&text, ui_mgr.get_text_buf(), meta.c_str());
                C2D_DrawText(&text, C2D_WithColor,
                             BTM_MARGIN_X + 8, y_pos + BTM_META_OFFSET, 0,
                             FONT_XS, FONT_XS, meta_color);
            }
        }
        // Dashed separator below item (skip for selected — accent fill acts as separator)
        if (i != ctx.selected_index) {
            u32 sep = ctx.theme->text_dim & 0x14FFFFFF;
            draw_dashed_line_h(0, y_pos - 2 + BTM_ITEM_HEIGHT_2ROW - 1, 320, sep);
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

// ============================================================
// PlayingScreen-style list: 30px items, y=BTM_LIST_START_Y..PlayBar::BAR_Y
// Used by PlayingScreen and SearchScreen
// ============================================================
void draw_playing_style_list(
    const std::vector<Track>& tracks,
    const RenderContext& ctx,
    UIManager& ui_mgr)
{
    if (tracks.empty()) return;
    C2D_Text text;
    C2D_TextBuf buf = ui_mgr.get_text_buf();

    constexpr float START_Y = BTM_LIST_START_Y;       // 8.0f
    constexpr float END_Y   = PlayBar::BAR_Y;          // 200.0f
    constexpr float H       = BTM_ITEM_HEIGHT_2ROW;    // 30.0f

    int scroll_items; float y_shift;
    calc_scroll(ctx.scroll_offset_y, H, scroll_items, y_shift);
    float y_base = START_Y - y_shift;
    int first    = scroll_items;
    int visible  = (int)((END_Y - START_Y) / H) + 1;

    for (int i = first; i < first + visible; ++i) {
        if (i < 0 || i >= (int)tracks.size()) continue;
        float y_item = y_base + (i - first) * H;
        if (y_item + H < START_Y || y_item >= END_Y) continue;

        const Track& tr  = tracks[i];
        bool is_playing  = (tr.id == ctx.playing_id);
        bool is_selected = (i == ctx.selected_index);

        if (is_selected) {
            C2D_DrawRectSolid(0, y_item, 0, 320, H, ctx.theme->accent);
            draw_selection_left_bar(y_item, H, ctx.theme->accent);
        } else if (is_playing) {
            C2D_DrawRectSolid(0, y_item, 0, 320, H, ctx.theme->playing_bg);
        } else {
            draw_item_bg(0, y_item, 320, H, ctx.theme->bg_bottom);
        }

        u32 title_color = (is_selected || is_playing)
                          ? ctx.theme->accent_text : ctx.theme->text_body;

        std::string display_title = is_playing ? (">> " + tr.title) : tr.title;
        C2D_TextParse(&text, buf, display_title.c_str());
        {
            float draw_x = BTM_MARGIN_X;
            if (is_selected) {
                float text_w    = text.width * FONT_SM;
                float display_w = 320.0f - BTM_MARGIN_X * 2;
                float eff_scroll = (text_w > display_w)
                    ? std::min((float)ctx.scroll_x, text_w - display_w) : 0.0f;
                draw_x = BTM_MARGIN_X - eff_scroll;
            }
            C2D_DrawText(&text, C2D_WithColor, draw_x, y_item, 0,
                         FONT_SM, FONT_SM, title_color);
        }
        if (!tr.duration.empty()) {
            C2D_TextParse(&text, buf, tr.duration.c_str());
            C2D_DrawText(&text, C2D_WithColor,
                         BTM_MARGIN_X + 8, y_item + BTM_META_OFFSET, 0,
                         FONT_XS, FONT_XS, title_color);
        }
        if (!is_selected) {
            u32 sep = ctx.theme->text_dim & 0x14FFFFFF;
            draw_dashed_line_h(0, y_item + H - 1, 320, sep);
        }
    }
}

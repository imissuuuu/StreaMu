#include "ui_renderer.h"
#include "ui_constants.h"
#include <time.h>

// === Loading Spinner ===
static const char* SPINNER_FRAMES[] = {
    "[/]", "[-]", "[\\]", "[|]"
};
static const int SPINNER_COUNT = 4;

static const char* get_spinner(time_t cached_time) {
    return SPINNER_FRAMES[cached_time % SPINNER_COUNT];
}

UIRenderer::UIRenderer(UIManager& manager, const ThemeColors& colors)
    : m_ui(manager), m_colors(colors) {}

UIRenderer::~UIRenderer() {}

void UIRenderer::draw_top_screen(const RenderContext& ctx) {
    C2D_Text text;

    // --- Wallpaper (drawn over solid background) ---
    if (m_wallpaper && m_wallpaper->is_loaded()) {
        m_wallpaper->draw_fullscreen();
    }

    // --- Status Bar Background (Item 7) ---
    C2D_DrawRectSolid(0, 0, 0, 400, 20.0f, m_colors.accent);
    
    // --- System Info Bar (icon-based) ---
    // Syscall cache: refresh every 2s only (per-frame calls can cause crashes)
    static u64 s_last_update_ms = 0;
    static time_t s_cached_time = 0;
    static struct tm s_cached_tm = {};
    static u8 s_cached_battery = 0;
    static u8 s_cached_wifi = 0;
    static u8 s_cached_charging = 0;

    u64 now_ms = osGetTime();
    if (s_last_update_ms == 0 || (now_ms - s_last_update_ms) >= 2000) {
        s_last_update_ms = now_ms;
        s_cached_time = time(NULL);
        struct tm* tmp = localtime(&s_cached_time);
        if (tmp) s_cached_tm = *tmp;
        PTMU_GetBatteryLevel(&s_cached_battery);
        s_cached_wifi = osGetWifiStrength();
        PTMU_GetBatteryChargeState(&s_cached_charging);
    }

    struct tm *tm = &s_cached_tm;
    u8 battery = s_cached_battery;
    u8 wifi = s_cached_wifi;
    u8 charging = s_cached_charging;

    u32 col      = m_colors.accent_text;
    u32 col_dim  = (col & 0x00FFFFFF) | (80u << 24);  // same RGB, alpha=80
    u32 col_bg   = m_colors.accent;                    // bar background (accent)

    // --- WiFi Icon: 3 ascending bars, bottom-aligned ---
    {
        float wx = TOP_MARGIN_X;
        float wy_base = 18.0f;  // bottom edge
        const int bar_w = 3;
        const int gap   = 1;
        const int bar_h[] = {6, 10, 14};

        if (wifi == 0) {
            // Disconnected/no signal: "x" text
            C2D_TextParse(&text, m_ui.get_text_buf(), "x");
            C2D_DrawText(&text, C2D_WithColor, wx + 2, TOP_SYS_Y, 0, FONT_SM, FONT_SM, col);
        } else {
            for (int i = 0; i < 3; i++) {
                u32 c = (i < wifi) ? col : col_dim;
                float bx = wx + i * (bar_w + gap);
                float by = wy_base - bar_h[i];
                C2D_DrawRectSolid(bx, by, 0, bar_w, bar_h[i], c);
            }
        }
    }

    // --- Battery Icon: outline + terminal + gauge segments ---
    {
        float bx = TOP_MARGIN_X + 18.0f;
        float by = 5.0f;
        float frame_w = 18.0f, frame_h = 10.0f;

        // Outer frame
        C2D_DrawRectSolid(bx, by, 0, frame_w, frame_h, col);
        // Inner cutout
        C2D_DrawRectSolid(bx + 1, by + 1, 0, frame_w - 2, frame_h - 2, col_bg);
        // Terminal nub
        C2D_DrawRectSolid(bx + frame_w, by + 3, 0, 2, 4, col);

        // Gauge (5 segments: 3px wide each, 1px padding)
        float seg_w = 3.0f;
        for (int i = 0; i < 5; i++) {
            if (i < battery) {
                float gx = bx + 1 + i * seg_w;
                C2D_DrawRectSolid(gx, by + 1, 0, seg_w, frame_h - 2, col);
            }
        }

        // Charging indicator
        if (charging) {
            C2D_TextParse(&text, m_ui.get_text_buf(), "+");
            C2D_DrawText(&text, C2D_WithColor, bx + frame_w + 4, TOP_SYS_Y, 0, FONT_SM, FONT_SM, col);
        }
    }

    // --- Clock ---
    {
        float clock_x = TOP_MARGIN_X + 50.0f;
        char time_str[8];
        if (tm) snprintf(time_str, sizeof(time_str), "%02d:%02d", tm->tm_hour, tm->tm_min);
        else    snprintf(time_str, sizeof(time_str), "--:--");
        C2D_TextParse(&text, m_ui.get_text_buf(), time_str);
        C2D_DrawText(&text, C2D_WithColor, clock_x, TOP_SYS_Y, 0, FONT_SM, FONT_SM, col);
    }

    // --- Version (right-aligned) ---
    C2D_TextParse(&text, m_ui.get_text_buf(), APP_VERSION);
    C2D_TextOptimize(&text);
    float ver_w = text.width * FONT_SM;
    C2D_DrawText(&text, C2D_WithColor, 400.0f - TOP_MARGIN_X - ver_w, TOP_SYS_Y, 0, FONT_SM, FONT_SM, m_colors.accent_text);

    // --- Server Status ---
    if (!ctx.is_server_connected) {
        C2D_TextParse(&text, m_ui.get_text_buf(), "[ SERVER OFFLINE ]");
        C2D_DrawText(&text, C2D_WithColor, TOP_OFFLINE_X, TOP_SYS_Y, 0, FONT_SM, FONT_SM, m_colors.warn);
    }

    // --- Section Title + Status (same line) ---
    // During popup, determine title from the underlying screen state
    AppState title_state = ctx.current_state;
    bool is_popup = (title_state == STATE_POPUP_PLAYLIST_ADD ||
                     title_state == STATE_POPUP_PLAYLIST_OPTIONS ||
                     title_state == STATE_POPUP_TRACK_OPTIONS ||
                     title_state == STATE_POPUP_TRACK_DETAILS ||
                     title_state == STATE_EXIT_CONFIRM ||
                     title_state == STATE_POPUP_NAV ||
                     title_state == STATE_POPUP_QA_ADD ||
                     title_state == STATE_POPUP_QA_REMOVE);
    if (is_popup) title_state = ctx.previous_state;

    std::string top_title = "StreaMu";
    if (title_state == STATE_HOME) top_title = "HOME";
    else if (title_state == STATE_SEARCH) top_title = "SEARCH";
    else if (title_state == STATE_PLAYLISTS) top_title = "PLAYLISTS";
    else if (title_state == STATE_PLAYLIST_DETAIL) top_title = "PLAYLIST DETAIL";
    else if (title_state == STATE_PLAYING_UI) top_title = "";
    else if (title_state == STATE_SETTINGS) top_title = "SETTINGS";

    // --- Title Badge (semi-transparent black, text-fitted) ---
    C2D_TextParse(&text, m_ui.get_text_buf(), top_title.c_str());
    if (top_title.size() > 0) {
        float tw = 0, th = 0;
        C2D_TextGetDimensions(&text, FONT_LG, FONT_LG, &tw, &th);
        float pad_x = 6.0f, pad_y = 2.0f;
        C2D_DrawRectSolid(0, 20.0f, 0,
                          pad_x + tw + pad_x, th + pad_y * 2,
                          C2D_Color32(30, 30, 30, 160));
    }
    C2D_DrawText(&text, C2D_WithColor, 6.0f, 20.0f + 2.0f, 0, FONT_LG, FONT_LG,
                 C2D_Color32(220, 220, 220, 255));

    // --- Status Badge (top-right, below banner) ---
    if (!ctx.g_status_msg.empty()) {
        std::string display_msg = ctx.g_status_msg;
        if (display_msg.find("Loading") != std::string::npos ||
            display_msg.find("Searching") != std::string::npos ||
            display_msg.find("Connecting") != std::string::npos ||
            display_msg.find("Downloading") != std::string::npos) {
            display_msg = std::string(get_spinner(s_cached_time)) + " " + display_msg;
        }
        C2D_TextParse(&text, m_ui.get_text_buf(), display_msg.c_str());
        float msg_w = 0, msg_h = 0;
        C2D_TextGetDimensions(&text, FONT_SM, FONT_SM, &msg_w, &msg_h);

        float pad_x = 6.0f;
        float pad_y = 2.0f;
        float badge_w = msg_w + pad_x * 2;
        float badge_h = msg_h + pad_y * 2;
        float badge_x = 400.0f - badge_w;
        float badge_y = 20.0f;

        C2D_DrawRectSolid(badge_x, badge_y, 0, badge_w, badge_h,
                          C2D_Color32(30, 30, 30, 160));
        C2D_DrawText(&text, C2D_WithColor,
                     badge_x + pad_x, badge_y + pad_y, 0,
                     FONT_SM, FONT_SM,
                     C2D_Color32(220, 220, 220, 255));
    }

    // --- Thumbnail (all screens except Settings) ---
    if (ctx.current_state != STATE_SETTINGS && m_thumbnail && m_thumbnail->is_loaded()) {
        m_thumbnail->draw_at(8.0f, 56.0f, 128.0f);
    }

    // --- Now Playing Title (with play/pause icon) ---
    if (!ctx.playing_title_lines.empty()) {
        // Fixed-size window (play bar colors, top area reserved for thumbnail)
        u32 win_bg = (ctx.config.mode == THEME_DARK) ? C2D_Color32(55, 55, 55, 0xD0)
                                                     : C2D_Color32(230, 230, 230, 0xD0);
        C2D_DrawRectSolid(0, TOP_NOW_PLAYING_WIN_Y, 0,
                          TOP_NOW_PLAYING_WIN_W, TOP_NOW_PLAYING_WIN_H,
                          win_bg);

        float draw_y = TOP_NOW_PLAYING_WIN_Y + TOP_NOW_PLAYING_PAD;

        // Line 1+: track title (no icon, prevents misalignment)
        for (const auto& line : ctx.playing_title_lines) {
            C2D_TextParse(&text, m_ui.get_text_buf(), line.c_str());
            C2D_DrawText(&text, C2D_WithColor, TOP_MARGIN_X, draw_y, 0, FONT_SM, FONT_SM, m_colors.text_body);
            draw_y += TOP_PLAYING_LINE;
        }

        // Last line: icon + playlist name + metadata
        // Draw icon and text separately at fixed X to prevent shift on toggle
        if (!ctx.active_playlist_name.empty() || !ctx.playing_meta.empty()) {
            // Icon: ">" (playing) / square (stopped)
            const char* icon_str = ctx.is_paused ? "\xe2\x96\xa0" : ">";
            C2D_TextParse(&text, m_ui.get_text_buf(), icon_str);
            C2D_DrawText(&text, C2D_WithColor, TOP_MARGIN_X, draw_y, 0, FONT_SM, FONT_SM, m_colors.text_body);

            // Text drawn at fixed X position (independent of icon width)
            std::string info = ctx.active_playlist_name;
            if (!ctx.playing_meta.empty()) {
                if (!info.empty()) info += "  ";
                info += ctx.playing_meta;
            }
            C2D_TextParse(&text, m_ui.get_text_buf(), info.c_str());
            C2D_DrawText(&text, C2D_WithColor, TOP_MARGIN_X + 12.0f, draw_y, 0, FONT_SM, FONT_SM, m_colors.text_body);
            draw_y += TOP_PLAYING_LINE;
        }
    }
    
}


void UIRenderer::draw_popup_overlay(const RenderContext& ctx) {
    bool is_popup = (ctx.current_state == STATE_POPUP_PLAYLIST_ADD ||
                     ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS ||
                     ctx.current_state == STATE_POPUP_TRACK_OPTIONS ||
                     ctx.current_state == STATE_POPUP_TRACK_DETAILS ||
                     ctx.current_state == STATE_EXIT_CONFIRM ||
                     ctx.current_state == STATE_POPUP_NAV ||
                     ctx.current_state == STATE_POPUP_QA_ADD ||
                     ctx.current_state == STATE_POPUP_QA_REMOVE);

    if (!is_popup) return;

    C2D_Text text;
    C2D_DrawRectSolid(0, 0, 0, 320, 240, m_colors.overlay);
    
    std::vector<std::string> options;
    std::string popup_title = "";
    if (ctx.current_state == STATE_POPUP_PLAYLIST_ADD) {
        popup_title = "Add to Playlist...";
        options.push_back("Create New");
        for (const auto& pl : ctx.playlists) options.push_back(pl.name);
    } else if (ctx.current_state == STATE_POPUP_PLAYLIST_OPTIONS) {
        popup_title = "Playlist Options";
        options.push_back("Edit Title");
        // Quick Access: "Remove" if registered, "Add" if slots available, "Full" otherwise
        {
            bool in_qa = false;
            int qa_count = 0;
            if (!ctx.config.quick_access_ids.empty()) {
                size_t start = 0;
                while (start < ctx.config.quick_access_ids.size()) {
                    size_t pos = ctx.config.quick_access_ids.find(',', start);
                    std::string id = (pos == std::string::npos)
                                     ? ctx.config.quick_access_ids.substr(start)
                                     : ctx.config.quick_access_ids.substr(start, pos - start);
                    if (!id.empty()) {
                        qa_count++;
                        if (id == ctx.selected_playlist_id) in_qa = true;
                    }
                    if (pos == std::string::npos) break;
                    start = pos + 1;
                }
            }
            if (in_qa) {
                options.push_back("Remove from Quick Access");
            } else if (qa_count < 4) {
                options.push_back("Add to Quick Access");
            } else {
                options.push_back("Quick Access is full");
            }
        }
        options.push_back("Delete (Cannot Undo)");
    } else if (ctx.current_state == STATE_POPUP_TRACK_OPTIONS) {
        popup_title = "Track Options";
        options.push_back("Show Details");
        if (ctx.previous_state == STATE_PLAYLIST_DETAIL || ctx.previous_state == STATE_PLAYING_UI) {
            options.push_back("Rename");
            options.push_back("Remove from Playlist");
        }
        options.push_back("Add to Playlist");
    } else if (ctx.current_state == STATE_POPUP_TRACK_DETAILS) {
        popup_title = "Track Details";
        // Look up track by selected_track_id (shared across screens)
        const Track* found_track = nullptr;
        for (const auto& t : ctx.playing_tracks) {
            if (t.id == ctx.selected_track_id) { found_track = &t; break; }
        }
        if (!found_track) {
            for (const auto& t : ctx.search_tracks) {
                if (t.id == ctx.selected_track_id) { found_track = &t; break; }
            }
        }
        if (!found_track) {
            for (const auto& t : ctx.g_tracks) {
                if (t.id == ctx.selected_track_id) { found_track = &t; break; }
            }
        }
        if (found_track) {
            const Track& t = *found_track;
            std::string ch = t.uploader.length() > 30 ? t.uploader.substr(0, 30) + "..." : t.uploader;
            options.push_back("Channel: " + ch);
            if (ctx.previous_state == STATE_SEARCH)
                options.push_back("Views: " + t.views);
            if (!t.upload_date.empty() && t.upload_date != "?") {
                options.push_back("Date: " + t.upload_date);
            }
            options.push_back("Time: " + t.duration);
        }
        options.push_back("Close");
    } else if (ctx.current_state == STATE_EXIT_CONFIRM) {
        popup_title = "Exit App?";
        options.push_back("No (Back)");
        options.push_back("Yes (Exit)");
    } else if (ctx.current_state == STATE_POPUP_NAV) {
        popup_title = "Navigation";
        options.push_back("Home");
        options.push_back("Search");
        options.push_back("Playlists");
        options.push_back("Settings");
    } else if (ctx.current_state == STATE_POPUP_QA_ADD) {
        popup_title = "Add to Quick Access";
        // Filter out already-registered playlists
        std::vector<std::string> qa_ids;
        {
            const std::string& csv = ctx.config.quick_access_ids;
            size_t s = 0;
            while (s < csv.size()) {
                size_t p = csv.find(',', s);
                std::string id = (p == std::string::npos) ? csv.substr(s) : csv.substr(s, p - s);
                if (!id.empty()) qa_ids.push_back(id);
                if (p == std::string::npos) break;
                s = p + 1;
            }
        }
        for (const auto& pl : ctx.playlists) {
            bool already = false;
            for (const auto& qid : qa_ids) {
                if (qid == pl.id) { already = true; break; }
            }
            if (!already) options.push_back(pl.name);
        }
    } else if (ctx.current_state == STATE_POPUP_QA_REMOVE) {
        popup_title = "Quick Access";
        options.push_back("Remove from Quick Access");
        options.push_back("Cancel");
    }
    
    int box_h = (int)(POPUP_HEADER_H + 8 + options.size() * POPUP_ITEM_H);
    if (box_h > POPUP_MAX_H) box_h = POPUP_MAX_H;
    int box_y = (240 - box_h) / 2;
    
    // Popup Background
    C2D_DrawRectSolid(POPUP_MARGIN_X, box_y, 0, POPUP_WIDTH, box_h, m_colors.popup_bg);
    // Popup Header
    C2D_DrawRectSolid(POPUP_MARGIN_X, box_y, 0, POPUP_WIDTH, POPUP_HEADER_H, m_colors.popup_header);
    
    C2D_TextParse(&text, m_ui.get_text_buf(), popup_title.c_str());
    C2D_DrawText(&text, C2D_WithColor, POPUP_MARGIN_X + 4, box_y + 6, 0, FONT_SM, FONT_SM, m_colors.popup_title);
    
    int start_idx = 0;
    int max_items = (int)((POPUP_MAX_H - POPUP_HEADER_H - 8) / POPUP_ITEM_H);
    if (ctx.popup_selected_index > max_items / 2) start_idx = ctx.popup_selected_index - max_items / 2;
    if (start_idx + max_items > (int)options.size()) start_idx = (int)options.size() - max_items;
    if (start_idx < 0) start_idx = 0;
    
    for (int i = start_idx; i < (int)options.size() && i < start_idx + max_items; i++) {
        float y_pos = box_y + POPUP_HEADER_H + 4 + (i - start_idx) * POPUP_ITEM_H;
        if (i == ctx.popup_selected_index) {
            C2D_DrawRectSolid(POPUP_MARGIN_X + 2, y_pos - 2, 0, POPUP_WIDTH - 4, POPUP_ITEM_H, m_colors.accent);
        }
        bool is_grayed = false;
        if (ctx.current_state == STATE_POPUP_NAV) {
            if (i == 2) is_grayed = (ctx.previous_state == STATE_PLAYLISTS);
            if (i == 3) is_grayed = (ctx.previous_state == STATE_SETTINGS);
        }
        u32 color = (i == ctx.popup_selected_index) ? m_colors.accent_text
                  : (is_grayed ? m_colors.text_dim : m_colors.popup_text);
        C2D_TextParse(&text, m_ui.get_text_buf(), options[i].c_str());
        C2D_DrawText(&text, C2D_WithColor, POPUP_INNER_X, y_pos, 0, FONT_SM, FONT_SM, color);
    }
}

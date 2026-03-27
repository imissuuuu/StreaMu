#include "home_screen.h"
#include "../ui_constants.h"
#include "../play_bar.h"

// --- Color helpers (Phase 7-A: gradient, bevel, press feedback) ---

static inline u8 col_r(u32 c) { return c & 0xFF; }
static inline u8 col_g(u32 c) { return (c >> 8) & 0xFF; }
static inline u8 col_b(u32 c) { return (c >> 16) & 0xFF; }

static inline u32 darken_color(u32 c, float factor) {
    return C2D_Color32((u8)(col_r(c) * factor),
                       (u8)(col_g(c) * factor),
                       (u8)(col_b(c) * factor), 255);
}

static void draw_gradient(float x, float y, float w, float h, u32 top_col, u32 bot_col) {
    int r0 = col_r(top_col), g0 = col_g(top_col), b0 = col_b(top_col);
    int r1 = col_r(bot_col), g1 = col_g(bot_col), b1 = col_b(bot_col);
    int ih = (int)h;
    for (int iy = 0; iy < ih; iy += GRAD_STEP) {
        float t = (float)iy / (float)ih;
        u8 r = (u8)(r0 + (int)((r1 - r0) * t));
        u8 g = (u8)(g0 + (int)((g1 - g0) * t));
        u8 b = (u8)(b0 + (int)((b1 - b0) * t));
        float sh = (iy + GRAD_STEP <= ih) ? (float)GRAD_STEP : h - (float)iy;
        C2D_DrawRectSolid(x, y + (float)iy, 0, w, sh, C2D_Color32(r, g, b, 255));
    }
}

static void draw_bevel(float x, float y, float w, float h, bool pressed) {
    u32 hi = pressed ? C2D_Color32(0, 0, 0, 64)   : C2D_Color32(255, 255, 255, 64);
    u32 lo = pressed ? C2D_Color32(255, 255, 255, 38) : C2D_Color32(0, 0, 0, 64);
    C2D_DrawRectSolid(x, y, 0, w, BEVEL_W, hi);              // top
    C2D_DrawRectSolid(x, y, 0, BEVEL_W, h, hi);              // left
    C2D_DrawRectSolid(x, y + h - BEVEL_W, 0, w, BEVEL_W, lo); // bottom
    C2D_DrawRectSolid(x + w - BEVEL_W, y, 0, BEVEL_W, h, lo); // right
}

static void draw_press_overlay(float x, float y, float w, float h, u32 inset_col) {
    C2D_DrawRectSolid(x, y, 0, w, h, C2D_Color32(0, 0, 0, 51));
    C2D_DrawRectSolid(x, y, 0, w, BEVEL_W, inset_col);
    C2D_DrawRectSolid(x, y, 0, BEVEL_W, h, inset_col);
    C2D_DrawRectSolid(x, y + h - BEVEL_W, 0, w, BEVEL_W, inset_col);
    C2D_DrawRectSolid(x + w - BEVEL_W, y, 0, BEVEL_W, h, inset_col);
}

// --- QA ID helpers ---

std::vector<std::string> HomeScreen::parse_qa_ids(const std::string& csv) {
    std::vector<std::string> ids;
    if (csv.empty()) return ids;
    size_t start = 0;
    while (start < csv.size()) {
        size_t pos = csv.find(',', start);
        std::string id = (pos == std::string::npos)
                         ? csv.substr(start)
                         : csv.substr(start, pos - start);
        if (!id.empty()) ids.push_back(id);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return ids;
}

// --- rebuild_qa_slots ---

void HomeScreen::rebuild_qa_slots(const AppContext& ctx) {
    qa_slots_.clear();
    auto qa_ids = parse_qa_ids(ctx.config.quick_access_ids);
    for (const auto& qid : qa_ids) {
        std::string name = qid;
        for (const auto& pl : ctx.playlists) {
            if (pl.id == qid) { name = pl.name; break; }
        }
        qa_slots_.push_back({name, qid});
    }
    if ((int)qa_ids.size() < 4) {
        qa_slots_.push_back({"+ Add", ""});
    }
}

// --- tile geometry ---

HomeScreen::Rect HomeScreen::get_tile_rect(TileId id) const {
    switch (id) {
    case TILE_QA:        return {HOME_LEFT_X,  HOME_AREA_TOP, HOME_COL_W, HOME_QA_H};
    case TILE_PLAYING:   return {HOME_LEFT_X,  HOME_QA_H,     HOME_COL_W, HOME_AREA_BOTTOM - HOME_QA_H};
    case TILE_SEARCH:    return {HOME_RIGHT_X, HOME_AREA_TOP,             HOME_COL_W, HOME_TILE_H};
    case TILE_PLAYLISTS: return {HOME_RIGHT_X, HOME_TILE_H,               HOME_COL_W, HOME_TILE_H};
    case TILE_SETTINGS:  return {HOME_RIGHT_X, HOME_TILE_H * 2.0f,        HOME_COL_W, HOME_AREA_BOTTOM - HOME_TILE_H * 2.0f};
    default:             return {0, 0, 0, 0};
    }
}

HomeScreen::TileId HomeScreen::get_selected_tile() const {
    if (sel_col_ == 0) {
        return (sel_row_ == 0) ? TILE_QA : TILE_PLAYING;
    } else {
        if (sel_row_ == 0) return TILE_SEARCH;
        if (sel_row_ == 1) return TILE_PLAYLISTS;
        return TILE_SETTINGS;
    }
}

bool HomeScreen::is_playing_visible(const RenderContext& ctx) const {
    return !ctx.active_playlist_id.empty()
        || !ctx.playing_id.empty()
        || show_search_result_;
}

// --- on_enter ---

void HomeScreen::on_enter(AppContext& ctx) {
    ctx.current_state = STATE_HOME;
    rebuild_qa_slots(ctx);
    sel_col_ = 1;
    sel_row_ = 0;
    qa_sel_ = 0;
    qa_focused_ = false;
}

// --- draw ---

void HomeScreen::draw_top(const RenderContext& ctx, UIManager& ui_mgr) {
    (void)ctx; (void)ui_mgr;
}

void HomeScreen::draw_bottom(const RenderContext& ctx, UIManager& ui_mgr) {
    C2D_TextBuf buf = ui_mgr.get_text_buf();
    C2D_Text text;

    TileId selected = get_selected_tile();
    u32 accent = ctx.theme->accent;
    u32 accent_dark = darken_color(accent, 0.80f);
    u32 inset_col = darken_color(accent, 0.50f);  // Darkened accent for inset border

    // --- Draw tile with gradient + bevel + press feedback ---
    auto draw_tile = [&](TileId id, const char* label) {
        Rect r = get_tile_rect(id);
        bool is_sel = (id == selected && !qa_focused_);
        bool is_pressed = (id == pressed_tile_);

        // Gradient background
        draw_gradient(r.x, r.y, r.w, r.h, accent, accent_dark);

        // Bevel (inverted when pressed)
        draw_bevel(r.x, r.y, r.w, r.h, is_pressed);

        // Press overlay
        if (is_pressed) {
            draw_press_overlay(r.x, r.y, r.w, r.h, inset_col);
        }

        // Label (with ▶ cursor space reserved)
        u32 col = ctx.theme->accent_text;
        float font = HOME_TILE_FONT;
        float ty = r.y + (r.h - font * 24.0f) / 2.0f;
        float tx = r.x + HOME_TILE_PAD;
        if (is_pressed) { tx += PRESS_TEXT_SHIFT; ty += PRESS_TEXT_SHIFT; }

        // ▶ cursor for D-pad selection
        if (is_sel && !is_pressed) {
            C2D_TextParse(&text, buf, ">");
            C2D_DrawText(&text, C2D_WithColor, tx, ty, 0, font, font, col);
        }
        C2D_TextParse(&text, buf, label);
        C2D_DrawText(&text, C2D_WithColor, tx + HOME_CURSOR_W, ty, 0, font, font, col);
    };

    // Right column tiles
    draw_tile(TILE_SEARCH, "Search");
    draw_tile(TILE_PLAYLISTS, "Playlists");
    draw_tile(TILE_SETTINGS, "Settings");

    // Left column: Playing tile
    if (is_playing_visible(ctx)) {
        const char* playing_label = !ctx.active_playlist_id.empty()
                                    ? "Playing"
                                    : "Search Result";
        draw_tile(TILE_PLAYING, playing_label);
    }

    // Left column: Quick Access tile
    {
        Rect r = get_tile_rect(TILE_QA);
        bool qa_tile_sel = (selected == TILE_QA && !qa_focused_);
        bool is_pressed = (TILE_QA == pressed_tile_);

        // Gradient background
        draw_gradient(r.x, r.y, r.w, r.h, accent, accent_dark);

        // Bevel
        draw_bevel(r.x, r.y, r.w, r.h, is_pressed);

        // Press overlay
        if (is_pressed) {
            draw_press_overlay(r.x, r.y, r.w, r.h, inset_col);
        }

        float press_off = is_pressed ? PRESS_TEXT_SHIFT : 0.0f;

        // QA header (with ▶ cursor when tile-selected)
        u32 hdr_col = ctx.theme->accent_text;
        float hdr_x = r.x + HOME_TILE_PAD + press_off;
        float hdr_y = r.y + HOME_TILE_PAD + press_off;
        if (qa_tile_sel && !is_pressed) {
            C2D_TextParse(&text, buf, ">");
            C2D_DrawText(&text, C2D_WithColor,
                         hdr_x, hdr_y,
                         0, HOME_QA_HEADER_FONT, HOME_QA_HEADER_FONT, hdr_col);
        }
        C2D_TextParse(&text, buf, "Quick Access");
        C2D_DrawText(&text, C2D_WithColor,
                     hdr_x + HOME_CURSOR_W, hdr_y,
                     0, HOME_QA_HEADER_FONT, HOME_QA_HEADER_FONT, hdr_col);

        // QA slots
        float slot_y = r.y + HOME_TILE_PAD + HOME_QA_HEADER_FONT * 24.0f + 4.0f;
        for (int i = 0; i < (int)qa_slots_.size(); ++i) {
            bool slot_sel = (qa_focused_ && selected == TILE_QA && i == qa_sel_);

            u32 slot_col = ctx.theme->accent_text;
            float text_y = slot_y + (HOME_QA_SLOT_H - HOME_QA_SLOT_FONT * 24.0f) / 2.0f;
            float slot_x = r.x + HOME_TILE_PAD + HOME_CURSOR_W + press_off;

            // ▶ cursor for selected QA slot
            if (slot_sel) {
                C2D_TextParse(&text, buf, ">");
                C2D_DrawText(&text, C2D_WithColor,
                             r.x + HOME_TILE_PAD + press_off, text_y + press_off,
                             0, HOME_QA_SLOT_FONT, HOME_QA_SLOT_FONT, slot_col);
            }

            C2D_TextParse(&text, buf, qa_slots_[i].label.c_str());
            C2D_DrawText(&text, C2D_WithColor,
                         slot_x, text_y + press_off,
                         0, HOME_QA_SLOT_FONT, HOME_QA_SLOT_FONT, slot_col);

            slot_y += HOME_QA_SLOT_H;
        }
    }

    PlayBar::draw(ctx, ui_mgr);
}

// --- update ---

std::string HomeScreen::update(AppContext& ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
    // Detect search track playback -> manage show_search_result_ flag
    if (!ctx.active_playlist_id.empty()) {
        show_search_result_ = false;
    } else if (!ctx.playing_id.empty()) {
        show_search_result_ = true;
    }

    // Rebuild QA slots on re-enter
    if (qa_slots_.empty()) {
        rebuild_qa_slots(ctx);
    }

    // --- Touch ---
    touchPosition touch;
    hidTouchRead(&touch);

    if (kDown & KEY_TOUCH) {
        ctx.touch_state.begin(touch.px, touch.py);
        // Hit test for press feedback
        pressed_tile_ = -1;
        int px = touch.px, py = touch.py;
        if (py < (int)PlayBar::BAR_Y) {
            for (int id = 0; id < TILE_COUNT; ++id) {
                TileId tid = static_cast<TileId>(id);
                if (tid == TILE_PLAYING && !is_playing_visible(ctx)) continue;
                Rect r = get_tile_rect(tid);
                if (px >= (int)r.x && px < (int)(r.x + r.w) &&
                    py >= (int)r.y && py < (int)(r.y + r.h)) {
                    pressed_tile_ = (int)tid;
                    break;
                }
            }
        }
    }
    if ((kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        ctx.touch_state.update(touch.px, touch.py);
    }
    if (!(kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
        pressed_tile_ = -1;
        bool was_tap = ctx.touch_state.end();
        if (was_tap) {
            int tx = ctx.touch_state.current_x;
            int ty = ctx.touch_state.current_y;

            // PlayBar touch
            if (ty >= (int)PlayBar::BAR_Y) {
                std::string bar_action = PlayBar::handle_touch(tx, ty, ctx);
                if (!bar_action.empty()) return bar_action;
            } else {
                // Hit test tiles
                for (int id = 0; id < TILE_COUNT; ++id) {
                    TileId tid = static_cast<TileId>(id);

                    // Skip invisible Playing tile
                    if (tid == TILE_PLAYING && !is_playing_visible(ctx)) continue;

                    Rect r = get_tile_rect(tid);
                    if (tx >= (int)r.x && tx < (int)(r.x + r.w) &&
                        ty >= (int)r.y && ty < (int)(r.y + r.h)) {

                        if (tid == TILE_QA) {
                            // Determine which QA slot was tapped
                            Rect qr = get_tile_rect(TILE_QA);
                            float slot_start = qr.y + HOME_TILE_PAD + HOME_QA_HEADER_FONT * 24.0f + 4.0f;
                            int slot_idx = (int)((ty - slot_start) / HOME_QA_SLOT_H);
                            if (slot_idx >= 0 && slot_idx < (int)qa_slots_.size()) {
                                qa_sel_ = slot_idx;
                                qa_focused_ = true;
                                sel_col_ = 0;
                                sel_row_ = 0;
                                return get_tile_action(TILE_QA, ctx);
                            }
                        } else {
                            // Update selection state
                            if (tid == TILE_SEARCH)    { sel_col_ = 1; sel_row_ = 0; }
                            if (tid == TILE_PLAYLISTS) { sel_col_ = 1; sel_row_ = 1; }
                            if (tid == TILE_SETTINGS)  { sel_col_ = 1; sel_row_ = 2; }
                            if (tid == TILE_PLAYING)   { sel_col_ = 0; sel_row_ = 1; }
                            qa_focused_ = false;
                            return get_tile_action(tid, ctx);
                        }
                        break;
                    }
                }
            }
        }
    }

    // --- D-pad navigation ---
    if (kRepeat & KEY_DDOWN) {
        if (qa_focused_) {
            // Move within QA slots
            if (qa_sel_ < (int)qa_slots_.size() - 1) {
                qa_sel_++;
            } else {
                // Exit QA → Playing (if visible) or wrap
                qa_focused_ = false;
                if (is_playing_visible(ctx)) {
                    sel_row_ = 1;
                } else {
                    sel_row_ = 0;
                    qa_sel_ = 0;
                }
            }
        } else if (sel_col_ == 0) {
            if (sel_row_ == 0) {
                if (is_playing_visible(ctx)) {
                    sel_row_ = 1;
                } else {
                    sel_row_ = 0; // wrap: stay on QA
                }
            } else {
                sel_row_ = 0; // wrap: Playing → QA
            }
        } else {
            sel_row_ = (sel_row_ + 1) % 3;
        }
    }

    if (kRepeat & KEY_DUP) {
        if (qa_focused_) {
            if (qa_sel_ > 0) {
                qa_sel_--;
            } else {
                qa_focused_ = false; // Exit QA focus upward
            }
        } else if (sel_col_ == 0) {
            if (sel_row_ == 1) {
                sel_row_ = 0;
            } else {
                // wrap: QA → Playing (if visible) or stay
                if (is_playing_visible(ctx)) {
                    sel_row_ = 1;
                }
            }
        } else {
            sel_row_ = (sel_row_ + 2) % 3; // wrap up
        }
    }

    if (kRepeat & KEY_DRIGHT) {
        if (sel_col_ == 0) {
            qa_focused_ = false;
            sel_col_ = 1;
            // Map left row to right row
            if (sel_row_ == 0) {
                sel_row_ = 0; // QA → Search
            } else {
                sel_row_ = 2; // Playing → Settings
            }
        }
    }

    if (kRepeat & KEY_DLEFT) {
        if (sel_col_ == 1) {
            sel_col_ = 0;
            if (sel_row_ <= 1) {
                sel_row_ = 0; // Search/Playlists → QA
            } else {
                // Settings → Playing if visible, else QA
                sel_row_ = is_playing_visible(ctx) ? 1 : 0;
            }
        }
    }

    // SELECT on QA slot → remove popup
    if (kDown & KEY_SELECT) {
        if (qa_focused_ && qa_sel_ >= 0 && qa_sel_ < (int)qa_slots_.size()) {
            if (!qa_slots_[qa_sel_].playlist_id.empty()) {
                ctx.selected_playlist_id = qa_slots_[qa_sel_].playlist_id;
                return "qa_remove_popup";
            }
        }
    }

    // A button
    if (kDown & KEY_A) {
        TileId tile = get_selected_tile();

        // Enter QA focus on first A press
        if (tile == TILE_QA && !qa_focused_ && !qa_slots_.empty()) {
            qa_focused_ = true;
            qa_sel_ = 0;
            return "";
        }

        return get_tile_action(tile, ctx);
    }

    // B button exits QA focus
    if (kDown & KEY_B) {
        if (qa_focused_) {
            qa_focused_ = false;
            return "";
        }
    }

    return "";
}

// --- get_tile_action ---

std::string HomeScreen::get_tile_action(TileId id, AppContext& ctx) {
    switch (id) {
    case TILE_SEARCH:    return "trigger_search";
    case TILE_PLAYLISTS: return "trigger_playlists";
    case TILE_SETTINGS:  return "trigger_settings";
    case TILE_PLAYING:
        if (is_playing_visible(ctx)) return "trigger_playing";
        return "";
    case TILE_QA:
        if (qa_focused_ && qa_sel_ >= 0 && qa_sel_ < (int)qa_slots_.size()) {
            if (qa_slots_[qa_sel_].playlist_id.empty()) {
                return "qa_add_popup";
            } else {
                ctx.selected_playlist_id = qa_slots_[qa_sel_].playlist_id;
                return "open_qa_playlist";
            }
        }
        return "";
    default: return "";
    }
}

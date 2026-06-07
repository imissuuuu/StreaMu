#include "track_list_helpers.h"
#include "play_bar.h"
#include "ui_icon_cache.h"
#include "ui_constants.h"
#include "ui_manager.h"

namespace {

struct TextWidthCacheEntry {
  std::string text;
  float scale = 0.0f;
  float width = 0.0f;
  u32 last_used_tick = 0;
};

struct FormattedTrackTextCacheEntry {
  std::string track_id;
  std::string title;
  std::string duration;
  std::string views;
  std::string upload_date;
  bool is_playing = false;
  bool show_views = false;
  std::string display_title;
  std::string meta_line;
  u32 last_used_tick = 0;
};

FormattedTrackTextCacheEntry &find_or_build_track_text_cache(
    const Track &track, bool is_playing, bool show_views) {
  static std::vector<FormattedTrackTextCacheEntry> cache;
  static u32 usage_tick = 0;
  static bool cache_initialized = false;
  if (!cache_initialized) {
    cache.reserve(128);
    cache_initialized = true;
  }

  ++usage_tick;
  for (auto &entry : cache) {
    if (entry.track_id == track.id && entry.title == track.title &&
        entry.duration == track.duration && entry.views == track.views &&
        entry.upload_date == track.upload_date &&
        entry.is_playing == is_playing && entry.show_views == show_views) {
      entry.last_used_tick = usage_tick;
      return entry;
    }
  }

  FormattedTrackTextCacheEntry new_entry = {};
  new_entry.track_id = track.id;
  new_entry.title = track.title;
  new_entry.duration = track.duration;
  new_entry.views = track.views;
  new_entry.upload_date = track.upload_date;
  new_entry.is_playing = is_playing;
  new_entry.show_views = show_views;
  new_entry.display_title = is_playing ? (">> " + track.title) : track.title;
  if (!track.duration.empty() && track.duration != "?") {
    new_entry.meta_line += track.duration;
  }
  if (show_views && !track.views.empty() && track.views != "?") {
    if (!new_entry.meta_line.empty()) {
      new_entry.meta_line += " \xC2\xB7 ";
    }
    new_entry.meta_line += track.views;
  }
  if (!track.upload_date.empty() && track.upload_date != "?") {
    if (!new_entry.meta_line.empty()) {
      new_entry.meta_line += " \xC2\xB7 ";
    }
    new_entry.meta_line += track.upload_date;
  }
  new_entry.last_used_tick = usage_tick;

  if (cache.size() < 128) {
    cache.push_back(std::move(new_entry));
    return cache.back();
  }

  size_t replace_index = 0;
  for (size_t i = 1; i < cache.size(); ++i) {
    if (cache[i].last_used_tick < cache[replace_index].last_used_tick) {
      replace_index = i;
    }
  }
  cache[replace_index] = std::move(new_entry);
  return cache[replace_index];
}

float measure_text_width_cached(const std::string &text, float scale,
                                UIManager &ui_mgr) {
  static std::vector<TextWidthCacheEntry> cache;
  static u32 usage_tick = 0;
  static bool cache_initialized = false;
  if (!cache_initialized) {
    cache.reserve(96);
    cache_initialized = true;
  }

  ++usage_tick;
  for (auto &entry : cache) {
    if (entry.scale == scale && entry.text == text) {
      entry.last_used_tick = usage_tick;
      return entry.width;
    }
  }

  C2D_Text measured_text;
  C2D_TextParse(&measured_text, ui_mgr.get_text_buf(), text.c_str());
  const float measured_width = measured_text.width * scale;

  TextWidthCacheEntry new_entry = {};
  new_entry.text = text;
  new_entry.scale = scale;
  new_entry.width = measured_width;
  new_entry.last_used_tick = usage_tick;

  if (cache.size() < 96) {
    cache.push_back(std::move(new_entry));
  } else {
    size_t replace_index = 0;
    for (size_t i = 1; i < cache.size(); ++i) {
      if (cache[i].last_used_tick < cache[replace_index].last_used_tick) {
        replace_index = i;
      }
    }
    cache[replace_index] = std::move(new_entry);
  }

  return measured_width;
}

int compute_title_scroll_limit(const Track &track, const RenderContext &ctx,
                               UIManager &ui_mgr) {
  if (track.id == "MODE_BTN")
    return 0;

  const auto &formatted = find_or_build_track_text_cache(
      track, track.id == ctx.playing_id && ctx.playing_id != "SEARCH_BTN",
      false);

  const float text_w =
      measure_text_width_cached(formatted.display_title, FONT_SM, ui_mgr);
  const float display_w = 320.0f - BTM_MARGIN_X * 2;
  if (text_w <= display_w)
    return 0;

  return static_cast<int>(text_w - display_w);
}

} // namespace

// ============================================================
// Shared helper: track list drawing for STATE_SEARCH / STATE_PLAYLIST_DETAIL
// ============================================================
void draw_track_list_bottom(const RenderContext &ctx, UIManager &ui_mgr,
                            bool show_views) {
  C2D_Text text;
  int max_vis = BTM_MAX_VISIBLE_2ROW;

  // Scroll: prefer scroll_offset_y during touch scroll (sub-pixel), cursor
  // follow for DPad
  int btm_start;
  float y_base = BTM_LIST_START_Y;
  if (ctx.scroll_offset_y != 0.0f) {
    int scroll_items;
    float y_shift;
    calc_scroll(ctx.scroll_offset_y, BTM_ITEM_HEIGHT_2ROW, scroll_items,
                y_shift);
    btm_start = scroll_items;
    if (btm_start < 0)
      btm_start = 0;
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
    if (y_pos >= 240.0f)
      break;
    // Special rendering for MODE_BTN row
    if (ctx.g_tracks[i].id == "MODE_BTN" &&
        ctx.current_state == STATE_PLAYLIST_DETAIL) {
      // PlayBar-style background for both halves
      u32 bar_bg = (ctx.config.mode == THEME_DARK)
                       ? C2D_Color32(55, 55, 55, 255)
                       : C2D_Color32(230, 230, 230, 255);
      C2D_DrawRectSolid(0, y_pos - 2, 0, 160, BTM_ITEM_HEIGHT_2ROW, bar_bg);
      C2D_DrawRectSolid(160, y_pos - 2, 0, 160, BTM_ITEM_HEIGHT_2ROW, bar_bg);

      // Highlight focused side when selected
      if (i == ctx.selected_index) {
        if (ctx.mode_btn_focus == 0) {
          C2D_DrawRectSolid(0, y_pos - 2, 0, 160, BTM_ITEM_HEIGHT_2ROW,
                            ctx.theme->accent);
        } else {
          C2D_DrawRectSolid(160, y_pos - 2, 0, 160, BTM_ITEM_HEIGHT_2ROW,
                            ctx.theme->accent);
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
                     ? ctx.theme->accent_text
                     : ctx.theme->text_body;
        draw_ui_icon_centered(UiIconId::Shuffle, 80.0f, btn_cy, sc);
      }

      // --- ORDER play icon (right half, center=240) ---
      // ▶ triangle, same size as PlayBar (half_h=12, 24px)
      {
        u32 oc = (i == ctx.selected_index && ctx.mode_btn_focus == 1)
                     ? ctx.theme->accent_text
                     : ctx.theme->text_body;
        draw_ui_icon_centered(UiIconId::Play, 240.0f, btn_cy, oc);
      }

      rendered++;
      continue;
    }

    bool is_playing = (ctx.g_tracks[i].id == ctx.playing_id &&
                       ctx.playing_id != "SEARCH_BTN");

    if (i == ctx.selected_index) {
      C2D_DrawRectSolid(0, y_pos - 2, 0, 320, BTM_ITEM_HEIGHT_2ROW,
                        ctx.theme->accent);
      draw_selection_left_bar(y_pos - 2, BTM_ITEM_HEIGHT_2ROW,
                              ctx.theme->accent);
    } else if (is_playing) {
      C2D_DrawRectSolid(0, y_pos - 2, 0, 320, BTM_ITEM_HEIGHT_2ROW,
                        ctx.theme->playing_bg);
    } else {
      draw_item_bg(0, y_pos - 2, 320, BTM_ITEM_HEIGHT_2ROW,
                   ctx.theme->bg_bottom);
    }

    // Row 1: Title
    const auto &formatted =
        find_or_build_track_text_cache(ctx.g_tracks[i], is_playing, show_views);

    u32 color = (i == ctx.selected_index) ? ctx.theme->accent_text
                : is_playing              ? ctx.theme->accent_text
                                          : ctx.theme->text_body;
    C2D_TextParse(&text, ui_mgr.get_text_buf(),
                  formatted.display_title.c_str());
    float draw_x = BTM_MARGIN_X;
    if (i == ctx.selected_index) {
      const float text_w =
          measure_text_width_cached(formatted.display_title, FONT_SM, ui_mgr);
      float display_w = 320.0f - BTM_MARGIN_X * 2;
      float eff_scroll = (text_w > display_w)
                             ? std::min((float)ctx.scroll_x, text_w - display_w)
                             : 0.0f;
      draw_x = BTM_MARGIN_X - eff_scroll;
    }
    C2D_DrawText(&text, C2D_WithColor, draw_x, y_pos, 0, FONT_SM, FONT_SM,
                 color);

    // Row 2: Metadata
    {
      if (!formatted.meta_line.empty()) {
        u32 meta_color = (i == ctx.selected_index || is_playing)
                             ? ctx.theme->accent_text
                             : ctx.theme->text_body;
        C2D_TextParse(&text, ui_mgr.get_text_buf(),
                      formatted.meta_line.c_str());
        C2D_DrawText(&text, C2D_WithColor, BTM_MARGIN_X + 8,
                     y_pos + BTM_META_OFFSET, 0, FONT_XS, FONT_XS, meta_color);
      }
    }
    // Dashed separator below item (skip for selected — accent fill acts as
    // separator)
    if (i != ctx.selected_index) {
      u32 sep = ctx.theme->text_dim & 0x14FFFFFF;
      draw_dashed_line_h(0, y_pos - 2 + BTM_ITEM_HEIGHT_2ROW - 1, 320, sep);
    }
    rendered++;
  }

  bool effectively_empty =
      ctx.g_tracks.empty() ||
      (ctx.g_tracks.size() == 1 && ctx.g_tracks[0].id == "MODE_BTN");
  if (effectively_empty && ctx.current_state == STATE_PLAYLIST_DETAIL) {
    C2D_TextParse(&text, ui_mgr.get_text_buf(), "No tracks in this playlist.");
    C2D_DrawText(&text, C2D_WithColor, BTM_MARGIN_X, BTM_LIST_START_Y + 12, 0,
                 FONT_SM, FONT_SM, ctx.theme->empty_text);
  }
}

// ============================================================
// draw_menu_button: Draw menu button on bottom screen (shared by all non-Home
// screens)
// ============================================================
void draw_menu_button(const RenderContext &ctx, UIManager &ui_mgr) {
  (void)ui_mgr;
  MenuBtnRect btn = get_menu_btn_rect(ctx.config);
  u32 bg = ctx.theme->text_dim & 0xC0FFFFFF;
  C2D_DrawRectSolid(btn.x, btn.y, 0, btn.w, btn.h, bg);
  draw_ui_icon(UiIconId::Menu, btn.x + 4.0f, btn.y - 1.0f,
               ctx.theme->text_dim);
}

// ============================================================
// Shared helper: DPad navigation for g_tracks list
// ============================================================
void navigate_track_list(AppContext &ctx, u32 kRepeat) {
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
      if (ctx.scroll_x < 0)
        ctx.scroll_x = 0;
    }
  }
  LightLock_Unlock(&ctx.lock);
}

void clamp_scroll_x_for_current_screen(AppContext &ctx, UIManager &ui_mgr) {
  LightLock_Lock(&ctx.lock);

  int max_scroll_x = 0;
  const AppState effective_state =
      is_popup_state(ctx.current_state) ? ctx.previous_state : ctx.current_state;

  switch (effective_state) {
  case STATE_SEARCH:
    if (ctx.selected_index >= 0 &&
        ctx.selected_index < (int)ctx.search_tracks.size()) {
      max_scroll_x = compute_title_scroll_limit(ctx.search_tracks[ctx.selected_index],
                                                ctx, ui_mgr);
    }
    break;
  case STATE_PLAYLIST_DETAIL:
    if (ctx.selected_index >= 0 &&
        ctx.selected_index < (int)ctx.g_tracks.size()) {
      max_scroll_x = compute_title_scroll_limit(ctx.g_tracks[ctx.selected_index],
                                                ctx, ui_mgr);
    }
    break;
  case STATE_PLAYING_UI:
    if (ctx.selected_index >= 0 &&
        ctx.selected_index < (int)ctx.playing_tracks.size()) {
      max_scroll_x =
          compute_title_scroll_limit(ctx.playing_tracks[ctx.selected_index], ctx,
                                     ui_mgr);
    }
    break;
  default:
    ctx.scroll_x = 0;
    LightLock_Unlock(&ctx.lock);
    return;
  }

  if (ctx.scroll_x < 0)
    ctx.scroll_x = 0;
  else if (ctx.scroll_x > max_scroll_x)
    ctx.scroll_x = max_scroll_x;

  LightLock_Unlock(&ctx.lock);
}

// ============================================================
// PlayingScreen-style list: 30px items, y=BTM_LIST_START_Y..PlayBar::BAR_Y
// Used by PlayingScreen and SearchScreen
// ============================================================
void draw_playing_style_list(const std::vector<Track> &tracks,
                             const RenderContext &ctx, UIManager &ui_mgr) {
  if (tracks.empty())
    return;
  C2D_Text text;
  C2D_TextBuf buf = ui_mgr.get_text_buf();

  constexpr float START_Y = BTM_LIST_START_Y; // 8.0f
  constexpr float END_Y = PlayBar::BAR_Y;     // 200.0f
  constexpr float H = BTM_ITEM_HEIGHT_2ROW;   // 30.0f

  int scroll_items;
  float y_shift;
  calc_scroll(ctx.scroll_offset_y, H, scroll_items, y_shift);
  float y_base = START_Y - y_shift;
  int first = scroll_items;
  int visible = (int)((END_Y - START_Y) / H) + 1;

  for (int i = first; i < first + visible; ++i) {
    if (i < 0 || i >= (int)tracks.size())
      continue;
    float y_item = y_base + (i - first) * H;
    if (y_item + H < START_Y || y_item >= END_Y)
      continue;

    const Track &tr = tracks[i];
    bool is_playing = (tr.id == ctx.playing_id);
    bool is_selected = (i == ctx.selected_index);

    if (is_selected) {
      C2D_DrawRectSolid(0, y_item, 0, 320, H, ctx.theme->accent);
      draw_selection_left_bar(y_item, H, ctx.theme->accent);
    } else if (is_playing) {
      C2D_DrawRectSolid(0, y_item, 0, 320, H, ctx.theme->playing_bg);
    } else {
      draw_item_bg(0, y_item, 320, H, ctx.theme->bg_bottom);
    }

    u32 title_color = (is_selected || is_playing) ? ctx.theme->accent_text
                                                  : ctx.theme->text_body;

    const auto &formatted =
        find_or_build_track_text_cache(tr, is_playing, false);
    C2D_TextParse(&text, buf, formatted.display_title.c_str());
    {
      float draw_x = BTM_MARGIN_X;
      if (is_selected) {
        const float text_w =
            measure_text_width_cached(formatted.display_title, FONT_SM, ui_mgr);
        float display_w = 320.0f - BTM_MARGIN_X * 2;
        float eff_scroll = (text_w > display_w) ? std::min((float)ctx.scroll_x,
                                                           text_w - display_w)
                                                : 0.0f;
        draw_x = BTM_MARGIN_X - eff_scroll;
      }
      C2D_DrawText(&text, C2D_WithColor, draw_x, y_item, 0, FONT_SM, FONT_SM,
                   title_color);
    }
    if (!formatted.meta_line.empty()) {
      C2D_TextParse(&text, buf, formatted.meta_line.c_str());
      C2D_DrawText(&text, C2D_WithColor, BTM_MARGIN_X + 8,
                   y_item + BTM_META_OFFSET, 0, FONT_XS, FONT_XS, title_color);
    }
    if (!is_selected) {
      u32 sep = ctx.theme->text_dim & 0x14FFFFFF;
      draw_dashed_line_h(0, y_item + H - 1, 320, sep);
    }
  }
}

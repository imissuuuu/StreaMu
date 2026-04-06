#include "settings_screen.h"
#include "../../ui/ui_manager.h"
#include "../ui_constants.h"
#include "../track_list_helpers.h"
#include <dirent.h>
#include <sys/stat.h>

#define WALLPAPER_DIR "sdmc:/3ds/StreaMu/wallpaper"

// Pinned Save button y position (fixed, always visible regardless of scroll content)
static constexpr float SAVE_Y = 210.0f;

// Settings screen item indices
enum SettingsItem {
  ITEM_MODE = 0,
  ITEM_COLOR = 1,
  ITEM_HUE = 2,
  ITEM_SATURATION = 3,
  ITEM_BRIGHTNESS = 4,
  ITEM_L_BUTTON = 5,
  ITEM_R_BUTTON = 6,
  ITEM_DPAD_SPEED = 7,
  ITEM_WALLPAPER = 8,
  ITEM_SERVER_IP = 9,
  ITEM_LANGUAGE = 10,
  ITEM_SEPARATOR = 11,
  ITEM_SAVE = 12
};

SettingsScreen::SettingsScreen(ThemeColors &colors, Wallpaper* wallpaper)
    : m_colors(colors), m_wallpaper(wallpaper) {}

void SettingsScreen::on_enter(AppContext &ctx) {
  ctx.current_state = STATE_SETTINGS;
  editing_config_ = ctx.config; // Copy current config
  apply_theme(editing_config_, preview_colors_); // Init preview colors
  selected_item_ = 0;
  last_tapped_item_ = -1;
  scroll_offset_ = 0.0f;
  in_palette_mode_ = false;
  in_edit_mode_ = false;
  in_wallpaper_mode_ = false;
  palette_row_ = 0;
  palette_col_ = 0;

  // Restore row/col from palette index
  if (editing_config_.palette_index >= 0) {
    palette_row_ = editing_config_.palette_index / COLOR_PALETTE_COLS;
    palette_col_ = editing_config_.palette_index % COLOR_PALETTE_COLS;
    if (palette_row_ >= COLOR_PALETTE_ROWS)
      palette_row_ = 0;
  }
}

void SettingsScreen::apply_preview() { apply_theme(editing_config_, preview_colors_); }

std::string SettingsScreen::lr_action_name(LRAction action) const {
  switch (action) {
  case LR_DISABLED:
    return "Disabled";
  case LR_SKIP_BACK:
    return "Skip Back";
  case LR_SKIP_FORWARD:
    return "Skip Fwd";
  case LR_PLAY_PAUSE:
    return "Play/Pause";
  default:
    return "???";
  }
}

std::string SettingsScreen::get_item_label(int index) const {
  switch (index) {
  case ITEM_MODE:
    return "Mode";
  case ITEM_COLOR:
    return "Color";
  case ITEM_HUE:
    return "Hue";
  case ITEM_SATURATION:
    return "Saturation";
  case ITEM_BRIGHTNESS:
    return "Brightness";
  case ITEM_L_BUTTON:
    return "L Button";
  case ITEM_R_BUTTON:
    return "R Button";
  case ITEM_DPAD_SPEED:
    return "D-Pad Speed";
  case ITEM_WALLPAPER:
    return "Wallpaper";
  case ITEM_SERVER_IP:
    return "Server IP";
  case ITEM_LANGUAGE:
    return "Language";
  case ITEM_SEPARATOR:
    return "";
  case ITEM_SAVE:
    return "Save & Back";
  default:
    return "";
  }
}

std::string SettingsScreen::get_item_value(int index) const {
  switch (index) {
  case ITEM_MODE:
    return editing_config_.mode == THEME_DARK ? "Dark" : "Light";
  case ITEM_COLOR:
    return editing_config_.palette_index >= 0 ? "Palette" : "Custom";
  case ITEM_HUE: {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", editing_config_.accent_hue);
    return std::string(buf);
  }
  case ITEM_SATURATION: {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.2f", editing_config_.accent_saturation);
    return std::string(buf);
  }
  case ITEM_BRIGHTNESS: {
    char buf[8];
    snprintf(buf, sizeof(buf), "%.2f", editing_config_.accent_brightness);
    return std::string(buf);
  }
  case ITEM_L_BUTTON:
    return lr_action_name(editing_config_.l_action);
  case ITEM_R_BUTTON:
    return lr_action_name(editing_config_.r_action);
  case ITEM_DPAD_SPEED: {
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", editing_config_.dpad_speed);
    return std::string(buf);
  }
  case ITEM_WALLPAPER:
    return editing_config_.wallpaper_file.empty() ? "None" : editing_config_.wallpaper_file;
  case ITEM_SERVER_IP:
    return editing_config_.server_ip.empty() ? "Not Set" : editing_config_.server_ip;
  case ITEM_LANGUAGE:
    return editing_config_.language == "ja" ? "JA" : "EN";
  default:
    return "";
  }
}

std::string SettingsScreen::get_item_description(int index) const {
  switch (index) {
  case ITEM_MODE:
    return "Switch between Light and Dark mode.\nAffects background, text, and "
           "popup colors.";
  case ITEM_COLOR:
    return "Press A to open the color palette.\nSelect a preset color for your "
           "theme.";
  case ITEM_HUE:
    return "Fine-tune the accent color.\nUse Left/Right to adjust hue (0-360).";
  case ITEM_SATURATION:
    return "Adjust color saturation.\nLeft: muted/pastel  Right: vivid.";
  case ITEM_BRIGHTNESS:
    return "Adjust color brightness.\nLeft: dark  Right: bright.";
  case ITEM_L_BUTTON:
    return "Set L button behavior.\nDisabled / Skip Back / Skip Fwd / Play-Pause";
  case ITEM_R_BUTTON:
    return "Set R button behavior.\nDisabled / Skip Back / Skip Fwd / Play-Pause";
  case ITEM_DPAD_SPEED:
    return "D-pad repeat speed when held.\n1=Slow  5=Normal  10=Fast";
  case ITEM_WALLPAPER:
    return "Set a PNG wallpaper for the top screen.\nPlace files in wallpaper/ folder on SD.";
  case ITEM_SERVER_IP:
    return "Server IP address and port.\nPress A to change.";
  case ITEM_LANGUAGE:
    return "Metadata language for search results.\nEN=English  JA=Japanese";
  case ITEM_SAVE:
    return "Save settings to SD card and return\nto the home screen.";
  default:
    return "";
  }
}

std::string SettingsScreen::update(AppContext &ctx, u32 kDown, u32 kHeld, u32 kRepeat) {
  // ========== Wallpaper selection mode ==========
  if (in_wallpaper_mode_) {
    int item_count = 1 + (int)wallpaper_files_.size(); // "None" + files

    if (kRepeat & KEY_DUP) {
      wallpaper_selected_--;
      if (wallpaper_selected_ < 0) wallpaper_selected_ = item_count - 1;
    }
    if (kRepeat & KEY_DDOWN) {
      wallpaper_selected_++;
      if (wallpaper_selected_ >= item_count) wallpaper_selected_ = 0;
    }

    // Touch selection
    touchPosition touch;
    hidTouchRead(&touch);
    if (kDown & KEY_TOUCH) {
      float list_y = 24;
      float item_h = 28;
      // Calculate scroll offset (same logic as draw_bottom)
      int max_visible = 7;
      int start_idx = 0;
      if (wallpaper_selected_ > max_visible / 2)
        start_idx = wallpaper_selected_ - max_visible / 2;
      if (start_idx + max_visible > item_count)
        start_idx = item_count - max_visible;
      if (start_idx < 0) start_idx = 0;
      int tapped = (int)((touch.py - list_y) / item_h) + start_idx;
      if (tapped >= 0 && tapped < item_count) {
        wallpaper_selected_ = tapped;
        // No double-tap needed, confirm immediately
        if (wallpaper_selected_ == 0) {
          editing_config_.wallpaper_file = "";
        } else {
          editing_config_.wallpaper_file = wallpaper_files_[wallpaper_selected_ - 1];
        }
        // Instant preview (load wallpaper)
        if (m_wallpaper) {
          if (editing_config_.wallpaper_file.empty()) {
            m_wallpaper->unload();
          } else {
            std::string path = std::string(WALLPAPER_DIR "/") + editing_config_.wallpaper_file;
            m_wallpaper->load(path);
          }
        }
        in_wallpaper_mode_ = false;
        ctx.touch_state.reset();
        return "";
      }
    }

    if (kDown & KEY_A) {
      if (wallpaper_selected_ == 0) {
        editing_config_.wallpaper_file = "";
      } else {
        editing_config_.wallpaper_file = wallpaper_files_[wallpaper_selected_ - 1];
      }
      if (m_wallpaper) {
        if (editing_config_.wallpaper_file.empty()) {
          m_wallpaper->unload();
        } else {
          std::string path = std::string(WALLPAPER_DIR "/") + editing_config_.wallpaper_file;
          m_wallpaper->load(path);
        }
      }
      in_wallpaper_mode_ = false;
    }
    if (kDown & KEY_B) {
      // Cancel: restore original wallpaper
      if (m_wallpaper) {
        if (ctx.config.wallpaper_file.empty()) {
          m_wallpaper->unload();
        } else {
          std::string path = std::string(WALLPAPER_DIR "/") + ctx.config.wallpaper_file;
          m_wallpaper->load(path);
        }
      }
      editing_config_.wallpaper_file = ctx.config.wallpaper_file;
      in_wallpaper_mode_ = false;
    }
    return "";
  }

  // ========== Palette mode ==========
  if (in_palette_mode_) {
    // D-pad cursor movement
    if (kRepeat & KEY_DUP)
      palette_row_ = (palette_row_ - 1 + COLOR_PALETTE_ROWS) % COLOR_PALETTE_ROWS;
    if (kRepeat & KEY_DDOWN)
      palette_row_ = (palette_row_ + 1) % COLOR_PALETTE_ROWS;
    if (kRepeat & KEY_DLEFT)
      palette_col_ = (palette_col_ - 1 + COLOR_PALETTE_COLS) % COLOR_PALETTE_COLS;
    if (kRepeat & KEY_DRIGHT)
      palette_col_ = (palette_col_ + 1) % COLOR_PALETTE_COLS;

    // Touch-down instant confirm (no release wait)
    touchPosition touch;
    hidTouchRead(&touch);
    if (kDown & KEY_TOUCH) {
      float grid_x = 10, grid_y = 24;
      float cell_w = 36, cell_h = 36, gap = 2;
      int col = (int)((touch.px - grid_x) / (cell_w + gap));
      int row = (int)((touch.py - grid_y) / (cell_h + gap));
      if (col >= 0 && col < COLOR_PALETTE_COLS && row >= 0 && row < COLOR_PALETTE_ROWS) {
        palette_col_ = col;
        palette_row_ = row;
        editing_config_.accent_hue = palette_get_hue(palette_row_, palette_col_);
        editing_config_.palette_index = palette_row_ * COLOR_PALETTE_COLS + palette_col_;
        if (palette_row_ == 4) {
          editing_config_.accent_saturation = 0.0f;
          editing_config_.accent_brightness = palette_get_gray_v(palette_col_);
        } else {
          editing_config_.accent_saturation = PALETTE_ROW_SV[palette_row_].s;
          editing_config_.accent_brightness = PALETTE_ROW_SV[palette_row_].v;
        }
        apply_preview();
        in_palette_mode_ = false;
        in_edit_mode_ = false;
        ctx.touch_state.reset();
        return "";
      }
    }

    // Real-time preview
    editing_config_.accent_hue = palette_get_hue(palette_row_, palette_col_);
    editing_config_.palette_index = palette_row_ * COLOR_PALETTE_COLS + palette_col_;
    if (palette_row_ == 4) {
      editing_config_.accent_saturation = 0.0f;
      editing_config_.accent_brightness = palette_get_gray_v(palette_col_);
    } else {
      editing_config_.accent_saturation = PALETTE_ROW_SV[palette_row_].s;
      editing_config_.accent_brightness = PALETTE_ROW_SV[palette_row_].v;
    }
    apply_preview();

    if (kDown & KEY_A) {
      in_palette_mode_ = false;
      in_edit_mode_ = false;
    }
    if (kDown & KEY_B) {
      editing_config_ = ctx.config;
      apply_preview();
      in_palette_mode_ = false;
      in_edit_mode_ = false;
    }
    return "";
  }

  // ========== D-Pad Speed slider edit mode ==========
  if (in_edit_mode_ && selected_item_ == ITEM_DPAD_SPEED) {
    touchPosition touch;
    hidTouchRead(&touch);

    float bar_x = 100, bar_w = 180;
    float item_y = 8.0f + ITEM_DPAD_SPEED * 28.0f - scroll_offset_;

    // Tap outside slider -> exit edit mode
    if (kDown & KEY_TOUCH) {
      if (touch.py < item_y || touch.py >= item_y + 28.0f) {
        in_edit_mode_ = false;
        last_tapped_item_ = -1;
        ctx.touch_state.reset();
        return "";
      }
    }
    // Snap to touch position
    if ((kDown & KEY_TOUCH) || (kHeld & KEY_TOUCH)) {
      if (touch.px >= (int)bar_x && touch.px <= (int)(bar_x + bar_w) &&
          touch.py >= (int)(item_y + 7) && touch.py <= (int)(item_y + 21)) {
        float ratio = (float)(touch.px - (int)bar_x) / bar_w;
        int step = (int)(ratio * 9.0f + 0.5f) + 1; // Round to 1-10
        if (step > 10) step = 10;
        editing_config_.dpad_speed = step;
      }
    }
    // D-pad left/right +/-1
    if (kRepeat & KEY_DRIGHT) {
      if (editing_config_.dpad_speed < 10) editing_config_.dpad_speed++;
    }
    if (kRepeat & KEY_DLEFT) {
      if (editing_config_.dpad_speed > 1) editing_config_.dpad_speed--;
    }
    if (kDown & KEY_A) { in_edit_mode_ = false; }
    if (kDown & KEY_B) {
      editing_config_.dpad_speed = ctx.config.dpad_speed;
      in_edit_mode_ = false;
    }
    return "";
  }

  // ========== HUE slider edit mode ==========
  if (in_edit_mode_ && selected_item_ == ITEM_HUE) {
    touchPosition touch;
    hidTouchRead(&touch);

    // Tap outside slider -> exit edit mode
    if (kDown & KEY_TOUCH) {
      float item_y = 8.0f + ITEM_HUE * 28.0f - scroll_offset_;
      if (touch.py < item_y || touch.py >= item_y + 28.0f) {
        in_edit_mode_ = false;
        last_tapped_item_ = -1;
        ctx.touch_state.reset();
        return "";
      }
    }
    // Slider drag
    if ((kDown & KEY_TOUCH) || (kHeld & KEY_TOUCH)) {
      float bar_x = 100, bar_w = 180;
      float item_y = 8.0f + ITEM_HUE * 28.0f - scroll_offset_;
      if (touch.px >= (int)bar_x && touch.px <= (int)(bar_x + bar_w) &&
          touch.py >= (int)(item_y + 7) && touch.py <= (int)(item_y + 21)) {
        editing_config_.accent_hue =
            (int)(((float)(touch.px - (int)bar_x) / bar_w) * 360.0f);
        if (editing_config_.accent_hue > 360) editing_config_.accent_hue = 360;
        if (editing_config_.accent_hue < 0)   editing_config_.accent_hue = 0;
        editing_config_.palette_index = -1;
        apply_preview();
      }
    }
    // D-pad fine-tune
    if (kRepeat & KEY_DRIGHT) {
      editing_config_.accent_hue = (editing_config_.accent_hue + 5) % 361;
      editing_config_.palette_index = -1;
      apply_preview();
    }
    if (kRepeat & KEY_DLEFT) {
      editing_config_.accent_hue = (editing_config_.accent_hue - 5 + 361) % 361;
      editing_config_.palette_index = -1;
      apply_preview();
    }
    if (kDown & KEY_A) { in_edit_mode_ = false; }
    if (kDown & KEY_B) {
      editing_config_ = ctx.config;
      apply_preview();
      in_edit_mode_ = false;
    }
    return "";
  }

  // ========== SATURATION / BRIGHTNESS slider edit mode ==========
  if (in_edit_mode_ && (selected_item_ == ITEM_SATURATION || selected_item_ == ITEM_BRIGHTNESS)) {
    float* target = (selected_item_ == ITEM_SATURATION)
        ? &editing_config_.accent_saturation
        : &editing_config_.accent_brightness;

    touchPosition touch;
    hidTouchRead(&touch);

    // Tap outside slider -> exit edit mode
    if (kDown & KEY_TOUCH) {
      float item_y = 8.0f + selected_item_ * 28.0f - scroll_offset_;
      if (touch.py < item_y || touch.py >= item_y + 28.0f) {
        in_edit_mode_ = false;
        last_tapped_item_ = -1;
        ctx.touch_state.reset();
        return "";
      }
    }
    // Slider drag
    if ((kDown & KEY_TOUCH) || (kHeld & KEY_TOUCH)) {
      float bar_x = 100, bar_w = 180;
      float item_y = 8.0f + selected_item_ * 28.0f - scroll_offset_;
      if (touch.px >= (int)bar_x && touch.px <= (int)(bar_x + bar_w) &&
          touch.py >= (int)(item_y + 7) && touch.py <= (int)(item_y + 21)) {
        *target = (float)(touch.px - (int)bar_x) / bar_w;
        if (*target > 1.0f) *target = 1.0f;
        if (*target < 0.0f) *target = 0.0f;
        editing_config_.palette_index = -1;
        apply_preview();
      }
    }
    // D-pad fine-tune
    if (kRepeat & KEY_DRIGHT) {
      *target += 0.05f;
      if (*target > 1.0f) *target = 1.0f;
      editing_config_.palette_index = -1;
      apply_preview();
    }
    if (kRepeat & KEY_DLEFT) {
      *target -= 0.05f;
      if (*target < 0.0f) *target = 0.0f;
      editing_config_.palette_index = -1;
      apply_preview();
    }
    if (kDown & KEY_A) { in_edit_mode_ = false; }
    if (kDown & KEY_B) {
      editing_config_ = ctx.config;
      apply_preview();
      in_edit_mode_ = false;
    }
    return "";
  }

  // ========== Normal mode: touch (two-tap + swipe scroll) ==========
  {
    touchPosition touch;
    hidTouchRead(&touch);
    if (kDown & KEY_TOUCH) {
      ctx.touch_state.begin(touch.px, touch.py);
    }
    // Touch held -> scroll handling
    if ((kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
      int delta_y = ctx.touch_state.update(touch.px, touch.py);
      if (ctx.touch_state.is_dragging) {
        scroll_offset_ += delta_y;
        if (scroll_offset_ < 0.0f) scroll_offset_ = 0.0f;
        // Items 0..ITEM_SEPARATOR scroll into SAVE_Y; Save is pinned below
        float max_scroll = 8.0f + (ITEM_SAVE - 1) * 28.0f + 20.0f - SAVE_Y;
        if (scroll_offset_ > max_scroll) scroll_offset_ = max_scroll;
        last_tapped_item_ = -1; // Reset two-tap during drag
      }
    }
    if (!(kHeld & KEY_TOUCH) && ctx.touch_state.is_touching) {
      bool was_tap = ctx.touch_state.end();
      if (was_tap) {
        int tap_x = ctx.touch_state.current_x;
        int tap_y = ctx.touch_state.current_y;

        // Menu button
        MenuBtnRect btn = get_menu_btn_rect(ctx.config);
        if (tap_x >= btn.x && tap_x <= btn.x + btn.w &&
            tap_y >= btn.y && tap_y <= btn.y + btn.h) {
          return "trigger_nav_menu";
        }

        // Pinned Save button tap (two-tap)
        if (tap_y >= (int)SAVE_Y && tap_y < (int)(SAVE_Y + 28)) {
          if (selected_item_ == ITEM_SAVE && last_tapped_item_ == ITEM_SAVE) {
            last_tapped_item_ = -1;
            ctx.config = editing_config_;
            ConfigManager::save(editing_config_);
            apply_theme(ctx.config, m_colors);
            return "trigger_home";
          } else {
            selected_item_ = ITEM_SAVE;
            last_tapped_item_ = ITEM_SAVE;
          }
          return "";
        }

        // Settings item tap (two-tap: 1st=select, 2nd=execute)
        float y = 8 - scroll_offset_;
        float item_h = 28;
        bool hit = false;
        for (int i = 0; i < ITEM_SAVE; i++) {  // Save is pinned; loop excludes it
          if (i == ITEM_SEPARATOR) { y += 20; continue; }
          if (tap_y >= (int)y && tap_y < (int)(y + item_h)) {
            hit = true;
            if (i == selected_item_ && i == last_tapped_item_) {
              // 2nd tap: execute
              last_tapped_item_ = -1;
              if (i == ITEM_COLOR) {
                in_palette_mode_ = true;
              } else if (i == ITEM_HUE || i == ITEM_SATURATION || i == ITEM_BRIGHTNESS) {
                in_edit_mode_ = true;
              } else if (i == ITEM_MODE) {
                editing_config_.mode =
                    (editing_config_.mode == THEME_LIGHT) ? THEME_DARK : THEME_LIGHT;
                apply_preview();
              } else if (i == ITEM_L_BUTTON) {
                editing_config_.l_action =
                    (LRAction)(((int)editing_config_.l_action + 1) % 4);
              } else if (i == ITEM_R_BUTTON) {
                editing_config_.r_action =
                    (LRAction)(((int)editing_config_.r_action + 1) % 4);
              } else if (i == ITEM_DPAD_SPEED) {
                in_edit_mode_ = true;
              } else if (i == ITEM_WALLPAPER) {
                wallpaper_files_ = list_wallpapers();
                wallpaper_selected_ = 0;
                in_wallpaper_mode_ = true;
              } else if (i == ITEM_LANGUAGE) {
                editing_config_.language = (editing_config_.language == "en") ? "ja" : "en";
              }
            } else {
              // 1st tap: select
              selected_item_ = i;
              last_tapped_item_ = i;
            }
            return "";
          }
          y += item_h;
        }
        if (!hit) last_tapped_item_ = -1;  // Empty area: deselect
      }
    }
  }

  // ========== D-pad navigation ==========
  if (kRepeat & KEY_DUP) {
    selected_item_--;
    if (selected_item_ == ITEM_SEPARATOR) selected_item_--;
    if (selected_item_ < 0) selected_item_ = ITEM_COUNT - 1;
    last_tapped_item_ = -1;
  }
  if (kRepeat & KEY_DDOWN) {
    selected_item_++;
    if (selected_item_ == ITEM_SEPARATOR) selected_item_++;
    if (selected_item_ >= ITEM_COUNT) selected_item_ = 0;
    last_tapped_item_ = -1;
  }

  // After D-pad move: scroll to keep selected item visible
  // (ITEM_SAVE is pinned at SAVE_Y, always visible — no scroll needed)
  if ((kRepeat & (KEY_DUP | KEY_DDOWN)) && selected_item_ != ITEM_SAVE) {
    float item_y = 8.0f;
    for (int i = 0; i < selected_item_; i++) {
      item_y += (i == ITEM_SEPARATOR) ? 20.0f : 28.0f;
    }
    if (item_y < scroll_offset_) {
      scroll_offset_ = item_y;
    } else if (item_y + 28.0f > scroll_offset_ + SAVE_Y) {
      scroll_offset_ = item_y + 28.0f - SAVE_Y;
    }
    if (scroll_offset_ < 0.0f) scroll_offset_ = 0.0f;
  }

  // KEY_A: direct action (MODE/L/R toggle, HUE/COLOR enter edit mode)
  if (kDown & KEY_A) {
    switch (selected_item_) {
    case ITEM_MODE:
      editing_config_.mode =
          (editing_config_.mode == THEME_LIGHT) ? THEME_DARK : THEME_LIGHT;
      apply_preview();
      break;
    case ITEM_HUE:
    case ITEM_SATURATION:
    case ITEM_BRIGHTNESS:
      in_edit_mode_ = true;
      break;
    case ITEM_L_BUTTON:
      editing_config_.l_action = (LRAction)(((int)editing_config_.l_action + 1) % 4);
      break;
    case ITEM_R_BUTTON:
      editing_config_.r_action = (LRAction)(((int)editing_config_.r_action + 1) % 4);
      break;
    case ITEM_COLOR:
      in_palette_mode_ = true;
      break;
    case ITEM_DPAD_SPEED:
      in_edit_mode_ = true;
      break;
    case ITEM_WALLPAPER:
      wallpaper_files_ = list_wallpapers();
      wallpaper_selected_ = 0;
      in_wallpaper_mode_ = true;
      break;
    case ITEM_LANGUAGE:
      editing_config_.language = (editing_config_.language == "en") ? "ja" : "en";
      break;
    case ITEM_SERVER_IP: {
      std::string current = editing_config_.server_ip;
      bool accepted = false;
      while (!accepted) {
        SwkbdState swkbd;
        char buf[64] = {0};
        swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 1, 63);
        swkbdSetHintText(&swkbd, "IP:Port");
        if (!current.empty())
          swkbdSetInitialText(&swkbd, current.c_str());
        SwkbdButton button = swkbdInputText(&swkbd, buf, sizeof(buf));
        if (button != SWKBD_BUTTON_CONFIRM || strlen(buf) == 0) break;
        std::string input(buf);
        // Simple validation: IP with '.' + ':' + digits-only port
        size_t colon = input.rfind(':');
        if (colon != std::string::npos && colon > 0 && colon < input.size() - 1
            && input.substr(0, colon).find('.') != std::string::npos) {
          bool port_ok = true;
          for (size_t i = colon + 1; i < input.size(); ++i)
            if (input[i] < '0' || input[i] > '9') { port_ok = false; break; }
          if (port_ok) { editing_config_.server_ip = input; accepted = true; }
        }
        if (!accepted) current = input;  // Re-display
      }
      break;
    }
    case ITEM_SAVE:
      ctx.config = editing_config_;
      ConfigManager::save(editing_config_);
      apply_theme(ctx.config, m_colors);
      return "trigger_home";
    }
  }

  if (kDown & KEY_B) {
    apply_theme(ctx.config, m_colors);
    return "trigger_home";
  }

  return "";
}

void SettingsScreen::draw_top(const RenderContext &ctx, UIManager &ui_mgr) {
  C2D_TextBuf buf = ui_mgr.get_text_buf();
  C2D_Text text;

  // Top screen background (preview)
  C2D_DrawRectSolid(0, 0, 0, 400, 240, preview_colors_.bg_top);

  // --- Header ---
  C2D_TextParse(&text, buf, "SETTINGS");
  C2D_DrawText(&text, C2D_WithColor, 8, 8, 0, 0.65f, 0.65f, preview_colors_.title);
  C2D_DrawRectSolid(8, 28, 0, 384, 1, preview_colors_.text_dim);

  if (in_palette_mode_) {
    // Palette mode: show preview on top screen
    C2D_TextParse(&text, buf, "Select a color from the palette below.");
    C2D_DrawText(&text, C2D_WithColor, 8, 40, 0, 0.5f, 0.5f, preview_colors_.text_body);

    // Mock UI preview
    float py = 70;
    C2D_TextParse(&text, buf, "Preview:");
    C2D_DrawText(&text, C2D_WithColor, 8, py, 0, 0.5f, 0.5f, preview_colors_.text_dim);
    py += 18;

    // Mock button with accent color
    C2D_DrawRectSolid(20, py, 0, 160, 40, preview_colors_.accent);
    C2D_TextParse(&text, buf, "Button");
    C2D_DrawText(&text, C2D_WithColor, 70, py + 10, 0, 0.55f, 0.55f, preview_colors_.accent_text);

    // Playing bar
    py += 50;
    C2D_DrawRectSolid(20, py, 0, 360, 25, preview_colors_.playing_bg);
    C2D_TextParse(&text, buf, ">> Now Playing: Sample Track");
    C2D_DrawText(&text, C2D_WithColor, 28, py + 4, 0, 0.5f, 0.5f, preview_colors_.playing_text);
  } else {
    // Normal mode: description of selected item + preview
    std::string desc = get_item_description(selected_item_);
    if (!desc.empty()) {
      // Split by newline and display
      float dy = 40;
      size_t pos = 0;
      while (pos < desc.size()) {
        size_t nl = desc.find('\n', pos);
        std::string line = (nl == std::string::npos)
                               ? desc.substr(pos)
                               : desc.substr(pos, nl - pos);
        C2D_TextParse(&text, buf, line.c_str());
        C2D_DrawText(&text, C2D_WithColor, 8, dy, 0, 0.5f, 0.5f, preview_colors_.text_body);
        dy += 16;
        if (nl == std::string::npos)
          break;
        pos = nl + 1;
      }
    }

    // Theme preview
    float py = 110;
    C2D_TextParse(&text, buf, "Theme Preview:");
    C2D_DrawText(&text, C2D_WithColor, 8, py, 0, 0.45f, 0.45f, preview_colors_.text_dim);
    py += 16;

    // Background preview
    C2D_DrawRectSolid(20, py, 0, 360, 80, preview_colors_.bg_bottom);

    // Mock list items
    C2D_DrawRectSolid(25, py + 5, 0, 350, 22, preview_colors_.accent);
    C2D_TextParse(&text, buf, "  Selected Item");
    C2D_DrawText(&text, C2D_WithColor, 30, py + 7, 0, 0.45f, 0.45f, preview_colors_.accent_text);

    C2D_TextParse(&text, buf, "  Normal Item");
    C2D_DrawText(&text, C2D_WithColor, 30, py + 30, 0, 0.45f, 0.45f, preview_colors_.text_body);

    C2D_DrawRectSolid(25, py + 52, 0, 350, 22, preview_colors_.playing_bg);
    C2D_TextParse(&text, buf, "  >> Playing Track");
    C2D_DrawText(&text, C2D_WithColor, 30, py + 54, 0, 0.45f, 0.45f, preview_colors_.playing_text);
  }
}

void SettingsScreen::draw_bottom(const RenderContext &ctx, UIManager &ui_mgr) {
  C2D_TextBuf buf = ui_mgr.get_text_buf();
  C2D_Text text;

  // Background
  C2D_DrawRectSolid(0, 0, 0, 320, 240, m_colors.bg_bottom);

  if (in_wallpaper_mode_) {
    // === Wallpaper selection list ===
    C2D_TextParse(&text, buf, "Wallpaper (A: Select, B: Cancel)");
    C2D_DrawText(&text, C2D_WithColor, 8, 4, 0, 0.45f, 0.45f, m_colors.text_body);

    float list_y = 24;
    float item_h = 28;
    int item_count = 1 + (int)wallpaper_files_.size();

    // Scroll support
    int max_visible = 7;
    int start_idx = 0;
    if (wallpaper_selected_ > max_visible / 2)
      start_idx = wallpaper_selected_ - max_visible / 2;
    if (start_idx + max_visible > item_count)
      start_idx = item_count - max_visible;
    if (start_idx < 0) start_idx = 0;

    for (int i = start_idx; i < item_count && i < start_idx + max_visible; i++) {
      float y = list_y + (i - start_idx) * item_h;

      if (i == wallpaper_selected_) {
        C2D_DrawRectSolid(0, y, 0, 320, item_h, m_colors.accent);
      }

      u32 color = (i == wallpaper_selected_) ? m_colors.accent_text : m_colors.text_body;
      std::string label = (i == 0) ? "[ None ]" : wallpaper_files_[i - 1];
      C2D_TextParse(&text, buf, label.c_str());
      C2D_DrawText(&text, C2D_WithColor, 16, y + 4, 0, 0.5f, 0.5f, color);
    }

    // Footer
    C2D_TextParse(&text, buf, "Place PNG files in wallpaper/ folder");
    C2D_DrawText(&text, C2D_WithColor, 40, 224, 0, 0.4f, 0.4f, m_colors.text_dim);

  } else if (in_palette_mode_) {
    // === Palette grid ===
    C2D_TextParse(&text, buf, "Color Palette (A: Select, B: Cancel)");
    C2D_DrawText(&text, C2D_WithColor, 8, 4, 0, 0.45f, 0.45f, m_colors.text_body);

    float grid_x = 10, grid_y = 24;
    float cell_w = 36, cell_h = 36;
    float gap = 2;

    for (int r = 0; r < COLOR_PALETTE_ROWS; r++) {
      for (int c = 0; c < COLOR_PALETTE_COLS; c++) {
        float cx = grid_x + c * (cell_w + gap);
        float cy = grid_y + r * (cell_h + gap);
        u32 color = palette_get_color(r, c);
        C2D_DrawRectSolid(cx, cy, 0, cell_w, cell_h, color);

        // Cursor on selected cell
        if (r == palette_row_ && c == palette_col_) {
          float b = 3.0f;
          u32 cc = m_colors.cursor_outline;
          C2D_DrawRectSolid(cx, cy, 0, cell_w, b, cc);
          C2D_DrawRectSolid(cx, cy + cell_h - b, 0, cell_w, b, cc);
          C2D_DrawRectSolid(cx, cy, 0, b, cell_h, cc);
          C2D_DrawRectSolid(cx + cell_w - b, cy, 0, b, cell_h, cc);
        }
      }
    }

    // Preview bar removed (unnecessary)

  } else {
    // === Normal settings list (with scroll) ===
    float y = 8 - scroll_offset_;
    float item_h = 28;

    for (int i = 0; i < ITEM_SAVE; i++) {  // Save is pinned below; exclude from scroll
      if (i == ITEM_SEPARATOR) {
        // Separator line (skip if off-screen)
        if (y + 20 >= 0 && y < SAVE_Y) {
          C2D_DrawRectSolid(8, y + 10, 0, 304, 1, m_colors.text_dim);
        }
        y += 20;
        continue;
      }

      // Skip off-screen items
      if (y + item_h < 0 || y >= SAVE_Y) { y += item_h; continue; }

      // Selection highlight (edit mode uses different bg color)
      if (i == selected_item_) {
        if (in_edit_mode_) {
          C2D_DrawRectSolid(0, y, 0, 320, item_h, m_colors.playing_bg); // Edit mode bg
        } else {
          C2D_DrawRectSolid(0, y, 0, 320, item_h, m_colors.accent);
        }
      }

      u32 label_color =
          (i == selected_item_) ? m_colors.accent_text : m_colors.text_body;
      u32 value_color =
          (i == selected_item_) ? m_colors.accent_text : m_colors.text_dim;

      std::string label = get_item_label(i);
      std::string value = get_item_value(i);

      if (i == ITEM_SAVE) {
        // Save & Back centered
        C2D_TextParse(&text, buf, "[ Save & Back ]");
        float tw = 0, th = 0;
        C2D_TextGetDimensions(&text, 0.55f, 0.55f, &tw, &th);
        C2D_DrawText(&text, C2D_WithColor, (320 - tw) / 2, y + 4, 0, 0.55f, 0.55f,
                     label_color);
      } else if (i == ITEM_HUE) {
        // Hue slider display
        C2D_TextParse(&text, buf, label.c_str());
        C2D_DrawText(&text, C2D_WithColor, 8, y + 4, 0, 0.5f, 0.5f, label_color);

        // Slider bar
        float bar_x = 100, bar_w = 180;
        float fill = (float)editing_config_.accent_hue / 360.0f;
        C2D_DrawRectSolid(bar_x, y + 10, 0, bar_w, 8, m_colors.text_dim);

        // Rainbow gradient (simplified: 6 segments)
        for (int seg = 0; seg < 6; seg++) {
          float sx = bar_x + seg * (bar_w / 6);
          float sw = bar_w / 6;
          u32 seg_color = hsv_to_color32(seg * 60, 0.8f, 0.8f);
          C2D_DrawRectSolid(sx, y + 10, 0, sw, 8, seg_color);
        }

        // Cursor position
        float cursor_x = bar_x + fill * bar_w - 2;
        C2D_DrawRectSolid(cursor_x, y + 7, 0, 4, 14, m_colors.cursor_outline);

        // Value text
        C2D_TextParse(&text, buf, value.c_str());
        C2D_DrawText(&text, C2D_WithColor, bar_x + bar_w + 5, y + 4, 0, 0.5f, 0.5f,
                     value_color);

      } else if (i == ITEM_SATURATION || i == ITEM_BRIGHTNESS) {
        // Saturation / Brightness slider display
        C2D_TextParse(&text, buf, label.c_str());
        C2D_DrawText(&text, C2D_WithColor, 8, y + 4, 0, 0.5f, 0.5f, label_color);

        float bar_x = 100, bar_w = 180;
        float cur_val = (i == ITEM_SATURATION)
            ? editing_config_.accent_saturation
            : editing_config_.accent_brightness;
        int hue = editing_config_.accent_hue;

        // Gradient bar (8 segments)
        for (int seg = 0; seg < 8; seg++) {
          float sx = bar_x + seg * (bar_w / 8);
          float sw = bar_w / 8;
          float t = ((float)seg + 0.5f) / 8.0f;
          u32 seg_color;
          if (i == ITEM_SATURATION) {
            seg_color = hsv_to_color32(hue, t, editing_config_.accent_brightness);
          } else {
            seg_color = hsv_to_color32(hue, editing_config_.accent_saturation, t);
          }
          C2D_DrawRectSolid(sx, y + 10, 0, sw, 8, seg_color);
        }

        // Cursor position
        float cursor_x = bar_x + cur_val * bar_w - 2;
        C2D_DrawRectSolid(cursor_x, y + 7, 0, 4, 14, m_colors.cursor_outline);

        // Value text
        C2D_TextParse(&text, buf, value.c_str());
        C2D_DrawText(&text, C2D_WithColor, bar_x + bar_w + 5, y + 4, 0, 0.5f, 0.5f,
                     value_color);

      } else if (i == ITEM_DPAD_SPEED) {
        // D-Pad Speed slider
        C2D_TextParse(&text, buf, get_item_label(i).c_str());
        C2D_DrawText(&text, C2D_WithColor, 8, y + 4, 0, 0.5f, 0.5f, label_color);

        float bar_x = 100, bar_w = 180;
        // Bar background
        C2D_DrawRectSolid(bar_x, y + 10, 0, bar_w, 8, m_colors.text_dim);
        // 10 tick marks
        for (int s = 1; s <= 10; s++) {
          float tx = bar_x + (float)(s - 1) / 9.0f * bar_w;
          C2D_DrawRectSolid(tx - 1, y + 8, 0, 2, 12, m_colors.bg_bottom);
        }
        // Cursor
        float cursor_x = bar_x + (float)(editing_config_.dpad_speed - 1) / 9.0f * bar_w - 2;
        C2D_DrawRectSolid(cursor_x, y + 7, 0, 4, 14, m_colors.cursor_outline);
        // Value
        C2D_TextParse(&text, buf, get_item_value(i).c_str());
        C2D_DrawText(&text, C2D_WithColor, bar_x + bar_w + 5, y + 4, 0, 0.5f, 0.5f, value_color);

      } else if (i == ITEM_COLOR) {
        // With color preview
        C2D_TextParse(&text, buf, label.c_str());
        C2D_DrawText(&text, C2D_WithColor, 8, y + 4, 0, 0.5f, 0.5f, label_color);

        // Color preview rect
        C2D_DrawRectSolid(100, y + 4, 0, 20, 18, preview_colors_.accent);

        C2D_TextParse(&text, buf, value.c_str());
        C2D_DrawText(&text, C2D_WithColor, 128, y + 4, 0, 0.5f, 0.5f, value_color);
      } else {
        // Normal label: [value] display
        C2D_TextParse(&text, buf, label.c_str());
        C2D_DrawText(&text, C2D_WithColor, 8, y + 4, 0, 0.5f, 0.5f, label_color);

        std::string disp = "[ " + value + " ]";
        C2D_TextParse(&text, buf, disp.c_str());
        C2D_DrawText(&text, C2D_WithColor, 160, y + 4, 0, 0.5f, 0.5f, value_color);
      }

      y += item_h;
    }

    // Pinned Save button (always visible regardless of scroll content)
    {
      bool save_sel = (selected_item_ == ITEM_SAVE);
      if (save_sel) {
        C2D_DrawRectSolid(0, SAVE_Y, 0, 320, 28, m_colors.accent);
      }
      u32 save_color = save_sel ? m_colors.accent_text : m_colors.text_body;
      C2D_TextParse(&text, buf, "[ Save & Back ]");
      float tw = 0, th = 0;
      C2D_TextGetDimensions(&text, 0.55f, 0.55f, &tw, &th);
      C2D_DrawText(&text, C2D_WithColor, (320 - tw) / 2, SAVE_Y + 4, 0, 0.55f, 0.55f, save_color);
    }

    draw_menu_button(ctx, ui_mgr);

    // Footer
    if (in_edit_mode_ && (selected_item_ == ITEM_HUE || selected_item_ == ITEM_SATURATION || selected_item_ == ITEM_BRIGHTNESS)) {
      C2D_TextParse(&text, buf, "Drag slider or D-Pad  Tap outside to confirm");
      C2D_DrawText(&text, C2D_WithColor, 50, 224, 0, 0.4f, 0.4f, m_colors.warn);
    } else {
      const char* hint = (last_tapped_item_ >= 0)
          ? "Tap again to execute  B: Cancel"
          : "Tap twice to edit  A: Quick edit  B: Back";
      C2D_TextParse(&text, buf, hint);
      C2D_DrawText(&text, C2D_WithColor, 50, 224, 0, 0.4f, 0.4f, m_colors.text_dim);
    }
  }
}

std::vector<std::string> SettingsScreen::list_wallpapers() const {
  std::vector<std::string> files;
  mkdir("sdmc:/3ds/StreaMu", 0777);
  mkdir(WALLPAPER_DIR, 0777);
  DIR* dir = opendir(WALLPAPER_DIR);
  if (!dir) return files;
  struct dirent* ent;
  while ((ent = readdir(dir)) != nullptr) {
    std::string name = ent->d_name;
    if (name.size() > 4) {
      std::string ext = name.substr(name.size() - 4);
      if (ext == ".png" || ext == ".PNG") {
        files.push_back(name);
      }
    }
  }
  closedir(dir);
  return files;
}

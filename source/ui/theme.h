#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>

// === Theme Mode ===
enum ThemeMode { THEME_LIGHT = 0, THEME_DARK = 1 };

// === L/R Button Behavior ===
enum LRAction { LR_DISABLED = 0, LR_SKIP = 1, LR_PLAY_PAUSE = 2 };

// === Hamburger Menu Button Position ===
enum MenuButtonSide { MENU_BTN_LEFT = 0, MENU_BTN_RIGHT = 1 };

// === Application Config ===
struct AppConfig {
  ThemeMode mode = THEME_LIGHT;
  int accent_hue = 220;  // 0-360 (default: blue)
  int palette_index = 0; // Palette index (-1 = custom hue)
  LRAction l_action = LR_SKIP;
  LRAction r_action = LR_SKIP;
  MenuButtonSide menu_btn_side = MENU_BTN_LEFT; // For future settings (not yet implemented)
  int dpad_speed = 5; // 1-10 (1=slowest, 5=normal, 10=fastest)
  std::string quick_access_ids = ""; // Comma-separated PL IDs (max 4)
  std::string wallpaper_file = "";  // Wallpaper filename (in wallpaper/ folder)
  std::string server_ip = "";      // Server IP:Port (e.g. "192.168.1.100:8080")
  std::string language = "en";     // Metadata language ("en" or "ja")
};

// === Theme Colors (changeable at runtime) ===
struct ThemeColors {
  // Background
  u32 bg_top;
  u32 bg_bottom;

  // Text
  u32 text_body;
  u32 text_dim;
  u32 text_version;

  // Accent (theme color dependent)
  u32 accent;
  u32 accent_text;
  u32 playing_bg;
  u32 playing_text;
  u32 title;
  u32 warn;

  // Popup
  u32 popup_bg;
  u32 popup_header;
  u32 popup_text;
  u32 popup_title;
  u32 overlay;

  // Other
  u32 empty_text;
  u32 cursor_outline;     // Cursor outline color (light:black, dark:white)
  u32 button_bg;          // TouchButton default background
  u32 settings_button_bg; // Settings button background
  u32 settings_button_text;
};

// === Color Palette (8 cols x 5 rows) ===
// Stores hue values only. Converted to RGB with S=0.75, V=0.78
static const int COLOR_PALETTE_COLS = 8;
static const int COLOR_PALETTE_ROWS = 5;
static const int COLOR_PALETTE[COLOR_PALETTE_ROWS][COLOR_PALETTE_COLS] = {
    //  Red     Orange   Yellow  Lime   Green   Cyan    Blue   Indigo
    {0, 30, 55, 90, 130, 170, 210, 250},
    //  Lavender Purple Magenta  Pink  Salmon  Coral    Sky   Mint
    {270, 290, 315, 340, 10, 20, 195, 155},
    //  Dark variant (high saturation, low brightness)
    {0, 30, 55, 90, 130, 170, 210, 250},
    //  Pastel variant (low saturation, high brightness)
    {0, 30, 55, 90, 130, 170, 210, 250},
    //  Grayscale (hue=0, S=0, V varies by column)
    {0, 0, 0, 0, 0, 0, 0, 0},
};

// SV values per palette row
struct PaletteSV {
  float s;
  float v;
};
static const PaletteSV PALETTE_ROW_SV[COLOR_PALETTE_ROWS] = {
    {0.75f, 0.78f}, // Row 0: Standard
    {0.75f, 0.78f}, // Row 1: Standard (different hues)
    {0.85f, 0.55f}, // Row 2: Dark
    {0.40f, 0.90f}, // Row 3: Pastel
    {0.00f, 0.00f}, // Row 4: Grayscale (V computed specially)
};

// === HSV to RGB conversion ===
// h: 0-360, s: 0.0-1.0, v: 0.0-1.0
inline u32 hsv_to_color32(int h, float s, float v) {
  h = ((h % 360) + 360) % 360;
  float c = v * s;
  (void)c; // used conceptually; actual conversion uses p/q/t below

  // Standard HSV conversion
  float hf = (float)h / 60.0f;
  int hi = (int)hf;
  float f = hf - (float)hi;
  float p = v * (1.0f - s);
  float q = v * (1.0f - s * f);
  float t = v * (1.0f - s * (1.0f - f));

  float r = 0, g = 0, b = 0;
  switch (hi % 6) {
  case 0:
    r = v;
    g = t;
    b = p;
    break;
  case 1:
    r = q;
    g = v;
    b = p;
    break;
  case 2:
    r = p;
    g = v;
    b = t;
    break;
  case 3:
    r = p;
    g = q;
    b = v;
    break;
  case 4:
    r = t;
    g = p;
    b = v;
    break;
  case 5:
    r = v;
    g = p;
    b = q;
    break;
  }
  return C2D_Color32((u8)(r * 255), (u8)(g * 255), (u8)(b * 255), 255);
}

// Get hue value from palette index
inline int palette_get_hue(int row, int col) {
  if (row < 0 || row >= COLOR_PALETTE_ROWS)
    row = 0;
  if (col < 0 || col >= COLOR_PALETTE_COLS)
    col = 0;
  return COLOR_PALETTE[row][col];
}

// Map column to V (brightness) for grayscale
inline float palette_get_gray_v(int col) {
  // 0:black, 1:dark gray, ..., 7:white
  return (float)col / 7.0f;
}

// Get RGB color from palette index
inline u32 palette_get_color(int row, int col) {
  int hue = palette_get_hue(row, col);
  PaletteSV sv = PALETTE_ROW_SV[(row >= 0 && row < COLOR_PALETTE_ROWS) ? row : 0];
  float s = sv.s;
  float v = sv.v;
  if (row == 4) {
    s = 0.0f;
    v = palette_get_gray_v(col);
  }
  return hsv_to_color32(hue, s, v);
}

// === Apply Theme ===
inline void apply_theme(const AppConfig &cfg, ThemeColors &colors) {
  int hue = cfg.accent_hue;
  float s = 0.75f;
  float v = 0.78f;

  if (cfg.palette_index >= 0) {
    int row = cfg.palette_index / COLOR_PALETTE_COLS;
    int col = cfg.palette_index % COLOR_PALETTE_COLS;
    if (row < COLOR_PALETTE_ROWS) {
      hue = palette_get_hue(row, col);
      s = PALETTE_ROW_SV[row].s;
      v = PALETTE_ROW_SV[row].v;
      if (row == 4) { // Grayscale
        s = 0.0f;
        v = palette_get_gray_v(col);
      }
    }
  }

  // Accent color (theme color)
  colors.accent = hsv_to_color32(hue, s, v);
  
  // Text color defaults to black (UI text etc.)
  colors.accent_text = C2D_Color32(30, 30, 30, 255);
  // Use white text for the two darkest palette colors (Row 4, Col 0/1) for readability
  if (cfg.palette_index >= 0) {
    int row = cfg.palette_index / COLOR_PALETTE_COLS;
    int col = cfg.palette_index % COLOR_PALETTE_COLS;
    if (row == 4 && col <= 1) {
      colors.accent_text = C2D_Color32(255, 255, 255, 255);
    }
  }

  float play_s = s * 0.9f;
  float play_v = v * 0.7f;
  if(play_s > 1.0f) play_s = 1.0f;
  if(play_v > 1.0f) play_v = 1.0f;
  colors.playing_bg = hsv_to_color32(hue, play_s, play_v);
  
  float ptxt_s = s * 0.4f;
  float ptxt_v = v * 1.1f;
  if(ptxt_v > 1.0f) ptxt_v = 1.0f;
  if(ptxt_s > 1.0f) ptxt_s = 1.0f;
  colors.playing_text = hsv_to_color32(hue, ptxt_s, ptxt_v);
  
  float cursor_s = s * 1.1f;
  float cursor_v = v * 0.6f;
  if (cursor_s > 1.0f) cursor_s = 1.0f;
  colors.cursor_outline = hsv_to_color32(hue, cursor_s, cursor_v);

  colors.warn = C2D_Color32(255, 80, 80, 255);
  colors.overlay = C2D_Color32(0, 0, 0, 180);
  colors.empty_text = C2D_Color32(80, 80, 80, 255);

  // Button colors
  colors.button_bg = colors.accent;
  colors.settings_button_bg = C2D_Color32(150, 150, 150, 255);
  colors.settings_button_text = C2D_Color32(255, 255, 255, 255);

  // Mode-dependent colors (no accent mixed into background)
  if (cfg.mode == THEME_DARK) {
    colors.bg_top = C2D_Color32(30, 30, 30, 255);
    colors.bg_bottom = C2D_Color32(30, 30, 30, 255);
    colors.text_body = C2D_Color32(220, 220, 220, 255);
    colors.text_dim = C2D_Color32(150, 150, 150, 255);
    colors.text_version = C2D_Color32(120, 120, 120, 255);
    colors.popup_bg = C2D_Color32(50, 50, 50, 255);
    colors.popup_header = C2D_Color32(65, 65, 65, 255);
    colors.popup_text = C2D_Color32(220, 220, 220, 255);
    colors.popup_title = C2D_Color32(240, 240, 240, 255);
    colors.title = colors.text_body; // Sync title with dark/light mode
  } else {
    // Light mode background (pure white or very light gray)
    colors.bg_top = C2D_Color32(245, 245, 245, 255);
    colors.bg_bottom = C2D_Color32(245, 245, 245, 255);
    colors.text_body = C2D_Color32(30, 30, 30, 255);
    colors.text_dim = C2D_Color32(120, 120, 120, 255);
    colors.text_version = C2D_Color32(150, 150, 150, 255);
    colors.popup_bg = C2D_Color32(240, 240, 245, 255);
    colors.popup_header = C2D_Color32(200, 200, 210, 255);
    colors.popup_text = C2D_Color32(30, 30, 30, 255);
    colors.popup_title = C2D_Color32(10, 10, 10, 255);
    colors.title = colors.text_body; // Sync title with dark/light mode
  }
}

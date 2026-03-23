#pragma once
#include "../../config_manager.h"
#include "../screen.h"
#include "../theme.h"
#include "../touch_button.h"
#include "../wallpaper.h"
#include <string>
#include <vector>

class SettingsScreen : public Screen {
public:
  SettingsScreen(ThemeColors &colors, Wallpaper* wallpaper);
  void on_enter(AppContext &ctx) override;
  std::string update(AppContext &ctx, u32 kDown, u32 kHeld, u32 kRepeat) override;
  void draw_top(const RenderContext &ctx, UIManager &ui_mgr) override;
  void draw_bottom(const RenderContext &ctx, UIManager &ui_mgr) override;

private:
  ThemeColors &m_colors;       // Ref to actual theme colors
  ThemeColors preview_colors_; // Independent preview colors
  AppConfig editing_config_;   // Working copy of config
  Wallpaper* m_wallpaper = nullptr; // Wallpaper manager (externally owned)
  int selected_item_ = 0;
  int last_tapped_item_ = -1;  // Two-tap: previously tapped item
  bool in_palette_mode_ = false;
  bool in_edit_mode_ = false;  // HUE slider edit state only
  bool in_wallpaper_mode_ = false; // Wallpaper selection mode
  int palette_row_ = 0;
  int palette_col_ = 0;
  std::vector<std::string> wallpaper_files_; // PNG files in folder
  int wallpaper_selected_ = 0;
  float scroll_offset_ = 0.0f; // Normal mode scroll offset (px)

  static const int ITEM_COUNT = 11; // Mode, Color, Hue, L, R, DpadSpeed, Wallpaper, ServerIP, Language, (separator), Save

  void apply_preview(); // Apply editing config to preview immediately
  std::string get_item_label(int index) const;
  std::string get_item_value(int index) const;
  std::string get_item_description(int index) const;
  std::string lr_action_name(LRAction action) const;
  std::vector<std::string> list_wallpapers() const;
};

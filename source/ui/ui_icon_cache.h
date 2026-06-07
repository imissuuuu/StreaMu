#pragma once

#include <3ds.h>

enum class UiIconId {
  Loop = 0,
  Prev,
  Play,
  Pause,
  Next,
  Shuffle,
  Home,
  Menu,
};

bool init_ui_icon_cache();
void cleanup_ui_icon_cache();
void draw_ui_icon(UiIconId id, float x, float y, u32 color);
void draw_ui_icon_centered(UiIconId id, float center_x, float center_y,
                           u32 color);
void draw_ui_separator(float x, float y, float width, u32 color);

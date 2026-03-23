#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>
#include <vector>

// Cached text object
struct CachedText {
  C2D_Text text;
  bool valid = false;

  void parse_and_optimize(C2D_TextBuf buf, const std::string &str) {
    if (str.empty()) {
      valid = false;
      return;
    }
    C2D_TextParse(&text, buf, str.c_str());
    C2D_TextOptimize(&text);
    valid = true;
  }

  void draw(float x, float y, float scale, u32 color) const {
    if (valid) {
      C2D_DrawText(&text, C2D_WithColor, x, y, 0, scale, scale, color);
    }
  }
};

class UIManager {
public:
  UIManager();
  ~UIManager();

  void init();
  void cleanup();

  // Clear screen buffer and begin drawing
  void begin_top_screen(u32 bg_color);
  void begin_bottom_screen(u32 bg_color);
  void end_frame();

  // Shared text buffer
  C2D_TextBuf get_text_buf() { return m_textBuf; }
  void clear_text_buf() { C2D_TextBufClear(m_textBuf); }

private:
  C3D_RenderTarget *m_topScreen;
  C3D_RenderTarget *m_bottomScreen;
  C2D_TextBuf m_textBuf;
  bool m_cleaned_up = false;
};

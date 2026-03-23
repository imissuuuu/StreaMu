#include "ui_manager.h"

UIManager::UIManager()
    : m_topScreen(nullptr), m_bottomScreen(nullptr), m_textBuf(nullptr) {}

UIManager::~UIManager() { cleanup(); }

void UIManager::init() {
  gfxInitDefault();
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
  C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
  C2D_Prepare();

  m_topScreen = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
  m_bottomScreen = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
  m_textBuf = C2D_TextBufNew(8192); // Large enough for text cache
}

void UIManager::cleanup() {
  if (m_cleaned_up)
    return;

  if (m_textBuf) {
    C2D_TextBufDelete(m_textBuf);
    m_textBuf = nullptr;
  }
  C2D_Fini();
  C3D_Fini();
  gfxExit();

  m_cleaned_up = true;
}

void UIManager::begin_top_screen(u32 bg_color) {
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  clear_text_buf();
  C2D_TargetClear(m_topScreen, bg_color);
  C2D_SceneBegin(m_topScreen);
}

void UIManager::begin_bottom_screen(u32 bg_color) {
  C2D_TargetClear(m_bottomScreen, bg_color);
  C2D_SceneBegin(m_bottomScreen);
}

void UIManager::end_frame() { C3D_FrameEnd(0); }

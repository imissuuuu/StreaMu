#pragma once
#include <3ds.h>
#include <citro2d.h>
#include <string>
#include "../app_state.h"
#include "../audio/player.h"
#include "../network/youtube_api.h"

void draw_top_screen(const AppState& state, const AudioPlayer& player);
void draw_bottom_screen(const AppState& state, const AudioPlayer& player);
void handle_search_input(AppState& state, u32 keys, touchPosition touch, YouTubeAPI& api);
void handle_results_input(AppState& state, u32 keys, touchPosition touch);
void handle_player_input(AppState& state, u32 keys, touchPosition touch, AudioPlayer& player);
void handle_playlist_input(AppState& state, u32 keys, touchPosition touch);
std::string format_time(int seconds);
void draw_seek_bar(float x, float y, float w, float h, float progress);
void draw_scrolling_text(const std::string& text, float x, float y, float max_width);
#pragma once
enum class Screen { SEARCH, RESULTS, PLAYER, PLAYLIST };
struct AppState {
    Screen current_screen;
    bool is_playing;
};
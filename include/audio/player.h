#pragma once
#include <3ds.h>
#include <string>

enum class PlayState { STOPPED, LOADING, PLAYING, PAUSED, ERROR };

class AudioPlayer {
public:
    void init(); void cleanup();
    void play(const std::string& stream_url);
    void pause(); void resume(); void stop();
    void update(); void set_volume(float vol);
    float get_progress() const; int get_elapsed() const; int get_duration() const;
    void decode_and_play(const std::string& stream_url);
    void fill_buffer(int buf_idx, const u8* data, size_t size);
private:
    PlayState state_;
    float volume_;
    bool stop_flag_;
    u8* audio_buf_[2];
    ndspWaveBuf wave_buf_[2];
    int DSP_CHANNEL = 0;
    size_t BUFFER_SIZE = 4096 * 4;
};
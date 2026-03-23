#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H

#include <3ds.h>
#include <vector>
#include <stdint.h>
#include "../../include/minimp3.h"

class MP3Player {
public:
    MP3Player();
    ~MP3Player();
    void init();
    void update(); 
    void stop();
    bool is_track_finished() const;
    void set_downloading_status(bool downloading);

    static bool is_playing;

private:
    mp3dec_t mp3d;
    ndspWaveBuf waveBuf[8]; // Increased from 2 to 8 buffers
    int16_t* audioBuffer;
    bool m_is_downloading = false;
};

#endif
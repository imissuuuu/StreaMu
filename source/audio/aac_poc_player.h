#ifndef AAC_POC_PLAYER_H
#define AAC_POC_PLAYER_H

#include <3ds.h>
#include <stdint.h>
#include <vector>

#include "aac_adts_stream_decoder.h"

class AacPocPlayer {
public:
    AacPocPlayer();
    ~AacPocPlayer();
    bool init();
    void update();
    void stop();
    bool is_track_finished() const;
    bool has_started_playing() const;
    void set_downloading_status(bool downloading);

    static bool is_playing;

private:
    AacAdtsStreamDecoder decoder_;
    ndspWaveBuf waveBuf[8];
    int16_t* audioBuffer;
    bool m_is_downloading = false;
    size_t read_offset_ = 0;
};

#endif

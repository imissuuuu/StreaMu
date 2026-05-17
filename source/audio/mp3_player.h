#ifndef MP3_PLAYER_H
#define MP3_PLAYER_H

#include "minimp3_stream_decoder.h"
#include <3ds.h>
#include <stdint.h>
#include <vector>

class MP3Player {
public:
  MP3Player();
  ~MP3Player();
  void init();
  void update();
  void stop();
  bool is_track_finished() const;
  bool has_started_playing() const;
  void set_downloading_status(bool downloading);

  static bool is_playing;

private:
  Minimp3StreamDecoder decoder_;
  ndspWaveBuf waveBuf[8]; // Increased from 2 to 8 buffers
  int16_t *audioBuffer;
  bool m_is_downloading = false;
  size_t read_offset_ = 0;
};

#endif

#ifndef VORBIS_POC_PLAYER_H
#define VORBIS_POC_PLAYER_H

#include <3ds.h>
#include <stdint.h>

#include "vorbis_stream_decoder.h"

class VorbisPocPlayer {
public:
  VorbisPocPlayer();
  ~VorbisPocPlayer();
  bool init();
  void update();
  void stop();
  bool is_track_finished() const;
  bool has_started_playing() const;
  void set_downloading_status(bool downloading);

  static bool is_playing;

private:
  VorbisStreamDecoder decoder_;
  ndspWaveBuf waveBuf[8];
  int16_t *audioBuffer;
  bool m_is_downloading = false;
  size_t read_offset_ = 0;
};

#endif

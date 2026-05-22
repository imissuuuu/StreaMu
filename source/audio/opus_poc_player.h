#ifndef OPUS_POC_PLAYER_H
#define OPUS_POC_PLAYER_H

#include <3ds.h>
#include <stdint.h>
#include <vector>

#include "opus_memory_decoder.h"

class OpusPocPlayer {
public:
  OpusPocPlayer();
  ~OpusPocPlayer();
  bool init();
  bool start(const uint8_t *data, size_t size);
  bool start_streaming(const std::vector<uint8_t> *buffer, LightLock *lock,
                       const bool *download_complete);
  void update();
  void stop();
  bool is_track_finished() const;
  bool has_started_playing() const;
  bool has_decode_failed() const;
  bool is_decoder_open() const;

  static bool is_playing;

private:
  OpusMemoryDecoder decoder_;
  ndspWaveBuf waveBuf[8];
  int16_t *audioBuffer;
  bool decode_failed_;
};

#endif

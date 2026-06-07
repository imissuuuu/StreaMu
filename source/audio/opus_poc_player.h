#ifndef OPUS_POC_PLAYER_H
#define OPUS_POC_PLAYER_H

#include <3ds.h>
#include <stdint.h>
#include <vector>

#include "opus_decode_tuning.h"
#include "opus_memory_decoder.h"
#include "webm_opus_streaming_decoder.h"

struct OpusPlayerUpdateStats {
  int decoded_buffers = 0;
  bool hit_decode_failure = false;
  u64 decode_ticks = 0;
};

enum class OpusInputKind {
  None,
  OggBytes,
  OggStream,
  WebmStream,
};

class OpusPocPlayer {
public:
  OpusPocPlayer();
  ~OpusPocPlayer();
  bool init();
  bool start(const uint8_t *data, size_t size);
  bool start_streaming(const std::vector<uint8_t> *buffer, LightLock *lock,
                       const bool *download_complete);
  bool start_webm_streaming(const std::vector<uint8_t> *buffer, LightLock *lock,
                            const bool *download_complete);
  void set_decode_tuning(const OpusDecodeTuning &tuning);
  void update();
  OpusPlayerUpdateStats update_with_stats();
  void stop();
  bool is_track_finished() const;
  bool has_started_playing() const;
  bool has_decode_failed() const;
  WebmRemuxError webm_remux_error() const;
  int queued_wavebuf_count() const;
  int free_wavebuf_count() const;

  static bool is_playing;

private:
  OpusMemoryDecoder decoder_;
  WebmOpusStreamingDecoder webm_decoder_;
  OpusInputKind input_kind_;
  ndspWaveBuf waveBuf[8];
  int16_t *audioBuffer;
  bool decode_failed_;
  bool ndsp_format_initialized_;
  OpusDecodeTuning decode_tuning_;
};

#endif

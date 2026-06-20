#ifndef OPUS_POC_PLAYER_H
#define OPUS_POC_PLAYER_H

#include <3ds.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "opus_decode_tuning.h"
#include "opus_memory_decoder.h"
#include "webm_pcm_decode_worker.h"

struct OpusPlayerUpdateStats {
  int decoded_buffers = 0;
  bool hit_decode_failure = false;
  u64 decode_ticks = 0;
  int queued_before_update = 0;
  int queued_after_update = 0;
  int free_before_update = 0;
  int free_after_update = 0;
  int target_queued_wavebufs = 0;
  int max_decode_buffers = 0;
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
  bool start_webm_streaming(std::vector<uint8_t> *buffer, LightLock *lock,
                            bool *download_complete,
                            const WebmSeekExecutionContext &seek_context,
                            int emit_start_ms, bool enable_parser_seek,
                            bool prefer_offset_seek,
                            const std::string &range_probe_base_url,
                            uint64_t range_filesize,
                            uint64_t parser_prefetch_offset);
  void set_decode_tuning(const OpusDecodeTuning &tuning);
  void update();
  OpusPlayerUpdateStats update_with_stats();
  void stop();
  bool is_track_finished() const;
  bool has_started_playing() const;
  bool has_decode_failed() const;
  WebmRemuxError webm_remux_error() const;
  bool get_webm_last_seek_runtime_point(uint64_t *out_start_byte,
                                        int *out_timecode_ms) const;
  int queued_wavebuf_count() const;
  int free_wavebuf_count() const;

  static bool is_playing;

private:
  bool submit_decoded_pcm_to_wavebuf(int wavebuf_index,
                                     const OpusDecodeResult &decoded);

  OpusMemoryDecoder decoder_;
  WebmPcmDecodeWorker webm_worker_;
  OpusInputKind input_kind_;
  ndspWaveBuf waveBuf[8];
  int16_t *audioBuffer;
  bool decode_failed_;
  bool ndsp_format_initialized_;
  OpusDecodeTuning decode_tuning_;
};

#endif

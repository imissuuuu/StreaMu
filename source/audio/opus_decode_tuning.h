#ifndef OPUS_DECODE_TUNING_H
#define OPUS_DECODE_TUNING_H

struct OpusDecodeTuning {
  int steady_target_queued_wavebufs = 7;
  int steady_max_decode_buffers_per_update = 2;
  int refill_target_queued_wavebufs = 8;
  int refill_decode_buffers_per_update = 6;
  int low_queue_wavebuf_threshold = 4;
};

inline OpusDecodeTuning default_opus_decode_tuning() {
  return OpusDecodeTuning{};
}

inline OpusDecodeTuning webm_startup_decode_tuning() {
  OpusDecodeTuning tuning = {};
  tuning.steady_target_queued_wavebufs = 6;
  tuning.steady_max_decode_buffers_per_update = 3;
  tuning.refill_target_queued_wavebufs = 4;
  tuning.refill_decode_buffers_per_update = 4;
  tuning.low_queue_wavebuf_threshold = 3;
  return tuning;
}

inline OpusDecodeTuning webm_steady_decode_tuning() {
  OpusDecodeTuning tuning = {};
  tuning.steady_target_queued_wavebufs = 7;
  tuning.steady_max_decode_buffers_per_update = 2;
  tuning.refill_target_queued_wavebufs = 6;
  tuning.refill_decode_buffers_per_update = 4;
  tuning.low_queue_wavebuf_threshold = 3;
  return tuning;
}

#endif

#ifndef OPUS_STREAM_PIPELINE_H
#define OPUS_STREAM_PIPELINE_H

#include <3ds.h>
#include <stddef.h>

#include "opus_poc_player.h"
#include "playback_observer.h"

struct OpusPipelineState {
  size_t stream_buffer_bytes;
  bool download_complete;
  int queued_wavebufs;
  int free_wavebufs;
};

struct PlaybackCoreSnapshot {
  size_t stream_buffer_bytes = 0;
  bool download_complete = false;
  int queued_wavebufs = 0;
  int free_wavebufs = 0;
  u64 heap_free_bytes = 0;
  u32 linear_free_bytes = 0;
};

OpusPipelineState collect_opus_pipeline_state();
PlaybackCoreSnapshot collect_playback_core_snapshot();
PlaybackCompareSnapshot collect_playback_compare_snapshot(
    StreamContainerMode stream_mode,
    const OpusPlayerUpdateStats &update_stats);

#endif

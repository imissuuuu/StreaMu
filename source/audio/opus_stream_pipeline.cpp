#include "opus_stream_pipeline.h"

#include "device_profile.h"
#include "opus_poc_player.h"

#include <3ds/allocator/linear.h>
#include <3ds/env.h>
#include <malloc.h>
#include <memory>
#include <stdint.h>
#include <vector>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;
extern bool g_stream_download_complete;
extern std::unique_ptr<OpusPocPlayer> g_opus_player_ptr;

OpusPipelineState collect_opus_pipeline_state() {
  OpusPipelineState state = {};

  LightLock_Lock(&stream_lock);
  state.stream_buffer_bytes =
      g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
  state.download_complete = g_stream_download_complete;
  LightLock_Unlock(&stream_lock);

  if (g_opus_player_ptr) {
    state.queued_wavebufs = g_opus_player_ptr->queued_wavebuf_count();
    state.free_wavebufs = g_opus_player_ptr->free_wavebuf_count();
  }

  return state;
}

PlaybackCoreSnapshot collect_playback_core_snapshot() {
  const OpusPipelineState pipeline_state = collect_opus_pipeline_state();
  const struct mallinfo heap_info = mallinfo();

  PlaybackCoreSnapshot snapshot = {};
  snapshot.stream_buffer_bytes = pipeline_state.stream_buffer_bytes;
  snapshot.download_complete = pipeline_state.download_complete;
  snapshot.queued_wavebufs = pipeline_state.queued_wavebufs;
  snapshot.free_wavebufs = pipeline_state.free_wavebufs;
  snapshot.heap_free_bytes = heap_info.fordblks;
  snapshot.linear_free_bytes = linearSpaceFree();
  return snapshot;
}

PlaybackCompareSnapshot collect_playback_compare_snapshot(
    StreamContainerMode stream_mode,
    const OpusPlayerUpdateStats &update_stats) {
  const OpusPipelineState pipeline_state = collect_opus_pipeline_state();
  const struct mallinfo heap_info = mallinfo();

  PlaybackCompareSnapshot snapshot = {};
  snapshot.stream_mode = stream_mode;
  snapshot.is_old3ds_baseline = is_old3ds_baseline_device();
  snapshot.download_complete = pipeline_state.download_complete;
  snapshot.stream_buffer_bytes = pipeline_state.stream_buffer_bytes;
  snapshot.queued_wavebufs = pipeline_state.queued_wavebufs;
  snapshot.free_wavebufs = pipeline_state.free_wavebufs;
  snapshot.decoded_buffers_in_update = update_stats.decoded_buffers;
  snapshot.update_decode_ticks = update_stats.decode_ticks;
  snapshot.app_heap_size_bytes = envGetHeapSize();
  snapshot.app_heap_used_bytes = heap_info.uordblks;
  snapshot.app_heap_free_bytes = heap_info.fordblks;
  snapshot.linear_free_bytes = linearSpaceFree();
  return snapshot;
}

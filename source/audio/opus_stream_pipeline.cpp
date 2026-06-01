#include "opus_stream_pipeline.h"

#include "opus_poc_player.h"

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

#ifndef PLAYBACK_OBSERVER_H
#define PLAYBACK_OBSERVER_H

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

#include "network/youtube_api.h"

enum class PlaybackCompareEvent {
  RequestStart,
  FirstByte,
  DecoderOpenStart,
  DecoderOpenOk,
  DecoderOpenFailed,
  FirstPcmQueued,
  AudioAudible,
  ThumbnailFetchStart,
  ThumbnailFetchDone,
  ThumbnailUploadDone,
  StreamComplete,
  StreamCancelled,
  StreamFailed,
  DecodeFailure,
};

struct PlaybackCompareSnapshot {
  StreamContainerMode stream_mode = StreamContainerMode::ProxyOggOpus;
  bool is_old3ds_baseline = true;
  bool download_complete = false;
  size_t stream_buffer_bytes = 0;
  int queued_wavebufs = 0;
  int free_wavebufs = 0;
  int decoded_buffers_in_update = 0;
  u64 update_decode_ticks = 0;
  u64 app_heap_size_bytes = 0;
  u64 app_heap_used_bytes = 0;
  u64 app_heap_free_bytes = 0;
  u32 linear_free_bytes = 0;
};

void playback_observer_begin_session(StreamContainerMode stream_mode,
                                     const char *video_id);
void playback_observer_log(PlaybackCompareEvent event,
                           const PlaybackCompareSnapshot &snapshot);
void playback_observer_log_simple(PlaybackCompareEvent event,
                                  StreamContainerMode stream_mode,
                                  size_t stream_buffer_bytes);
void playback_observer_end_session();
const char *playback_compare_event_name(PlaybackCompareEvent event);
const char *playback_compare_mode_name(StreamContainerMode mode);

#endif

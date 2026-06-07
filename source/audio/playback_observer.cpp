#include "playback_observer.h"

#include "device_profile.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

struct PlaybackObserverSession {
  bool active = false;
  StreamContainerMode stream_mode = StreamContainerMode::ProxyOggOpus;
  u64 start_ms = 0;
  char video_id[32] = {0};
};

struct PlaybackObserverState {
  PlaybackObserverSession session;
  LightLock lock;

  PlaybackObserverState() : session(), lock() { LightLock_Init(&lock); }
};

PlaybackObserverState g_state;

static void ensure_streamu_data_dir() { mkdir("sdmc:/3ds/StreaMu", 0777); }

static FILE *open_compare_log() {
  ensure_streamu_data_dir();
  return fopen("sdmc:/3ds/StreaMu/playback_compare.log", "a");
}

} // namespace

const char *playback_compare_event_name(PlaybackCompareEvent event) {
  switch (event) {
    case PlaybackCompareEvent::RequestStart:
      return "request_start";
    case PlaybackCompareEvent::FirstByte:
      return "first_byte";
    case PlaybackCompareEvent::DecoderOpenStart:
      return "decoder_open_start";
    case PlaybackCompareEvent::DecoderOpenOk:
      return "decoder_open_ok";
    case PlaybackCompareEvent::DecoderOpenFailed:
      return "decoder_open_failed";
    case PlaybackCompareEvent::FirstPcmQueued:
      return "first_pcm_queued";
    case PlaybackCompareEvent::AudioAudible:
      return "audio_audible";
    case PlaybackCompareEvent::ThumbnailFetchStart:
      return "thumbnail_fetch_start";
    case PlaybackCompareEvent::ThumbnailFetchDone:
      return "thumbnail_fetch_done";
    case PlaybackCompareEvent::ThumbnailUploadDone:
      return "thumbnail_upload_done";
    case PlaybackCompareEvent::StreamComplete:
      return "stream_complete";
    case PlaybackCompareEvent::StreamCancelled:
      return "stream_cancelled";
    case PlaybackCompareEvent::StreamFailed:
      return "stream_failed";
    case PlaybackCompareEvent::DecodeFailure:
      return "decode_failure";
  }
  return "unknown";
}

const char *playback_compare_mode_name(StreamContainerMode mode) {
  switch (mode) {
    case StreamContainerMode::ProxyWebmOpus:
      return "webm";
    case StreamContainerMode::ProxyOggOpus:
      return "ogg";
  }
  return "unknown";
}

void playback_observer_begin_session(StreamContainerMode stream_mode,
                                     const char *video_id) {
  LightLock_Lock(&g_state.lock);
  g_state.session.stream_mode = stream_mode;
  g_state.session.start_ms = osGetTime();
  memset(g_state.session.video_id, 0, sizeof(g_state.session.video_id));
  if (video_id && video_id[0] != '\0') {
    snprintf(g_state.session.video_id, sizeof(g_state.session.video_id), "%s",
             video_id);
  }
  g_state.session.active = true;
  LightLock_Unlock(&g_state.lock);
}

void playback_observer_log(PlaybackCompareEvent event,
                           const PlaybackCompareSnapshot &snapshot) {
  LightLock_Lock(&g_state.lock);
  const PlaybackObserverSession session = g_state.session;
  LightLock_Unlock(&g_state.lock);
  if (!session.active) {
    return;
  }
  FILE *f = open_compare_log();
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms = (now_ms >= session.start_ms) ? (now_ms - session.start_ms) : 0;
  fprintf(
      f,
      "[playback-compare] +%llums mode=%s event=%s video=%s old3ds=%d "
      "bytes=%lu queued=%d free=%d decoded=%d decode_ticks=%llu heap_free=%llu "
      "heap_used=%llu heap_size=%llu linear_free=%lu complete=%d\n",
      static_cast<unsigned long long>(elapsed_ms),
      playback_compare_mode_name(snapshot.stream_mode),
      playback_compare_event_name(event), session.video_id,
      snapshot.is_old3ds_baseline ? 1 : 0,
      static_cast<unsigned long>(snapshot.stream_buffer_bytes),
      snapshot.queued_wavebufs, snapshot.free_wavebufs,
      snapshot.decoded_buffers_in_update,
      static_cast<unsigned long long>(snapshot.update_decode_ticks),
      static_cast<unsigned long long>(snapshot.app_heap_free_bytes),
      static_cast<unsigned long long>(snapshot.app_heap_used_bytes),
      static_cast<unsigned long long>(snapshot.app_heap_size_bytes),
      static_cast<unsigned long>(snapshot.linear_free_bytes),
      snapshot.download_complete ? 1 : 0);
  fclose(f);
}

void playback_observer_log_simple(PlaybackCompareEvent event,
                                  StreamContainerMode stream_mode,
                                  size_t stream_buffer_bytes) {
  PlaybackCompareSnapshot snapshot = {};
  snapshot.stream_mode = stream_mode;
  snapshot.is_old3ds_baseline = is_old3ds_baseline_device();
  snapshot.stream_buffer_bytes = stream_buffer_bytes;
  playback_observer_log(event, snapshot);
}

void playback_observer_end_session() {
  LightLock_Lock(&g_state.lock);
  g_state.session.active = false;
  LightLock_Unlock(&g_state.lock);
}

#ifndef NON_AUDIO_INTERFERENCE_GATE_H
#define NON_AUDIO_INTERFERENCE_GATE_H

#include <stddef.h>

#include "webm_playback_controller.h"
#include "network/youtube_api.h"

struct NonAudioInterferenceInput {
  StreamContainerMode stream_mode = StreamContainerMode::ProxyOggOpus;
  WebmPlaybackStage webm_stage = WebmPlaybackStage::Idle;
  bool is_opus_direct = false;
  bool download_complete = false;
  size_t stream_buffer_bytes = 0;
  int queued_wavebufs = 0;
  bool audio_started = false;
};

bool should_defer_thumbnail_fetch(const NonAudioInterferenceInput &input);
bool should_defer_thumbnail_upload(const NonAudioInterferenceInput &input);

#endif

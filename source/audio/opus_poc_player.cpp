#include "opus_poc_player.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

bool OpusPocPlayer::is_playing = false;

static constexpr size_t OPUS_PCM_CAPACITY_SAMPLES = 8192;
static constexpr int OPUS_WAVE_BUF_COUNT = 8;
static constexpr u64 WEBM_DECODE_CALL_SLOW_MS = 100ULL;

static void append_webm_decode_call_log(
    u64 start_ms, int wavebuf_index, u64 duration_ms,
    const OpusDecodeResult &decoded, int queued_before_update,
    int queued_before_call, int queued_after_call, int free_before_update,
    int target_queued_wavebufs, int max_decode_buffers) {
  if (duration_ms < WEBM_DECODE_CALL_SLOW_MS) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (start_ms > 0 && now_ms >= start_ms) ? now_ms - start_ms : 0;
  fprintf(f,
          "[webm-decode-call] +%llums slow_decode_call index=%d "
          "duration_ms=%llu ok=%d eof=%d samples=%d channels=%d rate=%d "
          "queued_before_update=%d queued_before_call=%d queued_after_call=%d "
          "free_before_update=%d target=%d max_decode=%d\n",
          static_cast<unsigned long long>(elapsed_ms), wavebuf_index,
          static_cast<unsigned long long>(duration_ms), decoded.ok ? 1 : 0,
          decoded.eof ? 1 : 0, decoded.samples_per_channel, decoded.channels,
          decoded.sample_rate, queued_before_update, queued_before_call,
          queued_after_call, free_before_update, target_queued_wavebufs,
          max_decode_buffers);
  fclose(f);
}

OpusPocPlayer::OpusPocPlayer()
    : input_kind_(OpusInputKind::None), audioBuffer(NULL),
      decode_failed_(false), ndsp_format_initialized_(false),
      webm_decode_call_log_start_ms_(0),
      decode_tuning_(default_opus_decode_tuning()) {
  memset(waveBuf, 0, sizeof(waveBuf));
}

bool OpusPocPlayer::init() {
  decoder_.reset();
  decode_failed_ = false;
  audioBuffer = static_cast<int16_t *>(linearAlloc(
      OPUS_PCM_CAPACITY_SAMPLES * OPUS_WAVE_BUF_COUNT * sizeof(int16_t)));
  if (!audioBuffer) {
    return false;
  }

  ndspChnReset(0);
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  memset(waveBuf, 0, sizeof(waveBuf));
  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    waveBuf[i].data_vaddr = audioBuffer + (OPUS_PCM_CAPACITY_SAMPLES * i);
    waveBuf[i].status = NDSP_WBUF_FREE;
  }
  return true;
}

void OpusPocPlayer::set_decode_tuning(const OpusDecodeTuning &tuning) {
  decode_tuning_ = tuning;
}

bool OpusPocPlayer::start(const uint8_t *data, size_t size) {
  stop();
  decode_failed_ = false;
  ndsp_format_initialized_ = false;
  webm_decode_call_log_start_ms_ = 0;
  input_kind_ = OpusInputKind::OggBytes;
  if (!audioBuffer || !decoder_.open(data, size)) {
    decode_failed_ = true;
    input_kind_ = OpusInputKind::None;
    return false;
  }
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  ndspChnSetPaused(0, false);
  is_playing = true;
  return true;
}

bool OpusPocPlayer::start_streaming(const std::vector<uint8_t> *buffer,
                                    LightLock *lock,
                                    const bool *download_complete) {
  stop();
  decode_failed_ = false;
  ndsp_format_initialized_ = false;
  webm_decode_call_log_start_ms_ = 0;
  input_kind_ = OpusInputKind::OggStream;
  if (!audioBuffer ||
      !decoder_.open_streaming(buffer, lock, download_complete)) {
    decode_failed_ = true;
    input_kind_ = OpusInputKind::None;
    return false;
  }
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  ndspChnSetPaused(0, false);
  is_playing = true;
  return true;
}

bool OpusPocPlayer::start_webm_streaming(
    std::vector<uint8_t> *buffer, LightLock *lock, bool *download_complete,
    int seek_start_ms, int emit_start_ms, bool enable_parser_seek,
    bool prefer_offset_seek, const std::string &range_probe_base_url,
    uint64_t range_filesize, uint64_t parser_prefetch_offset) {
  stop();
  decode_failed_ = false;
  ndsp_format_initialized_ = false;
  webm_decode_call_log_start_ms_ = osGetTime();
  input_kind_ = OpusInputKind::WebmStream;
  if (!audioBuffer ||
      !webm_decoder_.open_streaming(
          buffer, lock, download_complete, seek_start_ms, emit_start_ms,
          enable_parser_seek, prefer_offset_seek, range_probe_base_url,
          range_filesize, parser_prefetch_offset)) {
    decode_failed_ = true;
    input_kind_ = OpusInputKind::None;
    webm_decode_call_log_start_ms_ = 0;
    return false;
  }
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  is_playing = true;
  return true;
}

void OpusPocPlayer::update() { (void)update_with_stats(); }

OpusPlayerUpdateStats OpusPocPlayer::update_with_stats() {
  OpusPlayerUpdateStats stats = {};
  const bool decoder_ready =
      (input_kind_ == OpusInputKind::WebmStream) ? true : decoder_.is_open();
  if (!is_playing || !audioBuffer || !decoder_ready) {
    return stats;
  }
  const u64 tick_start = svcGetSystemTick();

  const int queued_before_update = queued_wavebuf_count();
  const int free_before_update = free_wavebuf_count();
  const bool low_queue_refill =
      queued_before_update <= decode_tuning_.low_queue_wavebuf_threshold;
  int max_decode_buffers =
      low_queue_refill ? decode_tuning_.refill_decode_buffers_per_update
                       : decode_tuning_.steady_max_decode_buffers_per_update;
  int target_queued_wavebufs =
      low_queue_refill ? decode_tuning_.refill_target_queued_wavebufs
                       : decode_tuning_.steady_target_queued_wavebufs;
  if (input_kind_ == OpusInputKind::WebmStream && !has_started_playing()) {
    max_decode_buffers = decode_tuning_.prestart_max_decode_buffers_per_update;
    target_queued_wavebufs = decode_tuning_.prestart_target_queued_wavebufs;
  }
  stats.queued_before_update = queued_before_update;
  stats.free_before_update = free_before_update;
  stats.target_queued_wavebufs = target_queued_wavebufs;
  stats.max_decode_buffers = max_decode_buffers;

  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    if (stats.decoded_buffers >= max_decode_buffers ||
        queued_wavebuf_count() >= target_queued_wavebufs) {
      break;
    }
    if (waveBuf[i].status == NDSP_WBUF_DONE ||
        waveBuf[i].status == NDSP_WBUF_FREE) {
      const int queued_before_call = queued_wavebuf_count();
      const u64 decode_call_start_ms =
          (input_kind_ == OpusInputKind::WebmStream) ? osGetTime() : 0;
      OpusDecodeResult decoded =
          (input_kind_ == OpusInputKind::WebmStream)
              ? webm_decoder_.decode(
                    const_cast<int16_t *>(
                        static_cast<const int16_t *>(waveBuf[i].data_vaddr)),
                    OPUS_PCM_CAPACITY_SAMPLES)
              : decoder_.decode(
                    const_cast<int16_t *>(
                        static_cast<const int16_t *>(waveBuf[i].data_vaddr)),
                    OPUS_PCM_CAPACITY_SAMPLES);
      if (input_kind_ == OpusInputKind::WebmStream) {
        const u64 decode_call_end_ms = osGetTime();
        const u64 decode_call_duration_ms =
            decode_call_end_ms >= decode_call_start_ms
                ? decode_call_end_ms - decode_call_start_ms
                : 0;
        append_webm_decode_call_log(
            webm_decode_call_log_start_ms_, i, decode_call_duration_ms, decoded,
            queued_before_update, queued_before_call, queued_wavebuf_count(),
            free_before_update, target_queued_wavebufs, max_decode_buffers);
      }
      if (decoded.ok) {
        u16 format = (decoded.channels == 2) ? NDSP_FORMAT_STEREO_PCM16
                                             : NDSP_FORMAT_MONO_PCM16;
        if (!ndsp_format_initialized_) {
          ndspChnSetFormat(0, format);
          ndspChnSetRate(0, decoded.sample_rate);
          ndsp_format_initialized_ = true;
        }
        waveBuf[i].nsamples = decoded.samples_per_channel;
        DSP_FlushDataCache(waveBuf[i].data_vaddr, decoded.samples_per_channel *
                                                      decoded.channels *
                                                      sizeof(int16_t));
        ndspChnWaveBufAdd(0, &waveBuf[i]);
        stats.decoded_buffers++;
      } else if (decoded.eof) {
        break;
      } else if (input_kind_ == OpusInputKind::WebmStream &&
                 !webm_decoder_.has_failed()) {
        break;
      } else {
        decode_failed_ = true;
        stats.hit_decode_failure = true;
        break;
      }
    }
  }
  const u64 tick_end = svcGetSystemTick();
  stats.decode_ticks = (tick_end >= tick_start) ? (tick_end - tick_start) : 0;
  stats.queued_after_update = queued_wavebuf_count();
  stats.free_after_update = free_wavebuf_count();
  return stats;
}

bool OpusPocPlayer::is_track_finished() const {
  if (!is_playing) {
    return false;
  }
  const bool decoder_eof = (input_kind_ == OpusInputKind::WebmStream)
                               ? webm_decoder_.is_eof()
                               : decoder_.is_eof();
  if (!decoder_eof && !decode_failed_) {
    return false;
  }

  int finished_count = 0;
  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    if (waveBuf[i].status == NDSP_WBUF_FREE ||
        waveBuf[i].status == NDSP_WBUF_DONE) {
      finished_count++;
    }
  }
  return finished_count == OPUS_WAVE_BUF_COUNT;
}

void OpusPocPlayer::stop() {
  is_playing = false;
  decode_failed_ = false;
  ndspChnReset(0);
  ndspChnWaveBufClear(0);

  memset(waveBuf, 0, sizeof(waveBuf));
  if (audioBuffer) {
    for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
      waveBuf[i].data_vaddr = audioBuffer + (OPUS_PCM_CAPACITY_SAMPLES * i);
      waveBuf[i].status = NDSP_WBUF_FREE;
    }
    DSP_FlushDataCache(audioBuffer, OPUS_PCM_CAPACITY_SAMPLES *
                                        OPUS_WAVE_BUF_COUNT * sizeof(int16_t));
  }

  decoder_.reset();
  webm_decoder_.reset();
  input_kind_ = OpusInputKind::None;
  ndsp_format_initialized_ = false;
  webm_decode_call_log_start_ms_ = 0;
}

OpusPocPlayer::~OpusPocPlayer() {
  stop();
  if (audioBuffer) {
    linearFree(audioBuffer);
    audioBuffer = NULL;
  }
}

bool OpusPocPlayer::has_started_playing() const {
  if (!is_playing) {
    return false;
  }
  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    // WebM prebuffer keeps NDSP paused while buffers are only queued.
    // Treat actual PLAYING as the audible-start signal so UI state does not
    // leave Buffering early.
    if (waveBuf[i].status == NDSP_WBUF_PLAYING) {
      return true;
    }
  }
  return false;
}

bool OpusPocPlayer::has_decode_failed() const {
  if (input_kind_ == OpusInputKind::WebmStream) {
    return decode_failed_ || webm_decoder_.has_failed();
  }
  return decode_failed_ || decoder_.has_failed();
}

WebmRemuxError OpusPocPlayer::webm_remux_error() const {
  return (input_kind_ == OpusInputKind::WebmStream)
             ? webm_decoder_.remux_error()
             : WebmRemuxError::None;
}

bool OpusPocPlayer::get_webm_last_seek_runtime_point(
    uint64_t *out_start_byte, int *out_timecode_ms) const {
  if (input_kind_ != OpusInputKind::WebmStream) {
    return false;
  }
  return webm_decoder_.get_last_seek_runtime_point(out_start_byte,
                                                   out_timecode_ms);
}

int OpusPocPlayer::queued_wavebuf_count() const {
  int queued_count = 0;
  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    if (waveBuf[i].status == NDSP_WBUF_QUEUED ||
        waveBuf[i].status == NDSP_WBUF_PLAYING) {
      queued_count++;
    }
  }
  return queued_count;
}

int OpusPocPlayer::free_wavebuf_count() const {
  int free_count = 0;
  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    if (waveBuf[i].status == NDSP_WBUF_FREE ||
        waveBuf[i].status == NDSP_WBUF_DONE) {
      free_count++;
    }
  }
  return free_count;
}

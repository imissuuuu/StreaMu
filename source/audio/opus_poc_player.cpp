#include "opus_poc_player.h"

#include <3ds.h>
#include <malloc.h>
#include <string.h>

bool OpusPocPlayer::is_playing = false;

static constexpr size_t OPUS_PCM_CAPACITY_SAMPLES = 8192;
static constexpr int OPUS_WAVE_BUF_COUNT = 8;
OpusPocPlayer::OpusPocPlayer()
    : input_kind_(OpusInputKind::None), audioBuffer(NULL),
      decode_failed_(false), ndsp_format_initialized_(false),
      decode_tuning_(default_opus_decode_tuning()) {
  memset(waveBuf, 0, sizeof(waveBuf));
}

bool OpusPocPlayer::init() {
  decoder_.reset();
  decode_failed_ = false;
  audioBuffer = (int16_t *)linearAlloc(OPUS_PCM_CAPACITY_SAMPLES *
                                       OPUS_WAVE_BUF_COUNT * sizeof(int16_t));
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

bool OpusPocPlayer::start_webm_streaming(const std::vector<uint8_t> *buffer,
                                         LightLock *lock,
                                         const bool *download_complete) {
  stop();
  decode_failed_ = false;
  ndsp_format_initialized_ = false;
  input_kind_ = OpusInputKind::WebmStream;
  if (!audioBuffer ||
      !webm_decoder_.open_streaming(buffer, lock, download_complete)) {
    decode_failed_ = true;
    input_kind_ = OpusInputKind::None;
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
  const bool low_queue_refill =
      queued_before_update <= decode_tuning_.low_queue_wavebuf_threshold;
  const int max_decode_buffers =
      low_queue_refill ? decode_tuning_.refill_decode_buffers_per_update
                       : decode_tuning_.steady_max_decode_buffers_per_update;
  const int target_queued_wavebufs = low_queue_refill
                                          ? decode_tuning_.refill_target_queued_wavebufs
                                          : decode_tuning_.steady_target_queued_wavebufs;

  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    if (stats.decoded_buffers >= max_decode_buffers ||
        queued_wavebuf_count() >= target_queued_wavebufs) {
      break;
    }
    if (waveBuf[i].status == NDSP_WBUF_DONE ||
        waveBuf[i].status == NDSP_WBUF_FREE) {
      OpusDecodeResult decoded =
          (input_kind_ == OpusInputKind::WebmStream)
              ? webm_decoder_.decode((int16_t *)waveBuf[i].data_vaddr,
                                     OPUS_PCM_CAPACITY_SAMPLES)
              : decoder_.decode((int16_t *)waveBuf[i].data_vaddr,
                                OPUS_PCM_CAPACITY_SAMPLES);
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
    if (waveBuf[i].status == NDSP_WBUF_QUEUED ||
        waveBuf[i].status == NDSP_WBUF_PLAYING) {
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

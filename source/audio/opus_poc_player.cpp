#include "opus_poc_player.h"

#include <3ds.h>
#include <malloc.h>
#include <string.h>

bool OpusPocPlayer::is_playing = false;

static constexpr size_t OPUS_PCM_CAPACITY_SAMPLES = 8192;
static constexpr int OPUS_WAVE_BUF_COUNT = 8;
static constexpr int OPUS_STEADY_TARGET_QUEUED_WAVEBUFS = 7;
static constexpr int OPUS_STEADY_MAX_DECODE_BUFFERS_PER_UPDATE = 2;
static constexpr int OPUS_REFILL_DECODE_BUFFERS_PER_UPDATE = 6;
static constexpr int OPUS_LOW_QUEUE_WAVEBUF_THRESHOLD = 4;

OpusPocPlayer::OpusPocPlayer() : audioBuffer(NULL), decode_failed_(false) {
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

bool OpusPocPlayer::start(const uint8_t *data, size_t size) {
  stop();
  decode_failed_ = false;
  if (!audioBuffer || !decoder_.open(data, size)) {
    decode_failed_ = true;
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
  if (!audioBuffer ||
      !decoder_.open_streaming(buffer, lock, download_complete)) {
    decode_failed_ = true;
    return false;
  }
  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
  ndspChnSetPaused(0, false);
  is_playing = true;
  return true;
}

void OpusPocPlayer::update() { (void)update_with_stats(); }

OpusPlayerUpdateStats OpusPocPlayer::update_with_stats() {
  OpusPlayerUpdateStats stats = {};
  if (!is_playing || !audioBuffer || !decoder_.is_open()) {
    return stats;
  }

  const int queued_before_update = queued_wavebuf_count();
  const bool low_queue_refill =
      queued_before_update <= OPUS_LOW_QUEUE_WAVEBUF_THRESHOLD;
  const int max_decode_buffers =
      low_queue_refill
          ? OPUS_REFILL_DECODE_BUFFERS_PER_UPDATE
          : OPUS_STEADY_MAX_DECODE_BUFFERS_PER_UPDATE;
  const int target_queued_wavebufs =
      low_queue_refill ? OPUS_WAVE_BUF_COUNT : OPUS_STEADY_TARGET_QUEUED_WAVEBUFS;

  for (int i = 0; i < OPUS_WAVE_BUF_COUNT; i++) {
    if (stats.decoded_buffers >= max_decode_buffers ||
        queued_wavebuf_count() >= target_queued_wavebufs) {
      break;
    }
    if (waveBuf[i].status == NDSP_WBUF_DONE ||
        waveBuf[i].status == NDSP_WBUF_FREE) {
      OpusDecodeResult decoded = decoder_.decode(
          (int16_t *)waveBuf[i].data_vaddr, OPUS_PCM_CAPACITY_SAMPLES);
      if (decoded.ok) {
        u16 format = (decoded.channels == 2) ? NDSP_FORMAT_STEREO_PCM16
                                             : NDSP_FORMAT_MONO_PCM16;
        ndspChnSetFormat(0, format);
        ndspChnSetRate(0, decoded.sample_rate);
        waveBuf[i].nsamples = decoded.samples_per_channel;
        DSP_FlushDataCache(waveBuf[i].data_vaddr, decoded.samples_per_channel *
                                                      decoded.channels *
                                                      sizeof(int16_t));
        ndspChnWaveBufAdd(0, &waveBuf[i]);
        stats.decoded_buffers++;
      } else if (decoded.eof) {
        break;
      } else {
        decode_failed_ = true;
        stats.hit_decode_failure = true;
        break;
      }
    }
  }
  return stats;
}

bool OpusPocPlayer::is_track_finished() const {
  if (!is_playing) {
    return false;
  }
  if (!decoder_.is_eof() && !decode_failed_) {
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
  return decode_failed_ || decoder_.has_failed();
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

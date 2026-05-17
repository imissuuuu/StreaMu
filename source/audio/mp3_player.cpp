#include "mp3_player.h"
#include <3ds.h>
#include <malloc.h>
#include <memory>
#include <string.h>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;
bool MP3Player::is_playing = false;

MP3Player::MP3Player() : audioBuffer(NULL) {}

void MP3Player::init() {
  decoder_.reset();
  read_offset_ = 0;
  audioBuffer = (int16_t *)linearAlloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * 8 *
                                       sizeof(int16_t));
  ndspChnReset(0);

  memset(waveBuf, 0, sizeof(waveBuf));
  for (int i = 0; i < 8; i++) {
    waveBuf[i].data_vaddr =
        audioBuffer + (MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * i);
    waveBuf[i].status = NDSP_WBUF_FREE;
  }
}

void MP3Player::update() {
  // Wait for enough data to prevent stuttering from network delays
  LightLock_Lock(&stream_lock);
  size_t s_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
  LightLock_Unlock(&stream_lock);

  // If downloading and unread data < 30000 bytes, wait for buffering
  if (!is_playing)
    return;
  if (read_offset_ > s_size)
    read_offset_ = s_size;
  if (m_is_downloading && (s_size > read_offset_) &&
      (s_size - read_offset_ < 10000))
    return;

  for (int i = 0; i < 8; i++) {
    if (waveBuf[i].status == NDSP_WBUF_DONE ||
        waveBuf[i].status == NDSP_WBUF_FREE) {
      StreamDecodeResult decoded = {false, 0, 0, 0, 0};
      LightLock_Lock(&stream_lock);
      if (g_stream_buffer_ptr) {
        decoded = decoder_.decode(g_stream_buffer_ptr->data() + read_offset_,
                                  g_stream_buffer_ptr->size() - read_offset_,
                                  (int16_t *)waveBuf[i].data_vaddr,
                                  MINIMP3_MAX_SAMPLES_PER_FRAME * 2);
      }
      LightLock_Unlock(&stream_lock);

      if (decoded.ok) {
        // Auto-detect sample rate and mono/stereo format
        u16 format = (decoded.channels == 2) ? NDSP_FORMAT_STEREO_PCM16
                                             : NDSP_FORMAT_MONO_PCM16;
        ndspChnSetFormat(0, format);
        ndspChnSetRate(0, decoded.sample_rate);

        waveBuf[i].nsamples = decoded.samples_per_channel;

        // Flush CPU cache to DSP (required on 3DS to avoid noise)
        DSP_FlushDataCache(waveBuf[i].data_vaddr, decoded.samples_per_channel *
                                                      decoded.channels *
                                                      sizeof(int16_t));

        ndspChnWaveBufAdd(0, &waveBuf[i]);
        read_offset_ += decoded.bytes_consumed;
      } else if (decoded.bytes_consumed > 0) {
        read_offset_ += decoded.bytes_consumed;
      } else {
        break;
      }
    }
  }
}

bool MP3Player::is_track_finished() const {
  if (!is_playing)
    return false;

  // Not finished if still downloading
  if (m_is_downloading)
    return false;

  // Check remaining stream size
  LightLock_Lock(&stream_lock);
  size_t s_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
  LightLock_Unlock(&stream_lock);

  // Not finished if decodable data remains (ignore < 1024 bytes of trailing
  // junk)
  if (s_size > read_offset_ && (s_size - read_offset_) > 1024) {
    return false;
  }

  // All buffers FREE or DONE = no more data queued or playing
  int finished_count = 0;
  for (int i = 0; i < 8; i++) {
    if (waveBuf[i].status == NDSP_WBUF_FREE ||
        waveBuf[i].status == NDSP_WBUF_DONE) {
      finished_count++;
    }
  }

  // All 8 buffers done = track fully played
  if (finished_count == 8)
    return true;

  return false;
}

void MP3Player::stop() {
  is_playing = false;
  m_is_downloading = false;
  ndspChnReset(0);
  ndspChnWaveBufClear(0);

  // Prevent freeze/deadlock: force-free all buffers and flush HW cache on stop
  memset(waveBuf, 0, sizeof(waveBuf));
  for (int i = 0; i < 8; i++) {
    waveBuf[i].data_vaddr =
        audioBuffer + (MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * i);
    waveBuf[i].status = NDSP_WBUF_FREE;
  }
  DSP_FlushDataCache(audioBuffer,
                     MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * 8 * sizeof(int16_t));

  decoder_.reset();
  read_offset_ = 0;
}

MP3Player::~MP3Player() {
  if (audioBuffer)
    linearFree(audioBuffer);
}

void MP3Player::set_downloading_status(bool downloading) {
  m_is_downloading = downloading;
}

bool MP3Player::has_started_playing() const {
  if (!is_playing)
    return false;
  for (int i = 0; i < 8; i++) {
    if (waveBuf[i].status == NDSP_WBUF_QUEUED ||
        waveBuf[i].status == NDSP_WBUF_PLAYING)
      return true;
  }
  return false;
}

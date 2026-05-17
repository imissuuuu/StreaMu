#include <3ds.h>
#include "aac_poc_player.h"
#include <malloc.h>
#include <memory>
#include <string.h>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;

bool AacPocPlayer::is_playing = false;

static constexpr size_t AAC_PCM_CAPACITY_SAMPLES = 8192;
static constexpr int AAC_WAVE_BUF_COUNT = 8;

AacPocPlayer::AacPocPlayer() : audioBuffer(NULL) {}

bool AacPocPlayer::init() {
    decoder_.reset();
    read_offset_ = 0;
    audioBuffer = (int16_t*)linearAlloc(AAC_PCM_CAPACITY_SAMPLES *
                                        AAC_WAVE_BUF_COUNT * sizeof(int16_t));
    if (!audioBuffer) {
        return false;
    }

    ndspChnReset(0);
    memset(waveBuf, 0, sizeof(waveBuf));
    for (int i = 0; i < AAC_WAVE_BUF_COUNT; i++) {
        waveBuf[i].data_vaddr = audioBuffer + (AAC_PCM_CAPACITY_SAMPLES * i);
        waveBuf[i].status = NDSP_WBUF_FREE;
    }
    return true;
}

void AacPocPlayer::update() {
    LightLock_Lock(&stream_lock);
    size_t s_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);

    if (!is_playing || !audioBuffer) return;
    if (read_offset_ > s_size) read_offset_ = s_size;
    if (m_is_downloading && (s_size > read_offset_) &&
        (s_size - read_offset_ < 4096)) return;

    for (int i = 0; i < AAC_WAVE_BUF_COUNT; i++) {
        if (waveBuf[i].status == NDSP_WBUF_DONE ||
            waveBuf[i].status == NDSP_WBUF_FREE) {
            StreamDecodeResult decoded = {false, 0, 0, 0, 0};
            LightLock_Lock(&stream_lock);
            if (g_stream_buffer_ptr && read_offset_ < g_stream_buffer_ptr->size()) {
                decoded = decoder_.decode(g_stream_buffer_ptr->data() + read_offset_,
                                          g_stream_buffer_ptr->size() - read_offset_,
                                          (int16_t*)waveBuf[i].data_vaddr,
                                          AAC_PCM_CAPACITY_SAMPLES);
            }
            LightLock_Unlock(&stream_lock);

            if (decoded.ok) {
                u16 format = (decoded.channels == 2) ? NDSP_FORMAT_STEREO_PCM16
                                                     : NDSP_FORMAT_MONO_PCM16;
                ndspChnSetFormat(0, format);
                ndspChnSetRate(0, decoded.sample_rate);
                waveBuf[i].nsamples = decoded.samples_per_channel;
                DSP_FlushDataCache(waveBuf[i].data_vaddr,
                                   decoded.samples_per_channel *
                                       decoded.channels * sizeof(int16_t));
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

bool AacPocPlayer::is_track_finished() const {
    if (!is_playing) return false;
    if (m_is_downloading) return false;

    LightLock_Lock(&stream_lock);
    size_t s_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);
    if (s_size > read_offset_ && (s_size - read_offset_) > 1024) {
        return false;
    }

    int finished_count = 0;
    for (int i = 0; i < AAC_WAVE_BUF_COUNT; i++) {
        if (waveBuf[i].status == NDSP_WBUF_FREE || waveBuf[i].status == NDSP_WBUF_DONE) {
            finished_count++;
        }
    }
    return finished_count == AAC_WAVE_BUF_COUNT;
}

void AacPocPlayer::stop() {
    is_playing = false;
    m_is_downloading = false;
    ndspChnReset(0);
    ndspChnWaveBufClear(0);

    memset(waveBuf, 0, sizeof(waveBuf));
    if (audioBuffer) {
        for (int i = 0; i < AAC_WAVE_BUF_COUNT; i++) {
            waveBuf[i].data_vaddr = audioBuffer + (AAC_PCM_CAPACITY_SAMPLES * i);
            waveBuf[i].status = NDSP_WBUF_FREE;
        }
        DSP_FlushDataCache(audioBuffer, AAC_PCM_CAPACITY_SAMPLES *
                                        AAC_WAVE_BUF_COUNT * sizeof(int16_t));
    }

    decoder_.reset();
    read_offset_ = 0;
}

AacPocPlayer::~AacPocPlayer() {
    if (audioBuffer) linearFree(audioBuffer);
}

void AacPocPlayer::set_downloading_status(bool downloading) {
    m_is_downloading = downloading;
}

bool AacPocPlayer::has_started_playing() const {
    if (!is_playing) return false;
    for (int i = 0; i < AAC_WAVE_BUF_COUNT; i++) {
        if (waveBuf[i].status == NDSP_WBUF_QUEUED ||
            waveBuf[i].status == NDSP_WBUF_PLAYING) {
            return true;
        }
    }
    return false;
}

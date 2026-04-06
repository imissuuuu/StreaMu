#include <3ds.h>
#define MINIMP3_IMPLEMENTATION
#include "../../include/minimp3.h"
#include "mp3_player.h"
#include <malloc.h>
#include <string.h>
#include <memory>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;
bool MP3Player::is_playing = false;
static size_t read_offset = 0;

MP3Player::MP3Player() : audioBuffer(NULL) { mp3dec_init(&mp3d); }

void MP3Player::init() {
    mp3dec_init(&this->mp3d);
    audioBuffer = (int16_t*)linearAlloc(MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * 8 * sizeof(int16_t));
    ndspChnReset(0);
    
    memset(waveBuf, 0, sizeof(waveBuf));
    for (int i = 0; i < 8; i++) {
        waveBuf[i].data_vaddr = audioBuffer + (MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * i);
        waveBuf[i].status = NDSP_WBUF_FREE;
    }
}

void MP3Player::update() {
    // Wait for enough data to prevent stuttering from network delays
    LightLock_Lock(&stream_lock);
    size_t s_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);

    // If downloading and unread data < 30000 bytes, wait for buffering
    if (!is_playing) return;
    if (m_is_downloading && (s_size > read_offset) && (s_size - read_offset < 10000)) return;

    for (int i = 0; i < 8; i++) {
        if (waveBuf[i].status == NDSP_WBUF_DONE || waveBuf[i].status == NDSP_WBUF_FREE) {
            mp3dec_frame_info_t info;
            
            int samples = 0;
            LightLock_Lock(&stream_lock);
            if (g_stream_buffer_ptr) {
                samples = mp3dec_decode_frame(&this->mp3d, g_stream_buffer_ptr->data() + read_offset, 
                                              g_stream_buffer_ptr->size() - read_offset, 
                                              (int16_t*)waveBuf[i].data_vaddr, &info);
            }
            LightLock_Unlock(&stream_lock);
            
            if (samples > 0) {
                // Auto-detect sample rate and mono/stereo format
                u16 format = (info.channels == 2) ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16;
                ndspChnSetFormat(0, format);
                ndspChnSetRate(0, info.hz);

                waveBuf[i].nsamples = samples;

                // Flush CPU cache to DSP (required on 3DS to avoid noise)
                DSP_FlushDataCache(waveBuf[i].data_vaddr, samples * info.channels * sizeof(int16_t));

                ndspChnWaveBufAdd(0, &waveBuf[i]);
                read_offset += info.frame_bytes;
            } else if (info.frame_bytes > 0) {
                read_offset += info.frame_bytes; 
            } else {
                break; 
            }
        }
    }
}

bool MP3Player::is_track_finished() const {
    if (!is_playing) return false;
    
    // Not finished if still downloading
    if (m_is_downloading) return false;

    // Check remaining stream size
    LightLock_Lock(&stream_lock);
    size_t s_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);
    
    // Not finished if decodable data remains (ignore < 1024 bytes of trailing junk)
    if (s_size > read_offset && (s_size - read_offset) > 1024) {
        return false;
    }

    // All buffers FREE or DONE = no more data queued or playing
    int finished_count = 0;
    for (int i = 0; i < 8; i++) {
        if (waveBuf[i].status == NDSP_WBUF_FREE || waveBuf[i].status == NDSP_WBUF_DONE) {
            finished_count++;
        }
    }
    
    // All 8 buffers done = track fully played
    if (finished_count == 8) return true;
    
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
        waveBuf[i].data_vaddr = audioBuffer + (MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * i);
        waveBuf[i].status = NDSP_WBUF_FREE;
    }
    DSP_FlushDataCache(audioBuffer, MINIMP3_MAX_SAMPLES_PER_FRAME * 2 * 8 * sizeof(int16_t));
    
    read_offset = 0;
}

MP3Player::~MP3Player() {
    if (audioBuffer) linearFree(audioBuffer);
}

void MP3Player::set_downloading_status(bool downloading) {
    m_is_downloading = downloading;
}

bool MP3Player::has_started_playing() const {
    if (!is_playing) return false;
    for (int i = 0; i < 8; i++) {
        if (waveBuf[i].status == NDSP_WBUF_QUEUED ||
            waveBuf[i].status == NDSP_WBUF_PLAYING)
            return true;
    }
    return false;
}
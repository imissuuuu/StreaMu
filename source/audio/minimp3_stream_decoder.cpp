#define MINIMP3_IMPLEMENTATION
#include "minimp3_stream_decoder.h"

Minimp3StreamDecoder::Minimp3StreamDecoder() { reset(); }

void Minimp3StreamDecoder::reset() {
    mp3dec_init(&decoder_);
}

StreamDecodeResult Minimp3StreamDecoder::decode(const uint8_t* data, size_t size,
                                                int16_t* pcm_out,
                                                size_t pcm_capacity_samples) {
    StreamDecodeResult result = {false, 0, 0, 0, 0};
    if (!data || !pcm_out || size == 0 || pcm_capacity_samples == 0) {
        return result;
    }

    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(&decoder_, data,
                                            static_cast<int>(size), pcm_out,
                                            &info);
    result.bytes_consumed = info.frame_bytes > 0
                                ? static_cast<size_t>(info.frame_bytes)
                                : 0;
    result.samples_per_channel = samples;
    result.sample_rate = info.hz;
    result.channels = info.channels;
    result.ok = samples > 0 && info.hz > 0 &&
                (info.channels == 1 || info.channels == 2) &&
                (static_cast<size_t>(samples) * info.channels <=
                 pcm_capacity_samples);
    return result;
}

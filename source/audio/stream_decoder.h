#ifndef STREAM_DECODER_H
#define STREAM_DECODER_H

#include <stddef.h>
#include <stdint.h>

struct StreamDecodeResult {
    bool ok;
    size_t bytes_consumed;
    int samples_per_channel;
    int sample_rate;
    int channels;
};

class StreamDecoder {
public:
    virtual ~StreamDecoder() {}
    virtual void reset() = 0;
    virtual StreamDecodeResult decode(const uint8_t* data, size_t size,
                                      int16_t* pcm_out,
                                      size_t pcm_capacity_samples) = 0;
};

#endif

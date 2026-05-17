#ifndef AAC_ADTS_STREAM_DECODER_H
#define AAC_ADTS_STREAM_DECODER_H

#include "stream_decoder.h"

class AacAdtsStreamDecoder : public StreamDecoder {
public:
    AacAdtsStreamDecoder();
    ~AacAdtsStreamDecoder() override;
    void reset() override;
    StreamDecodeResult decode(const uint8_t* data, size_t size,
                              int16_t* pcm_out,
                              size_t pcm_capacity_samples) override;

private:
    void* decoder_;
};

#endif

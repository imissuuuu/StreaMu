#ifndef MINIMP3_STREAM_DECODER_H
#define MINIMP3_STREAM_DECODER_H

#include "../../include/minimp3.h"
#include "stream_decoder.h"

class Minimp3StreamDecoder : public StreamDecoder {
public:
    Minimp3StreamDecoder();
    void reset() override;
    StreamDecodeResult decode(const uint8_t* data, size_t size,
                              int16_t* pcm_out,
                              size_t pcm_capacity_samples) override;

private:
    mp3dec_t decoder_;
};

#endif

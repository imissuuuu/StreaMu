#include "aac_adts_stream_decoder.h"

#include <climits>

extern "C" {
#include "../../third_party/helix-aac/aacdec.h"
}

AacAdtsStreamDecoder::AacAdtsStreamDecoder() : decoder_(NULL) {
    reset();
}

AacAdtsStreamDecoder::~AacAdtsStreamDecoder() {
    if (decoder_) {
        AACFreeDecoder(decoder_);
        decoder_ = NULL;
    }
}

void AacAdtsStreamDecoder::reset() {
    if (decoder_) {
        AACFreeDecoder(decoder_);
        decoder_ = NULL;
    }
    decoder_ = AACInitDecoder();
}

StreamDecodeResult AacAdtsStreamDecoder::decode(const uint8_t* data, size_t size,
                                                int16_t* pcm_out,
                                                size_t pcm_capacity_samples) {
    StreamDecodeResult result = {false, 0, 0, 0, 0};
    if (!decoder_ || !data || !pcm_out || size == 0 || pcm_capacity_samples == 0) {
        return result;
    }
    if (size > (size_t)INT_MAX) {
        return result;
    }

    unsigned char* in_ptr = const_cast<unsigned char*>(data);
    int bytes_left = static_cast<int>(size);
    const int before = bytes_left;
    const int rc = AACDecode(decoder_, &in_ptr, &bytes_left, pcm_out);
    result.bytes_consumed = before >= bytes_left
                                ? static_cast<size_t>(before - bytes_left)
                                : 0;

    if (rc != ERR_AAC_NONE) {
        return result;
    }

    AACFrameInfo info;
    AACGetLastFrameInfo(decoder_, &info);
    if (info.nChans < 1 || info.nChans > 2 || info.sampRateOut <= 0 ||
        info.outputSamps <= 0 ||
        static_cast<size_t>(info.outputSamps) > pcm_capacity_samples) {
        return result;
    }

    result.ok = true;
    result.samples_per_channel = info.outputSamps / info.nChans;
    result.sample_rate = info.sampRateOut;
    result.channels = info.nChans;
    return result;
}

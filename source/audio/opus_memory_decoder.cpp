#include "opus_memory_decoder.h"

OpusMemoryDecoder::OpusMemoryDecoder()
    : file_(NULL), eof_(false), failed_(false) {}

OpusMemoryDecoder::~OpusMemoryDecoder() { reset(); }

bool OpusMemoryDecoder::open(const uint8_t *data, size_t size) {
  reset();
  if (!data || size == 0) {
    failed_ = true;
    return false;
  }

  int error = 0;
  file_ = op_open_memory(data, size, &error);
  if (!file_) {
    failed_ = true;
    return false;
  }

  const int channels = op_channel_count(file_, -1);
  if (channels < 1 || channels > 2) {
    failed_ = true;
    reset();
    failed_ = true;
    return false;
  }

  eof_ = false;
  failed_ = false;
  return true;
}

void OpusMemoryDecoder::reset() {
  if (file_) {
    op_free(file_);
    file_ = NULL;
  }
  eof_ = false;
  failed_ = false;
}

OpusDecodeResult OpusMemoryDecoder::decode(int16_t *pcm_out,
                                           size_t pcm_capacity_samples) {
  OpusDecodeResult result = {false, 0, 48000, 0, eof_};
  if (!file_ || !pcm_out || pcm_capacity_samples == 0 || eof_ || failed_) {
    return result;
  }
  if (pcm_capacity_samples > static_cast<size_t>(0x7fffffff)) {
    failed_ = true;
    return result;
  }

  int link = -1;
  const int decoded =
      op_read(file_, pcm_out, static_cast<int>(pcm_capacity_samples), &link);
  if (decoded == 0) {
    eof_ = true;
    result.eof = true;
    return result;
  }
  if (decoded < 0) {
    failed_ = true;
    return result;
  }

  const int channels = op_channel_count(file_, link);
  if (channels < 1 || channels > 2) {
    failed_ = true;
    return result;
  }

  result.ok = true;
  result.samples_per_channel = decoded;
  result.channels = channels;
  result.eof = false;
  return result;
}

bool OpusMemoryDecoder::is_open() const { return file_ != NULL; }

bool OpusMemoryDecoder::is_eof() const { return eof_; }

bool OpusMemoryDecoder::has_failed() const { return failed_; }

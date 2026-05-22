#include "opus_memory_decoder.h"

#include <string.h>

static int opus_stream_read(void *stream, unsigned char *ptr, int nbytes) {
  OpusMemoryDecoder::StreamSource *source =
      static_cast<OpusMemoryDecoder::StreamSource *>(stream);
  if (!source || !source->buffer || !source->lock ||
      !source->download_complete || !ptr || nbytes <= 0) {
    return OP_EREAD;
  }

  while (true) {
    LightLock_Lock(source->lock);
    const size_t available = source->buffer->size();
    const bool complete = *source->download_complete;
    if (source->offset < available) {
      const size_t remaining = available - source->offset;
      const size_t to_copy = remaining < static_cast<size_t>(nbytes)
                                 ? remaining
                                 : static_cast<size_t>(nbytes);
      memcpy(ptr, source->buffer->data() + source->offset, to_copy);
      source->offset += to_copy;
      LightLock_Unlock(source->lock);
      return static_cast<int>(to_copy);
    }
    LightLock_Unlock(source->lock);

    if (complete) {
      return 0;
    }
    svcSleepThread(10 * 1000 * 1000);
  }
}

OpusMemoryDecoder::OpusMemoryDecoder()
    : file_(NULL), stream_source_{NULL, NULL, NULL, 0}, eof_(false),
      failed_(false) {}

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

bool OpusMemoryDecoder::open_streaming(const std::vector<uint8_t> *buffer,
                                       LightLock *lock,
                                       const bool *download_complete) {
  reset();
  if (!buffer || !lock || !download_complete) {
    failed_ = true;
    return false;
  }

  stream_source_.buffer = buffer;
  stream_source_.lock = lock;
  stream_source_.download_complete = download_complete;
  stream_source_.offset = 0;

  OpusFileCallbacks callbacks = {};
  callbacks.read = opus_stream_read;

  int error = 0;
  file_ = op_open_callbacks(&stream_source_, &callbacks, NULL, 0, &error);
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

  const int decoded =
      op_read_stereo(file_, pcm_out, static_cast<int>(pcm_capacity_samples));
  if (decoded == 0) {
    eof_ = true;
    result.eof = true;
    return result;
  }
  if (decoded < 0) {
    failed_ = true;
    return result;
  }

  result.ok = true;
  result.samples_per_channel = decoded;
  result.channels = 2;
  result.eof = false;
  return result;
}

bool OpusMemoryDecoder::is_open() const { return file_ != NULL; }

bool OpusMemoryDecoder::is_eof() const { return eof_; }

bool OpusMemoryDecoder::has_failed() const { return failed_; }

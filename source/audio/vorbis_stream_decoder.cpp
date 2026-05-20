#include "vorbis_stream_decoder.h"

#include <string.h>

size_t VorbisStreamDecoder::read_memory(void *ptr, size_t size, size_t nmemb,
                                        void *datasource) {
  if (!ptr || !datasource || size == 0 || nmemb == 0) {
    return 0;
  }

  MemoryStream *stream = static_cast<MemoryStream *>(datasource);
  if (!stream->data || stream->offset >= stream->size) {
    return 0;
  }

  if (nmemb > static_cast<size_t>(-1) / size) {
    return 0;
  }

  const size_t available = stream->size - stream->offset;
  const size_t elements_to_copy =
      nmemb < (available / size) ? nmemb : (available / size);
  const size_t bytes_to_copy = elements_to_copy * size;
  memcpy(ptr, stream->data + stream->offset, bytes_to_copy);
  stream->offset += bytes_to_copy;
  return elements_to_copy;
}

int VorbisStreamDecoder::close_memory(void *) { return 0; }

VorbisStreamDecoder::VorbisStreamDecoder() : opened_(false) {
  memset(&file_, 0, sizeof(file_));
  stream_.data = NULL;
  stream_.size = 0;
  stream_.offset = 0;
}

VorbisStreamDecoder::~VorbisStreamDecoder() { close(); }

void VorbisStreamDecoder::reset() {
  close();
  stream_.data = NULL;
  stream_.size = 0;
  stream_.offset = 0;
}

void VorbisStreamDecoder::close() {
  if (opened_) {
    ov_clear(&file_);
    opened_ = false;
  }
  memset(&file_, 0, sizeof(file_));
}

bool VorbisStreamDecoder::open_if_needed(const uint8_t *data, size_t size) {
  if (opened_) {
    return true;
  }

  stream_.data = data;
  stream_.size = size;
  stream_.offset = 0;

  ov_callbacks callbacks;
  callbacks.read_func = VorbisStreamDecoder::read_memory;
  callbacks.seek_func = NULL;
  callbacks.close_func = VorbisStreamDecoder::close_memory;
  callbacks.tell_func = NULL;

  const int rc = ov_open_callbacks(&stream_, &file_, NULL, 0, callbacks);
  opened_ = (rc == 0);
  return opened_;
}

StreamDecodeResult VorbisStreamDecoder::decode(const uint8_t *data, size_t size,
                                               int16_t *pcm_out,
                                               size_t pcm_capacity_samples) {
  StreamDecodeResult result = {false, 0, 0, 0, 0};
  if (!data || !pcm_out || size == 0 || pcm_capacity_samples == 0) {
    return result;
  }

  const bool was_opened = opened_;
  if (!open_if_needed(data, size)) {
    result.bytes_consumed = stream_.offset > 0 ? stream_.offset : 1;
    reset();
    return result;
  }

  if (was_opened) {
    stream_.data = data;
    stream_.size = size;
    stream_.offset = 0;
  }

  const vorbis_info *info = ov_info(&file_, -1);
  if (!info || info->channels < 1 || info->channels > 2 || info->rate <= 0) {
    reset();
    return result;
  }

  const size_t max_bytes = pcm_capacity_samples * sizeof(int16_t);
  const size_t read_bytes = max_bytes > static_cast<size_t>(0x7fffffff)
                                ? static_cast<size_t>(0x7fffffff)
                                : max_bytes;
  int bitstream = 0;
  size_t total_decoded_bytes = 0;
  while (total_decoded_bytes < read_bytes) {
    const size_t remaining_bytes = read_bytes - total_decoded_bytes;
    const int decoded_bytes =
        ov_read(&file_, reinterpret_cast<char *>(pcm_out) + total_decoded_bytes,
                static_cast<int>(remaining_bytes), &bitstream);
    if (decoded_bytes <= 0) {
      break;
    }
    total_decoded_bytes += static_cast<size_t>(decoded_bytes);
  }

  result.bytes_consumed = stream_.offset;
  if (total_decoded_bytes == 0) {
    return result;
  }

  const int channels = info->channels;
  const int samples = static_cast<int>(total_decoded_bytes / sizeof(int16_t));
  if (samples <= 0 || (samples % channels) != 0) {
    return result;
  }

  result.ok = true;
  result.samples_per_channel = samples / channels;
  result.sample_rate = info->rate;
  result.channels = channels;
  return result;
}

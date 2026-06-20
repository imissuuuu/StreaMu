#ifndef WEBM_PCM_DECODE_WORKER_H
#define WEBM_PCM_DECODE_WORKER_H

#include <3ds.h>
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "opus_memory_decoder.h"
#include "webm_opus_streaming_decoder.h"
#include "webm_seek_types.h"

struct WebmPcmDecodeWorkerConfig {
  std::vector<uint8_t> *webm_buffer = NULL;
  LightLock *webm_lock = NULL;
  bool *webm_download_complete = NULL;
  WebmSeekExecutionContext seek_context;
  int emit_start_ms = 0;
  bool enable_parser_seek = false;
  bool prefer_offset_seek = false;
  std::string range_probe_base_url;
  uint64_t range_filesize = 0;
  uint64_t parser_prefetch_offset = 0;
  u64 perf_start_ms = 0;
};

struct WebmPcmWorkerSnapshot {
  bool running = false;
  bool failed = false;
  bool eof = false;
  WebmRemuxError error = WebmRemuxError::None;
  int queued_chunks = 0;
  int produced_chunks = 0;
  int consumed_chunks = 0;
  int queue_full_count = 0;
  u64 last_decode_ticks = 0;
};

class WebmPcmDecodeWorker {
public:
  WebmPcmDecodeWorker();
  ~WebmPcmDecodeWorker();

  bool start(const WebmPcmDecodeWorkerConfig &config);
  void stop();
  bool pop_decoded_pcm(int16_t *pcm_out, size_t pcm_capacity_samples,
                       OpusDecodeResult *out_result);
  bool is_running() const;
  bool is_eof() const;
  bool has_failed() const;
  WebmRemuxError remux_error() const;
  WebmPcmWorkerSnapshot snapshot() const;
  bool get_last_seek_runtime_point(uint64_t *out_start_byte,
                                   int *out_timecode_ms) const;

private:
  struct PcmChunk {
    int16_t *samples = NULL;
    int samples_per_channel = 0;
    int sample_rate = 48000;
    int channels = 2;
  };

  static void thread_entry(void *arg);
  void run();
  bool allocate_buffers();
  void release_buffers();
  bool push_chunk_locked(const OpusDecodeResult &decoded,
                         const int16_t *samples);
  void clear_queue_locked();
  void append_worker_log(const char *event,
                         const WebmPcmWorkerSnapshot &snapshot) const;
  void mark_failed(WebmRemuxError error);
  void mark_eof();
  void capture_seek_runtime_point();

  mutable LightLock lock_;
  Thread thread_;
  bool running_;
  bool stop_requested_;
  bool failed_;
  bool eof_;
  WebmRemuxError error_;
  WebmPcmDecodeWorkerConfig config_;
  WebmOpusStreamingDecoder decoder_;
  PcmChunk queue_[4];
  int read_index_;
  int write_index_;
  int queued_chunks_;
  int produced_chunks_;
  int consumed_chunks_;
  int queue_full_count_;
  u64 last_decode_ticks_;
  u64 last_queue_full_log_ms_;
  uint64_t last_seek_runtime_start_byte_;
  int last_seek_runtime_timecode_ms_;
  int16_t *decode_scratch_;
};

#endif

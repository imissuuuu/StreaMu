#include "../../include/network/youtube_api.h"
#include "../audio/opus_perf_metrics.h"
#include "../audio/playback_observer.h"
#include <3ds.h>
#include <curl/curl.h>
#include <memory>
#include <stdint.h>
#include <sstream>
#include <stdio.h>

extern std::unique_ptr<std::vector<uint8_t>> g_stream_buffer_ptr;
extern LightLock stream_lock;
extern bool g_stream_download_complete;
bool YouTubeAPI::should_cancel = false;
static bool g_webm_perf_active = false;
static bool g_webm_perf_first_byte_logged = false;
static size_t g_webm_perf_bytes = 0;
static u64 g_webm_perf_start_ms = 0;

static void append_webm_perf_log(const char *event, size_t bytes,
                                 size_t stream_buffer_bytes,
                                 size_t chunk_bytes) {
  if (!event || !g_webm_perf_active) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/webm_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (g_webm_perf_start_ms > 0 && now_ms >= g_webm_perf_start_ms)
          ? now_ms - g_webm_perf_start_ms
          : 0;
  fprintf(f, "[webm-perf] +%llums %s bytes=%lu buffer=%lu chunk=%lu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long>(bytes),
          static_cast<unsigned long>(stream_buffer_bytes),
          static_cast<unsigned long>(chunk_bytes));
  fclose(f);
}

#if STREAMU_ENABLE_OPUS_PERF_LOG
static bool g_opus_perf_active = false;
static bool g_opus_perf_first_byte_logged = false;
static bool g_opus_perf_start_bytes_logged = false;
static size_t g_opus_perf_bytes = 0;
static u64 g_opus_perf_start_ms = 0;
static u64 g_opus_perf_last_buffer_observe_ms = 0;

static void append_opus_perf_log(const char *event, size_t bytes,
                                 size_t stream_buffer_bytes,
                                 size_t chunk_bytes) {
  if (!event || !g_opus_perf_active) {
    return;
  }
  FILE *f = fopen("sdmc:/3ds/StreaMu/opus_perf.log", "a");
  if (!f) {
    return;
  }
  const u64 now_ms = osGetTime();
  const u64 elapsed_ms =
      (g_opus_perf_start_ms > 0 && now_ms >= g_opus_perf_start_ms)
          ? now_ms - g_opus_perf_start_ms
          : 0;
  fprintf(f, "[opus-perf] +%llums %s bytes=%lu buffer=%lu chunk=%lu\n",
          static_cast<unsigned long long>(elapsed_ms), event,
          static_cast<unsigned long>(bytes),
          static_cast<unsigned long>(stream_buffer_bytes),
          static_cast<unsigned long>(chunk_bytes));
  fclose(f);
}

static void append_opus_perf_log(OpusPerfEvent event, size_t bytes,
                                 size_t stream_buffer_bytes,
                                 size_t chunk_bytes) {
  append_opus_perf_log(opus_perf_event_name(event), bytes, stream_buffer_bytes,
                       chunk_bytes);
}
#endif

static int progress_callback(void *p, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
  return YouTubeAPI::should_cancel ? 1 : 0;
}

static size_t StreamingWriteCallback(void *contents, size_t size, size_t nmemb,
                                     void *userp) {
  size_t total_size = size * nmemb;
  size_t stream_buffer_size = 0;
  if (!YouTubeAPI::should_cancel && g_stream_buffer_ptr && total_size > 0) {
    uint8_t *data = static_cast<uint8_t *>(contents);
    LightLock_Lock(&stream_lock);
    g_stream_buffer_ptr->insert(g_stream_buffer_ptr->end(), data,
                                data + total_size);
    stream_buffer_size = g_stream_buffer_ptr->size();
    LightLock_Unlock(&stream_lock);
  }
#if STREAMU_ENABLE_OPUS_PERF_LOG
  if (g_opus_perf_active && total_size > 0) {
    g_opus_perf_bytes += total_size;
    if (!g_opus_perf_first_byte_logged) {
      g_opus_perf_first_byte_logged = true;
      append_opus_perf_log(OpusPerfEvent::FirstByte, g_opus_perf_bytes,
                           stream_buffer_size, total_size);
      playback_observer_log_simple(PlaybackCompareEvent::FirstByte,
                                   StreamContainerMode::ProxyOggOpus,
                                   stream_buffer_size);
    }
    if (!g_opus_perf_start_bytes_logged && g_opus_perf_bytes >= 16U * 1024U) {
      g_opus_perf_start_bytes_logged = true;
      append_opus_perf_log(OpusPerfEvent::Received16Kb, g_opus_perf_bytes,
                           stream_buffer_size, total_size);
    }
    const u64 now_ms = osGetTime();
    if (g_opus_perf_last_buffer_observe_ms == 0 ||
        now_ms >= g_opus_perf_last_buffer_observe_ms + 1000ULL) {
      append_opus_perf_log(OpusPerfEvent::StreamBufferObserve,
                           g_opus_perf_bytes, stream_buffer_size, total_size);
      g_opus_perf_last_buffer_observe_ms = now_ms;
    }
  }
#else
  (void)stream_buffer_size;
#endif
  if (g_webm_perf_active && total_size > 0) {
    g_webm_perf_bytes += total_size;
    if (!g_webm_perf_first_byte_logged) {
      g_webm_perf_first_byte_logged = true;
      append_webm_perf_log("first_byte", g_webm_perf_bytes, stream_buffer_size,
                           total_size);
      playback_observer_log_simple(PlaybackCompareEvent::FirstByte,
                                   StreamContainerMode::ProxyWebmOpus,
                                   stream_buffer_size);
    }
  }
  return total_size;
}

static size_t WriteCallback(void *contents, size_t size, size_t nmemb,
                            void *userp) {
  static_cast<std::string *>(userp)->append(static_cast<char *>(contents),
                                            size * nmemb);
  return size * nmemb;
}

static std::string parse_json_string_value(const std::string &json,
                                           const char *key) {
  if (!key) {
    return "";
  }
  const std::string search = std::string("\"") + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return "";
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return "";
  }
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n')) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] != '"') {
    return "";
  }
  ++pos;

  std::string out;
  while (pos < json.size()) {
    const char c = json[pos++];
    if (c == '\\') {
      if (pos >= json.size()) {
        return "";
      }
      const char escaped = json[pos++];
      switch (escaped) {
        case '"':
        case '\\':
        case '/':
          out.push_back(escaped);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          return "";
      }
      continue;
    }
    if (c == '"') {
      return out;
    }
    out.push_back(c);
  }
  return "";
}

static bool parse_json_u64_value(const std::string &json, const char *key,
                                 uint64_t *out_value) {
  if (!key || !out_value) {
    return false;
  }
  const std::string search = std::string("\"") + key + "\"";
  size_t pos = json.find(search);
  if (pos == std::string::npos) {
    return false;
  }
  pos = json.find(':', pos);
  if (pos == std::string::npos) {
    return false;
  }
  ++pos;
  while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                               json[pos] == '\r' || json[pos] == '\n')) {
    ++pos;
  }
  if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') {
    return false;
  }
  uint64_t value = 0;
  while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
    value = (value * 10ULL) + static_cast<uint64_t>(json[pos] - '0');
    ++pos;
  }
  *out_value = value;
  return true;
}

void YouTubeAPI::init() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  YouTubeAPI::should_cancel = false;
}
void YouTubeAPI::cleanup() { curl_global_cleanup(); }

bool YouTubeAPI::start_streaming(const std::string &url,
                                 StreamContainerMode mode) {
  CURL *curl = curl_easy_init();
  bool success = false;
  bool cancelled = false;
  if (!curl) {
    LightLock_Lock(&stream_lock);
    g_stream_download_complete = true;
    LightLock_Unlock(&stream_lock);
    return false;
  }
  LightLock_Lock(&stream_lock);
  g_stream_download_complete = false;
  LightLock_Unlock(&stream_lock);

  const bool is_opus_ogg = mode == StreamContainerMode::ProxyOggOpus;
  const bool is_webm_opus = mode == StreamContainerMode::ProxyWebmOpus;
#if STREAMU_ENABLE_OPUS_PERF_LOG
  g_opus_perf_active = is_opus_ogg;
  g_opus_perf_first_byte_logged = false;
  g_opus_perf_start_bytes_logged = false;
  g_opus_perf_bytes = 0;
  g_opus_perf_start_ms = is_opus_ogg ? osGetTime() : 0;
  g_opus_perf_last_buffer_observe_ms = 0;
  if (is_opus_ogg) {
    append_opus_perf_log(OpusPerfEvent::StreamRequestStart, 0, 0, 0);
  }
#else
  (void)is_opus_ogg;
#endif
  playback_observer_log_simple(PlaybackCompareEvent::RequestStart, mode, 0);
  g_webm_perf_active = is_webm_opus;
  g_webm_perf_first_byte_logged = false;
  g_webm_perf_bytes = 0;
  g_webm_perf_start_ms = is_webm_opus ? osGetTime() : 0;
  if (is_webm_opus) {
    append_webm_perf_log("stream_request_start", 0, 0, 0);
  }
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, StreamingWriteCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "3DS-YT");
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 8L);  // TCP connect timeout
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L); // 1 byte/s threshold
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, is_opus_ogg ? 180L : 60L);

  CURLcode res = curl_easy_perform(curl);
  cancelled = should_cancel || res == CURLE_ABORTED_BY_CALLBACK;
  if (res == CURLE_OK && !should_cancel) {
    success = true;
  }
  LightLock_Lock(&stream_lock);
  g_stream_download_complete = true;
  LightLock_Unlock(&stream_lock);
#if STREAMU_ENABLE_OPUS_PERF_LOG
  if (is_opus_ogg) {
    size_t final_buffer_size = 0;
    LightLock_Lock(&stream_lock);
    final_buffer_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);
    append_opus_perf_log(success ? OpusPerfEvent::StreamComplete
                                 : OpusPerfEvent::StreamFailed,
                         g_opus_perf_bytes, final_buffer_size, 0);
  }
  g_opus_perf_active = false;
#endif
  size_t final_buffer_size = 0;
  if (is_webm_opus) {
    LightLock_Lock(&stream_lock);
    final_buffer_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);
    append_webm_perf_log(
        success ? "stream_complete"
                : (cancelled ? "stream_cancelled" : "stream_failed"),
        g_webm_perf_bytes, final_buffer_size, 0);
  }
  if (!is_webm_opus) {
    LightLock_Lock(&stream_lock);
    final_buffer_size = g_stream_buffer_ptr ? g_stream_buffer_ptr->size() : 0;
    LightLock_Unlock(&stream_lock);
  }
  playback_observer_log_simple(
      success ? PlaybackCompareEvent::StreamComplete
              : (cancelled ? PlaybackCompareEvent::StreamCancelled
                           : PlaybackCompareEvent::StreamFailed),
      mode, final_buffer_size);
  g_webm_perf_active = false;
  curl_easy_cleanup(curl);
  return success;
}

std::string YouTubeAPI::http_get(const std::string &url, long timeout_sec) {
  CURL *curl = curl_easy_init();
  std::string readBuffer;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT,
                     timeout_sec);                        // Connection timeout
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec); // Overall timeout
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      readBuffer = ""; // Return empty on error
    }
    curl_easy_cleanup(curl);
  }
  return readBuffer;
}

std::string YouTubeAPI::http_get_ms(const std::string &url, long timeout_ms) {
  CURL *curl = curl_easy_init();
  std::string readBuffer;
  if (curl) {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
      readBuffer = "";
    }
    curl_easy_cleanup(curl);
  }
  return readBuffer;
}

bool YouTubeAPI::check_connection(long timeout_ms) {
  std::string url = get_base_url() + "/healthz";
  std::string res = http_get_ms(url, timeout_ms);
  return !res.empty();
}

void YouTubeAPI::search(const std::string &query, const std::string &lang,
                        SearchCallback callback) {
  std::string url = get_base_url() + "/search?q=" + query + "&lang=" + lang;
  std::string json = http_get(url, 15L); // 15s timeout for search
  if (!json.empty())
    callback(parse_search_results(json), true);
  else
    callback({}, false);
}

void YouTubeAPI::get_audio_stream_url(const std::string &video_id,
                                      int seek_seconds,
                                      StreamContainerMode mode,
                                      StreamCallback callback) {
  std::string url = get_base_url();
  if (mode == StreamContainerMode::ProxyWebmOpus) {
    url += "/stream_opus?i=" + video_id;
  } else {
    // Delegate WebM->Ogg remuxing to the proxy; 3DS decodes Opus directly.
    url += "/stream_opus_ogg?i=" + video_id;
    if (seek_seconds > 0) url += "&t=" + std::to_string(seek_seconds);
  }
  callback(url, true);
}

void YouTubeAPI::get_audio_stream_url(const std::string &video_id,
                                      int seek_seconds,
                                      StreamCallback callback) {
  get_audio_stream_url(video_id, seek_seconds,
                       StreamContainerMode::ProxyOggOpus, callback);
}

bool YouTubeAPI::get_webm_seek_stream_info(const std::string &video_id,
                                           WebmSeekStreamInfo *out_info) {
  if (!out_info || video_id.empty()) {
    return false;
  }
  const std::string url = get_base_url() + "/stream_opus_info?i=" + video_id;
  const std::string json = http_get(url, 15L);
  if (json.empty()) {
    return false;
  }

  WebmSeekStreamInfo parsed = {};
  parsed.stream_url = parse_json_string_value(json, "stream_url");
  parsed.has_filesize =
      parse_json_u64_value(json, "filesize", &parsed.filesize);
  parsed.has_duration_ms =
      parse_json_u64_value(json, "duration_ms", &parsed.duration_ms);
  if (parsed.stream_url.empty()) {
    parsed.stream_url = build_webm_stream_url(video_id, 0);
  }
  *out_info = parsed;
  return true;
}

std::string YouTubeAPI::build_webm_stream_url(const std::string &video_id,
                                              uint64_t start_byte) {
  std::string url = get_base_url() + "/stream_opus?i=" + video_id;
  if (start_byte > 0) {
    url += "&start=" + std::to_string(start_byte);
  }
  return url;
}

std::string
YouTubeAPI::build_webm_seek_probe_base_url(const std::string &video_id) {
  if (video_id.empty()) {
    return "";
  }
  return get_base_url() + "/stream_opus_probe?i=" + video_id;
}

std::string YouTubeAPI::get_webm_seek_probe(const std::string &video_id,
                                            uint64_t start_byte,
                                            uint64_t probe_size_bytes) {
  if (video_id.empty() || probe_size_bytes == 0) {
    return "";
  }
  std::string url = build_webm_seek_probe_base_url(video_id) +
                    "&start=" + std::to_string(start_byte) +
                    "&size=" + std::to_string(probe_size_bytes);
  return http_get(url, 15L);
}

std::vector<Track> YouTubeAPI::parse_search_results(const std::string &data) {
  std::vector<Track> results;
  std::stringstream ss(data);
  std::string line;

  while (std::getline(ss, line) && results.size() < 10) {
    std::stringstream linestream(line);
    std::string vid, title, duration, views, uploader, upload_date;

    std::getline(linestream, vid, '\t');
    std::getline(linestream, title, '\t');
    std::getline(linestream, duration, '\t');
    std::getline(linestream, views, '\t');
    std::getline(linestream, uploader, '\t');
    std::getline(linestream, upload_date, '\t');

    if (!vid.empty()) {
      std::string thumb = get_base_url() + "/thumbnail?id=" + vid;
      results.push_back(
          {title, vid, duration, views, uploader, upload_date, thumb});
    }
  }
  return results;
}
// Stub function implementations
bool YouTubeAPI::download_thumbnail(const std::string &video_id,
                                    std::vector<uint8_t> &data) {
  std::string url = get_base_url() + "/thumbnail?id=" + video_id;
  std::string raw = http_get(url, 8L);
  if (raw.empty()) return false;
  if (raw.size() > 512 * 1024)
    return false; // Reject unexpectedly large responses (>512KB)
  data.assign(raw.begin(), raw.end());
  return true;
}

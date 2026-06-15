#ifndef YOUTUBE_API_H
#define YOUTUBE_API_H
#include <functional>
#include <stdint.h>
#include <string>
#include <vector>

struct Track {
  std::string title;
  std::string id;
  std::string duration;
  std::string views;
  std::string uploader;
  std::string upload_date;
  std::string thumbnail_url; // "https://i.ytimg.com/vi/{id}/mqdefault.jpg"
};
typedef std::function<void(const std::vector<Track> &, bool)> SearchCallback;
typedef std::function<void(const std::string &, bool)> StreamCallback;

enum class StreamContainerMode {
  ProxyOggOpus,
  ProxyWebmOpus,
};

struct WebmSeekStreamInfo {
  std::string stream_url;
  uint64_t filesize = 0;
  uint64_t duration_ms = 0;
  bool has_filesize = false;
  bool has_duration_ms = false;
};

class YouTubeAPI {
public:
  static void init();
  static void cleanup();
  void search(const std::string &query, const std::string &lang,
              SearchCallback callback);
  void get_audio_stream_url(const std::string &video_id, int seek_seconds,
                            StreamContainerMode mode, StreamCallback callback);
  void get_audio_stream_url(const std::string &video_id, int seek_seconds,
                            StreamCallback callback);
  bool get_webm_seek_stream_info(const std::string &video_id,
                                 WebmSeekStreamInfo *out_info);
  std::string build_webm_stream_url(const std::string &video_id,
                                    uint64_t start_byte);
  std::string build_webm_seek_probe_base_url(const std::string &video_id);
  std::string get_webm_seek_probe(const std::string &video_id,
                                  uint64_t start_byte,
                                  uint64_t probe_size_bytes);
  bool start_streaming(const std::string &url, StreamContainerMode mode);
  static std::string http_get(const std::string &url, long timeout_sec = 15L);
  static std::string http_get_ms(const std::string &url, long timeout_ms);
  bool check_connection(long timeout_ms);

  static bool should_cancel;
  void set_server_ip(const std::string &ip) { m_server_ip = ip; }
  std::string get_base_url() const { return "http://" + m_server_ip; }

  bool download_thumbnail(const std::string &video_id,
                          std::vector<uint8_t> &data);

private:
  std::string m_server_ip;
  std::vector<Track> parse_search_results(const std::string &json);
};
#endif

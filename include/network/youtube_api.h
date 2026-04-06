#ifndef YOUTUBE_API_H
#define YOUTUBE_API_H
#include <string>
#include <vector>
#include <functional>
#include <stdint.h>

struct Track {
    std::string title;
    std::string id;
    std::string duration;
    std::string views;
    std::string uploader;
    std::string upload_date;
    std::string thumbnail_url; // "https://i.ytimg.com/vi/{id}/mqdefault.jpg"
};
typedef std::function<void(const std::vector<Track>&, bool)> SearchCallback;
typedef std::function<void(const std::string&, bool)> StreamCallback;

class YouTubeAPI {
public:
    void init();
    void cleanup();
    void search(const std::string& query, const std::string& lang, SearchCallback callback);
    void get_audio_stream_url(const std::string& video_id, int seek_seconds, std::function<void(const std::string&, bool)> callback);
    bool start_streaming(const std::string& url);
    std::string http_get(const std::string& url, long timeout_sec = 15L);
    bool check_connection();
    
    static bool should_cancel;
    void set_server_ip(const std::string& ip) { m_server_ip = ip; }
    std::string get_base_url() const { return "http://" + m_server_ip; }

    bool download_thumbnail(const std::string& video_id, std::vector<uint8_t>& data);

private:
    std::string m_server_ip;
    std::vector<Track> parse_search_results(const std::string& json);
};
#endif
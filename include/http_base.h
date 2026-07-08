#ifndef __HTTP_BASE_H__
#define __HTTP_BASE_H__

#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <algorithm>

namespace http_base{
    struct CaseInsensitiveHash {
        std::size_t operator()(std::string key) const {
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            return std::hash<std::string>{}(key);
        }
    };
    struct CaseInsensitiveEqual {
        bool operator()(const std::string& a, const std::string& b) const {
            return a.size() == b.size() &&
                std::equal(a.begin(), a.end(), b.begin(),
                    [](char ca, char cb){ return std::tolower(ca) == std::tolower(cb); });
        }
    };

    using header_map = std::unordered_map<std::string, std::string, CaseInsensitiveHash, CaseInsensitiveEqual>;

    std::vector<uint8_t> url_encode(const std::u8string& decoded);
    std::u8string url_decode(const std::vector<uint8_t>& encoded);
    std::string html_escape(const std::string& text);
    std::string json_escape(const std::string& text);
    std::string get_mime_type(const std::string& extension);
    bool parse_range(const std::string& range_header, std::size_t file_size, std::size_t& start, std::size_t& end);

    class HttpRequest{
    private:
        std::string method_;
        std::u8string path_;
        std::string version_ = "HTTP/1.1";
        header_map headers_;
    public:
        HttpRequest() = default;
        HttpRequest(const std::string& method, const std::u8string& path, const std::string& version = "HTTP/1.1");

        const std::string& method() const;
        const std::u8string& path() const;
        const std::string& version() const;
        const header_map& headers() const;
        std::string get_header(const std::string& key) const;

        void set_method(const std::string& method);
        void set_path(const std::u8string& path);
        void set_version(const std::string& version);
        void add_header(const std::string& key, const std::string& value);

        bool has_header(const std::string& key) const;
        void remove_header(const std::string& key);
        void clear_headers();
    };

    class HttpResponse{
    private:
        std::string version_ = "HTTP/1.1";
        int status_code_;
        std::string status_text_;
        header_map headers_;

        std::string get_status_text_(int status_code) const;
    public:
        HttpResponse() = default;
        HttpResponse(const std::string& version, int status_code);

        const std::string& version() const;
        int status_code() const;
        const std::string& status_text() const;
        const header_map& headers() const;
        std::string get_header(const std::string& key) const;

        void set_version(const std::string& version);
        void set_status_code(int status_code);
        void set_status_text(const std::string& status_text);
        void add_header(const std::string& key, const std::string& value);

        bool has_header(const std::string& key) const;
        void remove_header(const std::string& key);
        void clear_headers();
    };

}

#endif // __HTTP_BASE_H__
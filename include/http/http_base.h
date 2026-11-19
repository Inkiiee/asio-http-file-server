#ifndef __HTTP_BASE_H__
#define __HTTP_BASE_H__

#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>
#include <algorithm>
#include <string_view>
#include <cctype>
#include <optional>

namespace http_base{
    inline unsigned char safe_tolower(unsigned char c) noexcept {
        return static_cast<unsigned char>(std::tolower(c));
    }

    struct CaseInsensitiveHash {
        using is_transparent = void; // string_view 검색 허용 (C++20)

        std::size_t operator()(std::string_view sv) const noexcept {
            // FNV-1a 64-bit 해시 (문자열 복사/힙 할당 없음)
            std::size_t hash = 14695981039346656037ULL;
            for (unsigned char c : sv) {
                hash ^= safe_tolower(c);
                hash *= 1099511628211ULL;
            }
            return hash;
        }
    };

    struct CaseInsensitiveEqual {
        using is_transparent = void;

        bool operator()(std::string_view a, std::string_view b) const noexcept {
            return std::ranges::equal(a, b, [](unsigned char ca, unsigned char cb) {
                return safe_tolower(ca) == safe_tolower(cb);
            });
        }
    };

    using header_map = std::unordered_map<std::string, std::vector<std::string>, CaseInsensitiveHash, CaseInsensitiveEqual>;

    std::vector<uint8_t> url_encode(const std::u8string& decoded);
    std::u8string url_decode(const std::vector<uint8_t>& encoded, bool is_path = false);
    std::string html_escape(const std::string& text);
    std::string get_mime_type(const std::string& extension);
    bool parse_range(const std::string& range_header, std::size_t file_size, std::size_t& start, std::size_t& end);
    std::optional<std::size_t> str_to_num(const std::string& num_str, int base=10);

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
        const std::vector<std::string>& get_header(const std::string& key) const;

        void set_method(const std::string& method);
        void set_path(const std::u8string& path);
        void set_version(const std::string& version);
        void add_header(const std::string& key, const std::string& value);
        void set_header(const std::string& key, const std::string& value);

        bool has_header(const std::string& key) const;
        void remove_header(const std::string& key);
        void clear_headers();
    };

    class HttpResponse{
    private:
        std::string version_ = "HTTP/1.1";
        int status_code_ = 200;
        std::string status_text_ = "OK";
        header_map headers_;

        std::string get_status_text_(int status_code) const;
    public:
        HttpResponse() = default;
        HttpResponse(const std::string& version, int status_code);

        const std::string& version() const;
        int status_code() const;
        const std::string& status_text() const;
        const header_map& headers() const;
        const std::vector<std::string>& get_header(const std::string& key) const;

        void set_version(const std::string& version);
        void set_status_code(int status_code);
        void set_status_text(const std::string& status_text);
        void add_header(const std::string& key, const std::string& value);
        void set_header(const std::string& key, const std::string& value);

        bool has_header(const std::string& key) const;
        void remove_header(const std::string& key);
        void clear_headers();
    };

}

#endif // __HTTP_BASE_H__
#ifndef __HTTP_SESSION_H
#define __HTTP_SESSION_H

#include <asio.hpp>
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

namespace http_file_server{
    class HttpSession: public std::enable_shared_from_this<HttpSession>{
    private:
        asio::ip::tcp::socket socket_;
        static std::filesystem::path root_path_;

        asio::awaitable<void> handle_session();

        asio::awaitable<void> read_request(std::string& method, std::u8string& path, std::string& version,
                                            std::vector<std::pair<std::string, std::string>>& headers);

        asio::awaitable<void> send_response(int status_code, const std::string& status_text,
                                             const std::vector<std::pair<std::string, std::string>>& headers,
                                             const std::string& body = "");
        asio::awaitable<void> send_file_response(const std::filesystem::path& file_path,
                                                  const std::vector<std::pair<std::string, std::string>>& request_headers,
                                                  bool head_only);
        asio::awaitable<void> send_directory_listing(const std::filesystem::path& dir_path,
                                                      const std::u8string& request_path);

        std::u8string url_decode(const std::vector<uint8_t>& encoded);
        std::vector<uint8_t> url_encode(const std::u8string& decoded);

        bool is_valid_path(const std::filesystem::path& path);
        std::filesystem::path get_absolute_path(const std::filesystem::path& path);

        static std::string get_mime_type(const std::filesystem::path& path);
        static bool parse_range(const std::string& range_header, std::uintmax_t file_size,
                               std::uintmax_t& start, std::uintmax_t& end);
        static std::string html_escape(const std::string& text);
        static std::string json_escape(const std::string& text);
    public:
        HttpSession(asio::ip::tcp::socket socket);

        void start();
        void stop();

        static void set_root_path(const std::filesystem::path& path);
    };
}

#endif // __HTTP_SESSION_H
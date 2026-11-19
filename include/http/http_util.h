#ifndef __HTTP_UTIL_H__
#define __HTTP_UTIL_H__

#include "http_base.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace http_parser{
    class Parser{
    private:
        std::string buffer_;
    public:
        Parser(std::string buffer = ""): buffer_(std::move(buffer)) {};
        std::optional<http_base::HttpRequest> parse_request();
        std::optional<http_base::HttpResponse> parse_response();
    };
}

namespace http_sub_functions{
    bool iequals(std::string_view lhs, std::string_view rhs);
    std::string trim_copy(std::string_view value);
    bool header_has_token(const std::string& value, std::string_view token);
    bool headers_has_token(const std::vector<std::string>& values, std::string_view token);
    bool response_must_not_have_body(int status_code);
    std::size_t parse_chunk_size_line(const std::string& line);
}

namespace http_maker{
    class Maker{
    public:
        static std::string make_response(const http_base::HttpResponse& response);
        static std::string make_request(const http_base::HttpRequest& request);
        static std::string make_encoded_path(const std::filesystem::path& path, bool is_directory = false);
    };
}

#endif // __HTTP_UTIL_H__
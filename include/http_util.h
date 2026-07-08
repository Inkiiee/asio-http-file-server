#ifndef __HTTP_UTIL_H__
#define __HTTP_UTIL_H__

#include "http_base.h"

#include <filesystem>

namespace http_parser{
    class Parser{
    private:
        std::string buffer_;
    public:
        Parser(std::string buffer = ""): buffer_(std::move(buffer)) {};
        http_base::HttpRequest parse_request();
        http_base::HttpResponse parse_response();
    };
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
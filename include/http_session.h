#ifndef __HTTP_SESSION_H
#define __HTTP_SESSION_H

#include <asio.hpp>
#include <memory>
#include <string>

#include "http_base.h"
#include "web_dav_proto.h"
#include "my_proto.h"

namespace http_file_server{
    class HttpSession: public std::enable_shared_from_this<HttpSession>{
    private:
        asio::ip::tcp::socket socket_;
        std::string buf_;
        my_proto::MyProto proto_;
        web_dav_proto::WebDavProto web_dav_proto_;

        asio::awaitable<void> handle_session();
        asio::awaitable<void> process_request(const http_base::HttpRequest& request);
        asio::awaitable<void> process_get_request(const http_base::HttpRequest& request);
        asio::awaitable<void> process_put_request(const http_base::HttpRequest& request);

        asio::awaitable<void> read_request(http_base::HttpRequest& request);
        asio::awaitable<void> read_until(std::string& buffer, const std::string& delimiter);
        template<typename T>
        asio::awaitable<void> read_body(const http_base::HttpRequest& request, T& body, std::size_t& content_length);
        template<typename T>
        asio::awaitable<void> read_chunked_body(T& body, std::size_t& content_length);
        template<typename T>
        asio::awaitable<void> read_fixed_body(T& body, std::size_t content_length);

        asio::awaitable<void> write_no_body_response(const http_base::HttpResponse& response);
        template<typename T>
        asio::awaitable<void> write_response(const http_base::HttpResponse& response, T& body);
        template<typename T>
        asio::awaitable<void> write_chunked_body(T& body);
        template<typename T>
        asio::awaitable<void> write_fixed_body(T& body, std::size_t content_length);
    public:
        HttpSession(asio::ip::tcp::socket socket);

        void start();
        void stop();
        void set_root_path(const std::filesystem::path& path);
    };
}

#endif // __HTTP_SESSION_H
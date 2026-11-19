#ifndef __HTTP_SESSION_H
#define __HTTP_SESSION_H

#include "asio/awaitable.hpp"

#include "http_base.h"
#include "http_sender.h"
#include "http_receiver.h"
#include "web_dav_proto.h"
#include "my_proto.h"

namespace http_file_server{
    class HttpSession: public std::enable_shared_from_this<HttpSession>{
    private:
        asio::ip::tcp::socket socket_;

        my_proto::MyProto proto_;
        web_dav_proto::WebDavProto web_dav_proto_;
        HttpSender sender_;
        HttpReceiver receiver_;
        bool close_after_response_ = false;

        asio::awaitable<void> handle_session();
        asio::awaitable<void> process_request(const http_base::HttpRequest& request);
        void prepare_final_response(http_base::HttpResponse& response);
        asio::awaitable<void> send_no_body_response(http_base::HttpResponse& response);
        asio::awaitable<void> send_no_body_response(int status_code);

        template<typename T>
        asio::awaitable<void> send_response(http_base::HttpResponse& response, T& body){
            prepare_final_response(response);
            co_await sender_.send_response(response, body);
        }

        template<typename T>
        asio::awaitable<void> read_body(const http_base::HttpRequest& request, T& body, std::size_t& content_length){
            if(!receiver_.body_complete() && request.version() == "HTTP/1.1" &&
               request.has_header("Expect") && http_sub_functions::headers_has_token(request.get_header("Expect"), "100-continue"))
                co_await send_no_body_response(100);
            co_await receiver_.read_body(body, content_length);
        }
    public:
        HttpSession(asio::ip::tcp::socket socket);

        void start();
        void stop();
        void set_root_path(const std::filesystem::path& path);

        asio::awaitable<void> process_get_request(const http_base::HttpRequest& request);
        asio::awaitable<void> process_put_request(const http_base::HttpRequest& request);
        asio::awaitable<void> process_post_request(const http_base::HttpRequest& request);
    };
}

#endif // __HTTP_SESSION_H

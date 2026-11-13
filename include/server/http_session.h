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

        asio::awaitable<void> handle_session();
        asio::awaitable<void> process_request(const http_base::HttpRequest& request);
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
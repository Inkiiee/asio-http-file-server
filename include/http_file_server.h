/*
Written by: Inki Lee
*/

#ifndef __HTTP_FILE_SERVER_H__
#define __HTTP_FILE_SERVER_H__

#include <asio.hpp>

namespace http_file_server{
    class HttpFileServer{
    private:
        asio::io_context io_context_;
        asio::ip::tcp::endpoint listen_endpoint_;

        asio::awaitable<void> start_accept(asio::ip::tcp::acceptor& acceptor);
    public:
        HttpFileServer(const std::string& address, const std::string& port);
        HttpFileServer(const std::string& address, uint16_t port);

        ~HttpFileServer();

        void set_root_path(const std::string& path);
        void start();
    };
}

#endif // __HTTP_FILE_SERVER_H__ 
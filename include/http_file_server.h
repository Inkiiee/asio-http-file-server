/*
Written by: Inki Lee
*/

#ifndef __HTTP_FILE_SERVER_H__
#define __HTTP_FILE_SERVER_H__

#include <memory>

namespace http_file_server{
    class HttpFileServer{
    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    public:
        HttpFileServer(const std::string& address, const std::string& port);
        HttpFileServer(const std::string& address, uint16_t port);

        ~HttpFileServer();

        void set_root_path(const std::string& path);
        void start();
    };
}

#endif // __HTTP_FILE_SERVER_H__ 
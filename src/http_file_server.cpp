#include <iostream>
#include <asio.hpp>

#include "http_file_server.h"
#include "http_session.h"

using namespace http_file_server;
using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::as_tuple;
using asio::io_context;

class HttpFileServer::Impl{
private:
    io_context io_context_;
    tcp::endpoint listen_endpoint_;
    std::filesystem::path root_path_;
public:
    Impl(const std::string& address, const std::string& port){
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(address, port);
        listen_endpoint_ = *endpoints.begin();
    }
    Impl(const std::string& address, uint16_t port){
        tcp::resolver resolver(io_context_);
        auto endpoints = resolver.resolve(address, std::to_string(port));
        listen_endpoint_ = *endpoints.begin();
    }

    void set_root_path(const std::string& path){
        root_path_ = std::filesystem::canonical(std::filesystem::path(path));
    }

    awaitable<void> start_accept(tcp::acceptor& acceptor){
        for(;;){
            auto [e, socket] = co_await acceptor.async_accept(as_tuple);
            if(!e){
                auto session = std::make_shared<HttpSession>(std::move(socket));
                if(!root_path_.empty())
                    session->set_root_path(root_path_);
                session->start();
            }
        }
    }

    void start(){
        try{
            tcp::acceptor acceptor(io_context_, listen_endpoint_);
            co_spawn(io_context_, start_accept(acceptor), detached);
            io_context_.run();
        } catch (const std::exception& e) {
            std::cerr << "Error: " << e.what() << std::endl;
        }
    }
};

HttpFileServer::HttpFileServer(const std::string& address, const std::string& port) : impl_(std::make_unique<Impl>(address, port)) {}
HttpFileServer::HttpFileServer(const std::string& address, uint16_t port) : impl_(std::make_unique<Impl>(address, port)) {}
HttpFileServer::~HttpFileServer() {}

void HttpFileServer::set_root_path(const std::string& path){
    impl_->set_root_path(path);
}
void HttpFileServer::start(){
    impl_->start();
}
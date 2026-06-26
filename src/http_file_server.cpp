#include <iostream>
#include <memory>

#include "http_file_server.h"
#include "http_session.h"

using namespace http_file_server;
using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::as_tuple;

HttpFileServer::HttpFileServer(const std::string& address, const std::string& port){
    tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(address, port);
    listen_endpoint_ = *endpoints.begin();
}

HttpFileServer::HttpFileServer(const std::string& address, uint16_t port){
    tcp::resolver resolver(io_context_);
    auto endpoints = resolver.resolve(address, std::to_string(port));
    listen_endpoint_ = *endpoints.begin();
}

HttpFileServer::~HttpFileServer() {}

void HttpFileServer::set_root_path(const std::string& path){
    HttpSession::set_root_path(path);
}

awaitable<void> HttpFileServer::start_accept(tcp::acceptor& acceptor){
    for(;;){
        auto [e, socket] = co_await acceptor.async_accept(as_tuple);
        if(!e)
            std::make_shared<HttpSession>(std::move(socket))->start();
    }
}

void HttpFileServer::start(){
    try{
        tcp::acceptor acceptor(io_context_, listen_endpoint_);
        co_spawn(io_context_, start_accept(acceptor), detached);
        io_context_.run();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }
}
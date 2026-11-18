#include "http_sender.h"

#include <string>
#include <asio.hpp>

#include "http_base.h"
#include "http_util.h"

using namespace http_file_server;
using namespace http_base;

using asio::awaitable;

awaitable<void> HttpSender::send_no_body_response(const HttpResponse& response){
    auto response_str = http_maker::Maker::make_response(response);
    auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(response_str), asio::as_tuple);
    if(ec){
        throw std::runtime_error("Error writing response: " + ec.message());
    }
}

awaitable<void> HttpSender::send_no_body_response(int status_code){
    HttpResponse response;
    response.set_status_code(status_code);
    response.add_header("Content-Length", "0");

    if(status_code >= 400 && status_code < 600){
        response.add_header("Connection", "close");
    }

    auto response_str = http_maker::Maker::make_response(response);
    auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(response_str), asio::as_tuple);
    if(ec){
        throw std::runtime_error("Error writing response: " + ec.message());
    }
}

#include "http_sender.h"

#include <string>
#include <asio.hpp>

#include "http_base.h"
#include "http_util.h"

using namespace http_file_server;
using namespace http_base;

using asio::awaitable;

awaitable<void> HttpSender::send_no_body_response(const HttpResponse& response){
    auto headers = response;
    if(headers.status_code() < 200 || headers.status_code() == 204){
        headers.remove_header("Content-Length");
        headers.remove_header("Transfer-Encoding");
    }

    auto response_str = http_maker::Maker::make_response(headers);
    auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(response_str), asio::as_tuple);
    if(ec){
        throw std::runtime_error("Error writing response: " + ec.message());
    }
}

#include "http_receiver.h"

#include <string>
#include <asio.hpp>

#include "http_base.h"
#include "http_util.h"

using namespace std;
using namespace http_file_server;
using namespace http_base;
using namespace http_sub_functions;
using namespace http_parser;

using asio::awaitable;
using asio::as_tuple;

awaitable<bool> HttpReceiver::read_request(http_base::HttpRequest& request){
    if(!body_complete())
        throw std::logic_error("Previous request body was not consumed");

    auto [ec, bytes_transferred] = co_await async_read_until(*psocket_, asio::dynamic_buffer(buf_), "\r\n\r\n", as_tuple);
    if(ec) throw std::runtime_error("Connection closed: " + ec.message());

    string request_data = buf_.substr(0, bytes_transferred);
    buf_.erase(0, bytes_transferred);

    Parser parser(request_data);
    auto parse_request = parser.parse_request();
    if(!parse_request.has_value()){
        co_return false;
    }

    request = std::move(*parse_request);
    if(request.has_header("Transfer-Encoding")){
        const auto& codings = request.get_header("Transfer-Encoding");
        if(request.version() != "HTTP/1.1" || request.has_header("Content-Length") ||
           codings.size() != 1 || !iequals(codings.front(), "chunked"))
            co_return false;
        body_framing_ = BodyFraming::chunked;
    }
    else if(request.has_header("Content-Length")){
        const auto& lengths = request.get_header("Content-Length");
        auto length = lengths.size() == 1 ? str_to_num(lengths.front()) : nullopt;
        if(!length)
            co_return false;
        if(*length > 0){
            body_length_ = *length;
            body_framing_ = BodyFraming::fixed;
        }
    }

    co_return true;
}

awaitable<void> HttpReceiver::discard_body(){
    std::nullptr_t discard = nullptr;
    std::size_t discarded = 0;
    co_await read_body(discard, discarded);
}

awaitable<void> HttpReceiver::read_until(std::string& buffer, const std::string& delimiter){
    if(delimiter.empty())
        throw runtime_error("Delimiter must not be empty");

    for(;;){
        auto delimiter_pos = buf_.find(delimiter);
        if(delimiter_pos != string::npos){
            buffer.assign(buf_.data(), delimiter_pos);
            buf_.erase(0, delimiter_pos + delimiter.size());
            co_return;
        }

        auto [ec, bytes_transferred] = co_await asio::async_read_until(*psocket_, asio::dynamic_buffer(buf_), delimiter, as_tuple);
        (void)bytes_transferred;
        if(ec){
            throw std::runtime_error("Error reading: " + ec.message());
        }
    }
}

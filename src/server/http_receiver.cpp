#include "http_receiver.h"

#include <string>
#include <asio.hpp>
#include <asio/cancel_at.hpp>

#include "http_base.h"
#include "http_util.h"

using namespace std;
using namespace http_file_server;
using namespace http_base;
using namespace http_sub_functions;
using namespace http_parser;

using asio::awaitable;
using asio::as_tuple;

awaitable<ReadResult> HttpReceiver::read_request(http_base::HttpRequest& request){
    if(!body_complete())
        throw std::logic_error("Previous request body was not consumed");

    const auto deadline = Clock::now() + limits_.header_timeout;
    auto [ec, bytes_transferred] = co_await read_delimited("\r\n\r\n", limits_.header_bytes, deadline);
    if(buf_.empty() && (ec == asio::error::eof || ec == asio::error::operation_aborted))
        co_return ReadResult::closed; // EOF 또는 keep-alive 대기 시간 초과.
    if(ec == asio::error::not_found)
        throw HttpReadError(431, "Request headers are too large");
    if(ec == asio::error::operation_aborted)
        throw HttpReadError(408, "Request header read timed out");
    if(ec)
        throw std::runtime_error("Connection closed: " + ec.message());

    string request_data = buf_.substr(0, bytes_transferred);
    buf_.erase(0, bytes_transferred);

    Parser parser(request_data);
    auto parse_request = parser.parse_request();
    if(!parse_request.has_value()){
        co_return ReadResult::bad_request;
    }

    request = std::move(*parse_request);
    max_body_bytes_ = request.method() == "PUT" ? limits_.upload_bytes : limits_.other_body_bytes;
    if(request.has_header("Transfer-Encoding")){
        const auto& codings = request.get_header("Transfer-Encoding");
        if(request.version() != "HTTP/1.1" || request.has_header("Content-Length") ||
           codings.size() != 1 || !iequals(codings.front(), "chunked"))
            co_return ReadResult::bad_request;
        body_framing_ = BodyFraming::chunked;
    }
    else if(request.has_header("Content-Length")){
        const auto& lengths = request.get_header("Content-Length");
        auto length = lengths.size() == 1 ? str_to_num(lengths.front()) : nullopt;
        if(!length)
            co_return ReadResult::bad_request;
        if(*length > max_body_bytes_)
            throw HttpReadError(413, "Request body is too large");
        if(*length > 0){
            body_length_ = *length;
            body_framing_ = BodyFraming::fixed;
        }
    }

    co_return ReadResult::ok;
}

awaitable<void> HttpReceiver::discard_body(){
    std::nullptr_t discard = nullptr;
    std::size_t discarded = 0;
    co_await read_body(discard, discarded);
}

HttpReceiver::Clock::time_point HttpReceiver::next_body_read_deadline() const{
    return std::min(body_deadline_, Clock::now() + limits_.body_read_timeout);
}

awaitable<tuple<asio::error_code, std::size_t>> HttpReceiver::read_delimited(
    std::string_view delimiter, std::size_t max_bytes, Clock::time_point deadline){
    if(Clock::now() >= deadline)
        co_return tuple{asio::error::make_error_code(asio::error::operation_aborted), std::size_t{0}};

    // 이미 읽은 다음 메시지의 바이트를 제한된 dynamic_buffer가 잘라 내지 않도록 한다.
    auto pos = std::string_view(buf_).substr(0, max_bytes).find(delimiter);
    if(pos != std::string_view::npos)
        co_return tuple{asio::error_code{}, pos + delimiter.size()};
    if(buf_.size() >= max_bytes)
        co_return tuple{asio::error::make_error_code(asio::error::not_found), std::size_t{0}};

    co_return co_await asio::async_read_until(*psocket_, asio::dynamic_buffer(buf_, max_bytes),
        delimiter, asio::cancel_at(deadline, as_tuple));
}

awaitable<string> HttpReceiver::read_body_line(std::size_t max_bytes, int limit_status){
    auto [ec, bytes_transferred] = co_await read_delimited("\r\n", max_bytes, next_body_read_deadline());
    if(ec == asio::error::not_found)
        throw HttpReadError(limit_status, "Chunk metadata or trailer section is too large");
    if(ec == asio::error::operation_aborted)
        throw HttpReadError(408, "Request body read timed out");
    if(ec == asio::error::eof)
        throw HttpReadError(400, "Incomplete request body");
    if(ec)
        throw std::runtime_error("Error reading request body: " + ec.message());

    auto line = buf_.substr(0, bytes_transferred - 2);
    buf_.erase(0, bytes_transferred);
    co_return line;
}

awaitable<void> HttpReceiver::fill_body_buffer(std::size_t bytes){
    if(bytes == 0)
        throw std::logic_error("Request body read size must be positive");

    auto [ec, bytes_transferred] = co_await asio::async_read(*psocket_, asio::dynamic_buffer(buf_, bytes),
        asio::transfer_exactly(bytes), asio::cancel_at(next_body_read_deadline(), as_tuple));
    if(ec == asio::error::operation_aborted)
        throw HttpReadError(408, "Request body read timed out");
    if(ec == asio::error::eof)
        throw HttpReadError(400, "Incomplete request body");
    if(ec || bytes_transferred != bytes)
        throw std::runtime_error("Error reading request body: " + ec.message());
}

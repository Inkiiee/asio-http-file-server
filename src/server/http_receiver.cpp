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

awaitable<bool> HttpReceiver::read_request(http_base::HttpRequest& request, bool& is_need_close){
    auto [ec, bytes_transferred] = co_await async_read_until(*psocket_, asio::dynamic_buffer(buf_), "\r\n\r\n", as_tuple);
    if(ec) throw std::runtime_error("Connection closed: " + ec.message());

    string request_data = buf_.substr(0, bytes_transferred);
    buf_.erase(0, bytes_transferred);

    Parser parser(request_data);
    auto parse_request = parser.parse_request();
    if(!parse_request.has_value()){
        co_return false;
    }

    request = *parse_request;
    string method = request.method();
    auto path = request.path();
    string path_str(path.begin(), path.end());
    string version = request.version();

    if(request.has_header("Connection") && header_has_token(request.get_header("Connection")[0], "close")
        || request.version() == "HTTP/1.0")
        is_need_close = true;
    else
        is_need_close = false;

    co_return true;
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

#ifndef HTTP_RECEIVER_H
#define HTTP_RECEIVER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>

#include "http_base.h"
#include "http_util.h"

namespace http_file_server{
    class HttpReceiver{
    public:
        HttpReceiver(asio::ip::tcp::socket* socket, std::size_t io_chunk_size = 8192): psocket_(socket), io_chunk_size_(io_chunk_size){}
        asio::awaitable<void> read_request(http_base::HttpRequest& request, bool& is_need_close);
        virtual ~HttpReceiver() = default;

        template<typename T>
        asio::awaitable<void> read_body(const http_base::HttpRequest& request, T& body, std::size_t& content_length);
    private:
        std::string buf_;
        asio::ip::tcp::socket* psocket_;
        std::size_t io_chunk_size_;

        asio::awaitable<void> read_until(std::string& buffer, const std::string& delimiter);
        template<typename T>
        asio::awaitable<void> read_chunked_body(T& body, std::size_t& content_length);
        template<typename T>
        asio::awaitable<void> read_fixed_body(T& body, std::size_t content_length);
    };

    template<typename T>
    asio::awaitable<void> HttpReceiver::read_body(const http_base::HttpRequest& request, T& body, std::size_t& content_length){
        if(request.has_header("Transfer-Encoding") && http_sub_functions::header_has_token(request.get_header("Transfer-Encoding"), "chunked")){
            co_await read_chunked_body(body, content_length);
        }
        else if(request.has_header("Content-Length")){
            content_length = std::stoul(request.get_header("Content-Length"));
            co_await read_fixed_body(body, content_length);
        }
        content_length = 0;
        co_return;
    }

    template<typename T>
    asio::awaitable<void> HttpReceiver::read_chunked_body(T& body, std::size_t& content_length){
        content_length = 0;

        for(;;){
            std::string chunked_size_str;
            co_await read_until(chunked_size_str, "\r\n");

            std::size_t chunk_size = http_sub_functions::parse_chunk_size_line(chunked_size_str);
            if(chunk_size == 0){
                for(;;){
                    std::string trailer_line;
                    co_await read_until(trailer_line, "\r\n");
                    if(trailer_line.empty())
                        co_return;
                }
            }

            if(chunk_size > std::numeric_limits<std::size_t>::max() - content_length)
                throw std::runtime_error("Chunked body is too large");

            std::string chunk_data;
            co_await read_fixed_body(chunk_data, chunk_size);

            std::string chunk_crlf;
            co_await read_until(chunk_crlf, "\r\n");
            if(!chunk_crlf.empty())
                throw std::runtime_error("Malformed chunk data");

            if constexpr (std::is_same_v<T, std::string>)
                body += chunk_data;
            else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<char>>)
                body.insert(body.end(), chunk_data.begin(), chunk_data.end());
            else if constexpr (std::is_base_of_v<std::ostream, T>)
                body.write(chunk_data.data(), chunk_data.size());
            else
                throw std::runtime_error("Unsupported body type for read_chunked_body");

            content_length += chunk_size;
        }
    }

    template<typename T>
    asio::awaitable<void> HttpReceiver::read_fixed_body(T& body, std::size_t content_length){
        std::size_t bytes_to_read = content_length;
        std::size_t bytes_read = 0;

        // 버퍼에 이미 남아있는 데이터 먼저 처리
        if(!buf_.empty()){
            std::size_t buffered = std::min(buf_.size(), bytes_to_read);
            std::string chunk_data(buf_.begin(), buf_.begin() + buffered);
            buf_.erase(0, buffered);
            if constexpr (std::is_same_v<T, std::string>)
                body += chunk_data;
            else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<char>>)
                body.insert(body.end(), chunk_data.begin(), chunk_data.end());
            else if constexpr (std::is_base_of_v<std::ostream, T>)
                body.write(chunk_data.data(), chunk_data.size());
            else
                throw std::runtime_error("Unsupported body type for read_fixed_body");
            bytes_read += buffered;
        }

        while(bytes_read < bytes_to_read){
            std::size_t chunk_size = std::min(io_chunk_size_, bytes_to_read - bytes_read);
            auto [ec, bytes_transferred] = co_await asio::async_read(*psocket_, asio::dynamic_buffer(buf_), asio::transfer_exactly(chunk_size), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error reading fixed body: " + ec.message());
            }

            std::string chunk_data(buf_.begin(), buf_.begin() + bytes_transferred);
            buf_.erase(0, bytes_transferred);
            if constexpr (std::is_same_v<T, std::string>)
                body += chunk_data;
            else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<char>>)
                body.insert(body.end(), chunk_data.begin(), chunk_data.end());
            else if constexpr (std::is_base_of_v<std::ostream, T>)
                body.write(chunk_data.data(), chunk_data.size());
            else
                throw std::runtime_error("Unsupported body type for read_fixed_body");

            bytes_read += bytes_transferred;
        }
    }
}

#endif // HTTP_RECEIVER_H
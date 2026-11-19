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
        asio::awaitable<bool> read_request(http_base::HttpRequest& request);
        bool body_complete() const noexcept { return body_framing_ == BodyFraming::none; }
        asio::awaitable<void> discard_body();
        virtual ~HttpReceiver() = default;

        template<typename T>
        asio::awaitable<void> read_body(T& body, std::size_t& content_length);
    private:
        enum class BodyFraming { none, fixed, chunked };
        std::string buf_;
        asio::ip::tcp::socket* psocket_;
        std::size_t io_chunk_size_;
        BodyFraming body_framing_ = BodyFraming::none;
        std::size_t body_length_ = 0;

        asio::awaitable<void> read_until(std::string& buffer, const std::string& delimiter);
        template<typename T>
        asio::awaitable<void> read_chunked_body(T& body, std::size_t& content_length);
        template<typename T>
        asio::awaitable<void> read_fixed_body(T& body, std::size_t content_length);
        template<typename T>
        static void append_body(T& body, const char* data, std::size_t size);
    };

    template<typename T>
    asio::awaitable<void> HttpReceiver::read_body(T& body, std::size_t& content_length){
        content_length = 0;
        if(body_framing_ == BodyFraming::chunked){
            co_await read_chunked_body(body, content_length);
        }
        else if(body_framing_ == BodyFraming::fixed){
            co_await read_fixed_body(body, body_length_);
            content_length = body_length_;
        }
        body_framing_ = BodyFraming::none;
        body_length_ = 0;
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

            co_await read_fixed_body(body, chunk_size);

            std::string chunk_crlf;
            co_await read_fixed_body(chunk_crlf, 2);
            if(chunk_crlf != "\r\n")
                throw std::runtime_error("Malformed chunk data");

            content_length += chunk_size;
        }
    }

    template<typename T>
    asio::awaitable<void> HttpReceiver::read_fixed_body(T& body, std::size_t content_length){
        while(content_length > 0){
            if(buf_.empty()){
                auto chunk_size = std::min(io_chunk_size_, content_length);
                auto [ec, bytes_transferred] = co_await asio::async_read(*psocket_, asio::dynamic_buffer(buf_), asio::transfer_exactly(chunk_size), asio::as_tuple);
                if(ec || bytes_transferred == 0)
                    throw std::runtime_error("Error reading fixed body: " + ec.message());
            }

            auto buffered = std::min(buf_.size(), content_length);
            append_body(body, buf_.data(), buffered);
            buf_.erase(0, buffered);
            content_length -= buffered;
        }
    }

    template<typename T>
    void HttpReceiver::append_body(T& body, const char* data, std::size_t size){
        if constexpr (std::is_same_v<T, std::nullptr_t>)
            return;
        else if constexpr (std::is_same_v<T, std::string>)
            body.append(data, size);
        else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<char>>)
            body.insert(body.end(), data, data + size);
        else if constexpr (std::is_base_of_v<std::ostream, T>)
            body.write(data, size);
        else
            throw std::runtime_error("Unsupported body type");
    }
}

#endif // HTTP_RECEIVER_H

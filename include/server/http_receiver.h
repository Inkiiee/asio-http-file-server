#ifndef HTTP_RECEIVER_H
#define HTTP_RECEIVER_H

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include <asio/awaitable.hpp>
#include <asio/error_code.hpp>
#include <asio/ip/tcp.hpp>

#include "http_base.h"
#include "http_util.h"

namespace http_file_server{
    struct HttpReadLimits{
        std::size_t header_bytes = 16 * 1024;
        std::size_t upload_bytes = 1024 * 1024 * 1024;
        std::size_t other_body_bytes = 1024 * 1024;
        std::size_t chunk_line_bytes = 4 * 1024;
        std::size_t trailer_bytes = 16 * 1024;
        std::size_t trailer_count = 32;
        std::chrono::steady_clock::duration header_timeout = std::chrono::seconds(15);
        std::chrono::steady_clock::duration body_read_timeout = std::chrono::seconds(30);
        std::chrono::steady_clock::duration body_timeout = std::chrono::minutes(15);
    };

    class HttpReadError : public std::runtime_error{
    public:
        HttpReadError(int status_code, const char* message): std::runtime_error(message), status_code_(status_code){}
        int status_code() const noexcept { return status_code_; }
    private:
        int status_code_;
    };

    enum class ReadResult{
        ok,
        bad_request,
        closed
    };
    class HttpReceiver{
    public:
        HttpReceiver(asio::ip::tcp::socket* socket, std::size_t io_chunk_size = 8192, HttpReadLimits limits = {})
            : psocket_(socket), io_chunk_size_(io_chunk_size), limits_(limits){}
        asio::awaitable<ReadResult> read_request(http_base::HttpRequest& request);
        bool body_complete() const noexcept { return body_framing_ == BodyFraming::none; }
        asio::awaitable<void> discard_body();
        ~HttpReceiver() = default;

        template<typename T>
        asio::awaitable<void> read_body(T& body, std::size_t& content_length);
    private:
        using Clock = std::chrono::steady_clock;
        enum class BodyFraming { none, fixed, chunked };
        std::string buf_;
        asio::ip::tcp::socket* psocket_;
        std::size_t io_chunk_size_;
        HttpReadLimits limits_;
        BodyFraming body_framing_ = BodyFraming::none;
        std::size_t body_length_ = 0;
        std::size_t max_body_bytes_ = 0;
        Clock::time_point body_deadline_;

        Clock::time_point next_body_read_deadline() const;
        asio::awaitable<std::tuple<asio::error_code, std::size_t>> read_delimited(
            std::string_view delimiter, std::size_t max_bytes, Clock::time_point deadline);
        asio::awaitable<std::string> read_body_line(std::size_t max_bytes, int limit_status);
        asio::awaitable<void> fill_body_buffer(std::size_t bytes);
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
        body_deadline_ = Clock::now() + limits_.body_timeout;
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
            auto chunked_size_str = co_await read_body_line(limits_.chunk_line_bytes, 400);

            std::size_t chunk_size;
            try{
                chunk_size = http_sub_functions::parse_chunk_size_line(chunked_size_str);
            }
            catch(const std::runtime_error&){
                throw HttpReadError(400, "Invalid chunk size");
            }
            if(chunk_size == 0){
                std::size_t trailer_bytes = 0;
                std::size_t trailer_count = 0;
                for(;;){
                    auto trailer_line = co_await read_body_line(limits_.trailer_bytes - trailer_bytes, 431);
                    if(trailer_line.empty())
                        co_return;
                    trailer_bytes += trailer_line.size() + 2;
                    if(++trailer_count > limits_.trailer_count)
                        throw HttpReadError(431, "Too many trailer fields");
                }
            }

            if(chunk_size > max_body_bytes_ - content_length)
                throw HttpReadError(413, "Request body is too large");

            co_await read_fixed_body(body, chunk_size);

            std::string chunk_crlf;
            co_await read_fixed_body(chunk_crlf, 2);
            if(chunk_crlf != "\r\n")
                throw HttpReadError(400, "Malformed chunk data");

            content_length += chunk_size;
        }
    }

    template<typename T>
    asio::awaitable<void> HttpReceiver::read_fixed_body(T& body, std::size_t content_length){
        while(content_length > 0){
            if(buf_.empty())
                co_await fill_body_buffer(std::min(io_chunk_size_, content_length));

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

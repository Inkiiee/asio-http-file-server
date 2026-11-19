#ifndef HTTP_SENDER_H
#define HTTP_SENDER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/buffer.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

#include "http_base.h"
#include "http_util.h"

namespace http_file_server{
    class HttpSender{
    public:
        HttpSender(asio::ip::tcp::socket* socket, std::size_t io_chunk_size = 8192): psocket_(socket), io_chunk_size_(io_chunk_size){}
        virtual ~HttpSender() = default;

        asio::awaitable<void> send_no_body_response(const http_base::HttpResponse& response);
        template<typename T>
        asio::awaitable<void> send_response(const http_base::HttpResponse& response, T& body);
    private:
        asio::ip::tcp::socket* psocket_;
        std::size_t io_chunk_size_;

        template<typename T>
        asio::awaitable<void> send_chunked_body(T& body);
        template<typename T>
        asio::awaitable<void> send_fixed_body(T& body, std::size_t content_length);
    };

    template<typename T>
    asio::awaitable<void> HttpSender::send_response(const http_base::HttpResponse& response, T& body){
        if(http_sub_functions::response_must_not_have_body(response.status_code())){
            co_await send_no_body_response(response);
            co_return;
        }

        if(response.has_header("Transfer-Encoding")){
            const auto& codings = response.get_header("Transfer-Encoding");
            if(response.has_header("Content-Length") || codings.size() != 1 ||
               !http_sub_functions::iequals(codings.front(), "chunked"))
                throw std::runtime_error("Invalid response Transfer-Encoding");
            co_await send_no_body_response(response);
            co_await send_chunked_body(body);
            co_return;
        }

        const auto& lengths = response.get_header("Content-Length");
        auto length = lengths.size() == 1 ? http_base::str_to_num(lengths.front()) : std::nullopt;
        if(!length)
            throw std::runtime_error("Missing or invalid response Content-Length");

        co_await send_no_body_response(response);
        co_await send_fixed_body(body, *length);
    }

    template<typename T>
    asio::awaitable<void> HttpSender::send_chunked_body(T& body){
        std::size_t total_size = 0;
        std::size_t offset = 0;

        if constexpr (std::is_same_v<T, std::string>){
            total_size = body.size();
            while(offset < total_size){
                std::size_t current_chunk_size = std::min(io_chunk_size_, total_size - offset);
                std::string chunk_data = body.substr(offset, current_chunk_size);
                std::stringstream chunk_header;
                chunk_header << std::hex << current_chunk_size << "\r\n";

                std::string chunk = chunk_header.str();
                chunk += chunk_data;
                chunk += "\r\n";

                auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(chunk), asio::as_tuple);
                (void)bytes_transferred;
                if(ec){
                    throw std::runtime_error("Error writing chunked body: " + ec.message());
                }
                offset += current_chunk_size;
            }
        }
        else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<char>>){
            total_size = body.size();
            while(offset < total_size){
                std::size_t current_chunk_size = std::min(io_chunk_size_, total_size - offset);
                std::string chunk_data(body.begin() + offset, body.begin() + offset + current_chunk_size);
                std::stringstream chunk_header;
                chunk_header << std::hex << current_chunk_size << "\r\n";

                std::string chunk = chunk_header.str();
                chunk += chunk_data;
                chunk += "\r\n";

                auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(chunk), asio::as_tuple);
                (void)bytes_transferred;
                if(ec){
                    throw std::runtime_error("Error writing chunked body: " + ec.message());
                }
                offset += current_chunk_size;
            }
        }
        else if constexpr (std::is_base_of_v<std::istream, T>){
            // chunked는 전체 크기를 미리 알아야 하므로 seek 사용
            auto current_pos = body.tellg();
            body.seekg(0, std::ios::end);
            auto end_pos = body.tellg();
            body.seekg(current_pos);
            if(current_pos == std::streampos(-1) || end_pos == std::streampos(-1) || end_pos < current_pos)
                throw std::runtime_error("Unable to determine stream size for chunked response");

            total_size = static_cast<std::size_t>(end_pos - current_pos);

            while(offset < total_size){
                std::size_t current_chunk_size = std::min(io_chunk_size_, total_size - offset);
                std::vector<char> buffer(current_chunk_size);
                body.read(buffer.data(), current_chunk_size);
                auto bytes_actually_read = static_cast<std::size_t>(body.gcount());
                if(bytes_actually_read == 0)
                    throw std::runtime_error("Response stream ended before its announced length");
                std::stringstream chunk_header;
                chunk_header << std::hex << bytes_actually_read << "\r\n";

                std::string chunk = chunk_header.str();
                chunk.append(buffer.data(), bytes_actually_read);
                chunk += "\r\n";

                auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(chunk), asio::as_tuple);
                (void)bytes_transferred;
                if(ec){
                    throw std::runtime_error("Error writing chunked body: " + ec.message());
                }
                offset += bytes_actually_read;
            }
        }
        else
            throw std::runtime_error("Unsupported body type for write_chunked_body");

        std::string final_chunk = "0\r\n\r\n";
        auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(final_chunk), asio::as_tuple);
        (void)bytes_transferred;
        if(ec){
            throw std::runtime_error("Error writing final chunk: " + ec.message());
        }
    }

    template<typename T>
    asio::awaitable<void> HttpSender::send_fixed_body(T& body, std::size_t content_length){
        std::size_t total_size = 0;
        std::size_t offset = 0;

        if constexpr (std::is_same_v<T, std::string>){
            if(body.size() < content_length)
                throw std::runtime_error("Response body is shorter than Content-Length");
            total_size = content_length;
            while(offset < total_size){
                std::size_t current_chunk_size = std::min(io_chunk_size_, total_size - offset);
                std::string chunk_data = body.substr(offset, current_chunk_size);
                auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(chunk_data), asio::as_tuple);
                (void)bytes_transferred;
                if(ec){
                    throw std::runtime_error("Error writing fixed body: " + ec.message());
                }
                offset += current_chunk_size;
            }
        }
        else if constexpr (std::is_same_v<T, std::vector<std::uint8_t>> || std::is_same_v<T, std::vector<char>>){
            if(body.size() < content_length)
                throw std::runtime_error("Response body is shorter than Content-Length");
            total_size = content_length;
            while(offset < total_size){
                std::size_t current_chunk_size = std::min(io_chunk_size_, total_size - offset);
                std::string chunk_data(body.begin() + offset, body.begin() + offset + current_chunk_size);
                auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(chunk_data), asio::as_tuple);
                (void)bytes_transferred;
                if(ec){
                    throw std::runtime_error("Error writing fixed body: " + ec.message());
                }
                offset += current_chunk_size;
            }
        }
        else if constexpr (std::is_base_of_v<std::istream, T>){
            total_size = content_length;

            while(offset < total_size){
                std::size_t current_chunk_size = std::min(io_chunk_size_, total_size - offset);
                std::vector<char> buffer(current_chunk_size);
                body.read(buffer.data(), current_chunk_size);
                auto bytes_actually_read = body.gcount();
                if(bytes_actually_read <= 0)
                    throw std::runtime_error("Response stream ended before Content-Length bytes were sent");
                auto [ec, bytes_transferred] = co_await asio::async_write(*psocket_, asio::buffer(buffer.data(), bytes_actually_read), asio::as_tuple);
                (void)bytes_transferred;
                if(ec){
                    throw std::runtime_error("Error writing fixed body: " + ec.message());
                }
                offset += bytes_actually_read;
            }
        }
        else
            throw std::runtime_error("Unsupported body type for write_fixed_body");
    }
}

#endif //HTTP_SENDER_H

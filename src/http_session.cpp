#include "http_session.h"
#include "http_util.h"

#include <concepts>
#include <iostream>
#include <fstream>
#include <filesystem>

using namespace std;
using namespace http_file_server;
using namespace http_base;

using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::as_tuple;
namespace fs = std::filesystem;

HttpSession::HttpSession(tcp::socket socket): socket_(move(socket)) {
    socket_.set_option(tcp::no_delay(true));
}

void HttpSession::set_root_path(const fs::path& path){
    web_dav_proto_.set_root_path(path);
    proto_.set_root_path(path);
}

void HttpSession::start(){
    co_spawn(socket_.get_executor(),
        [self = shared_from_this()]() -> awaitable<void> {
            co_await self->handle_session();
        }, detached);
}

void HttpSession::stop(){
    asio::error_code ec;
    socket_.shutdown(tcp::socket::shutdown_both, ec);
    socket_.close(ec);
}

// ============================================================
//  Main session handler
// ============================================================
awaitable<void> HttpSession::handle_session(){
    try {
        while(true){
            bool is_need_close = false;
            http_base::HttpRequest request;
            co_await read_request(request);
            if(request.has_header("Connection") && request.get_header("Connection") == "close"){
                is_need_close = true;
            }

            co_await process_request(request);
            if(is_need_close){
                break;
            }
        }
    } 
    catch(const exception& e){
        cerr << "[HTTP] Session error: " << e.what() << endl;
    }

    stop();
}

awaitable<void> HttpSession::process_request(const http_base::HttpRequest& request){
    string method = request.method();
    if(method == "GET" || method == "HEAD"){
        co_await process_get_request(request);
    }
    else if(method == "POST"){
        string request_body;
        size_t content_length = 0;

        co_await read_body(request, request_body, content_length);
        cout << "[HTTP] Received POST request with body length: " << content_length << endl;
        cout << "[HTTP] Request body: " << request_body << endl;
        auto path = request.path();
        if(path == u8"/file/copy"){
            auto [response, body] = proto_.copy_file({request, request_body});
            co_await write_response(response, body);
        }
        else if(path == u8"/file/move"){
            auto [response, body] = proto_.move_file({request, request_body});
            co_await write_response(response, body);
        }
        else if(path == u8"/file/delete"){
            auto [response, body] = proto_.delete_file({request, request_body});
            co_await write_response(response, body);
        }
        else if(path == u8"/directory/create"){
            auto [response, body] = proto_.create_directory({request, request_body});
            co_await write_response(response, body);
        }
        else{
            http_base::HttpResponse response;
            response.set_status_code(404);
            response.add_header("Content-Length", "0");
            co_await write_no_body_response(response);
        }
    }
    else if(method == "PUT"){
        co_await process_put_request(request);
    }
    else if(method == "DELETE"){
        auto [response, body] = web_dav_proto_.delete_file({request, ""});
        co_await write_response(response, body);
    }
    else if(method == "OPTIONS"){
        HttpResponse response;
        response.set_status_code(200);
        response.add_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        response.add_header("DAV", "1, 2");
        response.add_header("Ms-Author-Via", "DAV");
        response.add_header("Content-Length", "0");

        co_await write_no_body_response(response);
    }
    else if(method == "PROPFIND"){
        string request_body;
        size_t content_length = 0;
        co_await read_body(request, request_body, content_length);
        auto [response, body] = web_dav_proto_.make_file_list_info({request, request_body});
        co_await write_response(response, body);
    }
    else if(method == "PROPPATCH"){
        string request_body;
        size_t content_length = 0;
        co_await read_body(request, request_body, content_length);

        auto [response, body] = web_dav_proto_.proppatch({request, request_body});
        co_await write_response(response, body);
    }
    else if(method == "MKCOL"){
        auto [response, body] = web_dav_proto_.create_directory({request, ""});
        co_await write_response(response, body);
    }
    else if(method == "COPY"){
        auto [response, body] = web_dav_proto_.copy_file({request, ""});
        co_await write_response(response, body);
    }
    else if(method == "MOVE"){
        auto [response, body] = web_dav_proto_.move_file({request, ""});
        co_await write_response(response, body);
    }
    else if(method == "LOCK"){
        string request_body;
        size_t content_length = 0;
        co_await read_body(request, request_body, content_length);

        auto [response, body] = web_dav_proto_.lock_resource({request, request_body});
        co_await write_response(response, body);
    }
    else if(method == "UNLOCK"){
        HttpResponse response;
        response.set_status_code(204);
        response.add_header("Content-Length", "0");
        co_await write_no_body_response(response);
    }
    else{
        http_base::HttpResponse response;
        response.set_status_code(405);
        response.add_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        response.add_header("Content-Length", "0");
        co_await write_no_body_response(response);
    }
}

awaitable<void> HttpSession::process_get_request(const http_base::HttpRequest& request){
    if(request.path().empty()){
        cout << "[HTTP] Empty path in GET request" << endl;
        HttpResponse response;
        response.set_status_code(400);
        response.add_header("Content-Length", "0");
        co_await write_no_body_response(response);

        co_return;
    }
    
    if(proto_.is_directory(request.path())){
        auto [response, body] = proto_.make_file_list_info({request, ""});
        co_await write_response(response, body);
    }
    else{
        auto path = proto_.download_file_path({request, ""});
        if(!fs::exists(path)){
            http_base::HttpResponse response;
            response.set_status_code(404);
            response.add_header("Content-Length", "0");
            co_await write_no_body_response(response);
            co_return;
        }

        auto file_size = fs::file_size(path);
        http_base::HttpResponse response;
        response.set_status_code(200);
        response.add_header("Content-Length", std::to_string(file_size));
        response.add_header("Content-Type", http_base::get_mime_type(path.extension().string()));
        response.add_header("Accept-Ranges", "bytes");
        response.add_header("ETag", web_dav_proto_.get_etag(request.path()));

        std::ifstream file(path, std::ios::binary);
        if(!file){
            http_base::HttpResponse response;
            response.set_status_code(500);
            response.add_header("Content-Length", "0");
            co_await write_no_body_response(response);
            co_return;
        }

        if(request.has_header("Range")){
            size_t start, end;
            if(http_base::parse_range(request.get_header("Range"), file_size, start, end)){
                response.set_status_code(206);
                response.add_header("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(file_size));
                size_t content_length = end - start + 1;
                response.add_header("Content-Length", std::to_string(content_length));

                if(request.method() == "HEAD"){
                    co_await write_no_body_response(response);
                    co_return;
                }

                file.seekg(start);
                co_await write_response(response, file);
            }
            else{
                response.set_status_code(416);
                response.add_header("Content-Length", "0");
                co_await write_no_body_response(response);
            }
        }
        else{
            co_await write_response(response, file);
        }
    }
}

awaitable<void> HttpSession::process_put_request(const http_base::HttpRequest& request){
    bool is_existing_file = proto_.is_exists(request.path());
    bool is_expect_continue = request.has_header("Expect") && request.get_header("Expect") == "100-continue";
    auto path = web_dav_proto_.upload_file_path({request, ""});
    if(!web_dav_proto_.is_valid_path(path)){
        cout << "[HTTP] PUT request failed: path is invalid" << endl;
        cout << "[HTTP] Accessed Path: " << path << endl;

        HttpResponse response;
        response.set_status_code(400); // Bad Request
        response.add_header("Content-Length", "0");
        response.add_header("Connection", "close");
        co_await write_no_body_response(response);

        throw std::runtime_error("PUT failed: invalid path");
    }

    if(request.has_header("If-None-Match")){
        string if_none_match = request.get_header("If-None-Match");
        if(if_none_match == "*"){
            if(is_existing_file){
                cout << "[HTTP] PUT request failed: resource already exists and If-None-Match is '*'" << endl;

                HttpResponse response;
                response.set_status_code(412); // Precondition Failed
                response.add_header("Content-Length", "0");
                response.add_header("Connection", "close");
                co_await write_no_body_response(response);

                throw std::runtime_error("PUT failed: precondition failed (If-None-Match)");
            }
        }
    }

    if(request.has_header("If-Match")){
        string if_match = request.get_header("If-Match");
        auto&& entry = web_dav_proto_.get_file_info(fs::path(request.path()));
        if(if_match != entry.etag){
            cout << "[HTTP] PUT request failed: ETag does not match If-Match header" << endl;
            cout << "[HTTP] If-Match: " << if_match << ", ETag: " << entry.etag << endl;

            HttpResponse response;
            response.set_status_code(412); // Precondition Failed
            response.add_header("Content-Length", "0");
            response.add_header("Connection", "close");
            co_await write_no_body_response(response);

            throw std::runtime_error("PUT failed: precondition failed (If-Match)");
        }
    }

    if(request.has_header("If-Unmodified-Since")){
        string if_unmodified_since = request.get_header("If-Unmodified-Since");
        auto unmodified_since = web_dav_proto_.parse_http_date(if_unmodified_since);
        if(!unmodified_since){
            cout << "[HTTP] PUT request failed: Invalid If-Unmodified-Since header value" << endl;
            cout << "[HTTP] If-Unmodified-Since: " << if_unmodified_since << endl;

            HttpResponse response;
            response.set_status_code(400); // Bad Request
            response.add_header("Content-Length", "0");
            response.add_header("Connection", "close");
            co_await write_no_body_response(response);

            throw std::runtime_error("PUT failed: invalid If-Unmodified-Since");
        }

        auto&& entry = web_dav_proto_.get_file_info(fs::path(request.path()));
        auto last_modified = web_dav_proto_.parse_http_date(entry.last_modified);
        if(last_modified && *last_modified > *unmodified_since){
            cout << "[HTTP] PUT request failed: Resource has been modified since the specified time" << endl;
            cout << "[HTTP] If-Unmodified-Since: " << if_unmodified_since << endl;
            cout << "[HTTP] Last-Modified: " << entry.last_modified << endl;

            HttpResponse response;
            response.set_status_code(412); // Precondition Failed
            response.add_header("Content-Length", "0");
            response.add_header("Connection", "close");
            co_await write_no_body_response(response);

            throw std::runtime_error("PUT failed: precondition failed (If-Unmodified-Since)");
        }
    }

    std::ofstream file(path, std::ios::binary);
    if(!file){
        cout << "[HTTP] PUT request failed: Unable to open file for writing" << endl;
        cout << "[HTTP] Path: " << path << endl;

        HttpResponse response;
        response.set_status_code(500); // Internal Server Error
        response.add_header("Content-Length", "0");
        response.add_header("Connection", "close");
        co_await write_no_body_response(response);

        throw std::runtime_error("PUT failed: cannot open file");
    }

    if(is_expect_continue){
        HttpResponse continue_response;
        continue_response.set_status_code(100); // Continue
        continue_response.add_header("Content-Length", "0");
        co_await write_no_body_response(continue_response);
    }

    size_t content_length = 0;
    co_await read_body(request, file, content_length);
    file.close();

    HttpResponse response;
    if(is_existing_file){
        response.set_status_code(200);
    } else {
        response.set_status_code(201);
    }
    response.add_header("Content-Length", "0");
    co_await write_no_body_response(response);

    cout<< "[HTTP] PUT request completed: " << content_length << " bytes written to " << path << endl;
}

awaitable<void> HttpSession::read_request(http_base::HttpRequest& request){
    auto [ec, bytes_transferred] = co_await async_read_until(socket_, asio::dynamic_buffer(buf_), "\r\n\r\n", as_tuple);
    if(ec){
        throw std::runtime_error("Connection closed: " + ec.message());
    }

    string request_data = buf_.substr(0, bytes_transferred);
    buf_.erase(0, bytes_transferred);

    http_parser::Parser parser(request_data);
    request = parser.parse_request();
    string method = request.method();
    auto path = request.path();
    string path_str(path.begin(), path.end());
    string version = request.version();

    cout << "[HTTP] Received request: " << method << " " << path_str << " " << version << endl;
    for(const auto& header : request.headers()){
        cout << "[HTTP] Header: " << header.first << ": " << header.second << endl;
    }
    co_return;
}

awaitable<void> HttpSession::read_until(string& buffer, const string& delimiter){
    auto [ec, bytes_transferred] = co_await asio::async_read_until(socket_, asio::dynamic_buffer(buffer), delimiter, as_tuple);
    if(ec){
        throw std::runtime_error("Error reading: " + ec.message());
    }

    buffer.assign(buffer.begin(), buffer.begin() + bytes_transferred);
    buf_.erase(0, bytes_transferred);
}

template<typename T>
awaitable<void> HttpSession::read_body(const HttpRequest& request, T& body, size_t& content_length){
    if(request.has_header("Transfer-Encoding") && request.get_header("Transfer-Encoding") == "chunked"){
        co_await read_chunked_body(body, content_length);
    }
    else if(request.has_header("Content-Length")){
        content_length = std::stoul(request.get_header("Content-Length"));
        co_await read_fixed_body(body, content_length);
    }
    else{
        content_length = 0;
        co_return;
    }
}

template<typename T>
awaitable<void> HttpSession::read_chunked_body(T& body, size_t& content_length){
    std::string chunked_size_str;
    co_await read_until(chunked_size_str, "\r\n");

    size_t chunk_size = std::stoul(chunked_size_str, nullptr, 16);
    if(chunk_size == 0){
        string trailing_crlf;
        co_await read_until(trailing_crlf, "\r\n");
        co_return;
    }

    std::string chunk_data;
    co_await read_until(chunk_data, "\r\n");

    if constexpr (is_same_v<T, string>)
        body += chunk_data;
    else if constexpr (is_same_v<T, vector<uint8_t>> || is_same_v<T, vector<char>>) 
        body.insert(body.end(), chunk_data.begin(), chunk_data.end());
    else if constexpr (is_base_of_v<std::ostream, T>) 
        body.write(chunk_data.data(), chunk_data.size());
    else
        throw std::runtime_error("Unsupported body type for read_chunked_body");
}

template<typename T>
awaitable<void> HttpSession::read_fixed_body(T& body, size_t content_length){
    size_t bytes_to_read = content_length;
    size_t bytes_read = 0;
    size_t default_chunk_size = 8192;

    // 버퍼에 이미 남아있는 데이터 먼저 처리
    if(!buf_.empty()){
        size_t buffered = std::min(buf_.size(), bytes_to_read);
        string chunk_data(buf_.begin(), buf_.begin() + buffered);
        buf_.erase(0, buffered);
        if constexpr (is_same_v<T, string>)
            body += chunk_data;
        else if constexpr (is_same_v<T, vector<uint8_t>> || is_same_v<T, vector<char>>)
            body.insert(body.end(), chunk_data.begin(), chunk_data.end());
        else if constexpr (is_base_of_v<std::ostream, T>)
            body.write(chunk_data.data(), chunk_data.size());
        else
            throw std::runtime_error("Unsupported body type for read_fixed_body");
        bytes_read += buffered;
    }

    while(bytes_read < bytes_to_read){
        size_t chunk_size = std::min(default_chunk_size, bytes_to_read - bytes_read);
        auto [ec, bytes_transferred] = co_await asio::async_read(socket_, asio::dynamic_buffer(buf_), asio::transfer_exactly(chunk_size), as_tuple);
        if(ec){
            throw std::runtime_error("Error reading fixed body: " + ec.message());
        }

        string chunk_data(buf_.begin(), buf_.begin() + bytes_transferred);
        buf_.erase(0, bytes_transferred);
        if constexpr (is_same_v<T, string>)
            body += chunk_data;
        else if constexpr (is_same_v<T, vector<uint8_t>> || is_same_v<T, vector<char>>) 
            body.insert(body.end(), chunk_data.begin(), chunk_data.end());
        else if constexpr (is_base_of_v<std::ostream, T>) 
            body.write(chunk_data.data(), chunk_data.size());
        else
            throw std::runtime_error("Unsupported body type for read_fixed_body");

        bytes_read += bytes_transferred;
    }
}

awaitable<void> HttpSession::write_no_body_response(const http_base::HttpResponse& response){
    auto response_str = http_maker::Maker::make_response(response);
    auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(response_str), asio::as_tuple);
    if(ec){
        throw std::runtime_error("Error writing response: " + ec.message());
    }
}

template<typename T>
asio::awaitable<void> HttpSession::write_response(const http_base::HttpResponse& response, T& body){
    auto response_str = http_maker::Maker::make_response(response);

    auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(response_str), asio::as_tuple);
    if(ec){
        throw std::runtime_error("Error writing response: " + ec.message());
    }
    if(response.has_header("Transfer-Encoding") && response.get_header("Transfer-Encoding") == "chunked")
        co_await write_chunked_body(body);
    else if(response.has_header("Content-Length")){
        size_t content_length = std::stoul(response.get_header("Content-Length"));
        co_await write_fixed_body(body, content_length);
    }
}
template<typename T>
asio::awaitable<void> HttpSession::write_chunked_body(T& body){
    size_t chunk_size = 8192;
    size_t total_size = 0;
    size_t offset = 0;

    if constexpr (is_same_v<T, string>){
        total_size = body.size();
        while(offset < total_size){
            size_t current_chunk_size = std::min(chunk_size, total_size - offset);
            string chunk_data = body.substr(offset, current_chunk_size);
            stringstream chunk_header;
            chunk_header << std::hex << current_chunk_size << "\r\n";

            auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(chunk_header.str() + chunk_data + "\r\n"), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error writing chunked body: " + ec.message());
            }
            offset += current_chunk_size;
        }
    }
    else if constexpr (is_same_v<T, vector<uint8_t>> || is_same_v<T, vector<char>>){
        total_size = body.size();
        while(offset < total_size){
            size_t current_chunk_size = std::min(chunk_size, total_size - offset);
            string chunk_data(body.begin() + offset, body.begin() + offset + current_chunk_size);
            stringstream chunk_header;
            chunk_header << std::hex << current_chunk_size << "\r\n";

            auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(chunk_header.str() + chunk_data + "\r\n"), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error writing chunked body: " + ec.message());
            }
            offset += current_chunk_size;
        }
    }
    else if constexpr (is_base_of_v<std::istream, T>){
        // chunked는 전체 크기를 미리 알아야 하므로 seek 사용
        auto current_pos = body.tellg();
        body.seekg(0, std::ios::end);
        total_size = body.tellg();
        body.seekg(current_pos);

        while(offset < total_size){
            size_t current_chunk_size = std::min(chunk_size, total_size - offset);
            std::vector<char> buffer(current_chunk_size);
            body.read(buffer.data(), current_chunk_size);
            auto bytes_actually_read = body.gcount();
            if(bytes_actually_read <= 0) break;
            string chunk_data(buffer.data(), bytes_actually_read);
            stringstream chunk_header;
            chunk_header << std::hex << bytes_actually_read << "\r\n";

            auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(chunk_header.str() + chunk_data + "\r\n"), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error writing chunked body: " + ec.message());
            }
            offset += current_chunk_size;
        }
    }
    else
        throw std::runtime_error("Unsupported body type for write_chunked_body");
}
template<typename T>
asio::awaitable<void> HttpSession::write_fixed_body(T& body, std::size_t content_length){
    size_t chunk_size = 8192;
    size_t total_size = 0;
    size_t offset = 0;

    if constexpr (is_same_v<T, string>){
        total_size = body.size();
        while(offset < total_size){
            size_t current_chunk_size = std::min(chunk_size, total_size - offset);
            string chunk_data = body.substr(offset, current_chunk_size);
            auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(chunk_data), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error writing fixed body: " + ec.message());
            }
            offset += current_chunk_size;
        }
    }
    else if constexpr (is_same_v<T, vector<uint8_t>> || is_same_v<T, vector<char>>){
        total_size = body.size();
        while(offset < total_size){
            size_t current_chunk_size = std::min(chunk_size, total_size - offset);
            string chunk_data(body.begin() + offset, body.begin() + offset + current_chunk_size);
            auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(chunk_data), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error writing fixed body: " + ec.message());
            }
            offset += current_chunk_size;
        }
    }
    else if constexpr (is_base_of_v<std::istream, T>){
        total_size = content_length;

        while(offset < total_size){
            size_t current_chunk_size = std::min(chunk_size, total_size - offset);
            std::vector<char> buffer(current_chunk_size);
            body.read(buffer.data(), current_chunk_size);
            auto bytes_actually_read = body.gcount();
            if(bytes_actually_read <= 0) break;
            auto [ec, bytes_transferred] = co_await asio::async_write(socket_, asio::buffer(buffer.data(), bytes_actually_read), asio::as_tuple);
            if(ec){
                throw std::runtime_error("Error writing fixed body: " + ec.message());
            }
            offset += bytes_actually_read;
        }
    }
    else
        throw std::runtime_error("Unsupported body type for write_fixed_body");
}
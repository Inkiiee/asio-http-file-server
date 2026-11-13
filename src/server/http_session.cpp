#include "http_session.h"
#include "http_util.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <asio.hpp>

using namespace std;
using namespace http_file_server;
using namespace http_base;
using namespace http_sub_functions;

using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;

namespace fs = std::filesystem;

HttpSession::HttpSession(tcp::socket socket): socket_(move(socket)), sender_(&socket_), receiver_(&socket_) {}

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
            co_await receiver_.read_request(request, is_need_close);

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
    std::cout << "[HTTP] Processing request: " << method << " " << string(request.path().begin(), request.path().end()) << std::endl;

    if(method == "GET" || method == "HEAD")
        co_await process_get_request(request);
    else if(method == "POST"){
        co_await process_post_request(request);
    }
    else if(method == "PUT"){
        co_await process_put_request(request);
    }
    else if(method == "DELETE"){
        auto [response, body] = web_dav_proto_.delete_file({request, ""});
        co_await sender_.send_response(response, body);
    }
    else if(method == "OPTIONS"){
        HttpResponse response;
        response.set_status_code(200);
        response.add_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        response.add_header("DAV", "1, 2");
        response.add_header("Ms-Author-Via", "DAV");
        response.add_header("Content-Length", "0");

        co_await sender_.send_no_body_response(response);
    }
    else if(method == "PROPFIND"){
        string request_body;
        size_t content_length = 0;
        co_await receiver_.read_body(request, request_body, content_length);
        auto [response, body] = web_dav_proto_.make_file_list_info({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else if(method == "PROPPATCH"){
        string request_body;
        size_t content_length = 0;
        co_await receiver_.read_body(request, request_body, content_length);

        auto [response, body] = web_dav_proto_.proppatch({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else if(method == "MKCOL"){
        auto [response, body] = web_dav_proto_.create_directory({request, ""});
        co_await sender_.send_response(response, body);
    }
    else if(method == "COPY"){
        auto [response, body] = web_dav_proto_.copy_file({request, ""});
        co_await sender_.send_response(response, body);
    }
    else if(method == "MOVE"){
        auto [response, body] = web_dav_proto_.move_file({request, ""});
        co_await sender_.send_response(response, body);
    }
    else if(method == "LOCK"){
        string request_body;
        size_t content_length = 0;
        co_await receiver_.read_body(request, request_body, content_length);

        auto [response, body] = web_dav_proto_.lock_resource({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else if(method == "UNLOCK"){
        co_await sender_.send_no_body_response(204);
    }
    else{
        http_base::HttpResponse response;
        response.set_status_code(405);
        response.add_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        response.add_header("Content-Length", "0");
        co_await sender_.send_no_body_response(response);
    }
}

awaitable<void> HttpSession::process_get_request(const http_base::HttpRequest& request){
    if(request.path().empty()){
        cout << "[HTTP] Empty path in GET request" << endl;
        co_await sender_.send_no_body_response(400);
        co_return;
    }
    
    if(proto_.is_directory(request.path())){
        auto [response, body] = proto_.make_file_list_info({request, ""});
        if(request.method() == "HEAD")
            co_await sender_.send_no_body_response(response);
        else
            co_await sender_.send_response(response, body);
    }
    else{
        auto path = proto_.download_file_path({request, ""});
        if(!proto_.is_valid_path(path)){ // block access unallowed space
            co_await sender_.send_no_body_response(400);
            co_return;
        }

        if(!fs::exists(path)){
            co_await sender_.send_no_body_response(404);
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
            co_await sender_.send_no_body_response(500);
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
                    co_await sender_.send_no_body_response(response);
                    co_return;
                }

                file.seekg(start);
                co_await sender_.send_response(response, file);
            }
            else{
                response.set_status_code(416);
                response.add_header("Content-Length", "0");
                co_await sender_.send_no_body_response(response);
            }
        }
        else{
            if(request.method() == "HEAD"){
                co_await sender_.send_no_body_response(response);
                co_return;
            }

            co_await sender_.send_response(response, file);
        }
    }
}

awaitable<void> HttpSession::process_put_request(const http_base::HttpRequest& request){
    bool is_existing_file = web_dav_proto_.is_exists(request.path());
    bool is_expect_continue = request.has_header("Expect") && iequals(request.get_header("Expect"), "100-continue");
    auto path = web_dav_proto_.upload_file_path({request, ""});
    if(!web_dav_proto_.is_valid_path(path)){
        cout << "[HTTP] PUT request failed: path is invalid" << endl;
        cout << "[HTTP] Accessed Path: " << path << endl;
        co_await sender_.send_no_body_response(400);

        throw std::runtime_error("PUT failed: invalid path");
    }

    if(request.has_header("If-None-Match")){
        string if_none_match = request.get_header("If-None-Match");
        if(if_none_match == "*"){
            if(is_existing_file){
                cout << "[HTTP] PUT request failed: resource already exists and If-None-Match is '*'" << endl;
                co_await sender_.send_no_body_response(412);

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
            co_await sender_.send_no_body_response(412);

            throw std::runtime_error("PUT failed: precondition failed (If-Match)");
        }
    }

    if(request.has_header("If-Unmodified-Since")){
        string if_unmodified_since = request.get_header("If-Unmodified-Since");
        auto unmodified_since = web_dav_proto_.parse_http_date(if_unmodified_since);
        if(!unmodified_since){
            cout << "[HTTP] PUT request failed: Invalid If-Unmodified-Since header value" << endl;
            cout << "[HTTP] If-Unmodified-Since: " << if_unmodified_since << endl;
            co_await sender_.send_no_body_response(400);

            throw std::runtime_error("PUT failed: invalid If-Unmodified-Since");
        }

        auto&& entry = web_dav_proto_.get_file_info(fs::path(request.path()));
        auto last_modified = web_dav_proto_.parse_http_date(entry.last_modified);
        if(last_modified && *last_modified > *unmodified_since){
            cout << "[HTTP] PUT request failed: Resource has been modified since the specified time" << endl;
            cout << "[HTTP] If-Unmodified-Since: " << if_unmodified_since << endl;
            cout << "[HTTP] Last-Modified: " << entry.last_modified << endl;
            co_await sender_.send_no_body_response(412);

            throw std::runtime_error("PUT failed: precondition failed (If-Unmodified-Since)");
        }
    }

    auto parent_path = path.parent_path();
    auto temp_file = parent_path / (path.filename().string() + ".tmp");
    std::ofstream file(temp_file, std::ios::binary);
    if(!file){
        cout << "[HTTP] PUT request failed: Unable to open file for writing" << endl;
        cout << "[HTTP] Path: " << path << endl;
        co_await sender_.send_no_body_response(500);

        std::error_code ignore_ec;
        fs::remove(temp_file, ignore_ec);
        throw std::runtime_error("PUT failed: cannot open file");
    }

    if(is_expect_continue){
        co_await sender_.send_no_body_response(100);
    }

    size_t content_length = 0;
    co_await receiver_.read_body(request, file, content_length);
    file.close();
    
    std::error_code ignore_ec;
    fs::rename(temp_file, path, ignore_ec);

    if(is_existing_file)
        co_await sender_.send_no_body_response(200);
    else 
        co_await sender_.send_no_body_response(201); // Created

    cout<< "[HTTP] PUT request completed: " << content_length << " bytes written to " << path << endl;
}

awaitable<void> HttpSession::process_post_request(const HttpRequest& request){
    string request_body;
    size_t content_length = 0;
    co_await receiver_.read_body(request, request_body, content_length);

    cout << "[HTTP] Request body: " << request_body << endl;
    auto path = request.path();
    if(path == u8"/file/copy"){
        auto [response, body] = proto_.copy_file({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else if(path == u8"/file/move"){
        auto [response, body] = proto_.move_file({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else if(path == u8"/file/delete"){
        auto [response, body] = proto_.delete_file({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else if(path == u8"/directory/create"){
        auto [response, body] = proto_.create_directory({request, request_body});
        co_await sender_.send_response(response, body);
    }
    else{
        co_await sender_.send_no_body_response(404);
        throw std::runtime_error("Unknown POST path: " + string(path.begin(), path.end()));
    }
}


#include "http_session.h"
#include "http_util.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
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

namespace {
    enum class ETagComparison { strong, weak };

    bool matches_etag(const vector<string>& tags, string_view current_tag, bool exists, ETagComparison comparison){
        if(!exists)
            return false;
        if(tags.size() == 1 && tags.front() == "*")
            return true;
        if(current_tag.empty())
            return false;

        if(comparison == ETagComparison::strong){
            return !current_tag.starts_with("W/") && find(tags.begin(), tags.end(), current_tag) != tags.end();
        }

        auto opaque_tag = [](string_view tag){
            return tag.starts_with("W/") ? tag.substr(2) : tag;
        };
        const auto current_opaque_tag = opaque_tag(current_tag);
        return ranges::any_of(tags, [&](string_view tag){ return opaque_tag(tag) == current_opaque_tag; });
    }
}

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
            http_base::HttpRequest request;
            if(!co_await receiver_.read_request(request)){
                close_after_response_ = true;
                co_await send_no_body_response(400);
                break;
            }

            close_after_response_ = request.version() == "HTTP/1.0" ||
                headers_has_token(request.get_header("Connection"), "close");
            co_await process_request(request);
            if(close_after_response_){
                break;
            }
        }
    }
    catch(const exception& e){
        cerr << "[HTTP] Session error: " << e.what() << endl;
    }

    stop();
}

void HttpSession::prepare_final_response(HttpResponse& response){
    close_after_response_ = close_after_response_ || !receiver_.body_complete() ||
        headers_has_token(response.get_header("Connection"), "close");
    if(close_after_response_)
        response.set_header("Connection", "close");
}

awaitable<void> HttpSession::send_no_body_response(HttpResponse& response){
    if(response.status_code() >= 200)
        prepare_final_response(response);
    co_await sender_.send_no_body_response(response);
}

awaitable<void> HttpSession::send_no_body_response(int status_code){
    HttpResponse response;
    response.set_status_code(status_code);
    if(!response_must_not_have_body(status_code))
        response.set_header("Content-Length", "0");
    co_await send_no_body_response(response);
}

awaitable<void> HttpSession::process_request(const http_base::HttpRequest& request){
    string method = request.method();
    std::cout << "[HTTP] Processing request: " << method << " " << string(request.path().begin(), request.path().end()) << std::endl;

    const bool ignores_body = method == "GET" || method == "HEAD" || method == "OPTIONS" ||
        method == "DELETE" || method == "MKCOL" || method == "COPY" || method == "MOVE" || method == "UNLOCK";
    if(ignores_body && !receiver_.body_complete() && !close_after_response_ && !request.has_header("Expect"))
        co_await receiver_.discard_body();

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
        co_await send_response(response, body);
    }
    else if(method == "OPTIONS"){
        HttpResponse response;
        response.set_status_code(200);
        response.set_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        response.set_header("DAV", "1, 2");
        response.set_header("Ms-Author-Via", "DAV");
        response.set_header("Content-Length", "0");

        co_await send_no_body_response(response);
    }
    else if(method == "PROPFIND"){
        string request_body;
        size_t content_length = 0;
        co_await read_body(request, request_body, content_length);
        auto [response, body] = web_dav_proto_.make_file_list_info({request, request_body});
        co_await send_response(response, body);
    }
    else if(method == "PROPPATCH"){
        string request_body;
        size_t content_length = 0;
        co_await read_body(request, request_body, content_length);

        auto [response, body] = web_dav_proto_.proppatch({request, request_body});
        co_await send_response(response, body);
    }
    else if(method == "MKCOL"){
        auto [response, body] = web_dav_proto_.create_directory({request, ""});
        co_await send_response(response, body);
    }
    else if(method == "COPY"){
        auto [response, body] = web_dav_proto_.copy_file({request, ""});
        co_await send_response(response, body);
    }
    else if(method == "MOVE"){
        auto [response, body] = web_dav_proto_.move_file({request, ""});
        co_await send_response(response, body);
    }
    else if(method == "LOCK"){
        string request_body;
        size_t content_length = 0;
        co_await read_body(request, request_body, content_length);

        auto [response, body] = web_dav_proto_.lock_resource({request, request_body});
        co_await send_response(response, body);
    }
    else if(method == "UNLOCK"){
        co_await send_no_body_response(204);
    }
    else{
        http_base::HttpResponse response;
        response.set_status_code(405);
        response.set_header("Allow", "GET, HEAD, POST, PUT, DELETE, OPTIONS, PROPFIND, PROPPATCH, MKCOL, COPY, MOVE, LOCK, UNLOCK");
        response.set_header("Content-Length", "0");
        co_await send_no_body_response(response);
    }
}

awaitable<void> HttpSession::process_get_request(const http_base::HttpRequest& request){
    if(request.path().empty()){
        cout << "[HTTP] Empty path in GET request" << endl;
        co_await send_no_body_response(400);
        co_return;
    }
    
    if(proto_.is_directory(request.path())){
        auto [response, body] = proto_.make_file_list_info({request, ""});
        if(request.method() == "HEAD")
            co_await send_no_body_response(response);
        else
            co_await send_response(response, body);
    }
    else{
        auto path = proto_.download_file_path({request, ""});
        if(!proto_.is_valid_path(path)){ // block access unallowed space
            co_await send_no_body_response(400);
            co_return;
        }

        if(!fs::exists(path)){
            co_await send_no_body_response(404);
            co_return;
        }

        auto file_size = fs::file_size(path);
        http_base::HttpResponse response;
        response.set_status_code(200);
        response.set_header("Content-Length", std::to_string(file_size));
        response.set_header("Content-Type", http_base::get_mime_type(path.extension().string()));
        response.set_header("Accept-Ranges", "bytes");
        response.set_header("ETag", web_dav_proto_.get_etag(request.path()));

        std::ifstream file(path, std::ios::binary);
        if(!file){
            co_await send_no_body_response(500);
            co_return;
        }

        if(request.has_header("Range")){
            size_t start, end;
            if(http_base::parse_range(request.get_header("Range")[0], file_size, start, end)){
                response.set_status_code(206);
                response.set_header("Content-Range", "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" + std::to_string(file_size));
                size_t content_length = end - start + 1;
                response.set_header("Content-Length", std::to_string(content_length));

                if(request.method() == "HEAD"){
                    co_await send_no_body_response(response);
                    co_return;
                }

                file.seekg(start);
                co_await send_response(response, file);
            }
            else{
                response.set_status_code(416);
                response.set_header("Content-Length", "0");
                co_await send_no_body_response(response);
            }
        }
        else{
            if(request.method() == "HEAD"){
                co_await send_no_body_response(response);
                co_return;
            }

            co_await send_response(response, file);
        }
    }
}

awaitable<void> HttpSession::process_put_request(const http_base::HttpRequest& request){
    auto path = web_dav_proto_.upload_file_path({request, ""});
    if(!web_dav_proto_.is_valid_path(path)){
        cout << "[HTTP] PUT request failed: path is invalid" << endl;
        cout << "[HTTP] Accessed Path: " << path << endl;
        co_await send_no_body_response(400);
        co_return;
    }

    if(!request.has_header("Content-Length") && !request.has_header("Transfer-Encoding")){
        co_await send_no_body_response(411);
        co_return;
    }

    const bool has_if_match = request.has_header("If-Match");
    const bool check_unmodified_since = !has_if_match && request.has_header("If-Unmodified-Since");
    const bool has_if_none_match = request.has_header("If-None-Match");
    auto precondition_failure = [&](bool exists) -> optional<int> {
        if(!has_if_match && !check_unmodified_since && !has_if_none_match)
            return nullopt;

        auto entry = exists ? web_dav_proto_.get_file_info(request.path()) : http_file_system::HttpFileEntry{};
        if(has_if_match && !matches_etag(request.get_header("If-Match"), entry.etag, exists, ETagComparison::strong))
            return 412;

        if(check_unmodified_since){
            auto unmodified_since = web_dav_proto_.parse_http_date(request.get_header("If-Unmodified-Since").front());
            if(!unmodified_since)
                return 400;

            auto last_modified = web_dav_proto_.parse_http_date(entry.last_modified);
            if(last_modified && *last_modified > *unmodified_since)
                return 412;
        }

        if(has_if_none_match && matches_etag(request.get_header("If-None-Match"), entry.etag, exists, ETagComparison::weak))
            return 412;

        return nullopt;
    };

    int status_code = 500;
    size_t content_length = 0;
    try{
        auto failure = precondition_failure(web_dav_proto_.is_exists(request.path()));
        if(!failure){
            http_file_system::TempName temp(path);
            std::ofstream file(temp.get_path(), std::ios::binary);
            if(!file)
                throw std::runtime_error("cannot open file");

            co_await read_body(request, file, content_length);
            file.close();
            if(!file)
                throw std::runtime_error("Failed to write upload file");

            // 본문 수신 중 다른 요청이 대상 파일을 변경했을 수 있다.
            const bool exists = web_dav_proto_.is_exists(request.path());
            failure = precondition_failure(exists);
            if(!failure){
                fs::rename(temp.get_path(), path);
                status_code = exists ? 200 : 201;
            }
        }

        if(failure)
            status_code = *failure;
    }
    catch(const exception& e){
        cerr << "[HTTP] PUT Failed: " << e.what() << endl;
    }

    co_await send_no_body_response(status_code);
    if(status_code < 400)
        cout << "[HTTP] PUT request completed: " << content_length << " bytes written to " << path << endl;
}

awaitable<void> HttpSession::process_post_request(const HttpRequest& request){
    string request_body;
    size_t content_length = 0;
    co_await read_body(request, request_body, content_length);

    cout << "[HTTP] Request body: " << request_body << endl;
    auto path = request.path();
    if(path == u8"/file/copy"){
        auto [response, body] = proto_.copy_file({request, request_body});
        co_await send_response(response, body);
    }
    else if(path == u8"/file/move"){
        auto [response, body] = proto_.move_file({request, request_body});
        co_await send_response(response, body);
    }
    else if(path == u8"/file/delete"){
        auto [response, body] = proto_.delete_file({request, request_body});
        co_await send_response(response, body);
    }
    else if(path == u8"/directory/create"){
        auto [response, body] = proto_.create_directory({request, request_body});
        co_await send_response(response, body);
    }
    else{
        co_await send_no_body_response(404);
    }
}


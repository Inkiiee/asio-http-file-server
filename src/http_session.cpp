#include "http_session.h"

#include <asio.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <utility>
#include <vector>
#include <string>
#include <algorithm>
#include <iostream>
#include <unordered_map>

using namespace std;
using namespace http_file_server;
using asio::ip::tcp;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::as_tuple;

namespace fs = std::filesystem;

// Static members
fs::path HttpSession::root_path_ = fs::current_path();

HttpSession::HttpSession(tcp::socket socket): socket_(std::move(socket)) {
    socket_.set_option(tcp::no_delay(true));
}

void HttpSession::set_root_path(const fs::path& path){
    root_path_ = fs::canonical(path);
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
        string method;
        u8string path;
        string version;
        vector<pair<string, string>> headers;
        string extra_body;

        co_await read_request(method, path, version, headers, extra_body);

        cout << "[HTTP] " << method << " "
             << string(reinterpret_cast<const char*>(path.data()), path.size())
             << " " << version << endl;

        bool is_head = (method == "HEAD");
        if(method != "GET" && method != "HEAD" && method != "POST" && method != "PUT"){
            vector<pair<string, string>> h;
            h.emplace_back("Allow", "GET, HEAD, POST, PUT");
            h.emplace_back("Connection", "close");
            co_await send_response(405, "Method Not Allowed", h,
                "<html><body><h1>405 Method Not Allowed</h1></body></html>");
            co_return;
        }

        if(method == "POST"){
            co_await handle_post_request(path, headers, extra_body);
            co_return;
        }
        else if(method == "PUT"){
            co_await handle_put_request(path, headers, extra_body);
            co_return;
        }
        else if(method == "GET" || method == "HEAD"){
            co_await handle_get_request(path, headers, is_head);
            co_return;
        }
    } 
    catch(const exception& e){
        cerr << "[HTTP] Session error: " << e.what() << endl;
    }

    stop();
}

// ============================================================
//  HTTP request parsing
// ============================================================
awaitable<void> HttpSession::read_request(string& method, u8string& path, string& version,
                                           vector<pair<string, string>>& headers,
                                           string& extra_body){
    string request_string;

    auto [ec, bytes_transferred] = co_await asio::async_read_until(
        socket_, asio::dynamic_buffer(request_string), "\r\n\r\n", as_tuple);
    if(ec)
        throw runtime_error("Failed to read request: " + ec.message());

    // Parse only up to the delimiter
    string request = request_string.substr(0, bytes_transferred);

    // Save any extra data read beyond the headers (beginning of body)
    if(request_string.size() > bytes_transferred)
        extra_body = request_string.substr(bytes_transferred);
    else
        extra_body.clear();

    istringstream stream(request);

    string path_raw;
    stream >> method >> path_raw >> version;

    // Strip query string (?...) if present
    auto query_pos = path_raw.find('?');
    if(query_pos != string::npos)
        path_raw = path_raw.substr(0, query_pos);

    // URL decode the path
    vector<uint8_t> path_bytes(path_raw.begin(), path_raw.end());
    path = url_decode(path_bytes);

    // Parse headers
    string line;
    getline(stream, line); // consume rest of request line
    while(getline(stream, line)){
        if(line.empty() || line == "\r")
            break;
        if(!line.empty() && line.back() == '\r')
            line.pop_back();

        auto colon_pos = line.find(':');
        if(colon_pos != string::npos){
            string key = line.substr(0, colon_pos);
            string value = line.substr(colon_pos + 1);
            auto start = value.find_first_not_of(' ');
            if(start != string::npos)
                value = value.substr(start);
            else
                value.clear();
            headers.emplace_back(std::move(key), std::move(value));
        }
    }
}

// ============================================================
//  Response helpers
// ============================================================
awaitable<void> HttpSession::send_response(int status_code, const string& status_text,
                                            const vector<pair<string, string>>& headers,
                                            const string& body){
    ostringstream response;
    response << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";

    bool has_content_length = false;
    bool has_content_type = false;
    for(const auto& [key, value] : headers){
        response << key << ": " << value << "\r\n";
        if(key == "Content-Length") has_content_length = true;
        if(key == "Content-Type") has_content_type = true;
    }

    if(!has_content_length)
        response << "Content-Length: " << body.size() << "\r\n";
    if(!has_content_type && !body.empty())
        response << "Content-Type: text/html; charset=utf-8\r\n";

    response << "\r\n";
    if(!body.empty())
        response << body;

    string response_str = response.str();
    auto [wec, wn] = co_await asio::async_write(socket_, asio::buffer(response_str), as_tuple);
    if(wec)
        throw runtime_error("Failed to send response: " + wec.message());
}

awaitable<void> HttpSession::handle_get_request(const u8string& path, const vector<pair<string, string>>& request_headers, bool is_head){
    fs::path abs_path = get_absolute_path(fs::path(path));
    if(!is_valid_path(abs_path)){
        vector<pair<string, string>> h;
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "<html><body><h1>403 Forbidden</h1></body></html>");
        co_return;
    }

    if(!fs::exists(abs_path)){
        vector<pair<string, string>> h;
        h.emplace_back("Connection", "close");
        co_await send_response(404, "Not Found", h,
            "<html><body><h1>404 Not Found</h1></body></html>");
        co_return;
    }

    if(fs::is_directory(abs_path)){
        // Redirect if trailing slash is missing (so relative links work)
        if(path.empty() || path.back() != u8'/'){
            auto encoded = url_encode(path + u8"/");
            string location(encoded.begin(), encoded.end());
            vector<pair<string, string>> h;
            h.emplace_back("Location", location);
            h.emplace_back("Connection", "close");
            co_await send_response(301, "Moved Permanently", h);
            co_return;
        }
        if(is_head){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "text/html; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(200, "OK", h);
        } 
        else {
            co_await send_directory_listing(abs_path, path);
        }
    }
    else if(fs::is_regular_file(abs_path)){
        co_await send_file_response(abs_path, request_headers, is_head);
    }
    else {
        vector<pair<string, string>> h;
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "<html><body><h1>403 Forbidden</h1></body></html>");
    }
}

// ============================================================
//  File serving (GET / Range)
// ============================================================
awaitable<void> HttpSession::send_file_response(const fs::path& file_path,
                                                 const vector<pair<string, string>>& request_headers,
                                                 bool head_only){
    auto file_size = fs::file_size(file_path);
    string mime_type = get_mime_type(file_path);

    // Look for Range header
    string range_header;
    for(const auto& [key, value] : request_headers){
        if(key == "Range"){
            range_header = value;
            break;
        }
    }

    uintmax_t range_start = 0;
    uintmax_t range_end   = file_size > 0 ? file_size - 1 : 0;
    bool is_range_request  = false;

    if(!range_header.empty() && file_size > 0){
        if(parse_range(range_header, file_size, range_start, range_end)){
            is_range_request = true;
        } else {
            // 416 Range Not Satisfiable
            vector<pair<string, string>> h;
            h.emplace_back("Content-Range", "bytes */" + to_string(file_size));
            h.emplace_back("Connection", "close");
            co_await send_response(416, "Range Not Satisfiable", h);
            co_return;
        }
    }

    uintmax_t content_length = file_size > 0 ? (range_end - range_start + 1) : 0;

    // Build response header
    ostringstream resp;
    if(is_range_request){
        resp << "HTTP/1.1 206 Partial Content\r\n";
        resp << "Content-Range: bytes " << range_start << "-" << range_end << "/" << file_size << "\r\n";
    } else {
        resp << "HTTP/1.1 200 OK\r\n";
    }

    resp << "Content-Type: "   << mime_type      << "\r\n";
    resp << "Content-Length: "  << content_length << "\r\n";
    resp << "Accept-Ranges: bytes\r\n";
    resp << "Connection: close\r\n";
    resp << "\r\n";

    // Send header
    string header_str = resp.str();
    auto [hec, hn] = co_await asio::async_write(socket_, asio::buffer(header_str), as_tuple);
    if(hec)
        throw runtime_error("Failed to send response header: " + hec.message());

    if(head_only || content_length == 0)
        co_return;

    // Stream file body in 64 KB chunks
    ifstream file(file_path, ios::binary);
    if(!file)
        throw runtime_error("Failed to open file: " + file_path.string());

    file.seekg(static_cast<streamoff>(range_start));

    constexpr size_t CHUNK_SIZE = 65536;
    vector<char> buffer(CHUNK_SIZE);
    uintmax_t remaining = content_length;

    while(remaining > 0){
        size_t to_read = static_cast<size_t>(min(static_cast<uintmax_t>(CHUNK_SIZE), remaining));
        file.read(buffer.data(), static_cast<streamsize>(to_read));
        auto bytes_read = file.gcount();
        if(bytes_read <= 0)
            break;

        auto [wec, wn] = co_await asio::async_write(socket_,
            asio::buffer(buffer.data(), static_cast<size_t>(bytes_read)), as_tuple);
        if(wec)
            throw runtime_error("Failed to send file data: " + wec.message());

        remaining -= static_cast<uintmax_t>(bytes_read);
    }
}

// ============================================================
//  Directory listing (JSON)
// ============================================================
awaitable<void> HttpSession::send_directory_listing(const fs::path& dir_path,
                                                     const u8string& request_path){
    string req_path_str(reinterpret_cast<const char*>(request_path.data()), request_path.size());

    // Collect & sort entries (directories first, then alphabetically)
    vector<fs::directory_entry> entries;
    error_code ec;
    for(const auto& entry : fs::directory_iterator(dir_path, ec))
        entries.push_back(entry);

    sort(entries.begin(), entries.end(),
        [](const fs::directory_entry& a, const fs::directory_entry& b){
            if(a.is_directory() != b.is_directory())
                return a.is_directory() > b.is_directory();
            return a.path().filename() < b.path().filename();
        });

    ostringstream json;
    json << "[\n";

    bool first = true;
    for(const auto& entry : entries){
        string name = entry.path().filename().string();
        bool is_dir = entry.is_directory();

        // URL path for GET request
        u8string u8name(reinterpret_cast<const char8_t*>(name.data()), name.size());
        auto encoded = url_encode(u8name);
        string href(encoded.begin(), encoded.end());
        string url_path = req_path_str + href;
        if(is_dir)
            url_path += "/";

        // File size (0 for directories)
        uintmax_t fsize = 0;
        if(!is_dir){
            error_code sec;
            fsize = entry.file_size(sec);
            if(sec) fsize = 0;
        }

        // Extension (empty for directories)
        string extension;
        if(!is_dir){
            extension = entry.path().extension().string();
        }

        // Last modified time
        string mtime_str;
        {
            error_code tec;
            auto ftime = entry.last_write_time(tec);
            if(!tec){
                auto sctp = chrono::time_point_cast<chrono::system_clock::duration>(
                    ftime - fs::file_time_type::clock::now() + chrono::system_clock::now());
                auto tt = chrono::system_clock::to_time_t(sctp);
                struct tm tm_buf;
                localtime_r(&tt, &tm_buf);
                char time_buf[20];
                strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
                mtime_str = time_buf;
            }
        }

        if(!first) json << ",\n";
        first = false;

        json << "  {"
             << "\"name\":\"" << json_escape(name) << "\","
             << "\"path\":\"" << json_escape(url_path) << "\","
             << "\"is_directory\":" << (is_dir ? "true" : "false") << ","
             << "\"size\":" << fsize << ","
             << "\"extension\":\"" << json_escape(extension) << "\","
             << "\"last_modified\":\"" << mtime_str << "\""
             << "}";
    }

    json << "\n]";

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h, json.str());
}

// ============================================================
//  URL encode / decode
// ============================================================
u8string HttpSession::url_decode(const vector<uint8_t>& encoded){
    vector<uint8_t> decoded;
    decoded.reserve(encoded.size());

    for(size_t i = 0; i < encoded.size(); ++i){
        if(encoded[i] == '%' && i + 2 < encoded.size()){
            auto hex_val = [](uint8_t c) -> int {
                if(c >= '0' && c <= '9') return c - '0';
                if(c >= 'a' && c <= 'f') return c - 'a' + 10;
                if(c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int high = hex_val(encoded[i + 1]);
            int low  = hex_val(encoded[i + 2]);
            if(high >= 0 && low >= 0){
                decoded.push_back(static_cast<uint8_t>(high * 16 + low));
                i += 2;
                continue;
            }
        }
        if(encoded[i] == '+')
            decoded.push_back(' ');
        else
            decoded.push_back(encoded[i]);
    }

    return u8string(reinterpret_cast<const char8_t*>(decoded.data()), decoded.size());
}

vector<uint8_t> HttpSession::url_encode(const u8string& decoded){
    vector<uint8_t> encoded;
    encoded.reserve(decoded.size());

    for(char8_t c : decoded){
        if((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~' || c == '/'){
            encoded.push_back(static_cast<uint8_t>(c));
        } else {
            static const char hex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(static_cast<uint8_t>(hex[(static_cast<uint8_t>(c) >> 4) & 0xF]));
            encoded.push_back(static_cast<uint8_t>(hex[static_cast<uint8_t>(c) & 0xF]));
        }
    }

    return encoded;
}

// ============================================================
//  Path helpers
// ============================================================
bool HttpSession::is_valid_path(const fs::path& path){
    // Prevent directory traversal – resolved path must start with root_path_
    error_code ec;
    auto canonical      = fs::weakly_canonical(path, ec);
    if(ec) return false;
    auto root_canonical = fs::weakly_canonical(root_path_, ec);
    if(ec) return false;

    auto it_root = root_canonical.begin();
    auto it_path = canonical.begin();
    for(; it_root != root_canonical.end(); ++it_root, ++it_path){
        if(it_path == canonical.end() || *it_path != *it_root)
            return false;
    }
    return true;
}

fs::path HttpSession::get_absolute_path(const fs::path& path){
    // HTTP path like /foo/bar → root_path_/foo/bar
    return root_path_ / path.relative_path();
}

// ============================================================
//  MIME type mapping
// ============================================================
string HttpSession::get_mime_type(const fs::path& path){
    static const unordered_map<string, string> mime_types = {
        {".html", "text/html; charset=utf-8"},
        {".htm",  "text/html; charset=utf-8"},
        {".css",  "text/css"},
        {".js",   "application/javascript"},
        {".json", "application/json"},
        {".xml",  "application/xml"},
        {".txt",  "text/plain; charset=utf-8"},
        {".csv",  "text/csv"},
        {".png",  "image/png"},
        {".jpg",  "image/jpeg"},
        {".jpeg", "image/jpeg"},
        {".gif",  "image/gif"},
        {".svg",  "image/svg+xml"},
        {".ico",  "image/x-icon"},
        {".webp", "image/webp"},
        {".mp3",  "audio/mpeg"},
        {".mp4",  "video/mp4"},
        {".webm", "video/webm"},
        {".ogg",  "audio/ogg"},
        {".wav",  "audio/wav"},
        {".pdf",  "application/pdf"},
        {".zip",  "application/zip"},
        {".gz",   "application/gzip"},
        {".tar",  "application/x-tar"},
        {".bz2",  "application/x-bzip2"},
        {".7z",   "application/x-7z-compressed"},
    };

    string ext = path.extension().string();
    transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    auto it = mime_types.find(ext);
    if(it != mime_types.end())
        return it->second;

    return "application/octet-stream";
}

// ============================================================
//  Range header parsing
// ============================================================
bool HttpSession::parse_range(const string& range_header, uintmax_t file_size,
                              uintmax_t& start, uintmax_t& end){
    // Supports: bytes=start-end, bytes=start-, bytes=-suffix
    if(range_header.size() < 7 || range_header.substr(0, 6) != "bytes=")
        return false;

    string range = range_header.substr(6);

    // Only single range supported (no multipart ranges)
    if(range.find(',') != string::npos)
        return false;

    auto dash = range.find('-');
    if(dash == string::npos)
        return false;

    string start_str = range.substr(0, dash);
    string end_str   = range.substr(dash + 1);

    try {
        if(start_str.empty()){
            // bytes=-suffix  →  last N bytes
            if(end_str.empty()) return false;
            uintmax_t suffix = stoull(end_str);
            if(suffix == 0) return false;
            if(suffix > file_size) suffix = file_size;
            start = file_size - suffix;
            end   = file_size - 1;
        } else {
            start = stoull(start_str);
            if(end_str.empty())
                end = file_size - 1;   // bytes=start-  →  to EOF
            else
                end = stoull(end_str);  // bytes=start-end
        }
    } catch(...){
        return false;
    }

    if(start > end || start >= file_size)
        return false;
    if(end >= file_size)
        end = file_size - 1;

    return true;
}

// ============================================================
//  HTML escaping (XSS prevention)
// ============================================================
string HttpSession::html_escape(const string& text){
    string result;
    result.reserve(text.size());
    for(char c : text){
        switch(c){
            case '&':  result += "&amp;";  break;
            case '<':  result += "&lt;";   break;
            case '>':  result += "&gt;";   break;
            case '"':  result += "&quot;"; break;
            case '\'': result += "&#39;";  break;
            default:   result += c;        break;
        }
    }
    return result;
}

// ============================================================
//  JSON string escaping
// ============================================================
string HttpSession::json_escape(const string& text){
    string result;
    result.reserve(text.size());
    for(char c : text){
        switch(c){
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n";  break;
            case '\r': result += "\\r";  break;
            case '\t': result += "\\t";  break;
            default:   result += c;      break;
        }
    }
    return result;
}

bool HttpSession::parse_path_from_json(const string& json_str, const string& key, fs::path& path_out){
    // Simple JSON parsing to extract a string value for the given key
    // This is a naive implementation and assumes well-formed JSON
    size_t key_pos = json_str.find('"' + key + '"');
    if(key_pos == string::npos)
        return false;

    size_t colon_pos = json_str.find(':', key_pos);
    if(colon_pos == string::npos)
        return false;

    size_t quote_start = json_str.find('"', colon_pos);
    if(quote_start == string::npos)
        return false;

    size_t quote_end = json_str.find('"', quote_start + 1);
    if(quote_end == string::npos)
        return false;

    string value = json_str.substr(quote_start + 1, quote_end - quote_start - 1);
    u8string u8value(reinterpret_cast<const char8_t*>(value.data()), value.size());
    path_out = fs::path(u8value);
    return true;
}

awaitable<void> HttpSession::handle_post_request(const u8string& path, const vector<pair<string, string>>& request_headers,
                                                  string& extra_body){
    size_t content_length = 0;
    bool has_content_length = false;
    for(const auto& [key, value] : request_headers){
        string lower_key = key;
        transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if(lower_key == "content-length"){
            try {
                content_length = stoull(value);
                has_content_length = true;
            } catch(...){
                vector<pair<string, string>> h;
                h.emplace_back("Content-Type", "application/json; charset=utf-8");
                h.emplace_back("Connection", "close");
                co_await send_response(400, "Bad Request", h,
                    "{\"error\": \"Invalid Content-Length header\"}");
                co_return;
            }
            break;
        }
    }

    if(!has_content_length){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(400, "Bad Request", h,
            "{\"error\": \"Missing Content-Length header\"}");
        co_return;
    }

    // Start with any body bytes already read during header parsing
    string body = std::move(extra_body);
    size_t read_bytes = body.size();

    while(read_bytes < content_length){
        size_t to_read = min(static_cast<size_t>(8192), content_length - read_bytes);
        string chunk(to_read, '\0');
        auto [ec, bytes_read] = co_await asio::async_read(socket_, asio::buffer(chunk), as_tuple);
        if(ec){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(500, "Internal Server Error", h,
                "{\"error\": \"Failed to read request body: " + json_escape(ec.message()) + "\"}");
            co_return;
        }

        body.append(chunk.data(), bytes_read);
        read_bytes += bytes_read;
    }

    // Trim body to exact content_length in case extra_body had more
    if(body.size() > content_length)
        body.resize(content_length);

    if(path == u8"/file/move"){
        fs::path src, dst;
        if(!parse_path_from_json(body, "src", src) || !parse_path_from_json(body, "dst", dst)){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(400, "Bad Request", h, "{\"error\": \"Missing or invalid 'source' or 'destination' in JSON body\"}");
            co_return;
        }
        co_await move_file(src, dst);
        co_return;
    }
    else if(path == u8"/file/delete"){
        fs::path target;
        if(!parse_path_from_json(body, "file_path", target)){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(400, "Bad Request", h, "{\"error\": \"Missing or invalid 'file_path' in JSON body\"}");
            co_return;
        }
        co_await delete_file(target);
        co_return;
    }
    else if(path == u8"/directory/create"){
        fs::path dir_path;
        if(!parse_path_from_json(body, "path", dir_path)){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(400, "Bad Request", h, "{\"error\": \"Missing or invalid 'path' in JSON body\"}");
            co_return;
        }
        co_await create_directory(dir_path);
        co_return;
    }
    else if(path == u8"/file/copy"){
        fs::path src, dst;
        if(!parse_path_from_json(body, "src", src) || !parse_path_from_json(body, "dst", dst)){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(400, "Bad Request", h, "{\"error\": \"Missing or invalid 'source' or 'destination' in JSON body\"}");
            co_return;
        }
        co_await copy_file(src, dst);
        co_return;
    }

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(400, "Bad Request", h, "{\"error\": \"Unknown POST request path\"}");
}
awaitable<void> HttpSession::move_file(const fs::path& source_path, const fs::path& dest_path){
    fs::path abs_source = get_absolute_path(source_path);
    fs::path abs_dest   = get_absolute_path(dest_path);

    if(!is_valid_path(abs_source) || !is_valid_path(abs_dest)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "{\"error\": \"Invalid source or destination path\"}");
        co_return;
    }

    if(!fs::exists(abs_source)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(404, "Not Found", h,
            "{\"error\": \"Source file does not exist\"}");
        co_return;
    }

    error_code ec;
    fs::rename(abs_source, abs_dest, ec);
    if(ec){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(500, "Internal Server Error", h,
            "{\"error\": \"Failed to move file: " + json_escape(ec.message()) + "\"}");
        co_return;
    }

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h,
        "{\"message\": \"File moved successfully\"}");
}
awaitable<void> HttpSession::delete_file(const fs::path& file_path){
    fs::path abs_path = get_absolute_path(file_path);
    if(!is_valid_path(abs_path)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "{\"error\": \"Invalid file path\"}");
        co_return;
    }

    if(!fs::exists(abs_path)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(404, "Not Found", h,
            "{\"error\": \"File does not exist\"}");
        co_return;
    }

    error_code ec;
    fs::remove_all(abs_path, ec);
    if(ec){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(500, "Internal Server Error", h,
            "{\"error\": \"Failed to delete file: " + json_escape(ec.message()) + "\"}");
        co_return;
    }

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h,
        "{\"message\": \"File deleted successfully\"}");
}
awaitable<void> HttpSession::create_directory(const fs::path& dir_path){
    fs::path abs_path = get_absolute_path(dir_path);
    if(!is_valid_path(abs_path)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "{\"error\": \"Invalid directory path\"}");
        co_return;
    }

    error_code ec;
    fs::create_directory(abs_path, ec);
    if(ec){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(500, "Internal Server Error", h,
            "{\"error\": \"Failed to create directory: " + json_escape(ec.message()) + "\"}");
        co_return;
    }

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h,
        "{\"message\": \"Directory created successfully\"}");
}
awaitable<void> HttpSession::copy_file(const fs::path& source_path, const fs::path& dest_path){
    fs::path abs_source = get_absolute_path(source_path);
    fs::path abs_dest   = get_absolute_path(dest_path);
    if(!is_valid_path(abs_source) || !is_valid_path(abs_dest)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "{\"error\": \"Invalid source or destination path\"}");
        co_return;
    }

    if(!fs::exists(abs_source)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(404, "Not Found", h,
            "{\"error\": \"Source file does not exist\"}");
        co_return;
    }
    if(fs::exists(abs_dest)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(409, "Conflict", h,
            "{\"error\": \"Destination file already exists\"}");
        co_return;
    }

    error_code ec;
    fs::copy(abs_source, abs_dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if(ec){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(500, "Internal Server Error", h,
            "{\"error\": \"Failed to copy file: " + json_escape(ec.message()) + "\"}");
        co_return;
    }

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h,
        "{\"message\": \"File copied successfully\"}");
}

awaitable<void> HttpSession::handle_put_request(const u8string& path, const vector<pair<string, string>>& request_headers,
                                                 string& extra_body){
    fs::path abs_path = get_absolute_path(fs::path(path));
    if(!is_valid_path(abs_path)){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(403, "Forbidden", h,
            "{\"error\": \"Invalid file path\"}");
        co_return;
    }

    size_t content_length = 0;
    bool has_content_length = false;
    for(const auto& [key, value] : request_headers){
        string lower_key = key;
        transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        if(lower_key == "content-length"){
            try {
                content_length = stoull(value);
                has_content_length = true;
            } catch(...){
                vector<pair<string, string>> h;
                h.emplace_back("Content-Type", "application/json; charset=utf-8");
                h.emplace_back("Connection", "close");
                co_await send_response(400, "Bad Request", h,
                    "{\"error\": \"Invalid Content-Length header\"}");
                co_return;
            }
            break;
        }
    }

    if(!has_content_length){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(400, "Bad Request", h,
            "{\"error\": \"Missing Content-Length header\"}");
        co_return;
    }

    // Create parent directories if needed
    error_code dir_ec;
    fs::create_directories(abs_path.parent_path(), dir_ec);

    fstream file(abs_path, ios::binary | ios::out | ios::trunc);
    if(!file){
        vector<pair<string, string>> h;
        h.emplace_back("Content-Type", "application/json; charset=utf-8");
        h.emplace_back("Connection", "close");
        co_await send_response(500, "Internal Server Error", h,
            "{\"error\": \"Failed to open file for writing\"}");
        co_return;
    }

    // Write any body bytes already read during header parsing
    size_t bytes_received = 0;
    if(!extra_body.empty()){
        size_t to_write = min(extra_body.size(), content_length);
        file.write(extra_body.data(), static_cast<streamsize>(to_write));
        if(!file){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(500, "Internal Server Error", h,
                "{\"error\": \"Failed to write to file\"}");
            co_return;
        }
        bytes_received = to_write;
    }

    while(bytes_received < content_length){
        size_t to_read = min(static_cast<size_t>(65536), content_length - bytes_received);
        vector<char> buffer(to_read);

        auto [ec, bytes_read] = co_await asio::async_read(socket_, asio::buffer(buffer), as_tuple);
        if(ec){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(500, "Internal Server Error", h,
                "{\"error\": \"Failed to read request body: " + json_escape(ec.message()) + "\"}");
            co_return;
        }

        file.write(buffer.data(), bytes_read);
        if(!file){
            vector<pair<string, string>> h;
            h.emplace_back("Content-Type", "application/json; charset=utf-8");
            h.emplace_back("Connection", "close");
            co_await send_response(500, "Internal Server Error", h,
                "{\"error\": \"Failed to write to file\"}");
            co_return;
        }

        bytes_received += static_cast<size_t>(bytes_read);
    }

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "application/json; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h,
        "{\"message\": \"File uploaded successfully\"}");
}
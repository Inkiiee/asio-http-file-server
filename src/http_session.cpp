#include "http_session.h"

#include <asio.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
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

HttpSession::HttpSession(tcp::socket socket)
    : socket_(std::move(socket)) {}

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

        co_await read_request(method, path, version, headers);

        cout << "[HTTP] " << method << " "
             << string(reinterpret_cast<const char*>(path.data()), path.size())
             << " " << version << endl;

        bool is_head = (method == "HEAD");
        if(method != "GET" && method != "HEAD"){
            vector<pair<string, string>> h;
            h.emplace_back("Allow", "GET, HEAD");
            h.emplace_back("Connection", "close");
            co_await send_response(405, "Method Not Allowed", h,
                "<html><body><h1>405 Method Not Allowed</h1></body></html>");
            co_return;
        }

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
            } else {
                co_await send_directory_listing(abs_path, path);
            }
        } else if(fs::is_regular_file(abs_path)){
            co_await send_file_response(abs_path, headers, is_head);
        } else {
            vector<pair<string, string>> h;
            h.emplace_back("Connection", "close");
            co_await send_response(403, "Forbidden", h,
                "<html><body><h1>403 Forbidden</h1></body></html>");
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
                                           vector<pair<string, string>>& headers){
    string request_string;

    auto [ec, bytes_transferred] = co_await asio::async_read_until(
        socket_, asio::dynamic_buffer(request_string), "\r\n\r\n", as_tuple);
    if(ec)
        throw runtime_error("Failed to read request: " + ec.message());

    // Parse only up to the delimiter
    string request = request_string.substr(0, bytes_transferred);
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
//  Directory listing
// ============================================================
awaitable<void> HttpSession::send_directory_listing(const fs::path& dir_path,
                                                     const u8string& request_path){
    string display_path(reinterpret_cast<const char*>(request_path.data()), request_path.size());

    ostringstream html;
    html << "<!DOCTYPE html>\n<html>\n<head>\n<meta charset=\"utf-8\">\n"
         << "<title>Index of " << html_escape(display_path) << "</title>\n"
         << "<style>\n"
         << "body { font-family: monospace; margin: 20px; }\n"
         << "a { text-decoration: none; color: #0066cc; }\n"
         << "a:hover { text-decoration: underline; }\n"
         << "table { border-collapse: collapse; }\n"
         << "td { padding: 4px 16px; }\n"
         << ".size { text-align: right; color: #666; }\n"
         << "</style>\n</head>\n<body>\n"
         << "<h1>Index of " << html_escape(display_path) << "</h1>\n"
         << "<hr>\n<table>\n";

    // Parent directory link
    if(request_path != u8"/"){
        html << "<tr><td><a href=\"../\">../</a></td><td class=\"size\">-</td></tr>\n";
    }

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

    for(const auto& entry : entries){
        string name = entry.path().filename().string();
        bool is_dir = entry.is_directory();

        // URL-encode the name for the href
        u8string u8name(reinterpret_cast<const char8_t*>(name.data()), name.size());
        auto encoded = url_encode(u8name);
        string href(encoded.begin(), encoded.end());

        string display_name = html_escape(name);
        if(is_dir){
            display_name += "/";
            href += "/";
        }

        // Human-readable file size
        string size_str = "-";
        if(!is_dir){
            error_code sec;
            auto fsize = entry.file_size(sec);
            if(!sec){
                if(fsize < 1024)
                    size_str = to_string(fsize) + " B";
                else if(fsize < 1024 * 1024)
                    size_str = to_string(fsize / 1024) + " KB";
                else if(fsize < 1024ULL * 1024 * 1024)
                    size_str = to_string(fsize / (1024 * 1024)) + " MB";
                else
                    size_str = to_string(fsize / (1024ULL * 1024 * 1024)) + " GB";
            }
        }

        html << "<tr><td><a href=\"" << href << "\">"
             << display_name << "</a></td>"
             << "<td class=\"size\">" << size_str << "</td></tr>\n";
    }

    html << "</table>\n<hr>\n</body>\n</html>\n";

    vector<pair<string, string>> h;
    h.emplace_back("Content-Type", "text/html; charset=utf-8");
    h.emplace_back("Connection", "close");
    co_await send_response(200, "OK", h, html.str());
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
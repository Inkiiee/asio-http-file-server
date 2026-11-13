#include "http_base.h"

using namespace http_base;

//HTTP Request implementation
HttpRequest::HttpRequest(const std::string& method, const std::u8string& path, const std::string& version)
    : method_(method), path_(path), version_(version) {}

const std::string& HttpRequest::method() const { return method_; }
const std::u8string& HttpRequest::path() const { return path_; }
const std::string& HttpRequest::version() const { return version_; }
void HttpRequest::set_method(const std::string& method) { method_ = method; }
void HttpRequest::set_path(const std::u8string& path) { path_ = path; }
void HttpRequest::set_version(const std::string& version) { version_ = version; }
const header_map& HttpRequest::headers() const { return headers_; }
bool HttpRequest::has_header(const std::string& key) const { return headers_.find(key) != headers_.end(); }
std::string HttpRequest::get_header(const std::string& key) const {
    auto it = headers_.find(key);
    return it != headers_.end() ? it->second : "";
}
void HttpRequest::remove_header(const std::string& key) { headers_.erase(key); }
void HttpRequest::add_header(const std::string& key, const std::string& value) { headers_[key] = value; }
void HttpRequest::clear_headers() { headers_.clear(); }

// HTTP Response implementation
HttpResponse::HttpResponse(const std::string& version, int status_code): version_(version), status_code_(status_code) {
    status_text_ = get_status_text_(status_code);
}

const std::string& HttpResponse::version() const { return version_; }
int HttpResponse::status_code() const { return status_code_; }
const std::string& HttpResponse::status_text() const { return status_text_; }
const header_map& HttpResponse::headers() const { return headers_; }
std::string HttpResponse::get_header(const std::string& key) const {
    auto it = headers_.find(key);
    return it != headers_.end() ? it->second : "";
}
void HttpResponse::set_version(const std::string& version){ version_ = version; }
void HttpResponse::set_status_code(int status_code){ status_code_ = status_code; status_text_ = get_status_text_(status_code); }
void HttpResponse::set_status_text(const std::string& status_text){ status_text_ = status_text; }
void HttpResponse::add_header(const std::string& key, const std::string& value){ headers_[key] = value; }
bool HttpResponse::has_header(const std::string& key) const{ return headers_.find(key) != headers_.end(); }
void HttpResponse::remove_header(const std::string& key) { headers_.erase(key); }
void HttpResponse::clear_headers(){ headers_.clear(); }
std::string HttpResponse::get_status_text_(int status_code) const{
    switch (status_code) {
    // 1xx Informational
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 103: return "Early Hints";
    case 104: return "Upload Resumption Supported";

    // 2xx Success
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-Authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";

    // 3xx Redirection
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 306: return "Unused";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";

    // 4xx Client Error
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Content Too Large";
    case 414: return "URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "Unused";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Content";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 425: return "Too Early";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 451: return "Unavailable For Legal Reasons";

    // 5xx Server Error
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";

    default:
        return "Unknown Status Code";
    }
}

namespace http_base{
    std::vector<uint8_t> url_encode(const std::u8string& decoded){
        std::vector<uint8_t> encoded;
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

    std::u8string url_decode(const std::vector<uint8_t>& encoded){
        std::vector<uint8_t> decoded;
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

        return std::u8string(reinterpret_cast<const char8_t*>(decoded.data()), decoded.size());
    }

    std::string html_escape(const std::string& text){
        std::string result;
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

    std::string json_escape(const std::string& text){
        std::string result;
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

    std::string get_mime_type(const std::string& extension){
        static const std::unordered_map<std::string, std::string> mime_types = {
            {".html", "text/html"},
            {".htm", "text/html"},
            {".css", "text/css"},
            {".js", "application/javascript"},
            {".json", "application/json"},
            {".xml", "application/xml"},
            {".jpg", "image/jpeg"},
            {".jpeg", "image/jpeg"},
            {".png", "image/png"},
            {".gif", "image/gif"},
            {".bmp", "image/bmp"},
            {".webp", "image/webp"},
            {".svg", "image/svg+xml"},
            {".ico", "image/x-icon"},
            {".txt", "text/plain"},
            {".csv", "text/csv"},
            {".pdf", "application/pdf"},
            {".zip", "application/zip"},
            {".tar", "application/x-tar"},
            {".gz", "application/gzip"},
            {".mp3", "audio/mpeg"},
            {".wav", "audio/wav"},
            {".mp4", "video/mp4"},
            {".avi", "video/x-msvideo"},
            {".mov", "video/quicktime"},
            {".wmv", "video/x-ms-wmv"},
            {".flv", "video/x-flv"},
            {".mkv", "video/x-matroska"},
            {".webm", "video/webm"},
            {".ogg", "application/ogg"},
            {".rtf", "application/rtf"},
            {".7z", "application/x-7z-compressed"},
            {".rar", "application/vnd.rar"},
            {".exe", "application/vnd.microsoft.portable-executable"},
            {".mpg", "video/mpeg"},
            {".mpeg", "video/mpeg"},
            {".m4v", "video/x-m4v"},
            {".flac", "audio/flac"},
            {".aac", "audio/aac"},
            {".m3u8", "application/vnd.apple.mpegurl"},
            {".ts", "video/mp2t"},
            {".woff", "font/woff"},
            {".woff2", "font/woff2"},
            {".ttf", "font/ttf"},
            {".eot", "application/vnd.ms-fontobject"},
            {".otf", "font/otf"},
            {".doc", "application/msword"},
            {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
            {".xls", "application/vnd.ms-excel"},
            {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
            {".ppt", "application/vnd.ms-powerpoint"},
            {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
            {".odt", "application/vnd.oasis.opendocument.text"},
            {".ods", "application/vnd.oasis.opendocument.spreadsheet"},
            {".odp", "application/vnd.oasis.opendocument.presentation"},
            {".m4a", "audio/mp4"},
            {".opus", "audio/opus"},
            {".wma", "audio/x-ms-wma"},
            {".mid", "audio/midi"},
            {".midi", "audio/midi"},
            {".3gp", "video/3gpp"},
            {".3g2", "video/3gpp2"},
            {".f4v", "video/x-f4v"},
            {".tif", "image/tiff"},
            {".tiff", "image/tiff"},
            {".avif", "image/avif"},
            {".heic", "image/heic"},
            {".heif", "image/heif"},
            {".raw", "image/x-raw"},
            {".psd", "image/vnd.adobe.photoshop"},
            {".ai", "application/postscript"},
            {".eps", "application/postscript"},
            {".ps", "application/postscript"},
            {".bz2", "application/x-bzip2"},
            {".xz", "application/x-xz"},
            {".zst", "application/zstd"},
            {".iso", "application/x-iso9660-image"},
            {".dmg", "application/x-apple-diskimage"},
            {".deb", "application/vnd.debian.binary-package"},
            {".rpm", "application/x-rpm"},
            {".apk", "application/vnd.android.package-archive"},
            {".msi", "application/x-msdownload"},
            {".bat", "application/x-msdos-program"},
            {".sh", "application/x-sh"},
            {".py", "text/x-python"},
            {".c", "text/x-c"},
            {".cpp", "text/x-c++src"},
            {".h", "text/x-c"},
            {".hpp", "text/x-c++hdr"},
            {".java", "text/x-java-source"},
            {".rs", "text/x-rust"},
            {".go", "text/x-go"},
            {".rb", "text/x-ruby"},
            {".php", "application/x-httpd-php"},
            {".sql", "application/sql"},
            {".md", "text/markdown"},
            {".yaml", "application/x-yaml"},
            {".yml", "application/x-yaml"},
            {".toml", "application/toml"},
            {".ini", "text/plain"},
            {".cfg", "text/plain"},
            {".conf", "text/plain"},
            {".log", "text/plain"},
            {".srt", "application/x-subrip"},
            {".vtt", "text/vtt"},
            {".ass", "text/x-ssa"},
            {".ssa", "text/x-ssa"},
            {".wasm", "application/wasm"},
            {".map", "application/json"},
            {".webmanifest", "application/manifest+json"},
            {".swf", "application/x-shockwave-flash"}
        };

        auto it = mime_types.find(extension);
        if(it != mime_types.end())
            return it->second;
        return "application/octet-stream"; // Default MIME type
    }

    bool parse_range(const std::string& range_header, std::size_t file_size, std::size_t& start, std::size_t& end){
        // Supports: bytes=start-end, bytes=start-, bytes=-suffix
        if(range_header.size() < 7 || range_header.substr(0, 6) != "bytes=")
            return false;

        std::string range_spec = range_header.substr(6);

        // Only single range supported (no multipart ranges)
        if(range_spec.find(',') != std::string::npos)
            return false;

        auto dash = range_spec.find('-');
        if(dash == std::string::npos)
            return false;

        std::string start_str = range_spec.substr(0, dash);
        std::string end_str   = range_spec.substr(dash + 1);

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
}
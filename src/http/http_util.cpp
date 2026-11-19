#include "http_util.h"

#include <sstream>
#include <iostream>
#include <algorithm>
#include <limits>
#include <unordered_set>

using namespace std;
using namespace http_parser;
using namespace http_maker;

namespace http_sub_functions{
    bool iequals(string_view lhs, string_view rhs){
        if(lhs.size() != rhs.size())
            return false;

        return equal(lhs.begin(), lhs.end(), rhs.begin(),
            [](unsigned char a, unsigned char b){
                return tolower(a) == tolower(b);
            });
    }

    string trim_copy(string_view value){
        auto first = value.find_first_not_of(" \t");
        if(first == string_view::npos)
            return "";

        auto last = value.find_last_not_of(" \t");
        return string(value.substr(first, last - first + 1));
    }

    bool header_has_token(const string& value, string_view token){
        size_t start = 0;
        while(start <= value.size()){
            auto comma = value.find(',', start);
            auto end = comma == string::npos ? value.size() : comma;
            auto current = trim_copy(string_view(value).substr(start, end - start));
            if(iequals(current, token))
                return true;
            if(comma == string::npos)
                break;
            start = comma + 1;
        }
        return false;
    }

    bool headers_has_token(const vector<string>& values, string_view token){
        for(const auto& value: values)
            if(header_has_token(value, token)) return true;
        return false;
    }

    bool response_must_not_have_body(int status_code){
        return (status_code >= 100 && status_code < 200) ||
            status_code == 204 ||
            status_code == 304;
    }

    size_t parse_chunk_size_line(const string& line){
        auto extension = line.find(';');
        auto size_part = trim_copy(string_view(line).substr(0, extension));
        if(size_part.empty())
            throw runtime_error("Invalid chunk size");

        auto value = http_base::str_to_num(size_part, 16);
        if(!value.has_value())
            throw runtime_error("Invalid chunk size");

        return *value;
    }
}

namespace {
    using header_rules_type = std::unordered_set<std::string, http_base::CaseInsensitiveHash, http_base::CaseInsensitiveEqual>;
    inline const header_rules_type only_one_headers_name{
        "host", "content-length", "content-type", "location", 
        "authorization", "proxy-authorization", "from", "max-forwards", "if-unmodified-since", "if-range",
        "date", "expires", "last-modified", "age", "retry-after",
        "destination", "overwrite", "depth", "range", "lock-token"
    };
    inline const header_rules_type list_valued_headers_name{
        "accept", "accept-encoding", "accept-language", "accept-charset", "te",
        "cache-control", "vary", "if-match", "if-none-match", "cache-status", "pragma",
        "connection", "via", "forwarded", "upgrade", "trailer",
        "content-encoding", "content-language", "transfer-encoding", "accept-ranges",
        "access-control-allow-methods", "access-control-allow-headers", "access-control-expose-headers", "access-control-request-headers", "sec-ch-ua",
        "www-authenticate", "proxy-authenticate", "link", "expect"
    };
    inline const header_rules_type special_headers_name{
        "user-agent", "server"
    };
    inline const header_rules_type server_cookie_headers_name{
        "set-cookie"
    };
    inline const header_rules_type client_cookie_headers_name{
        "cookie"
    };

    template <typename Container>
    void parse_list_valued_header_and_add_(Container& container, const string& key, const string& value, char delimiter = ',') {
        size_t start = 0;
        const size_t n = value.size();
        const bool is_link = http_sub_functions::iequals(key, "Link");
        const bool is_entity_tag = http_sub_functions::iequals(key, "If-Match") ||
                                   http_sub_functions::iequals(key, "If-None-Match");
        const char targets[] = {delimiter, '"', is_link ? '<' : '\0', '\0'};
        // ETag의 백슬래시는 이스케이프가 아닌 태그의 일부다.
        const char* quote_targets = is_entity_tag ? "\"" : "\\\"";

        while (start < n) {
            // 1. 앞쪽 공백 건너뛰기
            start = value.find_first_not_of(" \t", start);
            if (start == string::npos) break;

            size_t cur = start;
            bool in_quotes = false;

            // 2. 따옴표와 Link의 <...> 밖에서 구분자를 찾는다.
            while (cur < n) {
                if (in_quotes) {
                    cur = value.find_first_of(quote_targets, cur);
                    if (cur == string::npos) {
                        cur = n; // 닫히지 않은 따옴표 처리
                        break;
                    }
                    if (value[cur] == '\\') {
                        cur = min(cur + 2, n); // 이스케이프 문자(\", \\) 건너뛰기
                    } else { // '"'
                        in_quotes = false;
                        ++cur;
                    }
                }
                else {
                    cur = value.find_first_of(targets, cur);
                    if (cur == string::npos) {
                        cur = n;
                        break;
                    }
                    if (value[cur] == '"') {
                        in_quotes = true;
                        ++cur;
                    }
                    else if (value[cur] == '<') {
                        auto link_end = value.find('>', cur + 1);
                        cur = link_end == string::npos ? n : link_end + 1;
                    }
                    else // delimiter 발견
                        break;
                }
            }

            // 3. 뒤쪽 공백 제거 (cur는 delimiter 위치 또는 n)
            size_t end = cur;
            while (end > start && (value[end - 1] == ' ' || value[end - 1] == '\t'))
                --end;

            if (end > start)
                container.add_header(key, value.substr(start, end - start));
            start = (cur < n) ? cur + 1 : n;
        }
    }
    template <typename Container>
    void parse_special_header_and_add(Container& container, const string& key, const string& value) {
        size_t i = 0;
        const size_t n = value.size();
        while (i < n) {
            // 선행 공백 제거
            while (i < n && (value[i] == ' ' || value[i] == '\t')) ++i;
            if (i >= n) break;

            size_t start = i;
            int paren_depth = 0;
            while (i < n) {
                if (value[i] == '\\' && paren_depth > 0 && i + 1 < n) {
                    i += 2; // 괄호 안 이스케이프 문자 건너뛰기
                    continue;
                }
                if (value[i] == '(') {
                    // 토큰 중간에 '('가 공백 없이 바로 붙은 경우 앞 토큰 먼저 분리
                    if (paren_depth == 0 && i > start) {
                        break;
                    }
                    ++paren_depth;
                }
                else if (value[i] == ')' && paren_depth > 0) {
                    --paren_depth;
                    ++i;
                    // 최상위 괄호가 닫히면 주석 토큰 즉시 종료
                    if (paren_depth == 0) {
                        break;
                    }
                    continue;
                } 
                else if (paren_depth == 0 && (value[i] == ' ' || value[i] == '\t'))
                    break; // 괄호 밖의 공백/탭에서 토큰 분리
                ++i;
            }
            container.add_header(key, value.substr(start, i - start));
        }
    }
}

optional<http_base::HttpRequest> Parser::parse_request(){
    http_base::HttpRequest request;

    stringstream stream(buffer_);
    string path_raw, method, version;
    stream >> method >> path_raw >> version;
    if(path_raw.empty() || !path_raw.starts_with("/")) return nullopt;
    if(version != "HTTP/1.1" && version != "HTTP/1.0") return nullopt;
    
    request.set_method(method);
    request.set_version(version);

    auto query_pos = path_raw.find('?');
    if(query_pos != string::npos)
        path_raw = path_raw.substr(0, query_pos);

    //url decode the path
    vector<uint8_t> path_bytes(path_raw.begin(), path_raw.end());
    auto path_decoded = http_base::url_decode(path_bytes, true);
    request.set_path(path_decoded);

    // Parse headers
    string line;
    getline(stream, line); // consume the rest of the request line
    while(getline(stream, line)){
        if(line.empty() || line == "\r")
            break;
        if(!line.empty() && line.back() == '\r')
            line.pop_back();

        auto colon_pos = line.find(':');
        if(colon_pos == string::npos)
            return nullopt;
        string key = line.substr(0, colon_pos);
        if(key.empty() || key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`|~") != string::npos)
            return nullopt;
        string value = line.substr(colon_pos + 1);

        if(server_cookie_headers_name.find(key) != server_cookie_headers_name.end()) return nullopt;

        // 단일 헤더가 중복으로 들어오는 것을 명시적으로 제한
        if((only_one_headers_name.find(key) != only_one_headers_name.end()) && request.has_header(key))
            return nullopt;
        if(list_valued_headers_name.find(key) != list_valued_headers_name.end())
            parse_list_valued_header_and_add_(request, key, value, ',');
        else if(special_headers_name.find(key) != special_headers_name.end())
            parse_special_header_and_add(request, key, value);
        else if(client_cookie_headers_name.find(key) != client_cookie_headers_name.end())
            parse_list_valued_header_and_add_(request, key, value, ';');
        else
            request.add_header(key, value);

        if(http_sub_functions::iequals(key, "Transfer-Encoding") && !request.has_header(key))
            return nullopt;
    }

    // smuggling 방지용
    if(request.has_header("content-length") && request.has_header("transfer-encoding")) 
        return nullopt;
    if(version == "HTTP/1.1" && !request.has_header("host"))
        return nullopt;

    return request;
}

optional<http_base::HttpResponse> Parser::parse_response(){
    http_base::HttpResponse response;

    stringstream stream(buffer_);
    string version;
    int status_code;
    string status_text;
    stream >> version >> status_code;
    if(version != "HTTP/1.1" && version != "HTTP/1.0") return nullopt;

    response.set_version(version);
    response.set_status_code(status_code);

    getline(stream, status_text); // consume the rest of the status line

    string line;
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

            if(client_cookie_headers_name.find(key) != client_cookie_headers_name.end()) return nullopt;

            // 단일 헤더가 중복으로 들어오는 것을 명시적으로 제한
            if((only_one_headers_name.find(key) != only_one_headers_name.end()) && response.has_header(key))
                return nullopt;
            if(list_valued_headers_name.find(key) != list_valued_headers_name.end())
                parse_list_valued_header_and_add_(response, key, value, ',');
            else if(special_headers_name.find(key) != special_headers_name.end())
                parse_special_header_and_add(response, key, value);
            else
                response.add_header(key, value);
        }

        if(response.has_header("content-length") && response.has_header("transfer-encoding")) return nullopt;
    }

    return response;
}

string Maker::make_response(const http_base::HttpResponse& response){
    stringstream stream;
    stream << response.version() << " " << response.status_code() << " " << response.status_text() << "\r\n";
    for(const auto& header : response.headers()){
        if(header.second.empty()) continue;
        stream << header.first << ": ";

        string delimeter = ", ";
        if(special_headers_name.find(header.first) != special_headers_name.end())
            delimeter = " ";
        else if(server_cookie_headers_name.find(header.first) != server_cookie_headers_name.end())
            delimeter = "\r\n" + header.first + ": ";

        for(size_t i=0; i + 1 < header.second.size(); i++)
            stream << header.second[i] << delimeter;
        stream << header.second[header.second.size() - 1] << "\r\n";
    }
    stream << "\r\n";
    return stream.str();
}

string Maker::make_request(const http_base::HttpRequest& request){
    stringstream stream;
    string path = string(request.path().begin(), request.path().end());
    stream << request.method() << " " << path << " " << request.version() << "\r\n";
    for(const auto& header : request.headers()){
        if(header.second.empty()) continue;
        stream << header.first << ": ";

        string delimeter = ", ";
        if(special_headers_name.find(header.first) != special_headers_name.end())
            delimeter = " ";
        else if(client_cookie_headers_name.find(header.first) != client_cookie_headers_name.end())
            delimeter = "; ";

        for(size_t i=0; i + 1 < header.second.size(); i++)
            stream << header.second[i] << delimeter;
        stream << header.second[header.second.size() - 1] << "\r\n";
    }
    stream << "\r\n";
    return stream.str();
}

string Maker::make_encoded_path(const filesystem::path& path, bool is_directory){
    u8string encoded_path;
    bool first = true;
    for(const auto& part : path){
        auto part_str = part.u8string();
        if(first){
            first = false;
            if(part_str == u8"/" || part_str == u8"\\")
                continue; // 루트 구분자 건너뛰기
        }
        encoded_path += u8"/";
        auto encoded_part = http_base::url_encode(part_str);
        encoded_path += u8string(encoded_part.begin(), encoded_part.end());
    }
    if(encoded_path.empty())
        encoded_path = u8"/";

    if(is_directory && encoded_path.back() != u8'/')
        encoded_path += u8'/';
    return string(encoded_path.begin(), encoded_path.end());
}

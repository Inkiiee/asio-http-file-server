#include "http_util.h"

#include <sstream>
#include <iostream>

using namespace std;
using namespace http_parser;
using namespace http_maker;

http_base::HttpRequest Parser::parse_request(){
    http_base::HttpRequest request;

    stringstream stream(buffer_);
    string path_raw, method, version;
    stream >> method >> path_raw >> version;

    request.set_method(method);
    request.set_version(version);

    auto query_pos = path_raw.find('?');
    if(query_pos != string::npos)
        path_raw = path_raw.substr(0, query_pos);

    //url decode the path
    vector<uint8_t> path_bytes(path_raw.begin(), path_raw.end());
    auto path_decoded = http_base::url_decode(path_bytes);
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
        if(colon_pos != string::npos){
            string key = line.substr(0, colon_pos);
            string value = line.substr(colon_pos + 1);
            auto start = value.find_first_not_of(' ');
            if(start != string::npos)
                value = value.substr(start);
            else
                value.clear();
            request.add_header(key, value);
        }
    }

    return request;
}

http_base::HttpResponse Parser::parse_response(){
    http_base::HttpResponse response;

    stringstream stream(buffer_);
    string version;
    int status_code;
    string status_text;
    stream >> version >> status_code;
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
            response.add_header(key, value);
        }
    }

    return response;
}

string Maker::make_response(const http_base::HttpResponse& response){
    stringstream stream;
    stream << response.version() << " " << response.status_code() << " " << response.status_text() << "\r\n";
    for(const auto& header : response.headers()){
        stream << header.first << ": " << header.second << "\r\n";
    }
    stream << "\r\n";

    cout << "[HTTP] Sending response: " << response.status_code() << " " << response.status_text() << endl;
    for(const auto& header : response.headers()){
        cout << "[HTTP] Header: " << header.first << ": " << header.second << endl;
    }
    return stream.str();
}

string Maker::make_request(const http_base::HttpRequest& request){
    stringstream stream;
    string path = string(request.path().begin(), request.path().end());
    stream << request.method() << " " << path << " " << request.version() << "\r\n";
    for(const auto& header : request.headers()){
        stream << header.first << ": " << header.second << "\r\n";
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
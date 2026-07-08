#include "my_proto.h"
#include "http_util.h"
#include <nlohmann/json.hpp>

using namespace my_proto;
using namespace base_proto;
using namespace http_base;
using namespace http_file_system;
using namespace std;
using namespace http_maker;
namespace fs = std::filesystem;

void MyProto::set_data(const string& key, const string& value){
    nlohmann::json j;
    if(!data_.empty()){
        j = nlohmann::json::parse(data_, nullptr, false);
        if(j.is_discarded()) j = nlohmann::json::object();
    }
    j[key] = value;
    data_ = j.dump();
}
string MyProto::get_data(const string& key) const{
    if(data_.empty()) return "";
    nlohmann::json j = nlohmann::json::parse(data_, nullptr, false);
    if(j.is_discarded()) return "";
    return j.value(key, "");
}
string MyProto::serialize() const{
    return data_;
}

response_t MyProto::make_file_info(const request_t& request){
    data_.clear();

    fs::path file_path = request.first.path();
    if(!file_system_.is_exists(file_path)){
        HttpResponse response;
        response.set_status_code(404);
        response.add_header("Content-Length", "0");
        return {response, ""};
    }
    
    auto entry = file_system_.get_file_entry(file_path);
    
    auto encoded_name = url_encode(entry.name);
    string encoded_name_str(encoded_name.begin(), encoded_name.end());
    set_data("name", encoded_name_str);

    auto encoded_path_str = Maker::make_encoded_path(entry.path, entry.is_directory);
    set_data("path", encoded_path_str);
    set_data("is_directory", entry.is_directory ? "true" : "false");
    set_data("size", to_string(entry.size));
    set_data("extension", entry.extension);
    set_data("last_modified", entry.last_modified);
    
    HttpResponse response;
    response.set_status_code(200);
    response.add_header("Content-Type", "application/json");
    response.add_header("Content-Length", to_string(data_.size()));
    return {response, serialize()};
}
response_t MyProto::make_file_list_info(const request_t& request){
    data_.clear();

    fs::path dir_path = request.first.path();
    if(!file_system_.is_exists(dir_path)){
        HttpResponse response;
        response.set_status_code(404);
        response.add_header("Content-Length", "0");
        return {response, ""};
    }

    auto entries = file_system_.list_directory(dir_path, 0);
    nlohmann::json j_array = nlohmann::json::array();
    for(const auto& entry : entries){
        nlohmann::json j_entry;

        j_entry["name"] = string(entry.name.begin(), entry.name.end());
        
        auto encoded_path_str = Maker::make_encoded_path(entry.path, entry.is_directory);
        j_entry["path"] = encoded_path_str;
        j_entry["is_directory"] = entry.is_directory;
        j_entry["size"] = entry.size;
        j_entry["extension"] = entry.extension;
        j_entry["last_modified"] = entry.last_modified;

        j_array.push_back(j_entry);
    }
    data_ = j_array.dump();

    HttpResponse response;
    response.set_status_code(200);
    response.add_header("Content-Type", "application/json");
    response.add_header("Content-Length", to_string(data_.size()));
    return {response, serialize()};
}
fs::path MyProto::upload_file_path(const request_t& request){
    return file_system_.get_absolute_path(request.first.path());
}
fs::path MyProto::download_file_path(const request_t& request){
    return file_system_.get_absolute_path(request.first.path());
}
response_t MyProto::copy_file(const request_t& request){
    data_ = request.second;

    auto src_path_str = get_data("src");
    auto dest_path_str = get_data("dst");

    if(src_path_str.empty() || dest_path_str.empty()){
        HttpResponse response;
        response.set_status_code(400);
        set_data("error", "Invalid request: 'src' and 'dst' must be provided");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    if(!file_system_.is_exists(src_path_str)){
        HttpResponse response;
        response.set_status_code(404);
        data_.clear();
        set_data("error", "Source file does not exist");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    fs::path source_path(src_path_str);
    fs::path dest_path(dest_path_str);

    int result_code = file_system_.copy_file(source_path, dest_path, true);
    if(result_code < 200 || result_code >= 300){
        HttpResponse response;
        response.set_status_code(result_code);
        data_.clear();
        set_data("error", response.status_text());
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    HttpResponse response;
    response.set_status_code(result_code);
    data_.clear();
    set_data("success", response.status_text());
    response.add_header("Content-Type", "application/json");
    response.add_header("Content-Length", to_string(data_.size()));
    return {response, serialize()};
}
response_t MyProto::move_file(const request_t& request){
    data_ = request.second;

    auto src_path_str = get_data("src");
    auto dest_path_str = get_data("dst");

    if(src_path_str.empty() || dest_path_str.empty()){
        HttpResponse response;
        response.set_status_code(400);
        data_.clear();
        set_data("error", "Invalid request: 'src' and 'dst' must be provided");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    if(!file_system_.is_exists(src_path_str)){
        HttpResponse response;
        response.set_status_code(404);
        data_.clear();
        set_data("error", "Source file does not exist");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    fs::path source_path(src_path_str);
    fs::path dest_path(dest_path_str);

    int result_code = file_system_.move_file(source_path, dest_path, true);
    if(result_code < 200 || result_code >= 300){
        HttpResponse response;
        response.set_status_code(result_code);
        data_.clear();
        set_data("error", response.status_text());
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    HttpResponse response;
    response.set_status_code(result_code);
    data_.clear();
    set_data("success", response.status_text());
    response.add_header("Content-Type", "application/json");
    response.add_header("Content-Length", to_string(data_.size()));
    return {response, serialize()};
}
response_t MyProto::delete_file(const request_t& request){
    data_ = request.second;
    auto file_path_str = get_data("file_path");

    if(file_path_str.empty()){
        HttpResponse response;
        response.set_status_code(400);
        data_.clear();
        set_data("error", "Invalid request: 'file_path' must be provided");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    if(!file_system_.is_exists(file_path_str)){
        HttpResponse response;
        response.set_status_code(404);
        data_.clear();
        set_data("error", "File does not exist");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    fs::path file_path(file_path_str);

    int result_code = file_system_.delete_file(file_path);
    if(result_code < 200 || result_code >= 300){
        HttpResponse response;
        response.set_status_code(result_code);
        data_.clear();
        set_data("error", response.status_text());
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    HttpResponse response;
    response.set_status_code(result_code);
    data_.clear();
    set_data("success", response.status_text());
    response.add_header("Content-Type", "application/json");
    response.add_header("Content-Length", to_string(data_.size()));
    return {response, serialize()};
}
response_t MyProto::create_directory(const request_t& request){
    data_ = request.second;
    auto dir_path_str = get_data("dir_path");

    if(dir_path_str.empty()){
        HttpResponse response;
        response.set_status_code(400);
        data_.clear();
        set_data("error", "Invalid request: 'dir_path' must be provided");
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    fs::path dir_path(dir_path_str);

    int result_code = file_system_.create_directory(dir_path, true);
    if(result_code < 200 || result_code >= 300){
        HttpResponse response;
        response.set_status_code(result_code);
        data_.clear();
        set_data("error", response.status_text());
        response.add_header("Content-Type", "application/json");
        response.add_header("Content-Length", to_string(data_.size()));
        return {response, serialize()};
    }

    HttpResponse response;
    response.set_status_code(result_code);
    data_.clear();
    set_data("success", response.status_text());
    response.add_header("Content-Type", "application/json");
    response.add_header("Content-Length", to_string(data_.size()));
    return {response, serialize()};
}
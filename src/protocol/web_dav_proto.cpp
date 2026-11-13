#include "web_dav_proto.h"
#include "http_file_system.h"
#include "http_base.h"

#include <pugixml/pugixml.hpp>
#include <iostream>

using namespace std;
using namespace http_file_system;
using namespace http_base;
using namespace base_proto;
using namespace web_dav_proto;
namespace fs = std::filesystem;

static std::u8string extract_path_from_destination(const std::string& dest_header){
    // Destination: http://host/path or /path
    std::string url = dest_header;
    auto scheme_end = url.find("://");
    if(scheme_end != std::string::npos){
        auto path_start = url.find('/', scheme_end + 3);
        if(path_start != std::string::npos)
            url = url.substr(path_start);
        else
            url = "/";
    }
    vector<uint8_t> url_bytes(url.begin(), url.end());
    return url_decode(url_bytes);
}

vector<string> WebDavProto::get_requests(const string& xml_body){
    // RFC 4918: empty body means allprop
    if(xml_body.empty())
        return {"displayname", "getcontentlength", "getlastmodified", "resourcetype", "getcontenttype", "getetag"};

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_string(xml_body.c_str());

    if(!result)
        return {"displayname", "getcontentlength", "getlastmodified", "resourcetype", "getcontenttype", "getetag"};

    std::vector<std::string> properties;

    // namespace prefix가 D여도 pugixml에서는 태그 이름이 "D:propfind"처럼 보임
    pugi::xml_node propfind = doc.child("D:propfind");
    if (!propfind) {
        // 클라이언트가 prefix를 다르게 줄 수도 있으므로 local name 방식으로 찾는 게 더 안전함
        for (pugi::xml_node node : doc.children()) {
            std::string name = node.name();
            if (name == "propfind" || name.find(":propfind") != std::string::npos) {
                propfind = node;
                break;
            }
        }
    }
    if (!propfind) {
        std::cerr << "No propfind element\n";
        return {};
    }

    if(propfind.child("D:allprop") || propfind.child("allprop")) {
        // allprop 요청 시 모든 속성을 반환하도록 처리
        properties = {"displayname", "getcontentlength", "getlastmodified", "resourcetype", "getcontenttype", "getetag"};
        return properties;
    }

    pugi::xml_node prop = propfind.child("D:prop");
    if (!prop) {
        // 클라이언트가 prefix를 다르게 줄 수도 있으므로 local name 방식으로 찾는 게 더 안전함
        for (pugi::xml_node node : propfind.children()) {
            std::string name = node.name();
            if (name == "prop" || name.find(":prop") != std::string::npos) {
                prop = node;
                break;
            }
        }
    }
    if (!prop) {
        std::cerr << "No prop element\n";
        return {};
    }

    for (pugi::xml_node child : prop.children()) {
        std::string name = child.name();

        // "D:displayname" -> "displayname"으로 변환
        auto pos = name.find(':');
        if (pos != std::string::npos) {
            name = name.substr(pos + 1);
        }

        properties.push_back(name);
    }

    return properties;
}

void append_response_properties(pugi::xml_node& response, const HttpFileEntry& file_entry, const std::vector<std::string>& requested_properties) {
    pugi::xml_node href = response.append_child("D:href");

    std::string encoded_path;
    for (const auto& part : file_entry.path) {
        auto part_str = part.generic_u8string();
        if(part_str == u8"/" || part_str == u8"\\")
            continue;
        encoded_path += "/";
        auto encoded = url_encode(part_str);
        std::string encoded_str(encoded.begin(), encoded.end());
        encoded_path += encoded_str;
    }

    if (encoded_path.empty())
        encoded_path = "/";

    if (file_entry.is_directory && encoded_path.back() != '/')
        encoded_path += "/";

    href.text().set(encoded_path.c_str());

    pugi::xml_node propstat_200 = response.append_child("D:propstat");
    pugi::xml_node prop_200 = propstat_200.append_child("D:prop");

    pugi::xml_node propstat_404 = response.append_child("D:propstat");
    pugi::xml_node prop_404 = propstat_404.append_child("D:prop");

    for (const auto& property : requested_properties) {
        if (property == "displayname") {
            std::string display_name(file_entry.name.begin(), file_entry.name.end());
            prop_200.append_child("D:displayname").text().set(display_name.c_str());
        }
        else if (property == "getcontentlength") {
            if (!file_entry.is_directory) {
                prop_200.append_child("D:getcontentlength").text().set(std::to_string(file_entry.size).c_str());
            }
            else {
                prop_404.append_child("D:getcontentlength");
            }
        }
        else if (property == "getlastmodified") {
            prop_200.append_child("D:getlastmodified").text().set(file_entry.last_modified.c_str());
        }
        else if (property == "resourcetype") {
            pugi::xml_node resourcetype = prop_200.append_child("D:resourcetype");
            if (file_entry.is_directory) {
                resourcetype.append_child("D:collection");
            }
        }
        else if (property == "getcontenttype") {
            if (file_entry.is_directory) {
                prop_200.append_child("D:getcontenttype").text().set("httpd/unix-directory");
            }
            else {
                std::string content_type = get_mime_type(file_entry.extension);
                prop_200.append_child("D:getcontenttype").text().set(content_type.c_str());
            }
        }
        else if (property == "getetag") {
            if (!file_entry.is_directory) {
                prop_200.append_child("D:getetag").text().set(file_entry.etag.c_str());
            }
            else {
                prop_404.append_child("D:getetag");
            }
        }
        else {
            prop_404.append_child(("D:" + property).c_str());
        }
    }

    propstat_200.append_child("D:status").text().set("HTTP/1.1 200 OK");

    if (prop_404.first_child()) {
        propstat_404.append_child("D:status").text().set("HTTP/1.1 404 Not Found");
    } else {
        response.remove_child(propstat_404);
    }
}

string WebDavProto::get_response(const fs::path& path, int depth){
    pugi::xml_document doc;

    auto declaration = doc.prepend_child(pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "utf-8";
    
    pugi::xml_node multistatus = doc.append_child("D:multistatus");
    multistatus.append_attribute("xmlns:D") = "DAV:";

    pugi::xml_node response = multistatus.append_child("D:response");
    HttpFileEntry file_entry = file_system_.get_file_entry(path);

    auto requested_properties = get_requests(xml_body_);
    append_response_properties(response, file_entry, requested_properties);

    if(depth > 0 && file_entry.is_directory){
        auto entries = file_system_.list_directory(path, depth - 1);
        for(const auto& sub_entry: entries){
            pugi::xml_node sub_response = multistatus.append_child("D:response");
            append_response_properties(sub_response, sub_entry, requested_properties);
        }
    }

    std::ostringstream oss;
    doc.save(oss, "  ", pugi::format_default, pugi::encoding_utf8);
    return oss.str();
}

response_t WebDavProto::make_file_info(const request_t& request){
    set_xml_body(request.second);
    const auto& header = request.first.headers();
    int depth = 0;

    fs::path path(request.first.path());
    if(!file_system_.is_exists(path)){
        HttpResponse response;
        response.set_status_code(404);
        response.add_header("Content-Length", "0");
        return {response, ""};
    }

    string body =  get_response(path, depth);
    HttpResponse response;
    response.set_status_code(207);
    response.add_header("Content-Type", "application/xml; charset=utf-8");
    response.add_header("Content-Length", std::to_string(body.size()));
    return {response, body};
}

response_t WebDavProto::make_file_list_info(const request_t& request){
    set_xml_body(request.second);
    const auto& header = request.first.headers();
    int depth = 1;

    if(header.find("Depth") != header.end()){
        std::string depth_str = header.at("Depth");
        if(depth_str == "0") depth = 0;
        else if(depth_str == "1") depth = 1;
        else if(depth_str == "infinity") depth = -1;
    }

    fs::path path(request.first.path());
    if(!file_system_.is_exists(path)){
        HttpResponse response;
        response.set_status_code(404);
        response.add_header("Content-Length", "0");
        return {response, ""};
    }

    string body = get_response(path, depth);
    HttpResponse response;
    response.set_status_code(207);
    response.add_header("Content-Type", "application/xml; charset=utf-8");
    response.add_header("Content-Length", std::to_string(body.size()));
    return {response, body};
}

fs::path WebDavProto::upload_file_path(const request_t& request){
    return file_system_.get_absolute_path(request.first.path());
}

fs::path WebDavProto::download_file_path(const request_t& request){
    return file_system_.get_absolute_path(request.first.path());
}

response_t WebDavProto::copy_file(const request_t& request){
    if(!request.first.has_header("Destination")){
        HttpResponse response;
        response.set_status_code(400);
        response.add_header("Content-Length", "0");
        return {response, ""};
    }

    string dest_path_str = request.first.get_header("Destination");
    auto decoded_dest_path = extract_path_from_destination(dest_path_str);

    fs::path source_path(request.first.path());
    fs::path dest_path(decoded_dest_path);

    bool is_overwrite = false;
    if(request.first.has_header("Overwrite")){
        string overwrite_value = request.first.get_header("Overwrite");
        if(overwrite_value == "T" || overwrite_value == "t" || overwrite_value == "true" || overwrite_value == "TRUE")
            is_overwrite = true;
    }

    int result_code = file_system_.copy_file(source_path, dest_path, is_overwrite);
    if(result_code == 200) result_code = 201; // Created

    HttpResponse response;
    response.set_status_code(result_code);
    response.add_header("Content-Length", "0");
    return {response, ""};
}

response_t WebDavProto::move_file(const request_t& request){
    if(!request.first.has_header("Destination")){
        HttpResponse response;
        response.set_status_code(400);
        response.add_header("Content-Length", "0");
        return {response, ""};
    }
    string dest_path_str = request.first.get_header("Destination");
    auto decoded_dest_path = extract_path_from_destination(dest_path_str);

    fs::path source_path(request.first.path());
    fs::path dest_path(decoded_dest_path);

    bool is_overwrite = false;
    if(request.first.has_header("Overwrite")){
        string overwrite_value = request.first.get_header("Overwrite");
        if(overwrite_value == "T" || overwrite_value == "t" || overwrite_value == "true" || overwrite_value == "TRUE")
            is_overwrite = true;
    }

    int result_code = file_system_.move_file(source_path, dest_path, is_overwrite);
    if(result_code == 200) result_code = 201; // Created
    HttpResponse response;
    response.set_status_code(result_code);
    response.add_header("Content-Length", "0");
    return {response, ""};
}

response_t WebDavProto::delete_file(const request_t& request){
    fs::path file_path(request.first.path());
    int result_code = file_system_.delete_file(file_path);
    if(result_code == 200) result_code = 204; // No Content

    HttpResponse response;
    response.set_status_code(result_code);
    response.add_header("Content-Length", "0");
    return {response, ""};
}

response_t WebDavProto::create_directory(const request_t& request){
    fs::path dir_path(request.first.path());
    int result_code = file_system_.create_directory(dir_path);
    if(result_code == 200) result_code = 201; // Created
    HttpResponse response;
    response.set_status_code(result_code);
    response.add_header("Content-Length", "0");
    return {response, ""};
}

response_t WebDavProto::proppatch(const request_t& request){
    fs::path path(request.first.path());
    const string& xml_body = request.second;

    pugi::xml_document doc;
    pugi::xml_parse_result parse_result = doc.load_string(xml_body.c_str());

    // 207 Multi-Status 응답 생성
    pugi::xml_document resp_doc;
    auto declaration = resp_doc.prepend_child(pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "utf-8";

    pugi::xml_node multistatus = resp_doc.append_child("D:multistatus");
    multistatus.append_attribute("xmlns:D") = "DAV:";

    pugi::xml_node response_node = multistatus.append_child("D:response");

    std::string encoded_path;
    for(const auto& part : path){
        auto part_str = part.generic_u8string();
        if(part_str == u8"/" || part_str == u8"\\")
            continue;
        encoded_path += "/";
        auto encoded = url_encode(part_str);
        encoded_path += std::string(encoded.begin(), encoded.end());
    }
    if(encoded_path.empty()) encoded_path = "/";
    response_node.append_child("D:href").text().set(encoded_path.c_str());

    pugi::xml_node propstat = response_node.append_child("D:propstat");
    pugi::xml_node prop = propstat.append_child("D:prop");

    // 요청된 속성들을 파싱해서 200 OK로 응답
    if(parse_result){
        pugi::xml_node propertyupdate = doc.first_child();
        for(pugi::xml_node child : propertyupdate.children()){
            std::string child_name = child.name();
            if(child_name.find("set") != std::string::npos || child_name.find("remove") != std::string::npos){
                pugi::xml_node prop_node = child.first_child();
                for(pugi::xml_node p : prop_node.children()){
                    prop.append_child(p.name());
                }
            }
        }
    }

    propstat.append_child("D:status").text().set("HTTP/1.1 200 OK");

    std::ostringstream oss;
    resp_doc.save(oss, "  ", pugi::format_default, pugi::encoding_utf8);
    string body = oss.str();

    HttpResponse response;
    response.set_status_code(207);
    response.add_header("Content-Type", "application/xml; charset=utf-8");
    response.add_header("Content-Length", std::to_string(body.size()));
    return {response, body};
}

response_t WebDavProto::lock_resource(const request_t& request){
    fs::path path(request.first.path());

    auto now = std::chrono::system_clock::now();
    auto epoch = now.time_since_epoch().count();
    string lock_token = "opaquelocktoken:" + std::to_string(epoch);

    pugi::xml_document resp_doc;
    auto declaration = resp_doc.prepend_child(pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "utf-8";

    pugi::xml_node prop = resp_doc.append_child("D:prop");
    prop.append_attribute("xmlns:D") = "DAV:";

    pugi::xml_node lockdiscovery = prop.append_child("D:lockdiscovery");
    pugi::xml_node activelock = lockdiscovery.append_child("D:activelock");

    pugi::xml_node locktype = activelock.append_child("D:locktype");
    locktype.append_child("D:write");

    pugi::xml_node lockscope = activelock.append_child("D:lockscope");
    lockscope.append_child("D:exclusive");

    activelock.append_child("D:depth").text().set("infinity");
    activelock.append_child("D:timeout").text().set("Second-3600");

    pugi::xml_node locktoken = activelock.append_child("D:locktoken");
    locktoken.append_child("D:href").text().set(lock_token.c_str());

    std::ostringstream oss;
    resp_doc.save(oss, "  ", pugi::format_default, pugi::encoding_utf8);
    string body = oss.str();

    HttpResponse response;
    response.set_status_code(200);
    response.add_header("Content-Type", "application/xml; charset=utf-8");
    response.add_header("Content-Length", std::to_string(body.size()));
    response.add_header("Lock-Token", "<" + lock_token + ">");
    return {response, body};
}
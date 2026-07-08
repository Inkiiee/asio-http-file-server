#ifndef __WEB_DAV_PROTO_H__
#define __WEB_DAV_PROTO_H__

#include <string>
#include <vector>
#include "base_proto.h"

namespace web_dav_proto{
    class WebDavProto : public base_proto::BaseProto {
        std::string xml_body_;
    private:
        std::vector<std::string> get_requests(const std::string& xml_body);
        std::string get_response(const std::filesystem::path& path, int depth = 1);

        inline void set_xml_body(const std::string& xml_body){ xml_body_ = xml_body;}
    public:
        WebDavProto(): BaseProto(), xml_body_(""){}

        virtual base_proto::response_t make_file_info(const base_proto::request_t& request);
        virtual base_proto::response_t make_file_list_info(const base_proto::request_t& request);
        virtual std::filesystem::path upload_file_path(const base_proto::request_t& request);
        virtual std::filesystem::path download_file_path(const base_proto::request_t& request);
        virtual base_proto::response_t copy_file(const base_proto::request_t& request);
        virtual base_proto::response_t move_file(const base_proto::request_t& request);
        virtual base_proto::response_t delete_file(const base_proto::request_t& request);
        virtual base_proto::response_t create_directory(const base_proto::request_t& request);
        base_proto::response_t proppatch(const base_proto::request_t& request);
        base_proto::response_t lock_resource(const base_proto::request_t& request);
    };
}

#endif
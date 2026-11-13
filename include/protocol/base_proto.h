#ifndef __BASE_PROTO_H__
#define __BASE_PROTO_H__

#include <string>
#include <filesystem>

#include "http_base.h"
#include "http_file_system.h"

namespace base_proto{
    using response_t = std::pair<http_base::HttpResponse, std::string>; // <response, body>
    using request_t = std::pair<http_base::HttpRequest, std::string>; // <request, body>

    class BaseProto{
    protected:
        http_file_system::HttpFileSystem file_system_;
        std::filesystem::path root_path_;
    public:
        BaseProto() = default;
        ~BaseProto() = default;

        void set_root_path(const std::filesystem::path& path){
            root_path_ = path;
            file_system_.set_root_path(path.string());
        }

        virtual response_t make_file_info(const request_t& request) = 0;
        virtual response_t make_file_list_info(const request_t& request) = 0;
        virtual std::filesystem::path upload_file_path(const request_t& request) = 0;
        virtual std::filesystem::path download_file_path(const request_t& request) = 0;
        virtual response_t copy_file(const request_t& request) = 0;
        virtual response_t move_file(const request_t& request) = 0;
        virtual response_t delete_file(const request_t& request) = 0;
        virtual response_t create_directory(const request_t& request) = 0;

        bool is_directory(const std::filesystem::path& path){
            return file_system_.is_directory(path);
        }
        bool is_exists(const std::filesystem::path& path){
            return file_system_.is_exists(path);
        }
        http_file_system::HttpFileEntry get_file_info(const std::filesystem::path& path){
            return file_system_.get_file_entry(path);
        }
        std::optional<std::time_t> parse_http_date(const std::string& value){
            return file_system_.parse_http_date(value);
        }
        bool is_valid_path(const std::filesystem::path& path){
            return file_system_.is_valid_path(path);
        }
        std::string get_etag(const std::filesystem::path& path){
            auto entry = file_system_.get_file_entry(path);
            return entry.etag;
        }
    };
}
#endif // __BASE_PROTO_H__
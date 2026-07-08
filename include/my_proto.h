#ifndef __MY_PROTO_H__
#define __MY_PROTO_H__

#include <string>
#include <filesystem>

#include "base_proto.h"

namespace my_proto{
    class MyProto : public base_proto::BaseProto{
    private:
        std::string data_;

        void set_data(const std::string& key, const std::string& value);
        std::string get_data(const std::string& key) const;
        std::string serialize() const;
    public:
        MyProto(): BaseProto(), data_("") {};
        ~MyProto() = default;

        virtual base_proto::response_t make_file_info(const base_proto::request_t& request);
        virtual base_proto::response_t make_file_list_info(const base_proto::request_t& request);
        virtual std::filesystem::path upload_file_path(const base_proto::request_t& request);
        virtual std::filesystem::path download_file_path(const base_proto::request_t& request);
        virtual base_proto::response_t copy_file(const base_proto::request_t& request);
        virtual base_proto::response_t move_file(const base_proto::request_t& request);
        virtual base_proto::response_t delete_file(const base_proto::request_t& request);
        virtual base_proto::response_t create_directory(const base_proto::request_t& request);
    };
}

#endif // __MY_PROTO_H__
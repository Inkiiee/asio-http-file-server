#ifndef __HTTP_FILE_SYSTEM_H__
#define __HTTP_FILE_SYSTEM_H__

#include <filesystem>
#include <string>
#include <sstream>
#include <vector>
#include <cstdint>
#include <ctime>
#include <optional>
#include <random>
#include <unordered_set>

#include "http_file_entry.h"

namespace http_file_system{
    class RandomNameGeneration{
    public:
        RandomNameGeneration(int minimum = 0, int maximum = 9999): gen_(std::random_device{}()), dis_(minimum, maximum) {}
        std::filesystem::path gen(const std::filesystem::path& path){
            auto parent = path.parent_path();
            auto base_name = path.filename().string();
            auto name = base_name;
            do{
                name = base_name + std::to_string(dis_(gen_));
            }while(used_names_.find(name) != used_names_.end());
            used_names_.insert(name);
            return parent / name;
        }
        void return_path(const std::filesystem::path& path){
            auto parent = path.parent_path();
            auto base_name = path.filename().string();
            used_names_.erase(base_name);
        }
    private:
        std::mt19937 gen_;
        std::uniform_int_distribution<int> dis_;
        std::unordered_set<std::string> used_names_;
    };

    class HttpFileSystem{
    private:
        RandomNameGeneration temp_name_maker_;
        std::filesystem::path root_path_;

        inline bool safe_gmtime(const std::time_t* time, std::tm* result){
            #if defined(_WIN32) || defined(_WIN64)
                return gmtime_s(result, time) == 0;
            #else
                return gmtime_r(time, result) != nullptr;
            #endif
        }

    public:
        HttpFileSystem(const std::filesystem::path& root_path);
        HttpFileSystem(const std::string& root_path);
        HttpFileSystem();

        std::filesystem::path get_root_path() const;
        std::filesystem::path get_absolute_path(const std::filesystem::path& path);
        int move_file(const std::filesystem::path& source_path, const std::filesystem::path& dest_path, bool is_overwrite = false);
        int delete_file(const std::filesystem::path& file_path);
        int create_directory(const std::filesystem::path& dir_path, bool is_recursive = false);
        int copy_file(const std::filesystem::path& source_path, const std::filesystem::path& dest_path, bool is_overwrite = false);
        void set_root_path(const std::filesystem::path& path);
        std::vector<HttpFileEntry> list_directory(const std::filesystem::path& dir_path, int depth = 0);
        HttpFileEntry get_file_entry(const std::filesystem::path& path);
        bool is_directory(const std::filesystem::path& path);
        bool is_exists(const std::filesystem::path& path);
        bool is_valid_path(const std::filesystem::path& path);

        inline std::filesystem::path borrow_temp_path(const std::filesystem::path& path){
            return temp_name_maker_.gen(path);
        }
        inline void return_temp_path(const std::filesystem::path& path){
            temp_name_maker_.return_path(path);
        }

        inline std::optional<std::time_t> parse_http_date(const std::string& value) {
            std::tm tm = {};
            std::istringstream iss(value);
            iss.imbue(std::locale::classic());

            iss >> std::get_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");

            if (iss.fail())
                return std::nullopt;

        #if defined(_WIN32) || defined(_WIN64)
            return _mkgmtime(&tm);
        #else
            return timegm(&tm);
        #endif
        }
    };
}
#endif // __HTTP_FILE_SYSTEM_H__
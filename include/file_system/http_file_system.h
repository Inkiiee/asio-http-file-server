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
#include <unordered_map>

#include "http_file_entry.h"

namespace http_file_system{
    class RandomNameGeneration{
    public:
        RandomNameGeneration(int minimum = 0, int maximum = 9999): gen_(std::random_device{}()), dis_(minimum, maximum) {}
        std::filesystem::path gen(const std::filesystem::path& path){
            auto parent = path.parent_path();
            auto base_name = path.stem().string();
            auto name = base_name;
            int count = 0;
            do{
                name = base_name + std::to_string(dis_(gen_));
                count++;
            }while(count < 50 && (used_names_.find(name) != used_names_.end() || is_exists(parent / name)));

            if(count >= 50 && (used_names_.find(name) != used_names_.end() || is_exists(parent / name)))
                throw std::runtime_error("Unable to create temporary folder.");

            if(create_temp_directory(parent / name)){
                used_names_[name] = 1;
                return parent / name;
            }
            return "";
        }
        void increase_use_count(const std::filesystem::path& path){
            auto parent = path.parent_path();
            auto base_name = path.filename().string();
            if(used_names_.find(base_name) != used_names_.end())
                used_names_[base_name]++;
        }
        void return_path(const std::filesystem::path& path){
            auto parent = path.parent_path();
            auto base_name = path.filename().string();
            if(used_names_.find(base_name) != used_names_.end()){
                if(--used_names_[base_name] <= 0){
                    used_names_.erase(base_name);
                    remove_temp_dir(path);
                }
            }
        }
    private:
        std::mt19937 gen_;
        std::uniform_int_distribution<int> dis_;
        std::unordered_map<std::string, int> used_names_;
        std::string temp_dir_name_;

        bool is_exists(const std::filesystem::path& path){
            std::error_code ec;
            return std::filesystem::exists(path, ec) && !ec;
        }
        bool remove_temp_dir(const std::filesystem::path& path){
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            return !ec;
        }
        bool create_temp_directory(const std::filesystem::path& path){
            std::error_code ec;
            bool created = std::filesystem::create_directory(path, ec);
            if (ec && ec != std::errc::file_exists)
                throw std::filesystem::filesystem_error("Failed Create Temp Directory.", path, ec);

            return created;
        }
    };

    class TempName{
    public:
        TempName(const std::filesystem::path& base){
            base_name_ = base.filename().string();
            int count = 0;
            do {
                path_ = std::move(gen_.gen(base));
                count++;
            } while(path_ == "" && count < 10);

            if(path_ == "") throw std::runtime_error("Failed Create TempName object.");
        }
        ~TempName(){
            gen_.return_path(path_);
        }
        TempName(const TempName& t){
            path_ = t.path_;
            base_name_ = t.base_name_;
            gen_.increase_use_count(path_);
        }
        TempName(TempName&& t){
            path_ = std::move(t.path_);
            base_name_ = std::move(t.base_name_);
            t.base_name_ = "";
            t.path_ = "";
        }
        TempName& operator=(const TempName& t){
            gen_.return_path(path_);
            path_ = t.path_;
            base_name_ = t.base_name_;
            gen_.increase_use_count(path_);
            return *this;
        }
        TempName& operator=(TempName&& t){
            gen_.return_path(path_);
            path_ = std::move(t.path_);
            base_name_ = std::move(t.base_name_);
            t.base_name_ = "";
            t.path_ = "";
            return *this;
        }
        const std::filesystem::path get_path(){return path_ / base_name_;}
    private:
        std::filesystem::path path_;
        std::string base_name_;
        inline static RandomNameGeneration gen_{0, 9999};
    };

    class HttpFileSystem{
    private:
        std::filesystem::path root_path_;

        bool copy(const std::filesystem::path& abs_source, const std::filesystem::path& abs_dest);
        bool rename(const std::filesystem::path& abs_source, const std::filesystem::path& abs_dest);
        bool remove(const std::filesystem::path& abs_path);
        bool atomic_copy(const std::filesystem::path& abs_source, const std::filesystem::path& abs_dest);
        bool atomic_move(const std::filesystem::path& abs_source, const std::filesystem::path& abs_dest);

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
        bool is_sub_path(const std::filesystem::path& parent, const std::filesystem::path& child);
        bool is_valid_path(const std::filesystem::path& path);

        inline TempName get_temp_path(const std::filesystem::path& path){ return TempName{path}; }
        inline std::optional<std::time_t> parse_http_date(const std::string& value) {
            if(value.size() != 29 || !value.ends_with(" GMT"))
                return std::nullopt;
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

#include "http_file_system.h"

#include <algorithm>
#include <chrono>

namespace fs = std::filesystem;
using namespace http_file_system;

fs::path HttpFileSystem::get_root_path() const{
    return root_path_;
}

// 경로 검사 (이미 절대 경로가 전달된다고 가정)
bool HttpFileSystem::is_valid_path(const fs::path& path){
    std::error_code ec;
    auto canonical      = fs::weakly_canonical(path, ec);
    if(ec) return false;
    auto root_canonical = fs::weakly_canonical(root_path_, ec);
    if(ec) return false;

    auto it_root = root_canonical.begin();
    auto it_path = canonical.begin();
    for(; it_root != root_canonical.end(); ++it_root, ++it_path){
        if(it_path == canonical.end() || *it_path != *it_root)
            return false;
    }
    return true;
}

// 절대 경로 반환
fs::path HttpFileSystem::get_absolute_path(const fs::path& path){
    return root_path_ / path.relative_path();
}

// 파일 이동
int HttpFileSystem::move_file(const fs::path& source_path, const fs::path& dest_path, bool is_overwrite){
    const auto& abs_source = get_absolute_path(source_path);
    const auto& abs_dest   = get_absolute_path(dest_path);

    if(!is_valid_path(abs_source) || !is_valid_path(abs_dest))
        return 403; // Forbidden
    
    if(!fs::exists(abs_source))
        return 404; // Not Found

    if(fs::exists(abs_dest)){
        if(!is_overwrite)
            return 409; // Conflict
        
        // 덮어쓰기일 경우, 소스와 대상이 서로 다른 타입이면 대상 삭제
        bool is_source_dir = fs::is_directory(abs_source);
        bool is_dest_dir   = fs::is_directory(abs_dest);
        if(is_source_dir != is_dest_dir && is_overwrite)
            fs::remove_all(abs_dest);
    }

    std::error_code ec;
    fs::rename(abs_source, abs_dest, ec);
    if(ec)
        return 500; // Internal Server Error
    return 200; // OK
}

// 파일 삭제
int HttpFileSystem::delete_file(const fs::path& file_path){
    const auto& abs_path = get_absolute_path(file_path);
    if(!is_valid_path(abs_path))
        return 403; // Forbidden
    
    if(!fs::exists(abs_path))
        return 404; // Not Found
    
    std::error_code ec;
    fs::remove_all(abs_path, ec);
    if(ec)
        return 500; // Internal Server Error
    return 200; // OK
}

// 디렉토리 생성
int HttpFileSystem::create_directory(const fs::path& dir_path, bool is_recursive){
    const auto& abs_path = get_absolute_path(dir_path);
    if(!is_valid_path(abs_path))
        return 403; // Forbidden
    
    std::error_code ec;
    if(is_recursive)
        fs::create_directories(abs_path, ec);
    else
        fs::create_directory(abs_path, ec);
    
    if(ec)
        return 500; // Internal Server Error
    return 200; // OK
}

// 파일 복사
int HttpFileSystem::copy_file(const fs::path& source_path, const fs::path& dest_path, bool is_overwrite){
    const auto& abs_source = get_absolute_path(source_path);
    const auto& abs_dest   = get_absolute_path(dest_path);

    if(!is_valid_path(abs_source) || !is_valid_path(abs_dest))
        return 403; // Forbidden
    
    if(!fs::exists(abs_source))
        return 404; // Not Found

    if(fs::exists(abs_dest)){
        if(!is_overwrite)
            return 409; // Conflict
        
        // 덮어쓰기일 경우, 소스와 대상이 서로 다른 타입이면 대상 삭제
        bool is_source_dir = fs::is_directory(abs_source);
        bool is_dest_dir   = fs::is_directory(abs_dest);
        if(is_source_dir != is_dest_dir && is_overwrite)
            fs::remove_all(abs_dest);
    }

    std::error_code ec;
    fs::copy(abs_source, abs_dest, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if(ec)
        return 500; // Internal Server Error
    return 200; // OK
}

// 루트 경로 설정
void HttpFileSystem::set_root_path(const fs::path& path){
    root_path_ = fs::canonical(path);
}

// 디렉토리 목록 반환
std::vector<HttpFileEntry> HttpFileSystem::list_directory(const fs::path& dir_path, int depth){
    const auto& abs_path = get_absolute_path(dir_path);
    if(!is_valid_path(abs_path))
        return {};

    std::vector<HttpFileEntry> entries;
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(abs_path, ec)){
        if(ec) break;

        HttpFileEntry file_entry;
        file_entry.name = entry.path().filename().generic_u8string();
        file_entry.path = (dir_path / file_entry.name);
        file_entry.parent_path = dir_path.generic_u8string();
        file_entry.is_directory = entry.is_directory(ec);
        file_entry.size = entry.is_regular_file(ec) ? entry.file_size(ec) : 0;
        file_entry.extension = entry.path().extension().string();

        auto last_write_time = entry.last_write_time(ec);
        if(!ec){
            auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                last_write_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
            auto tt = std::chrono::system_clock::to_time_t(sctp);
            std::tm tm_result;
            if(safe_gmtime(&tt, &tm_result)){
                std::ostringstream oss;
                oss.imbue(std::locale::classic());
                oss << std::put_time(&tm_result, "%a, %d %b %Y %H:%M:%S GMT");
                file_entry.last_modified = oss.str();
            } else {
                file_entry.last_modified = "N/A";
            }
            auto mtime = last_write_time.time_since_epoch().count();
            file_entry.etag = "\"" + std::to_string(mtime) +  "-" + std::to_string(file_entry.size) + "\"";
        }

        if(file_entry.is_directory && depth > 0){
            HttpFileSystem sub_fs(abs_path);
            file_entry.sub = sub_fs.list_directory(entry.path(), depth - 1);
        }

        entries.push_back(std::move(file_entry));
    }

    sort(entries.begin(), entries.end(),
        [](const HttpFileEntry& a, const HttpFileEntry& b){
            if(a.is_directory != b.is_directory)
                return a.is_directory > b.is_directory;
            return a.name < b.name;
        });

    return entries;
}

HttpFileEntry HttpFileSystem::get_file_entry(const fs::path& path){
    const auto& abs_path = get_absolute_path(path);
    if(!is_valid_path(abs_path))
        return {};

    if(!fs::exists(abs_path))
        return {};

    HttpFileEntry file_entry;
    file_entry.name = abs_path.filename().generic_u8string();
    file_entry.path = path;
    file_entry.parent_path = path.parent_path().generic_u8string();
    file_entry.is_directory = fs::is_directory(abs_path);
    file_entry.size = fs::is_regular_file(abs_path) ? fs::file_size(abs_path) : 0;
    file_entry.extension = abs_path.extension().string();

    auto last_write_time = fs::last_write_time(abs_path);
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        last_write_time - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    auto tt = std::chrono::system_clock::to_time_t(sctp);
    std::tm tm_result;
    if(safe_gmtime(&tt, &tm_result)){
        std::ostringstream oss;
        oss.imbue(std::locale::classic());
        oss << std::put_time(&tm_result, "%a, %d %b %Y %H:%M:%S GMT");
        file_entry.last_modified = oss.str();
    } else {
        file_entry.last_modified = "N/A";
    }

    auto mtime = last_write_time.time_since_epoch().count();
    file_entry.etag = "\"" + std::to_string(mtime) +  "-" + std::to_string(file_entry.size) + "\"";

    return file_entry;
}

bool HttpFileSystem::is_directory(const fs::path& path){
    return fs::is_directory(get_absolute_path(path));
}

bool HttpFileSystem::is_exists(const fs::path& path){
    return fs::exists(get_absolute_path(path));
}

HttpFileSystem::HttpFileSystem(const fs::path& root_path): root_path_(fs::canonical(root_path)) {}
HttpFileSystem::HttpFileSystem(const std::string& root_path): root_path_(fs::canonical(fs::path(root_path))) {}
HttpFileSystem::HttpFileSystem(): root_path_(fs::current_path()) {}
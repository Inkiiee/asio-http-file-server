#include "http_file_system.h"

#include <algorithm>
#include <chrono>
#include <iostream>

namespace fs = std::filesystem;
using namespace http_file_system;

fs::path HttpFileSystem::get_root_path() const{
    return root_path_;
}

bool HttpFileSystem::copy(const fs::path& abs_source, const fs::path& abs_dest){
    std::error_code ec;
    fs::copy(abs_source, abs_dest, fs::copy_options::recursive, ec);
    if(ec) return false;
    return true;
}
bool HttpFileSystem::rename(const fs::path& abs_source, const fs::path& abs_dest){
    std::error_code ec;
    fs::rename(abs_source, abs_dest, ec);
    if(ec) return false;
    return true;
}
bool HttpFileSystem::remove(const fs::path& abs_path){
    std::error_code ec;
    fs::remove_all(abs_path, ec);
    if(ec) return false;
    return true;
}

bool HttpFileSystem::atomic_copy(const fs::path& abs_source, const fs::path& abs_dest){
    try{
        TempName backup_dest(abs_dest), temp(abs_dest);
        if(!copy(abs_source, temp.get_path())) // copy source to temp_path in abs_dest's parent path
            throw std::runtime_error("failed copy source -> temp.");

        // backup abs_dest
        std::error_code exists_ec;
        bool is_exists_dest = fs::exists(abs_dest, exists_ec) && !exists_ec;
        if(is_exists_dest){
            if(!rename(abs_dest, backup_dest.get_path())){
                std::cerr << "[atomic copy] failed make backup file.\n\tRecovering ..." << std::endl;
                if(!remove(temp.get_path()))
                    std::cerr << "[atomic copy] failed recovery." << std::endl;
                throw std::runtime_error("failed backup destination.");
            }
        }

        // move temp to abs_dest
        if(!rename(temp.get_path(), abs_dest)){
            std::cerr << "[atomic copy] failed copy file.\n\tRecovering ..." << std::endl;
            if((is_exists_dest && !rename(backup_dest.get_path(), abs_dest)) || !remove(temp.get_path()))
                std::cerr << "[atomic copy] failed recovery." << std::endl;
            throw std::runtime_error("failed move temp -> destination.");
        }

        if(is_exists_dest && !remove(backup_dest.get_path()))
            throw std::runtime_error("failed remove backup file.");
    }
    catch(const std::exception& e){
        std::cerr << "[atomic copy] failed: " << e.what() << std::endl;
        return false;
    }
    return true;
}

bool HttpFileSystem::atomic_move(const fs::path& abs_source, const fs::path& abs_dest){
    try{
        TempName backup_dest(abs_dest), temp(abs_dest);
        if(!copy(abs_source, temp.get_path())) // copy source to temp_path in abs_dest's parent path
            throw std::runtime_error("failed move source -> temp.");

        // backup abs_dest
        std::error_code exists_ec;
        bool is_exists_dest = fs::exists(abs_dest, exists_ec) && !exists_ec;
        if(is_exists_dest){
            if(!rename(abs_dest, backup_dest.get_path())){
                std::cerr << "[atomic move] failed make backup file.\n\tRecovering ..." << std::endl;
                if(!remove(temp.get_path()))
                    std::cerr << "[atomic move] failed recovery." << std::endl;
                throw std::runtime_error("failed backup destination.");
            }
        }

        // move temp to abs_dest
        if(!rename(temp.get_path(), abs_dest)){
            std::cerr << "[atomic move] failed move file.\n\tRecovering ..." << std::endl;
            if((is_exists_dest && !rename(backup_dest.get_path(), abs_dest)) || !remove(temp.get_path()))
                std::cerr << "[atomic move] failed recovery." << std::endl;
            throw std::runtime_error("failed move temp -> destination.");
        }

        if(!remove(abs_source) || (is_exists_dest && !remove(backup_dest.get_path())))
            throw std::runtime_error("failed remove temp & backup file.");
    }
    catch(const std::exception& e){
        std::cerr << "[atomic move] failed: " << e.what() << std::endl;
        return false;
    }
    return true;
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
    const bool is_dir_dest = is_directory(dest_path);
    const auto& abs_source = get_absolute_path(source_path);
    auto abs_dest   = get_absolute_path(dest_path);
    if(!is_valid_path(abs_source) || !is_valid_path(abs_dest)){
        std::cout << "[move_file] invalid path" << std::endl;
        return 403; // Forbidden
    }

    std::error_code exists_ec;
    if(!fs::exists(abs_source, exists_ec) || exists_ec){
        std::cout << "[move_file] source: " << abs_source.string() << " is not exists" << std::endl;
        return 404;
    }
    bool is_exists_dest = fs::exists(abs_dest, exists_ec) && !exists_ec;
    if(is_exists_dest && !is_overwrite){
        std::cout << "[move_file] destination exists " << abs_dest.string() << " but not set overwrite option" << std::endl;
        return 409; // Conflict
    }

    if(is_sub_path(abs_source, abs_dest)){
        std::cout << "[move_file] source: " << abs_source.string() << " is parent of destination: " << abs_dest.string() << std::endl;
        return 400;
    }

    if(is_dir_dest)
        abs_dest = abs_dest / (abs_source.filename() != "" ? abs_source.filename() : abs_source.parent_path().filename());

    std::error_code equiv_ec;
    if(fs::equivalent(abs_dest, abs_source, equiv_ec) && !equiv_ec){
        return 200; //같은 파일의 이동은 이동한 것으로 판정.
    }
    else if(equiv_ec){
        std::cout << "[move_file] validation check error(filesystem error): " << equiv_ec.message() << std::endl;
        return 500;
    }

    if(!atomic_move(abs_source, abs_dest)){
        std::cout << "[move_file] filesystem error" << std::endl;
        return 500;
    }

    return 200; // OK
}

// 파일 삭제
int HttpFileSystem::delete_file(const fs::path& file_path){
    const auto& abs_path = get_absolute_path(file_path);
    const auto& abs_root = root_path_;
    std::error_code equiv_ec;
    if(!is_valid_path(abs_path) || fs::equivalent(abs_path, abs_root, equiv_ec) || equiv_ec){
        std::cout << "[delete_file] invalid path: " << abs_path.string() << std::endl;
        return 403; // Forbidden
    }
    if(!fs::exists(abs_path)){
        std::cout << "[delete_file] target path: " << abs_path.string() << " not exists" << std::endl;
        return 404; // Not Found
    }
    if(!remove(abs_path)){
        std::cout << "[delete_file] filesystem error" << std::endl;
        return 500;
    }

    return 200; // OK
}

// 디렉토리 생성
int HttpFileSystem::create_directory(const fs::path& dir_path, bool is_recursive){
    const auto& abs_path = get_absolute_path(dir_path);
    if(!is_valid_path(abs_path)){
        std::cout << "[create_directory] invalid path: " << abs_path.string() << std::endl;
        return 403; // Forbidden
    }
    std::error_code ec;
    if(is_recursive)
        fs::create_directories(abs_path, ec);
    else
        fs::create_directory(abs_path, ec);
    
    if(ec){
        std::cout << "[create_directory] create directory error" << std::endl;
        return 500; // Internal Server Error
    }
    return 200; // OK
}

// 파일 복사
int HttpFileSystem::copy_file(const fs::path& source_path, const fs::path& dest_path, bool is_overwrite){
    const bool is_dir_dest = is_directory(dest_path);
    const auto& abs_source = get_absolute_path(source_path);
    auto abs_dest   = get_absolute_path(dest_path);
    if(!is_valid_path(abs_source) || !is_valid_path(abs_dest))
        return 403; // Forbidden
    
    std::error_code exists_ec;
    if(!fs::exists(abs_source, exists_ec) || exists_ec){
        std::cout << "[copy_file] source: " << abs_source.string() << " is not exists" << std::endl;
        return 404;
    }

    bool is_exists_dest = fs::exists(abs_dest, exists_ec) && !exists_ec;
    if(is_exists_dest && !is_overwrite){
        std::cout << "[copy_file] destination exists " << abs_dest.string() << " but not set overwrite option" << std::endl;
        return 409;
    }

    if(is_sub_path(abs_source, abs_dest)){
        std::cout << "[copy_file] source: " << abs_source.string() << " is parent of destination: " << abs_dest.string() << std::endl;
        return 400;
    }

    if(is_dir_dest)
        abs_dest = abs_dest / (abs_source.filename() != "" ? abs_source.filename() : abs_source.parent_path().filename());

    std::error_code equiv_ec;
    if(fs::equivalent(abs_dest, abs_source, equiv_ec) && !equiv_ec){
        return 200; //같은 파일의 복사 복사한 것으로 판정.
    }
    else if(equiv_ec){
        std::cout << "[copy_file] validation check error(filesystem error): " << equiv_ec.message() << std::endl;
        return 500;
    }

    if(!atomic_copy(abs_source, abs_dest)){
        std::cout << "[copy_file] filesystem error" << std::endl;
        return 500;
    }

    return 200;
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
    std::error_code ec;
    return fs::exists(get_absolute_path(path), ec) && !ec;
}

bool HttpFileSystem::is_sub_path(const fs::path& parent, const fs::path& child){
    fs::path p = fs::weakly_canonical(parent);
    fs::path c = fs::weakly_canonical(child);
    fs::path rel = c.lexically_relative(p);

    return !rel.empty() && *rel.begin() != "..";
}

HttpFileSystem::HttpFileSystem(const fs::path& root_path): root_path_(fs::canonical(root_path)) {}
HttpFileSystem::HttpFileSystem(const std::string& root_path): root_path_(fs::canonical(fs::path(root_path))) {}
HttpFileSystem::HttpFileSystem(): root_path_(fs::current_path()) {}
#ifndef __HTTP_FILE_ENTRY_H__
#define __HTTP_FILE_ENTRY_H__

#include <string>
#include <filesystem>
#include <vector>
#include <cstdint>

namespace http_file_system{
    struct HttpFileEntry{
        std::u8string name;
        std::filesystem::path path;
        std::u8string parent_path;
        bool is_directory;
        uint64_t size;
        std::string extension;
        std::string last_modified;
        std::string etag;
        std::vector<HttpFileEntry> sub;
    };
}

#endif // __HTTP_FILE_ENTRY_H__
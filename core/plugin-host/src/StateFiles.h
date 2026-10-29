// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <cstdlib>
#include <unistd.h>

namespace oscairn::state_files
{
namespace fs=std::filesystem;
class TemporaryDirectory
{
public:
    fs::path path;
    explicit TemporaryDirectory(const fs::path& parent=fs::temp_directory_path())
    {
        auto pattern=(parent/".oscairn-state-XXXXXX").string();
        if(!::mkdtemp(pattern.data()))throw std::runtime_error("Cannot create state working directory");
        path=fs::canonical(pattern);
    }
    ~TemporaryDirectory(){std::error_code error;if(!path.empty())fs::remove_all(path,error);}
    TemporaryDirectory(const TemporaryDirectory&)=delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&)=delete;
};
class Bundle
{
    fs::path destination_;
    static fs::path destination(const std::string& path)
    {
        if(path.empty() || path.find('\0')!=std::string::npos)throw std::invalid_argument("Invalid state directory");
        auto result=fs::absolute(path).lexically_normal();
        if(fs::exists(result) || fs::is_symlink(result))throw std::runtime_error("State destination already exists: "+result.string());
        fs::create_directories(result.parent_path());return result;
    }
public:
    TemporaryDirectory temporary;
    explicit Bundle(const std::string& path):destination_(destination(path)),temporary(destination_.parent_path()){}
    void publish()
    {
        if(fs::exists(destination_) || fs::is_symlink(destination_))throw std::runtime_error("State destination already exists");
        fs::rename(temporary.path,destination_);temporary.path.clear();
    }
};
inline void write(const fs::path& path,const void* data,size_t size)
{
    std::ofstream file(path,std::ios::binary);file.write(static_cast<const char*>(data),size);
    file.close(); // Buffered write failures can first appear when closing.
    if(!file)throw std::runtime_error("Cannot write state file: "+path.string());
}
inline std::vector<uint8_t> read(const fs::path& path,size_t limit)
{
    const auto size=fs::file_size(path);if(size>limit)throw std::runtime_error("State file exceeds size limit");
    std::vector<uint8_t> data(size);std::ifstream file(path,std::ios::binary);
    file.read(reinterpret_cast<char*>(data.data()),data.size());
    if(!file)throw std::runtime_error("Cannot read state file: "+path.string());
    return data;
}
// Lilv links external assets. Materialize those links in our private staging bundle
// so a published snapshot can move independently of the original sample library.
inline void materialize(const fs::path& directory)
{
    std::vector<fs::path> links;
    for(const auto& entry:fs::recursive_directory_iterator(directory))if(entry.is_symlink())links.push_back(entry.path());
    for(const auto& link:links)
    {
        const auto source=fs::canonical(link);
        if(!fs::is_regular_file(source))throw std::runtime_error("State asset must be a regular file: "+source.string());
        const auto copy=link.string()+".oscairn-copy";
        fs::copy_file(source,copy);fs::remove(link);fs::rename(copy,link);
    }
}
}

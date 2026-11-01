// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/package.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "miniz/miniz.h"
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        using V = const json_value_s*;
        constexpr size_t maxEntries = 65536, maxPath = 1024;
        constexpr uint64_t maxEntry = 2ull << 30, maxTotal = 8ull << 30;
        const std::set<std::string> tops{"Performances", "Sounds", "Assets"};

        [[noreturn]] void bad(const std::string& why) { throw std::runtime_error("not a usable Oscairn archive: " + why); }
        std::string text(V v)
        {
            if(!v || v->type != json_type_string) bad("the manifest is missing a name, ID or path");
            const auto* s = static_cast<const json_string_s*>(v->payload);
            return {s->string, s->string_size};
        }
        // A safe relative path: plain components, none empty, "." or "..", hidden or with a
        // separator, drive or control character in it.
        bool safe(const std::string& name)
        {
            if(name.empty() || name.size() > maxPath) return false;
            size_t start = 0;
            for(;;)
            {
                const auto end = name.find('/', start);
                const auto part = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
                if(part.empty() || part[0] == '.' || part.size() > 255) return false;
                if(std::any_of(part.begin(), part.end(), [](char c) { return c == '\\' || c == ':' || static_cast<unsigned char>(c) < 32; })) return false;
                if(end == std::string::npos) return true;
                start = end + 1;
            }
        }
        struct Zip
        {
            mz_zip_archive zip{};
            bool reading;
            explicit Zip(bool r) : reading(r) {}
            ~Zip() { reading ? mz_zip_reader_end(&zip) : mz_zip_writer_end(&zip); }
        };
    }

    std::string manifestJson(const Package& package)
    {
        std::string items;
        for(const auto& i : package.items)
            items += std::string(items.empty() ? "" : ",") + "{\"kind\":" + detail::quote(i.kind) + ",\"id\":" + detail::quote(i.id) + ",\"name\":" + detail::quote(i.name) + ",\"path\":" + detail::quote(i.path) + '}';
        return "{\"format\":\"oscairn.library\",\"version\":1,\"id\":" + detail::quote(package.id) + ",\"name\":" + detail::quote(package.name) +
               ",\"kind\":" + detail::quote(package.kind) + ",\"items\":[" + items + "]}\n";
    }

    void writePackage(const fs::path& archive, const Package& package, const fs::path& root)
    {
        if(fs::exists(archive)) throw std::runtime_error(archive.string() + " already exists");
        const auto manifest = manifestJson(package);
        const auto temporary = archive.parent_path() / ("." + archive.filename().string() + ".tmp");
        fs::remove(temporary);
        {
            Zip z(false);
            if(!mz_zip_writer_init_file(&z.zip, temporary.string().c_str(), 0)) throw std::runtime_error("cannot write " + archive.string());
            if(!mz_zip_writer_add_mem(&z.zip, "library.json", manifest.data(), manifest.size(), MZ_DEFAULT_COMPRESSION)) throw std::runtime_error("cannot write the manifest");
            std::vector<fs::path> files;
            for(const auto& top : tops)
                if(fs::is_directory(root / top))
                    for(const auto& e : fs::recursive_directory_iterator(root / top))
                    {
                        if(e.is_symlink()) throw std::runtime_error("a link in what is being packed: " + e.path().string());
                        if(e.is_regular_file()) files.push_back(e.path());
                    }
            std::sort(files.begin(), files.end());
            for(const auto& f : files)
            {
                const auto name = fs::relative(f, root).generic_string();
                if(!safe(name)) throw std::runtime_error("a file that cannot be packed: " + name);
                if(!mz_zip_writer_add_file(&z.zip, name.c_str(), f.string().c_str(), nullptr, 0, MZ_DEFAULT_COMPRESSION))
                    throw std::runtime_error("cannot pack " + name);
            }
            if(!mz_zip_writer_finalize_archive(&z.zip)) throw std::runtime_error("cannot finish " + archive.string());
        }
        if(fs::exists(archive)) { fs::remove(temporary); throw std::runtime_error(archive.string() + " already exists"); }
        fs::rename(temporary, archive);
    }

    Package readPackage(const fs::path& archive, const fs::path& into)
    {
        const bool extract = !into.empty();
        if(extract && fs::exists(into)) throw std::runtime_error(into.string() + " already exists");
        if(extract) fs::create_directories(into);
        try
        {
            Zip z(true);
            if(!mz_zip_reader_init_file(&z.zip, archive.string().c_str(), 0)) bad("it is not a zip archive");
            const auto count = mz_zip_reader_get_num_files(&z.zip);
            if(count > maxEntries) bad("it holds too many files");
            std::set<std::string> names;
            uint64_t total = 0;
            std::string manifest;
            for(mz_uint i = 0; i < count; ++i)
            {
                mz_zip_archive_file_stat st;
                if(!mz_zip_reader_file_stat(&z.zip, i, &st)) bad("an entry cannot be read");
                std::string name = st.m_filename;
                const bool directory = mz_zip_reader_is_file_a_directory(&z.zip, i);
                if(directory && !name.empty() && name.back() == '/') name.pop_back();
                if(!safe(name)) bad("an entry with an unsafe name: " + name);
                const auto top = name.substr(0, name.find('/'));
                if(name != "library.json" && !tops.count(top)) bad("an entry outside its folders: " + name);
                if(((st.m_external_attr >> 16) & S_IFMT) == S_IFLNK) bad("a link: " + name);
                if(!names.insert(name).second) bad("an entry twice: " + name);
                if(directory) { if(extract) fs::create_directories(into / name); continue; }
                if(st.m_uncomp_size > maxEntry || (total += st.m_uncomp_size) > maxTotal) bad("it is too large");
                if(name == "library.json")
                {
                    if(st.m_uncomp_size > (4u << 20)) bad("the manifest is too large");
                    manifest.resize(size_t(st.m_uncomp_size));
                    if(!mz_zip_reader_extract_to_mem(&z.zip, i, manifest.data(), manifest.size(), 0)) bad("the manifest cannot be read");
                    continue;
                }
                if(!extract) continue;
                const auto out = into / name;
                fs::create_directories(out.parent_path());
                if(!mz_zip_reader_extract_to_file(&z.zip, i, out.string().c_str(), 0)) bad("cannot extract " + name);
                if(fs::file_size(out) != st.m_uncomp_size) bad("an entry is not the size it says: " + name);
            }
            if(manifest.empty()) bad("it has no library.json");
            if(!detail::jsonPreflight(manifest, 16).empty()) bad("its manifest is not valid JSON");
            const std::unique_ptr<json_value_s, decltype(&std::free)> doc{json_parse(manifest.data(), manifest.size()), &std::free};
            V d = doc.get();
            if(text(detail::member(d, "format")) != "oscairn.library") bad("it is not an Oscairn library");
            const V version = detail::member(d, "version");
            if(!version || version->type != json_type_number || std::string(static_cast<const json_number_s*>(version->payload)->number, static_cast<const json_number_s*>(version->payload)->number_size) != "1")
                bad("its version is not 1");
            Package p;
            p.id = text(detail::member(d, "id")); p.name = text(detail::member(d, "name")); p.kind = text(detail::member(d, "kind"));
            if(p.id.empty() || p.id.size() > 64 || (p.kind != "library" && p.kind != "performance")) bad("its ID or kind is wrong");
            const V items = detail::member(d, "items");
            if(!items || items->type != json_type_array) bad("it lists no items");
            std::set<std::string> ids;
            for(auto* e = static_cast<const json_array_s*>(items->payload)->start; e; e = e->next)
            {
                Package::Item item{text(detail::member(e->value, "kind")), text(detail::member(e->value, "id")), text(detail::member(e->value, "name")), text(detail::member(e->value, "path"))};
                const bool sound = item.kind == "sound";
                if((!sound && item.kind != "performance") || item.id.empty() || !ids.insert(item.id).second) bad("an item's kind or ID is wrong");
                if(!safe(item.path) || item.path.substr(0, item.path.find('/')) != (sound ? "Sounds" : "Performances") || item.path.find('/', item.path.find('/') + 1) != std::string::npos ||
                   !names.count(item.path + (sound ? "/sound.json" : "/performance.json")))
                    bad("an item is not in the archive: " + item.path);
                p.items.push_back(std::move(item));
            }
            return p;
        }
        catch(...) { std::error_code ignored; if(extract) fs::remove_all(into, ignored); throw; }
    }
}

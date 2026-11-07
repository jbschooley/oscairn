// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/package.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "miniz/miniz.h"
#include "oscairn/assets.h"
#include "oscairn/library.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <memory>
#include <fstream>
#include <set>
#include <stdexcept>
#include <sys/stat.h>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        using V = const json_value_s*;
        constexpr size_t maxEntries = 1u << 20, maxPath = 1024; // a library of thousands of items, each a few files
        constexpr uint64_t maxEntry = 2ull << 30, maxTotal = 8ull << 30, maxNameBytes = 64ull << 20;
        const std::set<std::string> tops{"Performances", "Sounds", "Assets", "Plugins"};
        bool hidden(const fs::path& relative) { for(const auto& part : relative) if(!part.empty() && part.string()[0] == '.') return true; return false; }

        [[noreturn]] void bad(const std::string& why) { throw std::runtime_error("not a usable Oscairn archive: " + why); }
        std::string text(V v)
        {
            if(!v || v->type != json_type_string) bad("the manifest is missing a name, ID or path");
            const auto* s = static_cast<const json_string_s*>(v->payload);
            return {s->string, s->string_size};
        }
        // The package's ID becomes the library's folder name, and an exported item's ID is its
        // package's, so both are plain: 1 to 64 letters, digits, ".", "-" and "_", starting with a
        // letter or digit and not ending in "." (Windows drops it from a folder's name).
        bool plainId(std::string_view id)
        {
            return !id.empty() && id.size() <= 64 && std::isalnum(static_cast<unsigned char>(id.front())) && id.back() != '.' &&
                   std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; });
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

    std::string platform()
    {
#if defined(__APPLE__)
        const std::string os = "macos";
#elif defined(_WIN32)
        const std::string os = "windows";
#else
        const std::string os = "linux";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        return os + "-arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        return os + "-x86_64";
#else
        return os + "-other";
#endif
    }

    bool knownPlatform(const std::string& name)
    {
        static const std::set<std::string> platforms{"macos-arm64", "macos-x86_64", "linux-arm64", "linux-x86_64", "windows-x86_64"};
        return platforms.count(name) != 0;
    }

    std::string pluginFolderName(const std::string& id, const std::string& version)
    {
        std::string safe;
        for(const char c : id + "@" + version) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '@' || c == '_') ? c : '_';
        return safe;
    }

    std::vector<fs::path> pluginStoreFolders()
    {
        std::vector<fs::path> out;
        std::error_code e;
        for(const auto* format : {"clap", "lv2"})
            for(const auto& entry : fs::directory_iterator(dataHome() / "Plugins" / format, e))
                if(entry.path().filename().string()[0] != '.' && fs::is_directory(entry.path() / platform(), e)) out.push_back(entry.path() / platform());
        std::sort(out.begin(), out.end());
        return out;
    }

    std::string bundleDigest(const fs::path& build)
    {
        if(fs::is_regular_file(build)) return assets::sha256File(build);
        std::vector<fs::path> files;
        for(const auto& e : fs::recursive_directory_iterator(build))
        {
            const auto relative = fs::relative(e.path(), build);
            if(e.is_symlink()) throw std::runtime_error("a link in a plugin build: " + e.path().string());
            if(e.is_regular_file() && !hidden(relative)) files.push_back(relative);
        }
        std::sort(files.begin(), files.end());
        std::string lines;
        for(const auto& f : files) lines += f.generic_string() + '\n' + assets::sha256File(build / f) + '\n';
        return assets::sha256(lines);
    }

    std::string manifestJson(const Package& package)
    {
        std::string items, plugins, files;
        auto signatures = [](const std::vector<signing::Signature>& list) { std::string out; for(const auto& s : list) out += (out.empty() ? "" : ",") + signing::json(s); return "[" + out + "]"; };
        for(const auto& i : package.items)
            items += std::string(items.empty() ? "" : ",") + "{\"kind\":" + detail::quote(i.kind) + ",\"id\":" + detail::quote(i.id) + ",\"name\":" + detail::quote(i.name) + ",\"path\":" + detail::quote(i.path) +
                     ",\"signatures\":" + signatures(i.signatures) + '}';
        for(const auto& p : package.plugins)
        {
            std::string builds;
            for(const auto& b : p.builds) builds += std::string(builds.empty() ? "" : ",") + "{\"platform\":" + detail::quote(b.platform) + ",\"path\":" + detail::quote(b.path) + ",\"sha256\":" + detail::quote(b.sha256) +
                                                    ",\"signatures\":" + signatures(b.signatures) + '}';
            plugins += std::string(plugins.empty() ? "" : ",") + "{\"format\":" + detail::quote(p.format) + ",\"id\":" + detail::quote(p.id) + ",\"version\":" + detail::quote(p.version) +
                       ",\"name\":" + detail::quote(p.name) + ",\"builds\":[" + builds + "]}";
        }
        for(const auto& [path, sha] : package.files) files += std::string(files.empty() ? "" : ",") + detail::quote(path) + ":" + detail::quote(sha);
        return "{\"format\":\"oscairn.library\",\"version\":3,\"id\":" + detail::quote(package.id) + ",\"name\":" + detail::quote(package.name) +
               ",\"kind\":" + detail::quote(package.kind) + ",\"items\":[" + items + "],\"plugins\":[" + plugins + "],\"files\":{" + files + "}}\n";
    }

    void writePackage(const fs::path& archive, const Package& package, const fs::path& root, const signing::Key* key)
    {
        if(fs::exists(archive)) throw std::runtime_error(archive.string() + " already exists");
        // Every file, and its SHA-256 for the manifest; hidden files are not packed.
        std::vector<fs::path> files;
        for(const auto& top : tops)
            if(fs::is_directory(root / top))
                for(const auto& e : fs::recursive_directory_iterator(root / top))
                {
                    if(e.is_symlink()) throw std::runtime_error("a link in what is being packed: " + e.path().string());
                    if(e.is_regular_file() && !hidden(fs::relative(e.path(), root))) files.push_back(e.path());
                }
        std::sort(files.begin(), files.end());
        auto listed = package;
        for(const auto& f : files) listed.files[fs::relative(f, root).generic_string()] = assets::sha256File(f);
        const auto manifest = manifestJson(listed);
        const auto temporary = archive.parent_path() / ("." + archive.filename().string() + ".tmp");
        fs::remove(temporary);
        {
            Zip z(false);
            if(!mz_zip_writer_init_file(&z.zip, temporary.string().c_str(), 0)) throw std::runtime_error("cannot write " + archive.string());
            if(!mz_zip_writer_add_mem(&z.zip, "library.json", manifest.data(), manifest.size(), MZ_DEFAULT_COMPRESSION)) throw std::runtime_error("cannot write the manifest");
            if(key)
            {
                const auto sig = signing::json({key->publicKey, signing::sign(*key, signing::archiveMessage(manifest))}) + '\n';
                if(!mz_zip_writer_add_mem(&z.zip, "library.sig", sig.data(), sig.size(), MZ_DEFAULT_COMPRESSION)) throw std::runtime_error("cannot write the archive's signature");
            }
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

    void signPackage(const fs::path& archive, const signing::Key& key)
    {
        const auto unpacked = archive.parent_path() / ("." + archive.filename().string() + ".signing");
        const auto signedCopy = archive.parent_path() / ("." + archive.filename().string() + ".signed");
        std::error_code ignored;
        fs::remove_all(unpacked, ignored); fs::remove(signedCopy, ignored);
        try
        {
            writePackage(signedCopy, readPackage(archive, unpacked), unpacked, &key);
            fs::rename(signedCopy, archive);
        }
        catch(...) { fs::remove_all(unpacked, ignored); fs::remove(signedCopy, ignored); throw; }
        fs::remove_all(unpacked, ignored);
    }

    Package readPackage(const fs::path& archive, const fs::path& into)
    {
        const bool extract = !into.empty();
        if(extract && fs::exists(into)) throw std::runtime_error(into.string() + " already exists");
        if(extract) fs::create_directories(into);
        try
        {
            // Its entries: a zip's, read in place, or an unpacked folder's (RFC 0031), whose hidden
            // files, which desktops add, are skipped. Each is checked before any is written.
            struct Entry { std::string name; uint64_t size; bool directory, link; mz_uint index; };
            std::vector<Entry> entries;
            Zip z(true);
            const bool folder = fs::is_directory(archive);
            if(folder)
            {
                for(auto it = fs::recursive_directory_iterator(archive); it != fs::recursive_directory_iterator(); ++it)
                {
                    const auto relative = fs::relative(it->path(), archive);
                    if(hidden(relative)) { it.disable_recursion_pending(); continue; }
                    const bool link = it->is_symlink(), directory = !link && it->is_directory();
                    entries.push_back({relative.generic_string(), directory || link ? 0 : it->file_size(), directory, link, 0});
                    if(entries.size() > maxEntries) bad("it holds too many files");
                }
            }
            else
            {
                if(!mz_zip_reader_init_file(&z.zip, archive.string().c_str(), 0)) bad("it is not a zip archive");
                const auto count = mz_zip_reader_get_num_files(&z.zip);
                if(count > maxEntries) bad("it holds too many files");
                for(mz_uint i = 0; i < count; ++i)
                {
                    mz_zip_archive_file_stat st;
                    if(!mz_zip_reader_file_stat(&z.zip, i, &st)) bad("an entry cannot be read");
                    std::string name = st.m_filename;
                    const bool directory = mz_zip_reader_is_file_a_directory(&z.zip, i);
                    if(directory && !name.empty() && name.back() == '/') name.pop_back();
                    entries.push_back({name, st.m_uncomp_size, directory, ((st.m_external_attr >> 16) & S_IFMT) == S_IFLNK, i});
                }
            }
            // A small entry, whole: from the zip, or the folder's file read and checked for its size.
            auto small = [&](const Entry& e) {
                std::string bytes(size_t(e.size), '\0');
                if(folder)
                {
                    std::ifstream in(archive / e.name, std::ios::binary);
                    if(!in.read(bytes.data(), std::streamsize(bytes.size())) || in.peek() != EOF) bad("cannot read " + e.name);
                }
                else if(!mz_zip_reader_extract_to_mem(&z.zip, e.index, bytes.data(), bytes.size(), 0)) bad("cannot read " + e.name);
                return bytes;
            };
            std::set<std::string> names;
            uint64_t total = 0, nameBytes = 0, largest = 0;
            std::string manifest, archiveSignature;
            for(const auto& e : entries)
            {
                const auto& name = e.name;
                if(!safe(name)) bad("an entry with an unsafe name: " + name);
                const auto top = name.substr(0, name.find('/'));
                if(name != "library.json" && name != "library.sig" && !tops.count(top)) bad("an entry outside its folders: " + name);
                if(e.link) bad("a link: " + name);
                if((nameBytes += name.size()) > maxNameBytes) bad("its names are too long");
                if(!names.insert(name).second) bad("an entry twice: " + name);
                if(e.directory) continue;
                if(e.size > maxEntry || (total += e.size) > maxTotal) bad("it is too large");
                largest = std::max(largest, e.size);
                if(name == "library.json")
                {
                    if(e.size > maxManifestBytes) bad("the manifest is too large");
                    manifest = small(e);
                }
                if(name == "library.sig")
                {
                    if(e.size > 4096) bad("the archive's signature is too large");
                    archiveSignature = small(e);
                }
            }
            if(manifest.empty()) bad("it has no library.json");
            // Room for what it holds and one entry more (RFC 0031), before anything is written.
            if(extract && fs::space(into).available < total + largest) bad("there is not room for it on this device: it needs " + std::to_string((total + largest) >> 20) + " MiB");
            if(extract)
                for(const auto& e : entries)
                {
                    const auto out = into / e.name;
                    if(e.directory) { fs::create_directories(out); continue; }
                    if(e.name == "library.json" || e.name == "library.sig") continue;
                    fs::create_directories(out.parent_path());
                    if(folder) fs::copy_file(archive / e.name, out);
                    else if(!mz_zip_reader_extract_to_file(&z.zip, e.index, out.string().c_str(), 0)) bad("cannot extract " + e.name);
                    if(fs::file_size(out) != e.size) bad("an entry is not the size it says: " + e.name);
                }
            if(!detail::jsonPreflight(manifest, 16).empty()) bad("its manifest is not valid JSON");
            const std::unique_ptr<json_value_s, decltype(&std::free)> doc{json_parse(manifest.data(), manifest.size()), &std::free};
            V d = doc.get();
            if(text(detail::member(d, "format")) != "oscairn.library") bad("it is not an Oscairn library");
            const V version = detail::member(d, "version");
            if(!version || version->type != json_type_number || std::string(static_cast<const json_number_s*>(version->payload)->number, static_cast<const json_number_s*>(version->payload)->number_size) != "3")
                bad("its version is not 3");
            Package p;
            p.manifestSha256 = assets::sha256(manifest);
            // Signatures, each checked against what it signs; one that does not verify is refused like a wrong hash.
            auto signatures = [](V list) { try { return signing::signatures(list); } catch(const std::runtime_error& e) { bad(e.what()); } };
            if(!archiveSignature.empty())
            {
                if(!detail::jsonPreflight(archiveSignature, 2).empty()) bad("its signature is not valid JSON");
                const std::unique_ptr<json_value_s, decltype(&std::free)> sig{json_parse(archiveSignature.data(), archiveSignature.size()), &std::free};
                try { p.signature = signing::signatureFrom(sig.get()); } catch(const std::runtime_error& e) { bad(e.what()); }
                if(!signing::verify(p.signature, signing::archiveMessage(manifest))) bad("its signature does not verify");
            }
            p.id = text(detail::member(d, "id")); p.name = text(detail::member(d, "name")); p.kind = text(detail::member(d, "kind"));
            if(!plainId(p.id) || (p.kind != "library" && p.kind != "performance" && p.kind != "sound" && p.kind != "plugin")) bad("its ID or kind is wrong");
            const V items = detail::member(d, "items");
            if(!items || items->type != json_type_array) bad("it lists no items");
            std::set<std::string> ids;
            for(auto* e = static_cast<const json_array_s*>(items->payload)->start; e; e = e->next)
            {
                Package::Item item{text(detail::member(e->value, "kind")), text(detail::member(e->value, "id")), text(detail::member(e->value, "name")), text(detail::member(e->value, "path"))};
                const bool sound = item.kind == "sound";
                if((!sound && item.kind != "performance") || !plainId(item.id) || !ids.insert(item.id).second) bad("an item's kind or ID is wrong");
                if(!safe(item.path) || item.path.substr(0, item.path.find('/')) != (sound ? "Sounds" : "Performances") || item.path.find('/', item.path.find('/') + 1) != std::string::npos ||
                   !names.count(item.path + (sound ? "/sound.json" : "/performance.json")))
                    bad("an item is not in the archive: " + item.path);
                item.signatures = signatures(detail::member(e->value, "signatures"));
                p.items.push_back(std::move(item));
            }
            // Plugins: each build in the archive, under Plugins/.
            const V plugins = detail::member(d, "plugins");
            if(!plugins || plugins->type != json_type_array) bad("it lists no plugins");
            for(auto* e = static_cast<const json_array_s*>(plugins->payload)->start; e; e = e->next)
            {
                Package::Plugin plugin{text(detail::member(e->value, "format")), text(detail::member(e->value, "id")), text(detail::member(e->value, "version")), text(detail::member(e->value, "name")), {}};
                if((plugin.format != "clap" && plugin.format != "lv2") || plugin.id.empty() || plugin.id.size() > 256 || plugin.version.size() > 128) bad("a plugin's format, ID or version is wrong");
                const V builds = detail::member(e->value, "builds");
                if(!builds || builds->type != json_type_array) bad("a plugin lists no builds");
                for(auto* b = static_cast<const json_array_s*>(builds->payload)->start; b; b = b->next)
                {
                    Package::Build build{text(detail::member(b->value, "platform")), text(detail::member(b->value, "path")), text(detail::member(b->value, "sha256"))};
                    const auto inside = names.lower_bound(build.path + "/");
                    const bool there = names.count(build.path) || (inside != names.end() && inside->rfind(build.path + "/", 0) == 0);
                    if(!safe(build.path) || build.path.rfind("Plugins/", 0) != 0 || !there || build.sha256.size() != 64)
                        bad("a plugin build is not in the archive: " + build.path);
                    // The store only finds .clap and .lv2 bundles; a bare library file installs but is never loaded.
                    const std::string suffix = plugin.format == "lv2" ? ".lv2" : ".clap";
                    const std::string leaf { build.path.substr(build.path.rfind('/') + 1) };
                    if(leaf.size() < suffix.size() || leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) != 0)
                        bad("a plugin build is not a .clap or .lv2 bundle: " + build.path);
                    if(!knownPlatform(build.platform)) bad("a plugin build for no known platform: " + build.platform);
                    build.signatures = signatures(detail::member(b->value, "signatures"));
                    for(const auto& s : build.signatures)
                        if(!signing::verify(s, signing::buildMessage(plugin.format, plugin.id, plugin.version, build.platform, build.sha256))) bad("a plugin build's signature does not verify: " + build.path);
                    plugin.builds.push_back(std::move(build));
                }
                p.plugins.push_back(std::move(plugin));
            }
            // A plugin package carries plugin builds and nothing else.
            if(p.kind == "plugin" && (!p.items.empty() || p.plugins.empty())) bad("a plugin package holds plugins and no items");
            // Every file's SHA-256, as the manifest lists it, and each build's digest.
            const V files = detail::member(d, "files");
            if(!files || files->type != json_type_object) bad("it lists no files");
            for(auto* e = static_cast<const json_object_s*>(files->payload)->start; e; e = e->next)
                p.files[std::string(e->name->string, e->name->string_size)] = text(e->value);
            if(extract)
            {
                size_t found = 0;
                for(const auto& e : fs::recursive_directory_iterator(into))
                {
                    if(!e.is_regular_file()) continue;
                    const auto name = fs::relative(e.path(), into).generic_string();
                    const auto listed = p.files.find(name);
                    if(listed == p.files.end()) bad("a file the manifest does not list: " + name);
                    if(assets::sha256File(e.path()) != listed->second) bad("a file that is not what the manifest says: " + name);
                    ++found;
                }
                if(found != p.files.size()) bad("a file the manifest lists is not in it");
                for(const auto& plugin : p.plugins)
                    for(const auto& build : plugin.builds)
                        if(bundleDigest(into / build.path) != build.sha256) bad("a plugin build that is not what the manifest says: " + build.path);
                for(const auto& item : p.items)
                {
                    if(item.signatures.empty()) continue;
                    std::string hash;
                    try { hash = contentHash(into / item.path / (item.kind == "sound" ? "sound.json" : "performance.json")); } catch(const std::runtime_error&) { bad("a signed item without its info: " + item.path); }
                    for(const auto& s : item.signatures) if(!signing::verify(s, signing::itemMessage(hash))) bad("an item's signature does not verify: " + item.path);
                }
            }
            return p;
        }
        catch(...) { std::error_code ignored; if(extract) fs::remove_all(into, ignored); throw; }
    }
}

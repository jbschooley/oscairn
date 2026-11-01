// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/library.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "oscairn/assets.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        using V = const json_value_s*;
        using Doc = std::unique_ptr<json_value_s, decltype(&std::free)>;
        constexpr size_t maxFile = 4 * 1024 * 1024, maxTags = 32, maxTag = 64, maxRecent = 50;

        std::string slurp(const fs::path& file)
        {
            std::ifstream in(file, std::ios::binary);
            if(!in) throw std::runtime_error("cannot read " + file.string());
            std::ostringstream out; out << in.rdbuf();
            auto text = out.str();
            if(text.size() > maxFile) throw std::runtime_error(file.string() + " is too large");
            return text;
        }
        Doc parse(const std::string& text)
        {
            if(!detail::jsonPreflight(text, 32).empty()) return {nullptr, &std::free};
            return {json_parse(text.data(), text.size()), &std::free};
        }
        std::string str(V v)
        {
            if(!v || v->type != json_type_string) throw std::runtime_error("expected a string");
            const auto* s = static_cast<const json_string_s*>(v->payload);
            return {s->string, s->string_size};
        }
        std::vector<std::string> strings(V v)
        {
            std::vector<std::string> out;
            if(!v || v->type != json_type_array) throw std::runtime_error("expected a list");
            for(auto* e = static_cast<const json_array_s*>(v->payload)->start; e; e = e->next) out.push_back(str(e->value));
            return out;
        }
        std::string list(const std::vector<std::string>& values)
        {
            std::string out = "[";
            for(const auto& v : values) { if(out.size() > 1) out += ','; out += detail::quote(v); }
            return out + ']';
        }
        int64_t stamp(const fs::path& file) { return int64_t(fs::last_write_time(file).time_since_epoch().count()); }
        void writeAtomically(const fs::path& file, const std::string& text)
        {
            fs::create_directories(file.parent_path());
            const auto temporary = file.parent_path() / ("." + file.filename().string() + ".tmp");
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << text; if(!out) throw std::runtime_error("cannot write " + file.string()); }
            fs::rename(temporary, file);
        }
        std::string lower(std::string s) { for(auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
    }

    fs::path dataHome()
    {
        if(const char* home = std::getenv("OSCAIRN_DATA_HOME"); home && *home) return home;
        const char* user = std::getenv("HOME");
        if(!user || !*user) throw std::runtime_error("cannot find the user's home folder for the library");
#ifdef __APPLE__
        return fs::path(user) / "Library/Application Support/Oscairn";
#else
        if(const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return fs::path(xdg) / "oscairn";
        return fs::path(user) / ".local/share/oscairn";
#endif
    }

    bool validColor(const std::string& c)
    {
        return c.empty() || (c.size() == 7 && c[0] == '#' && std::all_of(c.begin() + 1, c.end(), [](char x) { return std::isxdigit(static_cast<unsigned char>(x)) != 0; }));
    }

    std::string contentHash(std::string_view document, const fs::path& nodes)
    {
        std::string summary(document);
        summary += '\n';
        std::vector<fs::path> files;
        std::error_code error;
        if(fs::is_directory(nodes, error))
            for(const auto& e : fs::recursive_directory_iterator(nodes, error)) if(e.is_regular_file()) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for(const auto& f : files) summary += fs::relative(f, nodes).generic_string() + ' ' + assets::sha256File(f) + '\n';
        return assets::sha256(summary);
    }

    std::string contentHash(const fs::path& file)
    {
        const auto text = slurp(file);
        // The top-level info object, by its key, skipped to its matching brace outside strings.
        const auto key = text.find(",\"info\":{");
        if(key == std::string::npos) throw std::runtime_error("no info block in " + file.string());
        size_t at = key + 8, depth = 0;
        bool quoted = false, escaped = false;
        for(; at < text.size(); ++at)
        {
            const char ch = text[at];
            if(quoted) { if(escaped) escaped = false; else if(ch == '\\') escaped = true; else if(ch == '"') quoted = false; continue; }
            if(ch == '"') quoted = true;
            else if(ch == '{') ++depth;
            else if(ch == '}' && --depth == 0) break;
        }
        if(at >= text.size()) throw std::runtime_error("an unfinished info block in " + file.string());
        return contentHash(text.substr(0, key + 8) + text.substr(at + 1), file.parent_path() / "nodes");
    }

    Control::PerformanceInfo readInfo(const fs::path& file, const std::string& format)
    {
        const auto doc = parse(slurp(file));
        V d = doc.get();
        if(!d || str(detail::member(d, "format")) != format) throw std::runtime_error("not a " + format);
        const V version = detail::member(d, "version");
        if(!version || version->type != json_type_number || std::string(static_cast<const json_number_s*>(version->payload)->number, static_cast<const json_number_s*>(version->payload)->number_size) != "4")
            throw std::runtime_error("unsupported version");
        const V info = detail::member(d, "info");
        if(!info || info->type != json_type_object) throw std::runtime_error("no info block");
        Control::PerformanceInfo out;
        out.id = str(detail::member(info, "id")); out.name = str(detail::member(info, "name"));
        out.description = str(detail::member(info, "description")); out.tags = strings(detail::member(info, "tags"));
        out.color = str(detail::member(info, "color"));
        out.revision = str(detail::member(info, "revision")); out.parent = str(detail::member(info, "parent")); out.hash = str(detail::member(info, "hash"));
        if(out.id.empty() || out.id.size() > 64 || !validColor(out.color) || out.name.size() > 256 || out.description.size() > 4096 || out.tags.size() > maxTags ||
           std::any_of(out.tags.begin(), out.tags.end(), [](const std::string& t) { return t.empty() || t.size() > maxTag; }))
            throw std::runtime_error("invalid info");
        return out;
    }

    Library::Library(fs::path home)
    {
        if(home.empty()) home = dataHome();
        performances_ = home / "Performances"; sounds_ = home / "Sounds"; cache_ = home / "library";
        readUser();
        rescan();
    }

    uint32_t Library::rescan()
    {
        // What was read before, by folder and the time its file had.
        std::map<std::string, std::pair<int64_t, Control::LibraryItem>> cached;
        try
        {
            const auto doc = parse(slurp(cache_ / "index.json"));
            if(V items = detail::member(doc.get(), "items"); items && items->type == json_type_array)
                for(auto* e = static_cast<const json_array_s*>(items->payload)->start; e; e = e->next)
                {
                    Control::LibraryItem item;
                    item.path = str(detail::member(e->value, "path")); item.id = str(detail::member(e->value, "id")); item.kind = str(detail::member(e->value, "kind"));
                    item.name = str(detail::member(e->value, "name")); item.description = str(detail::member(e->value, "description"));
                    item.tags = strings(detail::member(e->value, "tags")); item.color = str(detail::member(e->value, "color")); item.revision = str(detail::member(e->value, "revision")); item.hash = str(detail::member(e->value, "hash"));
                    const auto modified = std::stoll(str(detail::member(e->value, "modified")));
                    cached[item.path] = {modified, item};
                }
        }
        catch(const std::exception&) { cached.clear(); } // no index, or a damaged one: read every file
        items_.clear();
        std::string index;
        std::error_code error;
        for(const auto& [folder, kind, file] : {std::tuple{performances_, "performance", "performance.json"}, std::tuple{sounds_, "sound", "sound.json"}})
        if(fs::is_directory(folder, error))
            for(const auto& entry : fs::directory_iterator(folder, error))
            {
                const auto manifest = entry.path() / file;
                // Hidden folders are a save in progress or one left by a crash.
                if(!entry.is_directory() || entry.path().filename().string().starts_with(".") || !fs::is_regular_file(manifest, error)) continue;
                const auto path = entry.path().string();
                const auto modified = stamp(manifest);
                Control::LibraryItem item;
                if(const auto it = cached.find(path); it != cached.end() && it->second.first == modified) item = it->second.second;
                else
                {
                    try
                    {
                        const auto info = readInfo(manifest, std::string("oscairn.") + kind);
                        item.kind = kind; item.id = info.id; item.name = info.name; item.description = info.description; item.tags = info.tags; item.color = info.color; item.revision = info.revision; item.hash = info.hash; item.path = path;
                    }
                    catch(const std::exception&) { continue; } // not one the library can show; it stays on disk
                }
                if(std::any_of(items_.begin(), items_.end(), [&](const auto& i) { return i.id == item.id; })) continue; // a copied folder: the first wins
                items_.push_back(item);
                index += std::string(index.empty() ? "" : ",") + "{\"path\":" + detail::quote(item.path) + ",\"kind\":" + detail::quote(item.kind) + ",\"modified\":" + detail::quote(std::to_string(modified)) +
                         ",\"id\":" + detail::quote(item.id) + ",\"name\":" + detail::quote(item.name) + ",\"description\":" + detail::quote(item.description) + ",\"tags\":" + list(item.tags) + ",\"color\":" + detail::quote(item.color) + ",\"revision\":" + detail::quote(item.revision) + ",\"hash\":" + detail::quote(item.hash) + '}';
            }
        try { writeAtomically(cache_ / "index.json", "{\"items\":[" + index + "]}\n"); } catch(const std::exception&) {} // a cache: reading works without it
        // Favourites and recents of items that went stay in the user file, for when they return.
        return uint32_t(items_.size());
    }

    Control::LibraryResult Library::query(const Control::LibraryQuery& q) const
    {
        Control::LibraryResult result;
        const auto text = lower(q.text);
        std::vector<Control::LibraryItem> found;
        for(auto item : items_)
        {
            item.favourite = std::find(favourites_.begin(), favourites_.end(), item.id) != favourites_.end();
            const auto r = std::find(recent_.begin(), recent_.end(), item.id);
            item.recent = r == recent_.end() ? 0 : uint32_t(r - recent_.begin()) + 1;
            // Every chosen tag (AND), the text in its name, tags or description, and the toggles.
            if(!std::all_of(q.tags.begin(), q.tags.end(), [&](const std::string& t) { return std::find(item.tags.begin(), item.tags.end(), t) != item.tags.end(); })) continue;
            if(q.favourites && !item.favourite) continue;
            if(!q.kind.empty() && item.kind != q.kind) continue;
            if(q.recent && !item.recent) continue;
            if(!text.empty())
            {
                std::string hay = lower(item.name + ' ' + item.description);
                for(const auto& t : item.tags) hay += ' ' + lower(t);
                if(hay.find(text) == std::string::npos) continue;
            }
            found.push_back(item);
        }
        // Recent first when asked for, else by name.
        std::sort(found.begin(), found.end(), [&](const auto& a, const auto& b) { return q.recent ? a.recent < b.recent : lower(a.name) < lower(b.name); });
        std::map<std::string, uint32_t> counts;
        for(const auto& item : found) for(const auto& t : item.tags) ++counts[t];
        result.tags.assign(counts.begin(), counts.end());
        result.total = uint32_t(found.size());
        if(found.size() > q.limit) found.resize(q.limit);
        result.items = std::move(found);
        return result;
    }

    const Control::LibraryItem* Library::find(const std::string& id) const
    {
        const auto it = std::find_if(items_.begin(), items_.end(), [&](const auto& i) { return i.id == id; });
        return it == items_.end() ? nullptr : &*it;
    }

    void Library::setFavourite(const std::string& id, bool favourite)
    {
        if(!find(id)) throw std::invalid_argument("no such library item");
        std::erase(favourites_, id);
        if(favourite) favourites_.push_back(id);
        writeUser();
    }

    void Library::used(const std::string& id)
    {
        std::erase(recent_, id);
        recent_.insert(recent_.begin(), id);
        if(recent_.size() > maxRecent) recent_.resize(maxRecent);
        writeUser();
    }

    fs::path Library::newFolder(const std::string& name, const std::string& kind) const
    {
        std::string safe;
        for(const char c : name) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
        while(!safe.empty() && safe.back() == ' ') safe.pop_back();
        const bool sound = kind == "sound";
        if(safe.empty()) safe = sound ? "Sound" : "Performance";
        const auto& root = sound ? sounds_ : performances_;
        const std::string extension = sound ? ".osound" : ".oscairn";
        auto folder = root / (safe + extension);
        for(int n = 2; fs::exists(folder); ++n) folder = root / (safe + " " + std::to_string(n) + extension);
        return folder;
    }

    void Library::readUser()
    {
        try
        {
            const auto doc = parse(slurp(cache_ / "user.json"));
            favourites_ = strings(detail::member(doc.get(), "favourites"));
            recent_ = strings(detail::member(doc.get(), "recent"));
        }
        catch(const std::exception&) { favourites_.clear(); recent_.clear(); } // none yet
    }

    void Library::writeUser() const
    {
        writeAtomically(cache_ / "user.json", "{\"favourites\":" + list(favourites_) + ",\"recent\":" + list(recent_) + "}\n");
    }
}

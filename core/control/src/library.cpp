// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/library.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

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

    Control::PerformanceInfo readPerformanceInfo(const fs::path& folder)
    {
        const auto doc = parse(slurp(folder / "performance.json"));
        V d = doc.get();
        if(!d || str(detail::member(d, "format")) != "oscairn.performance") throw std::runtime_error("not a Performance");
        const V version = detail::member(d, "version");
        if(!version || version->type != json_type_number || std::string(static_cast<const json_number_s*>(version->payload)->number, static_cast<const json_number_s*>(version->payload)->number_size) != "3")
            throw std::runtime_error("unsupported Performance version");
        const V info = detail::member(d, "info");
        if(!info || info->type != json_type_object) throw std::runtime_error("a Performance without its info");
        Control::PerformanceInfo out;
        out.id = str(detail::member(info, "id")); out.name = str(detail::member(info, "name"));
        out.description = str(detail::member(info, "description")); out.tags = strings(detail::member(info, "tags"));
        if(out.id.empty() || out.id.size() > 64 || out.name.size() > 256 || out.description.size() > 4096 || out.tags.size() > maxTags ||
           std::any_of(out.tags.begin(), out.tags.end(), [](const std::string& t) { return t.empty() || t.size() > maxTag; }))
            throw std::runtime_error("invalid Performance info");
        return out;
    }

    Library::Library(fs::path home)
    {
        if(home.empty()) home = dataHome();
        performances_ = home / "Performances"; cache_ = home / "library";
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
                    item.path = str(detail::member(e->value, "path")); item.id = str(detail::member(e->value, "id"));
                    item.name = str(detail::member(e->value, "name")); item.description = str(detail::member(e->value, "description"));
                    item.tags = strings(detail::member(e->value, "tags"));
                    const auto modified = std::stoll(str(detail::member(e->value, "modified")));
                    cached[item.path] = {modified, item};
                }
        }
        catch(const std::exception&) { cached.clear(); } // no index, or a damaged one: read every file
        items_.clear();
        std::string index;
        std::error_code error;
        if(fs::is_directory(performances_, error))
            for(const auto& entry : fs::directory_iterator(performances_, error))
            {
                const auto manifest = entry.path() / "performance.json";
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
                        const auto info = readPerformanceInfo(entry.path());
                        item.id = info.id; item.name = info.name; item.description = info.description; item.tags = info.tags; item.path = path;
                    }
                    catch(const std::exception&) { continue; } // not one the library can show; it stays on disk
                }
                if(std::any_of(items_.begin(), items_.end(), [&](const auto& i) { return i.id == item.id; })) continue; // a copied folder: the first wins
                items_.push_back(item);
                index += std::string(index.empty() ? "" : ",") + "{\"path\":" + detail::quote(item.path) + ",\"modified\":" + detail::quote(std::to_string(modified)) +
                         ",\"id\":" + detail::quote(item.id) + ",\"name\":" + detail::quote(item.name) + ",\"description\":" + detail::quote(item.description) + ",\"tags\":" + list(item.tags) + '}';
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

    fs::path Library::newFolder(const std::string& name) const
    {
        std::string safe;
        for(const char c : name) safe += (std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '-' || c == '_') ? c : '_';
        while(!safe.empty() && safe.back() == ' ') safe.pop_back();
        if(safe.empty()) safe = "Performance";
        auto folder = performances_ / (safe + ".oscairn");
        for(int n = 2; fs::exists(folder); ++n) folder = performances_ / (safe + " " + std::to_string(n) + ".oscairn");
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

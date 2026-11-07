// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/library.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "oscairn/assets.h"
#include "oscairn/package.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <fcntl.h>
#include <unistd.h>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        using V = const json_value_s*;
        using Doc = std::unique_ptr<json_value_s, decltype(&std::free)>;
        constexpr size_t maxFile = 4 * 1024 * 1024, maxTags = 32, maxTag = 64, maxRecent = 50;

        std::string slurp(const fs::path& file, size_t limit = maxFile)
        {
            std::ifstream in(file, std::ios::binary);
            if(!in) throw std::runtime_error("cannot read " + file.string());
            std::ostringstream out; out << in.rdbuf();
            auto text = out.str();
            if(text.size() > limit) throw std::runtime_error(file.string() + " is too large");
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
        std::string lower(std::string s) { for(auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
    }

    void writeAtomically(const std::filesystem::path& file, const std::string& text)
    {
        namespace fs = std::filesystem;
        fs::create_directories(file.parent_path());
        const auto temporary = file.parent_path() / ("." + file.filename().string() + ".tmp");
        // On disk before the move, so a crash after it leaves the new file, not an empty one.
        bool ok = false;
        if(const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644); fd >= 0)
        {
            ok = true;
            for(size_t at = 0; ok && at < text.size();)
            {
                const auto n = ::write(fd, text.data() + at, text.size() - at);
                if(n > 0) at += size_t(n);
                else ok = n < 0 && errno == EINTR;
            }
            ok = ok && ::fsync(fd) == 0;
            ok = ::close(fd) == 0 && ok;
        }
        std::error_code error;
        if(ok) fs::rename(temporary, file, error);
        if(!ok || error) { std::error_code ignored; fs::remove(temporary, ignored); throw std::runtime_error("cannot write " + file.string()); }
        // The folder too, so the rename itself survives a power cut (RFC 0037).
        if(const int fd = ::open(file.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); fd >= 0) { ::fsync(fd); ::close(fd); }
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

    bool timeKnown()
    {
        if(const char* state = std::getenv("OSCAIRN_TIME"); state && *state) return std::string_view(state) == "known";
        // shortcut: the floor is this code's date (2026-11-01 UTC), not the image's build time and the
        // time last saved; the supervisor gives both once built (RFC 0035 step 6).
        return std::chrono::system_clock::now() >= std::chrono::system_clock::from_time_t(1793491200);
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

    namespace
    {
        // The top-level info object, by its key, from its opening brace to just past its matching
        // one, outside strings.
        std::pair<size_t, size_t> infoSpan(const std::string& text, const fs::path& file)
        {
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
            return {key + 8, at + 1};
        }
    }

    std::string contentHash(const fs::path& file)
    {
        const auto text = slurp(file);
        const auto [from, to] = infoSpan(text, file);
        return contentHash(text.substr(0, from) + text.substr(to), file.parent_path() / "nodes");
    }

    std::string infoJson(const Control::PerformanceInfo& info)
    {
        const auto& o = info.origin;
        return "{\"id\":" + detail::quote(info.id) + ",\"name\":" + detail::quote(info.name) + ",\"description\":" + detail::quote(info.description) + ",\"tags\":" + list(info.tags) +
               ",\"color\":" + detail::quote(info.color) + ",\"revision\":" + detail::quote(info.revision) + ",\"parent\":" + detail::quote(info.parent) + ",\"hash\":" + detail::quote(info.hash) +
               ",\"origin\":" + (o.library.empty() ? std::string("null") : "{\"library\":" + detail::quote(o.library) + ",\"item\":" + detail::quote(o.item) + ",\"hash\":" + detail::quote(o.hash) + '}') + '}';
    }

    void writeInfo(const fs::path& file, const Control::PerformanceInfo& info)
    {
        auto text = slurp(file);
        const auto [from, to] = infoSpan(text, file);
        writeAtomically(file, text.substr(0, from) + infoJson(info) + text.substr(to));
    }

    Control::PerformanceInfo readInfo(const fs::path& file, const std::string& format)
    {
        const auto doc = parse(slurp(file));
        V d = doc.get();
        if(!d || str(detail::member(d, "format")) != format) throw std::runtime_error("not a " + format);
        const V version = detail::member(d, "version");
        if(!version || version->type != json_type_number || std::string(static_cast<const json_number_s*>(version->payload)->number, static_cast<const json_number_s*>(version->payload)->number_size) != (format == "oscairn.performance" ? "13" : "8"))
            throw std::runtime_error("unsupported version");
        const V info = detail::member(d, "info");
        if(!info || info->type != json_type_object) throw std::runtime_error("no info block");
        Control::PerformanceInfo out;
        out.id = str(detail::member(info, "id")); out.name = str(detail::member(info, "name"));
        out.description = str(detail::member(info, "description")); out.tags = strings(detail::member(info, "tags"));
        out.color = str(detail::member(info, "color"));
        out.revision = str(detail::member(info, "revision")); out.parent = str(detail::member(info, "parent")); out.hash = str(detail::member(info, "hash"));
        if(const V o = detail::member(info, "origin"); !o || o->type != json_type_null)
            out.origin = {str(detail::member(o, "library")), str(detail::member(o, "item")), str(detail::member(o, "hash"))};
        if(out.id.empty() || out.id.size() > 64 || !validColor(out.color) || out.name.size() > 256 || out.description.size() > 4096 || out.tags.size() > maxTags ||
           std::any_of(out.tags.begin(), out.tags.end(), [](const std::string& t) { return t.empty() || t.size() > maxTag; }))
            throw std::runtime_error("invalid info");
        return out;
    }

    Library::Library(fs::path home)
    {
        if(home.empty()) home = dataHome();
        performances_ = home / "Performances"; sounds_ = home / "Sounds"; libraries_ = home / "Libraries"; cache_ = home / "library";
        // An install's or export's unpack folder left by a crash. Only those over a day old: a
        // newer one may be another process's still at work.
        std::error_code error;
        const auto dayAgo = fs::file_time_type::clock::now() - std::chrono::hours(24);
        for(const auto& entry : fs::directory_iterator(home, error))
        {
            const auto name = entry.path().filename().string();
            if(!name.starts_with(".install-") && !name.starts_with(".export-")) continue;
            std::error_code unread;
            const bool folder = fs::is_directory(entry.symlink_status(unread));
            const auto written = entry.last_write_time(unread);
            if(folder && !unread && written < dayAgo) fs::remove_all(entry.path(), error);
        }
        readUser();
        rescan();
    }

    uint32_t Library::rescan()
    {
        // What was read before, by folder and the time its file had.
        std::map<std::string, std::pair<int64_t, Control::LibraryItem>> cached;
        try
        {
            const auto doc = parse(slurp(cache_ / "index.json", maxManifestBytes)); // one row per item, as a manifest
            if(V items = detail::member(doc.get(), "items"); items && items->type == json_type_array)
                for(auto* e = static_cast<const json_array_s*>(items->payload)->start; e; e = e->next)
                {
                    Control::LibraryItem item;
                    item.path = str(detail::member(e->value, "path")); item.id = str(detail::member(e->value, "id")); item.kind = str(detail::member(e->value, "kind"));
                    item.name = str(detail::member(e->value, "name")); item.description = str(detail::member(e->value, "description"));
                    item.tags = strings(detail::member(e->value, "tags")); item.color = str(detail::member(e->value, "color")); item.revision = str(detail::member(e->value, "revision")); item.hash = str(detail::member(e->value, "hash"));
                    item.library = str(detail::member(e->value, "library"));
                    item.origin = {str(detail::member(e->value, "originLibrary")), str(detail::member(e->value, "originItem")), str(detail::member(e->value, "originHash"))};
                    const auto modified = std::stoll(str(detail::member(e->value, "modified")));
                    cached[item.path] = {modified, item};
                }
        }
        catch(const std::exception&) { cached.clear(); } // no index, or a damaged one: read every file
        items_.clear();
        std::string index;
        std::error_code error;
        // The user's own folders, then each installed library's, its items locked.
        std::vector<std::tuple<fs::path, const char*, const char*, std::string>> folders{{performances_, "performance", "performance.json", ""}, {sounds_, "sound", "sound.json", ""}};
        installed_.clear();
        if(fs::is_directory(libraries_, error))
            for(const auto& entry : fs::directory_iterator(libraries_, error))
            {
                if(!entry.is_directory() || entry.path().filename().string().starts_with(".")) continue;
                try
                {
                    const auto doc = parse(slurp(entry.path() / "library.json", maxManifestBytes));
                    Control::InstalledLibrary l{str(detail::member(doc.get(), "id")), str(detail::member(doc.get(), "name")), 0};
                    if(l.id != entry.path().filename().string()) continue; // not where it was installed
                    installed_.push_back(l);
                    folders.push_back({entry.path() / "Performances", "performance", "performance.json", l.id});
                    folders.push_back({entry.path() / "Sounds", "sound", "sound.json", l.id});
                }
                catch(const std::exception&) {} // a damaged library: install it again
            }
        for(const auto& [folder, kind, file, library] : folders)
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
                        item.origin = info.origin;
                    }
                    catch(const std::exception&) { continue; } // not one the library can show; it stays on disk
                }
                item.library = library;
                if(std::any_of(items_.begin(), items_.end(), [&](const auto& i) { return i.id == item.id; })) continue; // a copied folder: the first wins
                items_.push_back(item);
                for(auto& l : installed_) if(l.id == library) ++l.items;
                index += std::string(index.empty() ? "" : ",") + "{\"path\":" + detail::quote(item.path) + ",\"kind\":" + detail::quote(item.kind) + ",\"modified\":" + detail::quote(std::to_string(modified)) +
                         ",\"id\":" + detail::quote(item.id) + ",\"name\":" + detail::quote(item.name) + ",\"description\":" + detail::quote(item.description) + ",\"tags\":" + list(item.tags) + ",\"color\":" + detail::quote(item.color) + ",\"revision\":" + detail::quote(item.revision) + ",\"hash\":" + detail::quote(item.hash) +
                         ",\"library\":" + detail::quote(item.library) + ",\"originLibrary\":" + detail::quote(item.origin.library) + ",\"originItem\":" + detail::quote(item.origin.item) + ",\"originHash\":" + detail::quote(item.origin.hash) + '}';
            }
        try { writeAtomically(cache_ / "index.json", "{\"items\":[" + index + "]}\n"); } catch(const std::exception&) {} // a cache: reading works without it
        // Favorites and recents of items that went stay in the user file, for when they return.
        return uint32_t(items_.size());
    }

    Control::LibraryResult Library::query(const Control::LibraryQuery& q) const
    {
        Control::LibraryResult result;
        const auto text = lower(q.text);
        std::vector<Control::LibraryItem> found;
        for(auto item : items_)
        {
            item.favorite = std::find(favorites_.begin(), favorites_.end(), item.id) != favorites_.end();
            const auto r = std::find(recent_.begin(), recent_.end(), item.id);
            item.recent = r == recent_.end() ? 0 : uint32_t(r - recent_.begin()) + 1;
            // Every chosen tag (AND), the text in its name, tags or description, and the toggles.
            if(!std::all_of(q.tags.begin(), q.tags.end(), [&](const std::string& t) { return std::find(item.tags.begin(), item.tags.end(), t) != item.tags.end(); })) continue;
            if(q.favorites && !item.favorite) continue;
            if(!q.kind.empty() && item.kind != q.kind) continue;
            if(!q.id.empty() && item.id != q.id) continue;
            if(!q.source.empty() && item.library != (q.source == "mine" ? std::string() : q.source)) continue;
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
        for(const auto& item : found) for(const auto& t : item.tags) if(std::find(hiddenTags_.begin(), hiddenTags_.end(), t) == hiddenTags_.end()) ++counts[t];
        result.tags.assign(counts.begin(), counts.end());
        result.tagColors.assign(tagColors_.begin(), tagColors_.end());
        result.hiddenTags = hiddenTags_;
        result.total = uint32_t(found.size());
        if(found.size() > q.limit) found.resize(q.limit);
        result.items = std::move(found);
        return result;
    }

    std::vector<Control::InstalledLibrary> Library::installed() const { return installed_; }

    const Control::LibraryItem* Library::find(const std::string& id) const
    {
        const auto it = std::find_if(items_.begin(), items_.end(), [&](const auto& i) { return i.id == id; });
        return it == items_.end() ? nullptr : &*it;
    }

    void Library::setFavorite(const std::string& id, bool favorite)
    {
        if(!find(id)) throw std::invalid_argument("no such library item");
        std::erase(favorites_, id);
        if(favorite) favorites_.push_back(id);
        writeUser();
    }

    void Library::setTag(const std::string& tag, bool hidden, const std::string& color)
    {
        if(tag.empty() || tag.size() > maxTag || tag.find('\0') != std::string::npos) throw std::invalid_argument("invalid tag");
        if(!validColor(color)) throw std::invalid_argument("a tag's color is #rrggbb, or empty for none");
        std::erase(hiddenTags_, tag);
        if(hidden) hiddenTags_.push_back(tag);
        if(color.empty()) tagColors_.erase(tag); else tagColors_[tag] = color;
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
        // A dangling link counts as taken: an install's cleanup would remove it.
        for(int n = 2; fs::exists(fs::symlink_status(folder)); ++n) folder = root / (safe + " " + std::to_string(n) + extension);
        return folder;
    }

    void Library::readUser()
    {
        try
        {
            const auto doc = parse(slurp(cache_ / "user.json"));
            favorites_ = strings(detail::member(doc.get(), "favorites"));
            recent_ = strings(detail::member(doc.get(), "recent"));
            hiddenTags_ = strings(detail::member(doc.get(), "hiddenTags"));
            tagColors_.clear();
            if(V colors = detail::member(doc.get(), "tagColors"); colors && colors->type == json_type_object)
                for(auto* e = static_cast<const json_object_s*>(colors->payload)->start; e; e = e->next)
                {
                    const std::string tag(e->name->string, e->name->string_size), color = str(e->value);
                    if(validColor(color) && !color.empty()) tagColors_[tag] = color;
                }
        }
        catch(const std::exception&) { favorites_.clear(); recent_.clear(); hiddenTags_.clear(); tagColors_.clear(); } // none yet
    }

    void Library::writeUser() const
    {
        std::string colors;
        for(const auto& [tag, color] : tagColors_) colors += std::string(colors.empty() ? "" : ",") + detail::quote(tag) + ":" + detail::quote(color);
        writeAtomically(cache_ / "user.json", "{\"favorites\":" + list(favorites_) + ",\"recent\":" + list(recent_) + ",\"hiddenTags\":" + list(hiddenTags_) +
                                              ",\"tagColors\":{" + colors + "}}\n");
    }
}

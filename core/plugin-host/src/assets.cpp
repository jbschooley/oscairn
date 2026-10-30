// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/assets.h"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace oscairn::assets
{
    namespace fs = std::filesystem;
    namespace
    {
        // SHA-256 (FIPS 180-4), checked against the standard test vectors in core/plugin-host tests.
        class Sha256
        {
            std::array<uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
            std::array<uint8_t, 64> block{};
            size_t used = 0;
            uint64_t bits = 0;
            static uint32_t rotate(uint32_t x, int n) { return x >> n | x << (32 - n); }
            void compress()
            {
                static constexpr uint32_t k[64]{
                    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
                    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
                    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
                    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
                uint32_t w[64];
                for(int i = 0; i < 16; ++i) w[i] = uint32_t(block[4 * i]) << 24 | uint32_t(block[4 * i + 1]) << 16 | uint32_t(block[4 * i + 2]) << 8 | block[4 * i + 3];
                for(int i = 16; i < 64; ++i)
                {
                    const auto s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }
                auto [a, b, c, d, e, f, g, x] = h;
                for(int i = 0; i < 64; ++i)
                {
                    const auto t1 = x + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
                    const auto t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
                    x = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
                }
                h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += x;
            }
        public:
            void add(const void* data, size_t n)
            {
                auto* p = static_cast<const uint8_t*>(data); bits += uint64_t(n) * 8;
                while(n) { const auto take = std::min(n, 64 - used); std::memcpy(block.data() + used, p, take); used += take; p += take; n -= take; if(used == 64) { compress(); used = 0; } }
            }
            std::string hex()
            {
                const auto length = bits; const uint8_t one = 0x80, zero = 0;
                add(&one, 1); while(used != 56) add(&zero, 1);
                for(int i = 7; i >= 0; --i) { const uint8_t byte = uint8_t(length >> (8 * i)); add(&byte, 1); }
                std::string out; constexpr char digits[] = "0123456789abcdef";
                for(auto word : h) for(int i = 28; i >= 0; i -= 4) out += digits[(word >> i) & 15];
                return out;
            }
        };
        [[noreturn]] void fail(const std::string& why) { throw std::runtime_error(why); }
        bool plain(const std::string& s) { return s.find_first_of("\t\n\r") == std::string::npos && s.find('\0') == std::string::npos; }
        // A library-relative path: forward slashes, no empty, "." or ".." parts, not absolute.
        void checkRelative(const std::string& path)
        {
            if(path.empty() || path.size() > 1024 || !plain(path) || path.front() == '/' || path.find('\\') != std::string::npos) fail("invalid asset path: " + path);
            for(size_t at = 0; at <= path.size();)
            {
                const auto end = std::min(path.find('/', at), path.size()); const auto part = path.substr(at, end - at);
                if(part.empty() || part == "." || part == "..") fail("invalid asset path: " + path);
                at = end + 1;
            }
        }
        int64_t modified(const fs::path& file) { return int64_t(fs::last_write_time(file).time_since_epoch().count()); }
        fs::path defaultRoot()
        {
            if(const char* home = std::getenv("OSCAIRN_DATA_HOME"); home && *home) return fs::path(home) / "assets";
            const char* user = std::getenv("HOME");
            if(!user || !*user) fail("cannot find the user's home directory for sound assets");
#ifdef __APPLE__
            return fs::path(user) / "Library/Application Support/Oscairn/assets";
#else
            if(const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return fs::path(xdg) / "oscairn/assets";
            return fs::path(user) / ".local/share/oscairn/assets";
#endif
        }
        void writeAtomically(const fs::path& file, const std::string& text)
        {
            fs::create_directories(file.parent_path());
            const auto temporary = file.parent_path() / ("." + file.filename().string() + ".tmp");
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << text; out.close(); if(!out) fail("cannot write " + temporary.string()); }
            fs::rename(temporary, file);
        }
        // Engines in several processes may share one store: registry changes re-read the file
        // under this exclusive lock so one process never drops another's registration.
        struct RegistryLock
        {
            int fd;
            explicit RegistryLock(const fs::path& root)
            {
                fs::create_directories(root);
                fd = ::open((root / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
                if(fd < 0) fail("cannot lock the sound asset registry in " + root.string());
                while(::flock(fd, LOCK_EX) < 0) if(errno != EINTR) { ::close(fd); fail("cannot lock the sound asset registry"); }
            }
            ~RegistryLock() { ::close(fd); }
        };
        std::string randomId()
        {
            std::random_device device; std::string id; constexpr char digits[] = "0123456789abcdef";
            for(int i = 0; i < 16; ++i) id += digits[device() & 15];
            return id;
        }
    }

    std::string sha256(std::string_view bytes) { Sha256 s; s.add(bytes.data(), bytes.size()); return s.hex(); }
    std::string sha256File(const fs::path& file)
    {
        std::ifstream in(file, std::ios::binary); if(!in) fail("cannot read " + file.string());
        Sha256 s; std::vector<char> buffer(1 << 20);
        while(in) { in.read(buffer.data(), std::streamsize(buffer.size())); s.add(buffer.data(), size_t(in.gcount())); }
        if(in.bad()) fail("cannot read " + file.string());
        return s.hex();
    }

    Store::Store(fs::path root) : root_(std::move(root)) {}
    // The default location is found on first use, so hosts without a home directory still start.
    const fs::path& Store::root() const { if(root_.empty()) root_ = defaultRoot(); return root_; }

    // libraries.tsv: one line per library, "id<TAB>fingerprint<TAB>path<TAB>name".
    void Store::load() const
    {
        if(loaded_) return;
        loaded_ = true; libraries_.clear();
        std::ifstream in(root() / "libraries.tsv"); std::string line;
        while(std::getline(in, line))
        {
            std::istringstream fields(line); Library l;
            if(std::getline(fields, l.id, '\t') && std::getline(fields, l.fingerprint, '\t') && std::getline(fields, l.path, '\t') && std::getline(fields, l.name)) libraries_[l.id] = l;
        }
    }
    void Store::save() const
    {
        std::string text;
        for(const auto& [id, l] : libraries_) text += l.id + '\t' + l.fingerprint + '\t' + l.path + '\t' + l.name + '\n';
        writeAtomically(root() / "libraries.tsv", text);
    }
    // index/<id>.tsv: "size<TAB>modified<TAB>sha256<TAB>relative path" per file.
    Store::Index& Store::index(const std::string& id) const
    {
        if(auto found = indexes_.find(id); found != indexes_.end()) return found->second;
        auto& result = indexes_[id];
        std::ifstream in(root() / "index" / (id + ".tsv")); std::string line;
        while(std::getline(in, line))
        {
            std::istringstream fields(line); Entry e; std::string path; char tab;
            if(fields >> e.size >> e.modified && fields.get(tab) && std::getline(fields, e.sha256, '\t') && std::getline(fields, path)) result[path] = e;
        }
        return result;
    }
    void Store::saveIndex(const std::string& id) const
    {
        std::string text;
        for(const auto& [path, e] : index(id)) text += std::to_string(e.size) + '\t' + std::to_string(e.modified) + '\t' + e.sha256 + '\t' + path + '\n';
        writeAtomically(root() / "index" / (id + ".tsv"), text);
    }
    // The file's hash, from the index while its size and modification time are unchanged.
    std::string Store::hashOf(const std::string& id, const fs::path& base, const std::string& relative) const
    {
        const auto file = base / relative; auto& entries = index(id);
        const auto size = fs::file_size(file); const auto time = modified(file);
        if(auto found = entries.find(relative); found != entries.end() && found->second.size == size && found->second.modified == time) return found->second.sha256;
        entries[relative] = {size, time, sha256File(file)}; saveIndex(id);
        return entries[relative].sha256;
    }
    Library& Store::user() const
    {
        RegistryLock registry(root()); loaded_ = false; load();
        auto& l = libraries_["user"];
        if(l.id.empty()) { l = {"user", "User library", (root() / "user").string(), "", 0}; fs::create_directories(root() / "user"); save(); }
        return l;
    }

    std::string Store::registerFolder(const fs::path& folder)
    {
        std::lock_guard lock(mutex_); RegistryLock registry(root()); loaded_ = false; load();
        std::error_code ec; const auto path = fs::canonical(folder, ec);
        if(ec || !fs::is_directory(path)) fail("folder not found: " + folder.string());
        if(!plain(path.string())) fail("folder path contains tabs or line breaks: " + path.string());
        std::string id;
        for(const auto& [existing, l] : libraries_) if(l.path == path.string()) id = existing;
        if(id.empty()) id = randomId();
        // Hash every regular file now so later loads only check sizes and times. Links are skipped.
        auto& entries = index(id); Index fresh; size_t files = 0;
        for(auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied); it != fs::recursive_directory_iterator(); ++it)
        {
            if(it->is_symlink()) { if(it->is_directory()) it.disable_recursion_pending(); continue; }
            if(!it->is_regular_file()) continue;
            if(++files > 200000) fail("folder has more than 200,000 files: " + path.string());
            const auto relative = fs::relative(it->path(), path).generic_string();
            if(!plain(relative)) continue;
            const auto size = it->file_size(); const auto time = modified(it->path());
            const auto old = entries.find(relative);
            fresh[relative] = old != entries.end() && old->second.size == size && old->second.modified == time ? old->second : Entry{size, time, sha256File(it->path())};
        }
        entries = std::move(fresh); saveIndex(id);
        std::vector<std::string> hashes; for(const auto& [p, e] : entries) hashes.push_back(e.sha256);
        std::sort(hashes.begin(), hashes.end()); std::string all; for(const auto& h : hashes) all += h;
        libraries_[id] = {id, path.filename().string(), path.string(), sha256(all), entries.size()};
        save();
        return id;
    }

    Ref Store::import(const fs::path& file)
    {
        std::lock_guard lock(mutex_);
        std::error_code ec;
        if(!fs::is_regular_file(file, ec)) fail("file not found: " + file.string());
        const auto name = file.filename().string(); if(!plain(name) || name.size() > 255) fail("unsupported file name: " + name);
        const auto hash = sha256File(file); auto& library = user();
        const auto relative = hash.substr(0, 16) + "/" + name;
        const auto target = fs::path(library.path) / relative;
        if(!fs::exists(target)) { fs::create_directories(target.parent_path()); fs::copy_file(file, target); }
        if(hashOf("user", library.path, relative) != hash) fail("imported copy does not match: " + name);
        library.files = index("user").size();
        return {"user", relative, hash};
    }

    std::vector<Library> Store::libraries() const
    {
        std::lock_guard lock(mutex_); loaded_ = false; load();
        std::vector<Library> result;
        for(const auto& [id, l] : libraries_) { auto copy = l; copy.files = index(id).size(); result.push_back(copy); }
        return result;
    }

    bool Store::locate(const fs::path& directory, std::string& library, std::string& relative) const
    {
        std::lock_guard lock(mutex_); loaded_ = false; load();
        std::error_code ec; const auto path = fs::canonical(directory, ec); if(ec) return false;
        for(const auto& [id, l] : libraries_)
        {
            const fs::path base(l.path);
            const auto [end, at] = std::mismatch(base.begin(), base.end(), path.begin(), path.end());
            if(end != base.end()) continue;
            library = id; relative = fs::relative(path, base).generic_string(); if(relative == ".") relative.clear();
            return true;
        }
        return false;
    }

    fs::path Store::resolve(Ref& ref) const
    {
        std::lock_guard lock(mutex_); load();
        checkRelative(ref.path);
        if(!libraries_.count(ref.library)) { loaded_ = false; load(); } // registered by another process since
        const auto found = libraries_.find(ref.library);
        if(found != libraries_.end())
        {
            const fs::path base(found->second.path); std::error_code ec;
            const auto file = fs::canonical(base / ref.path, ec);
            if(!ec)
            {
                const auto [end, at] = std::mismatch(base.begin(), base.end(), file.begin(), file.end());
                if(end != base.end()) fail(ref.path + " is outside library " + found->second.name);
                if(!fs::is_regular_file(file)) fail(ref.path + " is not a file");
                const auto hash = hashOf(ref.library, base, ref.path);
                if(ref.sha256.empty() || ref.sha256 == hash) { ref.sha256 = hash; return file; }
            }
        }
        // Identical bytes anywhere satisfy a reference with a known hash (RFC 0009 decisions).
        if(!ref.sha256.empty())
            for(const auto& [id, l] : libraries_)
                for(const auto& [path, e] : index(id))
                {
                    if(e.sha256 != ref.sha256) continue;
                    std::error_code ec; const auto file = fs::path(l.path) / path;
                    if(fs::is_regular_file(file, ec) && hashOf(id, l.path, path) == ref.sha256) return file;
                }
        if(found == libraries_.end()) fail(ref.path + " not found: library " + ref.library + " is not registered");
        std::error_code ec;
        if(fs::exists(fs::path(found->second.path) / ref.path, ec)) fail(ref.path + " changed since it was saved (library " + found->second.name + ")");
        fail(ref.path + " not found in library " + found->second.name + " (" + found->second.path + ")");
    }
    std::string Store::resolve(const std::string& library, const std::string& path, std::string& hash)
    {
        Ref ref{library, path, hash}; const auto file = resolve(ref); hash = ref.sha256; return file.string();
    }
}

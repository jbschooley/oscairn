// SPDX-License-Identifier: AGPL-3.0-or-later
// The libraries a plugin build loads (package.h, outsideLibraries): Mach-O load commands and
// rpaths, ELF DT_NEEDED and RPATH/RUNPATH, read from the files themselves, so a macOS build is
// checked on Linux CI too. Every byte is untrusted: each read is bounds-checked, counts are
// capped, and a file that does not parse is said to, never trusted.
#include "oscairn/package.h"
#include <algorithm>
#include <fstream>
#include <set>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr uint64_t maxBinary = 2ull << 30; // an archive's largest entry

        struct Bytes
        {
            std::string data;
            bool has(uint64_t at, uint64_t n) const { return at <= data.size() && n <= data.size() - at; }
            uint64_t get(uint64_t at, unsigned n, bool big) const
            {
                if(!has(at, n)) throw std::runtime_error("truncated");
                uint64_t v = 0;
                for(unsigned i = 0; i < n; ++i) v |= uint64_t(uint8_t(data[at + (big ? n - 1 - i : i)])) << (8 * i);
                return v;
            }
            // A NUL-terminated string starting at `at`, ending before `end`.
            std::string text(uint64_t at, uint64_t end) const
            {
                end = std::min<uint64_t>(end, data.size());
                if(at >= end) throw std::runtime_error("a name outside its command");
                const auto stop = data.find('\0', at);
                if(stop == std::string::npos || stop >= end) throw std::runtime_error("a name without its end");
                return data.substr(at, stop - at);
            }
        };

        // A path relative to the build's root, lexically, or empty when it climbs out.
        std::string inside(const fs::path& relative)
        {
            const auto normal = relative.lexically_normal().generic_string();
            if(normal.empty() || normal == "." || normal == ".." || normal.rfind("../", 0) == 0 || normal.front() == '/') return {};
            return normal;
        }

        struct Checker
        {
            fs::path root;      // the build: a bundle folder, or a single file
            bool folder;
            std::vector<std::string> problems;
            void problem(const std::string& file, const std::string& what) { problems.push_back(file + ": " + what); }
            // A file in the build named relative to `from` (a folder in it, relative to the root).
            bool present(const std::string& from, const std::string& rest) const
            {
                if(!folder) return false; // a lone file carries nothing beside it
                const auto relative = inside(fs::path(from) / rest);
                std::error_code e;
                return !relative.empty() && fs::is_regular_file(root / relative, e);
            }

            // A sanitizer build (tests, never a release) links its runtime into every binary it makes, so
            // a check run by such a build accepts that runtime too; a normal build still refuses it.
#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer) || __has_feature(undefined_behavior_sanitizer)
#define OSCAIRN_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define OSCAIRN_SANITIZED 1
#endif
            static bool sanitizerRuntime([[maybe_unused]] const std::string& library)
            {
#ifdef OSCAIRN_SANITIZED
                const auto base = library.substr(library.find_last_of('/') + 1);
                for(const char* prefix : {"libtsan.so", "libasan.so", "libubsan.so", "liblsan.so", "libclang_rt."})
                    if(base.rfind(prefix, 0) == 0) return true;
#endif
                return false;
            }
            // The rpaths a sanitizer build adds on macOS so its runtime is found: the toolchain's clang folder.
            static bool sanitizerRpath([[maybe_unused]] const std::string& rpath)
            {
#ifdef OSCAIRN_SANITIZED
                return rpath == "@executable_path" || rpath.find("/lib/clang/") != std::string::npos;
#else
                return false;
#endif
            }
            // macOS: system libraries, or the bundle's own through @loader_path or @rpath.
            static bool systemDarwin(const std::string& p) { return p.rfind("/usr/lib/", 0) == 0 || p.rfind("/System/", 0) == 0; }
            void machO(const Bytes& b, uint64_t start, uint64_t size, const std::string& file, const std::string& from)
            {
                const uint64_t end = start + size;
                const auto magic = uint32_t(b.get(start, 4, false));
                const bool big = magic == 0xcefaedfe || magic == 0xcffaedfe, wide = magic == 0xfeedfacf || magic == 0xcffaedfe;
                if(!big && !wide && magic != 0xfeedface) throw std::runtime_error("not a Mach-O slice");
                const auto commands = b.get(start + 16, 4, big), commandBytes = b.get(start + 20, 4, big);
                const uint64_t first = start + (wide ? 32 : 28);
                if(commands > 65536 || commandBytes > size || first + commandBytes > end) throw std::runtime_error("load commands outside the file");
                std::vector<std::string> libraries, rpaths;
                uint64_t at = first;
                for(uint64_t i = 0; i < commands; ++i)
                {
                    const auto cmd = b.get(at, 4, big), length = b.get(at + 4, 4, big);
                    if(length < 8 || at + length > first + commandBytes) throw std::runtime_error("a load command outside its table");
                    const bool dylib = cmd == 0xc || cmd == 0x80000018 || cmd == 0x8000001f || cmd == 0x20 || cmd == 0x80000023; // load, weak, reexport, lazy, upward
                    if(dylib || cmd == 0x8000001c) // LC_RPATH
                    {
                        const auto name = b.get(at + 8, 4, big);
                        if(name < (dylib ? 24u : 12u) || name >= length) throw std::runtime_error("a load command's name outside it");
                        (dylib ? libraries : rpaths).push_back(b.text(at + name, at + length));
                    }
                    at += length;
                }
                // Rpaths: a system folder, or one in the bundle through @loader_path; a host's folder or any other is outside.
                std::vector<std::string> ownRpaths;
                bool systemRpath = false;
                for(const auto& r : rpaths)
                {
                    if(systemDarwin(r)) systemRpath = true;
                    else if(r.rfind("@loader_path", 0) == 0) ownRpaths.push_back(r.substr(12));
                    else if(sanitizerRpath(r)) continue;
                    else problem(file, "rpath " + r + " (outside the bundle and the system)");
                }
                for(const auto& l : libraries)
                {
                    if(systemDarwin(l) || sanitizerRuntime(l)) continue;
                    if(l.rfind("@loader_path/", 0) == 0) { if(!present(from, l.substr(13))) problem(file, l + " (not in the bundle)"); continue; }
                    if(l.rfind("@rpath/", 0) == 0)
                    {
                        const auto rest = l.substr(7);
                        const bool found = systemRpath || std::any_of(ownRpaths.begin(), ownRpaths.end(), [&](const std::string& r) { return present(from, (r.empty() ? std::string() : r.substr(1) + "/") + rest); });
                        if(!found) problem(file, l + " (not in the bundle or the system)");
                        continue;
                    }
                    // @executable_path is the host's folder, never the plugin's bundle.
                    problem(file, l + " (outside the bundle and the system)");
                }
            }
            void fat(const Bytes& b, const std::string& file, const std::string& from)
            {
                const bool wide = b.get(0, 4, true) == 0xcafebabf;
                const auto count = b.get(4, 4, true);
                const unsigned entry = wide ? 32 : 20;
                for(uint64_t i = 0; i < count; ++i)
                {
                    const uint64_t at = 8 + i * entry;
                    const auto offset = wide ? b.get(at + 8, 8, true) : b.get(at + 8, 4, true), size = wide ? b.get(at + 16, 8, true) : b.get(at + 12, 4, true);
                    if(!b.has(offset, size)) throw std::runtime_error("a slice outside the file");
                    machO(b, offset, size, file, from);
                }
            }

            // Linux: what every Oscairn image carries, or the bundle's own through $ORIGIN.
            void elf(const Bytes& b, const std::string& file, const std::string& from)
            {
                const auto kind = uint8_t(b.data.at(4)), order = uint8_t(b.data.at(5));
                if((kind != 1 && kind != 2) || (order != 1 && order != 2)) throw std::runtime_error("not a 32- or 64-bit ELF file");
                const bool wide = kind == 2, big = order == 2;
                const unsigned word = wide ? 8 : 4;
                const auto phoff = b.get(wide ? 32 : 28, word, big), phsize = b.get(wide ? 54 : 42, 2, big), phnum = b.get(wide ? 56 : 44, 2, big);
                if(phnum && phsize < (wide ? 56u : 32u)) throw std::runtime_error("program headers too small");
                struct Segment { uint64_t type, offset, address, size; };
                std::vector<Segment> segments;
                for(uint64_t i = 0; i < phnum; ++i)
                {
                    const auto at = phoff + i * phsize;
                    if(phoff > b.data.size() || i * phsize > b.data.size()) throw std::runtime_error("program headers outside the file");
                    segments.push_back(wide ? Segment{b.get(at, 4, big), b.get(at + 8, 8, big), b.get(at + 16, 8, big), b.get(at + 32, 8, big)}
                                            : Segment{b.get(at, 4, big), b.get(at + 4, 4, big), b.get(at + 8, 4, big), b.get(at + 16, 4, big)});
                }
                const auto dynamic = std::find_if(segments.begin(), segments.end(), [](const Segment& s) { return s.type == 2; });
                if(dynamic == segments.end()) return; // linked statically: it loads nothing
                if(!b.has(dynamic->offset, dynamic->size)) throw std::runtime_error("the dynamic section outside the file");
                std::vector<uint64_t> needed, paths;
                uint64_t table = 0, tableSize = 0;
                bool haveTable = false;
                for(uint64_t at = dynamic->offset; at + 2 * word <= dynamic->offset + dynamic->size; at += 2 * word)
                {
                    const auto tag = b.get(at, word, big), value = b.get(at + word, word, big);
                    if(tag == 0) break;
                    if(tag == 1) needed.push_back(value);
                    else if(tag == 15 || tag == 29) paths.push_back(value); // RPATH, RUNPATH
                    else if(tag == 5) { table = value; haveTable = true; }
                    else if(tag == 10) tableSize = value;
                    if(needed.size() + paths.size() > 4096) throw std::runtime_error("too many dynamic entries");
                }
                if(needed.empty() && paths.empty()) return;
                if(!haveTable) throw std::runtime_error("no string table");
                // The string table's address, as a place in the file.
                uint64_t start = 0;
                bool mapped = false;
                for(const auto& s : segments)
                    if(s.type == 1 && table >= s.address && table - s.address < s.size) { start = s.offset + (table - s.address); mapped = true; break; }
                if(!mapped || !b.has(start, 0)) throw std::runtime_error("the string table outside the file");
                const uint64_t end = tableSize && tableSize <= b.data.size() - start ? start + tableSize : b.data.size();
                auto name = [&](uint64_t offset) { if(offset >= end - start) throw std::runtime_error("a name outside the string table"); return b.text(start + offset, end); };
                std::vector<std::string> own; // $ORIGIN folders, relative to the root
                for(const auto offset : paths)
                {
                    const auto list = name(offset);
                    for(size_t at = 0; at <= list.size();)
                    {
                        const auto stop = std::min(list.find(':', at), list.size());
                        const auto entry = list.substr(at, stop - at);
                        at = stop + 1;
                        if(entry.empty()) continue;
                        if(entry.rfind("$ORIGIN", 0) == 0) own.push_back(entry.substr(7));
                        else if(entry.rfind("${ORIGIN}", 0) == 0) own.push_back(entry.substr(9));
                        else problem(file, "rpath " + entry + " (outside the bundle and the system)");
                    }
                }
                static const std::set<std::string> system{
                    "libc.so.6", "libm.so.6", "libdl.so.2", "libpthread.so.0", "librt.so.1", "libgcc_s.so.1", "libstdc++.so.6",
                    "ld-linux-aarch64.so.1", "ld-linux-x86-64.so.2", "libasound.so.2", "libfreetype.so.6", "libfontconfig.so.1",
                    "libX11.so.6", "libXext.so.6", "libXrandr.so.2", "libXinerama.so.1", "libXcursor.so.1", "libXrender.so.1"};
                for(const auto offset : needed)
                {
                    const auto library = name(offset);
                    if(library.find('/') == std::string::npos && (system.count(library) || sanitizerRuntime(library))) continue;
                    const bool found = library.find('/') == std::string::npos &&
                                       std::any_of(own.begin(), own.end(), [&](const std::string& r) { return present(from, (r.empty() ? std::string() : r.substr(1) + "/") + library); });
                    if(!found) problem(file, library + " (not in the bundle or the system's libraries)");
                }
            }

            void check(const fs::path& path)
            {
                const auto file = folder ? fs::relative(path, root).generic_string() : path.filename().string();
                const auto from = folder ? fs::path(file).parent_path().generic_string() : std::string();
                std::error_code e;
                const auto size = fs::file_size(path, e);
                if(e || size < 8) return;
                std::ifstream in(path, std::ios::binary);
                char head[4]{};
                if(!in.read(head, 4)) return;
                const auto magic = Bytes{std::string(head, 4)}.get(0, 4, true);
                const bool isElf = magic == 0x7f454c46, isFat = magic == 0xcafebabe || magic == 0xcafebabf,
                           isThin = magic == 0xfeedface || magic == 0xfeedfacf || magic == 0xcefaedfe || magic == 0xcffaedfe;
                if(!isElf && !isFat && !isThin) return; // not code this checks (resources, scripts, Windows DLLs)
                if(size > maxBinary) { problem(file, "too large to check"); return; }
                Bytes b;
                b.data.resize(size_t(size));
                in.seekg(0);
                if(!in.read(b.data.data(), std::streamsize(size))) { problem(file, "cannot be read"); return; }
                try
                {
                    // 0xcafebabe is a Java class file's magic too: a fat header holds a few slices.
                    if(isFat && b.get(4, 4, true) > 64) return;
                    if(isElf) elf(b, file, from);
                    else if(isFat) fat(b, file, from);
                    else machO(b, 0, b.data.size(), file, from);
                }
                catch(const std::exception& x) { problem(file, std::string("cannot be read as a library: ") + x.what()); }
            }
        };
    }

    std::vector<std::string> outsideLibraries(const fs::path& build)
    {
        Checker c{build, fs::is_directory(build), {}};
        if(!c.folder) { c.check(build); return c.problems; }
        std::vector<fs::path> files;
        for(const auto& e : fs::recursive_directory_iterator(build))
            if(e.is_regular_file() && !e.is_symlink()) files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for(const auto& f : files) c.check(f);
        return c.problems;
    }
}

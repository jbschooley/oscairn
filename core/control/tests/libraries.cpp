// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A plugin build must not load libraries from outside the system and its own bundle
// (package.h, outsideLibraries). Tiny Mach-O and ELF files made here, thin and fat: one linking
// a Homebrew library is refused, one linking only the system's or its own bundle's passes; the
// parser survives every truncation and thousands of random corruptions. oscairn-sign
// plugin-package refuses such a build unless told to pack it anyway, and says so. Every plugin
// named on the command line (Oscairn's own) passes.
//   oscairn-libraries-test OSCAIRN-SIGN [PLUGIN...]
#include "oscairn/package.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    void put(std::string& b, size_t at, uint64_t v, unsigned n, bool big = false) { if(b.size() < at + n) b.resize(at + n); for(unsigned i = 0; i < n; ++i) b[at + (big ? n - 1 - i : i)] = char(v >> (8 * i)); }
    void write(const fs::path& f, const std::string& bytes) { fs::create_directories(f.parent_path()); std::ofstream(f, std::ios::binary | std::ios::trunc) << bytes; }
    std::string join(const std::vector<std::string>& v) { std::string out; for(const auto& s : v) out += "\n  " + s; return out; }
    std::string shell(const fs::path& p) { std::string out = "'"; for(char c : p.string()) out += c == '\'' ? std::string("'\\''") : std::string(1, c); return out + "'"; }
    std::pair<int, std::string> run(const std::string& command)
    {
        std::string out; FILE* p = ::popen((command + " 2>&1").c_str(), "r"); if(!p) return {-1, out};
        char buffer[4096]; while(const auto n = std::fread(buffer, 1, sizeof buffer, p)) out.append(buffer, n);
        return {::pclose(p), out};
    }

    // A 64-bit little-endian Mach-O bundle: a load command per library (cmd, name), and rpaths.
    std::string machO(const std::vector<std::pair<uint32_t, std::string>>& commands)
    {
        std::string b, table;
        for(const auto& [cmd, name] : commands)
        {
            const uint32_t nameAt = cmd == 0x8000001c ? 12 : 24;
            const uint32_t size = uint32_t((nameAt + name.size() + 1 + 7) & ~size_t(7));
            std::string c(size, '\0');
            put(c, 0, cmd, 4); put(c, 4, size, 4); put(c, 8, nameAt, 4);
            c.replace(nameAt, name.size(), name);
            table += c;
        }
        put(b, 0, 0xfeedfacf, 4); put(b, 4, 0x0100000c, 4); put(b, 12, 8, 4); // arm64, MH_BUNDLE
        put(b, 16, commands.size(), 4); put(b, 20, table.size(), 4); put(b, 28, 0, 4);
        return b + table;
    }
    // A fat file holding the slices.
    std::string fat(const std::vector<std::string>& slices)
    {
        std::string b;
        put(b, 0, 0xcafebabe, 4, true); put(b, 4, slices.size(), 4, true);
        uint64_t at = 4096;
        std::string body;
        for(size_t i = 0; i < slices.size(); ++i)
        {
            put(b, 8 + i * 20 + 8, at + body.size(), 4, true); put(b, 8 + i * 20 + 12, slices[i].size(), 4, true);
            body += slices[i] + std::string((4096 - slices[i].size() % 4096) % 4096, '\0');
        }
        b.resize(at);
        return b + body;
    }
    // A 64-bit little-endian ELF shared object: one PT_LOAD over the whole file and a PT_DYNAMIC
    // with DT_NEEDED for each library, DT_RUNPATH when given, and the string table.
    std::string elf(const std::vector<std::string>& needed, const std::string& runpath = {})
    {
        std::string strings(1, '\0');
        std::vector<std::pair<uint64_t, uint64_t>> dynamic;
        for(const auto& n : needed) { dynamic.push_back({1, strings.size()}); strings += n + '\0'; }
        if(!runpath.empty()) { dynamic.push_back({29, strings.size()}); strings += runpath + '\0'; }
        const uint64_t phoff = 64, dynAt = phoff + 2 * 56, strAt = dynAt + (dynamic.size() + 3) * 16; // and DT_STRTAB, DT_STRSZ, DT_NULL
        dynamic.push_back({5, strAt}); dynamic.push_back({10, strings.size()});
        std::string b;
        b += "\x7f" "ELF"; put(b, 4, 2, 1); put(b, 5, 1, 1); put(b, 6, 1, 1);
        put(b, 16, 3, 2); put(b, 18, 183, 2); put(b, 32, phoff, 8); put(b, 52, 64, 2); put(b, 54, 56, 2); put(b, 56, 2, 2);
        const uint64_t total = strAt + strings.size();
        put(b, phoff, 1, 4); put(b, phoff + 8, 0, 8); put(b, phoff + 16, 0, 8); put(b, phoff + 32, total, 8); put(b, phoff + 40, total, 8);
        put(b, phoff + 56, 2, 4); put(b, phoff + 56 + 8, dynAt, 8); put(b, phoff + 56 + 16, dynAt, 8); put(b, phoff + 56 + 32, (dynamic.size() + 1) * 16, 8);
        b.resize(dynAt);
        for(const auto& [tag, value] : dynamic) { put(b, b.size(), tag, 8); put(b, b.size(), value, 8); }
        put(b, b.size(), 0, 8); put(b, b.size(), 0, 8);
        return b + strings;
    }
}

int main(int argc, char** argv)
{
    if(argc < 2) { std::fprintf(stderr, "usage: %s OSCAIRN-SIGN [PLUGIN...]\n", argv[0]); return 2; }
    const std::string tool = shell(argv[1]);
    std::random_device random;
    const auto root = fs::temp_directory_path() / ("oscairn-libraries-" + std::to_string(random()));
    fs::create_directories(root);
    constexpr uint32_t load = 0xc, weak = 0x80000018, reexport = 0x8000001f, upward = 0x80000023, rpath = 0x8000001c;
    try
    {
        // macOS: the system's libraries pass; Homebrew's, /usr/local's and other absolute paths do not, whatever the load command.
        const auto system = machO({{load, "/usr/lib/libSystem.B.dylib"}, {load, "/System/Library/Frameworks/CoreFoundation.framework/Versions/A/CoreFoundation"}, {weak, "/usr/lib/libc++.1.dylib"}});
        write(root / "system.clap", system);
        check(outsideLibraries(root / "system.clap").empty(), "a bundle linking only the system passes:" + join(outsideLibraries(root / "system.clap")));
        for(const auto& [cmd, path] : std::vector<std::pair<uint32_t, std::string>>{{load, "/opt/homebrew/lib/libfftw3.3.dylib"}, {weak, "/usr/local/lib/libsndfile.1.dylib"},
                                                                                  {reexport, "/Users/me/lib/libx.dylib"}, {upward, "@executable_path/../Frameworks/y.dylib"}, {load, "libbare.dylib"}})
        {
            write(root / "brew.clap", machO({{load, "/usr/lib/libSystem.B.dylib"}, {cmd, path}}));
            const auto found = outsideLibraries(root / "brew.clap");
            check(found.size() == 1 && found[0].find(path) != std::string::npos, "refused: " + path + join(found));
        }
        write(root / "rpath.clap", machO({{load, "/usr/lib/libSystem.B.dylib"}, {rpath, "/opt/homebrew/lib"}}));
        check(outsideLibraries(root / "rpath.clap").size() == 1, "a Homebrew rpath is refused");
        // Fat: each slice is checked; a Homebrew library in either is found.
        write(root / "fat.clap", fat({system, machO({{load, "/opt/homebrew/lib/libfoo.dylib"}})}));
        const auto inFat = outsideLibraries(root / "fat.clap");
        check(inFat.size() == 1 && inFat[0].find("/opt/homebrew/lib/libfoo.dylib") != std::string::npos, "a fat binary's slices are each checked:" + join(inFat));
        // A bundle folder: its own library through @loader_path and @rpath passes; one it does not carry is refused.
        const auto bundle = root / "Own.clap";
        write(bundle / "Contents/MacOS/Own", machO({{load, "/usr/lib/libSystem.B.dylib"}, {load, "@loader_path/../Frameworks/libown.dylib"}, {load, "@rpath/libmore.dylib"}, {rpath, "@loader_path/../Frameworks"}}));
        write(bundle / "Contents/Frameworks/libown.dylib", machO({{load, "/usr/lib/libSystem.B.dylib"}}));
        write(bundle / "Contents/Frameworks/libmore.dylib", machO({{load, "/usr/lib/libSystem.B.dylib"}}));
        write(bundle / "Contents/Info.plist", "<plist/>");
        check(outsideLibraries(bundle).empty(), "a bundle's own libraries through @loader_path and @rpath pass:" + join(outsideLibraries(bundle)));
        fs::remove(bundle / "Contents/Frameworks/libmore.dylib");
        check(outsideLibraries(bundle).size() == 1, "an @rpath library the bundle does not carry is refused");
        write(root / "climb.clap", machO({{load, "@loader_path/../../outside.dylib"}}));
        check(outsideLibraries(root / "climb.clap").size() == 1, "@loader_path out of a lone file is refused");

        // Linux: the image's libraries pass; others, and absolute rpaths, do not.
        write(root / "ok.so.clap", elf({"libc.so.6", "libm.so.6", "libstdc++.so.6", "libgcc_s.so.1", "libasound.so.2"}));
        check(outsideLibraries(root / "ok.so.clap").empty(), "an ELF linking only the image's libraries passes:" + join(outsideLibraries(root / "ok.so.clap")));
        write(root / "need.so.clap", elf({"libc.so.6", "libfftw3f.so.3"}));
        const auto needs = outsideLibraries(root / "need.so.clap");
        check(needs.size() == 1 && needs[0].find("libfftw3f.so.3") != std::string::npos, "a library not on the image is refused:" + join(needs));
        write(root / "run.so.clap", elf({"libc.so.6"}, "/home/linuxbrew/.linuxbrew/lib:$ORIGIN"));
        check(outsideLibraries(root / "run.so.clap").size() == 1, "a Linuxbrew RUNPATH is refused");
        write(root / "path.so.clap", elf({"/usr/local/lib/libc.so.6"}));
        check(outsideLibraries(root / "path.so.clap").size() == 1, "a needed library named by an absolute path is refused");
        const auto lv2 = root / "own.lv2";
        write(lv2 / "own.so", elf({"libc.so.6", "libown.so.1"}, "$ORIGIN/lib"));
        write(lv2 / "lib/libown.so.1", elf({"libc.so.6"}));
        check(outsideLibraries(lv2).empty(), "a bundle's own library through $ORIGIN passes:" + join(outsideLibraries(lv2)));

        // Untrusted bytes: every truncation and random corruptions are reported or pass, never crash.
        std::mt19937 rng(7);
        size_t reported = 0;
        for(const auto& sample : {system, fat({system}), elf({"libc.so.6", "libx.so"}, "$ORIGIN")})
        {
            for(size_t n = 0; n < sample.size(); ++n) { write(root / "cut.clap", sample.substr(0, n)); reported += !outsideLibraries(root / "cut.clap").empty(); }
            for(int i = 0; i < 3000; ++i)
            {
                auto bad = sample;
                for(int k = 0; k < 1 + int(rng() % 8); ++k) bad[rng() % bad.size()] = char(rng());
                write(root / "bad.clap", bad);
                outsideLibraries(root / "bad.clap");
            }
        }
        check(reported > 0, "truncated files are reported, not trusted");

        // oscairn-sign refuses a build linking Homebrew, packs it only when told, and says so.
        write(root / "Brew.clap", machO({{load, "/opt/homebrew/lib/libfftw3.3.dylib"}}));
        const auto refused = run(tool + " plugin-package " + shell(root / "Brew.olib") + " clap com.example.brew 1.0 Brew macos-arm64=" + shell(root / "Brew.clap"));
        check(refused.first != 0 && refused.second.find("/opt/homebrew/lib/libfftw3.3.dylib") != std::string::npos && !fs::exists(root / "Brew.olib"), "oscairn-sign refuses it: " + refused.second);
        const auto forced = run(tool + " plugin-package --allow-outside-libraries " + shell(root / "Brew.olib") + " clap com.example.brew 1.0 Brew macos-arm64=" + shell(root / "Brew.clap"));
        check(forced.first == 0 && forced.second.find("outside") != std::string::npos && forced.second.find("/opt/homebrew/lib/libfftw3.3.dylib") != std::string::npos, "and packs it when told, saying so: " + forced.second);
        const auto verified = run(tool + " verify " + shell(root / "Brew.olib"));
        check(verified.first == 0 && verified.second.find("outside clap com.example.brew 1.0 macos-arm64 Brew.clap: /opt/homebrew/lib/libfftw3.3.dylib") != std::string::npos, "verify reports it: " + verified.second);

        // Oscairn's own plugins pass.
        for(int i = 2; i < argc; ++i) if(*argv[i]) check(outsideLibraries(argv[i]).empty(), std::string(argv[i]) + " loads only the system's libraries and its own:" + join(outsideLibraries(argv[i])));
    }
    catch(const std::exception& e) { check(false, e.what()); }
    std::error_code ignored; fs::remove_all(root, ignored);
    return failures ? 1 : 0;
}

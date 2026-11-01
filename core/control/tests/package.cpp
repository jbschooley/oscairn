// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021's archives: a library packed and unpacked as it was, with a plugin build (RFC 0006),
// and archives that would climb out, link, repeat, overflow, lie about their items or carry a
// file that is not the one the manifest hashed refused, leaving nothing behind.
#include "oscairn/package.h"
#include "oscairn/assets.h"
#include "miniz/miniz.h"
#include <cstdio>
#include <fstream>
#include <functional>
#include <random>
#include <string>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    void write(const fs::path& file, const std::string& text) { fs::create_directories(file.parent_path()); std::ofstream(file, std::ios::binary) << text; }
    // A manifest listing the one Sound, and `files` (each file's SHA-256 by its path).
    std::string listing(const std::string& files) { return R"({"format":"oscairn.library","version":2,"id":"abc","name":"Pack","kind":"library","items":[{"kind":"sound","id":"s1","name":"Bass","path":"Sounds/Bass.osound"}],"plugins":[],"files":{)" + files + "}}"; }
    const std::string manifest = listing("\"Sounds/Bass.osound/sound.json\":\"" + assets::sha256("{}") + "\"");

    // An archive made by hand, entry by entry; attr: the entry's Unix mode (links).
    void craft(const fs::path& archive, const std::vector<std::pair<std::string, std::string>>& entries, std::function<void(mz_zip_archive&)> extra = {})
    {
        mz_zip_archive z{};
        mz_zip_writer_init_file(&z, archive.string().c_str(), 0);
        for(const auto& [name, data] : entries) mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
        if(extra) extra(z);
        mz_zip_writer_finalize_archive(&z); mz_zip_writer_end(&z);
    }
    // Sets a 32-bit field of the named entry's central directory record (offset from its signature).
    void patch(std::string& bytes, const std::string& name, size_t offset, uint32_t value)
    {
        for(size_t at = bytes.find("PK\x01\x02"); at != std::string::npos; at = bytes.find("PK\x01\x02", at + 4))
            if(bytes.compare(at + 46, name.size(), name) == 0)
            {
                for(int k = 0; k < 4; ++k) bytes[at + offset + size_t(k)] = char((value >> (8 * k)) & 0xFF);
                bytes[at + 5] = 3; // made by Unix, so the mode counts
                return;
            }
    }
    bool refused(const fs::path& archive, const fs::path& into, const std::string& what)
    {
        try { readPackage(archive, into); } catch(const std::runtime_error&) { return !fs::exists(into); }
        std::fprintf(stderr, "accepted: %s\n", what.c_str());
        return false;
    }
}

int main()
{
    std::random_device random;
    const auto root = fs::temp_directory_path() / ("oscairn-package-" + std::to_string(random()));
    fs::create_directories(root);
    try
    {
        // A library round-trips: manifest, items and their files.
        const auto staged = root / "staged";
        write(staged / "Sounds/Bass.osound/sound.json", "{}");
        write(staged / "Sounds/Bass.osound/nodes/node-1/state.bin", std::string(5000, 'x'));
        write(staged / "Assets/0123.wav", "RIFF");
        write(staged / ("Plugins/clap/synth@1/" + platform() + "/Synth.clap/Contents/binary"), "code");
        write(staged / ("Plugins/clap/synth@1/" + platform() + "/Synth.clap/.DS_Store"), "hidden");
        const auto build = "Plugins/clap/synth@1/" + platform() + "/Synth.clap";
        Package p; p.id = "abc"; p.name = "Pack"; p.items = {{"sound", "s1", "Bass", "Sounds/Bass.osound"}};
        p.plugins = {{"clap", "synth", "1", "Synth", {{platform(), build, bundleDigest(staged / build)}}}};
        writePackage(root / "Pack.olib", p, staged);
        bool again = false; try { writePackage(root / "Pack.olib", p, staged); } catch(const std::runtime_error&) { again = true; }
        check(again, "an existing archive is not overwritten");
        const auto read = readPackage(root / "Pack.olib", root / "out");
        check(read.id == "abc" && read.name == "Pack" && read.kind == "library" && read.items.size() == 1 && read.items[0].path == "Sounds/Bass.osound", "the manifest round-trips");
        check(fs::file_size(root / "out/Sounds/Bass.osound/nodes/node-1/state.bin") == 5000 && fs::exists(root / "out/Assets/0123.wav"), "its files round-trip");
        check(read.plugins.size() == 1 && read.plugins[0].builds.size() == 1 && read.plugins[0].builds[0].sha256 == bundleDigest(root / "out" / build) &&
              fs::exists(root / "out" / build / "Contents/binary") && !fs::exists(root / "out" / build / ".DS_Store"), "a plugin build round-trips, without hidden files");
        check(read.files.size() == 4 && read.files.at("Assets/0123.wav") == assets::sha256("RIFF"), "every file is listed with its SHA-256");
        const auto manifestOnly = readPackage(root / "Pack.olib", {});
        check(manifestOnly.items.size() == 1 && manifestOnly.items[0].id == "s1", "the manifest alone, extracting nothing");

        // Refused, leaving nothing behind.
        int n = 0;
        auto refuse = [&](const std::string& what, const std::vector<std::pair<std::string, std::string>>& entries, std::function<void(mz_zip_archive&)> extra = {},
                          std::function<void(std::string&)> after = {}) {
            const auto archive = root / ("bad" + std::to_string(++n) + ".olib");
            craft(archive, entries, extra);
            if(after)
            {
                std::ifstream in(archive, std::ios::binary); std::string bytes((std::istreambuf_iterator<char>(in)), {}); in.close();
                after(bytes);
                std::ofstream(archive, std::ios::binary | std::ios::trunc) << bytes;
            }
            check(refused(archive, root / ("bad-out" + std::to_string(n)), what), what + " is refused");
        };
        const std::pair<std::string, std::string> sound{"Sounds/Bass.osound/sound.json", "{}"}, top{"library.json", manifest};
        refuse("a path climbing out", {top, sound, {"Sounds/../../escape.txt", "x"}});
        // miniz will not write a leading slash, so a same-length name is renamed in the bytes.
        refuse("an absolute path", {top, sound, {"Ztmp/escape.txt", "x"}}, {}, [](std::string& bytes) {
            for(auto at = bytes.find("Ztmp/escape.txt"); at != std::string::npos; at = bytes.find("Ztmp/escape.txt", at)) bytes[at] = '/'; });
        refuse("a drive", {top, sound, {"Sounds/C:evil", "x"}});
        refuse("a backslash", {top, sound, {"Sounds\\..\\evil", "x"}});
        refuse("an entry outside the folders", {top, sound, {"bin/run.sh", "x"}});
        refuse("a hidden entry", {top, sound, {"Sounds/.hidden", "x"}});
        refuse("an entry twice", {top, sound, sound});
        refuse("no manifest", {sound});
        refuse("a manifest naming a missing item", {{"library.json", manifest}});
        refuse("another format", {{"library.json", R"({"format":"other","version":1,"id":"a","name":"b","kind":"library","items":[]})"}});
        // Patched after writing: an entry marked as a link, and one declaring a huge size.
        refuse("a link", {top, sound, {"Assets/link", "/etc/passwd"}}, {}, [](std::string& bytes) { patch(bytes, "Assets/link", 38, (0120777u << 16)); });
        refuse("an entry over 2 GiB", {top, sound, {"Assets/big.bin", "x"}}, {}, [](std::string& bytes) { patch(bytes, "Assets/big.bin", 24, 0xF0000000u); });
        // Files that are not what the manifest says.
        refuse("a file changed after it was hashed", {{"library.json", manifest}, {"Sounds/Bass.osound/sound.json", "{ }"}});
        refuse("a file the manifest does not list", {top, sound, {"Assets/extra.wav", "x"}});
        refuse("a file the manifest lists that is missing", {{"library.json", listing("\"Sounds/Bass.osound/sound.json\":\"" + assets::sha256("{}") + "\",\"Assets/gone.wav\":\"" + assets::sha256("x") + "\"")}, sound});
        const auto binary = "Plugins/clap/synth@1/" + platform() + "/Synth.clap/Contents/binary";
        auto withBuild = [&](const std::string& digest) {
            auto m = listing("\"Sounds/Bass.osound/sound.json\":\"" + assets::sha256("{}") + "\",\"" + binary + "\":\"" + assets::sha256("code") + "\"");
            return m.replace(m.find("\"plugins\":[]"), 12, "\"plugins\":[{\"format\":\"clap\",\"id\":\"synth\",\"version\":\"1\",\"name\":\"Synth\",\"builds\":[{\"platform\":\"" + platform() + "\",\"path\":\"" + build + "\",\"sha256\":\"" + digest + "\"}]}]");
        };
        refuse("a plugin build whose digest is not its own", {{"library.json", withBuild(std::string(64, '0'))}, sound, {binary, "code"}});
        refuse("a plugin build that is not in the archive", {{"library.json", withBuild(bundleDigest(staged / build))}, sound});
        check(!fs::exists(root / "escape.txt") && !fs::exists("/tmp/escape.txt"), "nothing escaped");
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    fs::remove_all(root);
    if(!failures) std::puts("ok: archives round-trip, and unsafe ones are refused");
    return failures ? 1 : 0;
}

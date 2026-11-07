// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021's archives: a library packed and unpacked as it was, with a plugin build (RFC 0006),
// and archives that would climb out, link, repeat, overflow, lie about their items or carry a
// file that is not the one the manifest hashed refused, leaving nothing behind.
#include "oscairn/package.h"
#include "oscairn/assets.h"
#include "oscairn/library.h"
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
    std::string listing(const std::string& files) { return R"({"format":"oscairn.library","version":3,"id":"abc","name":"Pack","kind":"library","items":[{"kind":"sound","id":"s1","name":"Bass","path":"Sounds/Bass.osound"}],"plugins":[],"files":{)" + files + "}}"; }
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
        // A manifest past the old 4 MiB limit, as a library of thousands of items has, is read; one
        // past maxManifestBytes is refused. Padded with JSON whitespace before its closing brace.
        auto padded = [&](size_t bytes) { auto m = manifest; m.insert(m.size() - 1, std::string(bytes - m.size(), ' ')); return m; };
        craft(root / "Big.olib", {{"library.json", padded((4u << 20) + 1)}, {"Sounds/Bass.osound/sound.json", "{}"}});
        check(readPackage(root / "Big.olib", root / "big-out").items.size() == 1, "a manifest just over 4 MiB is read");
        // More entries than the old limit of 65,536, as a library of thousands of items has.
        craft(root / "Many.olib", {{"library.json", manifest}, {"Sounds/Bass.osound/sound.json", "{}"}}, [](mz_zip_archive& z) {
            for(int i = 0; i < 70000; ++i) { const auto name = "Assets/" + std::to_string(i); mz_zip_writer_add_mem(&z, name.c_str(), "", 0, 0); } });
        check(readPackage(root / "Many.olib", {}).items.size() == 1, "an archive of 70,000 entries is read");
        // Names past 64 MiB in all, long ones well under the entry limit, are refused before they pile up.
        craft(root / "Long.olib", {{"library.json", manifest}, {"Sounds/Bass.osound/sound.json", "{}"}}, [](mz_zip_archive& z) {
            const auto folder = "Assets/" + std::string(250, 'a') + "/"; // miniz reads names up to 511 bytes
            for(int i = 0; i < 135000; ++i) { auto name = std::to_string(i); name = folder + name + std::string(242 - name.size(), 'b'); mz_zip_writer_add_mem(&z, name.c_str(), "", 0, 0); } });
        check(refused(root / "Long.olib", {}, "64 MiB of names"), "64 MiB of names is refused");
        fs::remove(root / "Long.olib");
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
        refuse("a manifest over the limit", {{"library.json", padded(size_t(maxManifestBytes) + 1)}, sound});
        refuse("an entry over 2 GiB", {top, sound, {"Assets/big.bin", "x"}}, {}, [](std::string& bytes) { patch(bytes, "Assets/big.bin", 24, 0xF0000000u); });
        // Files that are not what the manifest says.
        refuse("a file changed after it was hashed", {{"library.json", manifest}, {"Sounds/Bass.osound/sound.json", "{ }"}});
        refuse("a file the manifest does not list", {top, sound, {"Assets/extra.wav", "x"}});
        refuse("a file the manifest lists that is missing", {{"library.json", listing("\"Sounds/Bass.osound/sound.json\":\"" + assets::sha256("{}") + "\",\"Assets/gone.wav\":\"" + assets::sha256("x") + "\"")}, sound});
        const auto binary = "Plugins/clap/synth@1/" + platform() + "/Synth.clap/Contents/binary";
        auto withBuild = [&](const std::string& digest, const std::string& forPlatform = platform()) {
            auto m = listing("\"Sounds/Bass.osound/sound.json\":\"" + assets::sha256("{}") + "\",\"" + binary + "\":\"" + assets::sha256("code") + "\"");
            return m.replace(m.find("\"plugins\":[]"), 12, "\"plugins\":[{\"format\":\"clap\",\"id\":\"synth\",\"version\":\"1\",\"name\":\"Synth\",\"builds\":[{\"platform\":\"" + forPlatform + "\",\"path\":\"" + build + "\",\"sha256\":\"" + digest + "\"}]}]");
        };
        refuse("a plugin build whose digest is not its own", {{"library.json", withBuild(std::string(64, '0'))}, sound, {binary, "code"}});
        refuse("a plugin build that is not in the archive", {{"library.json", withBuild(bundleDigest(staged / build))}, sound});
        // A build's platform names a folder in the plugin store: one that climbs out would replace what is there.
        refuse("a plugin build for no known platform", {{"library.json", withBuild(bundleDigest(staged / build), "../../../Performances")}, sound, {binary, "code"}});
        check(!fs::exists(root / "escape.txt") && !fs::exists("/tmp/escape.txt"), "nothing escaped");
        // The package's ID names the library's folder, and an exported item's ID is its package's:
        // either one that is not a plain folder name is refused (as JSON: a\b is "a\\b"). Windows
        // drops a trailing dot from a folder's name, so the folder would not be the ID.
        for(const std::string bad : std::vector<std::string>{"../x", ".x", "a/b", "a\\\\b", "a:b", "x.", "", "a\\\"b", std::string(65, 'a')})
        {
            refuse("the package ID \"" + bad + "\"", {{"library.json", std::string(manifest).replace(manifest.find("\"id\":\"abc\""), 10, "\"id\":\"" + bad + "\"")}, sound});
            refuse("the item ID \"" + bad + "\"", {{"library.json", std::string(manifest).replace(manifest.find("\"id\":\"s1\""), 9, "\"id\":\"" + bad + "\"")}, sound});
        }
        for(const std::string good : std::vector<std::string>{"com.example.pack", "a", "A-b_c.9", "0123456789abcdef", std::string(64, 'a')})
        {
            craft(root / "Good.olib", {{"library.json", std::string(manifest).replace(manifest.find("\"id\":\"s1\""), 9, "\"id\":\"" + good + "\"").replace(manifest.find("\"id\":\"abc\""), 10, "\"id\":\"" + good + "\"")}, sound});
            std::string why; try { readPackage(root / "Good.olib", {}); } catch(const std::exception& e) { why = e.what(); }
            check(why.empty(), "the ID " + good + " is a plain name: " + why);
            fs::remove(root / "Good.olib");
        }
        refuse("version 2",{{"library.json", std::string(manifest).replace(manifest.find("\"version\":3"), 11, "\"version\":2")}, sound});
        // A plugin package: plugin builds and no items.
        {
            const auto onlyPlugin = root / "plugin-only";
            write(onlyPlugin / build / "Contents/binary", "code");
            Package plugin; plugin.id = "synth"; plugin.name = "Synth"; plugin.kind = "plugin"; plugin.license = "MIT";
            plugin.plugins = {{"clap", "synth", "1", "Synth", {{platform(), build, bundleDigest(onlyPlugin / build)}}}};
            writePackage(root / "Plugin.olib", plugin, onlyPlugin);
            const auto got = readPackage(root / "Plugin.olib", root / "plugin-out");
            check(got.kind == "plugin" && got.items.empty() && got.plugins.size() == 1 && fs::exists(root / "plugin-out" / build / "Contents/binary"), "a plugin package round-trips");
            // One bundle holding two plugins: both list it; the same plugin twice is refused.
            plugin.plugins.push_back({"clap", "synth.two", "1", "Synth Two", {{platform(), build, bundleDigest(onlyPlugin / build)}}});
            writePackage(root / "Two.olib", plugin, onlyPlugin);
            check(readPackage(root / "Two.olib", root / "two-out").plugins.size() == 2, "two plugins share one bundle");
            plugin.plugins[1].id = "synth";
            writePackage(root / "Twice.olib", plugin, onlyPlugin);
            check(refused(root / "Twice.olib", root / "twice-out", "a plugin listed twice"), "a plugin listed twice is refused");
            plugin.plugins.pop_back();
            // Its license and source: a plugin package names a license; under the GPL, LGPL or AGPL
            // it carries its source and a license text, and a source is an https:// address and a revision.
            int n = 0;
            auto packs = [&](Package q, bool withText) {
                const auto from = root / ("licensed-" + std::to_string(++n));
                fs::copy(onlyPlugin, from, fs::copy_options::recursive);
                if(withText) write(from / "Licenses/COPYING", "GPL");
                const auto file = root / ("Licensed-" + std::to_string(n) + ".olib");
                writePackage(file, q, from);
                try { const auto got = readPackage(file, root / ("licensed-out-" + std::to_string(n))); return got.license == q.license && got.sourceUrl == q.sourceUrl && got.sourceRevision == q.sourceRevision; }
                catch(const std::runtime_error&) { return false; }
            };
            auto gpl = plugin; gpl.license = "GPL-3.0-or-later";
            check(!packs([&] { auto q = plugin; q.license.clear(); return q; }(), false), "a plugin package without a license is refused");
            check(!packs(gpl, true), "a GPL plugin package without its source is refused");
            gpl.sourceUrl = "https://github.com/example/organ"; gpl.sourceRevision = "v2.1.0";
            check(!packs(gpl, false), "a GPL plugin package without a license text is refused");
            check(packs(gpl, true), "a GPL plugin package with its source and license text is read, with them");
            auto lgpl = gpl; lgpl.license = "MIT AND (LGPL-2.1-or-later OR BSD-3-Clause)"; lgpl.sourceUrl.clear();
            check(!packs(lgpl, true), "an expression naming the LGPL needs the source too");
            for(const auto& [url, revision] : std::vector<std::pair<std::string, std::string>>{{"http://example.com/x", "v1"}, {"https://example.com/x", ""}, {"https://example.com/x y", "v1"}, {"https://example.com/x", "a b"}})
            {
                auto q = gpl; q.sourceUrl = url; q.sourceRevision = revision;
                check(!packs(q, true), "the source " + url + " at \"" + revision + "\" is refused");
            }
            auto pluginKind = [&](std::string m) { return m.replace(m.find("\"kind\":\"library\""), 16, "\"kind\":\"plugin\""); };
            refuse("a plugin package with items", {{"library.json", pluginKind(withBuild(bundleDigest(staged / build)))}, sound, {binary, "code"}});
            refuse("a plugin package with no plugins", {{"library.json", R"({"format":"oscairn.library","version":3,"id":"abc","name":"Pack","kind":"plugin","items":[],"plugins":[],"files":{}})"}});
            refuse("an app package, not built yet", {{"library.json", std::string(manifest).replace(manifest.find("\"kind\":\"library\""), 16, "\"kind\":\"app\"")}, sound});
        }

        // Signatures (RFC 0006): an item, a build and the whole archive, each verified on reading.
        const auto mine = signing::makeKey(root / "mine.key"), other = signing::makeKey(root / "other.key");
        const auto signedRoot = root / "signed";
        const auto doc = signedRoot / "Sounds/Bass.osound/sound.json";
        write(doc, R"({"format":"oscairn.sound","info":{"id":"s1","name":"Bass"},"nodes":[]})");
        write(signedRoot / "Sounds/Bass.osound/nodes/node-1/state.bin", "state");
        write(signedRoot / binary, "code");
        auto itemSig = [&](const signing::Key& k) { return signing::Signature{k.publicKey, signing::sign(k, signing::itemMessage(contentHash(doc)))}; };
        auto buildSig = [&](const signing::Key& k) { return signing::Signature{k.publicKey, signing::sign(k, signing::buildMessage("clap", "synth", "1", platform(), bundleDigest(signedRoot / build)))}; };
        Package s; s.id = "abc"; s.name = "Pack";
        s.items = {{"sound", "s1", "Bass", "Sounds/Bass.osound", {itemSig(mine)}}};
        s.plugins = {{"clap", "synth", "1", "Synth", {{platform(), build, bundleDigest(signedRoot / build), {buildSig(mine)}}}}};
        writePackage(root / "Signed.olib", s, signedRoot, &mine);
        const auto got = readPackage(root / "Signed.olib", root / "signed-out");
        check(got.signature.key == mine.publicKey && got.items.at(0).signatures.size() == 1 && got.items[0].signatures[0].key == mine.publicKey &&
              got.plugins.at(0).builds.at(0).signatures.size() == 1 && got.plugins[0].builds[0].signatures[0].key == mine.publicKey, "item, build and archive signatures verify, and say who signed");
        check(readPackage(root / "Signed.olib", {}).signature.key == mine.publicKey, "the archive's signature is checked from the manifest alone");
        // The whole archive signed afterwards, as oscairn-sign does.
        writePackage(root / "Later.olib", p, staged);
        signPackage(root / "Later.olib", other);
        check(readPackage(root / "Later.olib", {}).signature.key == other.publicKey, "an archive signed afterwards verifies");

        // Each refused: entries of the signed archive rewritten as one who changed it would.
        auto entries = [](const fs::path& archive) {
            std::vector<std::pair<std::string, std::string>> out;
            mz_zip_archive z{}; mz_zip_reader_init_file(&z, archive.string().c_str(), 0);
            for(mz_uint i = 0; i < mz_zip_reader_get_num_files(&z); ++i)
            {
                mz_zip_archive_file_stat st; mz_zip_reader_file_stat(&z, i, &st);
                if(mz_zip_reader_is_file_a_directory(&z, i)) continue;
                std::string data(size_t(st.m_uncomp_size), '\0'); mz_zip_reader_extract_to_mem(&z, i, data.data(), data.size(), 0);
                out.push_back({st.m_filename, data});
            }
            mz_zip_reader_end(&z);
            return out;
        };
        auto edited = [&](const std::string& entry, const std::function<void(std::string&)>& change) {
            auto all = entries(root / "Signed.olib");
            for(auto& [name, data] : all) if(name == entry) change(data);
            return all;
        };
        refuse("a manifest changed under the archive's signature", edited("library.json", [](std::string& m) { m.replace(m.find("\"Pack\""), 6, "\"Pick\""); }));
        refuse("an archive signature by another key", edited("library.sig", [&](std::string& sig) { sig.replace(sig.find(mine.publicKey), 64, other.publicKey); }));
        // An item or build changed, and the manifest's hashes made to match, without the archive's signature.
        Package unsignedWhole = s;
        write(signedRoot / "Sounds/Bass.osound/nodes/node-1/state.bin", "tampered");
        writePackage(root / "TamperedItem.olib", unsignedWhole, signedRoot);
        check(refused(root / "TamperedItem.olib", root / "tampered-item", "a tampered item"), "a tampered item is refused by its signature");
        write(signedRoot / "Sounds/Bass.osound/nodes/node-1/state.bin", "state");
        write(signedRoot / binary, "evil");
        unsignedWhole.plugins[0].builds[0].sha256 = bundleDigest(signedRoot / build);
        writePackage(root / "TamperedBuild.olib", unsignedWhole, signedRoot);
        check(refused(root / "TamperedBuild.olib", {}, "a tampered build") && refused(root / "TamperedBuild.olib", root / "tampered-build", "a tampered build"), "a tampered build is refused by its signature");
        write(signedRoot / binary, "code");
        Package forged = s;
        forged.items[0].signatures[0].key = other.publicKey;
        writePackage(root / "OtherItemKey.olib", forged, signedRoot);
        check(refused(root / "OtherItemKey.olib", root / "other-item", "an item signature by another key"), "an item signature by another key is refused");
        forged = s; forged.plugins[0].builds[0].signatures[0].key = other.publicKey;
        writePackage(root / "OtherBuildKey.olib", forged, signedRoot);
        check(refused(root / "OtherBuildKey.olib", {}, "a build signature by another key"), "a build signature by another key is refused");
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    fs::remove_all(root);
    if(!failures) std::puts("ok: archives round-trip, and unsafe ones are refused");
    return failures ? 1 : 0;
}

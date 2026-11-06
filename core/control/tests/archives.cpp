// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021's libraries and exports, across devices (each its own OSCAIRN_DATA_HOME): an exported
// Performance carries its unedited part's Sound and the samples they use; installed into the
// user's own storage it gets new IDs, its part pointing at the Sound's copy, and installing again
// skips what is unchanged; installed as a library it is locked, comes back when deleted and goes
// when uninstalled, leaving the user's copies and the samples. The plugin it uses goes with it
// (RFC 0006) and installs into the store, where a Performance whose plugin is not at its saved
// path finds it. A build for another platform added on one device goes along too. The store
// lists each plugin with what uses it, and removing one is refused while the open Performance
// plays it; what used it then opens with a stand-in until it is installed again.
// Signatures (RFC 0006): oscairn-sign signs a build and an archive, an export carries the build's
// signature and signs its items and the whole archive when asked.
//   oscairn-archives-test FIXTURE.clap OSCAIRN-SIGN
#include "oscairn/session.h"
#include "oscairn/package.h"
#include "miniz/miniz.h"
#include "oscairn/wav_file.h"
#include "../../plugin-host/src/StateFiles.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    std::string read(const fs::path& f) { std::ifstream in(f, std::ios::binary); std::ostringstream out; out << in.rdbuf(); return out.str(); }
    void device(const fs::path& home) { fs::create_directories(home); ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1); }
    std::vector<Control::LibraryItem> items(Session& s, const std::string& kind) { Control::LibraryQuery q; q.kind = kind; return s.libraryQuery(q).items; }
    std::string shell(const fs::path& p) { std::string out = "'"; for(char c : p.string()) out += c == '\'' ? std::string("'\\''") : std::string(1, c); return out + "'"; }
    // A command's exit status and what it printed.
    std::pair<int, std::string> run(const std::string& command)
    {
        std::string out;
        FILE* p = ::popen(command.c_str(), "r");
        if(!p) return {-1, out};
        char buffer[4096];
        while(const auto n = std::fread(buffer, 1, sizeof buffer, p)) out.append(buffer, n);
        return {::pclose(p), out};
    }
    // A copy of an archive whose manifest each(a) rewrote; other entries are copied whole.
    void rewriteArchive(const fs::path& from, const fs::path& to, const std::function<std::string(std::string)>& each)
    {
        mz_zip_archive in{}, out{};
        mz_zip_reader_init_file(&in, from.c_str(), 0);
        mz_zip_writer_init_file(&out, to.c_str(), 0);
        for(mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i)
        {
            mz_zip_archive_file_stat st; mz_zip_reader_file_stat(&in, i, &st);
            if(std::string(st.m_filename) != "library.json") { mz_zip_writer_add_from_zip_reader(&out, &in, i); continue; }
            std::string m(size_t(st.m_uncomp_size), '\0'); mz_zip_reader_extract_to_mem(&in, i, m.data(), m.size(), 0);
            m = each(m);
            mz_zip_writer_add_mem(&out, "library.json", m.data(), m.size(), MZ_DEFAULT_COMPRESSION);
        }
        mz_zip_writer_finalize_archive(&out); mz_zip_writer_end(&out); mz_zip_reader_end(&in);
    }
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s FIXTURE OSCAIRN-SIGN\n", argv[0]); return 2; }
    const char* fixture = argv[1];
    const std::string tool = shell(argv[2]);
    try
    {
        state_files::TemporaryDirectory temp;
        const auto archive = temp.path / "Set.olib";
        const auto soundArchive = temp.path / "Bass.olib";
        kick::clap::AssetReference sample;
        std::string soundId, performanceId;
        const std::string other = platform() == "linux-arm64" ? "macos-arm64" : "linux-arm64";
        // Device A: a part saved as a Sound that uses a sample, then the Performance exported.
        {
            device(temp.path / "a");
            Session a(48000, 64);
            const auto synth = a.add("synth", fixture, "org.kick.test.synth");
            const auto part = a.addPart("Bass"); a.insertIntoPart(part, 0, synth); a.commit();
            soundId = a.savePartAsSound(part, "Bass").id;
            const auto wav = temp.path / "bass.wav"; writeWavFile(wav, std::vector<float>(480, .5f), 1, 48000);
            sample = a.importAsset(wav.string());
            // The fixture uses no samples, so its Sound's document is given one, as a sampler's lists it.
            const auto doc = fs::path(a.libraryQuery({}).items.front().path) / "sound.json";
            auto text = read(doc);
            const auto at = text.find("\"assets\":[]"); check(at != std::string::npos, "the Sound's plugin lists its samples");
            text.replace(at, 11, "\"assets\":[{\"library\":\"user\",\"path\":\"" + sample.path + "\",\"sha256\":\"" + sample.sha256 + "\"}]");
            std::ofstream(doc, std::ios::binary | std::ios::trunc) << text;
            a.libraryRescan();
            performanceId = a.savePerformanceToLibrary("Set");
            // The fixture stands in for another platform's build: it cannot be loaded here to check either way.
            const auto version = a.graphState().topology.nodes.at(0).plugin.version;
            bool here = false; try { a.addPluginBuild("clap", "org.kick.test.synth", version, platform(), fixture); } catch(const std::exception&) { here = true; }
            check(here, "this device's build is not added: it is the one it loads");
            a.addPluginBuild("clap", "org.kick.test.synth", version, other, fixture);
            check(a.exportPerformance(archive.string()).empty(), "nothing left out");
            const auto packed = readPackage(archive, {});
            check(packed.plugins.size() == 1 && packed.plugins[0].id == "org.kick.test.synth" && packed.plugins[0].builds.size() == 2 &&
                  packed.plugins[0].builds[0].sha256 == bundleDigest(fixture) && packed.plugins[0].builds[1].sha256 == bundleDigest(fixture),
                  "the plugin it uses goes with it, this device's build and the other platform's");
            bool again = false; try { a.exportPerformance(archive.string()); } catch(const std::exception&) { again = true; }
            check(again, "an export never overwrites a file");
            const auto contents = a.archiveContents(archive.string());
            check(contents.kind == "performance" && contents.name == "Set" && contents.items.size() == 2, "the export holds the Performance and its part's Sound: " + std::to_string(contents.items.size()));
            // From the library, without opening: the Performance as saved with its part's Sound, and a Sound alone.
            a.exportLibraryItem(performanceId, (temp.path / "Saved.olib").string());
            const auto saved = a.archiveContents((temp.path / "Saved.olib").string());
            check(saved.kind == "performance" && saved.id == performanceId && saved.items.size() == 2 && saved.items[1].id == soundId, "a library Performance exported with its part's Sound");
            a.exportLibraryItem(soundId, soundArchive.string());
            const auto sound = a.archiveContents(soundArchive.string());
            check(sound.kind == "sound" && sound.items.size() == 1 && sound.items[0].id == soundId, "a library Sound exported alone");
            bool missing = false; try { a.exportLibraryItem("nothing", (temp.path / "None.olib").string()); } catch(const std::exception&) { missing = true; }
            check(missing, "an item not in the library is refused");
        }
        // Device B: into the user's own storage, then as a library.
        {
            device(temp.path / "b");
            Session b(48000, 64);
            // Its plugin builds are unsigned (RFC 0006): listed first, refused without a yes, leaving nothing.
            const auto contents = b.archiveContents(archive.string());
            check(contents.code.size() == 2 && contents.code[0].status == "unsigned" && contents.code[0].kind == "plugin" && contents.code[0].version == "1" && contents.signer.status == "unsigned",
                  "an archive's contents say its plugin builds and the archive are unsigned");
            std::string why; try { b.installArchive(archive.string(), false); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("unknown source") != std::string::npos && why.find("KICK fixture 1 (" + platform() + ") unsigned") != std::string::npos && items(b, "sound").empty() &&
                  !fs::exists(temp.path / "b" / "Plugins"), "an unsigned plugin build is refused without a yes, listing it, installing nothing: " + why);
            const auto installed = b.installArchive(archive.string(), false, {}, true);
            check(installed.code.size() == 2 && installed.code[0].status == "unsigned", "and installed with one, saying so");
            check(installed.added == 2 && installed.skipped == 0 && installed.notes.empty(), "both copied in: " + std::to_string(installed.added));
            const auto store = temp.path / "b" / "Plugins" / "clap";
            check(fs::is_directory(store) && std::distance(fs::directory_iterator(store), fs::directory_iterator()) == 1, "its plugin is in the store");
            const auto listed = b.installedPlugins();
            check(listed.size() == 1 && listed[0].id == "org.kick.test.synth" && listed[0].platforms.size() == 2 && listed[0].bytes > 0 && listed[0].usedBy.size() == 2 && !listed[0].open,
                  "the store lists it with both platforms' builds and the two items that use it");
            // Re-exported from here, both builds go again: this device's and the other from the store.
            check(b.exportLibraryItem(items(b, "sound").at(0).id, (temp.path / "Again.olib").string()).empty() && readPackage(temp.path / "Again.olib", {}).plugins.at(0).builds.size() == 2,
                  "a re-export carries every platform's build");
            const auto sounds = items(b, "sound"), performances = items(b, "performance");
            check(sounds.size() == 1 && performances.size() == 1 && sounds[0].id != soundId && performances[0].id != performanceId && sounds[0].library.empty(),
                  "under new IDs, as the user's own");
            check(sounds[0].origin.item == soundId && sounds[0].origin.hash == sounds[0].hash, "each records where it came from and its hash as imported");
            b.loadPerformanceFromLibrary(performances[0].id);
            check(b.graphState().parts.size() == 1 && b.graphState().parts[0].from.sound == sounds[0].id, "the Performance's part points at the Sound's copy");
            auto copy = sample; check(!assets::Store(temp.path / "b" / "assets").resolve(copy).empty(), "the sample came with it");
            const auto again = b.installArchive(archive.string(), false);
            check(again.added == 0 && again.skipped == 2 && items(b, "sound").size() == 1, "installing again adds nothing");
            b.deleteLibraryItem(sounds[0].id);
            const auto back = b.installArchive(archive.string(), false);
            check(back.added == 1 && back.skipped == 1, "a deleted copy comes again, the unchanged one is skipped");

            // As a library: locked, under its own IDs.
            const auto library = b.installArchive(archive.string(), true);
            check(library.added == 2 && b.libraries().size() == 1 && b.libraries()[0].items == 2, "installed as a library");
            // An installed library's manifest past the old 4 MiB limit still reads (a converted
            // MONTAGE M library's does): padded with JSON whitespace before its closing brace.
            for(const auto& folder : fs::directory_iterator(temp.path / "b" / "Libraries"))
            {
                auto text = read(folder.path() / "library.json");
                text.insert(text.rfind('}'), std::string((4u << 20) + 1, ' '));
                std::ofstream(folder.path() / "library.json", std::ios::binary | std::ios::trunc) << text;
            }
            b.libraryRescan();
            check(b.libraries().size() == 1 && b.libraries()[0].items == 2, "an installed library with a manifest over 4 MiB is listed");
            bool locked = false;
            for(const auto& i : items(b, "sound")) if(i.id == soundId) locked = !i.library.empty();
            check(locked, "its items keep their IDs, locked");
            b.loadPerformanceFromLibrary(performanceId);
            const auto saved = b.savePerformanceToLibrary("");
            check(saved != performanceId, "saving a locked Performance makes a new one");
            const auto part = b.graphState().parts[0].id;
            check(b.savePartAsSound(part, "").id != soundId, "and saving a part from a locked Sound makes a new Sound");
            b.deleteLibraryItem(soundId);
            const auto restored = b.installArchive(archive.string(), true);
            check(restored.added == 1 && restored.skipped == 1, "installing again restores only what was deleted");
            bool refusedEdit = false; try { b.libraryEdit(soundId, {.name = "Mine"}); } catch(const std::exception&) { refusedEdit = true; }
            check(refusedEdit, "a locked item's info is not edited");
            const auto lockedId = b.libraries()[0].id;
            Control::LibraryQuery inLibrary; inLibrary.source = lockedId;
            check(b.libraryQuery(inLibrary).total == 2, "a library's items by source");
            const auto own = b.libraryCopy(soundId);
            auto find = [&](const std::string& id) { for(const auto& i : items(b, "sound")) if(i.id == id) return i; throw std::runtime_error("not in the library: " + id); };
            const auto ownItem = find(own);
            check(ownItem.library.empty() && ownItem.name == "Bass" && ownItem.origin.library == lockedId && ownItem.origin.item == soundId && ownItem.origin.hash == ownItem.hash,
                  "a locked item's copy is the user's own, under its name, recording where it came from");
            b.libraryEdit(own, {.name = "Mine", .tags = {"bass"}});
            check(find(own).name == "Mine", "and is edited");
            b.deleteLibraryItem(own);
            b.uninstallLibrary(b.libraries()[0].id);
            check(b.libraries().empty() && items(b, "performance").size() == 2 && items(b, "sound").size() == 2, "uninstalled: its items go, the user's stay");
            copy = sample; check(!assets::Store(temp.path / "b" / "assets").resolve(copy).empty(), "and so does the sample");
        }
        // Device C: chosen items only.
        {
            device(temp.path / "c");
            Session c(48000, 64);
            const auto installed = c.installArchive(archive.string(), false, {soundId}, true);
            check(installed.added == 1 && items(c, "performance").empty() && items(c, "sound").size() == 1, "only the chosen Sound");
            check(c.installArchive(soundArchive.string(), false).added == 1 && items(c, "sound").size() == 2, "an exported Sound installs as the user's own");
            bool refused = false; try { c.installArchive(archive.string(), false, {"nothing"}); } catch(const std::exception&) { refused = true; }
            check(refused, "an item the archive does not have is refused");
            // Unpacking happens in the data folder, never the OS temp folder: on a tmpfs /tmp a
            // big archive would fill RAM on the way in. The OS temp folder is read-only here, so
            // an install that unpacked there could not run. Root writes there anyway, so as root
            // this proves nothing and is skipped.
            if(::geteuid() != 0)
            {
                const auto fakeTemp = temp.path / "c" / "fake-temp";
                fs::create_directories(fakeTemp);
                ::setenv("TMPDIR", fakeTemp.c_str(), 1);
                ::chmod(fakeTemp.c_str(), 0500);
                const auto again = c.installArchive(soundArchive.string(), false); // the same export again: skipped
                ::chmod(fakeTemp.c_str(), 0700);
                ::unsetenv("TMPDIR");
                check(again.skipped == 1 && items(c, "sound").size() == 2, "installing unpacks inside the data folder, not the read-only OS temp folder");
            }
            else std::puts("skipped: unpacking outside a read-only OS temp folder (root writes there anyway)");
        }
        // An install or export cut short by a crash leaves its hidden folder in the data folder;
        // the library clears those over a day old when it starts, and leaves newer ones, which
        // may be another process's install in progress.
        {
            const auto home = temp.path / "o";
            device(home);
            const auto old = fs::file_time_type::clock::now() - std::chrono::hours(25);
            for(const auto* name : {".install-1", ".export-1", ".install-2", ".other-1"})
            {
                fs::create_directories(home / name / "Sounds");
                std::ofstream(home / name / "Sounds" / "big.bin") << "x";
                if(std::string(name) != ".install-2") fs::last_write_time(home / name, old);
            }
            Session o(48000, 64);
            check(items(o, "sound").empty(), "the library starts");
            check(!fs::exists(home / ".install-1") && !fs::exists(home / ".export-1"), "an install's and an export's folder over a day old are cleared");
            check(fs::is_directory(home / ".install-2") && fs::is_directory(home / ".other-1"), "a newer one, and any other hidden folder, stay");
        }
        // Device E: the library's index past the old 4 MiB limit is still used, as a library of
        // thousands of items has: a name only the index holds comes back.
        {
            device(temp.path / "e");
            Session e(48000, 64);
            e.savePerformanceToLibrary("Indexed");
            e.libraryRescan();
            const auto index = temp.path / "e" / "library" / "index.json";
            auto text = read(index);
            const auto at = text.find("\"name\":\"Indexed\"");
            check(at != std::string::npos, "the index holds the Performance");
            text.replace(at, 16, "\"name\":\"From the index\"");
            text.insert(text.rfind('}'), std::string((4u << 20) + 1, ' '));
            std::ofstream(index, std::ios::binary | std::ios::trunc) << text;
            e.libraryRescan();
            check(items(e, "performance").at(0).name == "From the index", "an index over 4 MiB is used");
        }
        // Device F: a plugin rebuilt in place while it plays goes with an export as rebuilt.
        {
            device(temp.path / "f");
            const auto build = temp.path / "f" / "Synth.clap";
            fs::copy_file(fixture, build);
            Session f(48000, 64);
            f.add("synth", build.string(), "org.kick.test.synth"); f.commit();
            check(f.exportPerformance((temp.path / "Before.olib").string()).empty(), "a plugin at its own path goes with an export");
            // Rebuilt as a linker does: written beside it and moved over it.
            fs::copy_file(fixture, build.string() + ".new");
            std::ofstream(build.string() + ".new", std::ios::binary | std::ios::app) << "rebuilt";
            fs::rename(build.string() + ".new", build);
            fs::last_write_time(build, fs::file_time_type::clock::now() + std::chrono::minutes(1));
            const auto notes = f.exportPerformance((temp.path / "After.olib").string());
            check(notes.empty() && readPackage(temp.path / "After.olib", {}).plugins.at(0).builds.at(0).sha256 == bundleDigest(build),
                  "and once rebuilt, as rebuilt: " + (notes.empty() ? std::string() : notes[0]));
        }
        // Device D: the plugin is not where the Performance last found it, so it opens from the store.
        {
            device(temp.path / "d");
            Session d(48000, 64);
            d.installArchive(archive.string(), false, {}, true);
            const auto performance = items(d, "performance").at(0);
            const auto doc = fs::path(performance.path) / "performance.json";
            auto text = read(doc);
            for(auto at = text.find(fixture); at != std::string::npos; at = text.find(fixture)) text.replace(at, std::strlen(fixture), "/nowhere/Synth.clap");
            std::ofstream(doc, std::ios::binary | std::ios::trunc) << text;
            d.libraryRescan();
            d.loadPerformanceFromLibrary(performance.id);
            const auto nodes = d.graphState().topology.nodes;
            check(nodes.size() == 1 && nodes[0].plugin.path.find((temp.path / "d" / "Plugins").string()) == 0 && d.sessionStatus().failures.empty(), "the plugin opens from the store");
            // Installing again with the build there already adds nothing and says nothing.
            check(d.installArchive(archive.string(), false).notes.empty(), "the same build is not installed twice");
            const auto plugin = d.installedPlugins().at(0);
            check(plugin.open, "the store says the open Performance plays it");
            bool refused = false; try { d.removePlugin(plugin.format, plugin.id, plugin.version); } catch(const std::exception&) { refused = true; }
            check(refused && !d.installedPlugins().empty(), "removing a plugin the open Performance plays is refused");
            d.removePlugin(plugin.format, plugin.id, plugin.version, other);
            check(d.installedPlugins().at(0).platforms == std::vector<std::string>{platform()}, "one platform's build removed alone");
            fs::create_directories(temp.path / "d" / "Victim");
            bool climbed = false; try { d.removePlugin(plugin.format, plugin.id, plugin.version, platform() + "/../../../../Victim"); } catch(const std::exception&) { climbed = true; }
            check(climbed && fs::is_directory(temp.path / "d" / "Victim"), "a platform that climbs out of the store is refused, removing nothing");
            d.remove(nodes[0].id); d.commit();
            d.removePlugin(plugin.format, plugin.id, plugin.version);
            check(d.installedPlugins().empty(), "removed once nothing open plays it");
            const auto cutShort = temp.path / "d" / "Plugins" / "clap" / ".synth@1.removing";
            fs::create_directories(cutShort / "bundle");
            check(d.installedPlugins().empty() && !fs::exists(cutShort), "a removal cut short is cleared away");
            d.loadPerformanceFromLibrary(performance.id);
            check(d.sessionStatus().failures.size() == 1, "what used it opens with a stand-in");
            d.installArchive(archive.string(), false, {}, true);
            d.loadPerformanceFromLibrary(performance.id);
            check(d.sessionStatus().failures.empty(), "and plays again once it is installed again");
        }
        // Device G: a broken build in the store, where the Performance last found it, opens as a
        // stand-in, which plays nothing: the build can be removed.
        {
            device(temp.path / "g");
            Session g(48000, 64);
            g.installArchive(archive.string(), false, {}, true);
            const auto plugins = temp.path / "g" / "Plugins" / "clap";
            const auto stored = fs::directory_iterator(plugins)->path() / platform() / fs::path(fixture).filename();
            std::ofstream(stored, std::ios::binary | std::ios::trunc) << "not a plugin";
            const auto performance = items(g, "performance").at(0);
            const auto doc = fs::path(performance.path) / "performance.json";
            auto text = read(doc);
            for(auto at = text.find(fixture); at != std::string::npos; at = text.find(fixture, at + 1)) text.replace(at, std::strlen(fixture), stored.string());
            std::ofstream(doc, std::ios::binary | std::ios::trunc) << text;
            g.libraryRescan();
            g.loadPerformanceFromLibrary(performance.id);
            check(g.sessionStatus().failures.size() == 1 && !g.installedPlugins().at(0).open, "a stand-in for the store's broken build does not play it");
            const auto plugin = g.installedPlugins().at(0);
            g.removePlugin(plugin.format, plugin.id, plugin.version);
            check(g.installedPlugins().empty(), "so it is removed");
        }
        // Device H (RFC 0006): a plugin author signs a build with oscairn-sign; an export carries
        // that signature, and signs its items and the whole archive with a key of the user's.
        const auto signedArchive = temp.path / "Signed.olib";
        std::string authorKey, version;
        {
            device(temp.path / "h");
            const auto author = temp.path / "author.key";
            const auto made = run(tool + " key " + shell(author));
            authorKey = signing::loadKey(author).publicKey;
            check(made.first == 0 && made.second.find(signing::keyId(authorKey)) != std::string::npos && made.second.find(authorKey) != std::string::npos, "oscairn-sign makes a key and shows its ID and public key");
            const auto build = temp.path / "h" / "Synth.clap";
            fs::copy_file(fixture, build);
            Session h(48000, 64);
            h.add("synth", build.string(), "org.kick.test.synth"); h.commit();
            version = h.graphState().topology.nodes.at(0).plugin.version;
            check(run(tool + " build " + shell(author) + " clap org.kick.test.synth " + shell(version) + " " + platform() + " " + shell(build)).first == 0 &&
                  signing::readSignatures(build.string() + ".sig").size() == 1, "oscairn-sign signs a build, beside it");
            const auto mine = h.makeSigningKey("mine");
            struct stat st{};
            check(::stat((temp.path / "h" / "Keys" / "mine.key").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "a key made in the data folder is readable only by the user");
            check(h.signingKeys().size() == 1 && h.signingKeys()[0].id == mine.id && mine.id == signing::keyId(mine.key), "the data folder's keys are listed by name, ID and public key");
            bool twice = false; try { h.makeSigningKey("mine"); } catch(const std::exception&) { twice = true; }
            check(twice, "a key is not made over another");
            h.savePerformanceToLibrary("Signed");
            check(h.exportPerformance(signedArchive.string(), false, {"mine", true, true}).empty(), "a signed export leaves nothing out");
            const auto packed = readPackage(signedArchive, temp.path / "h-out");
            check(packed.signature.key == mine.key && !packed.items.empty() && packed.items[0].signatures.size() == 1 && packed.items[0].signatures[0].key == mine.key,
                  "the export signs its items and the whole archive with the chosen key");
            check(packed.plugins.size() == 1 && packed.plugins[0].builds.size() == 1 && packed.plugins[0].builds[0].signatures.size() == 1 &&
                  packed.plugins[0].builds[0].signatures[0].key == authorKey, "the export carries the build's signature from beside it");
            h.exportPerformance((temp.path / "Plain.olib").string());
            const auto plain = readPackage(temp.path / "Plain.olib", {});
            check(plain.signature.key.empty() && plain.items[0].signatures.empty(), "an export is signed only when the exporter chooses");
            bool unnamed = false; try { h.exportPerformance((temp.path / "None.olib").string(), false, {"nobody", false, true}); } catch(const std::exception&) { unnamed = true; }
            check(unnamed && !fs::exists(temp.path / "None.olib"), "an export signed with a key that is not there is refused");
            // oscairn-sign: an archive signed afterwards, and its signatures checked.
            check(run(tool + " archive " + shell(author) + " " + shell(temp.path / "Plain.olib")).first == 0 && readPackage(temp.path / "Plain.olib", {}).signature.key == authorKey,
                  "oscairn-sign signs an archive whole");
            const auto verified = run(tool + " verify " + shell(signedArchive));
            check(verified.first == 0 && verified.second.find("archive " + packed.id + " " + mine.id) != std::string::npos &&
                  verified.second.find("build clap org.kick.test.synth " + version + " " + platform() + " " + signing::keyId(authorKey)) != std::string::npos,
                  "oscairn-sign says who signed what: " + verified.second);
            // --into: unpacked to check on a file system of the caller's, and cleaned after.
            const auto into = temp.path / "h" / "verify-into";
            const auto local = run(tool + " verify --into " + shell(into) + " " + shell(signedArchive));
            check(local.first == 0 && local.second == verified.second && !fs::exists(into), "oscairn-sign verifies into a folder of the caller's");
            bool refusedInto = false;
            fs::create_directories(into);
            refusedInto = run(tool + " verify --into " + shell(into) + " " + shell(signedArchive)).first != 0;
            check(refusedInto, "a --into folder that exists already is refused");
            fs::remove(into);
            // The manifest's name changed under the archive's signature, every other entry as it was.
            {
                mz_zip_archive in{}, out{};
                mz_zip_reader_init_file(&in, signedArchive.c_str(), 0);
                mz_zip_writer_init_file(&out, (temp.path / "Changed.olib").c_str(), 0);
                for(mz_uint i = 0; i < mz_zip_reader_get_num_files(&in); ++i)
                {
                    mz_zip_archive_file_stat st; mz_zip_reader_file_stat(&in, i, &st);
                    if(std::string(st.m_filename) != "library.json") { mz_zip_writer_add_from_zip_reader(&out, &in, i); continue; }
                    std::string m(size_t(st.m_uncomp_size), '\0'); mz_zip_reader_extract_to_mem(&in, i, m.data(), m.size(), 0);
                    m.replace(m.find("\"Signed\""), 8, "\"Signer\"");
                    mz_zip_writer_add_mem(&out, "library.json", m.data(), m.size(), MZ_DEFAULT_COMPRESSION);
                }
                mz_zip_writer_finalize_archive(&out); mz_zip_writer_end(&out); mz_zip_reader_end(&in);
            }
            check(run(tool + " verify " + shell(temp.path / "Changed.olib")).first != 0, "oscairn-sign refuses an archive changed after it was signed");
            h.exportPerformance((temp.path / "Unsigned.olib").string());
        }
        // Device I: the build's publisher unknown, then trusted for plugins; data installs whether signed or not.
        {
            device(temp.path / "i");
            Session i(48000, 64);
            const auto unknown = i.archiveContents(signedArchive.string());
            check(unknown.code.size() == 1 && unknown.code[0].status == "unknown" && unknown.code[0].key == signing::keyId(authorKey) && unknown.signer.status == "unknown",
                  "a build signed by a key not trusted here is unknown");
            std::string why; try { i.installArchive(signedArchive.string(), false); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("by an unknown key " + signing::keyId(authorKey)) != std::string::npos, "and refused without a yes: " + why);
            i.setTrustedKey({"", authorKey, "Author", {"plugins"}});
            const auto trusted = i.archiveContents(signedArchive.string());
            check(trusted.code.at(0).status == "trusted" && trusted.code[0].keyName == "Author", "once its key is trusted for plugins, it is trusted");
            const auto installed = i.installArchive(signedArchive.string(), false);
            check(installed.added == 1 && installed.notes.empty() && installed.code.at(0).status == "trusted", "and installs without asking");
            check(signing::readSignatures(temp.path / "i" / "Plugins" / "clap" / "org.kick.test.synth@1" / platform() / "Synth.clap.sig").size() == 1,
                  "the store keeps the build's signature beside it");
            check(i.exportLibraryItem(items(i, "performance").at(0).id, (temp.path / "Again.signed.olib").string()).empty() &&
                  readPackage(temp.path / "Again.signed.olib", {}).plugins.at(0).builds.at(0).signatures.at(0).key == authorKey, "a re-export carries the signature from the store");
            const auto plain = i.archiveContents((temp.path / "Unsigned.olib").string());
            check(plain.code.empty() && plain.signer.status == "unsigned", "an unsigned archive whose build is here brings no code");
            check(i.installArchive((temp.path / "Unsigned.olib").string(), true).added == 1, "unsigned data installs without asking");
        }
        // Device J: the publisher's key revoked: its build is never installed, the items still are.
        {
            device(temp.path / "j");
            Session j(48000, 64);
            j.setTrustedKey({"", authorKey, "Author", {"plugins"}, true});
            check(j.archiveContents(signedArchive.string()).code.at(0).status == "revoked", "a revoked key's build is said to be revoked");
            const auto installed = j.installArchive(signedArchive.string(), false, {}, true);
            check(installed.added == 1 && installed.code.at(0).status == "revoked" && installed.notes.size() == 1 && installed.notes[0].find("revoked") != std::string::npos &&
                  !fs::exists(temp.path / "j" / "Plugins"), "a revoked key's build is refused even with a yes, and noted");
        }
        // A build path without its .clap or .lv2: the store keeps bundles by extension, so such a
        // build installs into the store and is never found again. readPackage refuses it.
        {
            const auto build = temp.path / "bare" / "Plugins" / "clap" / "x@1" / platform() / "x";
            fs::create_directories(build.parent_path());
            fs::copy_file(fixture, build);
            Package p; p.id = "bare"; p.name = "Bare";
            p.plugins.push_back({"clap", "x", "1", "X", {{platform(), "Plugins/clap/x@1/" + platform() + "/x", bundleDigest(build), {}}}});
            const auto bare = temp.path / "Bare.olib";
            writePackage(bare, p, temp.path / "bare");
            std::string why;
            try { readPackage(bare, temp.path / "bare-out"); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("not a .clap or .lv2 bundle") != std::string::npos && !fs::exists(temp.path / "bare-out"),
                  "a build that is a bare file, not a .clap or .lv2 bundle, is refused: " + why);
        }
        // Device K: an item whose ID needs escaping in JSON (an `"`). Installing into the user's own
        // storage rewrites a Performance's part pointers as text, and an exported item's ID is its
        // package's, so such an ID is refused, installing nothing.
        {
            device(temp.path / "k");
            const auto root = temp.path / "k-root";
            fs::create_directories(root / "Sounds" / "esc");
            fs::create_directories(root / "Performances" / "esc-perf");
            std::ofstream(root / "Sounds" / "esc" / "sound.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.sound","version":8,"info":{"id":"ab\"cd","name":"Bass","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")"
                << std::string(64, '0') << R"("},"nodes":[]})";
            std::ofstream(root / "Performances" / "esc-perf" / "performance.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.performance","version":13,"info":{"id":"perf","name":"Set","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")"
                << std::string(64, '0') << R"("},"nodes":[{"key":"n1","plugin":{"format":"clap","id":"x","version":"1","path":"p"},"sound":"ab\"cd"}]})";
            Package p; p.id = "k.esc"; p.name = "Escaped";
            p.items.push_back({"sound", "ab\"cd", "Bass", "Sounds/esc", {}});
            p.items.push_back({"performance", "perf", "Set", "Performances/esc-perf", {}});
            const auto escaped = temp.path / "Escaped.olib";
            writePackage(escaped, p, root);
            Session k(48000, 64);
            std::string why; try { k.installArchive(escaped.string(), false); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("an item's kind or ID is wrong") != std::string::npos && items(k, "sound").empty() && items(k, "performance").empty(),
                  "an archive whose Sound's ID needs escaping is refused, installing nothing: " + why);
        }
        // A reverse-DNS library ID (RFC 0031's normal case) installs as a library: the folder is
        // the ID itself, so rescan lists it, its items show up, and it can be uninstalled.
        {
            device(temp.path / "l");
            const auto root = temp.path / "l-root";
            fs::create_directories(root / "Sounds" / "tone");
            std::ofstream(root / "Sounds" / "tone" / "sound.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.sound","version":8,"info":{"id":"tone","name":"Tone","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")"
                << std::string(64, '0') << R"("},"nodes":[]})";
            Package p; p.id = "com.example.pack"; p.name = "Dotted";
            p.items.push_back({"sound", "tone", "Tone", "Sounds/tone", {}});
            const auto dotted = temp.path / "Dotted.olib";
            writePackage(dotted, p, root);
            Session l(48000, 64);
            check(l.installArchive(dotted.string(), true).added == 1, "a dotted ID installs as a library");
            check(l.libraries().size() == 1 && l.libraries()[0].id == "com.example.pack" && l.libraries()[0].items == 1,
                  "and is listed, under its own ID: " + std::to_string(l.libraries().size()));
            Control::LibraryQuery inLibrary; inLibrary.source = "com.example.pack";
            check(l.libraryQuery(inLibrary).total == 1, "its items can be queried");
            check(fs::is_directory(temp.path / "l" / "Libraries" / "com.example.pack"), "the folder is the ID, dots and all");
            l.uninstallLibrary("com.example.pack");
            check(l.libraries().empty() && !fs::exists(temp.path / "l" / "Libraries" / "com.example.pack"), "and it is uninstalled");
            // An ID that differs only in case: on a case-insensitive file system (macOS, Windows)
            // it names the same folder, so it is refused there, never mixed into the other library.
            // On a case-sensitive one both install, and which lists the shared item is the folder order.
            check(l.installArchive(dotted.string(), true).added == 1, "the dotted library installs again");
            Package upper = p; upper.id = "Com.Example.Pack";
            const auto cased = temp.path / "Cased.olib";
            writePackage(cased, upper, root);
            std::string why; try { l.installArchive(cased.string(), true); } catch(const std::exception& e) { why = e.what(); }
            const auto listed = l.libraries();
            const auto items = [&](const char* id) { for(const auto& x : listed) if(x.id == id) return int(x.items); return -1; };
            const bool refused = why.find("another library") != std::string::npos;
            check(refused ? listed.size() == 1 && items("com.example.pack") == 1
                          : listed.size() == 2 && items("com.example.pack") >= 0 && items("Com.Example.Pack") >= 0 && items("com.example.pack") + items("Com.Example.Pack") == 1,
                  "an ID differing only in case is refused or installed apart, the first library still listed: " + why + ", "
                      + std::to_string(listed.size()) + " listed");
            l.uninstallLibrary("com.example.pack");
        }
        // An install cut short by a failed copy leaves nothing half behind: the item folders this
        // call made come back out, as the unpacked folder does, and a library whose install never
        // finished is not listed. What was there before stays, a link included.
        {
            device(temp.path / "m");
            const auto root = temp.path / "m-root";
            fs::create_directories(root / "Sounds" / "one");
            fs::create_directories(root / "Performances" / "two");
            std::ofstream(root / "Sounds" / "one" / "sound.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.sound","version":8,"info":{"id":"one","name":"One","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")"
                << std::string(64, '0') << R"("},"nodes":[]})";
            std::ofstream(root / "Performances" / "two" / "performance.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.performance","version":13,"info":{"id":"two","name":"Two","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")"
                << std::string(64, '0') << R"("},"nodes":[]})";
            Package p; p.id = "m.cut"; p.name = "Cut";
            p.items.push_back({"sound", "one", "One", "Sounds/one", {}});
            p.items.push_back({"performance", "two", "Two", "Performances/two", {}});
            const auto cut = temp.path / "Cut.olib";
            writePackage(cut, p, root);
            Session m(48000, 64);
            const auto home = temp.path / "m";
            const auto library = home / "Libraries" / "m.cut";
            // A copy that fails partway, as a full disk makes it: the item's folder is made, then
            // nothing can be added to it (an inherited macOS ACL; root is not held by it).
#ifdef __APPLE__
            if(::geteuid() != 0)
            {
                auto failCopiesInto = [&](const fs::path& folder) { fs::create_directories(folder); return run("chmod +a 'everyone deny add_file,add_subdirectory,file_inherit,directory_inherit,only_inherit' " + shell(folder)).first == 0; };
                auto clear = [&](const fs::path& folder) { run("chmod -N " + shell(folder)); };
                check(failCopiesInto(home / "Sounds"), "an ACL makes copies into the user's Sounds fail");
                bool threw = false;
                try { m.installArchive(cut.string(), false); } catch(const std::exception&) { threw = true; }
                check(threw && !fs::exists(home / "Sounds" / "One.osound") && items(m, "sound").empty() && items(m, "performance").empty(),
                      "an own-storage install whose copy fails partway leaves no half-copied item behind");
                clear(home / "Sounds");
                // Into a library folder that is there already, so it is not removed whole.
                check(failCopiesInto(library / "Sounds"), "an ACL makes copies into the library's Sounds fail");
                threw = false;
                try { m.installArchive(cut.string(), true); } catch(const std::exception&) { threw = true; }
                check(threw && fs::is_directory(library) && !fs::exists(library / "Sounds" / "one") && !fs::exists(library / "library.json") && m.libraries().empty(),
                      "a library install whose copy fails partway leaves no half-copied item and no listed library");
                clear(library / "Sounds");
            }
            else std::puts("skipped: a copy that fails partway (an ACL does not hold root)");
#else
            std::puts("skipped: a copy that fails partway (made with a macOS ACL)");
#endif
            // A link where a new item would go is not overwritten, followed or removed.
            fs::create_directories(home / "Performances");
            fs::create_symlink(temp.path / "nowhere", home / "Performances" / "Two.oscairn");
            check(m.installArchive(cut.string(), false).added == 2 && fs::is_symlink(home / "Performances" / "Two.oscairn") && items(m, "performance").size() == 1,
                  "an own-storage install goes beside a dangling link, keeping it");
            fs::create_directories(library / "Performances");
            fs::create_symlink(temp.path / "nowhere", library / "Performances" / "two");
            const auto linked = m.installArchive(cut.string(), true);
            check(linked.added == 1 && linked.skipped == 1 && fs::is_symlink(library / "Performances" / "two") && m.libraries().size() == 1,
                  "a library install keeps a link where an item goes, as what is there");
            fs::remove(library / "Performances" / "two");
            const auto rest = m.installArchive(cut.string(), true); // once the way is clear, it installs whole
            check(m.libraries().size() == 1 && m.libraries()[0].items == 2 && rest.added == 1,
                  "and the missing item comes back once the way is clear");
            // A manifest whose move in fails leaves no temporary file behind.
            fs::create_directories(home / "blocked" / "f.json" / "inside");
            bool threw = false;
            try { writeAtomically(home / "blocked" / "f.json", "{}"); } catch(const std::exception&) { threw = true; }
            check(threw && !fs::exists(home / "blocked" / ".f.json.tmp"), "a failed atomic write removes its temporary file");
        }
        // A skipped Sound whose copy's ID is the archive's own (a copy edited by hand, or synced):
        // the part-pointer rewrite replaces a needle with itself and must still end.
        {
            device(temp.path / "n");
            const auto root = temp.path / "n-root";
            fs::create_directories(root / "Sounds" / "same");
            fs::create_directories(root / "Performances" / "set");
            const std::string zeros(64, '0');
            std::ofstream(root / "Sounds" / "same" / "sound.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.sound","version":8,"info":{"id":"same","name":"Same","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")" << zeros << R"("},"nodes":[]})";
            std::ofstream(root / "Performances" / "set" / "performance.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.performance","version":13,"info":{"id":"set","name":"Set","description":"","tags":[],"color":"","revision":"r","parent":"","origin":null,"hash":")" << zeros
                << R"("},"nodes":[{"key":"n1","plugin":{"format":"clap","id":"x","version":"1","path":"p"},"sound":"same"}]})";
            Package p; p.id = "n.same"; p.name = "Same";
            p.items.push_back({"sound", "same", "Same", "Sounds/same", {}});
            p.items.push_back({"performance", "set", "Set", "Performances/set", {}});
            const auto same = temp.path / "Same.olib";
            writePackage(same, p, root);
            fs::create_directories(temp.path / "n" / "Sounds" / "Same.osound");
            std::ofstream(temp.path / "n" / "Sounds" / "Same.osound" / "sound.json", std::ios::binary | std::ios::trunc)
                << R"({"format":"oscairn.sound","version":8,"info":{"id":"same","name":"Same","description":"","tags":[],"color":"","revision":"r","parent":"","origin":{"library":"n.same","item":"same","hash":")" << zeros
                << R"("},"hash":")" << zeros << R"("},"nodes":[]})";
            Session n(48000, 64);
            check(items(n, "sound").size() == 1 && items(n, "sound")[0].id == "same", "the user's own copy is there under the archive's ID");
            const auto installed = n.installArchive(same.string(), false);
            check(installed.added == 1 && installed.skipped == 1, "the Sound is skipped, the Performance added");
            const auto doc = fs::path(items(n, "performance").at(0).path) / "performance.json";
            check(read(doc).find("\"sound\":\"same\"") != std::string::npos, "and its part points at the copy, whose ID is the archive's");
        }
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    if(!failures) std::puts("ok: export, install as the user's own and as a library, reinstall, uninstall, chosen items and plugins from the store");
    return failures ? 1 : 0;
}

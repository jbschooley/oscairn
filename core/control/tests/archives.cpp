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
//   oscairn-archives-test FIXTURE.clap
#include "oscairn/session.h"
#include "oscairn/package.h"
#include "oscairn/wav_file.h"
#include "../../plugin-host/src/StateFiles.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    std::string read(const fs::path& f) { std::ifstream in(f, std::ios::binary); std::ostringstream out; out << in.rdbuf(); return out.str(); }
    void device(const fs::path& home) { fs::create_directories(home); ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1); }
    std::vector<Control::LibraryItem> items(Session& s, const std::string& kind) { Control::LibraryQuery q; q.kind = kind; return s.libraryQuery(q).items; }
}

int main(int argc, char** argv)
{
    if(argc < 2) { std::fprintf(stderr, "usage: %s FIXTURE\n", argv[0]); return 2; }
    const char* fixture = argv[1];
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
            const auto installed = b.installArchive(archive.string(), false);
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
            const auto installed = c.installArchive(archive.string(), false, {soundId});
            check(installed.added == 1 && items(c, "performance").empty() && items(c, "sound").size() == 1, "only the chosen Sound");
            check(c.installArchive(soundArchive.string(), false).added == 1 && items(c, "sound").size() == 2, "an exported Sound installs as the user's own");
            bool refused = false; try { c.installArchive(archive.string(), false, {"nothing"}); } catch(const std::exception&) { refused = true; }
            check(refused, "an item the archive does not have is refused");
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
            d.installArchive(archive.string(), false);
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
            d.installArchive(archive.string(), false);
            d.loadPerformanceFromLibrary(performance.id);
            check(d.sessionStatus().failures.empty(), "and plays again once it is installed again");
        }
        // Device G: a broken build in the store, where the Performance last found it, opens as a
        // stand-in, which plays nothing: the build can be removed.
        {
            device(temp.path / "g");
            Session g(48000, 64);
            g.installArchive(archive.string(), false);
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
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    if(!failures) std::puts("ok: export, install as the user's own and as a library, reinstall, uninstall, chosen items and plugins from the store");
    return failures ? 1 : 0;
}

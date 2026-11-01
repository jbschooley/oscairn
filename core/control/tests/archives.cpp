// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021's libraries and exports, across devices (each its own OSCAIRN_DATA_HOME): an exported
// Performance carries its unedited part's Sound and the samples they use; installed into the
// user's own storage it gets new IDs, its part pointing at the Sound's copy, and installing again
// skips what is unchanged; installed as a library it is locked, comes back when deleted and goes
// when uninstalled, leaving the user's copies and the samples.
//   oscairn-archives-test FIXTURE.clap
#include "oscairn/session.h"
#include "oscairn/wav_file.h"
#include "../../plugin-host/src/StateFiles.h"
#include <cstdio>
#include <cstdlib>
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
            a.exportPerformance(archive.string());
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
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    if(!failures) std::puts("ok: export, install as the user's own and as a library, reinstall, uninstall and chosen items");
    return failures ? 1 : 0;
}

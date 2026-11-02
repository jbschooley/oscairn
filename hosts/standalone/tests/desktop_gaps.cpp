// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Desktop controls for what was reachable only through the control API, against a real engine
// session over the control socket, without an audio device or any MIDI device. With SNAPSHOTS,
// each new view is written to SNAPSHOTS-<name>.png.
// - A part's MIDI out on the Part page, and clock out beside the transport (RFC 0012).
// - Signing an export, its signer, and the keys in Settings (RFC 0006).
// - Choosing a controller's profile (RFC 0024).
// - Asking before an app opens (RFC 0025).
//   oscairn_desktop_gaps_test PLUGIN_DIR SCANNER THEMES [SNAPSHOTS]
#include "../DesktopShell.h"
#include "../Launcher.h"
#include "../LibraryPane.h"
#include "../PartsView.h"
#include "../Settings.h"
#include "../Setlist.h"
#include "oscairn/local_socket.h"
#include "oscairn/package.h"
#include "oscairn/session.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 4) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    const auto socket = std::filesystem::temp_directory_path() / ("oscairn-gaps-" + std::to_string(::getpid()) + ".sock");
    // A data folder of its own, so the user's library and keys are never read or written.
    const auto data = juce::File::createTempFile("oscairn-gaps-data");
    ::setenv("OSCAIRN_DATA_HOME", data.getFullPathName().toRawUTF8(), 1);
    {
        auto session = std::make_unique<Session>(48000, 128);
        // Endpoints described to the session only: no device is opened.
        const MidiEndpoint modx{MidiEndpoint::Kind::Usb, 1, "MODX M"};
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, 3, "KeyLab 61"}}; status.midiOutputs = {modx};
        session->setHostStatus(status);
        auto server = std::make_unique<LocalServer>(*session);
        server->listen(socket.string());
        std::atomic<bool> running = true, holdRequests = false, jobHeld = false, forcedHold = false;
        std::thread control([&] {
            std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
            while(running)
            {
                server->poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                session->service();
                session->process(nullptr, 0, out, 2, 128);
                // The test leaves a job in flight: while its own connection shows, stop
                // answering, so its calls wait, as a long engine call does. A job asked behind
                // a refresh still gets by: the hold lasts two seconds and then serves one tick,
                // which is longer than any call's wait.
                const auto since = std::chrono::steady_clock::now();
                while(running && (forcedHold.load() || (holdRequests.load() && server->clients() >= 2 && std::chrono::steady_clock::now() - since < std::chrono::milliseconds(2000))))
                    { jobHeld = true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
                jobHeld = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            server.reset(); session.reset();
        });
        {
            standalone::Engine engine(argv[1], socket.string());
            const auto configuration = juce::File::createTempFile("oscairn-gaps-config");
            standalone::Theme theme(configuration, juce::File(argv[3]));
            juce::LookAndFeel::setDefaultLookAndFeel(&theme);
            theme.apply("oscairn-dark");
            auto settle = [] { for(int i = 0; i < 30; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); };
            auto wait = [](auto done) { for(int i = 0; i < 1000 && !done(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); return done(); };
            auto snapshot = [&](juce::Component& c, const juce::String& name) {
                if(argc < 5) return;
                const juce::File out{juce::String(argv[4]) + "-" + name + ".png"};
                out.deleteFile();
                juce::FileOutputStream stream(out);
                juce::PNGImageFormat().writeImageToStream(c.createComponentSnapshot(c.getLocalBounds()), stream);
            };
            const auto plugins = standalone::bundledPlugins(argv[1], argv[2]);
            uint32_t lead = 0;
            engine.edit([&](Control& c) {
                lead = standalone::addPart(c, "Lead", false);
                for(const auto& p : plugins) if(p.name == "Basic Synth") standalone::insertPlugin(c, lead, 0, p);
            });
            check(wait([&] { return engine.graph().parts.size() == 1; }) && engine.editError().empty(), "a part to send from: " + engine.editError());
            auto midiOut = [&] { for(const auto& p : engine.graph().parts) if(p.id == lead) return p.midiOut; return PartMidiOut{}; };

            // MIDI out on the Part page (RFC 0012 step 1): its outputs, then its channel.
            {
                standalone::PartsView view(engine, argv[2]);
                standalone::setDesktopMode(view, 1); view.setEmbedded(true); view.setSize(1180, 820);
                view.showPart(lead);
                juce::ComboBox* channel = nullptr;
                juce::TextButton* outputs = nullptr;
                auto channelBox = [&] { channel = nullptr; for(auto* c : view.getChildren()) if(auto* b = dynamic_cast<juce::ComboBox*>(c); b && b->getName() == "MIDI out channel") channel = b; return channel != nullptr; };
                auto outputsButton = [&] { outputs = nullptr; for(auto* c : view.getChildren()) if(auto* b = dynamic_cast<juce::TextButton*>(c); b && b->getTooltip().contains("MIDI outputs")) outputs = b; return outputs != nullptr; };
                check(channelBox() && outputsButton() && outputs->getButtonText() == "Off" && !channel->isEnabled(), "the Part page shows MIDI out, off");
                engine.edit([&](Control& c) { standalone::setMidiOut(c, lead, {standalone::toggled({}, standalone::ruleFor(modx)), 0}); });
                check(wait([&] { view.refresh(); return outputsButton() && outputs->getButtonText() == "MODX M"; }), "an output chosen shows by name");
                check(channelBox() && channel->isEnabled() && channel->getSelectedId() == 1, "and its channel can be set, each message's own at first");
                if(channel) channel->setSelectedId(4, juce::sendNotificationSync);
                check(wait([&] { return midiOut().channel == 3; }) && midiOut().devices.size() == 1, "the channel menu sets channel 3");
                view.refresh(); settle();
                snapshot(view, "part-midi-out");
            }
            // Clock out beside the transport (RFC 0012 step 2): cabling, kept by the engine.
            {
                standalone::SequencerPane pane(engine);
                standalone::setDesktopMode(pane, 1); pane.setSize(1100, 760);
                pane.refresh();
                check(pane.clockOut().isVisible() && pane.clockOut().getButtonText() == "Clock out: Off", "clock out shows beside Follow MIDI clock, off: " + pane.clockOut().getButtonText().toStdString());
                engine.setClockOutputs({standalone::ruleFor(modx)});
                check(wait([&] { pane.refresh(); return pane.clockOut().getButtonText() == "Clock out: MODX M"; }), "an output chosen for clock shows by name");
                settle();
                snapshot(pane, "clock-out");
                engine.setClockOutputs({});
            }
            // Signing on export (RFC 0006): off by default; a key made from the export's choices,
            // chosen, signs the archive, whose signer the install dialog then names.
            {
                standalone::ExportChoices choices(engine, true);
                standalone::setDesktopMode(choices, 1);
                check(choices.key().empty() && choices.signWith.getSelectedId() == 1 && choices.signWith.getNumItems() == 2 && choices.signWith.getItemText(1) == "Make a key...",
                      "signing is off at first, with Make a key... offered");
                Control::SigningKey made;
                check(engine.makeSigningKey("stage", made).empty() && made.id.size() == 16, "a key made");
                choices.chooseKey("stage");
                check(choices.key() == "stage" && choices.signWith.getNumItems() == 3 && choices.signWith.getText() == "Sign with stage", "listed, and chosen once made");
                snapshot(choices, "export-sign");
                auto exportTo = [&](const char* name, const std::string& key) {
                    const auto file = data.getChildFile(name);
                    standalone::SoundAction out; out.kind = standalone::SoundAction::ExportPerformance; out.directory = file.getFullPathName().toStdString(); out.signKey = key;
                    engine.soundAction(std::move(out));
                    Control::Archive archive;
                    check(wait([&] { return !engine.soundBusy(); }) && file.existsAsFile() && engine.archiveContents(file.getFullPathName().toStdString(), archive).empty(), std::string("exported: ") + engine.soundMessage());
                    return archive;
                };
                const auto plain = exportTo("Plain.olib", {}), signedArchive = exportTo("Signed.olib", choices.key());
                check(plain.signer.status == "unsigned" && standalone::describeSigner(plain.signer) == "Not signed", "unsigned unless a key is chosen");
                check(signedArchive.signer.status == "unknown" && signedArchive.signer.key == made.id && engine.soundMessage().find("Signed with stage") != std::string::npos,
                      "signed with the key chosen: " + signedArchive.signer.status + " " + engine.soundMessage());
                const auto said = standalone::describeSigner(signedArchive.signer);
                check(said.contains(standalone::fingerprint(made.id)) && said.contains("not a key you trust"), "the install dialog names its signer by fingerprint: " + said.toStdString());
                // The keys in Settings: names and fingerprints, never a private key.
                standalone::Settings settings(engine);
                standalone::setDesktopMode(settings, 1); settings.setSize(640, 560);
                check(settings.keys().size() == 1 && settings.keys()[0].name == "stage" && settings.keys()[0].key.size() == 64 && standalone::fingerprint(made.id).length() == 19,
                      "Settings lists the key by name and fingerprint");
                snapshot(settings, "settings-keys");
            }
            // Choosing a controller's profile (RFC 0024): two profiles fit the SMK37's port, stock
            // and custom firmware; the first by id until the musician chooses. Inputs are names
            // only: no MIDI device is opened.
            {
                auto profiles = standalone::controllerProfiles();
                std::string stock; for(const auto& [id, json] : standalone::builtInProfiles()) if(id == "smk37-elite") stock = json;
                profiles.push_back(standalone::Profile::parse(juce::String(stock).replace("\"smk37-elite\"", "\"smk37-elite-custom\"").replace("(stock firmware)", "(custom firmware)").toStdString()));
                const std::vector<std::string> inputs{"IAC Bus 1", "SMK-37 Elite -Private", "SMK-37 Elite -Master", "SMK-37 Elite -MCP"};
                standalone::ProfileChoices kept;
                standalone::Settings settings(engine);
                standalone::setDesktopMode(settings, 1); settings.setSize(760, 640);
                settings.setControllers({profiles, [&] { return inputs; }, [&] { return kept; }, [&](const std::string& port, const std::string& id) { kept[port] = id; }});
                const auto& rows = settings.ports();
                auto profileName = [&](size_t row) { return rows.size() > row ? profiles[rows[row].chosen].name : std::string(); };
                check(rows.size() == 3 && rows[1].port == "SMK-37 Elite -Master" && rows[1].profiles.size() == 2 && profileName(1) == "SMK37 Elite (stock firmware)" && settings.profileBox(1)->isEnabled(),
                      "the SMK37's ports listed; two profiles fit -Master, the first by id in use");
                check(rows.size() == 3 && rows[0].note == "Ignored: a duplicate of another port" && rows[2].note == "Mackie Control" && rows[1].note == "Its controls join",
                      "the duplicate port shown as ignored");
                snapshot(settings, "settings-controllers");
                if(auto* box = settings.profileBox(1)) box->setSelectedId(2, juce::sendNotificationSync);
                check(kept["SMK-37 Elite -Master"] == "smk37-elite-custom", "choosing the custom profile keeps it for the port's name");
                check(wait([&] { return profileName(1) == "SMK37 Elite (custom firmware)"; }), "and it is in use");
            }
            // Asking before an app opens (RFC 0025): with memory classes, a launch past what is
            // free is refused with the reason; the prepared next entry in the way can go.
            {
                check(engine.launchApp({"oscairn.library", 120ull << 20}).admitted, "without memory classes every app opens");
                engine.appClosed("oscairn.library");
                // The protocol takes a reservation of at most 4 GiB, so a small device.
                constexpr uint64_t MiB = 1ull << 20;
                // The synth measured at 100 MiB, read when the budget is set, so the next song holds memory.
                // Its ID, version and build digest (Session::pluginKey): the graph over the socket does not carry the version.
                std::string key;
                for(const auto& p : plugins) if(p.name == "Basic Synth") key = "org.oscairn.basic-synth@0.1.0 " + bundleDigest(p.path);
                data.getChildFile("measurements.json").replaceWithText("{\"plugins\":{\"" + key + "\":{\"cpu\":0.01,\"rest\":0.01,\"voice\":0,\"voices\":0,\"memory\":" + std::to_string(100 * MiB) + "}}}");
                engine.edit([](Control& c) { c.setBudget({.9, 0, 3072 * MiB, 256 * MiB, 0, 64 * MiB}); });
                check(engine.launchApp({"oscairn.library", 120ull << 20}).admitted, "the Library fits");
                std::string path;
                engine.edit([&](Control& c) {
                    c.savePerformanceToLibrary("Next song", true);
                    Control::LibraryQuery q; q.text = "Next song";
                    for(const auto& item : c.libraryQuery(q).items) path = item.path;
                });
                Control::MemoryState before, after;
                engine.edit([&](Control& c) { before = c.memoryState(); c.preparePerformance(path); after = c.memoryState(); });
                const auto next = after.committed - before.committed;
                check(!path.empty() && after.nextCommitted && next > 0, "the next song prepared ahead, committed: " + std::to_string(after.committed) + "/" + std::to_string(before.committed) + " " + engine.editError());
                // Just too big beside the next song, and small enough without it.
                const uint64_t reserve = after.device - after.reserved - after.margin - after.committed + next / 2;
                std::unique_ptr<standalone::LaunchNotice> notice;
                bool retried = false;
                auto show = [&](std::unique_ptr<standalone::LaunchNotice> n) { notice = std::move(n); };
                check(!standalone::askToLaunch(engine, {"oscairn.groovebox", reserve}, "Groovebox", [&] { retried = true; }, show) && notice && notice->letGo.isVisible(),
                      "refused, with Let the next song go offered");
                if(notice)
                {
                    standalone::setDesktopMode(*notice, 2); notice->setSize(1024, 600);
                    check(notice->message().contains("letting the next entry"), "it says why: " + notice->message().toStdString());
                    snapshot(*notice, "launch-next-song");
                    notice->letGo.onClick();
                    check(wait([&] { return retried && !notice; }), "letting it go closes the notice and tries again");
                }
                Control::MemoryState now;
                engine.edit([&](Control& c) { now = c.memoryState(); });
                check(!now.nextPrepared && std::any_of(now.running.begin(), now.running.end(), [](const auto& a) { return a.app == "oscairn.groovebox"; }), "the next song let go, the Groovebox admitted");
                check(!standalone::askToLaunch(engine, {"oscairn.setlist", 3500 * MiB}, "Setlist", {}, show) && notice && !notice->letGo.isVisible() &&
                      notice->message().contains("close") && notice->message().contains("Library"), "refused past what closing frees, saying what to close: " + (notice ? notice->message().toStdString() : std::string()));
                if(notice) { standalone::setDesktopMode(*notice, 1); notice->setSize(1024, 600); snapshot(*notice, "launch-refused"); notice->ok.onClick(); wait([&] { return !notice; }); }
                engine.appClosed("oscairn.library");
                engine.edit([&](Control& c) { now = c.memoryState(); });
                check(std::none_of(now.running.begin(), now.running.end(), [](const auto& a) { return a.app == "oscairn.library"; }), "a closed app's reservation is let go");
                engine.edit([](Control& c) { c.setBudget({}); });
            }
            // A refresh in flight (finding: the periodic sound refresh every ~30 ticks) must not
            // make the engine busy: it reads on its own connection and makes only short calls,
            // so Record, Save and the library work while it runs.
            {
                holdRequests = true;
                standalone::SoundAction tick; tick.kind = standalone::SoundAction::Refresh;
                engine.soundAction(std::move(tick)); // the periodic refresh, or this one, next
                check(wait([&] { return jobHeld.load(); }), "a refresh job's own connection is in flight");
                check(engine.controlReady(), "a refresh in flight leaves the controls ready");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load(); }), "the refresh finished");
            }
            // The polled reads (meters, undo names, the open Performance's name) come from the
            // Engine's cache while a job holds the engine's thread, so the UI thread neither
            // waits on the socket nor drops the connection over a stalled request.
            {
                std::string probe;
                engine.edit([&](Control& c) {
                    c.savePerformanceToLibrary("Cache probe", true);
                    Control::LibraryQuery q; q.text = "Cache probe";
                    for(const auto& item : c.libraryQuery(q).items) probe = item.path;
                });
                check(wait([&] { return !probe.empty() && engine.editError().empty(); }), "a Performance to prepare: " + engine.editError());
                check(wait([&] { return engine.performanceInfo().has_value() && !engine.history().undo.empty(); }), "the reads are primed");
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = probe;
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a prepare job holds the engine's thread");
                const auto began = std::chrono::steady_clock::now();
                const auto info = engine.performanceInfo(); const auto undo = engine.history().undo; const auto meters = engine.meters();
                const auto waited = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - began).count();
                check(info && !undo.empty(), "the reads answer from the cache: " + std::to_string(meters.size()));
                check(engine.connected(), "and no call waits on the stalled socket");
                check(waited < 100000, "at once, not after a round trip: " + std::to_string(waited) + " us");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the prepare finished, the engine answers again");
                engine.edit([](Control& c) { c.addMacro("Cache probe control"); });
                check(wait([&] { return engine.history().undo.find("Cache probe control") != std::string::npos && engine.connected(); }), "the cache follows the engine again once free");
            }
            // An action asked while a job runs is refused, and said: a dropped Save must not
            // look saved.
            {
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a prepare of a missing folder holds the engine's thread");
                standalone::SoundAction save; save.kind = standalone::SoundAction::SaveToLibrary;
                engine.soundAction(std::move(save));
                check(engine.soundMessage().find("busy") != std::string::npos, "a Save dropped while busy says so: " + engine.soundMessage());
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
            }
            // While a job holds the engine's thread it cannot answer a launch ask either: the
            // shell admits at once, rather than freeze for the call's timeout and drop.
            {
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a prepare of a missing folder holds the engine's thread");
                const auto began = std::chrono::steady_clock::now();
                const bool opened = standalone::askToLaunch(engine, {"oscairn.library", 120ull << 20}, "Library", {}, {});
                const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
                check(opened && engine.connected(), "an app opens at once while a job runs");
                check(waited < 900, "the ask did not wait on the stalled socket: " + std::to_string(waited) + " ms");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the held job finished");
                engine.appClosed("oscairn.library");
            }
            // An editor asks for every value of a stepped parameter on every rebuild: the texts
            // are cached per value, so they answer while the next refresh round trips.
            {
                NodeId synth = 0;
                for(const auto& [id, n] : engine.graph().names) if(n == "Basic Synth") synth = id;
                const auto node = engine.graph().names.at(synth);
                check(synth != 0, "the Lead's Basic Synth node");
                check(wait([&] { return !engine.soundBusy(); }), "the engine is free to read texts");
                const auto saw = engine.paramText(node, 4, 0), detuned = engine.paramText(node, 4, 1), square = engine.paramText(node, 4, 2);
                check(saw == "Saw" && detuned == "Detuned saws" && square == "Square", "the oscillator names read once, every value kept");
                holdRequests = true;
                standalone::SoundAction prepare; prepare.kind = standalone::SoundAction::Prepare; prepare.directory = data.getChildFile("nothing").getFullPathName().toStdString();
                engine.soundAction(std::move(prepare));
                check(wait([&] { return engine.soundBusy() && !engine.controlReady() && jobHeld.load(); }), "a job runs while the editor rebuilds");
                check(engine.paramText(node, 4, 0) == saw && engine.paramText(node, 4, 1) == detuned && engine.paramText(node, 4, 2) == square,
                      "every value's text answers from the cache while the engine works");
                holdRequests = false;
                check(wait([&] { return !engine.soundBusy() && !jobHeld.load() && engine.connected(); }), "the job finished");
            }
            // A setlist marks entries "not in the library" once the mixed Sound+Performance read
            // of 1000 items passes them: entries are Performances, and the read asks for that
            // kind alone.
            {
                std::string lastId;
                engine.edit([&](Control& c) { lastId = c.savePerformanceToLibrary("Z last", true); });
                check(wait([&] { return !engine.soundBusy() && !lastId.empty(); }), "a Performance saved as the library's Z last");
                const auto lib = juce::File(::getenv("OSCAIRN_DATA_HOME"));
                const auto writeItem = [&](const char* kind, const std::string& folder, const std::string& id, const std::string& name) {
                    const auto at = lib.getChildFile(kind).getChildFile(folder);
                    at.createDirectory();
                    at.getChildFile(std::string(kind == "Sounds" ? "sound" : "performance") + ".json").replaceWithText(
                        "{\"format\":\"oscairn." + std::string(kind == "Sounds" ? "sound" : "performance") + "\",\"version\":" + std::string(kind == "Sounds" ? "7" : "11") +
                        ",\"info\":{\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"description\":\"\",\"tags\":[],\"color\":\"\",\"revision\":\"r\",\"parent\":\"\",\"hash\":\"h\",\"origin\":null}}");
                };
                for(int i = 0; i < 999; ++i) writeItem("Sounds", "Sound " + std::to_string(i), "sound-" + std::to_string(i), "Sound " + std::to_string(i));
                writeItem("Performances", "Z dummy.oscairn", "perf-dummy", "Z dummy"); // sorted just before Z last: past the 1000-item mixed read
                engine.edit([](Control& c) { c.libraryRescan(); });
                check(wait([&] { const auto all = engine.library({}); return all && all->total >= 1003; }), "the library read holds 1001 staged items");
                standalone::Actions actions;
                standalone::SetlistPane pane(engine, actions, data.getChildFile("Setlists-staged"));
                const std::string setlistName = "Staged";
                standalone::Setlist staged; staged.name = setlistName; staged.entries = {{lastId, "Z last", {}}, {"perf-gone", "Gone", {}}};
                standalone::writeSetlist(data.getChildFile("Setlists-staged"), staged);
                pane.open(standalone::setlistFile(data.getChildFile("Setlists-staged"), setlistName));
                pane.go(0);
                check(pane.current() == 0 && !pane.message().contains("no longer in the library"), "the last Performance opens: " + pane.message().toStdString());
                check(wait([&] { return !engine.soundBusy(); }), "it finished opening");
                pane.go(1);
                check(pane.current() == 0 && pane.message().contains("no longer in the library"), "a gone entry is still marked: " + pane.message().toStdString());
                check(wait([&] { return !engine.soundBusy(); }), "the staged setlist is done opening");
            }
            // A crash ends the desktop's every control with it; Home and the Performance app now
            // show the Panel's notice (and Restart where a restart is possible), and a setlist
            // entry does not pretend to be playing while the engine is gone.
            {
                // Stall the server from this thread: the engine's next periodic refresh cannot
                // finish its handshake, and its timeout marks the engine disconnected.
                forcedHold = true;
                check(wait([&] { return engine.jobRunning(); }), "the next refresh starts against the stalled server");
                check(wait([&] { return !engine.connected(); }), "its handshake times out; the engine reads as disconnected");
                const auto folder = data.getChildFile("Setlists-staged");
                standalone::Actions actions;
                standalone::SetlistPane pane(engine, actions, folder);
                check(pane.setlist().entries.size() == 2 && pane.current() == -1, "the staged setlist is open with nothing playing");
                pane.go(0);
                check(pane.current() == -1 && pane.message().contains("not answering"), "a setlist entry says it did not open: " + pane.message().toStdString());
                pane.step(1);
                check(pane.current() == -1, "and stepping does not move on from an entry that never opened");
                standalone::Launcher launcher(engine, {}, {});
                standalone::setDesktopMode(launcher, 1); launcher.setSize(900, 600); launcher.resized();
                launcher.recovery().update();
                check(launcher.recovery().isVisible() && launcher.recovery().notice().contains("Reconnecting"), "Home shows the disconnect notice");
                check(!launcher.recovery().restartShown(), "no Restart while the engine may come back (this engine is not managed)");
                forcedHold = false;
                for(int i = 0; i < 600 && !engine.connected(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); // the engine answers again
                check(engine.connected(), "the client reconnects");
                launcher.recovery().update();
                check(!launcher.recovery().isVisible(), "the notice goes when the engine is back");
            }
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
            configuration.deleteRecursively();
        }
        running = false;
        control.join();
    }
    std::filesystem::remove(socket);
    data.deleteRecursively();
    if(!failures) std::puts("ok: the desktop's controls for what the control API had first");
    return failures ? 1 : 0;
}

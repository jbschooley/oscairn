// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The desktop window (RFC 0019's Desktop surface) against a real engine session over the
// control socket, without an audio device: a small Performance (parts, a split, a bus, scenes,
// a control), then every tab renders in each built-in theme, and in Oscairn Dark in both input
// modes. With SNAPSHOTS, each render is written to SNAPSHOTS-<theme>-<input>-<tab>.png.
// Then a plugin's parameter pages (RFC 0019's generated editor): the Basic Synth's, and any
// PLUGIN.clap given after SNAPSHOTS, each written to SNAPSHOTS-params-<name>-<page>.png.
// Then the Library and Setlist apps against a library of its own: finding by text and tags, a
// pick from the Setlist answered by the Library, a send from the Library taken by the Setlist
// (RFC 0022), and the setlist opening its Performances in order.
//   oscairn_desktop_shell_test PLUGIN_DIR SCANNER THEMES [SNAPSHOTS [PLUGIN.clap...]]
#include "../DesktopShell.h"
#include "../LibraryPane.h"
#include "../Setlist.h"
#include "../Panel.h"
#include "../ParameterGrid.h"
#include "../ZoneEditor.h"
#include "oscairn/local_socket.h"
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
    const auto dir = std::filesystem::path(argv[1]);
    const auto socket = std::filesystem::temp_directory_path() / ("oscairn-desktop-" + std::to_string(::getpid()) + ".sock");
    // A library of its own, so the user's is never read or written.
    const auto data = juce::File::createTempFile("oscairn-shell-data");
    ::setenv("OSCAIRN_DATA_HOME", data.getFullPathName().toRawUTF8(), 1);
    {
        Session session(48000, 128);
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, 3, "KeyLab 61"}};
        session.setHostStatus(status);
        LocalServer server(session);
        server.listen(socket.string());
        std::atomic<bool> running = true, play = false;
        // Audio rendered on the same thread, so the meters move: once the Performance is made, a
        // chord held from the keys.
        std::thread control([&] {
            std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
            bool held = false;
            while(running)
            {
                server.poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                session.service();
                if(play && !held) { for(const int key : {36, 60, 64, 67, 79}) session.note(true, key, .8); held = true; }
                session.process(nullptr, 0, out, 2, 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        {
            standalone::Engine engine(dir.string(), socket.string());
            // The built-in themes, with a configuration folder of its own so no preference is touched.
            const auto configuration = juce::File::createTempFile("oscairn-shell-config");
            standalone::Theme theme(configuration, juce::File(argv[3]));
            juce::LookAndFeel::setDefaultLookAndFeel(&theme);
            const auto plugins = standalone::bundledPlugins(argv[1], argv[2]);
            auto plugin = [&](const char* name) { for(const auto& p : plugins) if(p.name == name) return p; throw std::runtime_error(std::string("missing ") + name); };
            uint32_t piano = 0, bass = 0;
            engine.edit([&](Control& c) {
                piano = standalone::addPart(c, "Piano", false);
                const auto strings = standalone::addPart(c, "Strings", false);
                bass = standalone::addPart(c, "Bass", false);
                const auto lead = standalone::addPart(c, "Lead", false), hall = standalone::addPart(c, "Hall", true);
                standalone::insertPlugin(c, piano, 0, plugin("Basic Synth")); standalone::insertPlugin(c, piano, 1, plugin("Delay"));
                standalone::insertPlugin(c, strings, 0, plugin("Basic Synth"));
                standalone::insertSplit(c, strings, 1, Split::Kind::LeftRight, plugins);
                standalone::insertPlugin(c, bass, 0, plugin("Transpose")); standalone::insertPlugin(c, bass, 1, plugin("Basic Synth"));
                standalone::insertPlugin(c, lead, 0, plugin("Basic Synth")); standalone::insertPlugin(c, hall, 0, plugin("Delay"));
                standalone::setZone(c, bass, 0, 47, -12); standalone::setZone(c, lead, 72, 127, 0);
                standalone::setDevices(c, lead, {{DeviceRule::Kind::UsbNamed, DeviceRule::Match::Equals, 0, "KeyLab 61"}});
                standalone::setSendLevel(c, piano, hall, .4); standalone::setSendLevel(c, strings, hall, .6);
                c.setPartLevel(piano, .85, false); c.setPartLevel(strings, .6, false); c.setPartLevel(lead, .7, true); c.commit();
                c.addScene("Verse"); c.addScene("Chorus");
                const auto knob = c.addMacro("Brightness");
                MacroTarget level; level.kind = MacroTarget::Kind::PartLevel; level.part = strings;
                c.setMacroTargets(knob, {level});
            });
            for(int i = 0; i < 500 && engine.graph().parts.size() < 5; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            play = true;
            const auto made = standalone::readParts(engine.graph());
            check(made.parts.size() == 4 && made.buses.size() == 1 && made.parts[0].sound == "Basic Synth" && made.buses[0].chain.size() == 1 && engine.editError().empty(),
                  "the Performance was made with its plugins: " + std::string(engine.editError()));

            const std::pair<const char*, standalone::InputMode> runs[] = {{"oscairn-dark", standalone::InputMode::Mouse}, {"oscairn-dark", standalone::InputMode::Touch},
                                                                          {"oscairn-light", standalone::InputMode::Mouse}, {"oscairn-classic", standalone::InputMode::Mouse},
                                                                          {"oscairn-block", standalone::InputMode::Mouse}};
            for(const auto& [themeId, mode] : runs)
            {
                theme.apply(themeId);
                check(standalone::themeData().id == themeId, std::string("the theme applied: ") + themeId);
                standalone::DesktopShell shell(engine, argv[2], mode);
                shell.setSize(1440, 900);
                for(int i = 0; i < 30; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); // the shell's first ticks
                // The meters: a value per channel of each part's sound.
                bool moving = false;
                for(int i = 0; i < 200 && !moving; ++i) { juce::MessageManager::getInstance()->runDispatchLoopUntil(10); for(const auto& m : engine.meters()) moving |= m.peak > 0; }
                check(moving, "the parts' meters move while notes play");
                for(const char* name : {"Part", "Mixer", "Routing", "Controls"})
                {
                    standalone::DesktopShell::Tab tab{};
                    check(standalone::DesktopShell::tabNamed(name, tab), std::string("a tab named ") + name);
                    shell.showTab(tab);
                    for(int i = 0; i < 30; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); // its meters' first readings
                    const auto image = shell.createComponentSnapshot(shell.getLocalBounds());
                    check(image.getWidth() == 1440 && image.getHeight() == 900, std::string("renders ") + name);
                    if(argc < 5) continue;
                    const juce::File out{juce::String(argv[4]) + "-" + juce::String(themeId).fromFirstOccurrenceOf("oscairn-", false, false) + "-" + (mode == standalone::InputMode::Touch ? "touch" : "mouse") + "-" + juce::String(name).toLowerCase() + ".png"};
                    out.deleteFile();
                    juce::FileOutputStream stream(out);
                    juce::PNGImageFormat().writeImageToStream(image, stream);
                }
            }
            // The Library and Setlist apps.
            {
                auto wait = [&](auto done) { for(int i = 0; i < 1000 && !done(); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); return done(); };
                auto snapshot = [&](juce::Component& c, const juce::String& name) {
                    if(argc < 5) return;
                    const juce::File out{juce::String(argv[4]) + "-" + name + ".png"};
                    out.deleteFile();
                    juce::FileOutputStream stream(out);
                    juce::PNGImageFormat().writeImageToStream(c.createComponentSnapshot(c.getLocalBounds()), stream);
                };
                theme.apply("oscairn-dark");
                engine.edit([](Control& c) {
                    c.setPerformanceInfo({"", "Verse Keys", "for the verse", {"keys", "live"}, "#3e63dd"});
                    c.savePerformanceToLibrary("");
                    c.setPerformanceInfo({"", "Verse Keys", "", {"strings", "pad", "live"}});
                    c.savePerformanceToLibrary("Big Strings", true);
                    c.setPerformanceInfo({"", "Big Strings", "with the bass split off", {"bass", "split"}, "#46a758"});
                    c.savePerformanceToLibrary("Bass Split", true);
                });
                check(engine.editError().empty(), "three Performances saved into the library: " + engine.editError());
                standalone::Actions actions;
                std::unique_ptr<standalone::LibraryPane> picker;
                std::unique_ptr<standalone::SetlistPane> setlist;
                actions.offer(standalone::pickPerformance, "Library", [&](const std::string&, standalone::Actions::Answer answer) {
                    picker = std::make_unique<standalone::LibraryPane>(engine, actions, standalone::LibraryPane::Mode::Pick);
                    picker->onPicked = std::move(answer);
                    standalone::setDesktopMode(*picker, 1); picker->setSize(760, 620);
                });
                actions.offer(standalone::sendPerformance, "Setlist", [&](const std::string& id, standalone::Actions::Answer answer) { answer(setlist->add(id).toStdString()); });

                standalone::LibraryPane library(engine, actions, standalone::LibraryPane::Mode::Play);
                standalone::setDesktopMode(library, 1); // as the app's window sets it
                library.setSize(900, 700);
                check(wait([&] { return library.items().size() == 3; }), "the library lists the three Performances");
                check(library.tags().size() == 6 && library.items()[0].name == "Bass Split", "sorted by name, with every tag counted");
                check(library.items()[0].color == "#46a758" && library.items()[2].color == "#3e63dd", "each with its color");
                snapshot(library, "library-mouse");
                standalone::setDesktopMode(library, 2); library.resized();
                snapshot(library, "library-touch");
                standalone::setDesktopMode(library, 1); library.resized();
                library.search("verse");
                check(wait([&] { return library.items().size() == 1; }) && library.items()[0].name == "Verse Keys", "found by its description");
                library.search("");
                library.toggleTag("live");
                check(wait([&] { return library.items().size() == 2; }), "a tag narrows the list");
                library.toggleTag("pad");
                check(wait([&] { return library.items().size() == 1; }) && library.items()[0].name == "Big Strings", "two tags narrow it to what has both");
                library.toggleTag("live"); library.toggleTag("pad");
                check(wait([&] { return library.items().size() == 3; }), "and clearing them shows all again");

                // From the Setlist, Add asks for a pick, which the Library answers.
                const auto folder = data.getChildFile("Setlists");
                setlist = std::make_unique<standalone::SetlistPane>(engine, actions, folder);
                standalone::setDesktopMode(*setlist, 1); setlist->setSize(700, 760);
                setlist->create("Friday");
                setlist->addFromLibrary();
                check(picker != nullptr, "Add opens the Library's picker");
                check(wait([&] { return picker->items().size() == 3; }), "the picker lists the library");
                snapshot(*picker, "library-pick");
                picker->choose(0);
                check(setlist->setlist().entries.size() == 1 && setlist->setlist().entries[0].name == "Bass Split", "the pick filled a slot: " + setlist->message().toStdString());
                // From the Library, Add to setlist sends to the Setlist.
                const auto verse = std::find_if(library.items().begin(), library.items().end(), [](const auto& i) { return i.name == "Verse Keys"; }) - library.items().begin();
                library.send(int(verse));
                check(setlist->setlist().entries.size() == 2 && library.message() == "Added to Friday", "the send was taken: " + library.message().toStdString());
                const auto saved = standalone::readSetlist(standalone::setlistFile(folder, "Friday"));
                check(saved.name == "Friday" && saved.entries.size() == 2 && saved.entries[1].name == "Verse Keys", "the setlist was saved");
                // Played in order: each opens its Performance.
                setlist->go(0);
                check(wait([&] { return !engine.soundBusy() && engine.performanceInfo() && engine.performanceInfo()->name == "Bass Split"; }), "the first entry opened");
                setlist->step(1);
                check(setlist->current() == 1 && wait([&] { return !engine.soundBusy() && engine.performanceInfo() && engine.performanceInfo()->name == "Verse Keys"; }), "Next opened the second");
                // Its own color for an entry, over the Performance's.
                check(setlist->colorAt(0) == juce::Colour(0xff46a758), "an entry shows its Performance's color");
                setlist->setColor(0, "#e5484d");
                check(setlist->colorAt(0) == juce::Colour(0xffe5484d), "and the setlist's own color over it");
                snapshot(*setlist, "setlist-mouse");
                standalone::setDesktopMode(*setlist, 2); setlist->resized();
                snapshot(*setlist, "setlist-touch");
                standalone::setDesktopMode(*setlist, 1);
                // More entries, shown as a 3 × 2 grid; Next turns the page.
                for(int i = 0; i < 5; ++i) { setlist->add(library.items()[size_t(i % 3)].id); }
                setlist->setDisplay({standalone::SetlistDisplay::Mode::Grid, 3, 2});
                setlist->setSize(900, 600);
                snapshot(*setlist, "setlist-grid");
                const auto grid = standalone::readSetlist(standalone::setlistFile(folder, "Friday"));
                check(grid.display.mode == standalone::SetlistDisplay::Mode::Grid && grid.display.columns == 3 && grid.display.rows == 2
                      && grid.entries.size() == 7 && grid.entries[0].color == "#e5484d" && grid.entries[1].color.empty(), "the display and colors were saved");
                check(wait([&] { return !engine.soundBusy(); }), "the engine is free");
                setlist->go(6);
                check(setlist->current() == 6 && wait([&] { return !engine.soundBusy(); }), "the second page's first entry opened");
                snapshot(*setlist, "setlist-grid-page-2");
                setlist->setDisplay({standalone::SetlistDisplay::Mode::Scroll, 3, 2});
                snapshot(*setlist, "setlist-scroll");
                // Sounds: a part saved to the library, listed apart, picked into another part, added as a new part.
                check(wait([&] { return !engine.soundBusy(); }), "the engine is free for Sounds");
                const auto partsBefore = engine.graph().parts;
                check(partsBefore.size() >= 2, "parts to save from and load into");
                engine.edit([&](Control& c) { c.savePartAsSound(partsBefore[0].id, "Grand"); });
                check(engine.editError().empty(), "a part saved as a Sound: " + engine.editError());
                library.showKind("sound");
                check(wait([&] { return library.items().size() == 1; }) && library.items()[0].kind == "sound" && library.items()[0].name == "Grand", "the Library lists the Sound apart");
                snapshot(library, "library-sounds");
                actions.offer(standalone::pickSound, "Library", [&](const std::string&, standalone::Actions::Answer answer) {
                    picker = std::make_unique<standalone::LibraryPane>(engine, actions, standalone::LibraryPane::Mode::Pick, "sound");
                    picker->onPicked = std::move(answer);
                    standalone::setDesktopMode(*picker, 1); picker->setSize(760, 620);
                });
                const auto into = partsBefore[1].id;
                actions.request(standalone::pickSound, {}, [&](const std::string& id) {
                    standalone::SoundAction load; load.kind = standalone::SoundAction::LoadSound; load.part = into; load.libraryId = id;
                    engine.soundAction(std::move(load));
                });
                check(wait([&] { return picker->items().size() == 1; }), "the Sound picker lists only Sounds");
                picker->choose(0);
                auto named = [&](uint32_t id) { for(const auto& p : engine.graph().parts) if(p.id == id) return p.name; return std::string(); };
                check(wait([&] { return !engine.soundBusy() && named(into) == "Grand"; }), "the picked Sound was loaded into the part: " + engine.soundMessage());
                library.newPart(0);
                check(wait([&] { return !engine.soundBusy() && engine.graph().parts.size() == partsBefore.size() + 1; }) && engine.graph().parts.back().name == "Grand", "and added as a new part");
                library.showKind("performance");
                check(wait([&] { return library.items().size() == 3; }), "back to the Performances");
                // A file that is not a setlist is refused.
                const auto bad = folder.getChildFile("Bad.json");
                bad.replaceWithText(R"({"format":"oscairn.setlist","version":2,"name":"x","entries":[]})");
                bool refused = false; try { standalone::readSetlist(bad); } catch(const std::exception&) { refused = true; }
                check(refused, "a setlist of another version is refused");
            }
            // Envelopes recognised from their parameters' names.
            {
                const auto adsr = standalone::envelopeOf({"Attack Shape", "Attack", "Decay", "Sustain", "Release"});
                check(adsr.points.size() == 3 && adsr.points[0].time == 1 && adsr.points[1].level == 3 && adsr.points[1].sustain && adsr.points[2].time == 4,
                      "an ADSR: attack to full, decay to the sustain level, release");
                const auto dx7 = standalone::envelopeOf({"Rate 1", "Rate 2", "Rate 3", "Rate 4", "Level 1", "Level 2", "Level 3", "Level 4"});
                check(dx7.start == 7 && dx7.points.size() == 4 && dx7.points[0].rate && dx7.points[2].sustain && dx7.points[3].level == 7,
                      "a DX7 envelope: rates and levels, from level 4 and holding at level 3");
                const auto fmx = standalone::envelopeOf({"Attack Level", "Decay 1 Level", "Decay 2 Level", "Release (Hold) Level", "Attack Time", "Decay 1 Time", "Decay 2 Time", "Release Time", "Hold Time"});
                check(fmx.start == 3 && fmx.points.size() == 5 && fmx.points[0].time == 8 && fmx.points[3].sustain && fmx.points[4].level == 3,
                      "an FM-X envelope: from its hold level, through each stage, back to it");
                check(standalone::envelopeOf({"Cutoff", "Resonance", "Release Shape"}).empty(), "no envelope where there is none");
                const auto fmLfo = standalone::lfoOf({"Speed", "Delay", "Pitch Depth", "Amp Depth", "Key Sync", "Wave"});
                check(fmLfo.wave == 5 && fmLfo.speed == 0 && fmLfo.delay == 1, "an LFO: its wave, speed and delay");
                check(standalone::lfoOf({"Cutoff", "Wave Folder"}).empty(), "no LFO without a wave and a speed");
                check(standalone::waveNamed("Saw down") == standalone::Wave::SawDown && standalone::waveNamed("Sawtooth") == standalone::Wave::SawUp &&
                      standalone::waveNamed("Sample and hold") == standalone::Wave::Random && standalone::waveNamed("MSEG") == standalone::Wave::None, "waves by their names");
                const auto filter = standalone::filterOf({"Subtype", "Type", "Cutoff", "Drive", "Resonance"});
                check(filter.cutoff == 2 && filter.resonance == 4 && filter.type == 1, "a filter: its cutoff, resonance and type");
                check(standalone::filterOf({"Cutoff", "Drive"}).empty(), "no filter without a resonance");
                using FK = standalone::FilterResponse::Kind;
                const auto lp24 = standalone::filterNamed("LP 24 dB"), fmLp = standalone::filterNamed("LPF24D"), hp = standalone::filterNamed("HPF12"), bef = standalone::filterNamed("BEF6");
                check(lp24.kind == FK::LowPass && lp24.poles == 4 && fmLp.kind == FK::LowPass && fmLp.poles == 4 && hp.kind == FK::HighPass && hp.poles == 2 &&
                      bef.kind == FK::Notch && bef.poles == 1 && standalone::filterNamed("LPF12+HPF12").kind == FK::BandPass &&
                      standalone::filterNamed("Thru").kind == FK::None && standalone::filterNamed("FX Comb +").kind == FK::Unknown, "filter types by their names");
            }
            // Parameter pages, in Oscairn Dark with a mouse.
            theme.apply("oscairn-dark");
            {
                // Selecting a plugin on the desktop's Part page shows its parameters there.
                standalone::PartsView view(engine, argv[2]);
                standalone::setDesktopMode(view, 1); view.setEmbedded(true); view.setSize(1180, 820);
                NodeId synth = 0;
                for(const auto& [id, n] : engine.graph().names) if(n == "Basic Synth") synth = id;
                view.showPart(piano); view.selectBox(synth);
                bool shown = false;
                for(auto* c : view.getChildren()) shown |= dynamic_cast<standalone::ParameterGrid*>(c) && c->isVisible() && c->getHeight() > 100;
                check(synth && shown, "a selected plugin's parameters show on the Part page");
                // The keys and velocities on the desktop's Part page: drag the lowest key up to C3.
                view.selectBox(0);
                standalone::ZoneEditor* zone = nullptr;
                for(auto* c : view.getChildren()) if(auto* z = dynamic_cast<standalone::ZoneEditor*>(c)) zone = z;
                check(zone != nullptr, "the Part page has its keys and velocities");
                if(zone)
                {
                    auto mouse = juce::Desktop::getInstance().getMainMouseSource();
                    const auto keys = zone->getLocalBounds().toFloat().withTrimmedTop(18).withTrimmedBottom(26);
                    const float white = keys.getWidth() / 75;
                    const juce::Point<float> from(keys.getX() + 1, keys.getBottom() - 4), to(keys.getX() + 28.5f * white, keys.getBottom() - 4); // C3 (48) is the 29th white key
                    auto event = [&](juce::Point<float> at, bool dragged) { return juce::MouseEvent(mouse, at, juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, zone, zone, juce::Time::getCurrentTime(), from, juce::Time::getCurrentTime(), 1, dragged); };
                    zone->mouseDown(event(from, false)); zone->mouseDrag(event(to, true)); zone->mouseUp(event(to, true));
                    auto lowKey = [&] { for(const auto& p : standalone::readParts(engine.graph()).parts) if(p.id == piano) return p.lowKey; return -1; };
                    for(int i = 0; i < 200 && lowKey() != 48; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                    check(lowKey() == 48, "dragging the range's low end sets the part's lowest key: " + std::to_string(lowKey()));
                    if(argc >= 5) { const juce::File out{juce::String(argv[4]) + "-part-zone.png"}; out.deleteFile(); juce::FileOutputStream stream(out); juce::PNGImageFormat().writeImageToStream(view.createComponentSnapshot(view.getLocalBounds()), stream); }
                    engine.edit([&](Control& c) { standalone::setZone(c, piano, 0, 127, 0); });
                }
                view.selectBox(0);
                shown = false;
                for(auto* c : view.getChildren()) shown |= dynamic_cast<standalone::ParameterGrid*>(c) != nullptr;
                check(!shown, "and go when nothing is selected");
            }
            std::vector<std::pair<std::string, std::string>> edited{{"Basic Synth", ""}};
            for(int i = 5; i < argc; ++i) edited.push_back({juce::File(argv[i]).getFileNameWithoutExtension().toStdString(), argv[i]});
            for(const auto& [name, path] : edited)
            {
                NodeId node = 0;
                if(path.empty()) { for(const auto& [id, n] : engine.graph().names) if(n == name) node = id; }
                else
                {
                    engine.edit([&, name = name, path = path](Control& c) { node = c.add(name, path); c.commit(); });
                    for(int i = 0; i < 500 && !engine.graph().names.count(node); ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                }
                check(node && engine.graph().names.count(node), "the plugin to edit is in the graph: " + name);
                juce::Component holder; standalone::setDesktopMode(holder, 1); holder.setSize(1180, 820);
                standalone::ParameterGrid grid(engine, node, name);
                holder.addAndMakeVisible(grid); grid.setBounds(holder.getLocalBounds().reduced(16));
                check(grid.pages() > 0, "the plugin's parameters make at least one page: " + name);
                // No two controls overlap (tiles, linked groups and wide panels all placed apart).
                auto overlapping = [&] {
                    std::vector<juce::Rectangle<int>> cells;
                    std::function<void(juce::Component&)> gather = [&](juce::Component& c) { for(auto* child : c.getChildren()) { if(dynamic_cast<juce::Slider*>(child) || dynamic_cast<juce::ComboBox*>(child)) cells.push_back(child->getBoundsInParent()); gather(*child); } };
                    gather(grid);
                    for(size_t a = 0; a < cells.size(); ++a) for(size_t b = a + 1; b < cells.size(); ++b) if(cells[a].intersects(cells[b])) return true;
                    return false;
                };
                if(name == "Basic Synth")
                {
                    // Its oscairn.ui hints: the most played first, knobs, the oscillator as buttons.
                    grid.showPage(0);
                    const auto names = grid.namesShown();
                    check(names.size() == 8 && names[0] == "Brightness" && names[1] == "Level" && names[2] == "Oscillator" && names.back() == "Output bus", "tiles in the plugin's priority order");
                    const auto rate = std::find(names.begin(), names.end(), "Pitch LFO rate");
                    check(grid.linkedShown() == 1 && rate != names.end() && rate + 1 != names.end() && *(rate + 1) == "Pitch LFO depth", "the plugin's linked LFO tiles, together");
                    int knobs = 0;
                    std::function<void(juce::Component&)> count = [&](juce::Component& c) {
                        for(auto* child : c.getChildren()) { if(auto* s = dynamic_cast<juce::Slider*>(child); s && s->isRotary()) ++knobs; count(*child); }
                    };
                    count(grid);
                    check(knobs == 6, "the knobs it asks for: " + std::to_string(knobs));
                }
                auto save = [&, name = name](const juce::String& suffix) {
                    for(int i = 0; i < 10; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                    const auto image = holder.createComponentSnapshot(holder.getLocalBounds());
                    check(image.getWidth() == holder.getWidth(), "a parameter page renders");
                    if(argc < 5) return;
                    const juce::File out{juce::String(argv[4]) + "-params-" + juce::String(name).replaceCharacter(' ', '-').toLowerCase() + "-" + suffix + ".png"};
                    out.deleteFile(); juce::FileOutputStream stream(out); juce::PNGImageFormat().writeImageToStream(image, stream);
                };
                for(size_t page = 0; page < std::min<size_t>(grid.pages(), 3); ++page) { grid.showPage(page); save(juce::String(int(page + 1))); }
                // A large page gets tabs, each showing part of it; the first such page, saved.
                bool tabbed = false;
                for(size_t page = 0; page < grid.pages(); ++page)
                {
                    grid.showPage(page);
                    if(grid.tabs() < 2) continue;
                    const auto first = grid.tilesShown();
                    size_t total = 0;
                    for(size_t tab = 0; tab < grid.tabs(); ++tab) { grid.showTab(tab); total += grid.tilesShown(); }
                    check(first > 0 && total > first, "a tabbed page splits its parameters between its tabs");
                    if(!tabbed) { grid.showTab(0); save("tabs"); tabbed = true; }
                }
                // The analog and FM engines' envelopes and LFOs show where their pages have them.
                if(name == "oscairn-analog" || name == "oscairn-fm")
                {
                    size_t envelopes = 0; bool saved = false, savedFilter = false;
                    for(size_t page = 0; page < grid.pages(); ++page)
                    {
                        grid.showPage(page);
                        for(size_t tab = 0; tab < std::max<size_t>(1, grid.tabs()); ++tab)
                        {
                            if(grid.tabs()) grid.showTab(tab);
                            envelopes += grid.panelsShown();
                            check(!overlapping(), name + ": no controls overlap on " + grid.pageName(page));
                            if(grid.panelsShown() && !saved) { save("envelope"); saved = true; }
                            if(grid.panelsShown() && (grid.pageName(page) == "Filter" || grid.pageName(page) == "Filter 1") && !savedFilter)
                            {
                                // Cutoff halfway and some resonance, so the curve shows its peak.
                                for(const auto& p : engine.parameters(node))
                                {
                                    if(p.module != grid.pageName(page)) continue;
                                    if(p.name == "Cutoff" || p.name == "Resonance") engine.setParam(name, p.id, p.minimum + (p.maximum - p.minimum) * (p.name == "Cutoff" ? .5 : .7));
                                    if(p.name == "Type") engine.setParam(name, p.id, p.minimum + 2); // the analog engine's LP 24 dB
                                }
                                for(int i = 0; i < 20; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                                save("filter"); savedFilter = true;
                            }
                        }
                    }
                    check(envelopes >= 2, name + "'s envelopes and LFOs are drawn: " + std::to_string(envelopes));
                }
                // The analog engine's filter envelope: its stages linked in order, at any width.
                if(name == "oscairn-analog")
                {
                    grid.showPage(0);
                    auto names = grid.namesShown();
                    const auto attack = std::find(names.begin(), names.end(), "Attack");
                    check(grid.linkedShown() >= 1 && attack + 3 < names.end() && *(attack + 1) == "Decay" && *(attack + 2) == "Sustain" && *(attack + 3) == "Release",
                          "the envelope's stages linked, in order");
                    holder.setSize(560, 820); grid.setBounds(holder.getLocalBounds().reduced(16));
                    save("narrow");
                    names = grid.namesShown();
                    const auto still = std::find(names.begin(), names.end(), "Attack");
                    check(still + 3 < names.end() && *(still + 3) == "Release", "and still together on a narrow screen");
                    check(!overlapping(), "no controls overlap on a narrow screen");
                    holder.setSize(1180, 820); grid.setBounds(holder.getLocalBounds().reduced(16));
                }
                // Dragging the analog engine's filter envelope's attack point to the right lengthens
                // its attack (the point starts where a zero attack leaves it: the curve's top left).
                if(name == "oscairn-analog")
                {
                    grid.showPage(0);
                    juce::Component* curve = nullptr;
                    std::function<void(juce::Component&)> find = [&](juce::Component& c) {
                        for(auto* child : c.getChildren())
                        {
                            if(!dynamic_cast<juce::Label*>(child) && !dynamic_cast<juce::Slider*>(child) && !dynamic_cast<juce::ComboBox*>(child) &&
                               !dynamic_cast<juce::Button*>(child) && !dynamic_cast<juce::TextEditor*>(child) && child->getHeight() == 150) curve = child;
                            find(*child);
                        }
                    };
                    find(grid);
                    uint32_t attack = 0; double before = 0;
                    for(const auto& p : engine.parameters(node)) if(p.module == "Filter Envelope" && p.name == "Attack") { attack = p.id; before = engine.param(name, p.id); }
                    check(curve && attack, "the filter envelope's curve and its attack");
                    if(curve && attack)
                    {
                        auto mouse = juce::Desktop::getInstance().getMainMouseSource();
                        const juce::Point<float> from(14, 28), to(214, 28);
                        auto event = [&](juce::Point<float> at, bool dragged) {
                            return juce::MouseEvent(mouse, at, juce::ModifierKeys::leftButtonModifier, 1, 0, 0, 0, 0, curve, curve, juce::Time::getCurrentTime(), from, juce::Time::getCurrentTime(), 1, dragged);
                        };
                        curve->mouseDown(event(from, false)); curve->mouseDrag(event(to, true)); curve->mouseUp(event(to, true));
                        for(int i = 0; i < 100 && engine.param(name, attack) == before; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                        check(engine.param(name, attack) > before, "dragging the attack point lengthens the attack");
                    }
                }
                // A search finds parameters on every page; clearing it returns to the page.
                grid.showPage(0);
                const auto pageTiles = grid.tilesShown();
                grid.search("a");
                check(grid.tilesShown() > 0 && grid.tabs() == 0, "a search shows its matches, without tabs");
                grid.search("release");
                save("search");
                grid.search("no parameter is called this");
                check(grid.tilesShown() == 0, "a search with no matches shows nothing");
                grid.search("");
                check(grid.tilesShown() == pageTiles, "clearing the search shows the page again");
            }
            // A device's knobs play the row selected on screen: a panel with four "row:N" knobs to
            // the right of its screen, and the Basic Synth's page over that screen.
            {
                const auto layout = standalone::Layout::parse(R"({"name": "Row test", "size": [1400, 600], "controls": [
                    {"type": "screen", "rect": [0, 0, 1024, 600]},
                    {"type": "knob", "rect": [1100, 20, 120, 120], "label": "P1", "bind": "row:1"}, {"type": "knob", "rect": [1100, 160, 120, 120], "label": "P2", "bind": "row:2"},
                    {"type": "knob", "rect": [1100, 300, 120, 120], "label": "P3", "bind": "row:3"}, {"type": "knob", "rect": [1100, 440, 120, 120], "label": "P4", "bind": "row:4"}]})");
                standalone::Panel panel(engine, layout);
                panel.setSize(1400, 600);
                check(engine.rowKnobs() == 4 && engine.rowEdge() == standalone::Engine::Edge::Right, "the panel's row knobs, right of its screen");
                NodeId synth = 0;
                for(const auto& [id, n] : engine.graph().names) if(n == "Basic Synth") synth = id;
                auto grid = std::make_unique<standalone::ParameterGrid>(engine, synth, "Basic Synth");
                standalone::setDesktopMode(*grid, 2);
                panel.overlay(grid.get());
                for(int i = 0; i < 20; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                // Knobs beside the screen play a column, top to bottom.
                const auto first = grid->rowShown();
                check(first.size() >= 2 && first[0] == "Brightness" && first[1] != "Level", "the first column is the side knobs' until another is chosen");
                // Tap a tile in the next column.
                juce::Component* tile = nullptr;
                std::function<void(juce::Component&)> find = [&](juce::Component& c) { for(auto* child : c.getChildren()) { if(auto* s = dynamic_cast<juce::Slider*>(child); s && s->isRotary() && s->getX() > 150 && s->getY() < 100 && !tile) tile = s; find(*child); } };
                find(*grid);
                if(tile)
                {
                    auto mouse = juce::Desktop::getInstance().getMainMouseSource();
                    const juce::Point<float> at(4, 4);
                    grid->mouseDown(juce::MouseEvent(mouse, at, {}, 1, 0, 0, 0, 0, tile, tile, juce::Time::getCurrentTime(), at, juce::Time::getCurrentTime(), 1, false));
                }
                const auto second = grid->rowShown();
                check(tile && !second.empty() && second != first && second[0] == "Level", "tapping a tile in another column gives the knobs that column");
                // P1 now plays that row's first parameter.
                juce::Slider* p1 = nullptr;
                std::function<void(juce::Component&)> knob = [&](juce::Component& c) { // the slider in P1's place on the panel
                    for(auto* child : c.getChildren()) { if(auto* s = dynamic_cast<juce::Slider*>(child); s && !p1 && s->getParentComponent()->getBounds().getTopLeft() == juce::Point<int>(1100, 20)) p1 = s; knob(*child); }
                };
                knob(panel);
                uint32_t played = 0; double range = 1;
                for(const auto& p : engine.parameters(synth)) if(p.name == second[0]) { played = p.id; range = p.maximum - p.minimum; }
                for(int i = 0; i < 10; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(40); // the panel's knobs follow the row
                check(p1 && p1->isEnabled() && played, "P1 follows the row");
                if(p1 && played)
                {
                    const double target = p1->getMinimum() + range * .25;
                    p1->setValue(target, juce::sendNotificationSync);
                    for(int i = 0; i < 100 && std::abs(engine.param("Basic Synth", played) - target) > 1e-6; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                    check(std::abs(engine.param("Basic Synth", played) - target) < 1e-6, "turning P1 sets " + second[0]);
                }
                if(argc >= 5) { const juce::File out{juce::String(argv[4]) + "-row-knobs.png"}; out.deleteFile(); juce::FileOutputStream stream(out); juce::PNGImageFormat().writeImageToStream(panel.createComponentSnapshot(panel.getLocalBounds()), stream); }
                panel.overlay(nullptr); grid.reset();
                check(engine.row().empty(), "the row goes with its page");
            }
            check(engine.rowKnobs() == 0, "and the knobs with their panel");
            {
                // Knobs under the screen play a row, left to right.
                const auto layout = standalone::Layout::parse(R"({"name": "Row test", "size": [1024, 760], "controls": [
                    {"type": "screen", "rect": [0, 0, 1024, 600]},
                    {"type": "knob", "rect": [20, 620, 120, 120], "label": "K1", "bind": "row:1"}, {"type": "knob", "rect": [160, 620, 120, 120], "label": "K2", "bind": "row:2"},
                    {"type": "knob", "rect": [300, 620, 120, 120], "label": "K3", "bind": "row:3"}]})");
                standalone::Panel panel(engine, layout);
                check(engine.rowKnobs() == 3 && engine.rowEdge() == standalone::Engine::Edge::Bottom, "knobs under the screen");
                NodeId synth = 0;
                for(const auto& [id, n] : engine.graph().names) if(n == "Basic Synth") synth = id;
                standalone::ParameterGrid grid(engine, synth, "Basic Synth");
                standalone::setDesktopMode(grid, 2); grid.setSize(1024, 600);
                const auto row = grid.rowShown();
                check(row.size() >= 3 && row[0] == "Brightness" && row[1] == "Level" && row[2] == "Oscillator", "the first row, left to right");
            }
            // The preference: Automatic until one is saved, then what was saved.
            const auto folder = juce::File::createTempFile("oscairn-input");
            check(standalone::readInputMode(folder) == standalone::InputMode::Automatic, "no saved preference reads as Automatic");
            standalone::writeInputMode(folder, standalone::InputMode::Touch);
            check(standalone::readInputMode(folder) == standalone::InputMode::Touch, "the saved preference reads back");
            folder.deleteRecursively();
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
            configuration.deleteRecursively();
        }
        running = false;
        control.join();
    }
    std::filesystem::remove(socket);
    data.deleteRecursively();
    if(!failures) std::puts("ok: the desktop window renders every tab in both input modes");
    return failures ? 1 : 0;
}

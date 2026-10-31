// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The desktop window (RFC 0019's Desktop surface) against a real engine session over the
// control socket, without an audio device: a small Performance (parts, a split, a bus, scenes,
// a control), then every tab renders in each built-in theme, and in Oscairn Dark in both input
// modes. With SNAPSHOTS, each render is written to SNAPSHOTS-<theme>-<input>-<tab>.png.
// Then a plugin's parameter pages (RFC 0019's generated editor): the Basic Synth's, and any
// PLUGIN.clap given after SNAPSHOTS, each written to SNAPSHOTS-params-<name>-<page>.png.
//   oscairn_desktop_shell_test PLUGIN_DIR SCANNER THEMES [SNAPSHOTS [PLUGIN.clap...]]
#include "../DesktopShell.h"
#include "../ParameterGrid.h"
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
                for(size_t page = 0; page < std::min<size_t>(grid.pages(), 3); ++page)
                {
                    grid.showPage(page);
                    for(int i = 0; i < 10; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                    const auto image = holder.createComponentSnapshot(holder.getLocalBounds());
                    check(image.getWidth() == 1180, "a parameter page renders");
                    if(argc < 5) continue;
                    const juce::File out{juce::String(argv[4]) + "-params-" + juce::String(name).replaceCharacter(' ', '-').toLowerCase() + "-" + juce::String(int(page + 1)) + ".png"};
                    out.deleteFile(); juce::FileOutputStream stream(out); juce::PNGImageFormat().writeImageToStream(image, stream);
                }
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
    if(!failures) std::puts("ok: the desktop window renders every tab in both input modes");
    return failures ? 1 : 0;
}

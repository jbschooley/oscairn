// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The desktop window (RFC 0019's Desktop surface) against a real engine session over the
// control socket, without an audio device: a small Performance (parts, a split, a bus, scenes,
// a control), then every tab in both input modes renders, and the parts list selects a part.
// With SNAPSHOTS, each render is written to SNAPSHOTS-<input>-<tab>.png.
//   oscairn_desktop_shell_test PLUGIN_DIR SCANNER [SNAPSHOTS]
#include "../DesktopShell.h"
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
    if(argc < 3) return 2;
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
        std::atomic<bool> running = true;
        std::thread control([&] {
            while(running)
            {
                server.poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                session.service();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        {
            standalone::Engine engine(dir.string(), socket.string());
            standalone::Theme theme;
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
            const auto made = standalone::readParts(engine.graph());
            check(made.parts.size() == 4 && made.buses.size() == 1 && made.parts[0].sound == "Basic Synth" && made.buses[0].chain.size() == 1 && engine.editError().empty(),
                  "the Performance was made with its plugins: " + std::string(engine.editError()));

            for(const auto mode : {standalone::InputMode::Mouse, standalone::InputMode::Touch})
            {
                standalone::DesktopShell shell(engine, argv[2], mode);
                shell.setSize(1440, 900);
                for(int i = 0; i < 30; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10); // the shell's first ticks
                for(const char* name : {"Part", "Mixer", "Routing", "Controls", "Sequencer"})
                {
                    standalone::DesktopShell::Tab tab{};
                    check(standalone::DesktopShell::tabNamed(name, tab), std::string("a tab named ") + name);
                    shell.showTab(tab);
                    for(int i = 0; i < 5; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                    const auto image = shell.createComponentSnapshot(shell.getLocalBounds());
                    check(image.getWidth() == 1440 && image.getHeight() == 900, std::string("renders ") + name);
                    if(argc < 4) continue;
                    const juce::File out{juce::String(argv[3]) + "-" + (mode == standalone::InputMode::Touch ? "touch" : "mouse") + "-" + juce::String(name).toLowerCase() + ".png"};
                    out.deleteFile();
                    juce::FileOutputStream stream(out);
                    juce::PNGImageFormat().writeImageToStream(image, stream);
                }
            }
            // The preference: Automatic until one is saved, then what was saved.
            const auto folder = juce::File::createTempFile("oscairn-input");
            check(standalone::readInputMode(folder) == standalone::InputMode::Automatic, "no saved preference reads as Automatic");
            standalone::writeInputMode(folder, standalone::InputMode::Touch);
            check(standalone::readInputMode(folder) == standalone::InputMode::Touch, "the saved preference reads back");
            folder.deleteRecursively();
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        }
        running = false;
        control.join();
    }
    std::filesystem::remove(socket);
    if(!failures) std::puts("ok: the desktop window renders every tab in both input modes");
    return failures ? 1 : 0;
}

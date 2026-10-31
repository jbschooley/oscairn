// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pad editor and the Parts views against a real engine session over the control socket,
// without an audio device: edits reach the session, and the views render (optionally to PNGs:
// SNAPSHOTS-pads.png, -parts.png, -part.png).
//   oscairn_pad_editor_test PLUGIN_DIR SCANNER [SNAPSHOTS]
#include "../PadEditor.h"
#include "../PartsView.h"
#include "../Theme.h"
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
    const auto socket = std::filesystem::temp_directory_path() / ("oscairn-pads-" + std::to_string(::getpid()) + ".sock");
    {
        // The session, as the engine helper would run it, served on its own control thread.
        Session session(48000, 128);
        session.add("sequencer", (dir / "oscairn-step-sequencer.clap").string());
        const auto synth = session.add("synth", (dir / "oscairn-basic-synth.clap").string());
        session.connectNotes(graphInput, synth); session.connectAudio(synth, graphOutput); session.commit();
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
            standalone::PadEditor editor(engine);
            editor.setSize(1024, 600);
            // The editor's edits go through the engine to the session's pad map.
            auto map = engine.padMap();
            fillPads(map, 0, {PadFill::Kind::Drums});
            map.assignments[0][3].channel = 9;
            engine.setPadMap(map);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while(!(engine.padMap() == map) && std::chrono::steady_clock::now() < deadline) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            check(engine.padMap() == map, "the engine keeps the edited map");
            standalone::PadEditor shown(engine);
            shown.setSize(1024, 600);
            auto save = [&](juce::Component& c, const char* suffix) {
                const auto image = c.createComponentSnapshot(c.getLocalBounds());
                check(image.getWidth() == 1024 && image.getHeight() == 600, "renders at the device's screen size");
                if(argc < 4) return;
                const juce::File out{juce::String(argv[3]) + suffix};
                out.deleteFile();
                juce::FileOutputStream stream(out);
                juce::PNGImageFormat().writeImageToStream(image, stream);
            };
            save(shown, "-pads.png");

            // The Parts views: a two-layer split and a shared delay, made with the views' edits.
            const auto plugins = standalone::bundledPlugins(argv[1], argv[2]);
            auto plugin = [&](const char* name) { for(const auto& p : plugins) if(p.name == name) return p; throw std::runtime_error(std::string("missing ") + name); };
            engine.edit([&](Control& c) {
                const auto bass = standalone::addPart(c, "Bass", false), lead = standalone::addPart(c, "Lead", false), room = standalone::addPart(c, "Room", true);
                standalone::insertPlugin(c, bass, 0, plugin("Basic Synth")); standalone::insertPlugin(c, lead, 0, plugin("Basic Synth"));
                standalone::insertPlugin(c, lead, 1, plugin("Delay")); standalone::insertPlugin(c, room, 0, plugin("Delay"));
                standalone::setZone(c, bass, 0, 59, -12); standalone::setZone(c, lead, 60, 127, 0);
                standalone::setSendLevel(c, bass, room, .5); standalone::setSendLevel(c, lead, room, .3);
                c.setPartLevel(lead, .8, false); c.commit();
                c.addScene("Verse"); c.setPartLevel(bass, .5, true); c.commit(); c.addScene("Chorus");
            });
            // The engine replays edits deferred behind its startup job from its timer.
            for(int i = 0; i < 500 && engine.graph().parts.size() < 3; ++i) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
            check(engine.graph().parts.size() == 3 && engine.editError().empty(), "the parts were made: " + std::string(engine.editError()));
            standalone::PartsView view(engine, argv[2]);
            view.setSize(1024, 600);
            view.refresh();
            save(view, "-parts.png");
            view.showPart(engine.graph().parts[1].id);
            save(view, "-part.png");
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        }
        running = false;
        control.join();
        check(session.padMap().assignments[0][0].note == 36 && session.padMap().assignments[0][1].note == 38 && session.padMap().assignments[0][3].channel == 9,
              "the session has the drum kit and pad 4's channel");
    }
    std::filesystem::remove(socket);
    if(!failures) std::puts("ok: pad editor and Parts views edit the session and render");
    return failures ? 1 : 0;
}

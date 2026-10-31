// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The pad editor against a real engine session over the control socket, without an audio
// device: edits reach the session's pad map, and the editor renders (optionally to a PNG).
//   oscairn_pad_editor_test PLUGIN_DIR [SNAPSHOT.png]
#include "../PadEditor.h"
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
    if(argc < 2) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    int failures = 0;
    auto check = [&](bool ok, const char* what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } };
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
            while(!(engine.padMap() == map) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            check(engine.padMap() == map, "the engine keeps the edited map");
            standalone::PadEditor shown(engine);
            shown.setSize(1024, 600);
            const auto image = shown.createComponentSnapshot(shown.getLocalBounds());
            check(image.getWidth() == 1024 && image.getHeight() == 600, "the editor renders at the device's screen size");
            if(argc > 2)
            {
                const juce::File out{juce::String(argv[2])};
                out.deleteFile();
                juce::FileOutputStream stream(out);
                juce::PNGImageFormat().writeImageToStream(image, stream);
            }
            juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        }
        running = false;
        control.join();
        check(session.padMap().assignments[0][0].note == 36 && session.padMap().assignments[0][1].note == 38 && session.padMap().assignments[0][3].channel == 9,
              "the session has the drum kit and pad 4's channel");
    }
    std::filesystem::remove(socket);
    if(!failures) std::puts("ok: pad editor edits reach the session and it renders");
    return failures ? 1 : 0;
}

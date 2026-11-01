// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 in the desktop app: a Performance of two parts playing the Basic Synth and a part
// whose plugin crashes on every block, every plugin in a worker as the app runs them. Once the
// crashing plugin is given up, the other parts still play (their meters move), its part is
// silent, and the desktop names it: in the engine's status and on the Parts view.
//   oscairn_plugin_crash_test PLUGIN_DIR SCANNER WORKER PROCESS_CRASH.clap
#include "../PartsView.h"
#include "../Theme.h"
#include "oscairn/local_socket.h"
#include "oscairn/session.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include <unistd.h>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 5) return 2;
    ::setenv("OSCAIRN_PLUGIN_WORKER", argv[3], 1);
    juce::ScopedJuceInitialiser_GUI gui;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    const auto dir = std::filesystem::path(argv[1]);
    const auto socket = std::filesystem::temp_directory_path() / ("oscairn-crash-" + std::to_string(::getpid()) + ".sock");
    const auto data = juce::File::createTempFile("oscairn-crash-data");
    ::setenv("OSCAIRN_DATA_HOME", data.getFullPathName().toRawUTF8(), 1);
    {
        Session session(48000, 128);
        LocalServer server(session);
        server.listen(socket.string());
        std::atomic<bool> running = true, play = false;
        // The audio thread's part, on the control thread: once the Performance is made, a chord held.
        std::thread control([&] {
            std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
            bool held = false;
            while(running)
            {
                server.poll(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
                session.service();
                if(play && !held) { for(const int key : {48, 60, 64, 67}) session.note(true, key, .8); held = true; }
                session.process(nullptr, 0, out, 2, 128);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        {
            standalone::Engine engine(dir.string(), socket.string());
            standalone::Theme theme;
            juce::LookAndFeel::setDefaultLookAndFeel(&theme);
            const standalone::PluginChoice synth{"Basic Synth", (dir / "oscairn-basic-synth.clap").string(), "org.oscairn.basic-synth"},
                                           crashing{"Crashing", argv[4], "org.kick.test.midi"};
            uint32_t piano = 0, bass = 0, crashy = 0;
            engine.edit([&](Control& c) {
                piano = standalone::addPart(c, "Piano", false); bass = standalone::addPart(c, "Bass", false); crashy = standalone::addPart(c, "Crashy", false);
                standalone::insertPlugin(c, piano, 0, synth); standalone::insertPlugin(c, bass, 0, synth); standalone::insertPlugin(c, crashy, 0, crashing);
                c.commit();
            });
            auto pump = [&](auto done, int seconds) {
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
                while(!done() && std::chrono::steady_clock::now() < until) juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
                return done();
            };
            check(pump([&] { return engine.graph().parts.size() == 3; }, 5) && engine.editError().empty(), "the Performance was made: " + engine.editError());
            play = true;
            // Restarted three times, then given up.
            auto failed = [&] {
                for(const auto& f : engine.status().failures) if(f.reason.find("not restarted") != std::string::npos) return f;
                return Control::SessionStatus::PluginFailure{};
            };
            check(pump([&] { return failed().node != 0; }, 10), "the crashing plugin is given up");
            const auto failure = failed();
            check(failure.name.find("Crashing") != std::string::npos && failure.reason.find("Plugin worker crashed") != std::string::npos,
                  "the desktop names the crashed plugin and why: " + failure.name + ": " + failure.reason);
            // The other parts keep playing; the crashed part is silent.
            bool pianoPlays = false, bassPlays = false, crashySilent = true;
            pump([&] {
                for(const auto& m : engine.meters())
                {
                    if(m.part == piano) pianoPlays |= m.peak > 0;
                    if(m.part == bass) bassPlays |= m.peak > 0;
                    if(m.part == crashy) crashySilent &= m.peak == 0;
                }
                return false;
            }, 1);
            check(pianoPlays && bassPlays, "the other parts still play");
            check(crashySilent, "the crashed part is silent");
            check(engine.status().failures.size() == 1, "only the crashed plugin is reported");
            // The Parts view says so.
            standalone::PartsView view(engine, argv[2]);
            view.setSize(1024, 600);
            bool named = false;
            pump([&] {
                for(auto* child : view.getChildren())
                    if(auto* label = dynamic_cast<juce::Label*>(child)) named |= label->getText().contains("is silent: Plugin worker crashed");
                return named;
            }, 3);
            check(named, "the Parts view names the silent plugin");
        }
        running = false;
        control.join();
    }
    std::filesystem::remove(socket);
    data.deleteRecursively();
    if(!failures) std::puts("ok: a crashing plugin in a Performance leaves the other parts playing and is reported");
    return failures ? 1 : 0;
}

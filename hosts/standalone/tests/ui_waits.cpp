// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The desktop's message thread and the engine's socket: a mute, a fader drag and the views'
// polls never wait on it, and opening a Performance costs the app little once the engine has
// loaded it. Against a session of 20 plugins served on the engine's real 5 ms timer, without
// an audio device; it prints what it measured.
//   oscairn_ui_waits_test PLUGIN_DIR SCANNER
#include "../Engine.h"
#include "../PartsModel.h"
#include "oscairn/local_socket.h"
#include "oscairn/session.h"
#include <juce_gui_extra/juce_gui_extra.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace oscairn;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    if(argc < 3) return 2;
    juce::ScopedJuceInitialiser_GUI gui;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    const auto socket = std::filesystem::temp_directory_path() / ("oscairn-waits-" + std::to_string(::getpid()) + ".sock");
    const auto folder = std::filesystem::temp_directory_path() / ("oscairn-waits-" + std::to_string(::getpid()));
    std::filesystem::create_directories(folder);
    const auto data = folder / "data";
    ::setenv("OSCAIRN_DATA_HOME", data.c_str(), 1); // the user's library is never read or written
    {
        // Two Performances of ten parts, each a synth and a delay: 20 plugins. A is served.
        const auto plugins = standalone::bundledPlugins(argv[1], argv[2]);
        auto plugin = [&](const char* name) { for(const auto& p : plugins) if(p.name == name) return p; throw std::runtime_error(std::string("missing ") + name); };
        auto build = [&](Session& s, const std::string& name) {
            for(int i = 0; i < 10; ++i)
            {
                const auto part = standalone::addPart(s, name + std::to_string(i), false);
                standalone::insertPlugin(s, part, 0, plugin("Basic Synth"));
                standalone::insertPlugin(s, part, 1, plugin("Delay"));
            }
            auto info = s.performanceInfo(); info.name = name; s.setPerformanceInfo(info);
            s.savePerformance((folder / name).string());
        };
        { Session other(48000, 128); build(other, "B"); }
        Session session(48000, 128);
        build(session, "A");
        check(session.graphState().topology.nodes.size() >= 20, "20 plugins: " + std::to_string(session.graphState().topology.nodes.size()));
        // Served as the engine serves it: the socket read every 5 ms. When a load is answered.
        LocalServer server(session);
        server.listen(socket.string());
        std::atomic<bool> running = true;
        std::mutex mutex;
        std::string shown = session.performanceInfo().name;
        std::optional<Clock::time_point> answered;
        std::thread control([&] {
            std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
            auto next = Clock::now();
            while(running)
            {
                server.poll(uint64_t(Clock::now().time_since_epoch().count()));
                if(const auto name = session.performanceInfo().name; name != shown) { std::lock_guard lock(mutex); shown = name; answered = Clock::now(); }
                session.service();
                session.process(nullptr, 0, out, 2, 128);
                std::this_thread::sleep_until(next += std::chrono::milliseconds(5));
            }
        });
        {
            standalone::Engine engine(argv[1], socket.string());
            auto run = [](int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); };
            auto wait = [&](auto done) { for(int i = 0; i < 1000 && !done(); ++i) run(10); return done(); };
            auto free = [&] { return wait([&] { return !engine.jobRunning(); }); };
            check(wait([&] { return engine.graph().parts.size() == 10 && !engine.jobRunning(); }), "the Performance read");
            auto waits = [&] { return engine.socketWaits(); };
            struct Spent { uint64_t count; long long micros; };
            auto since = [&](LocalClient::Waits before) { const auto now = waits(); return Spent{now.count - before.count, (long long)(now.time - before.time).count()}; };
            const auto lead = engine.graph().parts.front().id;
            auto level = [&](double v, bool m) { engine.edit([=](Control& c) { c.setPartLevel(lead, v, m); c.commit(); }); };
            auto muted = [&] { for(const auto& p : engine.graph().parts) if(p.id == lead) return p.mute; return false; };

            // A mute: shown at once, and the graph read again for its wires without waiting.
            free(); run(100);
            auto before = waits();
            level(1, true);
            check(wait([&] { return muted(); }), "muted");
            run(300); // the engine's graph.changed and the read it starts
            auto mute = since(before);
            std::printf("mute: %llu waits, %lld us waiting\n", (unsigned long long)mute.count, mute.micros);
            check(mute.count == 0, "a mute never waits on the socket: " + std::to_string(mute.count) + " waits, " + std::to_string(mute.micros) + " us");
            level(1, false); run(300);

            // A fader drag: 60 moves, one a frame.
            free(); run(100);
            before = waits();
            for(int i = 0; i < 60; ++i) { level(i / 60.0, false); run(16); }
            run(300);
            const auto drag = since(before);
            std::printf("drag: %llu waits, %lld us waiting\n", (unsigned long long)drag.count, drag.micros);
            check(drag.count == 0, "a fader drag never waits on the socket: " + std::to_string(drag.count) + " waits");

            // Steady state: what the views poll, at their pace, for two seconds, after a round of
            // them (on screen they poll all along; one not asked for in a second is read at once).
            free();
            const auto nodes = engine.graph().topology.nodes;
            engine.history(); engine.assignments(); engine.takes(); engine.performanceInfo();
            run(300);
            before = waits();
            for(int tick = 0; tick < 40; ++tick)
            {
                engine.meters(); engine.wireMeters();
                if(tick % 5 == 0)
                {
                    engine.history(); engine.assignments(); engine.takes(); engine.performanceInfo(); engine.audition();
                    for(const auto& n : nodes) engine.hasPluginWindow(n.id);
                }
                run(50);
            }
            const auto steady = since(before);
            std::printf("steady state, 2 s: %llu waits, %lld us waiting\n", (unsigned long long)steady.count, steady.micros);
            check(steady.count == 0, "the views' polls never wait on the socket: " + std::to_string(steady.count) + " waits, " + std::to_string(steady.micros) + " us");
            check(engine.meters().size() == 10 && engine.performanceInfo() && engine.performanceInfo()->name == "A" && engine.audition(), "and they show the engine's answers");

            // Opening the other Performance: the app's time from the engine's answer to showing it.
            for(const char* name : {"B", "A"})
            {
                free();
                { std::lock_guard lock(mutex); answered.reset(); }
                before = waits();
                standalone::SoundAction open; open.kind = standalone::SoundAction::LoadPerformance; open.directory = (folder / name).string();
                const auto asked = Clock::now();
                engine.soundAction(std::move(open));
                auto opened = [&] {
                    const auto g = engine.graph(); const auto info = engine.performanceInfo();
                    return !engine.soundBusy() && !g.parts.empty() && g.parts.front().name == std::string(name) + "0" && info && info->name == name;
                };
                bool done = false;
                for(int i = 0; i < 5000 && !(done = opened()); ++i) run(1);
                const auto end = Clock::now();
                std::optional<Clock::time_point> at; { std::lock_guard lock(mutex); at = answered; }
                check(done && at.has_value(), std::string("Performance ") + name + " shows");
                const auto app = at ? std::chrono::duration_cast<std::chrono::milliseconds>(end - *at).count() : -1;
                const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(end - asked).count();
                const auto load = since(before);
                std::printf("open %s: %lld ms from asking, %lld ms after the engine answered; %llu waits, %lld us waiting\n", name, (long long)total, (long long)app, (unsigned long long)load.count, load.micros);
                check(app >= 0 && app < 100, std::string("the app shows ") + name + " soon after the engine loaded it: " + std::to_string(app) + " ms");
                check(load.count == 0, std::string("and opening ") + name + " never waits on the socket: " + std::to_string(load.count) + " waits");
            }
        }
        running = false; control.join();
    }
    std::filesystem::remove(socket);
    std::filesystem::remove_all(folder);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}

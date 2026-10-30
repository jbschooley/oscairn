// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 step 4: a prepared sound whose worker crashes comes back with its state and
// parameters, faded in, while the node's other sound keeps playing; a plugin that keeps
// crashing is restarted a bounded number of times and then stays silent and reported.
//   oscairn-worker-recovery-test WORKER NOTE_CRASH.clap PROCESS_CRASH.clap
#include <oscairn/graph.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using kick::clap::Event;
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    constexpr double rate = 48000; constexpr uint32_t frames = 128;
    constexpr clap_id gain = 7;

    struct Player
    {
        oscairn::Graph graph{rate, frames};
        oscairn::NodeId node;
        std::vector<float> l = std::vector<float>(frames), r = std::vector<float>(frames);
        float* out[2]{l.data(), r.data()};
        explicit Player(const char* path) : node(graph.add(path, "org.kick.test.midi"))
        {
            graph.connectNotes(oscairn::graphInput, node); graph.connectAudio(node, oscairn::graphOutput); graph.commit();
        }
        oscairn::PluginHost& host() { return graph.host(node); }
        // One block at the block's pace, so the supervisor works in real time, then main-thread service.
        void play(std::vector<Event> events = {})
        {
            std::this_thread::sleep_for(std::chrono::microseconds(int64_t(1e6 * frames / rate)));
            graph.process(out, frames, nullptr, events.data(), uint32_t(events.size()));
            graph.service();
        }
        float last() const { return l[frames - 1]; }
    };

    void recovers(const char* plugin, const std::filesystem::path& directory)
    {
        // A saved state with gain .6, for a sound prepared from state rather than parameters.
        { auto raw = oscairn::loadPlugin(plugin, "org.kick.test.midi", rate, frames); raw->setParameter(gain, .6); raw->service(); raw->saveState(directory.string()); }
        Player p(plugin);
        auto& host = p.host();
        host.setParameter(gain, .25);
        const auto byParameter = host.prepareSound("By parameter", {{gain, .5}});
        const auto byState = host.prepareSound("By state", {}, directory.string());
        host.selectSound(0, 0); host.selectSound(byParameter, 1); host.selectSound(byState, 2);
        p.play({Event::note(true, 60, 1, 0, 0, 0, 1), Event::note(true, 60, 1, 0, 1, 0, 2), Event::note(true, 60, 1, 0, 2, 0, 3)});
        p.play();
        check(std::abs(p.last() - 1.35f) < 1e-5f, "three sounds play before the crash: " + std::to_string(p.last()));

        // Key 0 crashes the worker of the sound on channel 1 only. A restart can finish within a
        // block, before a failure could be seen, so the restart count is what is waited for.
        auto restarts = [&] { return host.statistics().restarts; };
        p.play({Event::note(true, 0, 1, 0, 1, 0, 4)});
        bool steady = true; int blocks = 0;
        for(; blocks < 1500 && restarts() < 1; ++blocks)
        {
            p.play();
            // Channels 0 and 2 keep playing throughout (.25 + .6).
            for(float x : p.l) steady = steady && std::abs(x - .85f) < 1e-5f;
        }
        check(restarts() == 1, "the crashed sound was restarted once");
        check(steady, "the other sounds played on unchanged while it was down");
        std::printf("restarted after %d blocks\n", blocks);

        // The restarted sound plays with its prepared value again, faded in.
        p.play({Event::note(true, 62, 1, 0, 1, 0, 5)});
        // The fade (10 ms, under four blocks) starts when the worker is back, at most a block before this note.
        check(p.l[0] < 1.349f && p.last() > p.l[0], "the restarted sound fades in: " + std::to_string(p.l[0]) + " to " + std::to_string(p.last()));
        for(int i = 0; i < 10; ++i) p.play();
        check(std::abs(p.last() - 1.35f) < 1e-5f, "the restarted sound keeps its parameter value: " + std::to_string(p.last()));

        // The sound prepared from state comes back with it too.
        p.play({Event::note(true, 0, 1, 0, 2, 0, 6)});
        for(blocks = 0; blocks < 1500 && restarts() < 2; ++blocks) p.play();
        p.play({Event::note(true, 62, 1, 0, 2, 0, 7)});
        for(int i = 0; i < 10; ++i) p.play();
        check(std::abs(p.last() - 1.35f) < 1e-5f, "the sound prepared from state comes back with it: " + std::to_string(p.last()));
        check(host.failure().empty(), "no failure reported once recovered: " + host.failure());
    }

    void givesUp(const char* plugin)
    {
        Player p(plugin);
        std::string reason;
        for(int block = 0; block < 1500 && reason.find("not restarted") == std::string::npos; ++block)
        {
            p.play({Event::note(true, 60, 1, 0, 0, 0, 1)});
            reason = p.host().failure();
        }
        check(reason.find("crashed") != std::string::npos && reason.find("not restarted: 3 restarts") != std::string::npos, "gave up and reported: " + reason);
        for(int i = 0; i < 50; ++i) p.play();
        check(p.host().failure() == reason, "no more restarts once given up");
        std::printf("%s\n", reason.c_str());
    }
}

int main(int argc, char** argv)
{
    if(argc < 4) { std::fprintf(stderr, "usage: %s WORKER NOTE_CRASH PROCESS_CRASH\n", argv[0]); return 2; }
    setenv("OSCAIRN_PLUGIN_WORKER", argv[1], 1);
    const auto directory = std::filesystem::temp_directory_path() / ("oscairn-recovery-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        recovers(argv[2], directory);
        givesUp(argv[3]);
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); failures = 1; }
    std::error_code ignored; std::filesystem::remove_all(directory, ignored);
    return failures ? 1 : 0;
}

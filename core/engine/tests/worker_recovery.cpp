// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 steps 4-5: a prepared sound whose worker crashes comes back with its state and
// parameters, faded in, while the node's other sound keeps playing; a plugin that keeps
// crashing is restarted a bounded number of times and then stays silent and reported. Native
// engines come back whole: Basic Synth with its prepared sounds and selections, the Sampler with
// a sound whose files resolve through the asset store, Delay with an effect context.
//   oscairn-worker-recovery-test WORKER NOTE_CRASH.clap PROCESS_CRASH.clap BASIC_SYNTH.clap SAMPLER.clap DELAY.clap
#include <oscairn/assets.h>
#include <oscairn/graph.h>
#include <oscairn/wav_file.h>
#include <csignal>
#include <unistd.h>
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
        explicit Player(const char* path, const char* id = "org.kick.test.midi", kick::clap::AssetResolver* assets = nullptr)
        {
            graph.setAssets(assets); node = graph.add(path, id);
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

    // By command line: Linux shortens process names to 15 characters.
    int workers()
    {
        FILE* list = popen(("pgrep -P " + std::to_string(getpid()) + " -f oscairn-plugin-worker").c_str(), "r");
        char line[32]; int n = 0;
        while(list && std::fgets(line, sizeof line, list)) ++n;
        if(list) pclose(list);
        return n;
    }

    // Per-plugin overrides of where a plugin runs.
    void placement(const char* synth)
    {
        auto policy = oscairn::placementPolicy();
        const int before = workers();
        policy.plugins["org.oscairn.basic-synth"] = oscairn::Placement::Engine;
        oscairn::setPlacementPolicy(policy);
        { auto host = oscairn::loadPlugin(synth, "org.oscairn.basic-synth", rate, frames); check(workers() == before, "a plugin placed in the engine starts no worker"); }
        policy.standard = oscairn::Placement::Engine; policy.plugins["org.oscairn.basic-synth"] = oscairn::Placement::Worker;
        oscairn::setPlacementPolicy(policy);
        { auto host = oscairn::loadPlugin(synth, "org.oscairn.basic-synth", rate, frames); check(workers() == before + 1, "a plugin placed in a worker starts one"); }
        { auto host = oscairn::loadPlugin(synth, "", rate, frames); check(workers() == before, "other plugins follow the standard placement"); }
    }

    // Kills this process's plugin workers, as a crash would.
    void killWorkers()
    {
        FILE* list = popen(("pgrep -P " + std::to_string(getpid()) + " -f oscairn-plugin-worker").c_str(), "r");
        char line[32]; int killed = 0;
        while(list && std::fgets(line, sizeof line, list)) if(kill(std::atoi(line), SIGKILL) == 0) ++killed;
        if(list) pclose(list);
        check(killed > 0, "a worker to kill");
    }
    template<class Ready> void awaitRestart(Player& p, Ready ready)
    {
        for(int block = 0; block < 1500 && !ready(); ++block) p.play();
        check(ready(), "restarted");
        for(int block = 0; block < 10; ++block) p.play(); // past the fade-in
    }
    std::vector<std::pair<uint32_t, std::string>> sounds(oscairn::PluginHost& host)
    {
        std::vector<std::pair<uint32_t, std::string>> result;
        for(const auto& c : host.soundContexts()) result.emplace_back(c.id, c.name);
        return result;
    }
    // Renders the same notes in both, block by block; true when every sample matches.
    bool same(Player& a, Player& b, std::vector<Event> notes, int blocks)
    {
        bool equal = true; double energy = 0;
        for(int i = 0; i < blocks; ++i)
        {
            a.play(i ? std::vector<Event>{} : notes); b.play(i ? std::vector<Event>{} : notes);
            for(uint32_t f = 0; f < frames; ++f) { equal = equal && std::abs(a.l[f] - b.l[f]) < 1e-6f && std::abs(a.r[f] - b.r[f]) < 1e-6f; energy += std::abs(a.l[f]); }
        }
        return equal && energy > 0;
    }

    void natives(const char* synth, const char* sampler, const char* delay, const std::filesystem::path& directory)
    {
        { // Basic Synth: two prepared sounds on two channels.
            Player p(synth, "");
            auto& host = p.host();
            const auto warm = host.prepareSound("Warm", {{4, 1}}), bright = host.prepareSound("Bright", {{4, 2}, {6, 18}, {7, 70}});
            host.selectSound(warm, 0); host.selectSound(bright, 1);
            p.play();
            const auto before = sounds(host);
            killWorkers();
            awaitRestart(p, [&] { return host.statistics().restarts >= 1; });
            check(sounds(host) == before, "Basic Synth: the prepared sounds come back with their IDs and names");
            Player reference(synth, "");
            const auto w = reference.host().prepareSound("Warm", {{4, 1}}), b = reference.host().prepareSound("Bright", {{4, 2}, {6, 18}, {7, 70}});
            reference.host().selectSound(w, 0); reference.host().selectSound(b, 1);
            check(same(p, reference, {Event::note(true, 60, .8, 0, 0), Event::note(true, 67, .8, 0, 1)}, 40), "Basic Synth: both channels play their sounds as before the crash");
            const auto later = host.prepareSound("Later", {{4, 0}});
            check(later != warm && later != bright && later != 0, "Basic Synth: a sound prepared after the restart gets a new ID");
        }
        { // Sampler: a sound whose sample resolves through the asset store, again after the restart.
            oscairn::assets::Store store(directory / "assets");
            const auto folder = directory / "samples"; std::filesystem::create_directories(folder);
            std::vector<float> tone(24000); for(size_t i = 0; i < tone.size(); ++i) tone[i] = float(.5 * std::sin(2 * M_PI * 220 * double(i) / rate));
            oscairn::writeWavFile(folder / "tone.wav", tone, 1, int(rate));
            const auto library = store.registerFolder(folder);
            auto use = [&](Player& x) { x.host().selectSound(x.host().prepareSound("Tone", {}, {}, {}, "<region> sample=tone.wav", library, "")); };
            Player p(sampler, "", &store); use(p);
            p.play();
            const auto before = sounds(p.host());
            killWorkers();
            awaitRestart(p, [&] { return p.host().statistics().restarts >= 1; });
            check(sounds(p.host()) == before, "Sampler: the sound comes back with its ID");
            Player reference(sampler, "", &store); use(reference);
            check(same(p, reference, {Event::note(true, 60, 1)}, 20), "Sampler: the restored sound plays its sample");
        }
        { // Delay: a root and an effect context in one worker; the context comes back with its values.
            using oscairn::Graph;
            auto make = [&](Graph& g, bool context) {
                const auto root = g.add(delay); const auto node = context ? g.addEffectContext(root) : root;
                g.host(node).setParameter(1, .25); g.host(node).setParameter(3, .9);
                g.connectAudio(oscairn::graphInput, node); g.connectAudio(node, oscairn::graphOutput); g.commit();
                return node;
            };
            const oscairn::AudioLayout ports{{{0, "Input", 2, 0}}, {{0, "Main", 2, 0}}};
            Graph g(rate, frames, oscairn::ProcessingMode::Realtime, {}, ports);
            const auto node = make(g, true);
            std::vector<float> in(frames), l(frames), r(frames), rl(frames), rr(frames);
            const float* inputs[]{in.data(), in.data()}; float* out[]{l.data(), r.data()}; float* ref[]{rl.data(), rr.data()};
            auto play = [&](Graph& x, float* const* o) { std::this_thread::sleep_for(std::chrono::microseconds(int64_t(1e6 * frames / rate))); x.process(inputs, 2, o, 2, frames, nullptr); x.service(); };
            for(int i = 0; i < 10; ++i) play(g, out);
            killWorkers();
            for(int i = 0; i < 1500 && g.host(node).statistics().restarts < 1; ++i) play(g, out);
            check(g.host(node).statistics().restarts >= 1, "Delay: restarted");
            for(int i = 0; i < 10; ++i) play(g, out);
            check(g.host(node).parameterValue(1) == .25 && g.host(node).parameterValue(3) == .9, "Delay: the context's values come back");
            check(g.host(node).effectResources() && g.host(node).effectResources()->prepared == 2, "Delay: the root and the context share one engine again");
            Graph alone(rate, frames, oscairn::ProcessingMode::Realtime, {}, ports);
            make(alone, false);
            // The delay time glides from its default at up to half a sample per sample: let both settle.
            in.assign(frames, 0.f);
            for(int i = 0; i < 400; ++i) { play(g, out); play(alone, ref); }
            bool equal = true; double energy = 0;
            for(int i = 0; i < 200; ++i)
            {
                in.assign(frames, 0.f); if(!i) in[0] = .5f;
                play(g, out); play(alone, ref);
                for(uint32_t f = 0; f < frames; ++f) { equal = equal && std::abs(l[f] - rl[f]) < 1e-6f; energy += std::abs(l[f]); }
            }
            check(equal && energy > 0, "Delay: the restored context sounds like a fresh delay with its values (energy " + std::to_string(energy) + ")");
        }
    }
}

int main(int argc, char** argv)
{
    if(argc < 7) { std::fprintf(stderr, "usage: %s WORKER NOTE_CRASH PROCESS_CRASH BASIC_SYNTH SAMPLER DELAY\n", argv[0]); return 2; }
    setenv("OSCAIRN_PLUGIN_WORKER", argv[1], 1);
    const auto directory = std::filesystem::temp_directory_path() / ("oscairn-recovery-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try
    {
        recovers(argv[2], directory);
        givesUp(argv[3]);
        natives(argv[4], argv[5], argv[6], directory / "natives");
        placement(argv[4]);
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); failures = 1; }
    std::error_code ignored; std::filesystem::remove_all(directory, ignored);
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Switching Performances (RFC 0021, "Fewer instances: reusing what is loaded"): how long the
// engine takes from performance.load until the next one plays, how many plugin instances it
// loads and how many it takes over, between two Performances that use the same plugins and two
// that share none; and that a reused instance sounds exactly as a fresh load of that state.
// Each shared Performance has four parts: two Basic Synths (native sound contexts) and two plain
// CLAP instruments (the prepared-instance pool), each through a Delay instance of its own. The
// other has four Samplers.
//   oscairn-performance-switch-test SYNTH.clap SAMPLER.clap DELAY.clap INSTRUMENT.clap
// With OSCAIRN_SWITCH_PERFORMANCES="A|B|C" (folders of saved Performances, A and B sharing
// plugins, C not), the same switches are timed with those too.
#include "oscairn/session.h"
#include "oscairn/plugin_host.h"
#include "oscairn/wav_file.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 128;

    struct Audio
    {
        std::array<float, frames> l{}, r{};
        float* out[2]{l.data(), r.data()};
        bool sounding() const { return std::any_of(l.begin(), l.end(), [](float x) { return x != 0; }); }
        void run(Session& s)
        {
            s.service(); forbidAllocation = true;
            const bool ok = s.process(out, frames);
            forbidAllocation = false; check(ok, "render");
        }
    };
    void blocks(Session& s, int n) { Audio a; for(int i = 0; i < n; ++i) a.run(s); }
    // Long enough for every plugin to have been silent the three seconds ringing out asks.
    void quiet(Session& s) { blocks(s, int(48000 * 3.2 / frames)); }

    void shared(const fs::path& file, int variant, const char* synth, const char* delay, const char* instrument)
    {
        Session s(48000, frames);
        for(uint32_t i = 0; i < 4; ++i)
        {
            const auto name = std::to_string(variant) + "-" + std::to_string(i);
            const auto n = s.add("instrument " + name, i % 2 ? instrument : synth), fx = s.add("delay " + name, delay);
            PartInput keys; keys.zones = {NoteZone{uint8_t(32 * i), uint8_t(32 * i + 31)}}; // a quarter of the keyboard each
            const auto part = s.addPart("Part " + name, keys);
            s.insertIntoPart(part, 0, n); s.insertIntoPart(part, 1, fx); s.commit();
            if(i % 2) check(s.setParameter(n, 7, .3 + .2 * variant + .05 * i), "instrument gain");
            else check(s.selectSound(n, s.prepareSound(n, "Saw " + name, {{4, double(1 + variant)}, {5, double(3 + i)}})), "select a sound");
            check(s.setParameter(fx, 1, .1 + .03 * variant + .02 * i) && s.setParameter(fx, 2, .3 + .1 * variant), "delay settings");
        }
        blocks(s, 2);
        s.savePerformance(file.string());
    }
    void other(const fs::path& file, const char* sampler, const fs::path& samples)
    {
        Session s(48000, frames);
        const auto folder = s.registerAssetFolder(samples.string());
        for(uint32_t i = 0; i < 4; ++i)
        {
            const auto n = s.add("sampler " + std::to_string(i), sampler);
            const auto part = s.addPart("Part " + std::to_string(i));
            s.insertIntoPart(part, 0, n); s.commit();
            check(s.selectSound(n, s.prepareSound(n, "Sample", {}, {}, {}, "<region> sample=sample.wav ampeg_release=0.2", folder.library, folder.directory)), "sampler sound");
        }
        blocks(s, 2);
        s.savePerformance(file.string());
    }

    // `count` plain CLAP instruments, a part of three keys each.
    void many(const fs::path& file, int variant, const char* instrument, uint32_t count)
    {
        Session s(48000, frames);
        for(uint32_t i = 0; i < count; ++i)
        {
            const auto n = s.add("instrument " + std::to_string(i), instrument);
            PartInput keys; keys.zones = {NoteZone{uint8_t(3 * i), uint8_t(3 * i + 2)}};
            const auto part = s.addPart("Part " + std::to_string(i), keys);
            s.insertIntoPart(part, 0, n); s.commit();
            check(s.setParameter(n, 7, .2 + .3 * variant + .01 * i), "instrument gain");
        }
        blocks(s, 2);
        s.savePerformance(file.string());
    }

    struct Switch { double ms = 0; uint64_t loaded = 0, reused = 0, kept = 0; };
    Switch load(Session& s, const fs::path& to)
    {
        Switch w;
        const auto loaded = pluginInstancesLoaded(), reused = s.reusedInstances(), kept = s.keptStates();
        const auto start = std::chrono::steady_clock::now();
        s.loadPerformance(to.string());
        w.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        w.loaded = pluginInstancesLoaded() - loaded; w.reused = s.reusedInstances() - reused; w.kept = s.keptStates() - kept;
        return w;
    }
    void report(const char* what, const Switch& w) { std::printf("%-52s %7.1f ms, %2llu instances loaded, %2llu reused (%llu holding their state)\n", what, w.ms, (unsigned long long)w.loaded, (unsigned long long)w.reused, (unsigned long long)w.kept); }

    // The same notes into a Session that switched and one that opened `file` fresh: every sample equal.
    void compare(Session& switched, const fs::path& file, const char* what)
    {
        Session fresh(48000, frames);
        fresh.loadPerformance(file.string());
        Audio x, y; double energy = 0; bool same = true;
        for(int block = 0; block < 600 && same; ++block)
        {
            // A note for each part: they have a quarter of the keyboard each.
            for(const int key : {20, 60, 70, 100})
            {
                if(block == 3 + key % 7) { switched.note(true, key, .8); fresh.note(true, key, .8); }
                if(block == 150 + key % 5) { switched.note(false, key, 0); fresh.note(false, key, 0); }
            }
            x.run(switched); y.run(fresh);
            if(x.l != y.l || x.r != y.r) { std::fprintf(stderr, "  different at block %d\n", block); same = false; }
            for(auto v : x.l) energy += v * v;
        }
        check(same, std::string(what) + ": a reused instance sounds as a fresh load of that state");
        check(energy > 1e-3, std::string(what) + ": audible");
    }

    // Resident memory of this process and its plugin workers, in MiB, from ps; 0 where there is none.
    double resident()
    {
#if defined(__unix__) || defined(__APPLE__)
        std::map<long, std::pair<long, long>> all; // pid: parent, KiB
        if(FILE* f = popen("ps -A -o pid= -o ppid= -o rss=", "r"))
        {
            long pid, parent, kib;
            while(std::fscanf(f, "%ld %ld %ld", &pid, &parent, &kib) == 3) all[pid] = {parent, kib};
            pclose(f);
        }
        auto ours = [&](long pid) { for(int depth = 0; pid > 1 && depth < 8; ++depth) { if(pid == long(getpid())) return true; const auto it = all.find(pid); if(it == all.end()) return false; pid = it->second.first; } return false; };
        long kib = 0;
        for(const auto& [pid, x] : all) if(ours(pid)) kib += x.second;
        return kib / 1024.0;
#else
        return 0;
#endif
    }
    void memory(const char* what, Session& s) { std::printf("%-52s %7.0f MiB resident, %zu instances kept warm\n", what, resident(), s.warmInstances()); }

    void timeOnly(Session& s, const std::vector<fs::path>& p)
    {
        // Real Performances (OSCAIRN_SWITCH_PERFORMANCES): A cold, then A and B (sharing plugins) in
        // turn five times, each silent before the change, then C (sharing little) and A again.
        report("real: open A (cold)", load(s, p[0])); quiet(s);
        memory("real: A open", s);
        std::vector<Switch> ab, ba;
        for(int round = 0; round < 5; ++round) { ab.push_back(load(s, p[1])); quiet(s); ba.push_back(load(s, p[0])); quiet(s); }
        memory("real: A open after switching with B", s);
        auto median = [](std::vector<Switch> v) { std::sort(v.begin(), v.end(), [](const Switch& x, const Switch& y) { return x.ms < y.ms; }); return v[v.size() / 2]; };
        report("real: A -> B, the first", ab[0]);
        report("real: B -> A, the first", ba[0]);
        report("real: A -> B, A silent (median of the other 4)", median({ab.begin() + 1, ab.end()}));
        report("real: B -> A, B silent (median of the other 4)", median({ba.begin() + 1, ba.end()}));
        report("real: A -> C (shares little)", load(s, p[2]));
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        report("real: C -> A (A's instruments kept warm)", load(s, p[0]));
    }
}

int main(int argc, char** argv)
{
    if(argc != 5) { std::fprintf(stderr, "usage: %s SYNTH.clap SAMPLER.clap DELAY.clap INSTRUMENT.clap\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory home; // a budget's measurements, not the user's
        ::setenv("OSCAIRN_DATA_HOME", home.path.c_str(), 1);
        std::printf("plugins in %s\n", placementPolicy().worker.empty() ? "the engine" : "workers");
        state_files::TemporaryDirectory temp;
        const auto samples = temp.path / "samples"; fs::create_directories(samples);
        { std::vector<float> wave(48000); for(size_t f = 0; f < wave.size(); ++f) wave[f] = float(.3 * ((f * 3) % 200) / 200.0 - .15); writeWavFile(samples / "sample.wav", wave, 1, 48000); }
        const auto a = temp.path / "a", b = temp.path / "b", c = temp.path / "c";
        shared(a, 0, argv[1], argv[3], argv[4]); shared(b, 1, argv[1], argv[3], argv[4]); other(c, argv[2], samples);

        Session s(48000, frames);
        report("open A (cold)", load(s, a));
        blocks(s, 20); s.note(true, 60, 1); blocks(s, 20); s.note(false, 60, 0); quiet(s);
        // A is silent and B uses the same plugins. Keeping A's instances warm fits (no budget: the
        // 32-instance bound alone), so they are kept rather than taken over (RFC 0021's middle rule):
        // B loads its instruments, and its Delays become contexts of A's (slice 1).
        auto w = load(s, b); report("A -> B, A silent (same plugins)", w);
        check(w.reused == 4 && w.loaded == 4, "A's instruments kept, not taken over, by B");
        compare(s, b, "A -> B"); quiet(s);
        check(s.warmInstances() == 4, "A's instruments kept warm: " + std::to_string(s.warmInstances()));
        w = load(s, a); report("B -> A, B silent (A's kept warm)", w);
        check(w.reused == 8 && w.kept == 4 && w.loaded == 0, "A's instruments back, holding their state; B's kept warm");
        compare(s, a, "B -> A"); quiet(s);
        w = load(s, b); report("A -> B again (B's kept warm)", w);
        check(w.reused == 8 && w.kept == 4 && w.loaded == 0, "B's instruments back, holding their state");
        compare(s, b, "A -> B again"); quiet(s);
        check(s.warmInstances() == 4 && s.memoryPressure() && s.warmInstances() == 0, "A's instruments kept warm, let go on memory pressure");
        // A cut (the two would not fit together) keeps nothing warm, so it takes over every free
        // playing instance.
        s.setBudget({64, 0});
        s.setBudget({64, s.estimate("").memory + s.estimate(a.string()).memory - 1});
        check(!s.estimate(a.string()).seamless, "B -> A cuts");
        w = load(s, a); report("B -> A, a cut", w);
        check(w.reused == 8 && w.loaded == 0 && w.kept == 0, "a cut takes over every free playing instance");
        compare(s, a, "cut B -> A"); quiet(s);
        // A document that fails once its plugins are loaded leaves the playing instances alone:
        // they get its state only once all of it has been read.
        {
            const auto broken = temp.path / "broken"; fs::copy(b, broken, fs::copy_options::recursive);
            auto bytes = state_files::read(broken / "performance.json", 4 << 20); std::string text(bytes.begin(), bytes.end());
            const auto at = text.find("\"scene\":0"); check(at != std::string::npos, "the broken copy's selected scene");
            text.replace(at, 9, "\"scene\":9"); state_files::write(broken / "performance.json", text.data(), text.size());
            auto values = [&] { std::vector<double> v; for(const auto& [n, name] : s.graphState().names) for(const auto& p : s.parameters(n)) v.push_back(s.parameter(n, p.id)); return v; };
            const auto before = values(); const auto revision = s.graphState().revision; bool failed = false;
            try { s.loadPerformance(broken.string()); } catch(const std::exception&) { failed = true; }
            check(failed && s.graphState().revision == revision && values() == before, "a failed load changes nothing that plays");
        }
        // A load that fails in that last step, its fourth instrument refusing its state even in a
        // fresh instance, once the first three playing instances it took over were given theirs:
        // they get their own back, so A sounds exactly as before the attempt.
        {
            const auto refusing = temp.path / "refusing"; fs::copy(b, refusing, fs::copy_options::recursive);
            const auto bytes = state_files::read(refusing / "performance.json", 4 << 20); const std::string text(bytes.begin(), bytes.end());
            const auto name = text.find("\"name\":\"instrument 1-3\""), key = text.rfind("\"key\":\"", name);
            check(name != std::string::npos && key != std::string::npos, "the fourth instrument's key");
            const auto node = text.substr(key + 7, text.find('"', key + 7) - key - 7);
            const std::string bad = "state 1\nnot a snapshot";
            state_files::write(refusing / "nodes" / node / "snapshot", bad.data(), bad.size());
            s.setBudget({64, s.estimate("").memory + s.estimate(refusing.string()).memory - 1}); // a cut: it takes every free instance
            check(!s.estimate(refusing.string()).seamless, "A -> the refusing copy cuts");
            bool failed = false;
            try { s.loadPerformance(refusing.string()); } catch(const std::exception& e) { failed = std::string(e.what()).find("instrument 1-3") != std::string::npos; }
            check(failed, "the load fails on the fourth instrument");
            compare(s, a, "a load refused after taking instances over"); quiet(s);
        }
        s.setBudget({});
        // A note held on the second part through the change: its instrument and Delay ring out on
        // the old instances, so its instrument loads afresh (its Delay becomes a context of the
        // playing engine, slice 1); the other parts' are taken over.
        s.note(true, 60, 1); blocks(s, 10);
        w = load(s, b); report("A -> B, a note held on A's second part", w);
        check(w.reused == 4 && w.loaded == 4, "A's instances kept, the one still sounding too");
        { Audio audio; bool rang = false; for(int i = 0; i < 50; ++i) { audio.run(s); rang = rang || audio.sounding(); } check(rang, "the held note plays on through the change"); }
        s.note(false, 60, 0);
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        check(!s.leaving(), "the old instances go once rung out");
        // Nothing shared.
        w = load(s, c); report("B -> C (no plugin shared)", w);
        check(w.reused == 0 && w.loaded == 4, "nothing to take over");
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        // A's and B's instruments, rung out, were kept warm (Delays, engines with contexts, are not).
        check(s.warmInstances() == 8, "A's and B's instruments kept warm once rung out: " + std::to_string(s.warmInstances()));
        // Back to B: they hold its state already, so they are reset instead of given it again.
        w = load(s, b); report("C -> B (B's instruments kept warm)", w);
        check(w.reused == 4 && w.kept == 4 && w.loaded == 4 && s.warmInstances() == 4, "the instances kept warm taken over, not given their state again; the Delays loaded");
        compare(s, b, "kept warm -> B");
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        // Memory short: what is kept warm goes first (RFC 0025).
        check(s.warmInstances() == 8 && s.memoryPressure() && s.warmInstances() == 0, "C's Samplers and A's instruments kept warm, let go on memory pressure");

        // Past the 32-instance bound a playing instance is taken over: 34 parts, one holding a note,
        // so 33 free; 32 are kept warm and one is taken. The one sounding is never taken.
        {
            const auto m = temp.path / "m", n = temp.path / "n";
            many(m, 0, argv[4], 34); many(n, 1, argv[4], 34);
            Session t(48000, frames);
            load(t, m); quiet(t);
            t.note(true, 1, 1); blocks(t, 10);
            w = load(t, n); report("34 instruments, one sounding", w);
            check(w.reused == 1 && w.loaded == 33, "one free instance past the bound taken over, the sounding one not");
            t.note(false, 1, 0);
            for(int i = 0; i < int(48000 * 31 / frames) && t.leaving(); ++i) blocks(t, 1);
            check(t.warmInstances() == 32, "32 kept warm: " + std::to_string(t.warmInstances()));
        }

        if(const char* real = std::getenv("OSCAIRN_SWITCH_PERFORMANCES"); real && *real)
        {
            std::vector<fs::path> p; std::string all(real);
            for(size_t at = 0; at <= all.size();) { const auto bar = std::min(all.find('|', at), all.size()); p.emplace_back(all.substr(at, bar - at)); at = bar + 1; }
            if(p.size() != 3) throw std::runtime_error("OSCAIRN_SWITCH_PERFORMANCES names three folders, A|B|C");
            Session r(48000, frames);
            timeOnly(r, p);
        }
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(failures) return 1;
    std::printf("ok: Performance switches measured\n");
    return 0;
}

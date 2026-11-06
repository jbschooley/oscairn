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
#include <string>

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

    struct Switch { double ms = 0; uint64_t loaded = 0, reused = 0; };
    Switch load(Session& s, const fs::path& to)
    {
        Switch w;
        const auto loaded = pluginInstancesLoaded(), reused = s.reusedInstances();
        const auto start = std::chrono::steady_clock::now();
        s.loadPerformance(to.string());
        w.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        w.loaded = pluginInstancesLoaded() - loaded; w.reused = s.reusedInstances() - reused;
        return w;
    }
    void report(const char* what, const Switch& w) { std::printf("%-52s %7.1f ms, %2llu instances loaded, %2llu reused\n", what, w.ms, (unsigned long long)w.loaded, (unsigned long long)w.reused); }

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

    void timeOnly(Session& s, const std::vector<fs::path>& p)
    {
        // Real Performances (OSCAIRN_SWITCH_PERFORMANCES): A cold, then A and B (sharing plugins) in
        // turn five times, each silent before the change, then C (sharing little) and A again.
        report("real: open A (cold)", load(s, p[0])); quiet(s);
        std::vector<Switch> ab, ba;
        for(int round = 0; round < 5; ++round) { ab.push_back(load(s, p[1])); quiet(s); ba.push_back(load(s, p[0])); quiet(s); }
        auto median = [](std::vector<Switch> v) { std::sort(v.begin(), v.end(), [](const Switch& x, const Switch& y) { return x.ms < y.ms; }); return v[v.size() / 2]; };
        report("real: A -> B, A silent (median of 5)", median(ab));
        report("real: B -> A, B silent (median of 5)", median(ba));
        report("real: A -> C (shares little)", load(s, p[2])); quiet(s); blocks(s, 50);
        report("real: C -> A", load(s, p[0]));
    }
}

int main(int argc, char** argv)
{
    if(argc != 5) { std::fprintf(stderr, "usage: %s SYNTH.clap SAMPLER.clap DELAY.clap INSTRUMENT.clap\n", argv[0]); return 2; }
    try
    {
        std::printf("plugins in %s\n", placementPolicy().worker.empty() ? "the engine" : "workers");
        state_files::TemporaryDirectory temp;
        const auto samples = temp.path / "samples"; fs::create_directories(samples);
        { std::vector<float> wave(48000); for(size_t f = 0; f < wave.size(); ++f) wave[f] = float(.3 * ((f * 3) % 200) / 200.0 - .15); writeWavFile(samples / "sample.wav", wave, 1, 48000); }
        const auto a = temp.path / "a", b = temp.path / "b", c = temp.path / "c";
        shared(a, 0, argv[1], argv[3], argv[4]); shared(b, 1, argv[1], argv[3], argv[4]); other(c, argv[2], samples);

        Session s(48000, frames);
        report("open A (cold)", load(s, a));
        blocks(s, 20); s.note(true, 60, 1); blocks(s, 20); s.note(false, 60, 0); quiet(s);
        // A is silent: its instances are free, and B uses the same plugins.
        auto w = load(s, b); report("A -> B, A silent (same plugins)", w);
        check(w.reused == 8 && w.loaded == 0, "every instance of A taken over by B");
        compare(s, b, "A -> B"); quiet(s);
        w = load(s, a); report("B -> A, B silent", w);
        check(w.reused == 8 && w.loaded == 0, "every instance of B taken over by A");
        compare(s, a, "B -> A"); quiet(s);
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
        // A note held on the second part through the change: its instrument and Delay ring out on
        // the old instances, so its instrument loads afresh (its Delay becomes a context of the
        // playing engine, slice 1); the other parts' are taken over.
        s.note(true, 60, 1); blocks(s, 10);
        w = load(s, b); report("A -> B, a note held on A's second part", w);
        check(w.reused == 7 && w.loaded == 1, "the instances still sounding are not taken");
        { Audio audio; bool rang = false; for(int i = 0; i < 50; ++i) { audio.run(s); rang = rang || audio.sounding(); } check(rang, "the held note plays on through the change"); }
        s.note(false, 60, 0);
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        check(!s.leaving(), "the old instances go once rung out");
        // Nothing shared.
        w = load(s, c); report("B -> C (no plugin shared)", w);
        check(w.reused == 0 && w.loaded == 4, "nothing to take over");
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        w = load(s, a); report("C -> A", w);

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

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

    // Two Basic Synth parts, a quarter of the keyboard each, each playing the last of `sounds`
    // prepared sounds (native contexts beside its Default).
    void synths(const fs::path& file, int variant, const char* synth, int sounds = 1)
    {
        Session s(48000, frames);
        for(uint32_t i = 0; i < 2; ++i)
        {
            const auto name = std::to_string(variant) + "-" + std::to_string(i);
            const auto n = s.add("synth " + name, synth);
            PartInput keys; keys.zones = {NoteZone{uint8_t(32 * i), uint8_t(32 * i + 31)}};
            const auto part = s.addPart("Part " + name, keys);
            s.insertIntoPart(part, 0, n); s.commit();
            uint32_t id = 0;
            for(int k = 0; k < sounds; ++k) id = s.prepareSound(n, "Sound " + name + "-" + std::to_string(k), {{1, .3 + .2 * variant}, {4, double((variant + i + k) % 3)}, {5, double(3 + i + 2 * variant + k)}});
            check(s.selectSound(n, id), "select a sound");
        }
        blocks(s, 2);
        s.savePerformance(file.string());
    }
    // One Basic Synth part on the whole keyboard playing one sound: with `delay`, through a Delay
    // (a quarter beat, feedback and wet at half and more), at the part's `level`; `brightness`, or
    // one from the variant.
    void ringing(const fs::path& file, int variant, const char* synth, const char* delay, double level, double brightness = -1)
    {
        Session s(48000, frames);
        const auto n = s.add("synth", synth);
        const auto part = s.addPart("Part"); s.insertIntoPart(part, 0, n);
        if(delay)
        {
            const auto fx = s.add("delay", delay); s.insertIntoPart(part, 1, fx); s.commit();
            check(s.setParameter(fx, 1, .25) && s.setParameter(fx, 2, .5) && s.setParameter(fx, 3, .6), "delay settings");
        }
        s.commit(); s.setPartLevel(part, level, false);
        check(s.selectSound(n, s.prepareSound(n, "Ring " + std::to_string(variant), {{1, brightness < 0 ? .4 + .3 * variant : brightness}, {4, double(variant % 3)}})), "select a sound");
        blocks(s, 2);
        s.savePerformance(file.string());
    }
    struct Switch { double ms = 0; uint64_t loaded = 0, reused = 0, kept = 0, beside = 0; };
    Switch load(Session& s, const fs::path& to)
    {
        Switch w;
        const auto loaded = pluginInstancesLoaded(), reused = s.reusedInstances(), kept = s.keptStates(), beside = s.besideSounds();
        const auto start = std::chrono::steady_clock::now();
        s.loadPerformance(to.string());
        w.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        w.loaded = pluginInstancesLoaded() - loaded; w.reused = s.reusedInstances() - reused; w.kept = s.keptStates() - kept; w.beside = s.besideSounds() - beside;
        return w;
    }
    void report(const char* what, const Switch& w) { std::printf("%-52s %7.1f ms, %2llu instances loaded, %2llu reused (%llu holding their state, %llu with sounds beside)\n", what, w.ms, (unsigned long long)w.loaded, (unsigned long long)w.reused, (unsigned long long)w.kept, (unsigned long long)w.beside); }

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
        // A is silent and B uses the same plugins. A's Basic Synths get B's sounds beside their own
        // (slice 2). Keeping A's plain instruments warm fits (no budget: the 32-instance bound alone),
        // so they are kept rather than taken over (RFC 0021's middle rule): B loads its own, and its
        // Delays become contexts of A's (slice 1).
        auto w = load(s, b); report("A -> B, A silent (same plugins)", w);
        check(w.reused == 6 && w.beside == 2 && w.loaded == 2, "A's synths given B's sounds beside, A's other instruments kept, not taken over, by B");
        compare(s, b, "A -> B"); quiet(s);
        check(s.warmInstances() == 2, "A's plain instruments kept warm: " + std::to_string(s.warmInstances()));
        w = load(s, a); report("B -> A, B silent (A's kept warm)", w);
        check(w.reused == 8 && w.beside == 2 && w.kept == 2 && w.loaded == 0, "A's instruments back, holding their state; the synths given A's sounds beside; B's kept warm");
        compare(s, a, "B -> A"); quiet(s);
        w = load(s, b); report("A -> B again (B's kept warm)", w);
        check(w.reused == 8 && w.beside == 2 && w.kept == 2 && w.loaded == 0, "B's instruments back, holding their state");
        compare(s, b, "A -> B again"); quiet(s);
        check(s.warmInstances() == 2 && s.memoryPressure() && s.warmInstances() == 0, "A's plain instruments kept warm, let go on memory pressure");
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
            auto sounds = [&] { size_t n = 0; for(const auto& [id, name] : s.graphState().names) n += s.sounds(id).contexts.size(); return n; };
            const auto before = values(); const auto revision = s.graphState().revision; const auto held = sounds(); bool failed = false;
            try { s.loadPerformance(broken.string()); } catch(const std::exception&) { failed = true; }
            check(failed && s.graphState().revision == revision && values() == before, "a failed load changes nothing that plays");
            check(sounds() == held, "a failed load leaves no sound prepared beside: " + std::to_string(held) + " then " + std::to_string(sounds()));
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
        check(w.reused == 6 && w.beside == 2 && w.loaded == 2, "A's instances kept, the one still sounding too; the synths given B's sounds beside");
        { Audio audio; bool rang = false; for(int i = 0; i < 50; ++i) { audio.run(s); rang = rang || audio.sounding(); } check(rang, "the held note plays on through the change"); }
        s.note(false, 60, 0);
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        check(!s.leaving(), "the old instances go once rung out");
        // Nothing shared.
        w = load(s, c); report("B -> C (no plugin shared)", w);
        check(w.reused == 0 && w.loaded == 4, "nothing to take over");
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        // A's and B's plain instruments and B's synths (A's, given B's sounds), rung out, were kept warm
        // (Delays, engines with contexts, are not).
        check(s.warmInstances() == 6, "A's and B's instruments kept warm once rung out: " + std::to_string(s.warmInstances()));
        // Back to B: the plain ones hold its state already, so they are reset instead of given it
        // again; the synths, holding sounds from before too, are given it.
        w = load(s, b); report("C -> B (B's instruments kept warm)", w);
        check(w.reused == 4 && w.kept == 2 && w.loaded == 4 && s.warmInstances() == 2, "the instances kept warm taken over, the plain ones not given their state again; the Delays loaded");
        compare(s, b, "kept warm -> B");
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) blocks(s, 1);
        // Memory short: what is kept warm goes first (RFC 0025).
        check(s.warmInstances() == 6 && s.memoryPressure() && s.warmInstances() == 0, "C's Samplers and A's plain instruments kept warm, let go on memory pressure");

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

        // Native sound contexts across a change (RFC 0021, slice 2): D and E are the same two Basic
        // Synths with other sounds. E's sounds are prepared beside D's in D's instances: nothing is
        // loaded, no state is given, a note held on D rings out in its own sound, then D's go.
        {
            const auto d = temp.path / "d", e = temp.path / "e", f = temp.path / "f";
            synths(d, 0, argv[1]); synths(e, 1, argv[1]);
            synths(f, 2, argv[1], 7); // the Default and seven sounds each: never room beside another's
            Session t(48000, frames);
            report("synths: open D (cold)", load(t, d)); quiet(t);
            auto contexts = [&](const char* name) { return t.sounds(t.nodeId(name)).contexts; };
            t.note(true, 20, 1); blocks(t, 10); // held on D's first part through the change
            w = load(t, e); report("synths: D -> E, a note held on D's first part", w);
            check(w.loaded == 0 && w.reused == 2 && w.beside == 2 && w.kept == 0, "D's instances given E's sounds beside: nothing loaded, no state given");
            blocks(t, 20);
            {
                const auto c = contexts("synth 1-0"); uint32_t voices = 0; bool old = false;
                for(const auto& x : c) if(x.name == "Sound 0-0-0") { old = true; voices = x.activeVoices; }
                check(c.size() == 4 && old && voices > 0, "D's sound keeps its held voice beside E's two sounds and the Default: " + std::to_string(c.size()) + " sounds, " + std::to_string(voices) + " voices");
                const auto other = contexts("synth 1-1");
                check(other.size() == 3, "D's idle sound on the other part discarded at once: " + std::to_string(other.size()));
                Audio audio; bool rang = false; for(int i = 0; i < 20; ++i) { audio.run(t); rang = rang || audio.sounding(); }
                check(rang, "the held note plays on through the change");
            }
            t.note(false, 20, 0);
            quiet(t);
            check(contexts("synth 1-0").size() == 3, "D's sound discarded once rung out: " + std::to_string(contexts("synth 1-0").size()));
            compare(t, e, "synths D -> E"); quiet(t);
            w = load(t, d); report("synths: E -> D, E silent", w);
            check(w.loaded == 0 && w.beside == 2, "back to D: its sounds beside E's");
            compare(t, d, "synths E -> D"); quiet(t);
            w = load(t, e); report("synths: D -> E, D silent", w);
            check(w.loaded == 0 && w.beside == 2, "D -> E again: E's sounds beside D's");
            compare(t, e, "synths D -> E again"); quiet(t);
            // No room beside: F's nodes have eight sounds each, so they go by the rules before it
            // (E's instances kept warm, F's loaded), and back from F, E finds F's instances full.
            w = load(t, f); report("synths: E -> F, no room beside", w);
            check(w.beside == 0 && w.loaded == 2, "no sounds prepared beside without room");
            compare(t, f, "synths E -> F (no room)"); quiet(t);
            w = load(t, e); report("synths: F -> E, F's instances full", w);
            check(w.beside == 0 && w.loaded == 0 && w.reused == 2, "E's instances kept warm taken over instead");
            compare(t, e, "synths F -> E (full)"); quiet(t);
            // Other ports: a node whose saved note ports differ takes no instance; it fails as a fresh
            // load would, leaving what plays with no sound added.
            {
                const auto odd = temp.path / "odd"; fs::copy(d, odd, fs::copy_options::recursive);
                auto bytes = state_files::read(odd / "performance.json", 4 << 20); std::string text(bytes.begin(), bytes.end());
                const auto at = text.find("\"noteInputs\":1"); check(at != std::string::npos, "a synth's note inputs");
                text.replace(at, 14, "\"noteInputs\":2"); state_files::write(odd / "performance.json", text.data(), text.size());
                const size_t held = contexts("synth 1-0").size() + contexts("synth 1-1").size(); const auto beside = t.besideSounds();
                bool failed = false;
                try { t.loadPerformance(odd.string()); } catch(const std::exception& x) { failed = std::string(x.what()).find("note port layout changed") != std::string::npos; }
                check(failed && t.besideSounds() == beside, "other ports: not prepared beside, and refused as a fresh load is");
                check(contexts("synth 1-0").size() + contexts("synth 1-1").size() == held, "other ports: no sound left beside what plays");
            }
        }

        // The outgoing sounds ring out through the outgoing chain (RFC 0021, "Ringing out through the
        // outgoing chain"): G is a Basic Synth through a Delay at half level, H the same synth dry at
        // full level. A note held on G through the change to H: its tail must go through G's Delay and
        // level, the incoming sound through H's. The reference opens H from a copy of the synth, so
        // nothing is prepared beside: G's instance rings out on its own, as in a change between
        // Performances that share nothing. The two must sound the same, sample for sample.
        {
            const auto g = temp.path / "g", h = temp.path / "h", hcopy = temp.path / "h-copy";
            const auto synthCopy = temp.path / ("synth-copy" + fs::path(argv[1]).extension().string());
            fs::copy(argv[1], synthCopy, fs::copy_options::recursive);
            ringing(g, 0, argv[1], argv[3], .5); ringing(h, 1, argv[1], nullptr, 1); ringing(hcopy, 1, synthCopy.c_str(), nullptr, 1);
            Session t(48000, frames), r(48000, frames);
            load(t, g); load(r, g); quiet(t); quiet(r);
            t.note(true, 60, .8); r.note(true, 60, .8); blocks(t, 10); blocks(r, 10);
            const auto wt = load(t, h), wr = load(r, hcopy);
            report("ring out: G -> H, a note held", wt); report("ring out: G -> H's copy (reference)", wr);
            check(wt.beside == 1 && wt.loaded == 0, "G's synth given H's sound beside");
            check(wr.beside == 0 && wr.loaded == 1, "the reference loads H's copy");
            check(t.ringOutRoutes() == 2, "the held sound's ring-out routed into G's Delay: " + std::to_string(t.ringOutRoutes()) + " routes");
            {
                bool hidden = true;
                for(const auto& x : t.graphState().topology.audioRoutes) hidden = hidden && x.from.port != Graph::ringOutPort;
                check(hidden, "the ring-out routes are not in the public routing");
            }
            Audio x, y; bool same = true; double energy = 0, tail = 0, tms = 0, rms = 0;
            auto timed = [](Audio& a, Session& s, double& ms) { const auto at = std::chrono::steady_clock::now(); a.run(s); ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - at).count(); };
            for(int block = 0; block < int(48000 * 5 / frames) && same; ++block)
            {
                if(block == 20) { t.note(false, 60, 0); r.note(false, 60, 0); }
                if(block == 30) { t.note(true, 72, .7); r.note(true, 72, .7); } // the incoming sound, through H's chain
                if(block == 60) { t.note(false, 72, 0); r.note(false, 72, 0); }
                if(block < 600) { timed(x, t, tms); timed(y, r, rms); } else { x.run(t); y.run(r); }
                if(x.l != y.l || x.r != y.r) { std::fprintf(stderr, "  ring out: different at block %d\n", block); same = false; }
                for(auto v : x.l) { energy += v * v; if(block >= 600) tail += v * v; }
            }
            check(same, "ring out: the outgoing tail through G's Delay and level, the incoming sound through H's, as in separate instances");
            check(energy > 1e-3, "ring out: audible");
            check(tail > 0, "ring out: G's Delay still echoing after both voices ended");
            quiet(t);
            {
                // The same notes once the outgoing chain is gone: H's chain alone.
                Audio z; double alone = 0;
                for(int block = 0; block < 600; ++block)
                {
                    if(block == 30) t.note(true, 72, .7);
                    if(block == 60) t.note(false, 72, 0);
                    timed(z, t, alone);
                }
                std::printf("ring out: render per block, the first 600 blocks: %.4f ms ringing out through G's chain, %.4f ms in separate instances, %.4f ms H alone\n", tms / 600, rms / 600, alone / 600);
                quiet(t);
            }
            check(t.ringOutRoutes() == 0, "the ring-out routes go once the outgoing sound is discarded: " + std::to_string(t.ringOutRoutes()));
            {
                bool gone = true;
                for(const auto& c : t.sounds(t.nodeId("synth")).contexts) gone = gone && c.name != "Ring 0";
                check(gone, "the outgoing sound discarded");
            }
            compare(t, h, "ring out: G -> H, then H as a fresh load");
            // Back from H to G with a note held: H has no effects, so its tail goes straight to the
            // outputs at H's level, and only retiring its sound removes those routes. The reference
            // plays H from the synth itself and opens G's copy.
            const auto gcopy = temp.path / "g-copy";
            ringing(gcopy, 0, synthCopy.c_str(), argv[3], .5);
            load(r, h); quiet(r); quiet(t);
            t.note(true, 60, .8); r.note(true, 60, .8); blocks(t, 10); blocks(r, 10);
            const auto bt = load(t, g), br = load(r, gcopy);
            check(bt.beside == 1 && br.beside == 0, "back to G: H's synth given G's sound beside; the reference opens G's copy");
            check(t.ringOutRoutes() == 2, "H's held sound rings out straight to the outputs: " + std::to_string(t.ringOutRoutes()) + " routes");
            {
                bool hidden = true;
                for(const auto& x : t.graphState().topology.audioRoutes) hidden = hidden && x.from.port != Graph::ringOutPort;
                check(hidden, "the ring-out routes to the outputs are not in the public routing");
            }
            same = true; energy = 0;
            for(int block = 0; block < 600 && same; ++block)
            {
                if(block == 20) { t.note(false, 60, 0); r.note(false, 60, 0); }
                if(block == 30) { t.note(true, 72, .7); r.note(true, 72, .7); }
                if(block == 60) { t.note(false, 72, 0); r.note(false, 72, 0); }
                x.run(t); y.run(r);
                if(x.l != y.l || x.r != y.r) { std::fprintf(stderr, "  ring out back: different at block %d\n", block); same = false; }
                for(auto v : x.l) energy += v * v;
            }
            check(same, "ring out: H's dry tail at H's level, G's sound through G's Delay, as in separate instances");
            check(energy > 1e-3, "ring out back: audible");
            quiet(t);
            check(t.ringOutRoutes() == 0, "the routes to the outputs go once H's sound is discarded: " + std::to_string(t.ringOutRoutes()));
        }

        // Only one change's tail carries over (RFC 0005, "Ringing out through the outgoing chain"): A is
        // a dark Basic Synth sound, dry at full level, B the synth through a Delay at half level, C the
        // synth dry at another level. A note held on A through the change to B rings out at A's level;
        // a second note is held on B; then the change to C. A's tail must fade out within the choke
        // time without a click, and B's go on through B's Delay. The reference plays B fresh and opens
        // C's copy: where A's tail has ended the two must sound the same, sample for sample, and before,
        // the difference is A's tail alone.
        {
            const auto a = temp.path / "older-a", b = temp.path / "older-b", c = temp.path / "older-c", ccopy = temp.path / "older-c-copy";
            const auto synthCopy = temp.path / ("synth-copy" + fs::path(argv[1]).extension().string());
            ringing(a, 3, argv[1], nullptr, 1, 0); ringing(b, 0, argv[1], argv[3], .5);
            ringing(c, 2, argv[1], nullptr, .8); ringing(ccopy, 2, synthCopy.c_str(), nullptr, .8);
            Session t(48000, frames), r(48000, frames);
            load(t, a); quiet(t);
            t.note(true, 60, .8); blocks(t, 10);
            check(load(t, b).beside == 1, "older tail: A's synth given B's sound beside");
            load(r, b);
            blocks(t, 5); blocks(r, 5);
            t.note(true, 72, .7); r.note(true, 72, .7);
            Audio x, y; float last = 0, peak = 0, slope = 0; // A's tail before the second change: t less r
            for(int block = 0; block < 10; ++block)
            {
                x.run(t); y.run(r);
                for(uint32_t i = 0; i < frames; ++i)
                {
                    const float d = x.l[i] - y.l[i];
                    if(block) slope = std::max(slope, std::abs(d - last));
                    peak = std::max(peak, std::abs(d)); last = d;
                }
            }
            check(peak > .01f, "older tail: A's tail sounds before the second change: " + std::to_string(peak));
            const auto wt = load(t, c), wr = load(r, ccopy);
            check(wt.beside == 1 && wr.loaded == 1, "older tail: B's synth given C's sound beside; the reference loads C's copy");
            check(t.ringOutRoutes() == 4, "older tail: B's sound rings out into B's Delay, A's to the outputs: " + std::to_string(t.ringOutRoutes()) + " routes");
            const uint32_t choke = 144; // the SDK's choke fade, 3 ms at 48 kHz
            float step = 0, after = 0; bool same = true; double energy = 0;
            for(int block = 0; block < 600; ++block)
            {
                if(block == 5) { t.note(false, 72, 0); r.note(false, 72, 0); }
                if(block == 10) { t.note(true, 64, .7); r.note(true, 64, .7); } // the incoming sound, through C's chain
                if(block == 40) { t.note(false, 64, 0); r.note(false, 64, 0); }
                if(block == 3) check(t.ringOutRoutes() == 2, "older tail: A's routes go once its sound has faded and is discarded: " + std::to_string(t.ringOutRoutes()));
                x.run(t); y.run(r);
                for(uint32_t i = 0; i < frames; ++i)
                {
                    const uint32_t at = uint32_t(block) * frames + i;
                    const float d = x.l[i] - y.l[i];
                    if(at <= choke) step = std::max(step, std::abs(d - last));
                    else { after = std::max(after, std::abs(d)); same = same && x.l[i] == y.l[i] && x.r[i] == y.r[i]; }
                    last = d;
                }
                for(auto v : x.l) energy += v * v;
            }
            std::printf("older tail: A's tail peak %.4f, its largest step %.5f before the second change, %.5f in the fade\n", peak, slope, step);
            check(after < 1e-6f, "older tail: A's tail gone within the choke time: " + std::to_string(after));
            check(step < 1.5f * slope, "older tail: faded without a click: a step of " + std::to_string(step) + " against " + std::to_string(slope));
            check(same, "older tail: B's tail through B's Delay and level, C's sound through C's, as in separate instances");
            check(energy > 1e-3, "older tail: audible");
            quiet(t);
            check(t.ringOutRoutes() == 0, "older tail: the ring-out routes go once B's sound is discarded: " + std::to_string(t.ringOutRoutes()));
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

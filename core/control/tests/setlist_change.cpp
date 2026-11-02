// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The change from one setlist entry to the next (RFC 0021, seamless switching), measured: how long
// from the cue until the next Performance plays, and the process's memory before, just after and
// once the last one has rung out; loaded at the cue, and prepared ahead while the last one plays
// so the cue only publishes it. Each Performance has eight parts of the bundled engines: four
// Samplers each playing its own sample, four Basic Synths, each part through its own Delay context.
//   oscairn-setlist-change-test SYNTH.clap SAMPLER.clap DELAY.clap
#include "oscairn/session.h"
#include "oscairn/plugin_host.h"
#include "oscairn/wav_file.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 128, parts = 8;
    constexpr size_t sampleFrames = 4u << 20; // 16 MiB of float audio per Sampler part

    struct Audio
    {
        std::array<float, frames> l{}, r{};
        float* out[2]{l.data(), r.data()};
        bool sounding = false;
        void run(Session& s)
        {
            s.service(); forbidAllocation = true;
            const bool ok = s.process(out, frames);
            forbidAllocation = false; check(ok, "render");
            sounding = std::any_of(l.begin(), l.end(), [](float x) { return x != 0; });
        }
    };
    double mib(uint64_t bytes) { return double(bytes) / (1 << 20); }

    // A Performance of `parts` parts; `variant` gives each its own samples and patches.
    void build(const fs::path& file, int variant, const char* synth, const char* sampler, const char* delay, const fs::path& samples)
    {
        Session s(48000, frames);
        const auto folder = s.registerAssetFolder(samples.string());
        NodeId engine = 0;
        for(uint32_t i = 0; i < parts; ++i)
        {
            const auto name = std::to_string(variant) + "-" + std::to_string(i);
            const auto instrument = s.add("instrument " + name, i % 2 ? synth : sampler);
            const auto fx = engine ? s.addEffectContext("delay " + name, engine) : (engine = s.add("delay " + name, delay));
            const auto part = s.addPart("Part " + name);
            s.insertIntoPart(part, 0, instrument); s.insertIntoPart(part, 1, fx); s.commit();
            const auto sound = i % 2
                ? s.prepareSound(instrument, "Saw " + name, {{4, double(1 + variant)}, {5, double(3 + i)}})
                : s.prepareSound(instrument, "Sample " + name, {}, {}, {}, "<region> sample=sample-" + std::to_string(variant) + "-" + std::to_string(i) + ".wav ampeg_release=0.2",
                                 folder.library, folder.directory);
            check(s.selectSound(instrument, sound), "select a sound");
            check(s.setParameter(fx, 1, .1 + .02 * i) && s.setParameter(fx, 2, .3), "delay settings");
        }
        Audio audio; audio.run(s);
        s.savePerformance(file.string());
    }

    struct Change { double prepareMs = 0, cueMs = 0; uint64_t before = 0, after = 0, rungOut = 0; size_t instances = 0; bool heldRang = false; };
    // From `from` playing with a note held, the cue to `to`, prepared ahead or not: its time, and memory around it.
    Change change(Session& s, const fs::path& from, const fs::path& to, bool ahead)
    {
        Audio audio; Change c;
        s.loadPerformance(from.string());
        for(int i = 0; i < 20; ++i) audio.run(s);
        s.note(true, 60, 1); audio.run(s); audio.run(s);
        c.before = residentBytes();
        if(ahead)
        {
            const auto start = std::chrono::steady_clock::now();
            check(s.preparePerformance(to.string()), "prepared ahead");
            c.prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            for(int i = 0; i < 20; ++i) audio.run(s);
        }
        const auto start = std::chrono::steady_clock::now();
        s.loadPerformance(to.string());
        c.cueMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        c.after = residentBytes(); c.instances = s.graphState().topology.nodes.size() + s.leaving();
        for(int i = 0; i < 100; ++i) { audio.run(s); c.heldRang = c.heldRang || audio.sounding; }
        s.note(false, 60, 0);
        for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) audio.run(s);
        check(!s.leaving(), "the last Performance's plugins go once rung out");
        for(int i = 0; i < 10; ++i) audio.run(s); // the retired plans and their hosts are freed
        c.rungOut = residentBytes();
        // Counted, not judged by resident memory, which the allocator keeps after plugins go.
        check(s.heldRemoved() == 0, "the last Performance's plugins are freed once rung out: " + std::to_string(s.heldRemoved()) + " held");
        return c;
    }
}

int main(int argc, char** argv)
{
    if(argc != 4) { std::fprintf(stderr, "usage: %s SYNTH.clap SAMPLER.clap DELAY.clap\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory temp;
        const auto samples = temp.path / "samples"; fs::create_directories(samples);
        for(int variant = 0; variant < 2; ++variant) for(uint32_t i = 0; i < parts; i += 2)
        {
            std::vector<float> wave(sampleFrames);
            for(size_t f = 0; f < wave.size(); ++f) wave[f] = float(.3 * ((f * (3 + i + variant)) % 200) / 200.0 - .15);
            writeWavFile(samples / ("sample-" + std::to_string(variant) + "-" + std::to_string(i) + ".wav"), wave, 1, 48000);
        }
        const auto a = temp.path / "a", b = temp.path / "b";
        build(a, 0, argv[1], argv[2], argv[3], samples); build(b, 1, argv[1], argv[2], argv[3], samples);

        Session s(48000, frames);
        for(const bool ahead : {false, true})
        {
            std::vector<double> cues;
            for(int round = 0; round < 5; ++round)
            {
                const auto c = change(s, round % 2 ? b : a, round % 2 ? a : b, ahead);
                check(c.heldRang, "the held note plays on through the change");
                cues.push_back(c.cueMs);
                std::printf("%s: cue %.1f ms; %zu plugin instances; resident memory %.0f MiB before, %.0f MiB just after, %.0f MiB once rung out\n",
                            ahead ? "prepared ahead" : "loaded at the cue", c.cueMs, c.instances, mib(c.before), mib(c.after), mib(c.rungOut));
                if(ahead) std::printf("  (preparing took %.1f ms while the last one played)\n", c.prepareMs);
            }
            std::sort(cues.begin(), cues.end());
            std::printf("%s: median %.2f ms from the cue until the next Performance is published\n", ahead ? "prepared ahead" : "loaded at the cue", cues[cues.size() / 2]);
        }
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(failures) return 1;
    std::printf("ok: setlist change measured\n");
    return 0;
}

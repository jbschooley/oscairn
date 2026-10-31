// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0018 step 1: a choked note fades out over 3 ms instead of being cut, then its voice ends;
// CLAP's per-note volume expression scales one note. Through the shared sound-contexts helper,
// on the Basic Synth.
//   oscairn-choke-fade-test BASIC_SYNTH.clap
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <clap/events.h>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace oscairn;
using kick::clap::Event;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr double rate = 48000;
    constexpr uint32_t frames = 128;
    double rms(const std::vector<float>& x, size_t from, size_t to)
    {
        double sum = 0; for(size_t i = from; i < to; ++i) sum += double(x[i]) * x[i];
        return std::sqrt(sum / double(to - from));
    }
    Event noteEvent(uint16_t type, int key, uint32_t time)
    {
        clap_event_note_t n{}; n.header = {sizeof(n), time, CLAP_CORE_EVENT_SPACE_ID, type, 0};
        n.note_id = -1; n.port_index = 0; n.channel = 0; n.key = int16_t(key); n.velocity = type == CLAP_EVENT_NOTE_ON ? 1 : 0;
        return Event::from(n);
    }
}

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    try
    {
        // Plays a held note for `blocks`, then the event at frame 32 of the next block, then more
        // blocks; returns the left channel from the event's block on.
        auto play = [&](const std::vector<Event>& at, int before, int after, bool* ended = nullptr) {
            Graph g(rate, frames);
            const auto synth = g.add(argv[1]);
            g.connectNotes(graphInput, synth); g.connectAudio(synth, graphOutput); g.commit();
            std::vector<float> l(frames), r(frames), out;
            float* buffers[2]{l.data(), r.data()};
            const auto on = noteEvent(CLAP_EVENT_NOTE_ON, 60, 0);
            g.process(buffers, frames, nullptr, &on, 1);
            for(int b = 0; b < before; ++b) g.process(buffers, frames, nullptr);
            for(int b = 0; b <= after; ++b)
            {
                forbidAllocation = true;
                const bool ok = g.process(buffers, frames, nullptr, b ? nullptr : at.data(), b ? 0 : uint32_t(at.size()));
                forbidAllocation = false;
                check(ok, "render");
                out.insert(out.end(), l.begin(), l.end());
                uint32_t n = 0; const auto* events = g.host(synth).outputEvents(n);
                for(uint32_t i = 0; ended && i < n; ++i) if(events[i].header()->type == CLAP_EVENT_NOTE_END) *ended = true;
            }
            return out;
        };
        const auto steady = play({}, 40, 4);
        const double level = rms(steady, 32, 32 + 48);
        check(level > .01, "the held note sounds: " + std::to_string(level));

        bool ended = false;
        const auto choked = play({noteEvent(CLAP_EVENT_NOTE_CHOKE, 60, 32)}, 40, 4, &ended);
        // 3 ms is 144 frames: the first 48 after the choke keep most of the level; a cut is silent at once.
        // Each measured against the same frames of the note left alone, as the waveform itself rises and falls.
        const double fading = rms(choked, 32, 32 + 48) / rms(steady, 32, 32 + 48), after = rms(choked, 32 + 150, choked.size());
        check(fading > .6, "a choke fades rather than cuts: " + std::to_string(fading) + " of the level in its first millisecond");
        check(after == 0 && ended, "silent after 3 ms, and the note ended: " + std::to_string(after));
        const double late = rms(choked, 32 + 72, 32 + 120) / rms(steady, 32 + 72, 32 + 120);
        check(late < .6, "and falls through the fade, not all at its end: " + std::to_string(late) + " of the level in its second half");
        // No sample-to-sample jump at the choke larger than the note makes on its own.
        double steadyStep = 0, chokeStep = 0;
        for(size_t i = 33; i < 32 + 160; ++i) { steadyStep = std::max(steadyStep, double(std::abs(steady[i] - steady[i - 1]))); chokeStep = std::max(chokeStep, double(std::abs(choked[i] - choked[i - 1]))); }
        check(chokeStep <= steadyStep * 1.05, "no jump at the choke: " + std::to_string(chokeStep) + " against " + std::to_string(steadyStep));

        // Per-note volume: one half of the level.
        clap_event_note_expression_t volume{}; volume.header = {sizeof(volume), 32, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
        volume.expression_id = CLAP_NOTE_EXPRESSION_VOLUME; volume.note_id = -1; volume.port_index = 0; volume.channel = 0; volume.key = 60; volume.value = .5;
        const auto halved = play({Event::from(volume)}, 40, 4);
        const double ratio = rms(halved, 32 + 128, 32 + 256) / rms(steady, 32 + 128, 32 + 256);
        check(std::abs(ratio - .5) < .02, "per-note volume .5 halves the note: " + std::to_string(ratio));
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a choke fades out over 3 ms; per-note volume scales a note");
    return failures ? 1 : 0;
}

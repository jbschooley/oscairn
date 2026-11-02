// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A mono synth gliding one voice ends a new note's ID while its key is still down (Surge's
// legato sends NOTE_END at once). The key's release must still reach the plugin, or it keeps a
// voice for a key it believes is held, through Performance changes too.
//   oscairn-note-end-legato-test FIXTURE.clap
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace oscairn;
using kick::clap::Event;

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    setenv("KICK_FIXTURE_LEGATO", "1", 1);
    int failures = 0;
    for(const bool lowFirst : {true, false})
    {
        Graph graph(48000, 128);
        const auto node = graph.add(argv[1], "org.kick.test.tail");
        graph.connectNotes(graphInput, node); graph.connectAudio(node, graphOutput); graph.commit();
        std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
        auto play = [&](std::vector<Event> events) {
            forbidAllocation = true;
            const bool ok = graph.process(nullptr, 0, out, 2, 128, nullptr, events.data(), uint32_t(events.size()), true, nullptr);
            forbidAllocation = false;
            return ok;
        };
        play({Event::note(true, 60, 1, 0)}); play({Event::note(true, 67, 1, 0)});
        if(lowFirst) { play({Event::note(false, 60, 0, 0)}); play({Event::note(false, 67, 0, 0)}); }
        else { play({Event::note(false, 67, 0, 0)}); play({Event::note(false, 60, 0, 0)}); }
        for(int i = 0; i < 20; ++i) play({}); // past the 1024-frame tail
        if(l[127] != 0)
        {
            std::fprintf(stderr, "FAIL: released %s first, the legato voice still sounds: %f\n", lowFirst ? "the low key" : "the high key", l[127]);
            ++failures;
        }
    }
    if(!failures) std::puts("ok: a note ended by the plugin while its key is down still gets its release");
    return failures ? 1 : 0;
}

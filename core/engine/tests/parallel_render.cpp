// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0034: a graph whose parts render at the same time renders exactly what it renders one
// part after another: every sample, and the events each node gets and sends, in the same order.
// Run once with plugins in the engine and once with every plugin in a worker (-workers).
//   oscairn-parallel-render-test TRANSPOSE.clap SYNTH.clap DELAY.clap
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace oscairn;
using kick::clap::Event;

namespace
{
    void check(bool ok, const char* what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); } }

    // Each node's events, in and out, as the graph reports them: node, direction, time and type.
    struct Recorder final : Graph::Listener
    {
        std::vector<uint64_t> seen;
        void events(NodeId node, const Event* events, uint32_t count, bool emitted) noexcept override
        {
            for(uint32_t i = 0; i < count && seen.size() < seen.capacity(); ++i)
                seen.push_back(uint64_t(node) << 48 | uint64_t(emitted) << 47 | uint64_t(events[i].header()->type) << 32 | events[i].header()->time);
        }
    };

    struct Parts
    {
        Graph graph{48000, 256, ProcessingMode::Realtime};
        Recorder recorder;
        Parts(char** argv, bool parallel)
        {
            graph.setParallel(parallel);
            // Three parts from the input: one through a note effect, two straight; two go through
            // delays sharing one engine (effect contexts, which never render at the same time).
            const auto transpose = graph.add(argv[1]), a = graph.add(argv[2]), b = graph.add(argv[2]), c = graph.add(argv[2]);
            const auto delay = graph.add(argv[3]), context = graph.addEffectContext(delay);
            graph.connectNotes(graphInput, transpose, {NoteZone{.channels = 0x0003}});
            graph.connectNotes(transpose, a);
            graph.connectNotes(graphInput, b, {NoteZone{.channels = 0x0004}});
            graph.connectNotes(graphInput, c);
            graph.connectAudio(a, delay); graph.connectAudio(delay, graphOutput);
            graph.connectAudio(b, context); graph.connectAudio(context, graphOutput);
            graph.connectAudio(c, graphOutput);
            graph.host(delay).setParameter(1, .125); graph.host(context).setParameter(1, .25);
            graph.commit();
            recorder.seen.reserve(1 << 20);
            graph.setListener(&recorder);
        }
    };
}

int main(int argc, char** argv)
{
    check(argc == 4, "usage: TRANSPOSE.clap SYNTH.clap DELAY.clap");
    Parts serial(argv, false), parallel(argv, true);
    std::array<std::array<float, 256>, 2> x{}, y{};
    float* xs[]{x[0].data(), x[1].data()}; float* ys[]{y[0].data(), y[1].data()};
    uint32_t seed = 1; auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    double energy = 0; std::array<bool, 128> held{};
    for(int block = 0; block < 1500; ++block)
    {
        // Uneven blocks, some larger than a worker's shared buffer is not: all within maxFrames.
        const uint32_t frames = block % 3 ? 128 : 37;
        std::array<Event, 4> events; uint32_t count = 0;
        for(uint32_t time = 0; time < frames && count < events.size(); time += 1 + next() % 64)
        {
            const int key = 48 + int(next() % 24); const int channel = int(next() % 4);
            events[count++] = held[size_t(key)] ? Event::note(false, key, 0, time, channel) : Event::note(true, key, .5 + (next() % 50) / 100.0, time, channel);
            held[size_t(key)] = !held[size_t(key)];
        }
        forbidAllocation = true;
        const bool ok = serial.graph.process(xs, frames, nullptr, events.data(), count) & parallel.graph.process(ys, frames, nullptr, events.data(), count);
        forbidAllocation = false;
        check(ok, "both render");
        for(uint32_t c = 0; c < 2; ++c) for(uint32_t i = 0; i < frames; ++i) { check(x[c][i] == y[c][i], "every sample the same, rendered serially and in parallel"); energy += std::abs(x[c][i]); }
    }
    check(energy > 1, "the parts sound");
    check(serial.recorder.seen.size() > 1000 && serial.recorder.seen == parallel.recorder.seen, "the same events in and out of every node, in the same order");
    check(serial.graph.eventDrops() == 0 && parallel.graph.eventDrops() == 0, "no events dropped");
    std::puts("parallel rendering matches serial rendering");
    return 0;
}

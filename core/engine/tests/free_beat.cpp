// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0032 step 3: the free-running beat (oscairn-clap/beat.h). With the transport stopped at
// 120 BPM, the follower fixture's beat runs at two beats a second from the graph's start; a
// second follower added 100 ms later gets the same position at the same sample, so both reach
// beat 1 on the same sample (two arpeggiators' Sync Quantize starting together). The transport
// event's own position stays put. In the engine and in a worker, at 256- and 37-sample blocks.
//   oscairn-free-beat-test WORKER follower-fixture.clap
#include "oscairn/graph.h"
#include "oscairn/transport.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    void run(const std::string& worker, const std::string& fixture, uint32_t block, bool inWorker)
    {
        const std::string where = std::string(inWorker ? "worker" : "engine") + ", " + std::to_string(block) + "-sample blocks: ";
        oscairn::setPlacementPolicy({inWorker ? worker : std::string(), oscairn::Placement::Worker});
        constexpr double rate = 48000;
        constexpr int64_t later = 4800, length = 30000; // 100 ms; past beat 1 at sample 24000
        oscairn::Graph g(rate, block, oscairn::ProcessingMode::Offline);
        const auto first = g.add(fixture);
        g.commit();
        oscairn::NodeId second = 0;
        oscairn::Transport transport(rate, 120);
        transport.setPlaying(false);
        const auto stoppedAt = transport.current().song_pos_beats;
        std::vector<float> left(block), right(block), a, b;
        float* outputs[]{left.data(), right.data()};
        for(int64_t at = 0; at < length; at += block)
        {
            if(!second && at >= later) { second = g.add(fixture); g.commit(); }
            const auto t = transport.current();
            check(t.song_pos_beats == stoppedAt, where + "the stopped transport's position stays put");
            check(g.process(outputs, block, &t), where + "process");
            for(auto [node, into] : {std::pair{first, &a}, std::pair{second, &b}})
            {
                const float* seen = node ? g.audioOutput(node, 2) : nullptr;
                if(seen) into->insert(into->end(), seen, seen + block); else into->insert(into->end(), block, -1.f);
            }
            transport.advance(block);
        }
        bool runs = true;
        for(int64_t i = 0; i < length; ++i) runs = runs && std::abs(a[size_t(i)] - float(i * 2 / rate)) < 1e-4f;
        check(runs, where + "the beat runs at the tempo while the transport is stopped");
        const auto start = size_t(((later + block - 1) / block) * block); // the second follower's first block
        bool same = b[start - 1] == -1.f;
        for(size_t i = start; i < size_t(length); ++i) same = same && a[i] == b[i];
        check(same, where + "the follower added 100 ms later has the same beat at every sample");
        auto crossing = [](const std::vector<float>& v) { size_t i = 0; while(i < v.size() && v[i] < 1.f) ++i; return i; };
        check(crossing(a) == crossing(b) && std::abs(double(crossing(a)) - 24000) <= 1,
              where + "both reach beat 1 on the same sample: " + std::to_string(crossing(a)) + " and " + std::to_string(crossing(b)));
    }
}

int main(int argc, char** argv)
{
    if(argc != 3) return std::fprintf(stderr, "usage: oscairn-free-beat-test WORKER follower-fixture.clap\n"), 2;
    for(bool inWorker : {false, true})
        for(uint32_t block : {256u, 37u}) run(argv[1], argv[2], block, inWorker);
    if(failures) return 1;
    std::printf("ok: the free-running beat runs while stopped, the same for plugins added at different times; engine and worker, two block sizes\n");
    return 0;
}

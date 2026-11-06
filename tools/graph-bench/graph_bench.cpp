// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0034: the time a graph block takes, rendering its parts one after another and at the
// same time. Blocks are paced at the real block rate on a real-time thread, as an audio
// device's callback would run them; nothing is played.
//
//   oscairn-graph-bench [--seconds S] [--frames F] [--parts N] [--voices V] SYNTH.clap DELAY.clap
//       N parts, each a synth playing a held chord of V notes (6) into its own delay. Serial and parallel
//       alternate every half second on the same graph.
//   oscairn-graph-bench [--seconds S] [--frames F] [--warm S] [--idle] --performance DIR
//       A saved Performance, through a Session, with a chord held on every channel (none with
//       --idle). One mode per run: OSCAIRN_GRAPH_SERIAL=1 renders serially. Prints the
//       engine process's CPU use too.
//
// OSCAIRN_PLUGIN_WORKER puts every plugin in a worker, as in the app.
#include "oscairn/graph.h"
#include "oscairn/realtime.h"
#include "oscairn/session.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <sys/resource.h>

namespace
{
    using Clock = std::chrono::steady_clock;
    struct Stats { std::vector<double> us; };
    void report(const char* name, Stats& s)
    {
        if(s.us.empty()) return;
        std::sort(s.us.begin(), s.us.end());
        double sum = 0; for(double v : s.us) sum += v;
        auto at = [&](double q) { return s.us[std::min(s.us.size() - 1, size_t(q * double(s.us.size())))]; };
        std::printf("%-10s blocks %6zu  mean %7.1f us  p50 %7.1f  p95 %7.1f  p99 %7.1f  max %7.1f\n", name, s.us.size(), sum / double(s.us.size()), at(.5), at(.95), at(.99), s.us.back());
    }
    double cpuSeconds()
    {
        rusage u{}; getrusage(RUSAGE_SELF, &u);
        return double(u.ru_utime.tv_sec + u.ru_stime.tv_sec) + double(u.ru_utime.tv_usec + u.ru_stime.tv_usec) * 1e-6;
    }
}

int main(int argc, char** argv)
{
    double seconds = 10, rate = 48000, warmSeconds = 1; uint32_t frames = 128, parts = 4, voices = 6; bool idle = false;
    std::string performance; std::vector<std::string> plugins;
    for(int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        auto value = [&] { return i + 1 < argc ? argv[++i] : (std::fprintf(stderr, "%s needs a value\n", a.c_str()), std::exit(2), nullptr); };
        if(a == "--seconds") seconds = std::atof(value());
        else if(a == "--frames") frames = uint32_t(std::atoi(value()));
        else if(a == "--parts") parts = uint32_t(std::atoi(value()));
        else if(a == "--voices") voices = uint32_t(std::atoi(value()));
        else if(a == "--idle") idle = true;
        else if(a == "--warm") warmSeconds = std::atof(value());
        else if(a == "--performance") performance = value();
        else plugins.push_back(a);
    }
    if(performance.empty() && plugins.size() != 2) { std::fprintf(stderr, "usage: see the comment at the top of graph_bench.cpp\n"); return 2; }
    const auto period = std::chrono::duration<double>(frames / rate);
    std::vector<float> left(frames), right(frames); float* out[]{left.data(), right.data()};

    if(!performance.empty())
    {
        oscairn::Session session(rate, frames);
        session.loadPerformance(performance);
        std::vector<kick::clap::Event> chord;
        if(!idle) for(int c = 0; c < 16; ++c) for(int k : {48, 55, 60, 64}) chord.push_back(kick::clap::Event::note(true, k, .7, 0, c));
        Stats s; float level = 0;
        std::thread audio([&] {
            oscairn::worker::makeRealtime(1e6 * period.count());
            auto next = Clock::now(); const auto blocks = uint64_t(seconds / period.count());
            const double cpu = cpuSeconds(); const auto began = Clock::now();
            for(uint64_t b = 0; b < blocks; ++b)
            {
                std::this_thread::sleep_until(next); next += std::chrono::duration_cast<Clock::duration>(period);
                // The chord after --warm seconds (1), as sounds may still be loading, in pieces: a Session takes
                // at most maxInputEvents a block. Blocks count from twice that.
                const auto warm = uint64_t(warmSeconds / period.count());
                const size_t from = b < warm ? chord.size() : std::min(chord.size(), size_t(b - warm) * 32), to = std::min(chord.size(), from + 32);
                const auto t = Clock::now();
                session.process(out, frames, std::span(chord.data() + from, to - from));
                if(b >= 2 * warm) s.us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t).count());
                for(uint32_t i = 0; i < frames; ++i) level = std::max(level, std::abs(left[i]));
            }
            const double wall = std::chrono::duration<double>(Clock::now() - began).count();
            std::printf("engine process CPU: %.1f%% of one core over %.1f s\n", 100 * (cpuSeconds() - cpu) / wall, wall);
        });
        // The main thread services the session as the app's does, at its 5 ms pace.
        for(auto until = Clock::now() + std::chrono::duration<double>(seconds); Clock::now() < until;) { session.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        audio.join();
        const auto state = session.graphState();
        for(const auto& n : state.topology.nodes) std::printf("node %u %s (%s)\n", n.id, state.names.count(n.id) ? state.names.at(n.id).c_str() : "", n.plugin.id.c_str());
        const auto status = session.sessionStatus();
        for(const auto& w : status.workers) std::printf("worker %u %s: CPU %.2f s, last block %.0f us, most %.0f us\n", w.node, w.name.c_str(), w.cpu, w.block, w.blockMax);
        for(const auto& f : status.failures) std::printf("failed %u %s: %s\n", f.node, f.name.c_str(), f.reason.c_str());
        for(const auto& n : status.notices) std::printf("notice: %s\n", n.c_str());
        std::printf("loudest sample %.3f\n", level);
        report(std::getenv("OSCAIRN_GRAPH_SERIAL") ? "serial" : "parallel", s);
        return 0;
    }

    oscairn::Graph graph(rate, frames);
    for(uint32_t p = 0; p < parts; ++p)
    {
        const auto synth = graph.add(plugins[0]), delay = graph.add(plugins[1]);
        oscairn::NoteZone zone; zone.channels = uint16_t(1u << (p % 16));
        graph.connectNotes(oscairn::graphInput, synth, {zone});
        graph.connectAudio(synth, delay); graph.connectAudio(delay, oscairn::graphOutput);
    }
    graph.commit();
    std::vector<kick::clap::Event> chord;
    for(uint32_t p = 0; p < parts; ++p) for(uint32_t v = 0; v < voices; ++v) chord.push_back(kick::clap::Event::note(true, int(36 + v * 5 % 48), .7, 0, int(p % 16)));
    Stats serial, parallel;
    std::thread audio([&] {
        oscairn::worker::makeRealtime(1e6 * period.count());
        auto next = Clock::now(); const auto blocks = uint64_t(seconds / period.count()), half = uint64_t(.5 / period.count());
        for(uint64_t b = 0; b < blocks; ++b)
        {
            std::this_thread::sleep_until(next); next += std::chrono::duration_cast<Clock::duration>(period);
            const bool inParallel = (b / half) % 2;
            graph.setParallel(inParallel);
            const auto t = Clock::now();
            graph.process(out, frames, nullptr, b == 0 ? chord.data() : nullptr, b == 0 ? uint32_t(chord.size()) : 0);
            // The first half second warms up.
            if(b >= half) (inParallel ? parallel : serial).us.push_back(std::chrono::duration<double, std::micro>(Clock::now() - t).count());
        }
    });
    for(auto until = Clock::now() + std::chrono::duration<double>(seconds); Clock::now() < until;) { graph.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    audio.join();
    std::printf("%u parts, %u frames at %.0f Hz (a block is %.0f us), plugins %s\n", parts, frames, rate, 1e6 * period.count(), std::getenv("OSCAIRN_PLUGIN_WORKER") ? "in workers" : "in the engine");
    report("serial", serial); report("parallel", parallel);
    return 0;
}

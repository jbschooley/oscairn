// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 steps 3-4: a plugin in a worker that crashes, hangs or is too slow stops only its own
// node, is restarted, and is given up after repeated failures. A normal instrument plays next to
// it in the same graph, paced in real time; its sound must be identical to the same instrument
// playing alone, every block must finish within the block's time, and the failure must be
// detected and named.
//   oscairn-worker-failures-test WORKER NORMAL.clap CRASH.clap HANG.clap SLOW.clap [--no-timing]
#include <oscairn/graph.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    // The test thread plays the audio thread, with its real-time scheduling.
    void makeRealtime(double periodUs)
    {
#ifdef __APPLE__
        mach_timebase_info_data_t base; mach_timebase_info(&base);
        const auto ticks = [&](double us) { return uint32_t(us * 1000.0 * base.denom / base.numer); };
        thread_time_constraint_policy_data_t policy{ticks(periodUs), ticks(periodUs * .5), ticks(periodUs * .9), 1};
        thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#else
        (void)periodUs; sched_param p{}; p.sched_priority = 70; pthread_setschedparam(pthread_self(), SCHED_FIFO, &p);
#endif
    }

    void run(const char* normal, const char* failing, const char* expected, bool timing)
    {
        constexpr double rate = 48000; constexpr uint32_t frames = 128;
        const double period = frames / rate;
        oscairn::Graph graph(rate, frames), alone(rate, frames);
        const auto a = graph.add(normal), b = graph.add(failing), r = alone.add(normal);
        // The failing plugin gets no notes, so it is silent even in a block it does finish.
        for(auto [g, n] : {std::pair{&graph, a}, std::pair{&graph, b}, std::pair{&alone, r}})
        {
            if(n != b) g->connectNotes(oscairn::graphInput, n);
            g->connectAudio(n, oscairn::graphOutput);
        }
        graph.commit(); alone.commit();

        std::vector<float> l(frames), rr(frames), el(frames), er(frames);
        float* out[]{l.data(), rr.data()}; float* ref[]{el.data(), er.data()};
        uint32_t overruns = 0, block = 0; double worst = 0, energy = 0; bool same = true;
        const auto start = std::chrono::steady_clock::now();
        auto next = start;
        // Real time until the failing plugin has failed through its restarts (each failure takes up
        // to the 250 ms hang and slowness limits), at most 3 s; the baseline plays for a second.
        const auto length = std::chrono::seconds(expected ? 3 : 1);
        bool givenUp = false;
        for(; !givenUp && std::chrono::steady_clock::now() - start < length; ++block)
        {
            if(expected && block % 40 == 0) givenUp = graph.host(b).failure().find("not restarted") != std::string::npos;
            std::this_thread::sleep_until(next);
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(period));
            std::vector<kick::clap::Event> notes;
            if(block % 40 == 0) notes.push_back(kick::clap::Event::note(true, 60 + int(block / 40 % 12), .8, 5));
            if(block % 40 == 30) notes.push_back(kick::clap::Event::note(false, 60 + int(block / 40 % 12), 0, 7));
            const auto t0 = std::chrono::steady_clock::now();
            graph.process(out, frames, nullptr, notes.data(), uint32_t(notes.size()));
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            alone.process(ref, frames, nullptr, notes.data(), uint32_t(notes.size()));
            worst = std::max(worst, took); if(took > period) ++overruns;
            same = same && l == el && rr == er;
            for(float x : el) energy += double(x) * x;
        }
        const auto reason = expected ? graph.host(b).failure() : std::string();
        const auto name = std::string(std::strrchr(failing, '/') ? std::strrchr(failing, '/') + 1 : failing);
        check(same, name + ": the other instrument sounds exactly as when playing alone");
        check(energy > 0, name + ": the other instrument made sound");
        if(expected) check(reason.find(expected) != std::string::npos && reason.find("not restarted") != std::string::npos,
                           name + ": failure reported as \"" + expected + "\" and given up, got \"" + reason + "\"");
        if(timing) check(overruns == 0, name + ": " + std::to_string(overruns) + " blocks took longer than the block's time");
        std::printf("%s: %u blocks, worst %.0f us of %.0f us, overruns %u, failure: %s\n", name.c_str(), block, worst * 1e6, period * 1e6, overruns, reason.c_str());
    }
}

int main(int argc, char** argv)
{
    if(argc < 6) { std::fprintf(stderr, "usage: %s WORKER NORMAL CRASH HANG SLOW [--no-timing]\n", argv[0]); return 2; }
    const bool timing = !(argc > 6 && std::strcmp(argv[6], "--no-timing") == 0);
    setenv("OSCAIRN_PLUGIN_WORKER", argv[1], 1);
    makeRealtime(1e6 * 128 / 48000);
    try
    {
        run(argv[2], argv[2], nullptr, timing); // baseline: two healthy instruments
        run(argv[2], argv[3], "crashed", timing);
        run(argv[2], argv[4], "hung", timing);
        run(argv[2], argv[5], "too slow", timing);
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 step 1: the cost of handing an audio block to plugin worker processes and back.
// The parent plays the engine: at the real block rate it writes each worker's input into
// shared memory, wakes it, and waits for its output, either one worker after another (a chain
// in the graph) or all at once (independent branches). Workers are forked processes that run
// fixed busy work or a real CLAP plugin. Each worker times its own processing, so the report
// separates the hand-off overhead from the plugin's own cost.
//
//   oscairn-handoff-bench [--blocks N] [--frames F] [--rate HZ] [--work-us US] [--spin-us US]
//                         [--realtime] [--plugin PATH --id ID] [--workers 1,8,32] [--check]
#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/os_sync_wait_on_address.h>
#else
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#endif

namespace
{
    using Clock = std::chrono::steady_clock;
    constexpr uint32_t maxFrames = 1024;

    // One worker's shared block: sequence numbers for request/completion, then audio.
    struct alignas(64) Slot
    {
        std::atomic<uint32_t> request{0};
        alignas(64) std::atomic<uint32_t> done{0};
        alignas(64) uint32_t frames = 0;
        double computeUs = 0;   // the worker's own processing time for the last block
        int failed = 0;
        float in[2][maxFrames];
        float out[2][maxFrames];
    };
    static_assert(std::atomic<uint32_t>::is_always_lock_free);

    // Futex-style wait for a value to change, shared between processes; a timeout bounds hangs.
    void waitWhileEqual(std::atomic<uint32_t>& word, uint32_t value, double spinUs)
    {
        const auto spinUntil = Clock::now() + std::chrono::duration<double, std::micro>(spinUs);
        while(word.load(std::memory_order_acquire) == value && Clock::now() < spinUntil) {}
        while(word.load(std::memory_order_acquire) == value)
        {
#ifdef __APPLE__
            os_sync_wait_on_address_with_timeout(&word, value, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME, 1000000000ull);
#else
            timespec timeout{1, 0};
            syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT, value, &timeout, nullptr, 0);
#endif
        }
    }
    void wake(std::atomic<uint32_t>& word)
    {
#ifdef __APPLE__
        os_sync_wake_by_address_any(&word, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#else
        syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE, 1, nullptr, nullptr, 0);
#endif
    }

    // Real-time scheduling as an audio thread would have it; reports whether the OS allowed it.
    bool makeRealtime(double periodUs)
    {
#ifdef __APPLE__
        mach_timebase_info_data_t base; mach_timebase_info(&base);
        const auto ticks = [&](double us) { return uint32_t(us * 1000.0 * base.denom / base.numer); };
        thread_time_constraint_policy_data_t policy{ticks(periodUs), ticks(periodUs * .5), ticks(periodUs * .9), 1};
        return thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
        (void)periodUs; sched_param p{}; p.sched_priority = 70;
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
#endif
    }

    struct Options
    {
        uint32_t blocks = 5000, frames = 128; double rate = 48000, workUs = 50, spinUs = 0; bool realtime = false, check = false;
        std::string plugin, id; std::vector<uint32_t> workers{1, 8, 32};
    };

    // Worker process: wait for a request, process the block, publish the result.
    [[noreturn]] void worker(Slot& slot, const Options& o)
    {
        if(o.realtime) makeRealtime(o.frames / o.rate * 1e6);
        std::unique_ptr<kick::clap::Host> host;
        if(!o.plugin.empty())
        {
            const auto catalog = kick::clap::inspect(o.plugin);
            const auto found = std::find_if(catalog.plugins.begin(), catalog.plugins.end(), [&](const auto& d) { return d.id == o.id; });
            host = std::make_unique<kick::clap::Host>();
            if(found == catalog.plugins.end() || !host->load(*found, o.rate, o.frames)) { slot.failed = 1; slot.done.store(~0u, std::memory_order_release); wake(slot.done); _exit(1); }
        }
        uint32_t seen = 0; bool noteSent = false;
        for(;;)
        {
            waitWhileEqual(slot.request, seen, o.spinUs);
            seen = slot.request.load(std::memory_order_acquire);
            if(seen == ~0u) _exit(0);
            const auto start = Clock::now();
            if(host)
            {
                float* outputs[]{slot.out[0], slot.out[1]};
                const auto note = kick::clap::Event::note(true, 60, .8);
                if(!host->process(nullptr, 0, outputs, 2, slot.frames, noteSent ? nullptr : &note, noteSent ? 0 : 1)) slot.failed = 1;
                noteSent = true;
                host->service();
            }
            else
            {
                // Fixed work standing in for a plugin's DSP, touching the block like a plugin would.
                const auto until = start + std::chrono::duration<double, std::micro>(o.workUs);
                do for(uint32_t i = 0; i < slot.frames; ++i) { slot.out[0][i] = slot.in[0][i] * .5f; slot.out[1][i] = slot.in[1][i] * .5f; }
                while(Clock::now() < until);
            }
            slot.computeUs = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            slot.done.store(seen, std::memory_order_release);
            wake(slot.done);
        }
    }

    double percentile(std::vector<double> v, double p)
    {
        if(v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, size_t(p / 100.0 * double(v.size() - 1) + .5))];
    }

    // Runs one configuration: N workers, chained or parallel. Returns false on any failure.
    bool run(const Options& o, uint32_t count, bool parallel)
    {
        auto* slots = static_cast<Slot*>(mmap(nullptr, sizeof(Slot) * count, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
        if(slots == MAP_FAILED) throw std::runtime_error("cannot map shared memory");
        for(uint32_t i = 0; i < count; ++i) new(&slots[i]) Slot();
        std::vector<pid_t> children;
        for(uint32_t i = 0; i < count; ++i)
        {
            const pid_t pid = fork();
            if(pid < 0) throw std::runtime_error("fork failed");
            if(pid == 0) worker(slots[i], o);
            children.push_back(pid);
        }
        const double periodUs = o.frames / o.rate * 1e6;
        const bool realtime = o.realtime && makeRealtime(periodUs);
        std::vector<double> total, overhead; total.reserve(o.blocks); overhead.reserve(o.blocks);
        uint32_t misses = 0; bool ok = true;
        auto next = Clock::now() + std::chrono::milliseconds(50); // let workers start
        for(uint32_t b = 1; b <= o.blocks + 50 && ok; ++b)
        {
            std::this_thread::sleep_until(next); next += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::micro>(periodUs));
            const auto start = Clock::now();
            auto hand = [&](Slot& s) { s.frames = o.frames; for(uint32_t i = 0; i < o.frames; ++i) s.in[0][i] = s.in[1][i] = float(i); s.request.store(b, std::memory_order_release); wake(s.request); };
            auto collect = [&](Slot& s) { waitWhileEqual(s.done, b - 1, o.spinUs); if(s.done.load() != b || s.failed) ok = false; };
            double compute = 0;
            if(parallel) { for(uint32_t i = 0; i < count; ++i) hand(slots[i]); for(uint32_t i = 0; i < count; ++i) { collect(slots[i]); compute = std::max(compute, slots[i].computeUs); } }
            else for(uint32_t i = 0; i < count; ++i) { hand(slots[i]); collect(slots[i]); compute += slots[i].computeUs; }
            const double us = std::chrono::duration<double, std::micro>(Clock::now() - start).count();
            if(b > 50) { total.push_back(us); overhead.push_back(us - compute); if(us > periodUs) ++misses; } // skip warm-up blocks
        }
        for(uint32_t i = 0; i < count; ++i) { slots[i].request.store(~0u, std::memory_order_release); wake(slots[i].request); }
        for(auto pid : children) { int status = 0; waitpid(pid, &status, 0); }
        munmap(slots, sizeof(Slot) * count);
        if(!ok) { std::printf("%-8s %3u workers: FAILED (a worker could not load or process)\n", parallel ? "parallel" : "chain", count); return false; }
        std::printf("%-8s %3u workers  overhead/block us: median %7.1f  p99 %7.1f  p99.9 %7.1f  max %8.1f   total p99.9 %8.1f  of %.0f  misses %u/%u%s\n",
            parallel ? "parallel" : "chain", count, percentile(overhead, 50), percentile(overhead, 99), percentile(overhead, 99.9),
            *std::max_element(overhead.begin(), overhead.end()), percentile(total, 99.9), periodUs, misses, o.blocks, realtime ? "  (real-time)" : "");
        return true;
    }
}

int main(int argc, char** argv)
{
    try
    {
        Options o;
        for(int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto value = [&] { if(i + 1 >= argc) throw std::runtime_error("missing value for " + a); return std::string(argv[++i]); };
            if(a == "--blocks") o.blocks = uint32_t(std::stoul(value()));
            else if(a == "--frames") o.frames = uint32_t(std::stoul(value()));
            else if(a == "--rate") o.rate = std::stod(value());
            else if(a == "--work-us") o.workUs = std::stod(value());
            else if(a == "--spin-us") o.spinUs = std::stod(value());
            else if(a == "--realtime") o.realtime = true;
            else if(a == "--plugin") o.plugin = value();
            else if(a == "--id") o.id = value();
            else if(a == "--check") o.check = true;
            else if(a == "--workers") { o.workers.clear(); auto list = value(); for(size_t at = 0; at < list.size();) { const auto end = std::min(list.find(',', at), list.size()); o.workers.push_back(uint32_t(std::stoul(list.substr(at, end - at)))); at = end + 1; } }
            else throw std::runtime_error("unknown option " + a);
        }
        if(o.frames < 1 || o.frames > maxFrames || o.blocks < 1 || o.rate < 8000 || o.workers.empty()) throw std::runtime_error("invalid options");
        if(o.check) { o.blocks = 200; o.workers = {1, 3}; }
        std::printf("block %u frames at %.0f Hz (%.0f us), %s, spin %.0f us\n", o.frames, o.rate, o.frames / o.rate * 1e6,
            o.plugin.empty() ? ("busy work " + std::to_string(int(o.workUs)) + " us per worker").c_str() : ("plugin " + o.id).c_str(), o.spinUs);
        bool ok = true;
        for(const auto count : o.workers) for(const bool parallel : {false, true}) ok = run(o, count, parallel) && ok;
        return ok ? 0 : 1;
    }
    catch(const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 2; }
}

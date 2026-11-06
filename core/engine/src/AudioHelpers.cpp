// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioHelpers.h"
#include "oscairn/realtime.h"
#include <algorithm>
#include <cstdlib>

namespace oscairn
{
    namespace
    {
        inline void relax() noexcept
        {
#if defined(__aarch64__)
            __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
            __builtin_ia32_pause();
#endif
        }
    }

    AudioHelpers& AudioHelpers::shared(double periodMicroseconds)
    {
        static AudioHelpers* helpers = new AudioHelpers(periodMicroseconds); // never freed: see the class
        return *helpers;
    }

    AudioHelpers::AudioHelpers(double periodMicroseconds)
    {
        uint32_t count = std::min(3u, std::max(1u, std::thread::hardware_concurrency()) - 1);
        if(const char* n = std::getenv("OSCAIRN_AUDIO_HELPERS"); n && *n) count = uint32_t(std::min(16L, std::max(0L, std::atol(n))));
        if(const char* w = std::getenv("OSCAIRN_HELPERS_WORTH_US"); w && *w) worth_ = float(std::max(0.0, std::atof(w)));
        for(uint32_t i = 0; i < count; ++i) threads_.emplace_back([this, periodMicroseconds] { helper(periodMicroseconds); });
        for(auto& t : threads_) t.detach();
    }

    void AudioHelpers::helper(double periodMicroseconds) noexcept
    {
        worker::makeRealtime(periodMicroseconds);
        // Between dispatches a helper sleeps: on a four-core board an idle spinning helper costs
        // more than its wake, and the audio thread takes any job a helper has not woken for.
        for(uint32_t seen = generation_.load(std::memory_order_acquire);;)
        {
            worker::waitWhileEqual(generation_, seen, std::chrono::seconds(1));
            seen = generation_.load(std::memory_order_acquire);
            work(seen);
        }
    }

    void AudioHelpers::work(uint32_t generation) noexcept
    {
        for(uint64_t v = next_.load(std::memory_order_acquire);;)
        {
            const uint32_t index = uint32_t(v & 0xFFFF), count = uint32_t(v >> 16 & 0xFFFF);
            if(uint32_t(v >> 32) != generation || index >= count) return;
            if(!next_.compare_exchange_weak(v, v + 1, std::memory_order_acq_rel, std::memory_order_acquire)) continue;
            // Taken: the dispatch waits for this job, so its job and context stay as they are.
            std::fesetenv(&environment_);
            job_(context_, index);
            if(done_.fetch_add(1, std::memory_order_acq_rel) + 1 == count) worker::wake(done_);
            v = next_.load(std::memory_order_acquire);
        }
    }

    void AudioHelpers::run(uint32_t count, Job job, void* context) noexcept
    {
        if(count < 2 || count > 0xFFFF || threads_.empty() || busy_.exchange(true, std::memory_order_acquire))
        {
            for(uint32_t i = 0; i < count; ++i) job(context, i);
            return;
        }
        job_ = job; context_ = context; std::fegetenv(&environment_); // the same rounding and flushing as this thread
        done_.store(0, std::memory_order_relaxed);
        const uint32_t generation = generation_.load(std::memory_order_relaxed) + 1;
        next_.store(uint64_t(generation) << 32 | uint64_t(count) << 16, std::memory_order_release);
        generation_.store(generation, std::memory_order_release);
        worker::wake(generation_, int(std::min<size_t>(count - 1, threads_.size())));
        work(generation);
        // The jobs helpers took: spin a little, as they are short, then sleep until the last wakes us.
        for(uint32_t spins = 0, d; (d = done_.load(std::memory_order_acquire)) != count;)
            if(++spins < 4096) relax();
            else worker::waitWhileEqual(done_, d, std::chrono::seconds(1));
        busy_.store(false, std::memory_order_release);
    }
}

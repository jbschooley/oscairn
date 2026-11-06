// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cfenv>
#include <cstdint>
#include <thread>
#include <vector>

namespace oscairn
{
    // RFC 0034: real-time threads that render a wave's plugins beside the audio thread, one per
    // core but one, at most three (OSCAIRN_AUDIO_HELPERS sets how many; 0: none), shared by every
    // graph in the process. Started by the first call to shared(), on the main thread, and never
    // stopped: they sleep until the process exits.
    class AudioHelpers
    {
    public:
        static AudioHelpers& shared(double periodMicroseconds);
        using Job = void (*)(void* context, uint32_t index) noexcept;
        // Audio thread: job(context, i) for every i below count, on this thread and the helpers;
        // returns when all are done. Never allocates or locks. While another thread is using the
        // helpers, or for a single job, all run on this thread.
        void run(uint32_t count, Job job, void* context) noexcept;
        uint32_t threads() const noexcept { return uint32_t(threads_.size()); }
        // The microseconds of rendering a dispatch must move off the audio thread to pay for waking
        // helpers: 30, or OSCAIRN_HELPERS_WORTH_US (0 dispatches every wave, for tests).
        float worth() const noexcept { return worth_; }

    private:
        explicit AudioHelpers(double periodMicroseconds);
        void helper(double periodMicroseconds) noexcept;
        void work(uint32_t generation) noexcept;

        // The dispatch's generation (high 32 bits), its job count (16) and the next job to take
        // (16), in one word, so a helper waking late can never take a later dispatch's job.
        std::atomic<uint64_t> next_{0};
        std::atomic<uint32_t> generation_{0}; // helpers sleep on it
        std::atomic<uint32_t> done_{0};       // jobs finished in this dispatch; the audio thread sleeps on it
        std::atomic<bool> busy_{false};       // a dispatch is running
        Job job_ = nullptr; void* context_ = nullptr; std::fenv_t environment_{}; // the dispatcher's, for its jobs
        std::vector<std::thread> threads_;
        float worth_ = 30;
    };
}

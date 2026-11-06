// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Real-time thread primitives shared by plugin workers (RFC 0010) and the graph's audio
// helpers (RFC 0034): futex (Linux) or os_sync (macOS) waits on a 32-bit word, valid between
// processes, and real-time scheduling for the calling thread.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace oscairn::worker
{
    // Waits while word == value; false on timeout. Shared between processes.
    bool waitWhileEqual(std::atomic<uint32_t>& word, uint32_t value, std::chrono::nanoseconds timeout) noexcept;
    void wake(std::atomic<uint32_t>& word) noexcept;
    // The calling thread at real-time priority: a time constraint of the period on macOS, SCHED_FIFO on Linux.
    bool makeRealtime(double periodMicroseconds) noexcept;
}

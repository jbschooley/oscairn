// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/local_socket.h"
#include <juce_events/juce_events.h>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>

namespace oscairn::host
{
    /// The engine's control socket on the main thread: read as soon as a request arrives (the
    /// server's watcher wakes this thread), and after each service tick, which sends what changed.
    class Pump : private juce::AsyncUpdater
    {
    public:
        Pump(LocalServer& server, bool managed, volatile std::sig_atomic_t& stopping) : server_(server), managed_(managed), stopping_(stopping)
        {
            server_.watch([this] { triggerAsyncUpdate(); });
        }
        ~Pump() override { cancelPendingUpdate(); }
        int result = 0;
        void poll()
        {
            if(stopping_) { juce::MessageManager::getInstance()->stopDispatchLoop(); return; }
            try
            {
                const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
                server_.poll(static_cast<uint64_t>(now));
            }
            catch(const std::exception& e) { std::cerr << "oscairn-engine: " << e.what() << '\n'; result = 1; stopping_ = 1; juce::MessageManager::getInstance()->stopDispatchLoop(); return; }
            // A managed engine stops when its UIs have quit normally (crashes leave it running),
            // checked after reading this poll's requests, so a window just opening keeps it.
            if(managed_ && server_.released()) { stopping_ = 1; juce::MessageManager::getInstance()->stopDispatchLoop(); }
        }

    private:
        void handleAsyncUpdate() override { poll(); }
        LocalServer& server_;
        bool managed_;
        volatile std::sig_atomic_t& stopping_;
    };
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <memory>
#include <string_view>

namespace oscairn
{
    /// Unix-domain stream transport for JSON-RPC. All methods belong to the control
    /// thread. poll() never waits; neither audio nor MIDI callbacks call this class.
    class LocalServer
    {
    public:
        static constexpr size_t maxClients = 8;
        static constexpr size_t maxQueuedBytes = 1024 * 1024;
        static constexpr size_t maxRequestsPerTick = 16; // per client
        explicit LocalServer(Control& control);
        ~LocalServer();
        LocalServer(const LocalServer&) = delete;
        LocalServer& operator=(const LocalServer&) = delete;
        /// Parent directory must be owned by this user and private (0700). Never
        /// replaces an existing endpoint. Socket is 0600; no TCP listener exists.
        void listen(const std::string& path);
        /// Takes ownership even on refusal/failure. Also supports socket activation.
        bool adopt(int connectedSocket);
        void poll(uint64_t monotonicNanoseconds);
        size_t clients() const;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /// Single-threaded local JSON-RPC client. Synchronous calls have bounded waits;
    /// use on a UI/control worker, never on audio. No automatic command replay.
    class LocalClient
    {
    public:
        LocalClient();
        ~LocalClient();
        LocalClient(const LocalClient&) = delete;
        LocalClient& operator=(const LocalClient&) = delete;
        void connect(const std::string& path, unsigned timeoutMs = 1000);
        void adopt(int connectedSocket); // takes ownership; drops previous connection
        void disconnect() noexcept;
        bool connected() const;
        /// params must be an encoded JSON object. Returns the complete response,
        /// including RPC errors. Timeout/disconnect throws and closes the connection:
        /// a sent mutation may have applied, so callers must refresh, never replay it.
        std::string call(std::string_view method, std::string_view params = "{}", unsigned timeoutMs = 1000);
        /// Receive subscribed notifications without waiting. A bounded notification
        /// queue overflows by disconnecting, never by silently dropping updates.
        void poll();
        std::vector<std::string> takeNotifications();
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

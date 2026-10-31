// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Plugin worker processes (RFC 0010): the block shared between the engine and a worker,
// cross-process wait/wake, and the control-socket encoding. Internal to oscairn_plugin_host.
#pragma once
#include <kick_clap/Host.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace oscairn::worker
{
    constexpr uint32_t maxEvents = kick::clap::Host::maxBlockEvents;
    constexpr uint32_t maxOutputEvents = 1024;
    constexpr uint32_t maxParameters = 4096;

    // Header of the shared block; audio follows it. request/done carry block sequence
    // numbers; the engine writes everything before request, the worker after done.
    struct alignas(64) Shared
    {
        std::atomic<uint32_t> request{0};
        alignas(64) std::atomic<uint32_t> done{0};
        alignas(64) std::atomic<uint32_t> stop{0}, panic{0};
        std::atomic<uint32_t> crashed{0}; // the signal that is ending the worker, set as it dies
        uint32_t frames = 0, eventCount = 0, hasTransport = 0, ok = 0, outputCount = 0;
        uint32_t inputChannels = 0, outputChannels = 0, maxFrames = 0, parameterCount = 0;
        double sampleRate = 48000;
        clap_event_transport_t transport{};
        std::atomic<uint64_t> outputDrops{0}, blocks{0}, droppedEvents{0}, processErrors{0}, deadlineMisses{0}, controlSilences{0}, idleBlocks{0};
        std::atomic<double> lastMicroseconds{0}, maximumMicroseconds{0};
        kick::clap::Event events[maxEvents];
        kick::clap::Event outputs[maxOutputEvents];
        std::atomic<double> parameterValues[maxParameters];
        float* input(uint32_t channel) noexcept { return reinterpret_cast<float*>(this + 1) + size_t(channel) * maxFrames; }
        float* output(uint32_t channel) noexcept { return input(inputChannels + channel); }
        static size_t bytes(uint32_t inputs, uint32_t outputs, uint32_t frames) { return sizeof(Shared) + sizeof(float) * size_t(inputs + outputs) * frames; }
    };
    static_assert(std::atomic<uint32_t>::is_always_lock_free && std::atomic<double>::is_always_lock_free && std::atomic<uint64_t>::is_always_lock_free);

    // Waits while word == value; false on timeout. Shared between processes.
    bool waitWhileEqual(std::atomic<uint32_t>& word, uint32_t value, std::chrono::nanoseconds timeout) noexcept;
    void wake(std::atomic<uint32_t>& word) noexcept;
    bool makeRealtime(double periodMicroseconds) noexcept;

    // Requests. Init and Shutdown concern the worker; every other request is followed by the
    // index of the plugin node it addresses (a worker hosts one plugin and the effect contexts
    // created from it).
    enum class Op : uint8_t
    {
        Init = 1, Service, SetParameter, Gesture, ParameterText, SaveState, LoadState, Shutdown,
        CreateEffectContext, DestroyNode, EffectResources, SoundContexts, PrepareSound, SelectSound,
        DiscardSound, SaveSnapshot, LoadSnapshot, AssetDependencies, Presets, LoadPreset
    };
    // First byte of every message from the worker: a reply, or a request to the engine made while
    // the worker serves one (the plugin resolving a sound asset).
    enum Reply : char { Failed = 0, Done = 1, ResolveAsset = 2, Invalid = 4 /* std::invalid_argument */ };

    // Length-prefixed messages over a stream socket.
    struct Writer
    {
        std::string data;
        Writer& u8(uint8_t v) { data.push_back(char(v)); return *this; }
        Writer& u32(uint32_t v) { data.append(reinterpret_cast<const char*>(&v), 4); return *this; }
        Writer& f64(double v) { data.append(reinterpret_cast<const char*>(&v), 8); return *this; }
        Writer& str(const std::string& v) { u32(uint32_t(v.size())); data += v; return *this; }
        Writer& bytes(const void* p, size_t n) { data.append(static_cast<const char*>(p), n); return *this; }
    };
    struct Reader
    {
        std::string data; size_t at = 0;
        void need(size_t n) { if(data.size() - at < n) throw std::runtime_error("Truncated worker message"); }
        uint8_t u8() { need(1); return uint8_t(data[at++]); }
        uint32_t u32() { uint32_t v; need(4); std::memcpy(&v, data.data() + at, 4); at += 4; return v; }
        double f64() { double v; need(8); std::memcpy(&v, data.data() + at, 8); at += 8; return v; }
        std::string str() { const auto n = u32(); need(n); std::string v = data.substr(at, n); at += n; return v; }
        void bytes(void* p, size_t n) { need(n); std::memcpy(p, data.data() + at, n); at += n; }
    };
    // Throw on a closed or failed socket; receive enforces maxBytes.
    void send(int socket, const std::string& message);
    // A reply that did not come within the socket's timeout: the worker is alive but late.
    struct WorkerTimeout : std::runtime_error { WorkerTimeout() : std::runtime_error("Plugin worker did not answer in time") {} };
    std::string receive(int socket, size_t maxBytes = 64u << 20); // throws WorkerTimeout, or runtime_error when the connection is gone
    void sendDescriptor(int socket, int fd);
    int receiveDescriptor(int socket);

    // The worker process: hosts one plugin and its effect contexts, serving control requests on `socket`.
    int run(int socket);
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include <kick_clap/Host.h>
#include <kick_clap/Discovery.h>
#include "Internal.h"
#include "Window.h"
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/latency.h>
#include <clap/ext/tail.h>
#include <clap/ext/thread-check.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/posix-fd-support.h>
#include <clap/ext/log.h>
#include <clap/ext/event-registry.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/gui.h>
#include <clap/ext/render.h>
#include <clap/factory/preset-discovery.h>
#include <oscairn-clap/beat.h>
#include <oscairn-clap/harmony.h>
#include <oscairn-clap/param-target.h>
#include <oscairn-clap/ui.h>
#include <algorithm>
#include <filesystem>
#include <cstdio>
#include <chrono>
#include <cmath>
#include <thread>
#ifndef _WIN32
#include <poll.h>
#endif

namespace kick::clap {
Event Event::midi(uint8_t status, uint8_t a, uint8_t b, uint32_t frame, uint16_t port) noexcept {
    clap_event_midi_t e{};
    e.header = {sizeof(e), frame, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
    e.port_index = port;
    e.data[0] = status; e.data[1] = a; e.data[2] = b;
    return from(e);
}
Event Event::note(bool on, int key, double velocity, uint32_t frame, int channel, int port, int noteId) noexcept {
    clap_event_note_t e{};
    e.header = {sizeof(e), frame, CLAP_CORE_EVENT_SPACE_ID,
                static_cast<uint16_t>(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
    e.note_id = noteId; e.port_index = static_cast<int16_t>(port);
    e.channel = static_cast<int16_t>(channel); e.key = static_cast<int16_t>(key);
    e.velocity = velocity;
    return from(e);
}

namespace {
thread_local const void* audioScope = nullptr;
Host::ActivationObserver activationObserver = nullptr; // observeActivations: set once, before any plugin loads
void* activationContext = nullptr;
struct AudioScope {
    const void* previous;
    explicit AudioScope(const void* owner) : previous(audioScope) { audioScope = owner; }
    ~AudioScope() { audioScope = previous; }
};
struct Gate {
    std::atomic_flag& flag;
    explicit Gate(std::atomic_flag& f) : flag(f) {
        while (flag.test_and_set(std::memory_order_acquire)) std::this_thread::yield();
    }
    ~Gate() { flag.clear(std::memory_order_release); }
};
struct Stream {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    bool failed = false;
    static int64_t CLAP_ABI write(const clap_ostream_t* s, const void* data, uint64_t size) {
        auto& self = *static_cast<Stream*>(s->ctx);
        if (size > Host::maxStateBytes - self.bytes.size()) { self.failed = true; return -1; }
        try {
            const auto* p = static_cast<const uint8_t*>(data);
            self.bytes.insert(self.bytes.end(), p, p + size);
            return static_cast<int64_t>(size);
        } catch (...) { self.failed = true; return -1; }
    }
    static int64_t CLAP_ABI read(const clap_istream_t* s, void* data, uint64_t size) {
        auto& self = *static_cast<Stream*>(s->ctx);
        const auto count = std::min<uint64_t>(size, self.bytes.size() - self.offset);
        if (count) std::memcpy(data, self.bytes.data() + self.offset, static_cast<size_t>(count));
        self.offset += static_cast<size_t>(count);
        return static_cast<int64_t>(count);
    }
};
bool validEvent(const clap_event_header_t* h) noexcept {
    if (h && h->space_id == Host::paramTargetSpace) {
        const auto* t = reinterpret_cast<const oscairn_event_param_target_t*>(h);
        return h->type == OSCAIRN_EVENT_PARAM_TARGET && h->size == sizeof(oscairn_event_param_target_t) &&
               (t->mode == OSCAIRN_PARAM_TARGET_SET || t->mode == OSCAIRN_PARAM_TARGET_MODULATE) && std::isfinite(t->value);
    }
    if (h && h->space_id == Host::beatSpace) {
        const auto* b = reinterpret_cast<const oscairn_event_beat_t*>(h);
        return h->type == OSCAIRN_EVENT_BEAT && h->size == sizeof(oscairn_event_beat_t) && std::isfinite(b->tempo) && b->tempo > 0;
    }
    if (h && h->space_id == Host::harmonySpace) {
        const auto& x = reinterpret_cast<const oscairn_event_harmony_t*>(h)->harmony;
        return h->type == OSCAIRN_EVENT_HARMONY && h->size == sizeof(oscairn_event_harmony_t) && x.key_root < 12 && x.chord_root < 12 &&
               x.chord_bass < 12 && x.chord_type <= OSCAIRN_CHORD_MINOR9 && x.scale < 4096 && (x.scale & 1) && !x.reserved;
    }
    if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->size > Event::capacity) return false;
    uint32_t size = 0;
    switch (h->type) {
        case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE:
        case CLAP_EVENT_NOTE_END: size = sizeof(clap_event_note_t); break;
        case CLAP_EVENT_NOTE_EXPRESSION: size = sizeof(clap_event_note_expression_t); break;
        case CLAP_EVENT_PARAM_VALUE: size = sizeof(clap_event_param_value_t); break;
        case CLAP_EVENT_PARAM_MOD: size = sizeof(clap_event_param_mod_t); break;
        case CLAP_EVENT_PARAM_GESTURE_BEGIN: case CLAP_EVENT_PARAM_GESTURE_END:
            size = sizeof(clap_event_param_gesture_t); break;
        case CLAP_EVENT_MIDI: size = sizeof(clap_event_midi_t); break;
        case CLAP_EVENT_MIDI2: size = sizeof(clap_event_midi2_t); break;
        case CLAP_EVENT_MIDI_SYSEX: size = sizeof(clap_event_midi_sysex_t); break;
        default: return false;
    }
    if (h->size != size) return false;
    if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF || h->type == CLAP_EVENT_NOTE_CHOKE || h->type == CLAP_EVENT_NOTE_END) {
        const auto& n = *reinterpret_cast<const clap_event_note_t*>(h);
        if (!std::isfinite(n.velocity) || n.velocity < 0 || n.velocity > 1 || n.note_id < -1 ||
            n.channel < -1 || n.channel > 15 || n.key < -1 || n.key > 127 || n.port_index < -1) return false;
        if (h->type == CLAP_EVENT_NOTE_ON && (n.channel < 0 || n.key < 0 || n.port_index < 0)) return false;
    }
    auto address=[](const auto& e){return e.note_id>=-1 && e.port_index>=-1 && e.channel>=-1 && e.channel<=15 && e.key>=-1 && e.key<=127;};
    if(h->type==CLAP_EVENT_PARAM_VALUE && !address(*reinterpret_cast<const clap_event_param_value_t*>(h)))return false;
    if(h->type==CLAP_EVENT_PARAM_MOD && !address(*reinterpret_cast<const clap_event_param_mod_t*>(h)))return false;
    if(h->type==CLAP_EVENT_NOTE_EXPRESSION && !address(*reinterpret_cast<const clap_event_note_expression_t*>(h)))return false;
    if (h->type == CLAP_EVENT_PARAM_VALUE && !std::isfinite(reinterpret_cast<const clap_event_param_value_t*>(h)->value)) return false;
    if (h->type == CLAP_EVENT_PARAM_MOD && !std::isfinite(reinterpret_cast<const clap_event_param_mod_t*>(h)->amount)) return false;
    if (h->type == CLAP_EVENT_NOTE_EXPRESSION && !std::isfinite(reinterpret_cast<const clap_event_note_expression_t*>(h)->value)) return false;
    if (h->type == CLAP_EVENT_MIDI) {
        const auto& m = *reinterpret_cast<const clap_event_midi_t*>(h);
        if (m.data[0] < 0x80 || m.data[0] == 0xf0 || m.data[0] == 0xf7 || m.data[1] > 127 || m.data[2] > 127) return false;
    }
    if (h->type == CLAP_EVENT_MIDI_SYSEX) { // one whole message, F0 to F7; its data bytes are checked where a plugin sends it
        const auto& x = *reinterpret_cast<const clap_event_midi_sysex_t*>(h);
        if (!x.buffer || x.size < 2 || x.size > SysexPool::maxMessage || x.buffer[0] != 0xf0 || x.buffer[x.size - 1] != 0xf7) return false;
    }
    return true;
}
}

bool SysexPool::hold(Event& event) noexcept {
    auto* h = reinterpret_cast<clap_event_header_t*>(event.bytes.data());
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_MIDI_SYSEX || h->size != sizeof(clap_event_midi_sysex_t)) return false;
    auto& x = *reinterpret_cast<clap_event_midi_sysex_t*>(h);
    if (!x.buffer || x.size > maxMessage || x.size > maxBlock - used) return false;
    std::memcpy(bytes + used, x.buffer, x.size);
    x.buffer = bytes + used;
    used += x.size;
    return true;
}

bool Event::valid() const noexcept { return validEvent(header()); }

struct Host::Impl {
    std::thread::id mainThread{};
    clap_host_t host{};
    std::shared_ptr<detail::Library> library;
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const oscairn_plugin_ui_t* ui = nullptr;
    const clap_plugin_state_t* state = nullptr;
    bool exclusiveState=false;
    const oscairn_plugin_sound_contexts_t* sounds = nullptr;
    const oscairn_plugin_asset_dependencies_t* assetDependencies = nullptr;
    bool assetOptional = false; // draft 2: it says which assets are optional
    AssetResolver* assets = nullptr;
    const clap_plugin_latency_t* pluginLatency = nullptr;
    const clap_plugin_timer_support_t* timerSupport = nullptr;
    const clap_plugin_posix_fd_support_t* fdSupport = nullptr;
    // oscairn.memory (RFC 0025): the plugin's estimate at its last activation, and the bytes the
    // next activation may declare (0: no limit), set by the caller and kept across loads.
    const oscairn_plugin_memory_t* memory = nullptr;
    bool hasDeclared = false, context = false; // context: an effect context, costing its context bytes alone
    oscairn_memory_t declared{};
    uint64_t memoryRoom = 0;
    bool offline = false; // clap.render: what the caller asked for, kept across loads
    Descriptor info;
    std::string message;
    std::string lastLoggedError; // main thread only: explains a following failed call
    bool audioLayoutLocked=false;
    std::vector<Parameter> paramInfo;
    uint64_t metadataVersion = 0;
    std::vector<void*> cookies;
    std::unique_ptr<std::atomic<double>[]> paramValues;
    std::vector<Port> inputs, outputs;
    std::vector<clap_note_port_info_t> noteInputs, noteOutputs;
    std::vector<clap_audio_buffer_t> inputBuffers, outputBuffers;
    std::vector<float*> inputPointers, outputPointers;
    std::vector<float> scratch;
    double sampleRate = 48000;
    uint32_t maxFrames = 512;
    int64_t steadyTime = 0;
    bool active = false, sleeping = false;
    std::atomic<bool> wantsBeat{false}, wantsHarmony{false}; // the plugin queried oscairn.beat's, oscairn.harmony's event space
    // oscairn.harmony: the last delivered (audio thread), and a copy the main thread reads
    // through the host extension, behind a sequence count that is odd while it is written.
    oscairn_harmony_t lastHarmony{};
    bool haveHarmony = false;
    std::atomic<uint32_t> harmonySequence{0};
    std::array<std::atomic<uint64_t>, 3> harmonyWords{};
    static_assert(sizeof(oscairn_harmony_t) == 3 * sizeof(uint64_t));
    void publishHarmony(const oscairn_harmony_t& h) noexcept {
        uint64_t words[3]; std::memcpy(words, &h, sizeof words);
        const auto n = harmonySequence.load(std::memory_order_relaxed);
        harmonySequence.store(n + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (size_t i = 0; i < 3; ++i) harmonyWords[i].store(words[i], std::memory_order_relaxed);
        harmonySequence.store(n + 2, std::memory_order_release);
    }
    static bool CLAP_ABI readHarmony(const clap_host_t* host, oscairn_harmony_t* out) {
        auto& s = self(host);
        if (!out) return false;
        uint64_t words[3]; uint32_t before, after;
        do {
            before = s.harmonySequence.load(std::memory_order_acquire);
            for (size_t i = 0; i < 3; ++i) words[i] = s.harmonyWords[i].load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            after = s.harmonySequence.load(std::memory_order_relaxed);
        } while ((before & 1) || before != after);
        if (!before) return false;
        std::memcpy(out, words, sizeof words);
        return true;
    }
    std::atomic<bool> processing{false}; // read by service() without the gate
    std::atomic<uint32_t> pendingCommands{0}; // enqueued but not yet delivered to the plugin
    bool transportObserved = false, hadTransport = false;
    clap_event_transport_t previousTransport{};
    uint32_t previousTransportFrames = 0;
    std::atomic_flag gate = ATOMIC_FLAG_INIT;
    std::atomic<bool> loaded{false}, failed{false}, dirty{false};
    std::atomic<uint64_t> parameterReports{0};
    std::atomic<bool> restart{false}, callback{false}, wake{false}, flush{false}, panic{false};
    std::atomic<uint32_t> rescan{0}, latencyFrames{0};
    std::atomic<uint64_t> blocks{0}, dropped{0}, errors{0};
    std::atomic<double> lastUs{0}, maxUs{0};
    std::atomic<uint64_t> deadlineMisses{0}, controlSilences{0}, idleBlocks{0};
    detail::Queue<Event, maxEvents> commands, emitted;
    std::atomic<uint64_t> outputDrops{0};
    struct TimedEvent { Event event; uint32_t order = 0; };
    // Separate bounded budgets for carry, queued controls and graph events.
    static constexpr uint32_t renderEvents = 2 * maxBlockEvents + maxProcessEvents;
    std::array<TimedEvent, renderEvents> ordered;
    std::array<Event, renderEvents> chunkEvents;
    std::array<Event, maxBlockEvents> carryEvents;
    uint32_t carryCount = 0;
    // Non-parameter output of the current process() call, collected only while
    // inProcess is set and published to the caller when the call returns.
    std::array<Event, maxBlockEvents> blockOutput;
    uint32_t blockOutputCount = 0, publishedOutputCount = 0;
    SysexPool sysexOutput; // the bytes of blockOutput's SysEx events
    bool inProcess = false;
    uint32_t chunkCount = 0;
    uint32_t outputFrameOffset = 0, currentProcessFrames = 1;
    struct Timer { clap_id id; uint32_t period; std::chrono::steady_clock::time_point next; };
    std::vector<Timer> timers;
    clap_id nextTimer = 1;
    struct Fd { int fd; clap_posix_fd_flags_t flags; };
    std::vector<Fd> fds;
    // CLAP gui: the plugin's own window, on the main thread; guiApi is null where it has none
    // here. An embedded editor sits in a window of the host's own. The plugin's requests may come
    // from any thread and are carried out in service().
    const clap_plugin_gui_t* gui = nullptr;
    const char* guiApi = nullptr;
    bool guiFloating = false;
    std::atomic<bool> guiCreated{false}, guiShow{false}, guiHide{false}, guiGone{false}, guiHints{false};
    std::atomic<uint64_t> guiSize{0}; // a size asked for off the main thread: width << 32 | height
    std::unique_ptr<detail::Window> window;
    static Impl& self(const clap_host_t* h) { return *static_cast<Impl*>(h->host_data); }
    bool main() {
        if (mainThread == std::thread::id{}) mainThread = std::this_thread::get_id();
        if (mainThread == std::this_thread::get_id()) return true;
        message = "CLAP control operation must run on its main thread";
        return false;
    }
    static bool CLAP_ABI isMain(const clap_host_t* h) { return self(h).mainThread == std::this_thread::get_id(); }
    static bool CLAP_ABI isAudio(const clap_host_t* h) { return audioScope == &self(h); }
    static void CLAP_ABI requestRestart(const clap_host_t* h) { self(h).restart.store(true); }
    static void CLAP_ABI requestCallback(const clap_host_t* h) { self(h).callback.store(true); }
    static void CLAP_ABI requestProcess(const clap_host_t* h) { self(h).wake.store(true); }
    static void CLAP_ABI requestFlush(const clap_host_t* h) { self(h).flush.store(true); self(h).wake.store(true); }
    static void CLAP_ABI rescanParams(const clap_host_t* h, clap_param_rescan_flags flags) { self(h).rescan.fetch_or(flags); }
    static void CLAP_ABI clearParams(const clap_host_t*, clap_id, clap_param_clear_flags) {}
    static void CLAP_ABI markDirty(const clap_host_t* h) { self(h).dirty.store(true); }
    static void CLAP_ABI latencyChanged(const clap_host_t* h) { self(h).restart.store(true); }
    static void CLAP_ABI tailChanged(const clap_host_t*) {}
    static bool CLAP_ABI supportedAudioRescan(const clap_host_t*, uint32_t flags) {
        return (flags & ~(CLAP_AUDIO_PORTS_RESCAN_NAMES | CLAP_AUDIO_PORTS_RESCAN_FLAGS |
                         CLAP_AUDIO_PORTS_RESCAN_CHANNEL_COUNT | CLAP_AUDIO_PORTS_RESCAN_PORT_TYPE |
                         CLAP_AUDIO_PORTS_RESCAN_IN_PLACE_PAIR | CLAP_AUDIO_PORTS_RESCAN_LIST)) == 0;
    }
    static void CLAP_ABI audioRescan(const clap_host_t* h, uint32_t) { self(h).restart.store(true); }
    static uint32_t CLAP_ABI dialects(const clap_host_t*) { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE; }
    static void CLAP_ABI noteRescan(const clap_host_t* h, uint32_t) { self(h).restart.store(true); }
    static void CLAP_ABI log(const clap_host_t* h, clap_log_severity severity, const char* text) {
        // Logging from audio callbacks must not format strings, write files, or lock.
        if (severity == CLAP_LOG_FATAL || severity == CLAP_LOG_PLUGIN_MISBEHAVING) self(h).errors.fetch_add(1);
        if (severity == CLAP_LOG_FATAL) self(h).failed.store(true);
        if (severity == CLAP_LOG_ERROR && text && isMain(h)) self(h).lastLoggedError.assign(text, strnlen(text, 512));
    }
    static bool CLAP_ABI registerTimer(const clap_host_t* h, uint32_t ms, clap_id* id) {
        auto& s = self(h);
        if (!isMain(h) || !id || s.timers.size() >= 128) return false;
        *id = s.nextTimer++;
        s.timers.push_back({*id, std::max(1u, ms), std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max(1u, ms))});
        return true;
    }
    static bool CLAP_ABI unregisterTimer(const clap_host_t* h, clap_id id) {
        auto& s = self(h);
        if (!isMain(h)) return false;
        const auto before = s.timers.size();
        s.timers.erase(std::remove_if(s.timers.begin(), s.timers.end(), [=](const Timer& t) { return t.id == id; }), s.timers.end());
        return before != s.timers.size();
    }
    static bool CLAP_ABI registerFd(const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        auto& s = self(h);
        if (!isMain(h) || fd < 0 || s.fds.size() >= 128 ||
            (flags & ~(CLAP_POSIX_FD_READ | CLAP_POSIX_FD_WRITE | CLAP_POSIX_FD_ERROR))) return false;
        for (const auto& item : s.fds) if (item.fd == fd) return false;
        s.fds.push_back({fd, flags}); return true;
    }
    static bool CLAP_ABI modifyFd(const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        auto& s = self(h);
        if (!isMain(h)) return false;
        for (auto& item : s.fds) if (item.fd == fd) { item.flags = flags; return true; }
        return false;
    }
    static bool CLAP_ABI unregisterFd(const clap_host_t* h, int fd) {
        auto& s = self(h);
        if (!isMain(h)) return false;
        auto old = s.fds.size();
        s.fds.erase(std::remove_if(s.fds.begin(), s.fds.end(), [=](const Fd& item) { return item.fd == fd; }), s.fds.end());
        return old != s.fds.size();
    }
    static bool CLAP_ABI resolveAsset(const clap_host_t* h, oscairn_asset_ref_t* ref, char* path, uint32_t pathCapacity, char* message, uint32_t messageCapacity) {
        auto report = [&](const std::string& text) { if (message && messageCapacity) { std::snprintf(message, messageCapacity, "%s", text.c_str()); } return false; };
        if (!isMain(h)) return report("assets resolve on the main thread only");
        if (!ref || !path || !pathCapacity) return report("invalid asset request");
        auto& s = self(h);
        if (!s.assets) return report("this host has no sound asset store");
        const std::string library(ref->library, strnlen(ref->library, sizeof(ref->library))), relative(ref->path, strnlen(ref->path, sizeof(ref->path)));
        std::string hash(ref->sha256, strnlen(ref->sha256, sizeof(ref->sha256)));
        try {
            const auto file = s.assets->resolve(library, relative, hash);
            if (file.size() + 1 > pathCapacity || hash.size() >= sizeof(ref->sha256)) return report("asset path too long");
            std::memcpy(path, file.c_str(), file.size() + 1);
            std::memcpy(ref->sha256, hash.c_str(), hash.size() + 1);
            return true;
        } catch (const std::exception& e) { return report(e.what()); }
    }
    // CLAP preset-load: the plugin reports how loading went.
    static void CLAP_ABI presetError(const clap_host_t* h, uint32_t, const char*, const char*, int32_t, const char* message) {
        self(h).message = std::string("Preset not loaded") + (message ? std::string(": ") + message : std::string());
    }
    static void CLAP_ABI presetLoaded(const clap_host_t* h, uint32_t, const char*, const char*) { self(h).message.clear(); }
    static void CLAP_ABI guiHintsChanged(const clap_host_t* h) { self(h).guiHints.store(true); }
    static bool CLAP_ABI guiRequestResize(const clap_host_t* h, uint32_t width, uint32_t height) {
        auto& s = self(h);
        if (!width || !height || !s.guiCreated.load() || s.guiFloating) return false;
        if (!isMain(h)) { s.guiSize.store(uint64_t(width) << 32 | height); return true; }
        if (!s.window) return false;
        s.window->setSize(width, height); return true;
    }
    static bool CLAP_ABI guiRequestShow(const clap_host_t* h) { auto& s = self(h); if (!s.guiCreated.load()) return false; s.guiShow.store(true); return true; }
    static bool CLAP_ABI guiRequestHide(const clap_host_t* h) { auto& s = self(h); if (!s.guiCreated.load()) return false; s.guiHide.store(true); return true; }
    // A floating window the person closed; one the plugin destroyed must be destroyed here too.
    static void CLAP_ABI guiClosed(const clap_host_t* h, bool destroyed) { if (destroyed) self(h).guiGone.store(true); }
    static const char* platformApi() noexcept {
#if defined(__APPLE__)
        return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
        return CLAP_WINDOW_API_WIN32;
#else
        return CLAP_WINDOW_API_X11;
#endif
    }
    // Embedded where the host has windows of its own, else floating where the plugin floats one.
    void findGui() {
        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        guiApi = nullptr; guiFloating = false;
        if (!gui || !gui->is_api_supported || !gui->create || !gui->destroy || !gui->show || !gui->hide) { gui = nullptr; return; }
        if (const char* api = detail::Window::api(); api && gui->get_size && gui->set_parent && gui->is_api_supported(plugin, api, false)) guiApi = api;
        else if (gui->is_api_supported(plugin, platformApi(), true)) { guiApi = platformApi(); guiFloating = true; }
    }
    void showGui(const std::string& title) {
        if (!guiCreated.load()) {
            detail::startWindows();
            if (!gui->create(plugin, guiApi, guiFloating)) throw std::runtime_error("Plugin could not open its window");
            guiCreated.store(true);
            if (!guiFloating) {
                uint32_t width = 0, height = 0;
                if (!gui->get_size(plugin, &width, &height) || !width || !height) { width = 640; height = 480; }
                window = detail::Window::create(title, width, height, gui->can_resize && gui->can_resize(plugin));
                window->closing = [this] { guiGone.store(true); };
                window->resized = [this](uint32_t& w, uint32_t& h) {
                    if (gui->adjust_size) gui->adjust_size(plugin, &w, &h);
                    if (gui->set_size) gui->set_size(plugin, w, h);
                };
                clap_window_t parent{}; parent.api = guiApi; parent.ptr = window->view();
                if (!gui->set_parent(plugin, &parent)) throw std::runtime_error("Plugin could not open its window in the host's");
            }
        }
        if (window) window->setTitle(title);
        else if (gui->suggest_title) gui->suggest_title(plugin, title.c_str());
        // An embedded editor shows with its window; a floating one must show itself.
        if (!gui->show(plugin) && guiFloating) throw std::runtime_error("Plugin could not show its window");
        if (window) window->show();
    }
    void destroyGui() {
        if (guiCreated.exchange(false)) gui->destroy(plugin);
        window.reset();
        guiShow.store(false); guiHide.store(false); guiGone.store(false); guiHints.store(false); guiSize.store(0);
    }
    void serviceGui() {
        if (!guiCreated.load()) return;
        if (guiGone.load()) { destroyGui(); return; }
        if (const auto size = guiSize.exchange(0); size && window) window->setSize(uint32_t(size >> 32), uint32_t(size));
        if (guiHints.exchange(false) && window && gui->can_resize) window->setResizable(gui->can_resize(plugin));
        if (guiHide.exchange(false)) { gui->hide(plugin); if (window) window->hide(); }
        if (guiShow.exchange(false)) { gui->show(plugin); if (window) window->show(); }
    }
    static const void* CLAP_ABI extension(const clap_host_t*, const char* id) {
        static const clap_host_thread_check_t threads{isMain, isAudio};
        static const clap_host_params_t parameters{rescanParams, clearParams, requestFlush};
        static const clap_host_state_t state{markDirty};
        static const clap_host_latency_t latency{latencyChanged};
        static const clap_host_tail_t tail{tailChanged};
        static const clap_host_audio_ports_t audio{supportedAudioRescan, audioRescan};
        static const clap_host_note_ports_t notes{dialects, noteRescan};
        static const clap_host_log_t logging{log};
        static const clap_host_event_registry_t registry{queryEventSpace};
        static const clap_host_timer_support_t timer{registerTimer, unregisterTimer};
        static const clap_host_posix_fd_support_t descriptors{registerFd, modifyFd, unregisterFd};
        static const oscairn_host_assets_t assets{resolveAsset};
        static const clap_host_preset_load_t presetLoad{presetError, presetLoaded};
        static const clap_host_gui_t windows{guiHintsChanged, guiRequestResize, guiRequestShow, guiRequestHide, guiClosed};
        static const oscairn_host_harmony_t harmony{readHarmony};
        if (!id) return nullptr;
        if (!std::strcmp(id, CLAP_EXT_GUI)) return &windows;
        if (!std::strcmp(id, OSCAIRN_EXT_ASSETS)) return &assets;
        if (!std::strcmp(id, OSCAIRN_EXT_HARMONY)) return &harmony;
        if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT)) return &presetLoad;
        if (!std::strcmp(id, CLAP_EXT_THREAD_CHECK)) return &threads;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &parameters;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return &state;
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TAIL)) return &tail;
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio;
        if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
        if (!std::strcmp(id, CLAP_EXT_LOG)) return &logging;
        if (!std::strcmp(id, CLAP_EXT_EVENT_REGISTRY)) return &registry;
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timer;
#ifndef _WIN32
        if (!std::strcmp(id, CLAP_EXT_POSIX_FD_SUPPORT)) return &descriptors;
#else
        (void)descriptors;
#endif
        return nullptr;
    }
    Impl() {
        host.clap_version = CLAP_VERSION; host.host_data = this;
        host.name = "Oscairn"; host.vendor = "Oscairn"; host.url = "https://github.com/oscairn/oscairn"; host.version = "0.1.0";
        host.get_extension = extension; host.request_restart = requestRestart;
        host.request_process = requestProcess; host.request_callback = requestCallback;
    }
    size_t paramIndex(clap_id id) const noexcept {
        const auto it = std::lower_bound(paramInfo.begin(), paramInfo.end(), id,
                                        [](const Parameter& p, clap_id target) { return p.id < target; });
        return it != paramInfo.end() && it->id == id ? static_cast<size_t>(it - paramInfo.begin()) : paramInfo.size();
    }
    void stop() {
        AudioScope scope(this);
        if (processing) plugin->stop_processing(plugin);
        processing = sleeping = false;
    }
    void deactivate() {
        stop();
        if (active) plugin->deactivate(plugin);
        active = false;
    }
    void close() {
        audioLayoutLocked=false;
        loaded.store(false);
        if (plugin) destroyGui();
        gui = nullptr; guiApi = nullptr;
        if (plugin) { deactivate(); if (plugin->destroy) plugin->destroy(plugin); }
        plugin = nullptr; params = nullptr; state = nullptr; sounds = nullptr; assetDependencies = nullptr; pluginLatency = nullptr;
        timerSupport = nullptr; fdSupport = nullptr; memory = nullptr; hasDeclared = false;
        timers.clear(); fds.clear();
        paramInfo.clear(); paramValues.reset(); cookies.clear(); inputs.clear(); outputs.clear(); noteInputs.clear(); noteOutputs.clear();
        inputBuffers.clear(); outputBuffers.clear(); inputPointers.clear(); outputPointers.clear(); scratch.clear();
        library.reset();
        Event event; while (commands.pop(event)) pendingCommands.fetch_sub(1); while (emitted.pop(event)) {}
        failed.store(false); dirty.store(false); restart.store(false); callback.store(false);
        wake.store(false); flush.store(false); panic.store(false); rescan.store(0); latencyFrames.store(0);
        steadyTime = 0;
        transportObserved = hadTransport = false;
        carryCount = 0;
        ++metadataVersion;
    }
    void initialize(double sampleRate,uint32_t frames,const std::vector<uint8_t>& bytes) {
        auto& s=*this;
        if (!s.plugin || !s.plugin->init || !s.plugin->destroy) throw std::runtime_error("Incomplete CLAP plugin interface");
        if (!s.plugin->init(s.plugin)) throw std::runtime_error("Plugin initialization failed");
        // The rest only once initialized: plugins built on clap-helpers fill them in during init.
        if (!s.plugin->activate || !s.plugin->deactivate || !s.plugin->start_processing || !s.plugin->stop_processing || !s.plugin->reset ||
            !s.plugin->process || !s.plugin->get_extension || !s.plugin->on_main_thread)
            throw std::runtime_error("Incomplete CLAP plugin interface");
        s.params = static_cast<const clap_plugin_params_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_PARAMS));
        s.ui = static_cast<const oscairn_plugin_ui_t*>(s.plugin->get_extension(s.plugin, OSCAIRN_EXT_UI));
        if (s.ui && !s.ui->param_hint) s.ui = nullptr;
        if (s.params && (!s.params->count || !s.params->get_info || !s.params->get_value || !s.params->flush))
            throw std::runtime_error("Incomplete CLAP parameter interface");
        s.sounds = static_cast<const oscairn_plugin_sound_contexts_t*>(s.plugin->get_extension(s.plugin,OSCAIRN_EXT_SOUND_CONTEXTS));
        if(s.sounds && (!s.sounds->capacity || !s.sounds->get || !s.sounds->prepare || !s.sounds->select || !s.sounds->discard || s.sounds->capacity(s.plugin)>256))
            throw std::runtime_error("Invalid sound-context interface");
        s.state = static_cast<const clap_plugin_state_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_STATE));
        s.exclusiveState=s.sounds && s.sounds->state;
        if(s.exclusiveState)s.state=s.sounds->state;
        if(s.state && (!s.state->save || !s.state->load))throw std::runtime_error("Incomplete plugin state interface");
        s.assetDependencies = static_cast<const oscairn_plugin_asset_dependencies_t*>(s.plugin->get_extension(s.plugin, OSCAIRN_EXT_ASSET_DEPENDENCIES));
        s.assetOptional = s.assetDependencies != nullptr;
        if(!s.assetDependencies) s.assetDependencies = static_cast<const oscairn_plugin_asset_dependencies_t*>(s.plugin->get_extension(s.plugin, OSCAIRN_EXT_ASSET_DEPENDENCIES_DRAFT_1)); // its first two members only
        if(s.assetDependencies && (!s.assetDependencies->count || !s.assetDependencies->get))throw std::runtime_error("Incomplete asset-dependency interface");
        s.assetOptional = s.assetOptional && s.assetDependencies->optional; // draft 2 without optional: every asset required
        s.pluginLatency = static_cast<const clap_plugin_latency_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_LATENCY));
        s.timerSupport = static_cast<const clap_plugin_timer_support_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_TIMER_SUPPORT));
        s.fdSupport = static_cast<const clap_plugin_posix_fd_support_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_POSIX_FD_SUPPORT));
        s.memory = static_cast<const oscairn_plugin_memory_t*>(s.plugin->get_extension(s.plugin, OSCAIRN_EXT_MEMORY));
        if(s.memory && (!s.memory->estimate || !s.memory->current))throw std::runtime_error("Incomplete memory interface");
        s.findGui();
        // Initialized and not yet processing, on the main thread, as clap.render asks.
        const auto* render = static_cast<const clap_plugin_render_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_RENDER));
        if (render && render->set)
            render->set(s.plugin, s.offline && !(render->has_hard_realtime_requirement && render->has_hard_realtime_requirement(s.plugin)) ? CLAP_RENDER_OFFLINE : CLAP_RENDER_REALTIME);
        if (!bytes.empty()) {
            if (bytes.size() > maxStateBytes || !s.state || !s.state->load) throw std::runtime_error("Plugin cannot restore saved state");
            Stream stream; stream.bytes = bytes;
            clap_istream_t input{&stream, Stream::read};
            if (!s.state->load(s.plugin, &input)) throw std::runtime_error("Plugin rejected saved state");
        }
        s.sampleRate = sampleRate; s.maxFrames = frames;
        s.activate(); s.loaded.store(true); s.dirty.store(false);
    }
    void readParameters() {
        struct Item { Parameter info; void* cookie; double value; };
        std::vector<Item> items;
        if (params) {
            const auto count = params->count(plugin);
            if (count > 32768) throw std::runtime_error("Plugin advertises too many parameters");
            for (uint32_t i = 0; i < count; ++i) {
                clap_param_info_t p{};
                if (!params->get_info(plugin, i, &p) || p.id == CLAP_INVALID_ID)
                    throw std::runtime_error("Invalid CLAP parameter metadata: parameter " + std::to_string(i) + " has no info");
                if (!std::isfinite(p.min_value) || !std::isfinite(p.max_value) || p.min_value > p.max_value)
                    throw std::runtime_error("Invalid CLAP parameter metadata: '" + std::string(p.name, strnlen(p.name, sizeof p.name)) + "' (" + std::to_string(p.id) +
                                             ") minimum " + std::to_string(p.min_value) + ", maximum " + std::to_string(p.max_value));
                // A default outside the range only misplaces a reset, so it is brought into the
                // range rather than refusing the plugin (Airwindows Consolidated reports garbage
                // defaults for the controls an effect does not use).
                p.default_value = std::isfinite(p.default_value) ? std::clamp(p.default_value, p.min_value, p.max_value) : p.min_value;
                double value = p.default_value;
                params->get_value(plugin, p.id, &value);
                if (!std::isfinite(value)) value = p.default_value;
                items.push_back({{p.id, std::string(p.name, strnlen(p.name, CLAP_NAME_SIZE)),
                                      std::string(p.module, strnlen(p.module, CLAP_PATH_SIZE)),
                                      p.min_value, p.max_value, p.default_value, p.flags}, p.cookie, value});
            }
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.info.id < b.info.id; });
        paramInfo.clear(); cookies.clear();
        paramValues = std::make_unique<std::atomic<double>[]>(items.size());
        for (size_t i = 0; i < items.size(); ++i) {
            if (i && items[i-1].info.id == items[i].info.id) throw std::runtime_error("Duplicate CLAP parameter ID");
            paramInfo.push_back(std::move(items[i].info)); cookies.push_back(items[i].cookie);
            paramValues[i].store(items[i].value);
        }
        applyHints(paramInfo);
        ++metadataVersion;
    }
    // Hints from oscairn.ui, each kept only where it fits its parameter: a control for stepped
    // values on a stepped parameter, a switch on two values, a range whose high end is another
    // parameter. Anything else is drawn as the host decides. A role it does not know is none.
    void applyHints(std::vector<Parameter>& list) {
        if (!ui) return;
        for (auto& p : list) {
            oscairn_param_hint_t hint{OSCAIRN_WIDGET_AUTO, 0, CLAP_INVALID_ID, 0, OSCAIRN_ROLE_NONE};
            if (!ui->param_hint(plugin, p.id, &hint)) continue;
            const bool stepped = (p.flags & CLAP_PARAM_IS_STEPPED) != 0;
            const auto values = stepped ? std::llround(p.maximum - p.minimum) + 1 : 0;
            const bool high = std::any_of(list.begin(), list.end(), [&](const Parameter& o) { return o.id == hint.range_high && o.id != p.id; });
            const bool fits = hint.widget == OSCAIRN_WIDGET_SLIDER || hint.widget == OSCAIRN_WIDGET_KNOB ||
                              ((hint.widget == OSCAIRN_WIDGET_BUTTONS || hint.widget == OSCAIRN_WIDGET_LIST) && stepped) ||
                              (hint.widget == OSCAIRN_WIDGET_SWITCH && values == 2) || (hint.widget == OSCAIRN_WIDGET_RANGE && high);
            p.widget = fits ? hint.widget : OSCAIRN_WIDGET_AUTO;
            p.rangeHigh = p.widget == OSCAIRN_WIDGET_RANGE ? hint.range_high : CLAP_INVALID_ID;
            p.priority = hint.priority;
            p.group = hint.group;
            p.role = hint.role <= OSCAIRN_ROLE_PAN ? hint.role : OSCAIRN_ROLE_NONE; // the last role this host knows
        }
    }
    // CLAP_PARAM_RESCAN_INFO: names, modules, the hidden and periodic flags and the editor hints,
    // taken in place, without a restart (an effect's mode can show and hide its controls without
    // a gap in its sound). False when more changed than that rescan may change.
    bool readParameterInfo() {
        if (!params || params->count(plugin) != paramInfo.size()) return false;
        auto next = paramInfo;
        constexpr uint32_t changeable = CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_PERIODIC;
        for (uint32_t i = 0; i < next.size(); ++i) {
            clap_param_info_t p{};
            if (!params->get_info(plugin, i, &p)) return false;
            const auto at = paramIndex(p.id);
            if (at == next.size()) return false;
            auto& info = next[at];
            info.name.assign(p.name, strnlen(p.name, CLAP_NAME_SIZE));
            info.module.assign(p.module, strnlen(p.module, CLAP_PATH_SIZE));
            info.flags = (info.flags & ~changeable) | (p.flags & changeable);
            info.widget = OSCAIRN_WIDGET_AUTO; info.priority = 0; info.rangeHigh = CLAP_INVALID_ID; info.group = 0; info.role = 0;
        }
        applyHints(next);
        Gate guard(gate);
        paramInfo = std::move(next);
        ++metadataVersion;
        return true;
    }
    void readPorts() {
        std::vector<Port> nextInputs,nextOutputs;
        // A plugin without the audio-ports extension has no audio ports. That is
        // normal for note effects such as sequencers and arpeggiators.
        const auto* audio = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
        if (audio && (!audio->count || !audio->get)) throw std::runtime_error("Incomplete CLAP audio-ports interface");
        for (bool input : {true, false}) {
            auto& ports = input ? nextInputs : nextOutputs;
            const auto count = audio ? audio->count(plugin, input) : 0;
            if (count > Host::maxAudioChannels) throw std::runtime_error("Audio bus count exceeds the 64-channel budget");
            uint32_t channels = 0;
            for (uint32_t i = 0; i < count; ++i) {
                clap_audio_port_info_t p{};
                if (!audio->get(plugin, i, input, &p) || p.channel_count == 0 || p.channel_count > Host::maxAudioChannels ||
                    channels + p.channel_count > Host::maxAudioChannels)
                    throw std::runtime_error("Invalid or unsupported audio channel layout");
                ports.push_back({p.id, std::string(p.name, strnlen(p.name, CLAP_NAME_SIZE)), p.channel_count, p.flags});
                channels += p.channel_count;
            }
        }
        auto same=[](const auto& a,const auto& b){
            return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const Port& x,const Port& y){return x.id==y.id && x.channels==y.channels;});};
        if(audioLayoutLocked && (!same(inputs,nextInputs) || !same(outputs,nextOutputs)))
            throw std::runtime_error("CLAP audio port layout changed; reload this graph node before reconnecting audio");
        inputs=std::move(nextInputs);outputs=std::move(nextOutputs);
        noteInputs.clear(); noteOutputs.clear();
        const auto* notes = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS));
        if (notes && notes->count && notes->get) {
            for (bool input : {true, false}) {
                auto& ports = input ? noteInputs : noteOutputs;
                const auto count = notes->count(plugin, input);
                if (count > 16) throw std::runtime_error("More than 16 note ports are unsupported");
                for (uint32_t i = 0; i < count; ++i) {
                    clap_note_port_info_t p{};
                    if (!notes->get(plugin, i, input, &p)) throw std::runtime_error("Invalid CLAP note port");
                    ports.push_back(p);
                }
            }
        }
        if (outputs.empty() && noteOutputs.empty()) throw std::runtime_error("Plugin has neither audio nor note outputs");
        size_t ni = 0, no = 0;
        for (const auto& p : inputs) ni += p.channels;
        for (const auto& p : outputs) no += p.channels;
        inputPointers.resize(ni); outputPointers.resize(no);
        inputBuffers.resize(inputs.size()); outputBuffers.resize(outputs.size());
        scratch.assign((ni + no) * maxFrames, 0.f);
        size_t offset = 0;
        for (size_t i = 0; i < inputs.size(); ++i) {
            inputBuffers[i] = {}; inputBuffers[i].channel_count = inputs[i].channels;
            inputBuffers[i].data32 = inputPointers.data() + offset; offset += inputs[i].channels;
        }
        offset = 0;
        for (size_t i = 0; i < outputs.size(); ++i) {
            outputBuffers[i] = {}; outputBuffers[i].channel_count = outputs[i].channels;
            outputBuffers[i].data32 = outputPointers.data() + offset; offset += outputs[i].channels;
        }
    }
    bool activate() {
        if (!std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 768000 || maxFrames == 0 || maxFrames > 65536)
            throw std::runtime_error("Invalid sample rate or maximum block size");
        readPorts(); readParameters();
        // Asked after the state is loaded and before activating, so a plugin that would not fit is
        // refused before its allocation (RFC 0025).
        hasDeclared = false;
        if (memory && memory->estimate(plugin, sampleRate, maxFrames, &declared)) {
            hasDeclared = true;
            uint64_t bytes = 0;
            for (const uint64_t part : {context ? 0 : declared.shared_bytes, declared.context_bytes, declared.asset_bytes}) {
                if (part > UINT64_MAX - bytes) throw std::runtime_error("Plugin's memory declaration is not credible"); // a sum that wraps
                bytes += part;
            }
            if (memoryRoom && bytes > memoryRoom)
                throw std::runtime_error("Plugin needs " + std::to_string((bytes + (1 << 20) - 1) >> 20) + " MiB of memory where " +
                                         std::to_string(memoryRoom >> 20) + " MiB is left");
        }
        if (activationObserver) activationObserver(activationContext, hasDeclared ? &declared : nullptr);
        if (!plugin->activate(plugin, sampleRate, 1, maxFrames)) throw std::runtime_error("Plugin activation failed");
        active = true;
        transportObserved = false; haveHarmony = false;
        latencyFrames.store(pluginLatency && pluginLatency->get ? pluginLatency->get(plugin) : 0);
        return true;
    }
    void fail(const std::string& why) { message = why; failed.store(true); }
    static uint32_t CLAP_ABI eventCount(const clap_input_events_t* list) { return static_cast<Impl*>(list->ctx)->chunkCount; }
    static const clap_event_header_t* CLAP_ABI eventGet(const clap_input_events_t* list, uint32_t i) {
        auto& s = *static_cast<Impl*>(list->ctx);
        return i < s.chunkCount ? s.chunkEvents[i].header() : nullptr;
    }
    static bool CLAP_ABI queryEventSpace(const clap_host_t* h, const char* name, uint16_t* space) {
        if (!name || !space) return false;
        if (!std::strcmp(name, OSCAIRN_EVENT_SPACE_BEAT)) { self(h).wantsBeat.store(true); *space = Host::beatSpace; return true; }
        if (!std::strcmp(name, OSCAIRN_EVENT_SPACE_HARMONY)) { self(h).wantsHarmony.store(true); *space = Host::harmonySpace; return true; }
        if (std::strcmp(name, OSCAIRN_EVENT_SPACE_PARAM_TARGET) != 0) return false;
        *space = Host::paramTargetSpace;
        return true;
    }
    static bool CLAP_ABI outputPush(const clap_output_events_t* list, const clap_event_header_t* event) {
        auto& s = *static_cast<Impl*>(list->ctx);
        if (!validEvent(event) || event->time >= s.currentProcessFrames) {
            if (event && event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_MIDI_SYSEX) s.dropped.fetch_add(1); // over the limit: counted
            return false;
        }
        if (event->space_id == Host::beatSpace || event->space_id == Host::harmonySpace) return false; // the host's to send
        const bool core = event->space_id == CLAP_CORE_EVENT_SPACE_ID;
        if (core && event->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* p = reinterpret_cast<const clap_event_param_value_t*>(event);
            auto i = s.paramIndex(p->param_id);
            if (i < s.paramInfo.size() && std::isfinite(p->value)) {
                if (s.paramValues[i].exchange(p->value) != p->value) { s.dirty.store(true); s.parameterReports.fetch_add(1, std::memory_order_release); }
            }
        }
        Event copy; std::memcpy(copy.bytes.data(), event, event->size);
        reinterpret_cast<clap_event_header_t*>(copy.bytes.data())->time += s.outputFrameOffset;
        if (core && event->type == CLAP_EVENT_MIDI_SYSEX) {
            // MIDI data bytes are 7-bit. The plugin's buffer is valid only during this call: copy it.
            const auto& x = *reinterpret_cast<const clap_event_midi_sysex_t*>(event);
            bool data = true;
            for (uint32_t i = 1; i + 1 < x.size; ++i) data = data && x.buffer[i] < 0x80;
            if (!data || !s.inProcess || !s.sysexOutput.hold(copy)) { s.dropped.fetch_add(1); return false; }
        }
        // Parameter and gesture events go to the control thread. Everything else
        // (notes, MIDI, expressions) is kept for the caller of process(), which
        // routes it to the next plugin in the same block.
        const bool control = core && (event->type == CLAP_EVENT_PARAM_VALUE || event->type == CLAP_EVENT_PARAM_MOD ||
                             event->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || event->type == CLAP_EVENT_PARAM_GESTURE_END);
        if (!control) {
            if (s.inProcess && s.blockOutputCount < maxBlockEvents) { s.blockOutput[s.blockOutputCount++] = copy; return true; }
            s.dropped.fetch_add(1); return false;
        }
        if (s.emitted.push(copy)) return true;
        s.outputDrops.fetch_add(1);
        s.dropped.fetch_add(1); return false;
    }
    void flushParameters() {
        if (!params || !params->flush) return;
        AudioScope scope(this);
        chunkCount = 0;
        outputFrameOffset = 0; currentProcessFrames = 1;
        // Only parameter events belong to flush(). Preserve musical events by
        // rendering them normally; service() never drains the command queue.
        clap_input_events_t in{this, eventCount, eventGet};
        clap_output_events_t out{this, outputPush};
        params->flush(plugin, &in, &out);
    }
    Event translate(Event event) const noexcept {
        auto* h = reinterpret_cast<clap_event_header_t*>(event.bytes.data());
        if (h->type == CLAP_EVENT_PARAM_VALUE) {
            auto* p = reinterpret_cast<clap_event_param_value_t*>(event.bytes.data());
            const auto i = paramIndex(p->param_id);
            if (i < cookies.size()) p->cookie = cookies[i];
        } else if (h->type == CLAP_EVENT_MIDI) {
            const auto& m = *reinterpret_cast<const clap_event_midi_t*>(event.bytes.data());
            if (m.port_index < noteInputs.size()) {
                const auto dialect = noteInputs[m.port_index].supported_dialects;
                if ((dialect & CLAP_NOTE_DIALECT_CLAP) &&
                    !(dialect & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE))) {
                    const auto status = m.data[0] & 0xf0;
                    if (status == 0x90 || status == 0x80)
                        return Event::note(status == 0x90 && m.data[2] != 0, m.data[1], m.data[2] / 127.0,
                                           h->time, m.data[0] & 15, m.port_index);
                }
            }
        } else if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
            const auto& n = *reinterpret_cast<const clap_event_note_t*>(event.bytes.data());
            if (n.port_index >= 0 && static_cast<size_t>(n.port_index) < noteInputs.size() &&
                !(noteInputs[n.port_index].supported_dialects & CLAP_NOTE_DIALECT_CLAP))
                return Event::midi(static_cast<uint8_t>((h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | n.channel),
                                   static_cast<uint8_t>(n.key), static_cast<uint8_t>(std::clamp(n.velocity, 0., 1.) * 127), h->time,
                                   static_cast<uint16_t>(n.port_index));
        }
        return event;
    }
    bool transportChanged(const clap_event_transport_t* current, uint32_t frames) noexcept {
        bool changed = !transportObserved || hadTransport != (current != nullptr);
        if (current && hadTransport && transportObserved) {
            const auto& old = previousTransport;
            changed |= current->flags != old.flags;
            if (current->flags & CLAP_TRANSPORT_HAS_TEMPO)
                changed |= current->tempo != old.tempo || current->tempo_inc != old.tempo_inc;
            if (current->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE)
                changed |= current->tsig_num != old.tsig_num || current->tsig_denom != old.tsig_denom;
            const auto elapsed = (old.flags & CLAP_TRANSPORT_IS_PLAYING) ? previousTransportFrames / sampleRate : 0.;
            if (current->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) {
                const auto expected = static_cast<long double>(old.song_pos_beats) +
                    elapsed * old.tempo / 60. * CLAP_BEATTIME_FACTOR;
                changed |= std::abs(static_cast<long double>(current->song_pos_beats) - expected) > 2.;
                changed |= current->loop_start_beats != old.loop_start_beats || current->loop_end_beats != old.loop_end_beats ||
                           current->bar_start != old.bar_start || current->bar_number != old.bar_number;
            }
            if (current->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) {
                const auto expected = static_cast<long double>(old.song_pos_seconds) + elapsed * CLAP_SECTIME_FACTOR;
                changed |= std::abs(static_cast<long double>(current->song_pos_seconds) - expected) > 2.;
                changed |= current->loop_start_seconds != old.loop_start_seconds || current->loop_end_seconds != old.loop_end_seconds;
            }
        }
        if (current) previousTransport = *current;
        hadTransport = current != nullptr; transportObserved = true; previousTransportFrames = frames;
        return changed;
    }
    bool accepts(const Event& event) const noexcept {
        const auto* h = event.header();
        if (h->type == CLAP_EVENT_NOTE_END) return false; // plugin-to-host only
        if (h->type == CLAP_EVENT_MIDI_SYSEX) { // on a note port that takes MIDI, as CLAP's dialects say
            const auto port = reinterpret_cast<const clap_event_midi_sysex_t*>(h)->port_index;
            return port < noteInputs.size() && (noteInputs[port].supported_dialects & CLAP_NOTE_DIALECT_MIDI);
        }
        if (h->type == CLAP_EVENT_PARAM_VALUE || h->type == CLAP_EVENT_PARAM_MOD ||
            h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type == CLAP_EVENT_PARAM_GESTURE_END) {
            const auto id = h->type == CLAP_EVENT_PARAM_VALUE ? reinterpret_cast<const clap_event_param_value_t*>(h)->param_id :
                            h->type == CLAP_EVENT_PARAM_MOD ? reinterpret_cast<const clap_event_param_mod_t*>(h)->param_id :
                            reinterpret_cast<const clap_event_param_gesture_t*>(h)->param_id;
            auto i = paramIndex(id);
            return i < paramInfo.size() && !(paramInfo[i].flags & CLAP_PARAM_IS_READONLY);
        }
        if (h->type == CLAP_EVENT_MIDI) {
            const auto& m = *reinterpret_cast<const clap_event_midi_t*>(h);
            return m.port_index < noteInputs.size() &&
                   (noteInputs[m.port_index].supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE));
        }
        if (h->type == CLAP_EVENT_MIDI2) return false; // not advertised by this host
        const int port = h->type == CLAP_EVENT_NOTE_EXPRESSION ? reinterpret_cast<const clap_event_note_expression_t*>(h)->port_index :
                         reinterpret_cast<const clap_event_note_t*>(h)->port_index;
        if (port == -1) return !noteInputs.empty();
        return port >= 0 && static_cast<size_t>(port) < noteInputs.size() &&
               (noteInputs[static_cast<size_t>(port)].supported_dialects & CLAP_NOTE_DIALECT_CLAP);
    }
};

Host::Host() : impl(std::make_unique<Impl>()) {}
Host::~Host() { Gate guard(impl->gate); impl->close(); }
bool Host::load(const Descriptor& descriptor, double sampleRate, uint32_t frames, const std::vector<uint8_t>& bytes) {
    auto& s = *impl;
    if (!s.main()) return false;
    Gate guard(s.gate);
    s.close(); s.message.clear(); s.info = descriptor; s.context = false;
    try {
        s.library = detail::openLibrary(descriptor.path);
        auto* factory = s.library->factory();
        if (!factory) throw std::runtime_error("No CLAP plugin factory");
        const clap_plugin_descriptor_t* chosen = nullptr;
        for (uint32_t i = 0, count = std::min(4096u, factory->get_plugin_count(factory)); i < count; ++i) {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (d && d->id && d->name && descriptor.id == d->id && clap_version_is_compatible(d->clap_version)) { chosen = d; break; }
        }
        if (!chosen) throw std::runtime_error("Saved plugin ID is absent from this binary");
        s.info = detail::describe(s.library->path, *chosen);
        s.plugin = factory->create_plugin(factory, &s.host, chosen->id);
        s.initialize(sampleRate,frames,bytes);
        return true;
    } catch (const std::exception& e) { s.message = e.what(); }
      catch (...) { s.message = "Plugin threw during initialization"; }
    s.close(); s.failed.store(true); return false;
}
bool Host::loadEffectContext(const Host& source) {
    auto& s=*impl;const auto& parent=*source.impl;
    if(this==&source || !s.main() || !parent.plugin || !parent.loaded.load())return false;
    Gate guard(s.gate);s.close();s.message.clear();
    try {
        const auto* ext=static_cast<const oscairn_plugin_effect_contexts_t*>(parent.plugin->get_extension(parent.plugin,OSCAIRN_EXT_EFFECT_CONTEXTS));
        if(!ext || !ext->create || !ext->resources)throw std::runtime_error("Plugin has no native effect contexts");
        s.library=parent.library;s.info=parent.info;s.context=true;s.memoryRoom=parent.memoryRoom;s.offline=parent.offline;s.assets=parent.assets;
        s.plugin=ext->create(parent.plugin,&s.host);
        if(!s.plugin)throw std::runtime_error("Effect context capacity exhausted");
        s.initialize(parent.sampleRate,parent.maxFrames,{});
        return true;
    } catch(const std::exception& e){s.message=e.what();}
      catch(...){s.message="Plugin threw preparing an effect context";}
    s.close();s.failed.store(true);return false;
}
void Host::setMemoryRoom(uint64_t bytes) noexcept { impl->memoryRoom = bytes; }
void Host::setOfflineRendering(bool offline) noexcept { impl->offline = offline; }
bool Host::declaredMemory(oscairn_memory_t& result) const {
    if (!impl->plugin || !impl->hasDeclared) return false;
    result = impl->declared; return true;
}
bool Host::currentMemory(oscairn_memory_t& result) const {
    auto& s = *impl;
    return s.main() && s.plugin && s.memory && s.active && s.memory->current(s.plugin, &result);
}
void Host::observeActivations(ActivationObserver observer, void* context) noexcept { activationObserver = observer; activationContext = context; }
bool Host::effectResources(oscairn_effect_resources_t& result) const {
    if(!impl->plugin)return false;
    const auto* ext=static_cast<const oscairn_plugin_effect_contexts_t*>(impl->plugin->get_extension(impl->plugin,OSCAIRN_EXT_EFFECT_CONTEXTS));
    return ext && ext->resources && ext->resources(impl->plugin,&result);
}
void Host::unload() { auto& s = *impl; if (!s.main()) return; Gate guard(s.gate); s.close(); }
bool Host::reconfigure(double rate, uint32_t frames) {
    auto& s = *impl;
    if (!s.main() || !loaded()) return false;
    Gate guard(s.gate);
    try { s.deactivate(); s.sampleRate = rate; s.maxFrames = frames; s.activate(); s.failed.store(false); return true; }
    catch (const std::exception& e) { s.fail(e.what()); return false; }
}
void Host::lockAudioLayout() { if(impl->main() && impl->plugin)impl->audioLayoutLocked=true; }
void Host::service() {
    auto& s = *impl;
    if (!s.main() || !s.plugin) return;
    // Main-thread callbacks, timers, fd callbacks and parameter value reads may run
    // while the plugin processes audio (CLAP's threading model allows it), so they
    // do not take the gate: taking it here silenced audio whenever they ran long.
    // Only the operations that change what the audio thread uses take the gate.
    try {
        if (s.callback.exchange(false)) s.plugin->on_main_thread(s.plugin);
        const auto now = std::chrono::steady_clock::now();
        // Callbacks may register/unregister timers and fds, so iterate snapshots.
        auto timers = s.timers;
        if (s.timerSupport && s.timerSupport->on_timer) for (const auto& timer : timers) if (now >= timer.next) {
            auto it = std::find_if(s.timers.begin(), s.timers.end(), [&](const Impl::Timer& t) { return t.id == timer.id; });
            if (it != s.timers.end()) { it->next = now + std::chrono::milliseconds(timer.period); s.timerSupport->on_timer(s.plugin, timer.id); }
        }
#ifndef _WIN32
        auto fds = s.fds;
        if (s.fdSupport && s.fdSupport->on_fd) for (const auto& item : fds) {
            pollfd fd{item.fd, static_cast<short>(((item.flags & CLAP_POSIX_FD_READ) ? POLLIN : 0) |
                                               ((item.flags & CLAP_POSIX_FD_WRITE) ? POLLOUT : 0)), 0};
            if (::poll(&fd, 1, 0) > 0) {
                auto flags = static_cast<clap_posix_fd_flags_t>(((fd.revents & POLLIN) ? CLAP_POSIX_FD_READ : 0) |
                       ((fd.revents & POLLOUT) ? CLAP_POSIX_FD_WRITE : 0) |
                       ((fd.revents & (POLLERR | POLLHUP | POLLNVAL)) ? CLAP_POSIX_FD_ERROR : 0));
                if (flags && std::any_of(s.fds.begin(), s.fds.end(), [&](const Impl::Fd& f) { return f.fd == item.fd; }))
                    s.fdSupport->on_fd(s.plugin, item.fd, flags);
            }
        }
#endif
        s.serviceGui();
        const auto flags = s.rescan.exchange(0);
        const bool restart = s.restart.exchange(false);
        if (restart || (flags & CLAP_PARAM_RESCAN_ALL) || ((flags & CLAP_PARAM_RESCAN_INFO) && !s.readParameterInfo())) {
            Gate guard(s.gate);
            s.deactivate(); s.activate();
        } else if (flags & CLAP_PARAM_RESCAN_VALUES) {
            for (size_t i = 0; i < s.paramInfo.size(); ++i) { double value; if (s.params->get_value(s.plugin, s.paramInfo[i].id, &value) && std::isfinite(value)) s.paramValues[i].store(value); }
        }
        // While the plugin is processing, process() performs requested flushes on
        // the audio thread. Otherwise flush here, excluding a process() that starts.
        if (!s.processing.load() && s.flush.load()) {
            Gate guard(s.gate);
            if (!s.processing.load() && s.flush.exchange(false)) s.flushParameters();
        }
        if (s.failed.load()) { Gate guard(s.gate); s.stop(); }
    } catch (const std::exception& e) { s.fail(e.what()); }
      catch (...) { s.fail("Plugin threw during main-thread service"); }
}
bool Host::saveState(std::vector<uint8_t>& destination) {
    auto& s = *impl;
    if (!s.main() || !s.plugin || !s.state || !s.state->save) { s.message = "Plugin does not support state saving"; return false; }
    try {
        // Parameter changes still queued for the audio thread must reach the plugin
        // before it saves, so a save right after moving a slider includes the move.
        // While audio is running, wait briefly for process() to deliver them, then
        // save without the gate so audio keeps playing: CLAP allows state saving on
        // the main thread while the plugin processes.
        if (s.pendingCommands.load() > 0) {
            using clock = std::chrono::steady_clock;
            auto audioBlocks = [&] { return s.blocks.load() + s.idleBlocks.load() + s.controlSilences.load(); };
            auto seen = audioBlocks();
            auto lastProgress = clock::now();
            const auto deadline = lastProgress + std::chrono::milliseconds(100);
            while (s.pendingCommands.load() > 0 && clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::microseconds(500));
                if (audioBlocks() != seen) { seen = audioBlocks(); lastProgress = clock::now(); }
                else if (clock::now() - lastProgress > std::chrono::milliseconds(10)) break; // audio is not running
            }
        }
        if (s.pendingCommands.load() == 0 && !s.exclusiveState) {
            Stream stream; clap_ostream_t output{&stream, Stream::write};
            if (!s.state->save(s.plugin, &output) || stream.failed) { s.message = "Plugin state saving failed"; return false; }
            destination = std::move(stream.bytes); s.dirty.store(false); return true;
        }
    } catch (...) { s.message = "Plugin threw during state saving"; return false; }

    // Audio is not running: deliver the queued changes through a parameter flush
    // under the gate, then save.
    Gate guard(s.gate);
    try {
        s.stop();
        s.chunkCount = 0;
        s.outputFrameOffset = 0; s.currentProcessFrames = 1;
        Event e;
        while (s.commands.pop(e)) {
            s.pendingCommands.fetch_sub(1);
            const auto type = e.header()->type;
            if (type == CLAP_EVENT_PARAM_VALUE || type == CLAP_EVENT_PARAM_GESTURE_BEGIN || type == CLAP_EVENT_PARAM_GESTURE_END)
                if (s.chunkCount < maxBlockEvents) s.chunkEvents[s.chunkCount++] = s.translate(e);
            if (type != CLAP_EVENT_PARAM_VALUE && type != CLAP_EVENT_PARAM_GESTURE_BEGIN && type != CLAP_EVENT_PARAM_GESTURE_END) {
                if (s.carryCount < maxBlockEvents) s.carryEvents[s.carryCount++] = e;
                else { s.panic.store(true); s.dropped.fetch_add(1); }
            }
        }
        if (s.params && s.params->flush) {
            AudioScope scope(&s);
            clap_input_events_t in{&s, Impl::eventCount, Impl::eventGet};
            clap_output_events_t out{&s, Impl::outputPush};
            s.params->flush(s.plugin, &in, &out);
        }
        Stream stream; clap_ostream_t output{&stream, Stream::write};
        if (!s.state->save(s.plugin, &output) || stream.failed) { s.message = "Plugin state saving failed"; return false; }
        destination = std::move(stream.bytes); s.dirty.store(false); return true;
    } catch (...) { s.message = "Plugin threw during state saving"; return false; }
}
bool Host::loadState(const std::vector<uint8_t>& bytes) {
    auto& s = *impl;
    if (!s.main() || !s.plugin || !s.state || !s.state->load || bytes.size() > maxStateBytes) return false;
    Gate guard(s.gate);
    try {
        s.deactivate();
        Stream stream; stream.bytes = bytes; clap_istream_t input{&stream, Stream::read};
        s.lastLoggedError.clear();
        if (!s.state->load(s.plugin, &input)) {
            s.activate(); s.message = "Plugin rejected state" + (s.lastLoggedError.empty() ? std::string() : ": " + s.lastLoggedError); return false;
        }
        Event e; while (s.commands.pop(e)) s.pendingCommands.fetch_sub(1);
        s.carryCount = 0;
        s.activate(); s.dirty.store(false); s.failed.store(false); return true;
    } catch (const std::exception& e) { s.fail(std::string("Plugin state restoration failed: ") + e.what()); return false; }
      catch (...) { s.fail("Plugin state restoration failed"); return false; }
}
std::vector<Preset> Host::presets() {
    auto& s = *impl;
    if (!s.main() || !s.library || !s.library->entry) return {};
    return indexPresets(s.library->entry, {s.info.id}).presets[s.info.id];
}

bool Host::loadPreset(const Preset& preset) {
    auto& s = *impl;
    if (!s.main() || !s.plugin) return false;
    auto* load = static_cast<const clap_plugin_preset_load_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_PRESET_LOAD));
    if (!load) load = static_cast<const clap_plugin_preset_load_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_PRESET_LOAD_COMPAT));
    if (!load || !load->from_location) { s.message = "Plugin cannot load presets"; return false; }
    Gate guard(s.gate);
    try {
        s.message.clear();
        if (!load->from_location(s.plugin, preset.kind, preset.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr : preset.location.c_str(),
                                 preset.loadKey.empty() ? nullptr : preset.loadKey.c_str())) {
            if (s.message.empty()) s.message = "Plugin did not load the preset";
            return false;
        }
        // The preset changed the plugin's parameters: read them back.
        if (s.params) for (size_t i = 0; i < s.paramInfo.size(); ++i) { double value; if (s.params->get_value(s.plugin, s.paramInfo[i].id, &value) && std::isfinite(value)) s.paramValues[i].store(value); }
        s.dirty.store(true);
        return true;
    } catch (...) { s.fail("Plugin threw while loading a preset"); return false; }
}

void Host::setAssets(AssetResolver* assets) noexcept { impl->assets = assets; }
bool Host::declaresAssets() const noexcept { return impl->assetDependencies; }
std::vector<AssetReference> Host::assetDependencies() const {
    auto& s = *impl; std::vector<AssetReference> result;
    if (!s.assetDependencies) return result;
    const auto count = std::min<uint32_t>(s.assetDependencies->count(s.plugin), 65536);
    for (uint32_t i = 0; i < count; ++i) {
        oscairn_asset_ref_t ref{};
        if (!s.assetDependencies->get(s.plugin, i, &ref)) throw std::runtime_error("Plugin did not describe an asset it uses");
        result.push_back({{ref.library, strnlen(ref.library, sizeof(ref.library))}, {ref.path, strnlen(ref.path, sizeof(ref.path))}, {ref.sha256, strnlen(ref.sha256, sizeof(ref.sha256))},
                          s.assetOptional && s.assetDependencies->optional(s.plugin, i)});
    }
    return result;
}
bool Host::hasGui() const noexcept { return impl->guiApi != nullptr; }
bool Host::guiOpen() const noexcept { return impl->guiCreated.load(); }
bool Host::showGui(const std::string& title) {
    auto& s = *impl;
    if (!s.main()) return false;
    if (!s.plugin || !s.guiApi) { s.message = "Plugin has no window here"; return false; }
    try { s.showGui(title); return true; }
    catch (const std::exception& e) { s.message = e.what(); }
    catch (...) { s.message = "Plugin threw opening its window"; }
    s.destroyGui(); return false;
}
void Host::closeGui() { auto& s = *impl; if (s.main() && s.plugin) s.destroyGui(); }
bool Host::supportsState() const noexcept { return impl->state && impl->state->save && impl->state->load; }
bool Host::supportsSoundSnapshot() const noexcept { return impl->exclusiveState && supportsState(); }
bool Host::loaded() const noexcept { return impl->loaded.load(); }
bool Host::faulted() const noexcept { return impl->failed.load(); }
const std::string& Host::error() const noexcept { return impl->message; }
const Descriptor& Host::descriptor() const noexcept { return impl->info; }
const std::vector<Parameter>& Host::parameters() const noexcept { return impl->paramInfo; }
uint64_t Host::metadataRevision() const noexcept { return impl->metadataVersion; }
const std::vector<Port>& Host::inputPorts() const noexcept { return impl->inputs; }
const std::vector<Port>& Host::outputPorts() const noexcept { return impl->outputs; }
bool Host::usesClapNotes(uint32_t port) const noexcept { return port < impl->noteInputs.size() && (impl->noteInputs[port].supported_dialects & CLAP_NOTE_DIALECT_CLAP); }
uint32_t Host::noteInputPorts() const noexcept { return static_cast<uint32_t>(impl->noteInputs.size()); }
uint32_t Host::noteOutputPorts() const noexcept { return static_cast<uint32_t>(impl->noteOutputs.size()); }
double Host::parameterValue(clap_id id) const noexcept {
    auto& s = *impl; const auto i = s.paramIndex(id);
    return i < s.paramInfo.size() ? s.paramValues[i].load() : 0;
}
std::string Host::parameterText(clap_id id, double value) const {
    char text[256]{};
    if (impl->params && impl->params->value_to_text && impl->params->value_to_text(impl->plugin, id, value, text, sizeof(text)))
        return {text, strnlen(text, sizeof(text))};
    return std::to_string(value);
}
bool Host::setParameter(clap_id id, double value) noexcept {
    auto& s = *impl; const auto index = s.paramIndex(id);
    if (index == s.paramInfo.size() || !std::isfinite(value) || (s.paramInfo[index].flags & CLAP_PARAM_IS_READONLY)) return false;
    value = std::clamp(value, s.paramInfo[index].minimum, s.paramInfo[index].maximum);
    if (s.paramInfo[index].flags & CLAP_PARAM_IS_STEPPED) value = std::trunc(value);
    clap_event_param_value_t event{};
    event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_IS_LIVE};
    event.param_id = id; event.note_id = -1; event.port_index = event.channel = event.key = -1; event.value = value;
    if (!enqueue(Event::from(event))) return false;
    s.paramValues[index].store(value); s.dirty.store(true); return true;
}
bool Host::parameterGesture(clap_id id, bool begin) noexcept {
    clap_event_param_gesture_t event{};
    event.header = {sizeof(event), 0, CLAP_CORE_EVENT_SPACE_ID,
                    static_cast<uint16_t>(begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END), CLAP_EVENT_IS_LIVE};
    event.param_id = id; return enqueue(Event::from(event));
}
bool Host::stateDirty() const noexcept { return impl->dirty.load(); }
uint64_t Host::parameterReports() const noexcept { return impl->parameterReports.load(std::memory_order_acquire); }
uint32_t Host::latency() const noexcept { return impl->latencyFrames.load(); }
bool Host::asleep() const noexcept { return impl->sleeping; }
Statistics Host::statistics() const noexcept { auto& s = *impl; return {s.blocks.load(), s.dropped.load(), s.errors.load(), s.lastUs.load(), s.maxUs.load(), s.deadlineMisses.load(), s.controlSilences.load(), s.idleBlocks.load()}; }
bool Host::enqueue(const Event& e) noexcept {
    auto& s = *impl;
    if (!validEvent(e.header()) || e.header()->space_id != CLAP_CORE_EVENT_SPACE_ID || e.header()->type == CLAP_EVENT_MIDI_SYSEX) return false;
    // Count before publishing, so process() never takes an event it has not counted.
    s.pendingCommands.fetch_add(1);
    if (s.commands.push(e)) { s.wake.store(true); return true; }
    s.pendingCommands.fetch_sub(1);
    s.dropped.fetch_add(1);
    // Control callers can retain and retry refused parameter/gesture commands.
    // Resetting here would discard already accepted commands and cut held notes.
    const auto type = e.header()->type;
    if (type != CLAP_EVENT_PARAM_VALUE && type != CLAP_EVENT_PARAM_MOD &&
        type != CLAP_EVENT_PARAM_GESTURE_BEGIN && type != CLAP_EVENT_PARAM_GESTURE_END)
        s.panic.store(true);
    return false;
}
bool Host::popOutput(Event& e) noexcept { return impl->emitted.pop(e); }
uint64_t Host::outputEventDrops() const noexcept { return impl->outputDrops.load(); }
const Event* Host::outputEvents(uint32_t& count) const noexcept { count = impl->publishedOutputCount; return impl->blockOutput.data(); }
void Host::panic() noexcept { impl->panic.store(true); impl->wake.store(true); }
void Host::reset() { auto& s = *impl; if (!s.main() || !s.plugin) return; Gate guard(s.gate); AudioScope scope(&s); s.plugin->reset(s.plugin); s.sleeping = false; }

bool Host::process(const float* const* input, uint32_t ni, float* const* output, uint32_t no, uint32_t frames,
                   const Event* incoming, uint32_t count, const clap_event_transport_t* transport) noexcept {
    auto& s = *impl;
    auto silence = [&] { if (output) for (uint32_t c = 0; c < no; ++c) if (output[c]) std::fill_n(output[c], frames, 0.f); };
    s.publishedOutputCount = 0;
    if (!frames) return true;
    // A note-only plugin (no audio outputs) may be processed with no output buffers.
    if ((no && !output) || (count && !incoming)) { silence(); return false; }
    if (s.gate.test_and_set(std::memory_order_acquire)) {
        s.controlSilences.fetch_add(1); silence(); return false;
    }
    struct Release { std::atomic_flag& flag; ~Release() { flag.clear(std::memory_order_release); } } release{s.gate};
    s.blockOutputCount = 0; s.sysexOutput.used = 0; s.inProcess = true;
    struct Publish {
        Impl& s;
        ~Publish() { s.inProcess = false; s.publishedOutputCount = s.blockOutputCount; }
    } publish{s}; // declared after release, so it runs while the gate is still held
    AudioScope scope(&s);
    if (!s.plugin || !s.active || s.failed.load()) { silence(); return false; }
    const auto start = std::chrono::steady_clock::now();
    struct Acknowledge {
        Impl& s; uint32_t count=0;
        ~Acknowledge() { if(count) s.pendingCommands.fetch_sub(count, std::memory_order_release); }
    } acknowledged{s}; // Saving must wait until plugin processing, not just queue removal.
    try {
        uint32_t total = 0;
        auto deliver = [&](Event e) {
            // A malformed or late event is dropped on its own; resetting the plugin
            // for it would cut every sounding note on stage.
            // Oscairn's host-to-plugin events go only to a plugin that asked for them, as they are.
            if (e.header()->space_id == Host::beatSpace || e.header()->space_id == Host::harmonySpace) {
                if (!validEvent(e.header()) || e.header()->time >= frames) { s.dropped.fetch_add(1); return; }
                if (e.header()->space_id == Host::beatSpace && !s.wantsBeat.load(std::memory_order_relaxed)) return;
                if (e.header()->space_id == Host::harmonySpace) {
                    const auto& h = reinterpret_cast<const oscairn_event_harmony_t*>(e.header())->harmony;
                    if (!s.wantsHarmony.load(std::memory_order_relaxed) || (s.haveHarmony && !std::memcmp(&h, &s.lastHarmony, sizeof h))) return;
                    s.lastHarmony = h; s.haveHarmony = true; s.publishHarmony(h);
                }
                if (total == Impl::renderEvents) { s.dropped.fetch_add(1); return; }
                s.ordered[total] = {e, total}; ++total; return;
            }
            // Events in other spaces (param-target) are for the caller, never for a plugin.
            if (!validEvent(e.header()) || e.header()->space_id != CLAP_CORE_EVENT_SPACE_ID || e.header()->time >= frames) { s.dropped.fetch_add(1); return; }
            // Overflow can drop note-offs, so it still resets the plugin rather than
            // risk stuck notes.
            if (total == Impl::renderEvents) { s.dropped.fetch_add(1); s.panic.store(true); return; }
            auto translated = s.translate(e);
            if (!s.accepts(translated)) { s.dropped.fetch_add(1); return; }
            s.ordered[total] = {translated, total}; ++total;
        };
        auto append = [&](Event e) {
            const auto* h=e.header();
            if(validEvent(h) && h->space_id==CLAP_CORE_EVENT_SPACE_ID &&
               (h->type==CLAP_EVENT_NOTE_OFF || h->type==CLAP_EVENT_NOTE_CHOKE)) {
                const auto n=*reinterpret_cast<const clap_event_note_t*>(h);
                for(size_t port=0;port<s.noteInputs.size();++port)if(n.port_index<0 || n.port_index==static_cast<int>(port)) {
                    if(usesClapNotes(static_cast<uint32_t>(port))) {
                        auto copy=n;copy.port_index=static_cast<int16_t>(port);deliver(Event::from(copy));
                    } else for(int channel=0;channel<16;++channel)if(n.channel<0 || n.channel==channel) {
                        if(n.key<0) deliver(Event::midi(0xb0|channel,h->type==CLAP_EVENT_NOTE_CHOKE?120:123,0,h->time,port));
                        else deliver(Event::midi(0x80|channel,n.key,static_cast<uint8_t>(n.velocity*127),h->time,port));
                    }
                }
            } else deliver(e);
        };
        Event event;
        for (uint32_t i = 0; i < s.carryCount; ++i) append(s.carryEvents[i]);
        s.carryCount = 0;
        const uint32_t direct = std::min(count, maxProcessEvents);
        while (acknowledged.count < maxBlockEvents && s.commands.pop(event)) { ++acknowledged.count; append(event); }
        for (uint32_t i = 0; i < direct; ++i) append(incoming[i]);
        if (count > maxProcessEvents) { s.dropped.fetch_add(count - maxProcessEvents); s.panic.store(true); }
        if (s.panic.exchange(false)) {
            // Never leave a stuck gate after an event overflow. Reset is an
            // audio-thread operation and covers every note channel and port.
            s.plugin->reset(s.plugin); total = 0; s.sleeping = false; s.haveHarmony = false; // the harmony comes again next block
        }
        std::sort(s.ordered.begin(), s.ordered.begin() + total, [](const Impl::TimedEvent& a, const Impl::TimedEvent& b) {
            return a.event.header()->time < b.event.header()->time ||
                (a.event.header()->time == b.event.header()->time && a.order < b.order);
        });
        bool hasInput = false;
        if (input) for (uint32_t c = 0; c < ni && !hasInput; ++c) if (input[c])
            for (uint32_t i = 0; i < frames; ++i) if (input[c][i] != 0.f) { hasInput = true; break; }
        const bool transportWake = s.transportChanged(transport, frames);
        if (s.sleeping && !total && !hasInput && !transportWake && !s.wake.exchange(false) && !s.flush.load()) { s.idleBlocks.fetch_add(1); s.steadyTime += frames; silence(); return true; }
        s.wake.store(false); s.sleeping = false;
        if (!s.processing) {
            if (!s.plugin->start_processing(s.plugin)) { s.failed.store(true); s.errors.fetch_add(1); silence(); return false; }
            s.processing = true;
        }
        if (s.flush.exchange(false)) s.flushParameters();
        uint32_t next = 0;
        for (uint32_t done = 0; done < frames;) {
            const auto n = std::min(s.maxFrames, frames - done);
            s.chunkCount = 0;
            s.outputFrameOffset = done; s.currentProcessFrames = n;
            while (next < total && s.ordered[next].event.header()->time < done + n) {
                auto e = s.ordered[next++].event;
                reinterpret_cast<clap_event_header_t*>(e.bytes.data())->time -= done;
                s.chunkEvents[s.chunkCount++] = e;
            }
            for (size_t c = 0; c < s.inputPointers.size(); ++c) {
                auto* target = s.scratch.data() + c * s.maxFrames;
                if (input && c < ni && input[c]) std::copy_n(input[c] + done, n, target);
                else std::fill_n(target, n, 0.f);
                s.inputPointers[c] = target;
            }
            for (size_t c = 0; c < s.outputPointers.size(); ++c) {
                auto* target = s.scratch.data() + (s.inputPointers.size() + c) * s.maxFrames;
                std::fill_n(target, n, 0.f); s.outputPointers[c] = target;
            }
            clap_input_events_t inEvents{&s, Impl::eventCount, Impl::eventGet};
            clap_output_events_t outEvents{&s, Impl::outputPush};
            clap_process_t block{};
            block.steady_time = s.steadyTime; block.frames_count = n;
            block.audio_inputs = s.inputBuffers.data(); block.audio_inputs_count = static_cast<uint32_t>(s.inputBuffers.size());
            block.audio_outputs = s.outputBuffers.data(); block.audio_outputs_count = static_cast<uint32_t>(s.outputBuffers.size());
            block.in_events = &inEvents; block.out_events = &outEvents;
            clap_event_transport_t position{};
            if (transport) {
                position = *transport;
                if ((position.flags & CLAP_TRANSPORT_IS_PLAYING) && (position.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE))
                    position.song_pos_seconds += static_cast<clap_sectime>((static_cast<double>(done) / s.sampleRate) * CLAP_SECTIME_FACTOR);
                if ((position.flags & CLAP_TRANSPORT_IS_PLAYING) && (position.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (position.flags & CLAP_TRANSPORT_HAS_TEMPO))
                    position.song_pos_beats += static_cast<clap_beattime>((static_cast<double>(done) / s.sampleRate) * position.tempo / 60. * CLAP_BEATTIME_FACTOR);
                position.header.time = 0; block.transport = &position;
            }
            const auto status = s.plugin->process(s.plugin, &block);
            if (status < CLAP_PROCESS_CONTINUE || status > CLAP_PROCESS_SLEEP) { s.failed.store(true); s.errors.fetch_add(1); silence(); return false; }
            for (uint32_t c = 0; c < no; ++c) if (output[c]) {
                auto* destination = output[c] + done;
                if (c < s.outputPointers.size()) {
                    for (uint32_t i = 0; i < n; ++i) {
                        const auto value = s.outputPointers[c][i];
                        if (!std::isfinite(value)) { s.failed.store(true); s.errors.fetch_add(1); silence(); return false; }
                        destination[i] = value;
                    }
                } else std::fill_n(destination, n, 0.f);
            }
            s.sleeping = status == CLAP_PROCESS_SLEEP;
            s.steadyTime += n; done += n;
        }
        s.blocks.fetch_add(1);
        const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
        s.lastUs.store(us); if (us > s.maxUs.load()) s.maxUs.store(us);
        if (us > frames / s.sampleRate * 1e6) s.deadlineMisses.fetch_add(1);
        return true;
    } catch (...) { s.failed.store(true); s.errors.fetch_add(1); silence(); return false; }
}
uint32_t Host::soundContextCapacity() const noexcept {return impl->sounds?impl->sounds->capacity(impl->plugin):0;}
std::vector<oscairn_sound_context_info_t> Host::soundContexts() const {
    std::vector<oscairn_sound_context_info_t> result;
    for(uint32_t i=0;i<soundContextCapacity();++i){oscairn_sound_context_info_t info{};if(impl->sounds->get(impl->plugin,i,&info)){info.name[63]=0;result.push_back(info);}}
    return result;
}
uint32_t Host::prepareSound(const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& patch,const std::string& library,const std::string& directory) {
    if(!impl->sounds)throw std::invalid_argument("Plugin has no native sound contexts");
    if(name.size()>63 || name.find('\0')!=std::string::npos || parameters.size()>128 || patch.size()>maxPatchBytes ||
       patch.find('\0')!=std::string::npos || patch.empty()!=library.empty() || (patch.empty() && !directory.empty()) ||
       library.size()>=sizeof(oscairn_asset_ref_t::library) || directory.size()>=sizeof(oscairn_asset_ref_t::path) ||
       library.find('\0')!=std::string::npos || directory.find('\0')!=std::string::npos)throw std::invalid_argument("Invalid sound patch");
    for(size_t i=0;i<parameters.size();++i){const auto& v=parameters[i];
        const auto p=std::find_if(impl->paramInfo.begin(),impl->paramInfo.end(),[&](const auto& p){return p.id==v.id;});
        if(p==impl->paramInfo.end() || (p->flags&CLAP_PARAM_IS_READONLY) || !std::isfinite(v.value) || v.value<p->minimum || v.value>p->maximum)
            throw std::invalid_argument("Invalid sound parameter");
        for(size_t j=0;j<i;++j)if(parameters[j].id==v.id)throw std::invalid_argument("Duplicate sound parameter");
    }
    char message[512]{};oscairn_asset_ref_t base{};
    std::memcpy(base.library,library.c_str(),library.size()+1);std::memcpy(base.path,directory.c_str(),directory.size()+1);
    const auto id=impl->sounds->prepare(impl->plugin,name.c_str(),parameters.data(),static_cast<uint32_t>(parameters.size()),
        patch.empty()?nullptr:patch.c_str(),patch.empty()?nullptr:&base,message,sizeof(message));
    message[sizeof(message)-1]=0;
    if(id==CLAP_INVALID_ID)throw std::runtime_error(message[0]?std::string("Sound preparation rejected: ")+message:"Sound preparation rejected: invalid patch or context capacity exhausted");
    return id;
}
uint32_t Host::prepareSound(const std::string& name,const Preset& preset) {
    if(!impl->sounds)throw std::invalid_argument("Plugin has no native sound contexts");
    if(!impl->sounds->prepare_preset)throw std::invalid_argument("Plugin cannot prepare sounds from presets");
    if(name.size()>63 || name.find('\0')!=std::string::npos || preset.kind>CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ||
       (preset.kind==CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)!=preset.location.empty() || preset.location.size()>maxPresetLocationBytes ||
       preset.loadKey.size()>maxPresetLocationBytes || preset.location.find('\0')!=std::string::npos || preset.loadKey.find('\0')!=std::string::npos)
        throw std::invalid_argument("Invalid sound preset");
    char message[512]{};
    const auto id=impl->sounds->prepare_preset(impl->plugin,name.c_str(),preset.kind,preset.location.empty()?nullptr:preset.location.c_str(),
        preset.loadKey.empty()?nullptr:preset.loadKey.c_str(),message,sizeof(message));
    message[sizeof(message)-1]=0;
    if(id==CLAP_INVALID_ID)throw std::runtime_error(std::string("Preset not prepared: ")+(message[0]?message:"rejected, or no free sound context"));
    return id;
}
bool Host::selectSound(uint32_t id,int16_t channel) {
    if(channel < -1 || channel>15)throw std::invalid_argument("Invalid sound channel");
    if(!impl->sounds)throw std::invalid_argument("Plugin has no native sound contexts");
    const bool accepted=impl->sounds->select(impl->plugin,id,channel);
    if(accepted)impl->wake.store(true);
    return accepted;
}
bool Host::discardSound(uint32_t id) {
    if(!impl->sounds)throw std::invalid_argument("Plugin has no native sound contexts");
    return impl->sounds->discard(impl->plugin,id);
}

} // namespace kick::clap

// SPDX-License-Identifier: AGPL-3.0-or-later
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/params.h>
#include <clap/ext/state.h>
#include <clap/ext/thread-check.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/latency.h>
#include <clap/ext/preset-load.h>
#include <clap/ext/gui.h>
#include <clap/ext/render.h>
#include <clap/factory/preset-discovery.h>
#include <oscairn-clap/ui.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/effect-contexts.h>
#include <oscairn-clap/assets.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

namespace {
std::atomic<bool> blockProcess{false}, enteredProcess{false}, releaseProcess{false};
std::atomic<bool> savingState{false}, releaseState{false};
std::atomic<int> threadErrors{0}, entryUsers{0}, processCalls{0}, callbackCalls{0};
// The effect's window (CLAP gui), floating only, so a host shows it without drawing anything:
// the instance whose window exists, whether it shows, and its suggested title. Main thread.
struct Plugin;
Plugin* guiOwner = nullptr;
bool guiShown = false;
std::string guiTitle;
// The memory fixture's engine bytes: one allocation per process (one engine per worker),
// kept alive by every instance of it, as an engine outliving its first context must.
std::shared_ptr<std::vector<char>> engineShared;
std::atomic<int> memoryInstances{0}; // live memory-fixture instances: an engine's prepared contexts
constexpr const char* ids[]{"org.kick.test.synth", "org.kick.test.effect", "org.kick.test.midi",
                            "org.kick.test.nan", "org.kick.test.error", "org.kick.test.sleep",
                            "org.kick.test.activate-failure", "org.kick.test.start-failure", "org.kick.test.stepped", "org.kick.test.no-state",
                            "org.kick.test.save-failure", "org.kick.test.transport", "org.kick.test.tail", "org.kick.test.memory"};
const char* instrument[]{CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, nullptr};
const char* effect[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, nullptr};
const clap_plugin_descriptor_t descriptors[]{
    {CLAP_VERSION, ids[0], "KICK fixture", "KICK", "", "", "", "1", "Test instrument", instrument},
    {CLAP_VERSION, ids[1], "KICK effect", "KICK", "", "", "", "1", "Test effect", effect},
    {CLAP_VERSION, ids[2], "KICK MIDI", "KICK", "", "", "", "1", "MIDI-only instrument", instrument},
    {CLAP_VERSION, ids[3], "KICK NaN", "KICK", "", "", "", "1", "Fault fixture", instrument},
    {CLAP_VERSION, ids[4], "KICK error", "KICK", "", "", "", "1", "Fault fixture", instrument},
    {CLAP_VERSION, ids[5], "KICK sleep", "KICK", "", "", "", "1", "Sleep fixture", instrument},
    {CLAP_VERSION, ids[6], "KICK activation failure", "KICK", "", "", "", "1", "Failure fixture", instrument},
    {CLAP_VERSION, ids[7], "KICK start failure", "KICK", "", "", "", "1", "Failure fixture", instrument},
    {CLAP_VERSION, ids[8], "KICK steps", "KICK", "", "", "", "1", "Stepped fixture", instrument},
    {CLAP_VERSION, ids[9], "KICK no state", "KICK", "", "", "", "1", "Unsupported state fixture", instrument},
    {CLAP_VERSION, ids[10], "KICK save failure", "KICK", "", "", "", "1", "Failed state fixture", instrument},
    {CLAP_VERSION, ids[11], "KICK transport", "KICK", "", "", "", "1", "Transport fixture", instrument},
    {CLAP_VERSION, ids[12], "KICK tail", "KICK", "", "", "", "1", "Release-tail instrument", instrument},
    {CLAP_VERSION, ids[13], "KICK memory", "KICK", "", "", "", "1", "Effect declaring its memory (oscairn.memory)", effect}};
struct Plugin {
    clap_plugin_t api{}, late{}; // late: the functions held back until init (KICK_FIXTURE_LATE_FUNCTIONS)
    const clap_host_t* host;
    const clap_host_thread_check_t* check;
    int kind;
    std::string asset; // main thread: a sound asset its state names ("library\0path"), resolved through oscairn.assets
    bool active = false, processing = false, gate = false, held = false, sustain = false;
    // Atomic: CLAP lets the main thread (state save, get_value) read these while
    // the audio thread writes them in process().
    std::atomic<double> gain{.5}, toggle{0}, mode{0};
    double shownMode = 0; // main thread: the mode the parameters' info last described
    clap_id timerId = CLAP_INVALID_ID;
    uint32_t maxFrames = 0, tailFrames=0;
    int down=0; // kind 12 in legato mode (KICK_FIXTURE_LEGATO): keys it believes are down
    clap_event_note_t tailNote{};
    std::shared_ptr<std::vector<char>> shared; std::vector<char> context; // the memory fixture's, allocated when it activates
    // The memory fixture declares 1 MiB shared and, per context, its gain (from its state) times
    // 16 MiB at 48 kHz, more at higher rates, as a reverb's buffers grow with the rate and mode.
    // KICK_FIXTURE_MEMORY_SHARED_MB makes its engine's shared bytes bigger as a sampler's samples.
    // Its effect contexts share them: one engine, one allocation, held while any context lives.
    // A gain above .9 (which its preset-load and the host's restart hook use) switches it to a
    // mode allocating 200 MiB more, as a reverb whose long mode restarts the plugin.
    static uint64_t sharedBytes(double gain) {
        const char* extra = std::getenv("KICK_FIXTURE_MEMORY_SHARED_MB");
        return (size_t(1) << 20) + (extra ? size_t(std::atoi(extra)) << 20 : 0) + (gain > .9 ? size_t(200) << 20 : 0);
    }
    static uint64_t contextBytes(double gain, double rate) { return uint64_t(gain * (16u << 20) * rate / 48000); }
    static Plugin& get(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }
    void main() const { if (!check || !check->is_main_thread(host)) ++threadErrors; }
    void audio() const { if (!check || !check->is_audio_thread(host)) ++threadErrors; }
    Plugin(const clap_host_t* h, int k) : host(h), check(nullptr), kind(k) {
        if (k == 13) ++memoryInstances;
        api.desc = &descriptors[k]; api.plugin_data = this;
        api.init = [](const clap_plugin_t* p) {
            auto& s = get(p);
            if (s.late.process) s.api = s.late;
            s.check = static_cast<const clap_host_thread_check_t*>(s.host->get_extension(s.host, CLAP_EXT_THREAD_CHECK));
            s.main(); s.host->request_callback(s.host);
            if (auto* timer = static_cast<const clap_host_timer_support_t*>(s.host->get_extension(s.host, CLAP_EXT_TIMER_SUPPORT)))
                timer->register_timer(s.host, 1, &s.timerId);
            return true;
        };
        api.destroy = [](const clap_plugin_t* p) { auto& s = get(p); s.main(); if (s.kind == 13) --memoryInstances; if (s.active || s.processing || guiOwner == &s) ++threadErrors; delete &s; };
        api.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t max) {
            auto& s = get(p); s.main(); if (s.kind == 6) return false;
            if (s.kind == 8 && s.shownMode != 0) ++threadErrors; // its info rescan must not restart it
            if (s.kind == 13) {
                // KICK_FIXTURE_MEMORY_EXTRA_MB: a wrong declaration, holding that much more than it declares.
                const char* extra = std::getenv("KICK_FIXTURE_MEMORY_EXTRA_MB");
                if (!engineShared) engineShared = std::make_shared<std::vector<char>>();
                const size_t bytes = sharedBytes(s.gain) + (extra ? size_t(std::atoi(extra)) << 20 : 0);
                // KICK_FIXTURE_MEMORY_SLOW_MS: the shared bytes allocated over that long, in 8 MiB steps, as a sampler loading.
                if (const char* slow = std::getenv("KICK_FIXTURE_MEMORY_SLOW_MS")) {
                    engineShared->clear(); engineShared->reserve(bytes);
                    const size_t step = size_t(8) << 20, steps = (bytes + step - 1) / step;
                    while (engineShared->size() < bytes) {
                        engineShared->insert(engineShared->end(), std::min(step, bytes - engineShared->size()), char(1));
                        std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(slow) / int(steps)));
                    }
                } else engineShared->assign(bytes, 1);
                s.shared = engineShared; s.context.assign(contextBytes(s.gain, rate), 1);
            }
            s.active = true; s.maxFrames = max; return true;
        };
        api.deactivate = [](const clap_plugin_t* p) { auto& s = get(p); s.main(); if (s.processing) ++threadErrors; s.active = false; s.shared.reset(); std::vector<char>().swap(s.context); };
        api.start_processing = [](const clap_plugin_t* p) { auto& s = get(p); s.audio(); if (!s.active) ++threadErrors; if (s.kind == 7) return false; s.processing = true; return true; };
        api.stop_processing = [](const clap_plugin_t* p) { auto& s = get(p); s.audio(); if (!s.processing) ++threadErrors; s.processing = false; };
        api.reset = [](const clap_plugin_t* p) { auto& s = get(p); s.audio(); s.gate = s.held = s.sustain = false;s.tailFrames=0;s.down=0; };
        api.process = process;
        api.get_extension = extension;
        api.on_main_thread = [](const clap_plugin_t* p) {
            auto& s = get(p); s.main(); ++callbackCalls;
            if (s.kind == 8 && s.shownMode != s.mode.load()) {
                s.shownMode = s.mode.load();
                if (auto* params = static_cast<const clap_host_params_t*>(s.host->get_extension(s.host, CLAP_EXT_PARAMS))) params->rescan(s.host, CLAP_PARAM_RESCAN_INFO);
            }
            if (auto* params = static_cast<const clap_host_params_t*>(s.host->get_extension(s.host, CLAP_EXT_PARAMS))) params->request_flush(s.host);
        };
        // As plugins built on clap-helpers do: only init and destroy until init fills in the rest.
        if (std::getenv("KICK_FIXTURE_LATE_FUNCTIONS")) {
            late = api;
            api.activate = nullptr; api.deactivate = nullptr; api.start_processing = nullptr; api.stop_processing = nullptr;
            api.reset = nullptr; api.process = nullptr; api.get_extension = nullptr; api.on_main_thread = nullptr;
        }
    }
    void event(const clap_event_header_t* e) {
        if (!e) { ++threadErrors; return; }
#ifdef FIXTURE_note_crash
        if (e->type == CLAP_EVENT_NOTE_ON && reinterpret_cast<const clap_event_note_t*>(e)->key == 0) std::abort();
#endif
        if (e->type == CLAP_EVENT_NOTE_ON) { if (kind == 2) ++threadErrors; gate = true; }
        // KICK_FIXTURE_STUCK: a voice that never ends, deaf to releases and chokes; only a reset stops it.
        if (e->type == CLAP_EVENT_NOTE_OFF || e->type == CLAP_EVENT_NOTE_CHOKE) { if (kind == 2) ++threadErrors; else if (!std::getenv("KICK_FIXTURE_STUCK")) gate = false; }
        if (e->type == CLAP_EVENT_MIDI) {
            const auto* m = reinterpret_cast<const clap_event_midi_t*>(e);
            const auto kind=m->data[0]&0xf0;
#ifdef FIXTURE_note_crash
            if (kind==0x90 && m->data[1]==0 && m->data[2]) std::abort();
#endif
            if (kind==0x90 && m->data[2]) held=gate=true;
            if (m->data[1] > 127 || m->data[2] > 127) ++threadErrors;
            if (kind==0x80 || (kind==0x90 && !m->data[2])) {held=false;if(!sustain)gate=false;}
            if (kind==0xb0 && m->data[1]==64) {sustain=m->data[2]>=64;if(!sustain && !held)gate=false;}
            if (kind==0xb0 && m->data[1]==120) gate=held=false;
            if (kind==0xb0 && m->data[1]==123) {held=false;if(!sustain)gate=false;}
        }
        if (e->type == CLAP_EVENT_PARAM_VALUE) {
            const auto* value = reinterpret_cast<const clap_event_param_value_t*>(e);
            if (value->param_id == 8) toggle = value->value;
            if (value->param_id == 9) { mode = value->value; if (kind == 8) host->request_callback(host); } // the toggle shows or hides with it
            if (value->param_id == 7) {
                gain = value->value; if (value->cookie != this) ++threadErrors;
                if (gain > .9) { host->request_restart(host); host->request_callback(host); }
            }
        }
    }
    static clap_process_status process(const clap_plugin_t* p, const clap_process_t* b) {
#ifdef FIXTURE_slow
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
#endif
#ifdef FIXTURE_process_crash
        std::abort();
#endif
#ifdef FIXTURE_process_hang
        std::this_thread::sleep_for(std::chrono::seconds(20));
#endif
#ifdef FIXTURE_leak
        { auto* memory = new char[size_t(1) << 20]; std::memset(memory, 1, size_t(1) << 20); static volatile char* keep; keep = memory; } // 1 MiB a block, never freed
#endif
        auto& s = get(p); s.audio(); ++processCalls;
        if (blockProcess.exchange(false)) { enteredProcess=true; while (!releaseProcess.load()) std::this_thread::yield(); }
        if (!s.active || !s.processing || !b->frames_count || b->frames_count > s.maxFrames) ++threadErrors;
        if (s.kind == 4) return CLAP_PROCESS_ERROR;
        if (s.kind == 11) s.gate = b->transport && (b->transport->flags & CLAP_TRANSPORT_IS_PLAYING);
        uint32_t event = 0;
        const auto count = b->in_events->size(b->in_events);
        for (uint32_t i = 0; i < b->frames_count; ++i) {
            while (event < count) {
                const auto* e = b->in_events->get(b->in_events, event);
                if (e->time < i || e->time >= b->frames_count) ++threadErrors;
                if (e->time > i) break;
                s.event(e);
                // Legato, as a mono synth slides one voice: a key down while another is held ends the new
                // note's ID at once, though its key stays down; the plugin sounds while it believes a key is.
                static const bool legato=std::getenv("KICK_FIXTURE_LEGATO")!=nullptr;
                if(s.kind==12 && legato && e->type==CLAP_EVENT_NOTE_ON && s.down++>0){auto end=*reinterpret_cast<const clap_event_note_t*>(e);end.header.type=CLAP_EVENT_NOTE_END;end.header.time=i;b->out_events->try_push(b->out_events,&end.header);}
                if(s.kind==12 && legato && (e->type==CLAP_EVENT_NOTE_OFF || e->type==CLAP_EVENT_NOTE_CHOKE) && s.down>0)--s.down;
                if(s.kind==12 && e->type==CLAP_EVENT_NOTE_ON && !(legato && s.down>1)){s.tailNote=*reinterpret_cast<const clap_event_note_t*>(e);s.tailFrames=0;}
                if(s.kind==12 && (e->type==CLAP_EVENT_NOTE_OFF || e->type==CLAP_EVENT_NOTE_CHOKE) && !(legato && s.down>0))s.tailFrames=e->type==CLAP_EVENT_NOTE_OFF?1024:1;
                ++event;
            }
            for (uint32_t c = 0; c < b->audio_outputs[0].channel_count; ++c) {
                float value = s.gate || s.down>0 ? static_cast<float>(s.gain) : static_cast<float>(s.gain)*s.tailFrames/1024.f;
                if (s.kind == 1) value = b->audio_inputs[0].data32[c][i] * static_cast<float>(s.gain);
                if (s.kind == 3) value = std::numeric_limits<float>::quiet_NaN();
                b->audio_outputs[0].data32[c][i] = value;
            }
            if(s.tailFrames && !--s.tailFrames){s.tailNote.header.type=CLAP_EVENT_NOTE_END;s.tailNote.header.time=i;b->out_events->try_push(b->out_events,&s.tailNote.header);}
        }
        clap_event_param_value_t value{};
        value.header = {sizeof(value), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        value.param_id = 7; value.value = s.gain;
        b->out_events->try_push(b->out_events, &value.header);
        return !s.gate && (s.kind == 5 || s.kind == 11) ? CLAP_PROCESS_SLEEP : CLAP_PROCESS_CONTINUE;
    }
    static const void* extension(const clap_plugin_t* plugin, const char* id) {
        static const clap_plugin_audio_ports_t audio{
            [](const clap_plugin_t* p, bool input) -> uint32_t { get(p).main(); return input ? (get(p).kind == 1 || get(p).kind == 13 ? 1 : 0) : 1; },
            [](const clap_plugin_t* p, uint32_t i, bool input, clap_audio_port_info_t* out) {
                auto& s = get(p); s.main(); if (i || (input && s.kind != 1 && s.kind != 13)) return false;
                *out = {}; out->id = 0; std::strcpy(out->name, "Main"); out->flags = CLAP_AUDIO_PORT_IS_MAIN;
                out->channel_count = s.kind == 0 ? 1 : 2; out->port_type = s.kind == 0 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
                out->in_place_pair = CLAP_INVALID_ID; return true;
            }};
        static const clap_plugin_note_ports_t notes{
            [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 1 : 0; },
            [](const clap_plugin_t* p, uint32_t i, bool input, clap_note_port_info_t* out) {
                if (i || !input) return false;
                *out = {}; out->id = 0;
                out->supported_dialects = get(p).kind == 2 ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
                out->preferred_dialect = get(p).kind == 2 ? CLAP_NOTE_DIALECT_MIDI : CLAP_NOTE_DIALECT_CLAP;
                return true;
            }};
        static const clap_plugin_params_t params{
            [](const clap_plugin_t* p) -> uint32_t { return get(p).kind == 8 ? 4 : 2; },
            [](const clap_plugin_t* p, uint32_t index, clap_param_info_t* out) {
                get(p).main();
                if (get(p).kind == 8 && (index == 2 || index == 3)) {
                    *out = {}; out->id = index == 2 ? 8 : 9; out->cookie = &get(p);
                    std::strcpy(out->name, index == 2 ? "Toggle" : "Mode");
                    out->flags = CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_AUTOMATABLE;
                    if (index == 2 && get(p).shownMode >= 2) out->flags |= CLAP_PARAM_IS_HIDDEN; // modes 2 and up have no toggle
                    if (index == 3) out->flags |= CLAP_PARAM_IS_ENUM;
                    out->min_value = 0; out->max_value = index == 2 ? 1 : 4; return true;
                }
                if (index > 1) return false;
                *out = {}; out->id = index ? 99 : 7; out->cookie = &get(p);
                std::strcpy(out->name, index ? "Read only" : "Gain");
                out->min_value = 0; out->max_value = 1; out->default_value = .5;
                if (std::getenv("KICK_FIXTURE_BAD_DEFAULT")) out->default_value = index ? 1e34 : std::numeric_limits<double>::quiet_NaN();
                out->flags = index ? CLAP_PARAM_IS_READONLY : CLAP_PARAM_IS_AUTOMATABLE;
                return true;
            },
            [](const clap_plugin_t* p, clap_id id, double* value) { get(p).main(); if (id == 8) *value = get(p).toggle; else if (id == 9) *value = get(p).mode; else if (id == 7 || id == 99) *value = get(p).gain; else return false; return true; },
            [](const clap_plugin_t*, clap_id, double value, char* out, uint32_t size) { std::snprintf(out, size, "%.2f", value); return true; },
            [](const clap_plugin_t*, clap_id, const char* text, double* value) { *value = std::atof(text); return true; },
            [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
                get(p).audio(); for (uint32_t i = 0; i < in->size(in); ++i) get(p).event(in->get(in, i));
            }};
        static const clap_plugin_state_t state{
            [](const clap_plugin_t* p, const clap_ostream_t* out) {
                get(p).main();
#ifdef FIXTURE_held_state
                savingState.store(true);
                while (!releaseState.load()) std::this_thread::yield();
                savingState.store(false);
#endif
                // Exercise short writes and a genuinely opaque payload.
                const double value = get(p).gain;
                const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
                for (size_t i = 0; i < sizeof(value); ++i) if (out->write(out, bytes + i, 1) != 1) return false;
                const auto& asset = get(p).asset;
                if (!asset.empty() && out->write(out, asset.data(), asset.size()) != int64_t(asset.size())) return false;
                return !(get(p).kind == 10 && value > .65);
            },
            [](const clap_plugin_t* p, const clap_istream_t* in) {
                auto& s = get(p); s.main(); if (s.active) ++threadErrors;
                // One slow load, for hosts' reply timeouts: when the file named here exists, it is removed and the load takes 6 s.
                if (const char* once = std::getenv("KICK_FIXTURE_SLOW_LOAD_ONCE")) if (std::remove(once) == 0) std::this_thread::sleep_for(std::chrono::seconds(6));
                double value = 0;
                if (in->read(in, &value, sizeof(value)) != sizeof(value) || !std::isfinite(value)) return false;
                // After the gain, optionally a sound asset ("library\0path"), which must resolve, as a
                // sampler's state with a library reference must (RFC 0009).
                std::string asset; char chunk[256]; int64_t got;
                while ((got = in->read(in, chunk, sizeof chunk)) > 0) asset.append(chunk, size_t(got));
                if (!asset.empty()) {
                    const auto zero = asset.find('\0');
                    const auto* assets = static_cast<const oscairn_host_assets_t*>(s.host->get_extension(s.host, OSCAIRN_EXT_ASSETS));
                    if (zero == std::string::npos || !assets) return false;
                    oscairn_asset_ref_t ref{}; char path[2048], message[256];
                    std::snprintf(ref.library, sizeof ref.library, "%s", asset.substr(0, zero).c_str());
                    std::snprintf(ref.path, sizeof ref.path, "%s", asset.substr(zero + 1).c_str());
                    if (!assets->resolve(s.host, &ref, path, sizeof path, message, sizeof message)) return false;
                }
                s.gain = value; s.asset = asset; return true;
            }};
        static const clap_plugin_latency_t latency{[](const clap_plugin_t*) -> uint32_t { return 13; }};
        static const clap_plugin_timer_support_t timers{[](const clap_plugin_t* p, clap_id id) {
            auto& s = get(p); s.main();
            auto* hostTimers = static_cast<const clap_host_timer_support_t*>(s.host->get_extension(s.host, CLAP_EXT_TIMER_SUPPORT));
            if (!hostTimers->unregister_timer(s.host, id)) ++threadErrors;
            ++callbackCalls;
        }};
        if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audio;
        if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notes;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        // The stepped fixture's editor hints, some that fit their parameters and some that do
        // not, which the host must replace with "the host decides".
        static const oscairn_plugin_ui_t ui{[](const clap_plugin_t*, clap_id id, oscairn_param_hint_t* hint) {
            switch (id) {
            case 7: *hint = {OSCAIRN_WIDGET_RANGE, 1, 99, 0, OSCAIRN_ROLE_CUTOFF}; return true; // a range up to "Read only": fits; the cutoff
            case 8: *hint = {OSCAIRN_WIDGET_SWITCH, 2, CLAP_INVALID_ID, 5, 0}; return true; // two values: fits; grouped
            case 9: *hint = {OSCAIRN_WIDGET_SWITCH, 3, CLAP_INVALID_ID, 5, 0}; return true; // five values: no switch
            case 99: *hint = {42, 0, CLAP_INVALID_ID, 0, 4242}; return true;       // no such widget or role
            default: return false;
            }
        }};
        if (!std::strcmp(id, OSCAIRN_EXT_UI)) return get(plugin).kind == 8 ? &ui : nullptr;
        if (!std::strcmp(id, CLAP_EXT_STATE)) return get(plugin).kind == 9 ? nullptr : &state;
        if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &latency;
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timers;
        // Presets: "quiet" and "bright" inside the plugin; a preset file holds its gain as text.
        static const clap_plugin_preset_load_t presets{[](const clap_plugin_t* p, uint32_t kind, const char* location, const char* key) {
            auto& s = get(p); s.main();
            double value = -1;
            if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && key) value = !std::strcmp(key, "quiet") ? .25 : !std::strcmp(key, "bright") ? .75 : -1;
            else if (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && location) { std::ifstream file(location); file >> value; }
            auto* host = static_cast<const clap_host_preset_load_t*>(s.host->get_extension(s.host, CLAP_EXT_PRESET_LOAD));
            if (!(value >= 0 && value <= 1)) { if (host) host->on_error(s.host, kind, location, key, 0, "no such preset"); return false; }
            s.gain = value;
            if (host) host->loaded(s.host, kind, location, key);
            return true;
        }};
        if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD)) return &presets;
        // clap.render: each mode the host sets is appended, as a line, to the file
        // KICK_FIXTURE_RENDER_LOG names; KICK_FIXTURE_HARD_REALTIME: it must run in real time.
        static const clap_plugin_render_t render{
            [](const clap_plugin_t* p) { get(p).main(); return std::getenv("KICK_FIXTURE_HARD_REALTIME") != nullptr; },
            [](const clap_plugin_t* p, clap_plugin_render_mode mode) {
                auto& s = get(p); s.main(); if (s.processing) ++threadErrors;
                if (const char* log = std::getenv("KICK_FIXTURE_RENDER_LOG"))
                    std::ofstream(log, std::ios::app) << (mode == CLAP_RENDER_OFFLINE ? "offline" : "realtime") << '\n';
                return true;
            }};
        if (!std::strcmp(id, CLAP_EXT_RENDER)) return &render;
        static const oscairn_plugin_memory_t memory{
            [](const clap_plugin_t* p, double rate, uint32_t, oscairn_memory_t* out) {
                auto& s = get(p); s.main(); if (s.active) ++threadErrors;
                *out = {sharedBytes(s.gain), contextBytes(s.gain, rate), 0}; return true;
            },
            [](const clap_plugin_t* p, oscairn_memory_t* out) {
                auto& s = get(p); s.main(); if (!s.active) return false;
                *out = {s.shared ? s.shared->size() : 0, s.context.size(), 0}; return true;
            }};
        if (!std::strcmp(id, OSCAIRN_EXT_MEMORY)) return get(plugin).kind == 13 ? &memory : nullptr;
        // With KICK_FIXTURE_MEMORY_CONTEXTS, the memory fixture is also an engine: its effect
        // contexts share the engine's bytes and keep them alive after the originating instance is
        // destroyed (oscairn.effect-contexts). Gated: a host's effect-context accounting differs.
        static const oscairn_plugin_effect_contexts_t contexts{
            [](const clap_plugin_t* p, const clap_host_t* host) -> const clap_plugin_t* {
                auto& parent = get(p); if (parent.kind != 13) return nullptr;
                return &(new Plugin(host, 13))->api;
            },
            [](const clap_plugin_t* p, oscairn_effect_resources_t* out) {
                auto& s = get(p); s.main();
                *out = {sharedBytes(s.gain), s.context.size(), 4, unsigned(memoryInstances.load()), s.active}; return true;
            }};
        if (!std::strcmp(id, OSCAIRN_EXT_EFFECT_CONTEXTS))
            return get(plugin).kind == 13 && std::getenv("KICK_FIXTURE_MEMORY_CONTEXTS") ? &contexts : nullptr;
        static const clap_plugin_gui_t gui{
            [](const clap_plugin_t*, const char*, bool floating) { return floating; },
            [](const clap_plugin_t*, const char**, bool*) { return false; },
            [](const clap_plugin_t* p, const char*, bool floating) { auto& s = get(p); s.main(); if (!floating || guiOwner) return false; guiOwner = &s; return true; },
            [](const clap_plugin_t* p) { auto& s = get(p); s.main(); if (guiOwner != &s) ++threadErrors; guiOwner = nullptr; guiShown = false; },
            [](const clap_plugin_t*, double) { return false; },
            [](const clap_plugin_t*, uint32_t*, uint32_t*) { return false; },
            [](const clap_plugin_t*) { return false; },
            [](const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; },
            [](const clap_plugin_t*, uint32_t*, uint32_t*) { return false; },
            [](const clap_plugin_t*, uint32_t, uint32_t) { return false; },
            [](const clap_plugin_t*, const clap_window_t*) { return false; },
            [](const clap_plugin_t*, const clap_window_t*) { return false; },
            [](const clap_plugin_t* p, const char* title) { get(p).main(); guiTitle = title ? title : ""; },
            [](const clap_plugin_t* p) { auto& s = get(p); s.main(); if (guiOwner != &s) return false; guiShown = true; return true; },
            [](const clap_plugin_t* p) { auto& s = get(p); s.main(); if (guiOwner != &s) return false; guiShown = false; return true; }};
        if (!std::strcmp(id, CLAP_EXT_GUI)) return get(plugin).kind == 1 ? &gui : nullptr;
        return nullptr;
    }
};
// Preset discovery: two presets inside the plugin for the MIDI fixture (and one for another
// plugin, which a host must not list for it), and *.kickpreset files in KICK_FIXTURE_PRESETS.
// With KICK_FIXTURE_PRESETS_HANG set, reading them hangs; KICK_FIXTURE_PRESETS_LOG names a file it notes each reading in.
const clap_preset_discovery_provider_descriptor_t presetProvider{CLAP_VERSION, "org.kick.test.presets", "Fixture presets", "Kick"};
const clap_preset_discovery_provider_t* createPresetProvider(const clap_preset_discovery_indexer_t* indexer)
{
    static clap_preset_discovery_provider_t provider{};
    provider.desc = &presetProvider;
    provider.provider_data = const_cast<clap_preset_discovery_indexer_t*>(indexer);
    provider.init = [](const clap_preset_discovery_provider_t* p) {
        auto* index = static_cast<const clap_preset_discovery_indexer_t*>(p->provider_data);
        const clap_preset_discovery_filetype_t type{"Fixture preset", nullptr, "kickpreset"};
        if (!index->declare_filetype(index, &type)) return false;
        const clap_preset_discovery_location_t factory{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory", CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
        if (!index->declare_location(index, &factory)) return false;
        if (const char* folder = std::getenv("KICK_FIXTURE_PRESETS")) {
            const clap_preset_discovery_location_t user{CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "User", CLAP_PRESET_DISCOVERY_LOCATION_FILE, folder};
            if (!index->declare_location(index, &user)) return false;
        }
        // The asset library org.kick.test.factory 1.0 or later, when installed: its folder, found
        // through oscairn.assets' library_root on the indexer, as a fork finds its factory patches.
        const auto* assets = index->get_extension ? static_cast<const oscairn_host_assets_t*>(index->get_extension(index, OSCAIRN_EXT_ASSETS)) : nullptr;
        char root[4096];
        if (assets && assets->library_root && assets->library_root(index, "org.kick.test.factory", "1.0", root, sizeof root)) {
            const clap_preset_discovery_location_t library{CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT, "Factory library", CLAP_PRESET_DISCOVERY_LOCATION_FILE, root};
            if (!index->declare_location(index, &library)) return false;
        }
        return true;
    };
    provider.destroy = [](const clap_preset_discovery_provider_t*) {};
    provider.get_metadata = [](const clap_preset_discovery_provider_t*, uint32_t kind, const char* location, const clap_preset_discovery_metadata_receiver_t* r) {
        const clap_universal_plugin_id_t midi{"clap", "org.kick.test.midi"}, synth{"clap", "org.kick.test.synth"};
        if (const char* log = std::getenv("KICK_FIXTURE_PRESETS_LOG")) std::ofstream(log, std::ios::app) << "asked\n"; // a line per reading
        if (std::getenv("KICK_FIXTURE_PRESETS_HANG")) std::this_thread::sleep_for(std::chrono::seconds(20));
        if (kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) {
            if (!r->begin_preset(r, "Quiet", "quiet")) return true;
            r->add_plugin_id(r, &midi);
            if (!r->begin_preset(r, "Bright", "bright")) return true;
            r->add_plugin_id(r, &midi);
            if (!r->begin_preset(r, "Synth only", "synth")) return true;
            r->add_plugin_id(r, &synth);
            return true;
        }
        if (!location) return false;
        const auto name = std::filesystem::path(location).stem().string();
        if (r->begin_preset(r, name.c_str(), nullptr)) r->add_plugin_id(r, &midi);
        return true;
    };
    provider.get_extension = [](const clap_preset_discovery_provider_t*, const char*) -> const void* { return nullptr; };
    return &provider;
}
const clap_preset_discovery_factory_t presetFactory{
    [](const clap_preset_discovery_factory_t*) -> uint32_t { return 1; },
    [](const clap_preset_discovery_factory_t*, uint32_t index) -> const clap_preset_discovery_provider_descriptor_t* { return index ? nullptr : &presetProvider; },
    [](const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer, const char* id) -> const clap_preset_discovery_provider_t* {
        return std::strcmp(id, presetProvider.id) ? nullptr : createPresetProvider(indexer);
    }};
const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*) -> uint32_t { return 14; },
    [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* { return index < 14 ? &descriptors[index] : nullptr; },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
        for (int i = 0; i < 14; ++i) if (!std::strcmp(id, ids[i])) return &(new Plugin(host, i))->api;
        return nullptr;
    }};
}
extern "C" {
CLAP_EXPORT void fixture_block_process() { enteredProcess=false; releaseProcess=false; blockProcess=true; }
CLAP_EXPORT bool fixture_entered_process() { return enteredProcess.load(); }
CLAP_EXPORT void fixture_release_process() { releaseProcess=true; }
CLAP_EXPORT bool fixture_saving_state() { return savingState.load(); }
CLAP_EXPORT void fixture_release_state() { releaseState.store(true); }
CLAP_EXPORT int fixture_thread_errors() { return threadErrors.load(); }
CLAP_EXPORT int fixture_entry_users() { return entryUsers.load(); }
CLAP_EXPORT int fixture_process_calls() { return processCalls.load(); }
CLAP_EXPORT int fixture_callback_calls() { return callbackCalls.load(); }
// The effect's window: 0 none, 1 hidden, 2 shown; its title. Main thread.
CLAP_EXPORT int fixture_gui_state() { return guiOwner ? 1 + guiShown : 0; }
CLAP_EXPORT const char* fixture_gui_title() { return guiTitle.c_str(); }
// The effect asking its host, as a plugin's window does: 0 hide, 1 show, 2 closed and destroyed, 3 resize.
CLAP_EXPORT bool fixture_gui_request(int what) {
    if (!guiOwner) return false;
    const auto* host = guiOwner->host;
    const auto* gui = static_cast<const clap_host_gui_t*>(host->get_extension(host, CLAP_EXT_GUI));
    if (!gui) return false;
    switch (what) {
    case 0: return gui->request_hide(host);
    case 1: return gui->request_show(host);
    case 2: guiShown = false; gui->closed(host, true); return true;
    default: return gui->request_resize(host, 300, 200);
    }
}
CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION,
    [](const char*) {
#ifdef FIXTURE_crash
        std::fputs("fixture: crashing during scan\n", stderr);
        std::abort();
#elif defined(FIXTURE_hang)
        std::this_thread::sleep_for(std::chrono::seconds(20));
#elif defined(FIXTURE_hog)
        { auto* memory = new char[size_t(2) << 30]; memory[0] = 1; static volatile char* keep; keep = memory; } // 2 GiB while scanned
#endif
        ++entryUsers; return true;
    },
    [] { --entryUsers; },
    [](const char* id) -> const void* {
        if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID)) return &factory;
        if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID)) return &presetFactory;
        return nullptr;
    }};
}

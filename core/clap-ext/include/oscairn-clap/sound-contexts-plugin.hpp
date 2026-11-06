// SPDX-License-Identifier: MIT
//
// Part of the Oscairn plugin SDK (MIT, like the extension headers beside it; relicensed from
// AGPL-3.0-or-later on 2026-10-31 so plugins of any license can use it).
//
// Native sound-context ownership shared by Oscairn instruments: preparation slots,
// the bounded selection queue, voice ownership and release rules, NOTE_END, and the
// exclusive context snapshot. The instrument (Derived) supplies its patch type, voice
// DSP, parameters and patch serialization. See docs/rfcs/0005-sound-contexts.md.
//
// Derived provides:
//   bool accepts(const Patch&, const clap_event_note_t&)   whether a note starts a voice
//   void start(Voice&, Context&)                            initialize voice DSP
//   void release(Voice&)                                    enter the release stage
//   void render(const clap_process_t*, uint32_t from, uint32_t to)
//   void applyParam(const clap_event_header_t*)
//   void selectedGlobally(Context&, const clap_output_events_t*)   after an all-channel selection
//   static uint32_t prepare(const clap_plugin_t*, name, parameters, count, patch, directory, message, capacity)
// and builds its clap_plugin_state_t / oscairn_plugin_sound_contexts_t tables from the statics below.
//   static constexpr const char* stateVersion; static constexpr size_t maxStateBytes
//   bool writePatch(std::ostream&, const Patch&); bool readPatch(std::istream&, Patch&)
//   void restored()                                         after a snapshot load
// To prepare contexts from its CLAP presets (draft 5), it provides
//   bool readPreset(uint32_t kind, const char* location, const char* loadKey, Patch&, std::string& error)
// which reads the preset its preset discovery listed (preset-discovery-plugin.hpp) into a patch
// that starts as Patch{}, and sets the table's prepare_preset to Engine::preparePreset.
// To prepare a snapshot's sounds beside the playing ones (oscairn.sound-snapshots), it builds an
// oscairn_plugin_sound_snapshots_t table {Engine::prepareSnapshot, Engine::setRingOut,
// Engine::chokeRetiring}; readPatch then also runs on the main thread while rendering, into a patch
// not yet published. With setRingOut, render adds the main output of each context into the two
// ring-out channels retiring(slot) returns (when not null) instead of its main port, so the host can
// ring it out through the outgoing Performance's chain. Only one change's tail carries over: at a
// second change while one rings, chokeRetiring fades the older tail out over the choke time, through
// ringOut[2..3] (still its own chain), and the newer tail takes ringOut[0..1]. Only the main port
// rings out (other ports follow the incoming chain), and a note held over 30 s after the change goes
// back to the main output (RFC 0005, the limits).
// It may provide buildPreset(Context&, std::string& error), called with a preset's or a snapshot's
// patch in place before the context is published (to build per-sound state; false refuses it),
// snapshotLoading(bool), called around reading a snapshot's patches, and
// voiceFinished(Voice&), called whenever a voice ends: released out, choked, stolen or reset.
// A snapshot load that succeeds resets every context's pins (users) and ends every voice: in
// restored(), forget any pins the instrument holds itself, such as a ringing tail's. A refused
// load changes nothing and doesn't call restored(), so do not forget them in snapshotLoading(true).
//
// MIDI pitch bend and the mod wheel (CC 1) are kept per channel; each voice follows its
// channel's values smoothly in VoiceBase::bend and modWheel once render calls followControls(v)
// every sample. The plugin chooses what they do (its bend range, the wheel's target).

#pragma once
#include <clap/clap.h>
#include <clap/factory/preset-discovery.h>
#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>

namespace oscairn::contexts
{
    struct VoiceBase
    {
        bool     active = false, held = false;
        uint32_t context = 0;
        int32_t  noteId = -1;
        int16_t  key = 0, channel = 0;
        double   velocity = 0.0;
        double   pressure = 0.0; // 0..1, from note expressions
        double   volume = 1.0;   // CLAP's per-note volume expression, linear 0..4
        double   bend = 0.0;     // the channel's pitch bend, -1..1 of the plugin's range, smoothed
        double   modWheel = 0.0; // the channel's mod wheel (CC 1), 0..1, smoothed
        uint32_t chokeLeft = 0, chokeLength = 0; // a choked voice fades out over chokeLength frames
        uint64_t started = 0;
    };

    template<class Derived, class Voice, class Patch, uint32_t ContextCapacity = 8, size_t VoiceCount = 16>
    struct Engine
    {
        static constexpr uint32_t contextCapacity = ContextCapacity, selectionCapacity = 32;
        struct Context
        {
            // Main-thread metadata; audio uses only pinned slot indices.
            bool prepared = false;
            uint32_t id = 0;
            char name[64]{};
            Patch patch{};
            // users includes selected channels, queued selections and active voices.
            std::atomic<uint32_t> users{0}, selected{0}, voices{0};
        };

        clap_plugin_t plugin{};
        const clap_output_events_t* output = nullptr;
        float* const* ringOut = nullptr; // audio thread: the ring-out buffer's four channels for this process(), or null
        uint32_t eventTime = 0;
        std::array<Voice, VoiceCount> voices{};
        uint64_t notesStarted = 0;
        std::array<Context, ContextCapacity> contexts;
        std::array<uint32_t, 16> selected{}; // audio thread
        std::array<bool, 16> sustainDown{};
        // Per channel. MPE-style per-note bend (CLAP_NOTE_EXPRESSION_TUNING) is not applied.
        std::array<double, 16> channelBend{}, channelModWheel{};
        struct Selection { uint32_t slot; int16_t channel; };
        std::array<Selection, selectionCapacity> selections{}; // channel chokeMark: chokeRetiring's request
        static constexpr int16_t chokeMark = -2;
        std::array<bool, ContextCapacity> fading{}; // audio thread: choked by chokeRetiring, rendering into ringOut[2..3] until its voices end
        std::atomic<uint32_t> selectionHead{0}, selectionTail{0};
        uint32_t nextContext = 1; // main thread, never reuse IDs
        uint32_t chokeFrames = 144; // 3 ms at 48 kHz; setSampleRate keeps it 3 ms
        double controlGlide = 1.0 - std::exp(-1.0 / 240.0); // per sample: a 5 ms time constant at 48 kHz

        // From activate: a choke fades a voice out over 3 ms instead of cutting it (RFC 0018).
        void setSampleRate(double rate)
        {
            chokeFrames = rate > 0 ? std::max<uint32_t>(1, uint32_t(rate * .003 + .5)) : 144;
            controlGlide = rate > 0 ? 1.0 - std::exp(-1.0 / (rate * .005)) : 1.0 - std::exp(-1.0 / 240.0);
        }
        // Once per sample per voice: moves its bend and mod wheel toward its channel's, so a
        // wheel's 7-bit steps do not zipper. Held and released voices alike.
        void followControls(Voice& v) const
        {
            follow(v.bend, channelBend[v.channel]); follow(v.modWheel, channelModWheel[v.channel]);
        }
        void follow(double& value, double target) const
        { value = std::abs(target - value) < 1e-9 ? target : value + (target - value) * controlGlide; } // arrives, never denormal
        // A voice's gain at `offset` frames into the segment being rendered: its volume expression,
        // and its choke fade. Multiply each of its samples by it; a voice that ignores it still
        // ends when its fade would have, but with a cut.
        static double amplitude(const Voice& v, uint32_t offset)
        {
            if(!v.chokeLength) return v.volume;
            return v.chokeLeft > offset ? v.volume * double(v.chokeLeft - offset) / double(v.chokeLength) : 0.0;
        }

        Engine()
        {
            contexts[0].prepared = true; contexts[0].users = 16; contexts[0].selected = 65535;
            std::strcpy(contexts[0].name, "Default");
        }
        Derived& derived() { return static_cast<Derived&>(*this); }
        static Derived& self(const clap_plugin_t* p) { return *static_cast<Derived*>(p->plugin_data); }

        void endNote(int32_t noteId, int16_t channel, int16_t key)
        {
            if(!output) return;
            clap_event_note_t end{}; end.header = {sizeof(end), eventTime, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_END, 0};
            end.note_id = noteId; end.port_index = 0; end.channel = channel; end.key = key;
            output->try_push(output, &end.header);
        }
        void finishVoice(Voice& v)
        {
            if(!v.active) return;
            derived().voiceFinished(v);
            endNote(v.noteId, v.channel, v.key);
            v.active = false;
            auto& c = contexts[v.context];
            if(c.voices.fetch_sub(1, std::memory_order_relaxed) == 1) fading[v.context] = false;
            // Last access to this context before releasing its lifetime pin.
            c.users.fetch_sub(1, std::memory_order_release);
        }
        void resetVoices()
        {
            for(auto& v : voices) finishVoice(v);
            sustainDown.fill(false); channelBend.fill(0); channelModWheel.fill(0);
        }
        void noteOn(const clap_event_note_t& note)
        {
            if(note.port_index != 0 || note.channel < 0 || note.channel > 15 || note.key < 0 || note.key > 127) return;
            const auto slot = selected[note.channel];
            auto& c = contexts[slot];
            // A note that starts no voice still ends, so the host can release its route.
            if(!derived().accepts(c.patch, note)) { endNote(note.note_id, note.channel, note.key); return; }
            Voice* voice = nullptr;
            for(auto& v : voices) if(!v.active) { voice = &v; break; }
            if(!voice) { voice = &voices[0]; for(auto& v : voices) if(v.started < voice->started) voice = &v; finishVoice(*voice); }
            *voice = Voice{};
            voice->active = voice->held = true; voice->key = note.key; voice->channel = note.channel; voice->noteId = note.note_id;
            voice->context = slot; voice->velocity = note.velocity; voice->started = ++notesStarted;
            voice->bend = channelBend[note.channel]; voice->modWheel = channelModWheel[note.channel]; // no glide at the start
            c.users.fetch_add(1, std::memory_order_relaxed); c.voices.fetch_add(1, std::memory_order_relaxed);
            derived().start(*voice, c);
        }
        static bool matches(const Voice& v, int32_t id, int16_t key, int16_t channel)
        { return v.active && (id < 0 || v.noteId == id) && (key < 0 || v.key == key) && (channel < 0 || v.channel == channel); }
        void noteOff(const clap_event_note_t& note, bool choke = false, bool midi = false)
        {
            if(note.port_index > 0) return;
            // Raw MIDI has no note IDs; only that dialect uses oldest-held-first release.
            Voice* oldest = nullptr;
            const bool fifo = midi && !choke && note.note_id < 0 && note.key >= 0 && note.channel >= 0;
            for(auto& v : voices) if(matches(v, note.note_id, note.key, note.channel) && (choke || v.held))
            {
                if(fifo) { if(!oldest || v.started < oldest->started) oldest = &v; continue; }
                v.held = false;
                if(choke) { if(!v.chokeLength) v.chokeLength = v.chokeLeft = chokeFrames; } // fades, then ends
                else if(note.key < 0 || !sustainDown[v.channel]) derived().release(v);
            }
            if(oldest) { oldest->held = false; if(!sustainDown[oldest->channel]) derived().release(*oldest); }
            // An emergency wildcard release must also release already pedal-held voices.
            if(note.key < 0) for(auto& v : voices) if(matches(v, note.note_id, note.key, note.channel)) derived().release(v);
        }
        void applyExpression(const clap_event_note_expression_t& e)
        {
            if(e.port_index > 0) return;
            if(e.expression_id == CLAP_NOTE_EXPRESSION_PRESSURE)
                for(auto& v : voices) if(matches(v, e.note_id, e.key, e.channel)) v.pressure = e.value < 0 ? 0 : e.value > 1 ? 1 : e.value;
            if(e.expression_id == CLAP_NOTE_EXPRESSION_VOLUME)
                for(auto& v : voices) if(matches(v, e.note_id, e.key, e.channel)) v.volume = e.value < 0 ? 0 : e.value > 4 ? 4 : e.value;
        }
        // Renders a segment, then ends the choked voices whose fade it finished.
        void renderSegment(const clap_process_t* process, uint32_t from, uint32_t to)
        {
            derived().render(process, from, to);
            const uint32_t frames = to - from;
            for(auto& v : voices)
                if(v.active && v.chokeLength)
                {
                    v.chokeLeft = v.chokeLeft > frames ? v.chokeLeft - frames : 0;
                    if(!v.chokeLeft) { const auto at = eventTime; eventTime = to ? to - 1 : 0; finishVoice(v); eventTime = at; }
                }
        }
        void applyMidi(const clap_event_midi_t& e)
        {
            if(e.port_index) return;
            const auto channel = e.data[0] & 15, kind = e.data[0] & 0xf0;
            if(kind == 0x90 || kind == 0x80)
            {
                clap_event_note_t n{}; n.note_id = -1; n.channel = channel; n.key = e.data[1]; n.velocity = e.data[2] / 127.0;
                if(kind == 0x90 && e.data[2]) noteOn(n); else noteOff(n, false, true);
                return;
            }
            if(kind == 0xa0 || kind == 0xd0)
            {
                clap_event_note_expression_t pressure{}; pressure.expression_id = CLAP_NOTE_EXPRESSION_PRESSURE;
                pressure.note_id = -1; pressure.channel = channel; pressure.key = kind == 0xa0 ? e.data[1] : -1;
                pressure.value = (kind == 0xa0 ? e.data[2] : e.data[1]) / 127.0; applyExpression(pressure); return;
            }
            if(kind == 0xe0)
            {
                // 14 bits, center 8192; full up is exactly +1.
                const int value = ((e.data[2] & 127) << 7 | (e.data[1] & 127)) - 8192;
                channelBend[channel] = value / (value > 0 ? 8191.0 : 8192.0); return;
            }
            if(kind != 0xb0) return;
            if(e.data[1] == 1) channelModWheel[channel] = (e.data[2] & 127) / 127.0;
            else if(e.data[1] == 64)
            {
                sustainDown[channel] = e.data[2] >= 64;
                if(!sustainDown[channel]) for(auto& v : voices) if(v.active && v.channel == channel && !v.held) derived().release(v);
            }
            else if(e.data[1] == 120 || e.data[1] == 123)
            {
                clap_event_note_t n{}; n.note_id = -1; n.key = -1; n.channel = channel;
                noteOff(n, e.data[1] == 120);
            }
        }
        void applySelections(const clap_output_events_t* out)
        {
            auto tail = selectionTail.load(std::memory_order_relaxed);
            const auto head = selectionHead.load(std::memory_order_acquire);
            while(tail != head)
            {
                const auto selection = selections[tail % selectionCapacity]; auto& next = contexts[selection.slot];
                if(selection.channel == chokeMark)
                {
                    // Every retiring context, before the selections queued after the request: its voices fade out.
                    for(auto& v : voices) if(v.active && !contexts[v.context].selected.load(std::memory_order_relaxed))
                    { fading[v.context] = true; v.held = false; if(!v.chokeLength) v.chokeLength = v.chokeLeft = chokeFrames; }
                    ++tail; continue;
                }
                for(int ch = 0; ch < 16; ++ch) if(selection.channel < 0 || selection.channel == ch)
                {
                    const auto old = selected[ch]; if(old == selection.slot) continue;
                    next.users.fetch_add(1, std::memory_order_relaxed); next.selected.fetch_or(1u << ch, std::memory_order_relaxed);
                    selected[ch] = selection.slot; contexts[old].selected.fetch_and(~(1u << ch), std::memory_order_relaxed);
                    contexts[old].users.fetch_sub(1, std::memory_order_release);
                }
                if(selection.channel < 0) derived().selectedGlobally(next, out);
                next.users.fetch_sub(1, std::memory_order_release); // queued-selection pin
                ++tail;
            }
            selectionTail.store(tail, std::memory_order_release);
        }

        static clap_process_status process(const clap_plugin_t* p, const clap_process_t* process)
        {
            auto& s = self(p);
            if(process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2)
                return CLAP_PROCESS_ERROR;
            s.output = process->out_events; s.eventTime = 0;
            struct ClearOutput { Engine& s; ~ClearOutput() { s.output = nullptr; s.ringOut = nullptr; } } clearOutput{s}; // ring_out gives one process() its buffer
            s.applySelections(process->out_events);
            const uint32_t frames = process->frames_count;
            const uint32_t count = process->in_events->size(process->in_events);
            uint32_t done = 0;
            for(uint32_t e = 0; e < count; ++e)
            {
                const auto* header = process->in_events->get(process->in_events, e);
                if(header->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
                const uint32_t at = header->time < frames ? header->time : frames;
                if(at > done) { s.renderSegment(process, done, at); done = at; }
                s.eventTime = at;
                if(header->type == CLAP_EVENT_PARAM_VALUE || header->type == CLAP_EVENT_PARAM_MOD)
                    s.applyParam(header);
                else if(header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF || header->type == CLAP_EVENT_NOTE_CHOKE)
                {
                    const auto* note = reinterpret_cast<const clap_event_note_t*>(header);
                    if(header->type == CLAP_EVENT_NOTE_ON) s.noteOn(*note);
                    else s.noteOff(*note, header->type == CLAP_EVENT_NOTE_CHOKE);
                }
                else if(header->type == CLAP_EVENT_MIDI) s.applyMidi(*reinterpret_cast<const clap_event_midi_t*>(header));
                else if(header->type == CLAP_EVENT_NOTE_EXPRESSION)
                    s.applyExpression(*reinterpret_cast<const clap_event_note_expression_t*>(header));
            }
            s.renderSegment(process, done, frames);
            return CLAP_PROCESS_CONTINUE;
        }

        // oscairn.sound-snapshots draft 3: the ring-out buffer for the next process(), or none.
        static void setRingOut(const clap_plugin_t* p, const clap_audio_buffer_t* buffer)
        {
            const bool valid = buffer && buffer->channel_count >= 4 && buffer->data32 && buffer->data32[0] && buffer->data32[1] && buffer->data32[2] && buffer->data32[3];
            self(p).ringOut = valid ? buffer->data32 : nullptr;
        }
        // Main thread, draft 3: queued like select. At that boundary, before the selections queued after
        // it, every retiring context's voices are choked (the choke fade) and render into ringOut[2..3]
        // until they end. False when the selection queue is full.
        static bool chokeRetiring(const clap_plugin_t* p)
        {
            auto& s = self(p);
            const auto head = s.selectionHead.load(std::memory_order_relaxed);
            if(head - s.selectionTail.load(std::memory_order_acquire) == selectionCapacity) return false;
            s.selections[head % selectionCapacity] = {0, chokeMark};
            s.selectionHead.store(head + 1, std::memory_order_release); return true;
        }
        // Audio thread, while rendering: the two ring-out channels the context adds its main output into
        // instead of its main port, or null. A retiring context (selected by no channel; only contexts
        // with voices are rendered at all) takes ringOut[0..1], one chokeRetiring is fading ringOut[2..3].
        // Null without a ring-out buffer. Its main output only: a retiring context's other ports render
        // to the instance's ports as usual.
        float* const* retiring(uint32_t slot) const
        {
            if(!ringOut || contexts[slot].selected.load(std::memory_order_relaxed)) return nullptr;
            return fading[slot] ? ringOut + 2 : ringOut;
        }

        // Main thread: a free non-default slot, or null. publish() assigns its ID.
        Context* freeSlot()
        {
            if(nextContext == CLAP_INVALID_ID) return nullptr;
            for(uint32_t i = 1; i < ContextCapacity; ++i) if(!contexts[i].prepared) return &contexts[i];
            return nullptr;
        }
        uint32_t publish(Context& c, const char* name)
        { c.id = nextContext++; std::strcpy(c.name, name); c.prepared = true; return c.id; }
        // prepare_preset: a context from the preset at (kind, location, loadKey), read by Derived::readPreset.
        static uint32_t preparePreset(const clap_plugin_t* p, const char* name, uint32_t kind, const char* location, const char* loadKey, char* message, uint32_t capacity)
        {
            auto fail = [&](const char* why) { if(message && capacity) std::snprintf(message, capacity, "%s", why); return CLAP_INVALID_ID; };
            if(!name || std::strlen(name) > 63) return fail("invalid sound name");
            if(kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? location != nullptr : kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE || !location)
                return fail("invalid preset location");
            auto& s = self(p); auto* c = s.freeSlot();
            if(!c) return fail("no free sound context");
            Patch patch{}; std::string error;
            if(!s.readPreset(kind, location, loadKey, patch, error)) return fail(error.empty() ? "preset not read" : error.c_str());
            c->patch = std::move(patch);
            if(!s.buildPreset(*c, error)) { c->patch = Patch{}; return fail(error.empty() ? "preset not built" : error.c_str()); }
            return s.publish(*c, name);
        }

        // The snapshot callbacks are called only with rendering excluded.
        static bool saveSnapshot(const clap_plugin_t* p, const clap_ostream_t* stream)
        {
            auto& s = self(p); s.applySelections(nullptr);
            std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17) << Derived::stateVersion << '\n';
            for(uint32_t slot = 0; slot < ContextCapacity; ++slot)
            {
                const auto& c = s.contexts[slot]; out << c.prepared;
                if(c.prepared) { out << ' ' << std::quoted(c.name); if(!s.writePatch(out, c.patch)) return false; }
                out << '\n';
            }
            for(auto slot : s.selected) out << slot << ' ';
            out << '\n'; const auto data = out.str();
            for(size_t at = 0; at < data.size();)
            {
                const auto n = stream->write(stream, data.data() + at, data.size() - at);
                if(n <= 0 || size_t(n) > data.size() - at) return false;
                at += size_t(n);
            }
            return true;
        }
        struct SavedContext { bool used = false; std::string name; Patch patch{}; };
        using Saved = std::array<SavedContext, ContextCapacity>;
        // Reads and validates a whole snapshot without changing anything; `needed`: the sounds in it.
        bool readSnapshot(const clap_istream_t* stream, Saved& saved, std::array<uint32_t, 16>& selection, uint32_t& needed)
        {
            std::string data; char buffer[1024];
            for(;;)
            {
                const auto n = stream->read(stream, buffer, sizeof(buffer));
                if(n < 0 || n > 1024 || data.size() + n > Derived::maxStateBytes) return false;
                if(!n) break;
                data.append(buffer, size_t(n));
            }
            std::istringstream in(data); in.imbue(std::locale::classic()); std::string version;
            if(!(in >> version) || version != Derived::stateVersion) return false;
            needed = 0;
            for(auto& patch : saved)
            {
                unsigned used; if(!(in >> used) || used > 1) return false;
                patch.used = used;
                if(!used) continue;
                ++needed;
                if(!(in >> std::quoted(patch.name)) || patch.name.size() > 63 || patch.name.find('\0') != std::string::npos) return false;
                if(!derived().readPatch(in, patch.patch)) return false;
            }
            if(!saved[0].used) return false;
            for(auto& slot : selection) if(!(in >> slot) || slot >= saved.size() || !saved[slot].used) return false;
            in >> std::ws; return in.eof();
        }
        static bool loadSnapshot(const clap_plugin_t* p, const clap_istream_t* stream)
        {
            auto& s = self(p);
            struct Loading { Derived& s; explicit Loading(Derived& d) : s(d) { s.snapshotLoading(true); } ~Loading() { s.snapshotLoading(false); } } loading{s};
            Saved saved{}; std::array<uint32_t, 16> selection{}; uint32_t needed = 0;
            if(!s.readSnapshot(stream, saved, selection, needed)) return false;
            if(needed - 1 > CLAP_INVALID_ID - s.nextContext) return false;
            s.applySelections(nullptr); s.resetVoices();
            for(size_t i = 0; i < saved.size(); ++i)
            {
                auto& c = s.contexts[i]; auto& patch = saved[i];
                c.prepared = patch.used; c.users = 0; c.selected = 0; c.voices = 0;
                c.patch = patch.used ? std::move(patch.patch) : Patch{};
                if(!patch.used) continue;
                c.id = i ? s.nextContext++ : 0; std::strcpy(c.name, patch.name.c_str());
            }
            s.selected = selection;
            for(size_t ch = 0; ch < selection.size(); ++ch) { auto& c = s.contexts[selection[ch]]; c.users.fetch_add(1); c.selected.fetch_or(1u << ch); }
            s.restored();
            return true;
        }
        // oscairn.sound-snapshots: every sound of a snapshot prepared as a new context beside the
        // playing ones, as prepare does (main thread, concurrently with rendering). All or nothing.
        static bool prepareSnapshot(const clap_plugin_t* p, const clap_istream_t* stream, uint32_t* selected, char* message, uint32_t capacity)
        {
            auto fail = [&](const char* why) { if(message && capacity) std::snprintf(message, capacity, "%s", why); return false; };
            if(!stream || !selected) return fail("invalid arguments");
            auto& s = self(p);
            Saved saved{}; std::array<uint32_t, 16> selection{}; uint32_t needed = 0;
            if(!s.readSnapshot(stream, saved, selection, needed)) return fail("invalid snapshot");
            uint32_t free = 0;
            for(uint32_t i = 1; i < ContextCapacity; ++i) free += !s.contexts[i].prepared;
            if(free < needed || needed > CLAP_INVALID_ID - s.nextContext) return fail("no free sound contexts for the snapshot");
            std::array<Context*, ContextCapacity> made{}; std::array<uint32_t, ContextCapacity> ids{};
            for(uint32_t i = 0; i < ContextCapacity; ++i)
            {
                if(!saved[i].used) continue;
                auto* c = s.freeSlot(); c->patch = std::move(saved[i].patch); std::string error;
                if(!s.buildPreset(*c, error))
                {
                    c->patch = Patch{};
                    for(auto* m : made) if(m) { m->prepared = false; m->patch = Patch{}; s.discarded(*m); } // never selected: no users
                    return fail(error.empty() ? "sound not built" : error.c_str());
                }
                ids[i] = s.publish(*c, saved[i].name.c_str()); made[i] = c;
            }
            for(size_t ch = 0; ch < 16; ++ch) selected[ch] = ids[selection[ch]];
            return true;
        }

        // Extension callbacks. Each plugin builds its own tables from these (see sampler.cpp):
        // plain internal globals rather than template statics shared across modules.
        static uint32_t capacity(const clap_plugin_t*) { return ContextCapacity; }
        static bool get(const clap_plugin_t* p, uint32_t slot, oscairn_sound_context_info_t* info)
        {
            if(slot >= ContextCapacity || !info) return false;
            const auto& c = self(p).contexts[slot];
            if(!c.prepared) return false;
            *info = {}; info->id = c.id; std::memcpy(info->name, c.name, sizeof(c.name));
            info->selected_channels = c.selected.load(); info->active_voices = c.voices.load();
            info->idle = c.users.load(std::memory_order_acquire) == 0; return true;
        }
        static bool select(const clap_plugin_t* p, uint32_t id, int16_t channel)
        {
            if(channel < -1 || channel > 15) return false;
            auto& s = self(p);
            const auto head = s.selectionHead.load(std::memory_order_relaxed);
            if(head - s.selectionTail.load(std::memory_order_acquire) == selectionCapacity) return false;
            for(uint32_t i = 0; i < ContextCapacity; ++i)
            {
                auto& c = s.contexts[i]; if(!c.prepared || c.id != id) continue;
                c.users.fetch_add(1, std::memory_order_relaxed); s.selections[head % selectionCapacity] = {i, channel};
                s.selectionHead.store(head + 1, std::memory_order_release); return true;
            }
            return false;
        }
        static bool discard(const clap_plugin_t* p, uint32_t id)
        {
            if(!id) return false;
            auto& s = self(p);
            for(auto& c : s.contexts) if(c.prepared && c.id == id)
            {
                if(c.users.load(std::memory_order_acquire)) return false;
                c.prepared = false; s.discarded(c); return true;
            }
            return false;
        }
        // Main thread, after a context is unpinned and discarded.
        void discarded(Context&) {}
        void snapshotLoading(bool) {}
        bool buildPreset(Context&, std::string&) { return true; }
        void voiceFinished(Voice&) {}
    };
}

// SPDX-License-Identifier: MIT
//
// Part of the Oscairn plugin SDK (MIT, like the extension headers beside it; relicensed from
// AGPL-3.0-or-later on 2026-10-31 so plugins of any licence can use it).
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
// and may provide snapshotLoading(bool), called around reading a snapshot's patches.

#pragma once
#include <clap/clap.h>
#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <array>
#include <atomic>
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
        double   gain = 1.0;     // CLAP's per-note volume expression, linear 0..4
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
        uint32_t eventTime = 0;
        std::array<Voice, VoiceCount> voices{};
        uint64_t notesStarted = 0;
        std::array<Context, ContextCapacity> contexts;
        std::array<uint32_t, 16> selected{}; // audio thread
        std::array<bool, 16> sustainDown{};
        struct Selection { uint32_t slot; int16_t channel; };
        std::array<Selection, selectionCapacity> selections{};
        std::atomic<uint32_t> selectionHead{0}, selectionTail{0};
        uint32_t nextContext = 1; // main thread, never reuse IDs
        uint32_t chokeFrames = 144; // 3 ms at 48 kHz; setSampleRate keeps it 3 ms

        // From activate: a choke fades a voice out over 3 ms instead of cutting it (RFC 0018).
        void setSampleRate(double rate) { chokeFrames = rate > 0 ? std::max<uint32_t>(1, uint32_t(rate * .003 + .5)) : 144; }
        // A voice's gain at `offset` frames into the segment being rendered: its volume expression,
        // and its choke fade. Multiply each of its samples by it; a voice that ignores it still
        // ends when its fade would have, but with a cut.
        static double amplitude(const Voice& v, uint32_t offset)
        {
            if(!v.chokeLength) return v.gain;
            return v.chokeLeft > offset ? v.gain * double(v.chokeLeft - offset) / double(v.chokeLength) : 0.0;
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
            endNote(v.noteId, v.channel, v.key);
            v.active = false;
            auto& c = contexts[v.context];
            c.voices.fetch_sub(1, std::memory_order_relaxed);
            // Last access to this context before releasing its lifetime pin.
            c.users.fetch_sub(1, std::memory_order_release);
        }
        void resetVoices()
        {
            for(auto& v : voices) finishVoice(v);
            sustainDown.fill(false);
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
                for(auto& v : voices) if(matches(v, e.note_id, e.key, e.channel)) v.gain = e.value < 0 ? 0 : e.value > 4 ? 4 : e.value;
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
            if(kind != 0xb0) return;
            if(e.data[1] == 64)
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
            struct ClearOutput { Engine& s; ~ClearOutput() { s.output = nullptr; } } clearOutput{s};
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

        // Main thread: a free non-default slot, or null. publish() assigns its ID.
        Context* freeSlot()
        {
            if(nextContext == CLAP_INVALID_ID) return nullptr;
            for(uint32_t i = 1; i < ContextCapacity; ++i) if(!contexts[i].prepared) return &contexts[i];
            return nullptr;
        }
        uint32_t publish(Context& c, const char* name)
        { c.id = nextContext++; std::strcpy(c.name, name); c.prepared = true; return c.id; }

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
        static bool loadSnapshot(const clap_plugin_t* p, const clap_istream_t* stream)
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
            auto& s = self(p);
            struct Loading { Derived& s; explicit Loading(Derived& d) : s(d) { s.snapshotLoading(true); } ~Loading() { s.snapshotLoading(false); } } loading{s};
            struct Saved { bool used = false; std::string name; Patch patch{}; };
            std::array<Saved, ContextCapacity> saved{}; std::array<uint32_t, 16> selection{}; uint32_t needed = 0;
            for(auto& patch : saved)
            {
                unsigned used; if(!(in >> used) || used > 1) return false;
                patch.used = used;
                if(!used) continue;
                ++needed;
                if(!(in >> std::quoted(patch.name)) || patch.name.size() > 63 || patch.name.find('\0') != std::string::npos) return false;
                if(!s.readPatch(in, patch.patch)) return false;
            }
            if(!saved[0].used) return false;
            for(auto& slot : selection) if(!(in >> slot) || slot >= saved.size() || !saved[slot].used) return false;
            in >> std::ws; if(!in.eof()) return false;
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
    };
}

// SPDX-License-Identifier: MIT
//
// Part of the Oscairn plugin SDK (MIT, like the extension headers beside it).
//
// oscairn.voice-budget (draft 1, voice-budget.h) for a plugin's own pool of voices: the settings
// the host gives (Limits), the order to give voices up in (victim), and the report of each note's
// loudness (Report). Header-only, no allocation; everything here runs on the audio thread.
//
// An instrument built on sound-contexts-plugin.hpp gets all of it from its Engine (see there).
// Any other engine, such as one playing several regions or elements per note, uses it so:
//   - keep a Limits; set_limits calls limits.set(limit, contexts, count) (never during process);
//   - when a new voice is needed, or at a block's start while more voices sound than the limit
//     (or a context's own limit) allows, describe each voice as a Candidate and call victim();
//     fade the voice it names out (2 to 5 ms) rather than cutting it, unless the pool has no
//     free slot at all;
//   - for active_voices, count the voices sounding; for notes, add each voice to a Report and
//     write it out.
// victim() gives up voices one at a time, so a layered note loses its quietest layer before
// another note loses its only one when the layer is the quieter: keep a note's every voice a
// Candidate with its own loudness.

#pragma once
#include <oscairn-clap/voice-budget.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace oscairn::voices
{
    // The host's settings, as set_limits last gave them.
    struct Limits
    {
        uint32_t limit = 0; // voices for the instance; 0: as many as the pool holds
        std::array<oscairn_voice_budget_context_t, OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS> contexts{};
        uint32_t count = 0;
        void set(uint32_t voices, const oscairn_voice_budget_context_t* list, uint32_t n) noexcept
        {
            limit = voices; count = list ? std::min<uint32_t>(n, OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS) : 0;
            for(uint32_t i = 0; i < count; ++i) contexts[i] = list[i];
        }
        // A context's settings: its own entry; else, for one a channel selects, the entry for the
        // selected contexts; else none (no limit or reserve, priority 0).
        oscairn_voice_budget_context_t of(uint32_t id, bool selected) const noexcept
        {
            const oscairn_voice_budget_context_t* found = nullptr;
            for(uint32_t i = 0; i < count; ++i)
            {
                if(contexts[i].context == id && id != OSCAIRN_VOICE_BUDGET_SELECTED) return contexts[i];
                if(selected && contexts[i].context == OSCAIRN_VOICE_BUDGET_SELECTED) found = &contexts[i];
            }
            if(found) return *found;
            oscairn_voice_budget_context_t none{}; none.context = id; return none;
        }
    };

    // A voice as the order sees it. ending: already fading out (a choke or a steal), so giving it
    // up frees nothing new; mono: the plugin's own sound plays mono, whatever the host says.
    struct Candidate
    {
        uint32_t context = 0;
        bool selected = false, held = false, ending = false, mono = false;
        double loudness = 0;
        uint64_t age = 0; // larger: newer
    };

    constexpr uint32_t none = OSCAIRN_VOICE_BUDGET_SELECTED; // victim()'s `incoming` when no new voice waits

    // The voice to give up, by voice-budget.h's order, among `count` voices that get(i) describes
    // (a Candidate, or nullptr for a free slot): for a new voice in context `incoming` (or `none`),
    // from context `only` alone when it is not `none` (to keep that context within its own limit).
    // -1 when every voice is protected or ending.
    template<class Get>
    int victim(size_t count, Get&& get, const Limits& limits, uint32_t incoming, uint32_t only = none)
    {
        // Each context's held voices, for its reserve.
        struct Held { uint32_t context = 0, count = 0; };
        std::array<Held, 64> held{}; size_t contexts = 0;
        auto heldOf = [&](uint32_t context) -> uint32_t& {
            for(size_t i = 0; i < contexts; ++i) if(held[i].context == context) return held[i].count;
            if(contexts == held.size()) return held[held.size() - 1].count; // shortcut: past 64 contexts the last slot is shared; no Oscairn plugin has that many
            held[contexts].context = context; return held[contexts++].count;
        };
        for(size_t i = 0; i < count; ++i) if(const Candidate* c = get(i); c && c->held && !c->ending) ++heldOf(c->context);
        int best = -1; const Candidate* chosen = nullptr; int bestRank = 0, bestOther = 0; uint8_t bestPriority = 0;
        for(size_t i = 0; i < count; ++i)
        {
            const Candidate* c = get(i);
            if(!c || c->ending || (only != none && c->context != only)) continue;
            const auto settings = limits.of(c->context, c->selected);
            if(c->held && (c->mono || (settings.flags & (OSCAIRN_VOICE_BUDGET_MONO | OSCAIRN_VOICE_BUDGET_LEGATO)))) continue; // never a held mono key
            if(c->held && c->context != incoming && heldOf(c->context) <= settings.reserve) continue; // its reserve
            const int rank = c->held ? 1 : 0, other = incoming != none && c->context != incoming ? 1 : 0;
            const bool better = !chosen || (rank != bestRank ? rank < bestRank : other != bestOther ? other < bestOther :
                settings.priority != bestPriority ? settings.priority < bestPriority : c->loudness != chosen->loudness ? c->loudness < chosen->loudness : c->age < chosen->age);
            if(better) { best = int(i); chosen = c; bestRank = rank; bestOther = other; bestPriority = settings.priority; }
        }
        return best;
    }

    // Each note's voices and loudness, for notes(): add every sounding voice, then write.
    template<size_t Capacity>
    struct Report
    {
        std::array<oscairn_voice_budget_note_t, Capacity> notes{};
        uint32_t count = 0;
        void add(int32_t noteId, int16_t port, int16_t channel, int16_t key, double loudness) noexcept
        {
            for(uint32_t i = 0; i < count; ++i)
            {
                auto& n = notes[i];
                if(n.note_id == noteId && n.port_index == port && n.channel == channel && n.key == key)
                { ++n.voices; n.loudness = std::max(n.loudness, float(loudness)); return; }
            }
            if(count < Capacity) notes[count++] = {noteId, port, channel, key, 1, float(loudness)};
        }
        uint32_t write(oscairn_voice_budget_note_t* out, uint32_t capacity) const noexcept
        {
            const uint32_t n = out ? std::min(count, capacity) : 0;
            for(uint32_t i = 0; i < n; ++i) out[i] = notes[i];
            return n;
        }
    };
}

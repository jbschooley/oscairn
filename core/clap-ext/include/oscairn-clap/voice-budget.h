/* SPDX-License-Identifier: MIT */
#pragma once
#include <clap/clap.h>
#ifdef __cplusplus
extern "C" {
#endif

/*
 * oscairn.voice-budget (draft 1): a plugin that gives up its own voices within a limit the host
 * sets, so the host need not choose among its notes (RFC 0018, option C). The host shares one
 * processing budget among every plugin; it measures each plugin's cost, and for a plugin with
 * this extension it reads what the plugin plays and tells it how many voices it may keep.
 *
 * A voice is whatever the plugin renders and can give up on its own: a note's one oscillator, one
 * unison copy, one layer, or one element or region of a sample-based sound. A note may sound with
 * several voices; giving one up need not end the note. The plugin fades what it gives up (2 to 5
 * ms is enough), never cuts it.
 *
 * Per sound context (oscairn.sound-contexts), which is per part: a plugin playing several parts'
 * sounds from one pool of voices (one instance, several contexts) gets each context's limit,
 * reserve and priority, and whether its part plays mono or legato, and chooses among its voices
 * accordingly (the order below). A plugin without sound contexts reads context 0's settings.
 * (Oscairn: parts sharing one instance, RFC 0021, get an entry each, for the sound their channel
 * selects, with the part's reserve and priority; an instance one part plays alone gets one entry,
 * for the selected contexts, which also covers a Performance change, the outgoing sounds ringing
 * out beside the incoming ones. No mode flags, since parts have no mono or legato mode yet, and
 * no limit of a context's own: the host keeps each part's note limit itself, RFC 0018 step 2.)
 *
 * Calls, all optional to act on but required in the table (no null entries):
 *
 *   honors_volume [main-thread]: whether every voice follows CLAP_NOTE_EXPRESSION_VOLUME, so
 *   the host's own 5 ms fade before a choke (when it gives up a whole note at its hard limit)
 *   is known to work.
 *
 *   set_limits [audio-thread, never during process()]: the voices the instance may keep from the
 *   next process() on (0: as many as its pool holds), and the settings of up to
 *   OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS contexts. The host calls it between blocks whenever any of
 *   it changes; the plugin copies what it needs. Past the limit, the plugin gives up voices at
 *   the start of the next block, in its own order; a new note past it takes a voice the same way.
 *
 *   active_voices [audio-thread, after process()]: the voices sounding now (fading ones included),
 *   and the plugin's estimate of one voice's cost in microseconds a block at the current block
 *   size (0: no estimate). The host measures the instance's time either way, and uses the count
 *   to share it per voice rather than per note.
 *
 *   notes [audio-thread, after process()]: each note still sounding, by the address the host gave
 *   it (note ID, port, channel, key), with how many voices sound it and how loud it is now: the
 *   largest of its voices' current gains, linear, 1 for a voice at full level (its envelope,
 *   velocity and volume expression; not the patch's output level). Writes at most `capacity` and
 *   returns how many it wrote. The host compares these across plugins to give up the least
 *   audible notes first.
 *
 * The order a plugin gives up voices in, which the Oscairn SDK's helper (voice-budget-plugin.hpp)
 * implements:
 *   1. never a held voice of a context marked OSCAIRN_VOICE_BUDGET_MONO or _LEGATO (its one key,
 *      a legato line's included), nor a held voice of a context at or under its reserve, unless
 *      the new voice is for that same context;
 *   2. released voices (key up, pedal or not) before held ones;
 *   3. the new voice's own context before others;
 *   4. the context of lower priority before higher;
 *   5. the quietest, then the oldest.
 * A plugin may refine this (one element of a layered note before a whole note) but should not
 * break rule 1.
 */

static const char OSCAIRN_EXT_VOICE_BUDGET[] = "oscairn.voice-budget/draft-1";

enum
{
    OSCAIRN_VOICE_BUDGET_MONO = 1 << 0,   // the part plays one note at a time
    OSCAIRN_VOICE_BUDGET_LEGATO = 1 << 1, // and glides between notes without retriggering
};

// The context set_limits names when it means every context a MIDI channel selects (the part's own
// sounds), apart from those it names by ID.
static const uint32_t OSCAIRN_VOICE_BUDGET_SELECTED = CLAP_INVALID_ID;
static const uint32_t OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS = 16;

typedef struct oscairn_voice_budget_context
{
    uint32_t context;  // a sound context's ID, or OSCAIRN_VOICE_BUDGET_SELECTED
    uint32_t limit;    // voices it may keep; 0: no limit of its own
    uint32_t reserve;  // voices never given up for another context's
    uint8_t  priority; // 0 to 10: a context of lower priority gives way first
    uint8_t  flags;    // OSCAIRN_VOICE_BUDGET_MONO, OSCAIRN_VOICE_BUDGET_LEGATO
} oscairn_voice_budget_context_t;

typedef struct oscairn_voice_budget_note
{
    int32_t  note_id;
    int16_t  port_index, channel, key;
    uint32_t voices;   // voices sounding it now
    float    loudness; // linear, the largest of its voices' gains now
} oscairn_voice_budget_note_t;

typedef struct oscairn_plugin_voice_budget
{
    bool (CLAP_ABI* honors_volume)(const clap_plugin_t* plugin);
    void (CLAP_ABI* set_limits)(const clap_plugin_t* plugin, uint32_t limit, const oscairn_voice_budget_context_t* contexts, uint32_t count);
    uint32_t (CLAP_ABI* active_voices)(const clap_plugin_t* plugin, double* voice_cost);
    uint32_t (CLAP_ABI* notes)(const clap_plugin_t* plugin, oscairn_voice_budget_note_t* notes, uint32_t capacity);
} oscairn_plugin_voice_budget_t;

#ifdef __cplusplus
}
#endif

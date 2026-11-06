/* SPDX-License-Identifier: MIT */
#pragma once
#include <clap/clap.h>
#include <oscairn-clap/assets.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native prepared sound contexts. Context-management methods run on the main thread
 * concurrently with process(), without stopping voices; state callbacks are exclusive.
 * Capacity is constant for the instance lifetime; Oscairn refuses a plugin reporting more
 * than 256 or missing any function but state. Telemetry may change while read.
 * prepare builds an independent patch from plugin defaults plus supplied values, or
 * from patch text when the plugin defines one (the Oscairn Sampler takes SFZ). Asset
 * paths in that text are relative to base, a library folder whose files the plugin
 * resolves through the host's oscairn.assets extension (base->sha256 is unused). Both
 * are null for parameter-only patches. Preparation may read files.
 * select queues a bounded operation for the next process boundary, before notes;
 * channel -1 selects all channels. Accepted selections pin their context until
 * consumed. Voices, sustain and expression retain their owning context.
 * discard fails while selected, queued, sounding, or retaining a tail. IDs are
 * never reused during an instance lifetime. Unload invalidates all IDs.
 * prepare_preset (draft 5, optional) builds a patch from one of the presets the plugin lists
 * through CLAP preset discovery, with the same rules as prepare.
 * No format-level claim is made for plugins not implementing this extension.
 */
static const char OSCAIRN_EXT_SOUND_CONTEXTS[] = "oscairn.sound-contexts/draft-5";
typedef struct oscairn_sound_parameter { clap_id id; double value; } oscairn_sound_parameter_t;
typedef struct oscairn_sound_context_info {
    uint32_t id;
    char name[64];
    uint32_t selected_channels; /* bit mask; telemetry fields are independent snapshots */
    uint32_t active_voices;
    bool idle; /* no selections, pending requests, voices or tails */
} oscairn_sound_context_info_t;
typedef struct oscairn_plugin_sound_contexts {
    uint32_t (CLAP_ABI *capacity)(const clap_plugin_t*);
    bool (CLAP_ABI *get)(const clap_plugin_t*,uint32_t slot,oscairn_sound_context_info_t*);
    /* CLAP_INVALID_ID means rejected: invalid patch or no free context. On rejection the
     * plugin may write a NUL-terminated diagnostic (with source line) into message. */
    uint32_t (CLAP_ABI *prepare)(const clap_plugin_t*,const char* name,const oscairn_sound_parameter_t*,uint32_t count,
                                 const char* patch,const oscairn_asset_ref_t* base,char* message,uint32_t message_capacity);
    bool (CLAP_ABI *select)(const clap_plugin_t*,uint32_t id,int16_t channel);
    bool (CLAP_ABI *discard)(const clap_plugin_t*,uint32_t id);
    /* Optional full patch/context/channel snapshot. Unlike the concurrent methods
     * above, the host must exclude process/flush and stop_processing before calling
     * save/load. Includes accepted queued selections, excludes voices and histories.
     * load validates completely before replacing patches and gives them fresh IDs.
     * A host may expose this as its state facility; it is not CLAP_EXT_STATE. */
    const clap_plugin_state_t* state;
    /* Optional: null when the plugin prepares no presets. location_kind, location and load_key
     * exactly as clap_plugin_preset_load.from_location takes them, from the plugin's own preset
     * discovery: location null for CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, else a file path;
     * load_key null for a file holding one preset. Returns as prepare does, with a diagnostic. */
    uint32_t (CLAP_ABI *prepare_preset)(const clap_plugin_t*,const char* name,uint32_t location_kind,const char* location,
                                        const char* load_key,char* message,uint32_t message_capacity);
} oscairn_plugin_sound_contexts_t;

/* A snapshot's sounds prepared beside the playing ones (RFC 0005, "Preparing a snapshot beside"),
 * offered only with oscairn.sound-contexts and its state. prepare runs as the prepare above does:
 * main thread, concurrently with process(), may read files, never touches a playing sound,
 * selection or voice. snapshot is a whole snapshot as state->save writes it. It validates all of
 * it, then prepares every sound in it (its Default too) as a new context with a fresh ID; all or
 * nothing, never stealing. selected[16] receives the new ID each MIDI channel selects in the
 * snapshot; nothing is selected. On false nothing was added, and the plugin may write a
 * NUL-terminated diagnostic into message.
 * ring_out (draft 3, optional; RFC 0005, "Ringing out through the outgoing chain"): called on the
 * audio thread immediately before every process() call, with a zeroed buffer of four data32
 * channels of that call's frames_count, or null for none. Valid only until that process()
 * returns; a process() not preceded by a call has none. During that process() the plugin adds the
 * main-port output of every retiring context (selected by no MIDI channel, with voices or a tail
 * left) into channels 0 and 1 instead of its main output port, and that of a retiring context
 * choke_retiring is fading into channels 2 and 3. It is not an audio port: the plugin only stores
 * the pointer, without allocating, locking or blocking.
 * choke_retiring (draft 3, required with ring_out): only one change's tail carries over. At a
 * second Performance change while the first change's tail still rings, the host calls it on the
 * main thread just before selecting the incoming sounds. It is queued like select: at that process
 * boundary, before the selections queued after it, the plugin chokes every context retiring then
 * (a short fade, as a CLAP choke, no click), whose main output goes into channels 2 and 3 until it
 * ends; the sounds the selections leave then take channels 0 and 1. False when nothing could be
 * queued.
 * Limits (RFC 0005): only the main port rings out, other output ports follow the incoming chain;
 * and a note held more than 30 s after the change goes back to the main output, the incoming chain. */
static const char OSCAIRN_EXT_SOUND_SNAPSHOTS[] = "oscairn.sound-snapshots/draft-3";
typedef struct oscairn_plugin_sound_snapshots {
    bool (CLAP_ABI *prepare)(const clap_plugin_t*,const clap_istream_t* snapshot,uint32_t* selected,char* message,uint32_t message_capacity);
    void (CLAP_ABI *ring_out)(const clap_plugin_t*,const clap_audio_buffer_t* buffer);
    bool (CLAP_ABI *choke_retiring)(const clap_plugin_t*);
} oscairn_plugin_sound_snapshots_t;
#ifdef __cplusplus
}
#endif

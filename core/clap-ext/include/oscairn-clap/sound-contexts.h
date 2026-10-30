/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <clap/clap.h>
#include <oscairn-clap/assets.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native prepared sound contexts. Context-management methods run on the main thread
 * concurrently with process(), without stopping voices; state callbacks are exclusive.
 * Capacity is constant for the instance lifetime. Telemetry may change while read.
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
 * No format-level claim is made for plugins not implementing this extension.
 */
static const char OSCAIRN_EXT_SOUND_CONTEXTS[] = "oscairn.sound-contexts/draft-4";
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
} oscairn_plugin_sound_contexts_t;
#ifdef __cplusplus
}
#endif

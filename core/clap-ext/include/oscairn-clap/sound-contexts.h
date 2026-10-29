/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Native prepared sound contexts. All methods are called on the main thread and
 * MUST be safe concurrently with process(), without stopping existing voices.
 * Capacity is constant for the instance lifetime. Telemetry may change while read.
 * prepare builds an independent patch from plugin defaults plus supplied values.
 * select queues a bounded operation for the next process boundary, before notes;
 * channel -1 selects all channels. Accepted selections pin their context until
 * consumed. Voices, sustain and expression retain their owning context.
 * discard fails while selected, queued, sounding, or retaining a tail. IDs are
 * never reused during an instance lifetime. Unload invalidates all IDs.
 * No format-level claim is made for plugins not implementing this extension.
 */
static const char OSCAIRN_EXT_SOUND_CONTEXTS[] = "oscairn.sound-contexts/draft";
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
    /* CLAP_INVALID_ID means rejected: invalid patch or no free context. */
    uint32_t (CLAP_ABI *prepare)(const clap_plugin_t*,const char* name,const oscairn_sound_parameter_t*,uint32_t count);
    bool (CLAP_ABI *select)(const clap_plugin_t*,uint32_t id,int16_t channel);
    bool (CLAP_ABI *discard)(const clap_plugin_t*,uint32_t id);
} oscairn_plugin_sound_contexts_t;
#ifdef __cplusplus
}
#endif

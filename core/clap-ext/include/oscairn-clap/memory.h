/* SPDX-License-Identifier: MIT */
#pragma once
#include <clap/clap.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Memory declared before it is allocated (docs/rfcs/0025-resource-budgets.md#declarations).
 * A plugin reports what activating would allocate, so a host near its memory limit can refuse
 * it before the allocation instead of after. The host still measures what the plugin holds and
 * counts the larger of the two; a plugin found holding more than it declared is no longer
 * trusted on that device.
 *
 * Bytes are the plugin's own storage, excluding host and allocator overhead:
 *   shared_bytes:  once per instance (or per engine shared by effect contexts): tables,
 *                  engine-wide buffers, audio shared between contexts;
 *   context_bytes: each context; an instance without contexts is one context, and with
 *                  oscairn.effect-contexts or oscairn.sound-contexts the originating
 *                  interface is one too. A new context on a running engine costs this alone;
 *   asset_bytes:   decoded assets (samples) the current state uses.
 * The total for an instance is shared_bytes + context_bytes + asset_bytes. An engine that also
 * offers oscairn.effect-contexts reports the same shared and context bytes through both. */
static const char OSCAIRN_EXT_MEMORY[] = "oscairn.memory/draft";
typedef struct oscairn_memory {
    uint64_t shared_bytes;
    uint64_t context_bytes;
    uint64_t asset_bytes;
} oscairn_memory_t;
typedef struct oscairn_plugin_memory {
    /* Main thread, after init and after the state is loaded, before activate: what activating
     * at this sample rate and maximum block size would allocate for the current state (its
     * mode, its samples). Makes no allocation of that size. The host may call it again after
     * loading another state. False: not known for this state; the host uses its measurement. */
    bool (CLAP_ABI *estimate)(const clap_plugin_t*, double sample_rate, uint32_t max_frames,
                              oscairn_memory_t* out);
    /* Main thread, after activate: what the plugin holds now. False: not known. */
    bool (CLAP_ABI *current)(const clap_plugin_t*, oscairn_memory_t* out);
} oscairn_plugin_memory_t;

#ifdef __cplusplus
}
#endif

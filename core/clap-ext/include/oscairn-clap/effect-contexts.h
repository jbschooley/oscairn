/* SPDX-License-Identifier: MIT */
#pragma once
#include <clap/clap.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Each context is a separately scheduled CLAP interface with independent params,
 * state, history, host callbacks and lifecycle. create is main-thread only, safe
 * concurrently with rendering existing contexts, and returns an uninitialized
 * interface owned by the caller (normal init/activate/destroy rules). Failure
 * leaves existing contexts untouched. Children keep engine resources alive even
 * after the originating interface is destroyed; the host keeps the module loaded.
 * Contexts use the same sample rate; no concurrent process calls on one engine.
 * Hosts must retire every referencing audio plan before destroying an interface.
 * Destruction deliberately cuts that context's tail. To preserve it, keep feeding
 * silence until tail_active is false. This is independent of shared-send routing.
 * No engine/instance addresses or physical slots are persistent identities.
 */
static const char OSCAIRN_EXT_EFFECT_CONTEXTS[] = "oscairn.effect-contexts/draft";
typedef struct oscairn_effect_resources {
    uint64_t shared_bytes;  /* engine-owned storage, counted once per engine */
    uint64_t context_bytes; /* this context's storage, excluding host/allocator overhead */
    uint32_t capacity, prepared; /* includes the originating context */
    bool tail_active; /* atomic snapshot, not permission to destroy a live audio plan */
} oscairn_effect_resources_t;
typedef struct oscairn_plugin_effect_contexts {
    const clap_plugin_t* (CLAP_ABI *create)(const clap_plugin_t*, const clap_host_t*);
    bool (CLAP_ABI *resources)(const clap_plugin_t*, oscairn_effect_resources_t*);
} oscairn_plugin_effect_contexts_t;
#ifdef __cplusplus
}
#endif

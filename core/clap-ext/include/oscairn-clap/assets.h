/* SPDX-License-Identifier: MIT */
#pragma once
#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sound assets as Performance dependencies (docs/rfcs/0009-sound-assets-and-dependencies.md).
 * A reference names a library, a path relative to it and the file's SHA-256 (64 lowercase hex
 * digits, or empty when not yet known). Plugin state stores references, never local paths. */
typedef struct oscairn_asset_ref {
    char library[64];
    char path[1024];
    char sha256[65];
} oscairn_asset_ref_t;

/* Host extension. Main thread. resolve writes a local, verified file path for ref. When
 * ref->sha256 is empty it is filled in with the file's hash; when given, the file must have
 * that content (identical bytes in another library also satisfy it). On failure returns false
 * and writes a NUL-terminated reason into message. */
static const char OSCAIRN_EXT_ASSETS[] = "oscairn.assets/draft-1";
typedef struct oscairn_host_assets {
    bool (CLAP_ABI *resolve)(const clap_host_t*, oscairn_asset_ref_t* ref, char* path, uint32_t path_capacity,
                             char* message, uint32_t message_capacity);
} oscairn_host_assets_t;

/* Plugin extension. Main thread. Lists every asset the plugin's current state uses, so a host
 * can record a Performance's dependencies and check them before loading it. */
static const char OSCAIRN_EXT_ASSET_DEPENDENCIES[] = "oscairn.asset-dependencies/draft-1";
typedef struct oscairn_plugin_asset_dependencies {
    uint32_t (CLAP_ABI *count)(const clap_plugin_t*);
    bool (CLAP_ABI *get)(const clap_plugin_t*, uint32_t index, oscairn_asset_ref_t* ref);
} oscairn_plugin_asset_dependencies_t;

#ifdef __cplusplus
}
#endif

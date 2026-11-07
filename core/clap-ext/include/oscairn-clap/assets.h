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
 * and writes a NUL-terminated reason into message.
 *
 * Draft 2 adds library_root: the folder holding an installed asset library's files at their
 * paths, read-only, when a version at least min_version (NULL or "": any) is installed. It writes
 * the NUL-terminated path into path and returns true; false when the library is not installed,
 * is older, or the path does not fit. `owner` is what the table came from: the clap_host_t, or
 * the clap_preset_discovery_indexer_t, whose get_extension answers OSCAIRN_EXT_ASSETS too, so a
 * preset provider can declare the library's folder as a location in its init. From an indexer,
 * resolve is NULL. A host answers draft 1 with the same table (resolve only). */
static const char OSCAIRN_EXT_ASSETS[] = "oscairn.assets/draft-2";
static const char OSCAIRN_EXT_ASSETS_DRAFT_1[] = "oscairn.assets/draft-1";
typedef struct oscairn_host_assets {
    bool (CLAP_ABI *resolve)(const clap_host_t*, oscairn_asset_ref_t* ref, char* path, uint32_t path_capacity,
                             char* message, uint32_t message_capacity);
    bool (CLAP_ABI *library_root)(const void* owner, const char* library_id, const char* min_version, char* path, uint32_t path_capacity); /* draft 2 */
} oscairn_host_assets_t;

/* Plugin extension. Main thread. Lists every asset the plugin's current state uses, so a host
 * can record a Performance's dependencies and check them before loading it. Draft 2 adds
 * optional: true for an asset the plugin can do without (it keeps a fallback, as the arpeggiator
 * embeds its phrases), so a host loads the plugin with that asset missing, its resolve failing,
 * and reports a notice instead of refusing. A host also accepts draft 1 (count and get only:
 * every asset required). */
static const char OSCAIRN_EXT_ASSET_DEPENDENCIES[] = "oscairn.asset-dependencies/draft-2";
static const char OSCAIRN_EXT_ASSET_DEPENDENCIES_DRAFT_1[] = "oscairn.asset-dependencies/draft-1";
typedef struct oscairn_plugin_asset_dependencies {
    uint32_t (CLAP_ABI *count)(const clap_plugin_t*);
    bool (CLAP_ABI *get)(const clap_plugin_t*, uint32_t index, oscairn_asset_ref_t* ref);
    bool (CLAP_ABI *optional)(const clap_plugin_t*, uint32_t index); /* draft 2 only; null: every asset required */
} oscairn_plugin_asset_dependencies_t;

#ifdef __cplusplus
}
#endif

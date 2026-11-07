// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Test fixture: the Basic Synth holding 16 MiB of its own for each sound context, as the Oscairn
// Analog holds a synthesizer per sound, and declaring it through oscairn.memory as the Analog does:
// one context's bytes as context_bytes, the state's other sounds in shared_bytes (RFC 0025).
// With OSCAIRN_GROWING_SYNTH_EXTRA_MB, each sound prepared after activation holds that much more
// than it declares, a plugin the worker's limit must still stop.
#define clap_entry basic_synth_entry
#include "../../../plugins/basic-synth/basic_synth.cpp"
#undef clap_entry
#include <oscairn-clap/memory.h>
#include <map>
#include <memory>
#include <vector>

namespace
{
    constexpr size_t mib = size_t(1) << 20, contextMib = 16;
    struct Held { std::array<std::vector<char>, Synth::contextCapacity> blocks; bool active = false; };
    std::map<const clap_plugin_t*, Held> held; // main thread

    // Each prepared context holds its bytes, touched so they are resident; a discarded one lets them go.
    void sync(const clap_plugin_t* p)
    {
        auto& h = held[p];
        const char* extra = std::getenv("OSCAIRN_GROWING_SYNTH_EXTRA_MB");
        for(uint32_t i = 0; i < Synth::contextCapacity; ++i)
        {
            auto& b = h.blocks[i];
            if(!self(p).contexts[i].prepared) { std::vector<char>().swap(b); continue; }
            if(b.empty()) b.assign((contextMib + (h.active && extra ? std::strtoull(extra, nullptr, 10) : 0)) * mib, 1);
        }
    }
    uint32_t sounds(const clap_plugin_t* p) { uint32_t n = 0; for(const auto& c : self(p).contexts) n += c.prepared; return n; }

    bool estimate(const clap_plugin_t* p, double, uint32_t, oscairn_memory_t* out)
    {
        const auto n = sounds(p);
        *out = {mib + contextMib * mib * (n ? n - 1 : 0), contextMib * mib, 0};
        return true;
    }
    const oscairn_plugin_memory_t memory{estimate, [](const clap_plugin_t* p, oscairn_memory_t* out) { return estimate(p, 0, 0, out); }};

    oscairn_plugin_sound_contexts_t growingContexts = soundContexts;
    oscairn_plugin_sound_snapshots_t growingSnapshots = soundSnapshots;
    clap_plugin_state_t growingState = snapshot;

    const clap_plugin_t* createGrowing()
    {
        growingContexts.prepare = [](const clap_plugin_t* p, const char* name, const oscairn_sound_parameter_t* v, uint32_t n, const char* patch,
                                     const oscairn_asset_ref_t* base, char* message, uint32_t capacity) {
            const auto id = soundContexts.prepare(p, name, v, n, patch, base, message, capacity); sync(p); return id; };
        growingContexts.prepare_preset = [](const clap_plugin_t* p, const char* name, uint32_t kind, const char* location, const char* key, char* message, uint32_t capacity) {
            const auto id = soundContexts.prepare_preset(p, name, kind, location, key, message, capacity); sync(p); return id; };
        growingContexts.discard = [](const clap_plugin_t* p, uint32_t id) { const bool ok = soundContexts.discard(p, id); sync(p); return ok; };
        growingState.load = [](const clap_plugin_t* p, const clap_istream_t* in) { const bool ok = snapshot.load(p, in); sync(p); return ok; };
        growingContexts.state = &growingState;
        growingSnapshots.prepare = [](const clap_plugin_t* p, const clap_istream_t* in, uint32_t* selected, char* message, uint32_t capacity) {
            const bool ok = soundSnapshots.prepare(p, in, selected, message, capacity); sync(p); return ok; };
        auto* plugin = const_cast<clap_plugin_t*>(create());
        held[plugin];
        static void (*destroy)(const clap_plugin_t*) = plugin->destroy;
        static bool (*activate)(const clap_plugin_t*, double, uint32_t, uint32_t) = plugin->activate;
        static const void* (*extension)(const clap_plugin_t*, const char*) = plugin->get_extension;
        plugin->destroy = [](const clap_plugin_t* p) { held.erase(p); destroy(p); };
        plugin->activate = [](const clap_plugin_t* p, double rate, uint32_t lo, uint32_t hi) {
            const bool ok = activate(p, rate, lo, hi); sync(p); held[p].active = ok; return ok; };
        plugin->get_extension = [](const clap_plugin_t* p, const char* id) -> const void* {
            if(!std::strcmp(id, OSCAIRN_EXT_MEMORY)) return &memory;
            if(!std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS)) return &growingContexts;
            if(!std::strcmp(id, OSCAIRN_EXT_SOUND_SNAPSHOTS)) return &growingSnapshots;
            return extension(p, id); };
        return plugin;
    }
    const clap_plugin_factory_t growingFactory = {
        [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
        [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* { return index == 0 ? &descriptor : nullptr; },
        [](const clap_plugin_factory_t*, const clap_host_t*, const char* id) -> const clap_plugin_t* { return std::strcmp(id, descriptor.id) == 0 ? createGrowing() : nullptr; },
    };
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char*) { return true; },
    []() {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &growingFactory : nullptr; },
};

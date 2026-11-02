// SPDX-License-Identifier: MIT
//
// Part of the Oscairn plugin SDK (MIT, like the extension headers beside it).
//
// CLAP preset discovery for a plugin: the factory and one provider, so a plugin says only where
// its presets are and what each is called. See docs/rfcs/0005-sound-contexts.md#presets.
//
// Presets (the plugin's own type) provides, all static:
//   static const clap_preset_discovery_provider_descriptor_t descriptor;  its provider
//   static const char* const pluginId;                                    the plugin its presets are for
//   static void declare(oscairn::presets::Declarations&)                  where they are:
//       d.inside("Factory") for presets inside the plugin, d.files("Patches", folder, "fxp") for files
//   static void list(uint32_t kind, const char* location, oscairn::presets::Listing&)
//       the presets in one location: l.add(name, loadKey) for each, until it returns false
// and the plugin's clap_entry.get_factory returns &oscairn::presets::Factory<Presets>::factory for
// CLAP_PRESET_DISCOVERY_FACTORY_ID. A preset is then loaded by its (kind, location, loadKey):
// see Engine::preparePreset in sound-contexts-plugin.hpp.

#pragma once
#include <clap/clap.h>
#include <clap/factory/preset-discovery.h>

#include <cstring>

namespace oscairn::presets
{
    struct Declarations
    {
        const clap_preset_discovery_indexer_t* indexer;
        bool ok = true;
        // Presets inside the plugin; their load keys tell them apart.
        void inside(const char* name, uint32_t flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT)
        {
            const clap_preset_discovery_location_t l{flags, name, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr};
            ok = indexer->declare_location(indexer, &l) && ok;
        }
        // A file, or a folder searched for files ending ".extension" (null or empty: every file).
        void files(const char* name, const char* path, const char* extension, uint32_t flags = CLAP_PRESET_DISCOVERY_IS_FACTORY_CONTENT)
        {
            const clap_preset_discovery_filetype_t t{name, nullptr, extension};
            const clap_preset_discovery_location_t l{flags, name, CLAP_PRESET_DISCOVERY_LOCATION_FILE, path};
            ok = indexer->declare_filetype(indexer, &t) && indexer->declare_location(indexer, &l) && ok;
        }
    };

    struct Listing
    {
        const clap_preset_discovery_metadata_receiver_t* receiver;
        const char* pluginId;
        bool more = true;
        // A preset in this location. The load key may be null for a file that is one preset.
        bool add(const char* name, const char* loadKey = nullptr)
        {
            if(!more || !(more = receiver->begin_preset(receiver, name, loadKey))) return false;
            const clap_universal_plugin_id_t id{"clap", pluginId};
            receiver->add_plugin_id(receiver, &id);
            return true;
        }
        void error(const char* message) { receiver->on_error(receiver, 0, message); }
    };

    template<class Presets>
    struct Factory
    {
        struct Provider
        {
            clap_preset_discovery_provider_t provider;
            const clap_preset_discovery_indexer_t* indexer;
        };
        static const Provider& of(const clap_preset_discovery_provider_t* p) { return *static_cast<const Provider*>(p->provider_data); }

        static const clap_preset_discovery_provider_t* create(const clap_preset_discovery_factory_t*, const clap_preset_discovery_indexer_t* indexer, const char* id)
        {
            if(!indexer || !id || std::strcmp(id, Presets::descriptor.id)) return nullptr;
            auto* p = new Provider{{}, indexer};
            p->provider.desc = &Presets::descriptor;
            p->provider.provider_data = p;
            p->provider.init = [](const clap_preset_discovery_provider_t* self) {
                Declarations d{of(self).indexer}; Presets::declare(d); return d.ok;
            };
            p->provider.destroy = [](const clap_preset_discovery_provider_t* self) { delete &of(self); };
            p->provider.get_metadata = [](const clap_preset_discovery_provider_t*, uint32_t kind, const char* location, const clap_preset_discovery_metadata_receiver_t* r) {
                if(!r || (kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE && !location)) return false;
                Listing l{r, Presets::pluginId}; Presets::list(kind, location, l); return true;
            };
            p->provider.get_extension = [](const clap_preset_discovery_provider_t*, const char*) -> const void* { return nullptr; };
            return &p->provider;
        }
        static constexpr clap_preset_discovery_factory_t factory{
            [](const clap_preset_discovery_factory_t*) -> uint32_t { return 1; },
            [](const clap_preset_discovery_factory_t*, uint32_t index) { return index ? nullptr : &Presets::descriptor; },
            create};
    };
}

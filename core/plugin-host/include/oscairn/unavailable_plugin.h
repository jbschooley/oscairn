// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/plugin_host.h"
#include <memory>
#include <string>
#include <vector>

namespace oscairn
{
    /// A plugin that is not here (RFC 0006: unavailable-node preservation): not installed, a
    /// version other than the one saved, or one that failed to load. It stands in its place
    /// with the saved identity (descriptor, so it saves under the same format, ID and version),
    /// ports and parameters, so the Performance's wires, parts, scenes and controls keep it; it
    /// plays silence and says why (failure()); its saved state is kept byte for byte and written
    /// back as it was, so installing the plugin later restores it.
    struct UnavailablePlugin
    {
        kick::clap::Descriptor descriptor;                   // as saved: path, ID, name, version
        std::vector<kick::clap::Port> inputs, outputs;
        uint32_t noteInputs = 0, noteOutputs = 0;
        std::vector<kick::clap::Parameter> parameters;       // those the saved snapshot and the document name
        std::string reason;                                  // why it is not here
    };
    std::shared_ptr<PluginHost> unavailablePlugin(UnavailablePlugin plugin);
}

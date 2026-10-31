// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <map>
#include <string>
#include <vector>

namespace oscairn::standalone
{
    // What the Parts and Part views show (RFC 0011), read from the graph state, and the edits
    // they make. Every edit commits, so it is heard at once.
    struct PluginChoice { std::string name, path, id; };
    struct PartBox { NodeId node = 0; std::string name, plugin, role; };
    struct PartRow
    {
        uint32_t id = 0;
        std::string name, sound, input; // sound: the first instrument's plugin; input: "Any device · C3–C8"
        bool bus = false, notes = false; // notes: it plays from MIDI inputs and the panel
        std::vector<DeviceRule> devices; // which inputs (RFC 0012); none: any
        int lowKey = 0, highKey = 127, transpose = 0; // the input's first zone, or all keys
        std::vector<PartBox> chain;
        double level = 1;
        bool mute = false;
        uint32_t through = 0;            // the bus it plays through, or 0 for Main
        std::map<uint32_t, double> sends; // bus -> level
    };
    struct PartsState { std::vector<PartRow> parts, buses; };

    PartsState readParts(const Control::GraphState&);
    // A node's role from its ports: Notes, Instrument, Effect, Sequencer, Generator or Analyzer.
    std::string roleOf(const Graph::NodeInfo&);
    // The plugins a part can add from a folder, found by the isolated scanner `helper`. Blocks; run
    // it off the UI thread.
    std::vector<PluginChoice> bundledPlugins(const std::string& folder, const std::string& helper);

    uint32_t addPart(Control&, const std::string& name, bool bus);
    void insertPlugin(Control&, uint32_t part, size_t index, const PluginChoice&); // a new node, named uniquely
    void removeBox(Control&, uint32_t part, NodeId node);                         // the node, reconnecting its user wires
    void setZone(Control&, uint32_t part, int lowKey, int highKey, int transpose);
    // Device selectors, as the Part page shows and picks them: "Built in + KeyLab 61".
    std::string devicesName(const std::vector<DeviceRule>&);
    DeviceRule ruleFor(const MidiEndpoint&);      // that device by name, or its port number
    std::vector<DeviceRule> toggled(std::vector<DeviceRule>, const DeviceRule&); // All clears the list
    void setDevices(Control&, uint32_t part, const std::vector<DeviceRule>&);
    void setOutput(Control&, uint32_t part, uint32_t bus);                         // 0: Main
    void setSendLevel(Control&, uint32_t part, uint32_t bus, double level);        // 0 removes the send
}

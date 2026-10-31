// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace oscairn::standalone
{
    // What the Parts and Part views show (RFC 0011), read from the graph state, and the edits
    // they make. Every edit commits, so it is heard at once.
    struct PluginChoice { std::string name, path, id; };
    struct PartBox;
    // A split's lane: its boxes, its key range (its first zone, or all keys) and its Mix.
    struct PartLane
    {
        std::string name; // Left, Right, Mid, Side or Band n for the kinds that divide the sound
        std::vector<PartBox> boxes;
        int lowKey = 0, highKey = 127;
        double level = 1;
        bool mute = false, solo = false;
    };
    // A box in a chain: a node, or (split nonzero) a split with its lanes, named after them.
    struct PartBox
    {
        NodeId node = 0; std::string name, plugin, role;
        uint32_t split = 0;
        Split::Kind kind = Split::Kind::Copy;
        std::string splitter;             // its Split node's name, for its crossovers
        std::vector<PartLane> lanes;
    };
    const PartBox* findBox(const std::vector<PartBox>& chain, NodeId node); // anywhere, lanes included
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
    void insertPlugin(Control&, uint32_t part, size_t index, const PluginChoice&, ChainPlace = {}); // a new node, named uniquely
    // Two empty lanes; kinds other than copy add their Split and Join nodes from `plugins`.
    uint32_t insertSplit(Control&, uint32_t part, size_t index, Split::Kind kind = Split::Kind::Copy, const std::vector<PluginChoice>& plugins = {});
    // Removing a split keeps its first lane's boxes; a removed lane's boxes are removed with it.
    void removeSplitKeepingFirst(Control&, uint32_t part, uint32_t split);
    void removeLaneAndBoxes(Control&, uint32_t part, uint32_t split, uint32_t lane);
    void editLane(Control&, uint32_t part, uint32_t split, uint32_t lane, const std::function<void(Lane&)>& change);
    void setLaneKeys(Control&, uint32_t part, uint32_t split, uint32_t lane, int lowKey, int highKey);
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

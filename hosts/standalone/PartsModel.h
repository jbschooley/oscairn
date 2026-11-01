// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <functional>
#include <map>
#include <set>
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
        double level = 1, pan = 0;
        bool mute = false, solo = false;
    };
    // A box in a chain: a node, or (split nonzero) a split with its lanes, named after them.
    struct PartBox
    {
        NodeId node = 0; std::string name, plugin, role;
        uint32_t split = 0;
        bool userWires = false;           // a wire drawn in Routing touches it: a badge, and Routing...
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
        int maxNotes = 0;                // notes at once (RFC 0018); 0: no limit
        int lowKey = 0, highKey = 127, transpose = 0; // the input's first zone, or all keys
        int lowVelocity = 0, highVelocity = 127;      // its velocities, as MIDI values
        std::vector<PartBox> chain;
        double level = 1, pan = 0;
        bool mute = false, solo = false;
        bool silenced = false;           // another part's solo silences it
        uint32_t through = 0;            // the bus it plays through, or 0 for Main
        std::map<uint32_t, double> sends; // bus -> level
        std::vector<std::string> exits;  // "→ Hall (shared)": nodes outside it its boxes are wired to in Routing
        bool edited = false;             // loaded from a library Sound and changed since (RFC 0021)
        bool draining = false;           // its notes in drain: being switched out, notes held still sounding
    };
    // A node outside every part that several parts are wired into in Routing: a bus in all but
    // name (RFC 0011), shown as a row under the buses.
    struct SharedRow { NodeId node = 0; std::string name; std::vector<std::string> fedBy; };
    struct PartsState { std::vector<PartRow> parts, buses; std::vector<SharedRow> shared; };

    // The Routing view (RFC 0011 step 4): the real graph as nodes in columns, and its wires.
    // Columns: 0 inputs (the keys and MIDI, each audio input), 1 note processing, 2 instruments,
    // 3 effects and buses, 4 outputs. The graph's input and output appear once per port; `port`
    // is notesPort for the keys and MIDI.
    struct RoutingNode
    {
        NodeId node = 0;
        uint32_t port = 0;
        std::string name, role;
        uint32_t part = 0;     // the part or bus it belongs to; 0: none
        std::string partName;
        bool userWires = false, splitNode = false;
    };
    constexpr uint32_t notesPort = 0xFFFFFFFFu;
    // One source into a node's input: a row of that input's implicit Mix.
    struct MixRow { uint32_t wire = 0; std::string source; double level = 1, pan = 0; bool mute = false, solo = false, user = false, editable = true; };
    struct RoutingInput { uint32_t port = 0; std::string name; std::vector<MixRow> rows; };
    struct RoutingState
    {
        std::vector<RoutingNode> nodes;
        std::vector<Wire> wires;
        const RoutingNode* find(NodeId node, uint32_t port) const; // the ends by port, other nodes by node
    };
    RoutingState readRouting(const Control::GraphState&);
    // How a wire is drawn: enabled; disabled (muted; with RFC 0011's full scene model, also off in
    // the scene); or draining (a note wire taking no new notes while held ones ring out, RFC 0021).
    // A wire that no longer fits its plugin's ports (Wire::connected false) is drawn apart.
    enum class WireState { Enabled, Disabled, Draining };
    WireState wireState(const Wire&, const Graph::Description&);
    // How many channels an audio wire carries: the source channels its pairs take, or else all its
    // source port's (Routing draws a line each); 1 for a note wire or an unknown port.
    uint32_t wireChannels(const Wire&, const Graph::Description&);

    // The Routing view's layout (layered by signal flow): each box one column after what feeds
    // it, inputs first and outputs last; a wire skipping columns keeps a slot in each it crosses;
    // each column ordered to cross fewer wires; each wire leaving and entering at its own place on
    // a box's edge. Curves run only in the gaps between columns, so no wire passes under a box.
    // A collapsed part is one box with its plugins as chips; an expanded part is a frame holding
    // its plugins, laid out the same way inside it. Pixels, from (0, 0).
    struct RoutingRect { int x = 0, y = 0, w = 0, h = 0; bool contains(double px, double py) const { return px > x && px < x + w && py > y && py < y + h; } };
    struct RoutingPoint { double x = 0, y = 0; };
    struct RoutingLayout
    {
        struct Box
        {
            enum class Kind { Node, Part, Frame } kind = Kind::Node;
            NodeId node = 0; uint32_t port = 0; // a node, or a graph input or output port
            uint32_t part = 0;                  // a part's box or frame, or the part a node is in
            std::string name, detail;
            std::vector<std::string> chips;     // a collapsed part's plugins, in chain order
            RoutingRect bounds;
        };
        struct Path { Wire wire; WireState state = WireState::Enabled; std::vector<RoutingPoint> points; };
        std::vector<Box> boxes; // frames first, then what is drawn over them
        std::vector<Path> paths;
        int width = 0, height = 0;
    };
    struct RoutingMetrics { int node = 170, nodeHeight = 44, part = 280, partHeight = 64, gap = 56, row = 16, slot = 10, frame = 14, frameHead = 40; };
    // collapsed: parts are one box each, but those in `expanded`; otherwise every part is a frame.
    RoutingLayout layoutRouting(const Control::GraphState&, bool collapsed, const std::set<uint32_t>& expanded, const RoutingMetrics& = {});
    // Points along a path as drawn: a curve through its points, level at each one.
    std::vector<RoutingPoint> routingCurve(const std::vector<RoutingPoint>& points, int steps = 16);
    // A node's audio inputs that have sources, with a row per source (its Mix), and its notes.
    std::vector<RoutingInput> inputsOf(const Control::GraphState&, NodeId node);

    PartsState readParts(const Control::GraphState&);
    // The Part page's settings as data (RFC 0019's view model): which a part has, in order, and
    // how many grid columns each takes (0: the whole row). Every surface lays out the same list.
    struct PartField
    {
        enum class Kind { Devices, NoteLimit, Zone, Transpose, Output, Level, Send };
        Kind kind;
        std::string label;
        int span = 1;
        uint32_t bus = 0; // Send: the bus
    };
    std::vector<PartField> partFields(const PartRow&, const PartsState&);
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
    void setVelocities(Control&, uint32_t part, int low, int high); // MIDI values 0 to 127 the part plays
    // Device selectors, as the Part page shows and picks them: "Built in + KeyLab 61".
    std::string devicesName(const std::vector<DeviceRule>&);
    DeviceRule ruleFor(const MidiEndpoint&);      // that device by name, or its port number
    std::vector<DeviceRule> toggled(std::vector<DeviceRule>, const DeviceRule&); // All clears the list
    void setDevices(Control&, uint32_t part, const std::vector<DeviceRule>&);
    void setMaxNotes(Control&, uint32_t part, int maxNotes); // 0: no limit
    void setOutput(Control&, uint32_t part, uint32_t bus);                         // 0: Main
    void setSendLevel(Control&, uint32_t part, uint32_t bus, double level);        // 0 removes the send
    // Routing's edits: user wires and nodes outside the parts.
    void connectNodes(Control&, NodeId from, NodeId to, WireKind);                 // main port to main port
    void disconnect(Control&, uint32_t wire);
    void setWireMix(Control&, uint32_t wire, double level, double pan, bool mute, bool solo);
    NodeId addLoose(Control&, const PluginChoice&);                                // a node in no part
    void removeLoose(Control&, NodeId node);                                       // reconnecting its user wires
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace oscairn
{
    // RFC 0011 step 1: the model behind the part views. The graph stays the only signal model;
    // these record who made each wire, the parts' chains, and each input's implicit Mix. The
    // Session compiles wires into graph routes.

    enum class WireKind { Audio, Notes };

    // A wire end whose port a plugin replacement could not map: the main port once the node has
    // one again, or no port at all.
    constexpr uint32_t unmappedMainPort = 0xFFFFFFFEu, unmappedPort = 0xFFFFFFFFu;
    enum class WireOwner { Part, User }; // made by the part views, or drawn in Routing

    struct Wire
    {
        uint32_t id = 0;
        WireKind kind = WireKind::Audio;
        WireOwner owner = WireOwner::User;
        uint32_t part = 0;                  // the part that made it, for part wires
        NodeId from = 0, to = 0;
        uint32_t fromPort = 0, toPort = 0;  // stable audio port IDs; unused for notes
        // Explicit channel pairs (from, to), as a channel picker sets them; empty applies the
        // channel rules: equal counts one to one, mono to stereo copied with pan, stereo to
        // mono summed at -3 dB, anything else needs pairs.
        std::vector<std::pair<uint32_t, uint32_t>> channels;
        // This source's controls in the destination input's implicit Mix. Pan balances a stereo
        // destination.
        double level = 1, pan = 0;
        bool mute = false, solo = false;
        std::vector<NoteZone> zones; // note wires: the zones notes must pass (note_zone.h); none passes all
        uint32_t send = 0;           // part wires: the bus this wire sends to, if it is a send
        uint32_t split = 0, lane = 0; // part wires: the split lane this wire joins from, or enters in a bus (its Mix), if any
        bool fader = false;          // part wires: where the part plays, carrying its level and mute
        bool connected = true; // false: kept, though it no longer fits (after a plugin replacement)
    };

    // RFC 0012 selectors. A MIDI input the host has: its source number (note_zone.h), name and
    // kind; port is the number of a DIN or TRS port.
    struct MidiEndpoint
    {
        enum class Kind { BuiltIn, Port, Usb, Network } kind = Kind::Usb;
        uint8_t source = 0;
        std::string name;
        uint32_t port = 0;
        bool operator==(const MidiEndpoint&) const = default;
    };

    // The names files and the control protocol use, indexed by the enums.
    inline constexpr std::array<const char*, 4> endpointKinds{"builtIn", "port", "usb", "network"};
    inline constexpr std::array<const char*, 7> deviceRuleKinds{"builtIn", "external", "all", "port", "usb", "usbNamed", "network"};
    inline constexpr std::array<const char*, 3> deviceMatches{"equals", "contains", "regex"};

    // A rule choosing MIDI inputs: by kind, port number, or name (USB named; Network with a
    // pattern). Names match ignoring case.
    struct DeviceRule
    {
        enum class Kind { BuiltIn, External, All, Port, Usb, UsbNamed, Network } kind = Kind::All;
        enum class Match { Equals, Contains, Regex } match = Match::Contains;
        uint32_t port = 0;
        std::string pattern;
        bool operator==(const DeviceRule&) const = default;
        bool valid() const; // a usable pattern where one is needed
        bool matches(const MidiEndpoint&) const;
    };
    // The sources a selector chooses, as a zone's source mask: everything for no rules. Built in
    // always includes the panel's own notes (builtInSource).
    uint64_t resolveDevices(const std::vector<DeviceRule>& rules, const std::vector<MidiEndpoint>& endpoints);

    struct PartInput
    {
        // None: the chain generates (a sequencer). Bus: a shared bus, fed by other parts' sends and outputs.
        enum class Kind { Notes, Audio, None, Bus } kind = Kind::Notes;
        uint32_t port = 0;                                         // graph input port, for audio
        std::vector<NoteZone> zones;                               // notes: the part's zones (splits, layers, curves)
        std::vector<DeviceRule> devices;                           // notes: which inputs play it; none: all
    };
    // The zones a part's input wire carries: its own, each hearing only its selected devices.
    std::vector<NoteZone> inputZones(const PartInput& input, const std::vector<MidiEndpoint>& endpoints);

    struct Send { uint32_t bus = 0; double level = 1; }; // a copy of a part's sound into a shared bus

    // RFC 0011 step 3: a split divides a part's notes and sound into lanes, each its own chain,
    // which join again by summing into whatever follows. In a chain, the entry splitMark | id
    // stands for the part's split with that ID; a lane's chain may hold another split.
    constexpr NodeId splitMark = 0x80000000u;
    constexpr bool isSplit(NodeId entry) { return (entry & splitMark) != 0; }
    struct Lane
    {
        std::vector<NodeId> chain;
        std::vector<NoteZone> zones;        // the notes this lane takes; none: all
        double level = 1, pan = 0;          // its row in the join's Mix
        bool mute = false, solo = false;    // solo silences the split's other lanes
    };
    struct Split
    {
        uint32_t id = 0;
        // Copy: every lane gets the notes (through its zones) and a copy of the sound. The others
        // divide the sound with an Oscairn Split node (plugins/splitters), lane k taking its output
        // k: left/right and mid/side (two lanes, joined again by an Oscairn Join node), and bands
        // (two to four lanes, joining by summing into what follows). Every lane gets the notes.
        enum class Kind { Copy, LeftRight, MidSide, Bands } kind = Kind::Copy;
        std::vector<Lane> lanes;
        NodeId splitter = 0, joiner = 0; // the Split and Join nodes, for the kinds that have them
    };
    inline constexpr std::array<const char*, 4> splitKinds{"copy", "leftRight", "midSide", "bands"};
    inline constexpr const char* splitPlugin = "org.oscairn.split";
    inline constexpr const char* joinPlugin = "org.oscairn.join";
    // Where a chain edit applies: the part's own chain (split 0), or one lane of a split.
    struct ChainPlace { uint32_t split = 0; uint32_t lane = 0; };
    // The two zone lists a note passes in turn, as one: each pair that lets some note through.
    std::vector<NoteZone> intersectZones(const std::vector<NoteZone>& first, const std::vector<NoteZone>& then);

    // An input, the chain of nodes it reaches in order, and where it plays: a graph output port,
    // or through a shared bus. Sends copy its sound into buses as well.
    struct Part
    {
        uint32_t id = 0;
        std::string name;
        PartInput input;
        std::vector<NodeId> chain; // nodes and split entries, in order
        std::vector<Split> splits;
        uint32_t output = 0; // graph output port ID, when it plays to no bus
        uint32_t bus = 0;    // the bus it plays through instead, or 0
        std::vector<Send> sends;
        double level = 1;    // its fader: where it plays and its sends (post-fader)
        bool mute = false;   // silences where it plays and its sends
    };

    // The control tree (RFC 0011): a control (a macro, Assign knob or Super Knob) is a value from
    // 0 to 1 that drives its targets, each with its own range and curve; a target may be another
    // control. Physical knobs, MIDI CC and scenes set controls.
    struct MacroTarget
    {
        enum class Kind { Parameter, PartLevel, PartMute, Macro } kind = Kind::Parameter;
        enum class Curve { Linear, Exponential, Logarithmic, SCurve } curve = Curve::Linear;
        NodeId node = 0; uint32_t param = 0; // Parameter
        uint32_t part = 0;                   // PartLevel, PartMute
        uint32_t macro = 0;                  // Macro
        // The range the control's 0..1 covers, as fractions of the target's range; end below
        // start reverses it. Relative targets move around `base` (the value when assigned) by
        // half the range either way instead.
        double start = 0, end = 1;
        bool relative = false;
        double base = 0;
        // What a control value does here, as a fraction of the target's range.
        double map(double value) const;
    };
    struct Macro { uint32_t id = 0; std::string name; double value = 0; std::vector<MacroTarget> targets; };

    // A scene: a state of the Performance for a section of a song. It records every part's level
    // and mute, and a value for each scene parameter (the plugin parameters scenes control).
    // Selecting one applies them without rebuilding anything, so held notes and tails go on.
    struct SceneParameter { NodeId node = 0; uint32_t param = 0; bool operator==(const SceneParameter&) const = default; };
    struct Scene
    {
        struct PartState { uint32_t part = 0; double level = 1; bool mute = false; };
        struct Value { SceneParameter at; double value = 0; };
        uint32_t id = 0;
        std::string name;
        std::vector<PartState> parts;
        std::vector<Value> values;
        std::vector<std::pair<uint32_t, double>> macros; // each control's value
    };

    // What removing a node does with the user wires on it.
    enum class UserWires { Remove, Reconnect }; // Reconnect joins its user sources to its user destinations
}

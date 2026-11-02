// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"

#include <array>
#include <optional>
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
        bool enabled = true;   // false: off (scenes): a note wire takes no new notes, an audio wire is silent
        bool operator==(const Wire&) const = default;
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
        uint16_t maxNotes = 0;                                     // notes at once (RFC 0018), 0..128; 0: no limit
        bool operator==(const PartInput&) const = default;
    };
    // The zones a part's input wire carries: its own, each hearing only its selected devices,
    // with its note limit.
    std::vector<NoteZone> inputZones(const PartInput& input, const std::vector<MidiEndpoint>& endpoints);

    // RFC 0012 step 1: a part's MIDI to hardware outputs: the outputs its selector chooses (none:
    // no output; rules as for inputs, matched against the host's MIDI outputs), on `channel` (1 to
    // 16; 0 keeps each message's own). What goes is what the part's instruments are played with
    // (its notes through its zones, a sequencer's or arpeggiator's notes, the controllers that
    // reach them), or with no instrument, what leaves the end of its chain.
    struct PartMidiOut
    {
        std::vector<DeviceRule> devices;
        uint8_t channel = 0;
        bool operator==(const PartMidiOut&) const = default;
    };

    // RFC 0033: which notes belong to the key, not how they are tuned: a root (0 = C .. 11 = B)
    // and a scale as pitch classes above it, bit n for n semitones up (bit 0, the root, always).
    // The Performance has one; a part may override it. Default: C major.
    struct MusicalKey
    {
        uint8_t root = 0;
        uint16_t scale = 0xAB5;
        bool operator==(const MusicalKey&) const = default;
        bool valid() const { return root < 12 && scale < 4096 && (scale & 1); }
    };

    struct Send { uint32_t bus = 0; double level = 1; bool operator==(const Send&) const = default; }; // a copy of a part's sound into a shared bus

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
        bool operator==(const Lane&) const = default;
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
        bool operator==(const Split&) const = default;
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
    // Where a part came from (RFC 0021): the library Sound and the revision loaded into it, its
    // name then, and whether the part has been edited since. Empty when it came from none.
    struct PartSource
    {
        std::string sound, revision, name;
        bool modified = false;
        bool operator==(const PartSource&) const = default;
    };
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
        double pan = 0;      // where it plays, -1 left to 1 right; its sends keep their own
        bool mute = false;   // silences where it plays and its sends
        // While any part is soloed, only soloed parts, the parts that feed them and the buses
        // they feed are heard.
        bool solo = false;
        PartSource from;
        PartMidiOut midiOut;
        std::optional<MusicalKey> key; // its own key and scale; none: the Performance's (RFC 0033)
        bool operator==(const Part&) const = default;
    };
    // A part's meter, after its fader and pan, linear: one value per channel of its sound (its last
    // plugin's main audio output as wired to where it plays: one for mono, two for stereo, more
    // for surround), and the largest of them.
    struct PartMeter { uint32_t part = 0; double peak = 0; std::vector<double> channels; };
    // An audio wire's meter, after its level and pan, linear: one value per channel its source's
    // port sends, and the largest of them.
    struct WireMeter { uint32_t wire = 0; double peak = 0; std::vector<double> channels; };

    // The control tree (RFC 0011): a control (a macro, Assign knob or Super Knob) is a value from
    // 0 to 1 that drives its targets, each with its own range and curve; a target may be another
    // control. Physical knobs, MIDI CC and scenes set controls.
    struct MacroTarget
    {
        enum class Kind { Parameter, PartLevel, PartMute, Macro, Morph } kind = Kind::Parameter;
        // Db: evenly in decibels from `low` at 0 to `high` at 1, as a gain, and 0 at 0 (a fader's
        // taper); Power: the value to the power `exponent`.
        enum class Curve { Linear, Exponential, Logarithmic, SCurve, Db, Power } curve = Curve::Linear;
        NodeId node = 0; uint32_t param = 0; // Parameter
        uint32_t part = 0;                   // PartLevel, PartMute
        uint32_t macro = 0;                  // Macro
        uint32_t sceneFrom = 0, sceneTo = 0; // Morph: the scenes the control moves between, from at 0 to at 1
        // The range the control's 0..1 covers, as fractions of the target's range; end below
        // start reverses it. Relative targets move around `base` by half the range either way.
        double start = 0, end = 1;
        bool relative = false;
        // Parameters, part levels and mutes: the destination's value with no control moving it,
        // the same on every target of that destination (RFC 0011's summing rule). Each target
        // adds its offset around it, and the sum is clamped to the range.
        double base = 0;
        std::vector<uint32_t> offIn; // the scenes this mapping is off in (RFC 0011); on in every other
        double low = -96, high = 0; // Db, in dB: -144 <= low < high <= 0
        double exponent = 2;        // Power: 0.1 to 10
        double shape(double value) const;  // the curve: 0..1 to 0..1
        bool validCurve() const;           // its numbers within their ranges
        double offset(double value) const; // what a control value adds to the base
        // What a control value does here alone, as a fraction of the target's range.
        double map(double value) const;
        // Whether several controls' targets sum into this one's destination, and whether two share it.
        bool sums() const { return kind == Kind::Parameter || kind == Kind::PartLevel || kind == Kind::PartMute; }
        bool sameDestination(const MacroTarget& o) const { return sums() && o.kind == kind && o.node == node && o.param == param && o.part == part; }
        bool operator==(const MacroTarget&) const = default;
    };
    inline constexpr std::array<const char*, 6> curveKinds{"linear", "exponential", "logarithmic", "s-curve", "db", "power"}; // as files and the control protocol name them
    // A control belongs to the Performance, or to a part (RFC 0021): a part's controls target only
    // its plugins and its other controls, and go with it into a Sound.
    struct Macro { uint32_t id = 0; std::string name; double value = 0; std::vector<MacroTarget> targets; uint32_t part = 0; };

    // Physical controls (RFC 0011): what a knob, fader or button is assigned to. Defaults come in
    // layers (physical_controls.h); a Performance override sets one by the control's logical
    // identity, "knob:1", "fader:3" or "button:12" (physical_controls.h), and is saved with it.
    struct Assignment
    {
        enum class Kind { None, Parameter, Control, PartLevel, PartMute, Scene } kind = Kind::None;
        NodeId node = 0; uint32_t param = 0; // Parameter
        uint32_t id = 0;                     // Control, PartLevel, PartMute, Scene: the control, part or scene
        bool operator==(const Assignment&) const = default;
    };
    inline constexpr std::array<const char*, 6> assignmentKinds{"none", "parameter", "control", "partLevel", "partMute", "scene"};
    struct AssignmentOverride { std::string control; Assignment target; uint32_t part = 0; bool operator==(const AssignmentOverride&) const = default; }; // part: a part's Sound's own; 0 the Performance's
    bool validPhysicalControl(const std::string& control); // knob, fader, button, wheel, strip, jog, pedal or footswitch, then a colon and 1 to 999

    // A scene: a state of the Performance for a section of a song (RFC 0011's full model). Storing
    // one records all of it: which plugins and wires are off, each part's mute and solo, the mix
    // (each part's level, pan and sends, each lane's Mix, each user wire's Mix row), every plugin
    // parameter scenes touch with the controls' values, and, on the mappings themselves
    // (MacroTarget::offIn), which are off in it. Selecting one applies the kinds in its scope and
    // leaves the rest as they are; nothing is rebuilt that is playing, so held notes and tails go on.
    struct SceneParameter { NodeId node = 0; uint32_t param = 0; bool operator==(const SceneParameter&) const = default; auto operator<=>(const SceneParameter&) const = default; };
    // A wire by what it joins and, for a part's wire, its place in the part: part wires are rebuilt
    // under new IDs as their part changes.
    struct WireRef
    {
        WireKind kind = WireKind::Audio;
        uint32_t part = 0; // 0: a user wire
        NodeId from = 0, to = 0;
        uint32_t fromPort = 0, toPort = 0, send = 0, split = 0, lane = 0;
        bool fader = false;
        static WireRef of(const Wire& w) { return {w.kind, w.owner == WireOwner::Part ? w.part : 0, w.from, w.to, w.fromPort, w.toPort, w.send, w.split, w.lane, w.fader}; }
        bool operator==(const WireRef&) const = default;
    };
    struct Scene
    {
        enum Kind : uint32_t { Enabled = 1, Mutes = 2, Mix = 4, Parameters = 8, Mappings = 16, Everything = 31 };
        // How selecting it moves there: over `glide` milliseconds (0: at once) along `curve`, and
        // when: at once, or at the next beat or bar while the transport plays.
        enum class Curve { Linear, Ease, EqualPower };
        enum class Launch { Now, Beat, Bar };
        struct LaneState { uint32_t split = 0, lane = 0; double level = 1, pan = 0; bool mute = false, solo = false; bool operator==(const LaneState&) const = default; };
        struct PartState
        {
            uint32_t part = 0;
            double level = 1, pan = 0; bool mute = false, solo = false;
            std::vector<Send> sends;
            std::vector<LaneState> lanes;
            bool operator==(const PartState&) const = default;
        };
        struct WireMix { WireRef wire; double level = 1, pan = 0; bool mute = false, solo = false; bool operator==(const WireMix&) const = default; };
        struct Value { SceneParameter at; double value = 0; bool operator==(const Value&) const = default; };
        uint32_t id = 0;
        std::string name;
        uint32_t scope = Everything;     // the kinds selecting it applies
        uint32_t glide = 0;              // milliseconds
        Curve curve = Curve::Linear;
        Launch launch = Launch::Now;
        std::vector<NodeId> off;         // Enabled: the plugins off
        std::vector<WireRef> offWires;   // Enabled: the wires off
        std::vector<PartState> parts;    // Mutes (mute, solo) and Mix (the rest)
        std::vector<WireMix> wires;      // Mix: user wires' Mix rows
        std::vector<Value> values;       // Parameters
        std::vector<std::pair<uint32_t, double>> macros; // Parameters: each control's value
        bool operator==(const Scene&) const = default;
    };
    inline constexpr std::array<const char*, 5> sceneKinds{"enabled", "mutes", "mix", "parameters", "mappings"}; // the scope's names, bit by bit
    inline constexpr std::array<const char*, 3> sceneCurves{"linear", "ease", "equal-power"}, sceneLaunches{"now", "beat", "bar"};

    // What removing a node does with the user wires on it.
    enum class UserWires { Remove, Reconnect }; // Reconnect joins its user sources to its user destinations
}

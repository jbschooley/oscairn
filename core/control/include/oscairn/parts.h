// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"

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
        bool connected = true; // false: kept, though it no longer fits (after a plugin replacement)
    };

    struct PartInput
    {
        enum class Kind { Notes, Audio, None } kind = Kind::Notes; // None: the chain generates (a sequencer)
        uint32_t port = 0;                                         // graph input port, for audio
        std::vector<NoteZone> zones;                               // notes: the part's zones (splits, layers, curves)
    };

    // An input, the chain of nodes it reaches in order, and the graph output port it plays to.
    struct Part
    {
        uint32_t id = 0;
        std::string name;
        PartInput input;
        std::vector<NodeId> chain;
        uint32_t output = 0; // graph output port ID
    };

    // What removing a node does with the user wires on it.
    enum class UserWires { Remove, Reconnect }; // Reconnect joins its user sources to its user destinations
}

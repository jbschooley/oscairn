// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 2's gate through the Parts views' model: a two-layer keyboard split and a
// shared delay, built only with the edits the views make, with the plugins the isolated scanner
// finds. The views read back what they show.
//   oscairn-parts-model-test PLUGIN_DIR SCANNER
#include "../PartsModel.h"
#include "oscairn/session.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace oscairn;
using namespace oscairn::standalone;

int main(int argc, char** argv)
{
    if(argc < 3) return 2;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    try
    {
        const auto plugins = bundledPlugins(argv[1], argv[2]);
        auto find = [&](const std::string& name) {
            const auto it = std::find_if(plugins.begin(), plugins.end(), [&](const PluginChoice& p) { return p.name == name; });
            if(it == plugins.end()) throw std::runtime_error("the scanner did not find " + name);
            return *it;
        };
        const auto synth = find("Basic Synth"), delay = find("Delay"), transpose = find("Transpose");

        Session s(48000, 128);
        const auto bass = addPart(s, "Bass", false), lead = addPart(s, "Lead", false), room = addPart(s, "Room", true);
        insertPlugin(s, bass, 0, synth); insertPlugin(s, lead, 0, synth); insertPlugin(s, room, 0, delay);
        insertPlugin(s, bass, 0, transpose); // a note effect in front
        setZone(s, bass, 0, 59, 0); setZone(s, lead, 60, 127, 0);
        setSendLevel(s, bass, room, .5); setSendLevel(s, lead, room, .3);

        const auto state = readParts(s.graphState());
        check(state.parts.size() == 2 && state.buses.size() == 1, "two parts and a bus");
        const auto& b = state.parts[0];
        check(b.name == "Bass" && b.input == "Any device · C-1–B3" && b.sound == "Basic Synth", "the bass row: " + b.input);
        check(b.chain.size() == 2 && b.chain[0].role == "Notes" && b.chain[1].role == "Instrument" && b.chain[1].name == "Basic Synth", "the bass chain: a note effect, then the synth");
        check(state.parts[1].chain[0].name == "Basic Synth 2", "a second synth gets its own name");
        check(state.parts[1].input == "Any device · C4–G9", "the lead row: " + state.parts[1].input);
        check(state.buses[0].input == "From Bass, Lead" && state.buses[0].chain[0].role == "Effect", "the bus row: " + state.buses[0].input);
        check(b.sends.at(room) == .5 && state.parts[1].sends.at(room) == .3, "the sends read back");
        size_t user = 0; for(const auto& w : s.wires()) user += w.owner == WireOwner::User;
        check(user == 0, "no wire drawn by hand");

        // Both layers sound, each from its own keys, and the bus carries a tail.
        std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
        auto energy = [&](int blocks) { double e = 0; for(int i = 0; i < blocks; ++i) { s.service(); s.process(out, 128); for(float v : l) e += std::abs(v); } return e; };
        energy(4);
        s.note(true, 40, .8); const auto low = energy(40); s.note(false, 40, 0); energy(200);
        s.note(true, 80, .8); const auto high = energy(40); s.note(false, 80, 0);
        check(low > 0 && high > 0, "both layers sound");

        // Removing a box, moving the split, the output and a send.
        removeBox(s, bass, readParts(s.graphState()).parts[0].chain[0].node);
        check(readParts(s.graphState()).parts[0].chain.size() == 1, "the note effect removed");
        setZone(s, lead, 0, 127, 0);
        check(readParts(s.graphState()).parts[1].input == "Any device · all keys" && s.parts()[1].input.zones.empty(), "all keys again: no zone left");
        setOutput(s, lead, room); check(readParts(s.graphState()).parts[1].through == room, "the lead plays through the room");
        setSendLevel(s, bass, room, 0); check(readParts(s.graphState()).parts[0].sends.empty(), "a send at 0 is removed");
        check(roleOf({0, {}, {}, {}, 0, 1}) == "Sequencer", "a node with only note outputs is a sequencer");

        // The Plays from menu: a connected device picked by name, a kind added, Any device clears.
        const MidiEndpoint keylab{MidiEndpoint::Kind::Usb, 3, "KeyLab 61"}, din{MidiEndpoint::Kind::Port, 4, "MIDI 2", 2};
        auto devices = toggled({}, ruleFor(keylab));
        devices = toggled(devices, {DeviceRule::Kind::BuiltIn});
        setDevices(s, lead, devices);
        check(s.parts()[1].input.devices == devices && readParts(s.graphState()).parts[1].input == "KeyLab 61 + Built in · all keys", "picked: " + readParts(s.graphState()).parts[1].input);
        check(toggled(devices, ruleFor(keylab)) == std::vector<DeviceRule>{{DeviceRule::Kind::BuiltIn}}, "picking it again removes it");
        check(toggled(devices, {DeviceRule::Kind::All}).empty() && devicesName(toggled({}, ruleFor(din))) == "Port 2", "Any device clears; a DIN port by number");
        check(devicesName({{DeviceRule::Kind::UsbNamed, DeviceRule::Match::Contains, 0, "key"}, {DeviceRule::Kind::Network}}) == "“key” + Network", "patterns and kinds named");

        // Splits from the Part page: a layer added in a second lane, its keys and level, then
        // removing the lane, and the split, take their plugins with them.
        const auto layers = addPart(s, "Layers", false);
        const auto split = insertSplit(s, layers, 0);
        insertPlugin(s, layers, 0, synth, {split, 0}); insertPlugin(s, layers, 0, synth, {split, 1}); insertPlugin(s, layers, 1, delay, {split, 1});
        setLaneKeys(s, layers, split, 1, 60, 127);
        editLane(s, layers, split, 1, [](Lane& l) { l.level = .5; });
        auto row = readParts(s.graphState()).parts.back();
        check(row.chain.size() == 1 && row.chain[0].split == split && row.chain[0].lanes.size() == 2 && row.chain[0].lanes[1].boxes.size() == 2 &&
              row.chain[0].lanes[1].lowKey == 60 && row.chain[0].lanes[1].level == .5 && row.chain[0].name == "Basic Synth 3 | Basic Synth 4", "the split as the views read it: " + row.chain[0].name);
        check(findBox(row.chain, row.chain[0].lanes[1].boxes[0].node) != nullptr && row.sound == "Basic Synth 3", "boxes in lanes are found; the part's sound is its first instrument");
        const auto nodes = s.graphState().topology.nodes.size();
        removeLaneAndBoxes(s, layers, split, 1);
        check(s.graphState().topology.nodes.size() == nodes - 2 && s.parts().back().splits[0].lanes.size() == 1, "removing a lane removes its plugins");
        const auto kept = s.parts().back().splits[0].lanes[0].chain;
        s.addLane(layers, split); insertPlugin(s, layers, 0, delay, {split, 1});
        removeSplitKeepingFirst(s, layers, split);
        check(s.parts().back().chain == kept && s.parts().back().splits.empty() && s.graphState().topology.nodes.size() == nodes - 2, "removing the split keeps its first lane's plugins in place");

        // A left/right split adds its own Split and Join nodes, which are not boxes; its lanes are named.
        const auto before = s.graphState().topology.nodes.size();
        const auto sides = insertSplit(s, layers, 1, Split::Kind::LeftRight, plugins);
        row = readParts(s.graphState()).parts.back();
        check(s.graphState().topology.nodes.size() == before + 2 && row.chain.size() == 2 && row.chain[1].kind == Split::Kind::LeftRight &&
              row.chain[1].lanes[0].name == "Left" && row.chain[1].lanes[1].name == "Right" && row.chain[1].name == "Left | Right" && !row.chain[1].splitter.empty(),
              "a left/right split as the views read it: " + row.chain[1].name);
        removeSplitKeepingFirst(s, layers, sides);
        check(s.graphState().topology.nodes.size() == before, "removing it removes its nodes");
        bool refused = false; try { insertSplit(s, layers, 0, Split::Kind::Bands, {}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused && s.graphState().topology.nodes.size() == before, "without the Split plugin, nothing is added");
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a split and a shared delay built through the Parts views' model");
    return failures ? 1 : 0;
}

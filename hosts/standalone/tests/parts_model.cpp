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
        // Solo: the other parts read as silenced, which dims them in the Parts list and Mixer.
        s.setPartSolo(lead, true); s.commit();
        auto soloed = readParts(s.graphState());
        check(soloed.parts[0].silenced && !soloed.parts[1].silenced && !soloed.parts[0].mute, "another part's solo silences the bass, without muting it");
        s.setPartSolo(lead, false); s.commit();
        check(!readParts(s.graphState()).parts[0].silenced, "and no longer");

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

        // Routing (RFC 0011 step 4): every node and wire appears; wiring a second source makes a
        // Mix row for it, and a node several parts feed is a shared row in the Parts list.
        const auto wide = insertSplit(s, layers, 0, Split::Kind::LeftRight, plugins);
        auto g = s.graphState();
        auto routing = readRouting(g);
        bool everyNode = true;
        for(const auto& n : g.topology.nodes)
        {
            const auto* r = routing.find(n.id, 0);
            everyNode = everyNode && r && std::count_if(routing.nodes.begin(), routing.nodes.end(), [&](const RoutingNode& x) { return x.node == n.id; }) == 1;
        }
        check(everyNode && routing.wires.size() == g.wires.size(), "every node once, and every wire, in Routing");
        const auto& sp = *std::find_if(g.parts.begin(), g.parts.end(), [&](const Part& p) { return p.id == layers; });
        const auto* splitNode = routing.find(sp.splits[0].splitter, 0);
        check(splitNode && splitNode->splitNode && splitNode->part == layers && splitNode->column == 3, "a split's node belongs to its part, among the effects");
        const auto* keys = routing.find(graphInput, notesPort); const auto* main = routing.find(graphOutput, 0);
        check(keys && keys->column == 0 && main && main->column == 4, "the keys on the left, Main on the right");
        auto byName = [&](const std::string& name) { for(const auto& [id, n] : s.graphState().names) if(n == name) return id; throw std::runtime_error("no node " + name); };
        const auto bassSynth = byName("Basic Synth"), leadSynth = byName("Basic Synth 2");
        check(routing.find(bassSynth, 0)->column == 2 && routing.find(bassSynth, 0)->partName == "Bass", "an instrument in its part's column");
        // Two parts wired by hand into a reverb outside the parts.
        const auto hall = addLoose(s, delay);
        connectNodes(s, bassSynth, hall, WireKind::Audio); connectNodes(s, leadSynth, hall, WireKind::Audio); connectNodes(s, hall, graphOutput, WireKind::Audio);
        auto parts = readParts(s.graphState());
        check(parts.shared.size() == 1 && parts.shared[0].node == hall && parts.shared[0].fedBy == std::vector<std::string>{"Bass", "Lead"}, "a node two parts feed is a shared row");
        check(std::find(parts.parts[0].exits.begin(), parts.parts[0].exits.end(), s.graphState().names.at(hall) + " (shared)") != parts.parts[0].exits.end(), "the feeding part shows where it goes");
        check(findBox(parts.parts[0].chain, bassSynth) && findBox(parts.parts[0].chain, bassSynth)->userWires, "and its box a badge");
        auto inputs = inputsOf(s.graphState(), hall);
        check(inputs.size() == 1 && inputs[0].rows.size() == 2 && inputs[0].rows[0].user && inputs[0].rows[1].user, "the reverb's input: a Mix row per source");
        setWireMix(s, inputs[0].rows[1].wire, .5, -.5, true, false);
        inputs = inputsOf(s.graphState(), hall);
        check(inputs[0].rows[1].level == .5 && inputs[0].rows[1].pan == -.5 && inputs[0].rows[1].mute, "a Mix row set");
        disconnect(s, inputs[0].rows[1].wire);
        check(inputsOf(s.graphState(), hall)[0].rows.size() == 1 && readParts(s.graphState()).shared.empty(), "disconnected: one source, no longer shared");
        refused = false; try { removeLoose(s, bassSynth); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a part's plugin is removed in its part, not in Routing");
        const auto nodesBefore = s.graphState().topology.nodes.size();
        removeLoose(s, hall);
        check(s.graphState().topology.nodes.size() == nodesBefore - 1, "a plugin outside the parts removed in Routing");
        removeSplitKeepingFirst(s, layers, wide);
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a split and a shared delay built through the Parts views' model");
    return failures ? 1 : 0;
}

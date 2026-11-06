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

        // The Routing layout, collapsed and expanded: no wire passes under a plugin or part, boxes
        // do not overlap, collapsed parts are one box each, expanded ones frames round their plugins.
        {
            const auto g = s.graphState();
            size_t collapsedPaths = 0, expandedPaths = 0;
            for(const bool collapsedView : {true, false})
            {
                const auto layout = layoutRouting(g, collapsedView, {});
                using Kind = RoutingLayout::Box::Kind;
                std::string under;
                for(const auto& p : layout.paths)
                    for(const auto& pt : routingCurve(p.points))
                        for(const auto& b : layout.boxes)
                            if(b.kind != Kind::Frame && b.bounds.contains(pt.x, pt.y)) under = b.name;
                check(under.empty(), std::string(collapsedView ? "collapsed" : "expanded") + ": no wire passes under a box (one passes under " + under + ")");
                bool overlap = false;
                for(size_t i = 0; i < layout.boxes.size(); ++i) for(size_t j = i + 1; j < layout.boxes.size(); ++j)
                {
                    const auto& a = layout.boxes[i]; const auto& b = layout.boxes[j];
                    if(a.kind == Kind::Frame && b.part == a.part) continue; // a frame holds its plugins
                    if(a.bounds.x < b.bounds.x + b.bounds.w && b.bounds.x < a.bounds.x + a.bounds.w && a.bounds.y < b.bounds.y + b.bounds.h && b.bounds.y < a.bounds.y + a.bounds.h) overlap = true;
                }
                check(!overlap, std::string(collapsedView ? "collapsed" : "expanded") + ": boxes do not overlap");
                const auto parts = std::count_if(layout.boxes.begin(), layout.boxes.end(), [&](const auto& b) { return b.kind == (collapsedView ? Kind::Part : Kind::Frame); });
                const auto partNodes = std::count_if(layout.boxes.begin(), layout.boxes.end(), [](const auto& b) { return b.kind == Kind::Node && b.part; });
                const auto expected = readParts(g).parts.size() + readParts(g).buses.size();
                check(size_t(parts) == expected && (collapsedView ? partNodes == 0 : partNodes > 0), std::string(collapsedView ? "one box per part" : "a frame per part, its plugins inside"));
                (collapsedView ? collapsedPaths : expandedPaths) = layout.paths.size();
            }
            check(collapsedPaths < expandedPaths, "collapsed, fewer wires: " + std::to_string(collapsedPaths) + " of " + std::to_string(expandedPaths));
            const auto one = layoutRouting(g, true, {bass});
            const auto frames = std::count_if(one.boxes.begin(), one.boxes.end(), [&](const auto& b) { return b.kind == RoutingLayout::Box::Kind::Frame && b.part == bass; });
            check(frames == 1 && std::none_of(one.boxes.begin(), one.boxes.end(), [&](const auto& b) { return b.kind == RoutingLayout::Box::Kind::Part && b.part == bass; }), "one part expanded, the rest collapsed");
        }

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
        // A velocity range, kept apart from the keys; the whole range again leaves no zone.
        setVelocities(s, lead, 64, 127); setZone(s, lead, 48, 72, 0);
        const auto soft = readParts(s.graphState()).parts[1];
        check(soft.lowVelocity == 64 && soft.highVelocity == 127 && soft.lowKey == 48 && soft.highKey == 72, "velocities and keys together");
        setVelocities(s, lead, 0, 127); setZone(s, lead, 0, 127, 0);
        check(s.parts()[1].input.zones.empty(), "all keys and velocities again: no zone left");
        // The Part page's fields, as every surface lays them out: a part that plays notes has
        // its inputs, keys and transpose; every part where it plays, its level and a send per bus.
        {
            const auto state = readParts(s.graphState());
            std::vector<std::string> labels;
            for(const auto& f : partFields(state.parts[1], state)) labels.push_back(f.label + (f.span ? "" : " (row)"));
            const std::vector<std::string> expected{"Plays from", "Notes at once", "Reserved notes", "Priority", "Keys and velocity (row)", "Transpose", "Plays to", "MIDI out", "Level", "Send to Room"};
            check(labels == expected, "a part's fields, in order");
            const auto busFields = partFields(state.buses[0], state);
            check(busFields.size() == 2 && busFields[0].kind == PartField::Kind::Output && busFields[1].kind == PartField::Kind::Level, "a bus: where it plays and its level, no send to itself");
        }
        setOutput(s, lead, room); check(readParts(s.graphState()).parts[1].through == room, "the lead plays through the room");
        setSendLevel(s, bass, room, 0); check(readParts(s.graphState()).parts[0].sends.empty(), "a send at 0 is removed");
        check(roleOf({0, {}, {}, {}, 0, 1}) == "Sequencer", "a node with only note outputs is a sequencer");
        setMaxNotes(s, lead, 8);
        check(s.parts()[1].input.maxNotes == 8 && readParts(s.graphState()).parts[1].maxNotes == 8, "a part's note limit set from the Part page");
        setReserve(s, lead, 6); setPriority(s, lead, 7);
        check(readParts(s.graphState()).parts[1].reserve == 6 && readParts(s.graphState()).parts[1].priority == 7, "a part's reserve and priority set from the Part page");
        setMaxNotes(s, lead, 4);
        check(s.parts()[1].input.reserve == 4, "a lower note limit brings the reserve down to it");
        setMaxNotes(s, lead, 0); setReserve(s, lead, 0); setPriority(s, lead, 0);
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
        // MIDI out from the Part page (RFC 0012 step 1): an output by name on a channel, then off.
        const MidiEndpoint modx{MidiEndpoint::Kind::Usb, 0, "MODX M"};
        check(midiOutName({}) == "Off" && readParts(s.graphState()).parts[1].midiOut == PartMidiOut{}, "MIDI out starts off");
        setMidiOut(s, lead, {toggled({}, ruleFor(modx)), 3});
        const auto sent = readParts(s.graphState()).parts[1].midiOut;
        check(s.parts()[1].midiOut.channel == 3 && sent.channel == 3 && midiOutName(sent.devices) == "MODX M", "the lead sends to the MODX on channel 3: " + midiOutName(sent.devices));
        setMidiOut(s, lead, {toggled(sent.devices, ruleFor(modx)), 3});
        check(s.parts()[1].midiOut.devices.empty() && midiOutName(readParts(s.graphState()).parts[1].midiOut.devices) == "Off", "picking it again turns it off");

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
        check(splitNode && splitNode->splitNode && splitNode->part == layers, "a split's node belongs to its part");
        // Laid out by flow: the keys first, Main last, a plugin after what feeds it.
        {
            const auto layout = layoutRouting(g, false, {});
            auto boxOf = [&](NodeId node, uint32_t port) -> const RoutingLayout::Box* {
                for(const auto& b : layout.boxes) if(b.kind == RoutingLayout::Box::Kind::Node && b.node == node && b.port == port) return &b;
                return nullptr; };
            const auto* keys = boxOf(graphInput, notesPort); const auto* main = boxOf(graphOutput, 0);
            bool keysFirst = keys && main, mainLast = keysFirst;
            for(const auto& b : layout.boxes) if(keysFirst && b.kind == RoutingLayout::Box::Kind::Node)
            {
                keysFirst = keysFirst && keys->bounds.x <= b.bounds.x;
                mainLast = mainLast && main->bounds.x >= b.bounds.x;
            }
            check(keysFirst && mainLast, "the keys on the left, Main on the right");
            const auto* split = boxOf(sp.splits[0].splitter, 0);
            bool afterFeed = split != nullptr;
            for(const auto& w : g.wires) if(split && w.to == sp.splits[0].splitter && w.from != graphInput) if(const auto* f = boxOf(w.from, 0)) afterFeed = afterFeed && f->bounds.x < split->bounds.x;
            check(afterFeed, "a split after what feeds it");
        }
        auto byName = [&](const std::string& name) { for(const auto& [id, n] : s.graphState().names) if(n == name) return id; throw std::runtime_error("no node " + name); };
        const auto bassSynth = byName("Basic Synth"), leadSynth = byName("Basic Synth 2");
        check(routing.find(bassSynth, 0)->partName == "Bass", "an instrument knows its part");
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

        // How wires and parts are drawn: enabled, disabled (muted or switched off) or draining (on a
        // plugin switched off and still sounding out); a part with such a plugin is draining.
        {
            auto g = s.graphState();
            const auto input = std::find_if(g.wires.begin(), g.wires.end(), [&](const Wire& w) { return w.owner == WireOwner::Part && w.part == bass && w.from == graphInput && w.kind == WireKind::Notes; });
            check(input != g.wires.end() && wireState(*input, g) == WireState::Enabled && !readParts(g).parts[0].draining, "a part's note input, enabled");
            g.disabled.insert(input->to); g.ringing.insert(input->to);
            check(wireState(*input, g) == WireState::Draining, "draining while its plugin, switched off, sounds out");
            const auto rows = readParts(g).parts;
            check(std::any_of(rows.begin(), rows.end(), [&](const PartRow& r) { return r.id == bass && r.draining; }) &&
                  std::none_of(rows.begin(), rows.end(), [&](const PartRow& r) { return r.id != bass && r.draining; }), "its part is draining, the others not");
            g.ringing.clear();
            check(wireState(*input, g) == WireState::Disabled, "disabled once its plugin has rung out");
            const auto offRows = readParts(g).parts;
            const auto bassRow = std::find_if(offRows.begin(), offRows.end(), [&](const PartRow& r) { return r.id == bass; });
            const auto* box = bassRow == offRows.end() ? nullptr : findBox(bassRow->chain, input->to);
            check(box && box->off, "its box is marked off");
            const auto routed = layoutRouting(g, true, {});
            check(std::any_of(routed.boxes.begin(), routed.boxes.end(), [](const RoutingLayout::Box& b) {
                      return std::any_of(b.chips.begin(), b.chips.end(), [](const std::string& c) { return c.ends_with(" · off"); }); }), "and its chip in Routing");
            g.disabled.clear();
            auto off = *input; off.enabled = false;
            check(wireState(off, g) == WireState::Disabled, "a wire switched off is disabled");
            auto muted = *input; muted.mute = true;
            check(wireState(muted, g) == WireState::Disabled, "a muted wire is disabled");
            // A line per channel: the source port's, or the source channels its pairs take.
            check(wireChannels(*input, g.topology) == 1, "a note wire is one line");
            Wire audio; audio.from = graphInput; audio.fromPort = 7;
            g.topology.audioLayout.inputs = {{7, "Quad in", 4, 0}};
            check(wireChannels(audio, g.topology) == 4, "a wire from a four-channel port: " + std::to_string(wireChannels(audio, g.topology)));
            audio.channels = {{0, 0}, {0, 1}};
            check(wireChannels(audio, g.topology) == 1, "one source channel copied to two is one");
            audio.channels = {{2, 0}, {3, 1}};
            check(wireChannels(audio, g.topology) == 2, "two source channels are two");
        }
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a split and a shared delay built through the Parts views' model; wire and part states, wire channels");
    return failures ? 1 : 0;
}

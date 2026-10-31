// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PartsModel.h"
#include "oscairn/pads.h"
#include <algorithm>
#include <filesystem>
#include "oscairn/discovery.h"

namespace oscairn::standalone
{
    namespace
    {
        const Graph::NodeInfo* nodeInfo(const Control::GraphState& g, NodeId id)
        {
            for(const auto& n : g.topology.nodes) if(n.id == id) return &n;
            return nullptr;
        }
        std::string range(int low, int high) { return noteName(low) + "–" + noteName(high); }
        // Which part (or bus) each node belongs to: in its chain or a lane, or a split's node.
        std::map<NodeId, const Part*> owners(const Control::GraphState& g)
        {
            std::map<NodeId, const Part*> out;
            for(const auto& p : g.parts)
            {
                for(const auto e : p.chain) if(!isSplit(e)) out[e] = &p;
                for(const auto& s : p.splits)
                {
                    for(const auto& l : s.lanes) for(const auto e : l.chain) if(!isSplit(e)) out[e] = &p;
                    if(s.splitter) out[s.splitter] = &p;
                    if(s.joiner) out[s.joiner] = &p;
                }
            }
            return out;
        }
        bool hasUserWires(const Control::GraphState& g, NodeId node)
        {
            return std::any_of(g.wires.begin(), g.wires.end(), [&](const Wire& w) { return w.owner == WireOwner::User && (w.from == node || w.to == node); });
        }
        std::string nameOf(const Control::GraphState& g, NodeId node) { return g.names.count(node) ? g.names.at(node) : std::string("a plugin"); }
    }

    std::string roleOf(const Graph::NodeInfo& n)
    {
        size_t in = 0, out = 0;
        for(const auto& p : n.inputs) in += p.channels;
        for(const auto& p : n.outputs) out += p.channels;
        if(n.noteInputs && n.noteOutputs && !out) return "Notes";
        if(n.noteOutputs && !out) return n.noteInputs ? "Notes" : "Sequencer";
        if(out && !in) return n.noteInputs ? "Instrument" : "Generator";
        if(in && out) return "Effect";
        if(in) return "Analyzer";
        return "Notes";
    }

    PartsState readParts(const Control::GraphState& g)
    {
        PartsState state;
        std::map<uint32_t, std::vector<std::string>> feeders; // bus -> parts sending to it
        for(const auto& p : g.parts)
        {
            if(p.bus) feeders[p.bus].push_back(p.name);
            for(const auto& s : p.sends) feeders[s.bus].push_back(p.name);
        }
        for(const auto& p : g.parts)
        {
            PartRow row;
            row.id = p.id; row.name = p.name; row.bus = p.input.kind == PartInput::Kind::Bus;
            row.level = p.level; row.pan = p.pan; row.mute = p.mute; row.solo = p.solo; row.through = p.bus;
            // Its own mute, or someone else's solo: the wires where it plays are muted either way.
            row.silenced = !p.mute && std::any_of(g.wires.begin(), g.wires.end(), [&](const Wire& w) { return w.owner == WireOwner::Part && w.part == p.id && w.fader && w.mute; });
            row.notes = p.input.kind == PartInput::Kind::Notes; row.devices = p.input.devices; row.maxNotes = p.input.maxNotes;
            for(const auto& s : p.sends) row.sends[s.bus] = s.level;
            if(!p.input.zones.empty()) { const auto& z = p.input.zones.front(); row.lowKey = z.lowKey; row.highKey = z.highKey; row.transpose = z.transpose; }
            // Boxes in chain order; a split is a box holding its lanes' boxes.
            std::function<std::vector<PartBox>(const std::vector<NodeId>&)> boxes = [&](const std::vector<NodeId>& chain) {
                std::vector<PartBox> out;
                for(const auto entry : chain)
                {
                    if(isSplit(entry))
                    {
                        const auto s = std::find_if(p.splits.begin(), p.splits.end(), [&](const Split& x) { return x.id == (entry & ~splitMark); });
                        if(s == p.splits.end()) continue;
                        PartBox box; box.split = s->id; box.role = "Split"; box.kind = s->kind;
                        if(s->splitter && g.names.count(s->splitter)) box.splitter = g.names.at(s->splitter);
                        for(size_t k = 0; k < s->lanes.size(); ++k)
                        {
                            const auto& l = s->lanes[k];
                            PartLane lane; lane.boxes = boxes(l.chain); lane.level = l.level; lane.pan = l.pan; lane.mute = l.mute; lane.solo = l.solo;
                            if(s->kind == Split::Kind::LeftRight) lane.name = k ? "Right" : "Left";
                            else if(s->kind == Split::Kind::MidSide) lane.name = k ? "Side" : "Mid";
                            else if(s->kind == Split::Kind::Bands) lane.name = "Band " + std::to_string(k + 1);
                            if(!l.zones.empty()) { lane.lowKey = l.zones.front().lowKey; lane.highKey = l.zones.front().highKey; }
                            // A lane is named after its instrument, else its last box.
                            const auto instrument = std::find_if(lane.boxes.begin(), lane.boxes.end(), [](const PartBox& x) { return x.role == "Instrument"; });
                            box.name += (box.name.empty() ? "" : " | ") + (instrument != lane.boxes.end() ? instrument->name : !lane.boxes.empty() ? lane.boxes.back().name : lane.name.empty() ? std::string("Dry") : lane.name);
                            for(const auto& x : lane.boxes) box.userWires = box.userWires || x.userWires; // a lane's badge shows on its split
                            box.lanes.push_back(std::move(lane));
                        }
                        out.push_back(std::move(box));
                    }
                    else if(const auto* n = nodeInfo(g, entry))
                    {
                        out.push_back({entry, g.names.count(entry) ? g.names.at(entry) : std::string(), n->plugin.name, roleOf(*n)});
                        out.back().userWires = hasUserWires(g, entry);
                        if(row.sound.empty() && out.back().role == "Instrument") row.sound = out.back().name;
                    }
                }
                return out;
            };
            row.chain = boxes(p.chain);
            switch(p.input.kind)
            {
            case PartInput::Kind::Notes:
                row.input = devicesName(row.devices) + " · " + (row.lowKey == 0 && row.highKey == 127 ? std::string("all keys") : range(row.lowKey, row.highKey));
                if(row.transpose) row.input += " · " + std::string(row.transpose > 0 ? "+" : "−") + std::to_string(std::abs(row.transpose));
                break;
            case PartInput::Kind::Audio: row.input = "Audio in " + std::to_string(p.input.port + 1); break;
            case PartInput::Kind::None: row.input = "Its own notes"; break;
            case PartInput::Kind::Bus:
            {
                std::string from;
                for(const auto& f : feeders[p.id]) from += (from.empty() ? "" : ", ") + f;
                row.input = from.empty() ? "Nothing sends to it yet" : "From " + from;
                break;
            }
            }
            (row.bus ? state.buses : state.parts).push_back(std::move(row));
        }
        // Nodes outside the parts that parts are wired into in Routing; several make a shared row.
        const auto owner = owners(g);
        std::map<NodeId, std::vector<uint32_t>> fed; // node outside the parts -> the parts feeding it
        for(const auto& w : g.wires)
        {
            if(w.owner != WireOwner::User || w.kind != WireKind::Audio || owner.count(w.to) || w.to == graphOutput) continue;
            const auto from = owner.find(w.from);
            if(from == owner.end()) continue;
            auto& parts = fed[w.to];
            if(std::find(parts.begin(), parts.end(), from->second->id) == parts.end()) parts.push_back(from->second->id);
        }
        for(const auto& [node, parts] : fed)
        {
            if(parts.size() > 1)
            {
                SharedRow row{node, nameOf(g, node), {}};
                for(const auto id : parts) for(const auto& p : g.parts) if(p.id == id) row.fedBy.push_back(p.name);
                state.shared.push_back(std::move(row));
            }
            for(const auto id : parts)
                for(auto* list : {&state.parts, &state.buses})
                    for(auto& r : *list) if(r.id == id) r.exits.push_back(nameOf(g, node) + (parts.size() > 1 ? " (shared)" : ""));
        }
        return state;
    }

    const RoutingNode* RoutingState::find(NodeId node, uint32_t port) const
    {
        for(const auto& n : nodes)
            if(n.node == node && ((node != graphInput && node != graphOutput) || n.port == port)) return &n;
        return nullptr;
    }

    RoutingState readRouting(const Control::GraphState& g)
    {
        RoutingState state;
        state.wires = g.wires;
        const auto owner = owners(g);
        state.nodes.push_back({graphInput, notesPort, "Keys & MIDI", "Notes", 0});
        for(const auto& p : g.topology.audioLayout.inputs) state.nodes.push_back({graphInput, p.id, p.name.empty() ? "Audio in" : p.name, "Audio", 0});
        // Nodes in part order, then the rest.
        std::vector<NodeId> order;
        for(const auto& p : g.parts)
        {
            for(const auto& [node, part] : owner) if(part == &p) order.push_back(node);
        }
        for(const auto& n : g.topology.nodes) if(std::find(order.begin(), order.end(), n.id) == order.end()) order.push_back(n.id);
        for(const auto id : order)
        {
            const auto* n = nodeInfo(g, id);
            if(!n) continue;
            RoutingNode r{id, 0, nameOf(g, id), roleOf(*n)};
            const bool splitter = n->plugin.id == splitPlugin || n->plugin.id == joinPlugin;
            r.column = (r.role == "Notes" || r.role == "Sequencer") ? 1 : (r.role == "Instrument" || r.role == "Generator") ? 2 : 3;
            r.splitNode = splitter;
            if(const auto o = owner.find(id); o != owner.end()) { r.part = o->second->id; r.partName = o->second->name; }
            r.userWires = hasUserWires(g, id);
            state.nodes.push_back(r);
        }
        for(const auto& p : g.topology.audioLayout.outputs) state.nodes.push_back({graphOutput, p.id, p.name.empty() ? "Output" : p.name, "Output", 4});
        return state;
    }

    std::vector<RoutingInput> inputsOf(const Control::GraphState& g, NodeId node)
    {
        std::vector<RoutingInput> out;
        auto add = [&](uint32_t port, const std::string& name, const Wire& w) {
            auto at = std::find_if(out.begin(), out.end(), [&](const RoutingInput& i) { return i.port == port; });
            if(at == out.end()) { out.push_back({port, name, {}}); at = out.end() - 1; }
            std::string source = w.from == graphInput ? (w.kind == WireKind::Notes ? std::string("Keys & MIDI") : "Audio in") : nameOf(g, w.from);
            if(w.owner == WireOwner::Part) for(const auto& p : g.parts) if(p.id == w.part) source += " (" + p.name + ")";
            // A lane's or split's wire takes its Mix from the lane; the rest are edited here.
            at->rows.push_back({w.id, source, w.level, w.pan, w.mute, w.solo, w.owner == WireOwner::User, w.split == 0});
        };
        std::vector<kick::clap::Port> ports;
        if(node == graphOutput) ports = g.topology.audioLayout.outputs;
        else if(const auto* n = nodeInfo(g, node)) ports = n->inputs;
        for(const auto& w : g.wires)
        {
            if(w.to != node) continue;
            if(w.kind == WireKind::Notes) { add(notesPort, "Notes", w); continue; }
            std::string name = "Input " + std::to_string(w.toPort + 1);
            for(const auto& p : ports) if(p.id == w.toPort) name = p.name.empty() ? name : p.name;
            add(w.toPort, name, w);
        }
        return out;
    }

    std::vector<PluginChoice> bundledPlugins(const std::string& folder, const std::string& helper)
    {
        std::vector<PluginChoice> result;
        for(const auto& d : scanPlugins({folder}, helper).plugins)
        {
            // "Oscairn Basic Synth" from Oscairn is shown as "Basic Synth".
            auto name = d.name;
            if(!d.vendor.empty() && name.size() > d.vendor.size() + 1 && name.compare(0, d.vendor.size() + 1, d.vendor + " ") == 0) name = name.substr(d.vendor.size() + 1);
            result.push_back({name, d.path, d.id});
        }
        std::sort(result.begin(), result.end(), [](const PluginChoice& a, const PluginChoice& b) { return a.name < b.name; });
        return result;
    }

    uint32_t addPart(Control& c, const std::string& name, bool bus)
    {
        const auto id = c.addPart(name, bus ? PartInput{PartInput::Kind::Bus} : PartInput{});
        c.commit();
        return id;
    }

    const PartBox* findBox(const std::vector<PartBox>& chain, NodeId node)
    {
        for(const auto& b : chain)
        {
            if(!b.split && b.node == node) return &b;
            for(const auto& l : b.lanes) if(const auto* f = findBox(l.boxes, node)) return f;
        }
        return nullptr;
    }

    namespace
    {
        // A node for a plugin, named after it: "Basic Synth", then "Basic Synth 2" and so on.
        NodeId addNamed(Control& c, const PluginChoice& plugin)
        {
            const auto names = c.graphState().names;
            std::string name;
            for(int n = 1;; ++n)
            {
                name = plugin.name + (n > 1 ? " " + std::to_string(n) : std::string());
                if(std::none_of(names.begin(), names.end(), [&](const auto& e) { return e.second == name; })) break;
            }
            return c.add(name, plugin.path, plugin.id);
        }
    }

    void insertPlugin(Control& c, uint32_t part, size_t index, const PluginChoice& plugin, ChainPlace place)
    {
        const auto node = addNamed(c, plugin);
        try { c.insertIntoPart(part, index, node, place); }
        catch(...) { c.remove(node); throw; }
        c.commit();
    }

    uint32_t insertSplit(Control& c, uint32_t part, size_t index, Split::Kind kind, const std::vector<PluginChoice>& plugins)
    {
        NodeId splitter = 0, joiner = 0;
        auto choice = [&](const char* id) {
            const auto found = std::find_if(plugins.begin(), plugins.end(), [&](const PluginChoice& p) { return p.id == id; });
            if(found == plugins.end()) throw std::invalid_argument("the Split and Join plugins are not installed");
            return *found;
        };
        try
        {
            if(kind != Split::Kind::Copy) splitter = addNamed(c, choice(splitPlugin));
            if(kind == Split::Kind::LeftRight || kind == Split::Kind::MidSide) joiner = addNamed(c, choice(joinPlugin));
            const auto split = c.addSplit(part, index, {}, kind, splitter, joiner);
            c.commit();
            return split;
        }
        catch(...) { if(splitter) c.remove(splitter); if(joiner) c.remove(joiner); throw; }
    }

    namespace
    {
        const Part& partIn(const std::vector<Part>& parts, uint32_t part)
        {
            const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.id == part; });
            if(found == parts.end()) throw std::invalid_argument("unknown part");
            return *found;
        }
        const Split& splitIn(const Part& p, uint32_t split)
        {
            const auto found = std::find_if(p.splits.begin(), p.splits.end(), [&](const Split& s) { return s.id == split; });
            if(found == p.splits.end()) throw std::invalid_argument("unknown split");
            return *found;
        }
        // The nodes in a chain, those in its splits' lanes included.
        void nodesIn(const Part& p, const std::vector<NodeId>& chain, std::vector<NodeId>& out)
        {
            for(const auto e : chain)
                if(!isSplit(e)) out.push_back(e);
                else for(const auto& l : splitIn(p, e & ~splitMark).lanes) nodesIn(p, l.chain, out);
        }
    }

    void removeSplitKeepingFirst(Control& c, uint32_t part, uint32_t split)
    {
        const auto parts = c.graphState().parts;
        const auto& p = partIn(parts, part);
        const auto& s = splitIn(p, split);
        std::vector<NodeId> leaving;
        for(size_t i = 1; i < s.lanes.size(); ++i) nodesIn(p, s.lanes[i].chain, leaving);
        c.removeSplit(part, split);
        for(const auto n : leaving) c.remove(n, UserWires::Reconnect);
        c.commit();
    }

    void removeLaneAndBoxes(Control& c, uint32_t part, uint32_t split, uint32_t lane)
    {
        const auto parts = c.graphState().parts;
        const auto& p = partIn(parts, part);
        const auto& s = splitIn(p, split);
        if(lane >= s.lanes.size()) throw std::invalid_argument("unknown lane");
        std::vector<NodeId> leaving;
        nodesIn(p, s.lanes[lane].chain, leaving);
        c.removeLane(part, split, lane);
        for(const auto n : leaving) c.remove(n, UserWires::Reconnect);
        c.commit();
    }

    void editLane(Control& c, uint32_t part, uint32_t split, uint32_t lane, const std::function<void(Lane&)>& change)
    {
        const auto parts = c.graphState().parts;
        const auto& s = splitIn(partIn(parts, part), split);
        if(lane >= s.lanes.size()) throw std::invalid_argument("unknown lane");
        auto l = s.lanes[lane];
        change(l);
        c.setLane(part, split, lane, l);
        c.commit();
    }

    void setLaneKeys(Control& c, uint32_t part, uint32_t split, uint32_t lane, int lowKey, int highKey)
    {
        editLane(c, part, split, lane, [&](Lane& l) {
            NoteZone zone = l.zones.empty() ? NoteZone{} : l.zones.front();
            zone.lowKey = uint8_t(std::clamp(lowKey, 0, 127)); zone.highKey = uint8_t(std::clamp(highKey, int(zone.lowKey), 127));
            if(l.zones.empty()) { if(!(zone == NoteZone{})) l.zones.push_back(zone); }
            else if(zone == NoteZone{} && l.zones.size() == 1) l.zones.clear();
            else l.zones.front() = zone;
        });
    }

    void removeBox(Control& c, uint32_t part, NodeId node)
    {
        c.removeFromPart(part, node);
        c.remove(node, UserWires::Reconnect);
        c.commit();
    }

    void setZone(Control& c, uint32_t part, int lowKey, int highKey, int transpose)
    {
        const auto parts = c.graphState().parts;
        const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.id == part; });
        if(found == parts.end()) throw std::invalid_argument("unknown part");
        auto input = found->input;
        NoteZone zone = input.zones.empty() ? NoteZone{} : input.zones.front();
        zone.lowKey = uint8_t(std::clamp(lowKey, 0, 127)); zone.highKey = uint8_t(std::clamp(highKey, int(zone.lowKey), 127));
        zone.transpose = int8_t(std::clamp(transpose, -48, 48));
        const bool everything = zone == NoteZone{};
        if(input.zones.empty()) { if(!everything) input.zones.push_back(zone); }
        else if(everything && input.zones.size() == 1) input.zones.clear();
        else input.zones.front() = zone;
        c.setPartInput(part, input);
        c.commit();
    }

    std::string devicesName(const std::vector<DeviceRule>& rules)
    {
        if(rules.empty()) return "Any device";
        std::string out;
        for(const auto& r : rules)
        {
            std::string name;
            switch(r.kind)
            {
            case DeviceRule::Kind::BuiltIn: name = "Built in"; break;
            case DeviceRule::Kind::External: name = "All external"; break;
            case DeviceRule::Kind::All: name = "Any device"; break;
            case DeviceRule::Kind::Port: name = "Port " + std::to_string(r.port); break;
            case DeviceRule::Kind::Usb: name = "All USB"; break;
            case DeviceRule::Kind::UsbNamed: case DeviceRule::Kind::Network:
                name = r.kind == DeviceRule::Kind::Network ? (r.pattern.empty() ? "Network" : "Network ") : "";
                if(!r.pattern.empty()) name += r.match == DeviceRule::Match::Equals ? r.pattern : r.match == DeviceRule::Match::Contains ? "“" + r.pattern + "”" : "/" + r.pattern + "/";
                break;
            }
            out += (out.empty() ? "" : " + ") + name;
        }
        return out;
    }

    DeviceRule ruleFor(const MidiEndpoint& e)
    {
        switch(e.kind)
        {
        case MidiEndpoint::Kind::BuiltIn: return {DeviceRule::Kind::BuiltIn};
        case MidiEndpoint::Kind::Port: return {DeviceRule::Kind::Port, DeviceRule::Match::Contains, e.port};
        case MidiEndpoint::Kind::Network: return {DeviceRule::Kind::Network, DeviceRule::Match::Equals, 0, e.name};
        case MidiEndpoint::Kind::Usb: break;
        }
        return {DeviceRule::Kind::UsbNamed, DeviceRule::Match::Equals, 0, e.name};
    }

    std::vector<DeviceRule> toggled(std::vector<DeviceRule> rules, const DeviceRule& rule)
    {
        if(rule.kind == DeviceRule::Kind::All) return {};
        if(std::erase(rules, rule) == 0) rules.push_back(rule);
        return rules;
    }

    void setDevices(Control& c, uint32_t part, const std::vector<DeviceRule>& devices)
    {
        const auto parts = c.graphState().parts;
        const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.id == part; });
        if(found == parts.end()) throw std::invalid_argument("unknown part");
        auto input = found->input;
        input.devices = devices;
        c.setPartInput(part, input);
        c.commit();
    }

    void connectNodes(Control& c, NodeId from, NodeId to, WireKind kind)
    {
        if(kind == WireKind::Audio) c.connectAudio(from, to); else c.connectNotes(from, to);
        c.commit();
    }

    void disconnect(Control& c, uint32_t wire) { c.disconnectWire(wire); c.commit(); }

    void setWireMix(Control& c, uint32_t wire, double level, double pan, bool mute, bool solo) { c.setMix(wire, level, pan, mute, solo); c.commit(); }

    NodeId addLoose(Control& c, const PluginChoice& plugin)
    {
        const auto node = addNamed(c, plugin);
        c.commit();
        return node;
    }

    void removeLoose(Control& c, NodeId node)
    {
        const auto g = c.graphState();
        if(owners(g).count(node)) throw std::invalid_argument("the plugin belongs to a part: remove it there");
        c.remove(node, UserWires::Reconnect);
        c.commit();
    }

    void setMaxNotes(Control& c, uint32_t part, int maxNotes)
    {
        const auto parts = c.graphState().parts;
        const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.id == part; });
        if(found == parts.end()) throw std::invalid_argument("unknown part");
        auto input = found->input;
        input.maxNotes = uint16_t(std::clamp(maxNotes, 0, 128));
        c.setPartInput(part, input);
        c.commit();
    }

    void setOutput(Control& c, uint32_t part, uint32_t bus) { c.setPartOutput(part, bus); c.commit(); }

    void setSendLevel(Control& c, uint32_t part, uint32_t bus, double level)
    {
        c.setSend(part, bus, level > 0 ? std::optional<double>(level) : std::nullopt);
        c.commit();
    }
}

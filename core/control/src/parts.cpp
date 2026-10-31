// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 1: parts, wire ownership, implicit Mixes and port-level routes. The Session
// keeps the wires; every edit compiles them into the graph's channel routes and note edges.
#include "oscairn/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <tuple>

namespace oscairn
{
    namespace
    {
        // A node's audio ports as a source (input == false) or a destination. The graph's own
        // input sends its configured inputs; its output receives its configured outputs.
        const std::vector<kick::clap::Port>* portsOf(const Graph& g, const AudioLayout& layout, NodeId node, bool input)
        {
            if(node == graphInput) return input ? nullptr : &layout.inputs;
            if(node == graphOutput) return input ? &layout.outputs : nullptr;
            if(!g.contains(node)) return nullptr;
            return input ? &g.host(node).inputPorts() : &g.host(node).outputPorts();
        }
        std::optional<uint32_t> channelsOf(const Graph& g, const AudioLayout& layout, NodeId node, uint32_t port, bool input)
        {
            if(const auto* list = portsOf(g, layout, node, input))
                for(const auto& p : *list) if(p.id == port) return p.channels;
            return {};
        }
        uint32_t mainPort(const Graph& g, const AudioLayout& layout, NodeId node, bool input)
        {
            if(node == graphInput || node == graphOutput)
            {
                const auto* list = portsOf(g, layout, node, input);
                if(!list || list->empty()) throw std::invalid_argument("the graph has no such audio port");
                return list->front().id;
            }
            const auto main = g.host(node).mainAudioPorts(input);
            if(main.empty()) throw std::invalid_argument("no main audio port");
            return main.front();
        }
        bool sendsAudio(const Graph& g, const AudioLayout& layout, NodeId node) { const auto* p = portsOf(g, layout, node, false); return p && !p->empty(); }
        bool takesAudio(const Graph& g, const AudioLayout& layout, NodeId node) { const auto* p = portsOf(g, layout, node, true); return p && !p->empty(); }
        bool sendsNotes(const Graph& g, NodeId node) { return node == graphInput || (g.contains(node) && g.host(node).noteOutputPorts()); }
        bool takesNotes(const Graph& g, NodeId node) { return node != graphInput && node != graphOutput && g.contains(node) && g.host(node).noteInputPorts(); }

        struct Pair { uint32_t from, to; double gain; };
        // One audio wire's channel pairs at the given Mix gain, or nothing when it does not fit.
        std::optional<std::vector<Pair>> pairs(const Graph& g, const AudioLayout& layout, const Wire& w, double gain)
        {
            const auto n = channelsOf(g, layout, w.from, w.fromPort, false), m = channelsOf(g, layout, w.to, w.toPort, true);
            if(!n || !m) return {};
            std::vector<Pair> result;
            if(!w.channels.empty())
            {
                for(const auto& [a, b] : w.channels) { if(a >= *n || b >= *m) return {}; result.push_back({a, b, gain}); }
            }
            else if(*n == *m) for(uint32_t c = 0; c < *n; ++c) result.push_back({c, c, gain});
            else if(*n == 1 && *m == 2) result = {{0, 0, gain}, {0, 1, gain}};
            else if(*n == 2 && *m == 1) result = {{0, 0, gain * M_SQRT1_2}, {1, 0, gain * M_SQRT1_2}};
            else return {};
            if(*m == 2) for(auto& p : result) p.gain *= p.to ? std::min(1.0, 1 + w.pan) : std::min(1.0, 1 - w.pan);
            return result;
        }
    }

    namespace
    {
        std::string lower(std::string s) { for(auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
        bool named(const DeviceRule& rule, const std::string& name)
        {
            switch(rule.match)
            {
                case DeviceRule::Match::Equals: return lower(name) == lower(rule.pattern);
                case DeviceRule::Match::Contains: return lower(name).find(lower(rule.pattern)) != std::string::npos;
                case DeviceRule::Match::Regex: return std::regex_search(name, std::regex(rule.pattern, std::regex::ECMAScript | std::regex::icase));
            }
            return false;
        }
    }

    bool DeviceRule::valid() const
    {
        if(kind == Kind::UsbNamed && pattern.empty()) return false;
        if(match == Match::Regex) try { std::regex(pattern, std::regex::ECMAScript | std::regex::icase); } catch(const std::regex_error&) { return false; }
        return pattern.size() <= 256;
    }

    bool DeviceRule::matches(const MidiEndpoint& e) const
    {
        using K = MidiEndpoint::Kind;
        switch(kind)
        {
            case Kind::BuiltIn: return e.kind == K::BuiltIn;
            case Kind::External: return e.kind != K::BuiltIn;
            case Kind::All: return true;
            case Kind::Port: return e.kind == K::Port && e.port == port;
            case Kind::Usb: return e.kind == K::Usb;
            case Kind::UsbNamed: return e.kind == K::Usb && named(*this, e.name);
            case Kind::Network: return e.kind == K::Network && (pattern.empty() || named(*this, e.name));
        }
        return false;
    }

    uint64_t resolveDevices(const std::vector<DeviceRule>& rules, const std::vector<MidiEndpoint>& endpoints)
    {
        if(rules.empty()) return ~0ull;
        uint64_t mask = 0;
        for(const auto& r : rules)
        {
            if(r.kind == DeviceRule::Kind::All) return ~0ull;
            if(r.kind == DeviceRule::Kind::BuiltIn) mask |= 1ull << builtInSource;
            for(const auto& e : endpoints) if(e.source < maxSources && r.matches(e)) mask |= 1ull << e.source;
        }
        return mask;
    }

    std::vector<NoteZone> inputZones(const PartInput& input, const std::vector<MidiEndpoint>& endpoints)
    {
        auto zones = input.zones;
        if(input.devices.empty()) return zones;
        if(zones.empty()) zones.emplace_back(); // a zone passing every note, to carry the devices
        const auto mask = resolveDevices(input.devices, endpoints);
        for(auto& z : zones) z.sources = mask;
        return zones;
    }

    std::vector<NoteZone> intersectZones(const std::vector<NoteZone>& first, const std::vector<NoteZone>& then)
    {
        std::vector<NoteZone> out;
        for(const auto& a : first)
            for(const auto& b : then)
            {
                // b sees a's output: the key after a's transpose, the velocity after a's shaping.
                NoteZone z = a;
                z.lowKey = uint8_t(std::max<int>(a.lowKey, b.lowKey - a.transpose));
                z.highKey = uint8_t(std::min<int>(a.highKey, b.highKey - a.transpose));
                if(a.fixedVelocity > 0) { if(a.fixedVelocity < b.lowVelocity || a.fixedVelocity > b.highVelocity) continue; }
                else
                {
                    z.lowVelocity = std::max(a.lowVelocity, std::pow(b.lowVelocity, 1 / a.curve));
                    z.highVelocity = std::min(a.highVelocity, std::pow(b.highVelocity, 1 / a.curve));
                }
                z.channels = a.channels & b.channels;
                z.sources = a.sources & b.sources;
                const int transpose = a.transpose + b.transpose;
                if(transpose < -127 || transpose > 127) continue;
                z.transpose = int8_t(transpose);
                if(b.fixedVelocity > 0) z.fixedVelocity = b.fixedVelocity;
                else if(a.fixedVelocity > 0) z.fixedVelocity = std::pow(a.fixedVelocity, b.curve);
                else z.curve = std::min(16.0, a.curve * b.curve); // the steepest one zone holds
                if(a.highKey < b.lowKey - a.transpose || a.lowKey > b.highKey - a.transpose || z.lowKey > z.highKey || z.lowVelocity > z.highVelocity || !z.channels) continue;
                if(z.valid()) out.push_back(z);
            }
        return out;
    }

    // Compiles the wires into the graph's routes, changing only what differs, and marks each
    // wire connected or not.
    void Session::compileWires(Graph& g, std::vector<Wire>& wires)
    {
        const auto layout = g.describe().audioLayout;
        using Key = std::tuple<NodeId, uint32_t, uint32_t, NodeId, uint32_t, uint32_t>;
        std::map<Key, double> audio;
        // Note connections and their zones: none when any wire on the connection passes every note.
        std::map<std::pair<NodeId, NodeId>, std::vector<NoteZone>> notes;
        std::set<std::pair<NodeId, NodeId>> passAll;
        // Each input's implicit Mix: a soloed source silences the unsoloed ones.
        std::set<std::pair<NodeId, uint32_t>> soloed;
        for(const auto& w : wires) if(w.kind == WireKind::Audio && w.solo) soloed.insert({w.to, w.toPort});
        for(auto& w : wires)
        {
            if(w.kind == WireKind::Notes)
            {
                w.connected = sendsNotes(g, w.from) && takesNotes(g, w.to);
                if(!w.connected) continue;
                auto& zones = notes[{w.from, w.to}];
                if(w.zones.empty()) passAll.insert({w.from, w.to});
                else if(zones.size() + w.zones.size() > NoteZones::capacity) w.connected = false; // over the connection's zones
                else zones.insert(zones.end(), w.zones.begin(), w.zones.end());
                continue;
            }
            const bool silent = w.mute || (soloed.count({w.to, w.toPort}) && !w.solo);
            const auto compiled = pairs(g, layout, w, silent ? 0 : w.level);
            w.connected = compiled.has_value();
            if(!compiled || silent) continue;
            for(const auto& p : *compiled) audio[{w.from, w.fromPort, p.from, w.to, w.toPort, p.to}] += p.gain;
        }
        const auto present = g.describe();
        std::map<Key, double> existing;
        for(const auto& r : present.audioRoutes)
            for(uint32_t i = 0; i < r.channels; ++i) existing[{r.from.node, r.from.port, r.from.channel + i, r.to.node, r.to.port, r.to.channel + i}] = r.gain;
        auto connection = [](const Key& k, double gain) {
            return AudioConnection{{std::get<0>(k), std::get<1>(k), std::get<2>(k)}, {std::get<3>(k), std::get<4>(k), std::get<5>(k)}, 1, gain};
        };
        for(const auto& [k, gain] : existing) if(!audio.count(k)) g.disconnectAudio(connection(k, gain));
        for(const auto& [k, gain] : audio) if(auto e = existing.find(k); e == existing.end() || e->second != gain) g.connectAudio(connection(k, gain));
        for(auto& [e, zones] : notes) if(passAll.count(e)) zones.clear();
        for(const auto& e : present.notes) if(!notes.count(e)) g.disconnectNotes(e.first, e.second);
        for(const auto& [e, zones] : notes)
        {
            const bool exists = std::find(present.notes.begin(), present.notes.end(), e) != present.notes.end();
            const auto had = present.noteZones.find(e);
            if(!exists || (had == present.noteZones.end() ? !zones.empty() : had->second != zones)) g.connectNotes(e.first, e.second, zones);
        }
    }

    void Session::syncWires() { compileWires(graph_, wires_); staged_ = true; }

    // Applies an edit to the parts and wires as a whole: if it or its compilation fails, both
    // return to how they were.
    template<class Edit> void Session::editWires(Edit&& edit)
    {
        auto parts = parts_; auto wires = wires_;
        try { edit(); syncWires(); }
        catch(...) { parts_.swap(parts); wires_.swap(wires); syncWires(); throw; }
    }

    // Where sound sent to a bus enters it: its first box, or, with an empty chain, wherever the
    // bus itself plays. Bus edits are refused if they would loop, so this ends.
    // Where sound sent to a bus enters it: its first box that takes sound, or, through a split
    // before that, the first such box in each lane (an empty lane continues after the split), at
    // the lane's Mix. With no box to enter, wherever the bus itself plays. Bus edits are refused
    // if they would loop, so this ends.
    std::vector<Session::BusEntry> Session::busEntries(uint32_t id)
    {
        const auto layout = graph_.describe().audioLayout;
        const auto& bus = findPart(id);
        std::function<std::vector<BusEntry>(const std::vector<NodeId>&)> reach = [&](const std::vector<NodeId>& rest) -> std::vector<BusEntry> {
            for(size_t i = 0; i < rest.size(); ++i)
            {
                const auto entry = rest[i];
                if(!isSplit(entry)) { if(takesAudio(graph_, layout, entry)) return {{entry, mainPort(graph_, layout, entry, true)}}; continue; }
                const auto& split = *std::find_if(bus.splits.begin(), bus.splits.end(), [&](const Split& s) { return s.id == (entry & ~splitMark); });
                if(split.splitter) return {{split.splitter, mainPort(graph_, layout, split.splitter, true)}};
                const bool solo = std::any_of(split.lanes.begin(), split.lanes.end(), [](const Lane& l) { return l.solo; });
                std::vector<BusEntry> out;
                for(uint32_t k = 0; k < split.lanes.size(); ++k)
                {
                    const auto& lane = split.lanes[k];
                    auto sequence = lane.chain;
                    sequence.insert(sequence.end(), rest.begin() + std::ptrdiff_t(i + 1), rest.end());
                    for(auto x : reach(sequence))
                    {
                        x.level *= lane.level; x.pan = std::clamp(x.pan + lane.pan, -1.0, 1.0);
                        x.mute = x.mute || lane.mute || (solo && !lane.solo);
                        if(!x.split) { x.split = split.id; x.lane = k; }
                        out.push_back(x);
                    }
                }
                return out;
            }
            if(bus.bus) return busEntries(bus.bus);
            return {{graphOutput, bus.output}};
        };
        return reach(bus.chain);
    }

    // Rebuilds every part's own wires; each depends only on its own chain and the buses it plays
    // into, so the order does not matter.
    void Session::buildParts() { for(const auto& p : parts_) buildPart(p); }

    // Rebuilds one part's own wires: its input into the first box, box to box, through each
    // split's lanes and joining after them, the last boxes to its output or bus, and its sends.
    // A rebuilt wire keeps the Mix settings of the one it replaces; a wire joining from a lane
    // takes the lane's, and the part's fader and sends set theirs.
    void Session::buildPart(const Part& part)
    {
        const auto layout = graph_.describe().audioLayout;
        std::vector<Wire> previous;
        std::erase_if(wires_, [&](const Wire& w) { const bool own = w.owner == WireOwner::Part && w.part == part.id; if(own) previous.push_back(w); return own; });
        auto add = [&](Wire w) {
            w.owner = WireOwner::Part; w.part = part.id; w.id = nextWire_++;
            for(const auto& p : previous)
                if(p.kind == w.kind && p.from == w.from && p.to == w.to && p.fromPort == w.fromPort && p.toPort == w.toPort)
                    { w.level = p.level; w.pan = p.pan; w.mute = p.mute; w.solo = p.solo; break; }
            wires_.push_back(std::move(w));
            return &wires_.back();
        };
        // Where the chain has got to: each box (or the input) whose sound and notes reach the next
        // entry, with the zones its notes pass and, after a split, the Mix of the lane it ends.
        struct End
        {
            NodeId node = graphInput;
            bool audio = false, notes = false;
            uint32_t port = 0;
            std::vector<NoteZone> zones;
            double level = 1, pan = 0;
            bool mute = false;
            uint32_t split = 0, lane = 0;
        };
        auto audioWire = [&](const End& e, NodeId to, uint32_t toPort) {
            Wire w; w.from = e.node; w.fromPort = e.port; w.to = to; w.toPort = toPort;
            auto* added = add(w);
            if(e.split) { added->level = e.level; added->pan = e.pan; added->mute = e.mute; added->solo = false; added->split = e.split; added->lane = e.lane; }
            return added;
        };
        auto into = [&](const std::vector<End>& ends, NodeId node) {
            for(const auto& e : ends)
            {
                if(e.audio && takesAudio(graph_, layout, node)) audioWire(e, node, mainPort(graph_, layout, node, true));
                if(e.notes && takesNotes(graph_, node)) { Wire w; w.kind = WireKind::Notes; w.from = e.node; w.to = node; w.zones = e.zones; add(w); }
            }
            End next; next.node = node; next.audio = sendsAudio(graph_, layout, node); next.notes = sendsNotes(graph_, node);
            if(next.audio) next.port = mainPort(graph_, layout, node, false);
            return std::vector<End>{next};
        };
        std::function<std::vector<End>(const std::vector<NodeId>&, std::vector<End>)> walk = [&](const std::vector<NodeId>& chain, std::vector<End> ends) {
            for(const auto entry : chain)
            {
                if(!isSplit(entry)) { ends = into(ends, entry); continue; }
                const auto& split = *std::find_if(part.splits.begin(), part.splits.end(), [&](const Split& s) { return s.id == (entry & ~splitMark); });
                const bool solo = std::any_of(split.lanes.begin(), split.lanes.end(), [](const Lane& l) { return l.solo; });
                // Kinds with a Split node: the sound goes into it, and each lane's sound starts at
                // one of its outputs and ends at the same input of the Join node, if any.
                if(split.splitter) { auto sound = ends; for(auto& e : sound) e.notes = false; into(sound, split.splitter); }
                std::vector<End> joined;
                for(uint32_t i = 0; i < split.lanes.size(); ++i)
                {
                    const auto& lane = split.lanes[i];
                    auto in = ends;
                    for(auto& e : in)
                    {
                        if(split.splitter) e.audio = false;
                        if(e.notes && !lane.zones.empty())
                        {
                            e.zones = e.zones.empty() ? lane.zones : intersectZones(e.zones, lane.zones);
                            if(e.zones.empty()) e.notes = false; // no note gets through both
                        }
                    }
                    if(split.splitter)
                    {
                        End from; from.node = split.splitter; from.audio = true; from.port = graph_.host(split.splitter).outputPorts().at(i).id;
                        std::erase_if(in, [](const End& e) { return !e.notes; });
                        in.push_back(from);
                    }
                    for(auto out : walk(lane.chain, in))
                    {
                        out.level *= lane.level; out.pan = std::clamp(out.pan + lane.pan, -1.0, 1.0);
                        out.mute = out.mute || lane.mute || (solo && !lane.solo);
                        if(!out.split) { out.split = split.id; out.lane = i; }
                        if(split.joiner && out.audio) { audioWire(out, split.joiner, graph_.host(split.joiner).inputPorts().at(i).id); out.audio = false; }
                        if(out.audio || out.notes) joined.push_back(out);
                    }
                }
                if(split.joiner)
                {
                    End j; j.node = split.joiner; j.audio = sendsAudio(graph_, layout, split.joiner);
                    if(j.audio) j.port = mainPort(graph_, layout, split.joiner, false);
                    joined.push_back(j);
                }
                ends = std::move(joined);
            }
            return ends;
        };
        End input; input.audio = part.input.kind == PartInput::Kind::Audio; input.notes = part.input.kind == PartInput::Kind::Notes;
        input.port = part.input.port; input.zones = inputZones(part.input, hostStatus_.midiInputs);
        // A wire into a bus's lane also takes that lane's Mix.
        auto enter = [](Wire* w, const End& e, const BusEntry& d, double level, bool mute) {
            w->level = level * e.level * d.level; w->mute = mute || e.mute || d.mute;
            if(d.split) { w->pan = std::clamp(e.pan + d.pan, -1.0, 1.0); if(!e.split) { w->split = d.split; w->lane = d.lane; } }
        };
        const auto destinations = part.bus ? busEntries(part.bus) : std::vector<BusEntry>{{graphOutput, part.output}};
        const bool mute = part.mute || !heard(part);
        for(auto e : walk(part.chain, {input}))
        {
            if(!e.audio) continue;
            for(const auto& d : destinations) { auto* w = audioWire(e, d.node, d.port); w->fader = true; enter(w, e, d, part.level, mute); }
            for(const auto& send : part.sends)
                for(const auto& d : busEntries(send.bus))
                {
                    auto* s = audioWire(e, d.node, d.port);
                    s->send = send.bus; enter(s, e, d, send.level * part.level, mute); // post-fader
                }
        }
    }

    // Whether sound from part `from` can reach part `to` through outputs and sends.
    bool Session::feeds(uint32_t from, uint32_t to)
    {
        if(from == to) return true;
        const auto& p = findPart(from);
        if(p.bus && feeds(p.bus, to)) return true;
        return std::any_of(p.sends.begin(), p.sends.end(), [&](const Send& s) { return feeds(s.bus, to); });
    }

    namespace
    {
        void checkZones(const std::vector<NoteZone>& zones)
        {
            if(zones.size() > NoteZones::capacity || std::any_of(zones.begin(), zones.end(), [](const NoteZone& z) { return !z.valid(); }))
                throw std::invalid_argument("invalid or too many note zones");
        }
        void checkInput(const PartInput& input)
        {
            checkZones(input.zones);
            if(input.devices.size() > 16 || std::any_of(input.devices.begin(), input.devices.end(), [](const DeviceRule& r) { return !r.valid(); }))
                throw std::invalid_argument("invalid or too many device rules");
        }
    }

    Part& Session::findPart(uint32_t id)
    {
        const auto it = std::find_if(parts_.begin(), parts_.end(), [&](const Part& p) { return p.id == id; });
        if(it == parts_.end()) throw std::invalid_argument("unknown part");
        return *it;
    }

    uint32_t Session::addPart(const std::string& name, PartInput input, std::optional<uint32_t> output)
    {
        const auto layout = graph_.describe().audioLayout;
        if(layout.outputs.empty()) throw std::invalid_argument("the graph has no audio outputs");
        checkInput(input);
        Part part{nextPart_, name, input, {}, {}, output.value_or(layout.outputs.front().id)};
        if(!channelsOf(graph_, layout, graphOutput, part.output, true)) throw std::invalid_argument("no such graph output port");
        if(input.kind == PartInput::Kind::Audio && !channelsOf(graph_, layout, graphInput, input.port, false)) throw std::invalid_argument("no such graph input port");
        editWires([&] { parts_.push_back(part); buildParts(); });
        return nextPart_++;
    }

    void Session::setPartInput(uint32_t id, const PartInput& input)
    {
        findPart(id);
        checkInput(input);
        const auto layout = graph_.describe().audioLayout;
        if(input.kind == PartInput::Kind::Audio && !channelsOf(graph_, layout, graphInput, input.port, false)) throw std::invalid_argument("no such graph input port");
        editWires([&] { findPart(id).input = input; buildParts(); });
    }

    Part& Session::findBus(uint32_t id)
    {
        auto& bus = findPart(id);
        if(bus.input.kind != PartInput::Kind::Bus) throw std::invalid_argument("not a bus");
        return bus;
    }

    void Session::setPartOutput(uint32_t id, uint32_t bus, std::optional<uint32_t> port)
    {
        findPart(id);
        if(bus) { findBus(bus); if(feeds(bus, id)) throw std::invalid_argument("that would loop the bus into itself"); }
        const auto layout = graph_.describe().audioLayout;
        const auto output = port.value_or(layout.outputs.front().id);
        if(!bus && !channelsOf(graph_, layout, graphOutput, output, true)) throw std::invalid_argument("no such graph output port");
        editWires([&] { auto& p = findPart(id); p.bus = bus; if(!bus) p.output = output; buildParts(); });
    }

    void Session::setSend(uint32_t id, uint32_t bus, std::optional<double> level)
    {
        findPart(id); findBus(bus);
        if(level && (!std::isfinite(*level) || *level < 0)) throw std::invalid_argument("invalid send level");
        if(level && feeds(bus, id)) throw std::invalid_argument("that would loop the bus into itself");
        editWires([&] {
            auto& sends = findPart(id).sends;
            const auto it = std::find_if(sends.begin(), sends.end(), [&](const Send& s) { return s.bus == bus; });
            if(!level) { if(it != sends.end()) sends.erase(it); }
            else if(it != sends.end()) it->level = *level;
            else sends.push_back({bus, *level});
            buildParts();
        });
    }

    bool Session::heard(const Part& part)
    {
        if(std::none_of(parts_.begin(), parts_.end(), [](const Part& p) { return p.solo; })) return true;
        return std::any_of(parts_.begin(), parts_.end(), [&](const Part& q) { return q.solo && (feeds(part.id, q.id) || feeds(q.id, part.id)); });
    }

    void Session::setPartSolo(uint32_t id, bool solo)
    {
        findPart(id);
        editWires([&] { findPart(id).solo = solo; buildParts(); });
    }

    // Each part's peak is the largest of its boxes' peaks times the wire carrying them to where it
    // plays; a muted wire shows nothing. The held value falls by about 17 dB a second at the
    // desktop's 5 ms control tick.
    // shortcut: lanes and pans are not summed, so a part with several lanes shows its loudest; measure the summed bus if that misleads.
    void Session::meter()
    {
        std::map<NodeId, float> peaks;
        for(const auto& w : wires_) if(w.fader && w.owner == WireOwner::Part && !peaks.count(w.from)) peaks[w.from] = graph_.takePeak(w.from);
        std::map<uint32_t, double> now;
        for(const auto& w : wires_) if(w.fader && w.owner == WireOwner::Part && !w.mute) now[w.part] = std::max(now[w.part], peaks[w.from] * w.level);
        std::map<uint32_t, double> held;
        for(const auto& p : parts_) held[p.id] = std::max(now[p.id], meters_.count(p.id) ? meters_[p.id] * .99 : 0.0);
        meters_.swap(held);
    }

    std::vector<PartMeter> Session::partMeters() const
    {
        std::vector<PartMeter> out;
        for(const auto& [part, peak] : meters_) out.push_back({part, peak});
        return out;
    }

    void Session::setPartLevel(uint32_t id, double level, bool mute)
    {
        findPart(id);
        if(!std::isfinite(level) || level < 0) throw std::invalid_argument("invalid part level");
        editWires([&] { auto& p = findPart(id); p.level = level; p.mute = mute; buildParts(); });
    }

    void Session::setPartName(uint32_t id, const std::string& name)
    {
        findPart(id);
        editWires([&] { findPart(id).name = name; });
    }

    double MacroTarget::map(double value) const
    {
        const double x = std::clamp(value, 0.0, 1.0);
        double shaped = x;
        switch(curve)
        {
        case Curve::Linear: break;
        case Curve::Exponential: shaped = (std::exp(4 * x) - 1) / (std::exp(4.0) - 1); break;
        case Curve::Logarithmic: shaped = std::log1p((std::exp(4.0) - 1) * x) / 4; break;
        case Curve::SCurve: shaped = x * x * (3 - 2 * x); break;
        }
        if(relative) return std::clamp(base + (shaped - .5) * (end - start), 0.0, 1.0);
        return std::clamp(start + (end - start) * shaped, 0.0, 1.0);
    }

    Macro& Session::findMacro(uint32_t id)
    {
        const auto it = std::find_if(macros_.begin(), macros_.end(), [&](const Macro& m) { return m.id == id; });
        if(it == macros_.end()) throw std::invalid_argument("unknown control");
        return *it;
    }

    // A target's present value as a fraction of its range: where a relative target moves from.
    double Session::currentFraction(const MacroTarget& t) const
    {
        switch(t.kind)
        {
        case MacroTarget::Kind::Parameter:
        {
            const auto info = parameterInfo(t.node, t.param);
            return info.maximum > info.minimum ? (parameter(t.node, t.param) - info.minimum) / (info.maximum - info.minimum) : 0;
        }
        case MacroTarget::Kind::PartLevel: for(const auto& p : parts_) if(p.id == t.part) return std::clamp(p.level, 0.0, 1.0); return 1;
        case MacroTarget::Kind::PartMute: for(const auto& p : parts_) if(p.id == t.part) return p.mute ? 1 : 0; return 0;
        case MacroTarget::Kind::Macro: for(const auto& m : macros_) if(m.id == t.macro) return m.value; return 0;
        }
        return 0;
    }

    uint32_t Session::addMacro(const std::string& name)
    {
        macros_.push_back({nextMacro_, name, 0, {}});
        return nextMacro_++;
    }

    void Session::removeMacro(uint32_t id)
    {
        findMacro(id);
        std::erase_if(macros_, [&](const Macro& m) { return m.id == id; });
        for(auto& m : macros_) std::erase_if(m.targets, [&](const MacroTarget& t) { return t.kind == MacroTarget::Kind::Macro && t.macro == id; });
        for(auto& s : scenes_) std::erase_if(s.macros, [&](const auto& v) { return v.first == id; });
    }

    void Session::setMacroName(uint32_t id, const std::string& name) { findMacro(id).name = name; }

    void Session::setMacroTargets(uint32_t id, const std::vector<MacroTarget>& requested)
    {
        findMacro(id);
        auto targets = requested;
        if(targets.size() > 256) throw std::invalid_argument("at most 256 targets per control");
        for(auto& t : targets)
        {
            if(!std::isfinite(t.start) || !std::isfinite(t.end) || t.start < 0 || t.start > 1 || t.end < 0 || t.end > 1) throw std::invalid_argument("a target's range is within 0..1");
            switch(t.kind)
            {
            case MacroTarget::Kind::Parameter:
                if(!graph_.contains(t.node) || !hasParameter(t.node, t.param) || (parameterInfo(t.node, t.param).flags & CLAP_PARAM_IS_READONLY)) throw std::invalid_argument("not a writable parameter");
                break;
            case MacroTarget::Kind::PartLevel: case MacroTarget::Kind::PartMute: findPart(t.part); break;
            case MacroTarget::Kind::Macro:
            {
                findMacro(t.macro);
                // No loops: the target must not lead back to this control.
                std::vector<uint32_t> pending{t.macro}, seen;
                while(!pending.empty())
                {
                    const auto m = pending.back(); pending.pop_back();
                    if(m == id) throw std::invalid_argument("that would make the controls loop");
                    if(std::find(seen.begin(), seen.end(), m) != seen.end()) continue;
                    seen.push_back(m);
                    for(const auto& next : findMacro(m).targets) if(next.kind == MacroTarget::Kind::Macro) pending.push_back(next.macro);
                }
                break;
            }
            }
            // A relative target moves around its value when it became relative; one that already
            // was keeps that base, so editing a control's other targets does not shift it.
            if(t.relative)
            {
                const auto& before = findMacro(id).targets;
                const auto same = std::find_if(before.begin(), before.end(), [&](const MacroTarget& o) {
                    return o.relative && o.kind == t.kind && o.node == t.node && o.param == t.param && o.part == t.part && o.macro == t.macro; });
                t.base = same != before.end() ? same->base : currentFraction(t);
            }
        }
        findMacro(id).targets = std::move(targets);
    }

    // Sets a control and, through its targets, everything below it; part faders are rebuilt once.
    void Session::driveMacro(uint32_t id, double value, bool& faders)
    {
        auto& macro = findMacro(id);
        macro.value = std::clamp(value, 0.0, 1.0);
        const auto targets = macro.targets; // `macro` may move as children are driven
        for(const auto& t : targets)
        {
            const double v = t.map(macro.value);
            switch(t.kind)
            {
            case MacroTarget::Kind::Parameter:
                if(graph_.contains(t.node) && hasParameter(t.node, t.param))
                {
                    const auto info = parameterInfo(t.node, t.param);
                    setParameter(t.node, t.param, info.minimum + (info.maximum - info.minimum) * v);
                }
                break;
            case MacroTarget::Kind::PartLevel: for(auto& p : parts_) if(p.id == t.part) { p.level = v; faders = true; } break;
            case MacroTarget::Kind::PartMute: for(auto& p : parts_) if(p.id == t.part) { p.mute = v >= .5; faders = true; } break;
            case MacroTarget::Kind::Macro: driveMacro(t.macro, v, faders); break;
            }
        }
    }

    void Session::setMacro(uint32_t id, double value)
    {
        if(!std::isfinite(value)) throw std::invalid_argument("a control's value is 0..1");
        findMacro(id);
        bool faders = false;
        driveMacro(id, value, faders);
        // shortcut: part faders recompile the graph plan on every move; fine at knob rates on the
        // desktop, make them direct gain updates before devices sweep them continuously.
        if(faders) { editWires([&] { buildParts(); }); commit(); }
    }

    Scene& Session::findScene(uint32_t id)
    {
        const auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == id; });
        if(it == scenes_.end()) throw std::invalid_argument("unknown scene");
        return *it;
    }

    // A scene records every part's fader and each scene parameter's value, pending edits included.
    void Session::capture(Scene& scene) const
    {
        scene.parts.clear(); scene.values.clear();
        for(const auto& p : parts_) scene.parts.push_back({p.id, p.level, p.mute});
        for(const auto& at : sceneParameters_) if(graph_.contains(at.node)) scene.values.push_back({at, parameter(at.node, at.param)});
        scene.macros.clear();
        for(const auto& m : macros_) scene.macros.push_back({m.id, m.value});
    }

    uint32_t Session::addScene(const std::string& name)
    {
        Scene scene; scene.id = nextScene_; scene.name = name;
        capture(scene);
        scenes_.push_back(std::move(scene)); scene_ = nextScene_;
        return nextScene_++;
    }

    void Session::storeScene(uint32_t id) { capture(findScene(id)); scene_ = id; }

    // Faders change route gains in a new plan and parameters arrive as events at the next block,
    // so nothing is rebuilt: held notes and tails go on. Staged graph edits are committed with it.
    void Session::selectScene(uint32_t id)
    {
        const auto scene = findScene(id);
        // Controls first: the scene's own part faders and parameter values then have the last word.
        bool faders = false;
        for(const auto& [macro, value] : scene.macros) if(std::any_of(macros_.begin(), macros_.end(), [&](const Macro& m) { return m.id == macro; })) driveMacro(macro, value, faders);
        editWires([&] {
            for(const auto& state : scene.parts)
                for(auto& p : parts_) if(p.id == state.part) { p.level = state.level; p.mute = state.mute; }
            buildParts();
        });
        commit();
        for(const auto& v : scene.values)
            if(graph_.contains(v.at.node) && hasParameter(v.at.node, v.at.param)) setParameter(v.at.node, v.at.param, v.value);
        scene_ = id;
    }

    void Session::removeScene(uint32_t id)
    {
        findScene(id);
        std::erase_if(scenes_, [&](const Scene& s) { return s.id == id; });
        if(scene_ == id) scene_ = 0;
    }

    void Session::setSceneName(uint32_t id, const std::string& name) { findScene(id).name = name; }

    void Session::setSceneParameters(const std::vector<SceneParameter>& parameters)
    {
        for(size_t i = 0; i < parameters.size(); ++i)
        {
            const auto& at = parameters[i];
            if(!graph_.contains(at.node) || !hasParameter(at.node, at.param) || (parameterInfo(at.node, at.param).flags & CLAP_PARAM_IS_READONLY))
                throw std::invalid_argument("not a writable parameter");
            if(std::find(parameters.begin(), parameters.begin() + std::ptrdiff_t(i), at) != parameters.begin() + std::ptrdiff_t(i)) throw std::invalid_argument("a scene parameter listed twice");
        }
        // Each scene keeps its values for parameters still listed; a new one takes its current value.
        for(auto& scene : scenes_)
        {
            std::vector<Scene::Value> values;
            for(const auto& at : parameters)
            {
                const auto had = std::find_if(scene.values.begin(), scene.values.end(), [&](const Scene::Value& v) { return v.at == at; });
                values.push_back(had != scene.values.end() ? *had : Scene::Value{at, parameter(at.node, at.param)});
            }
            scene.values = std::move(values);
        }
        sceneParameters_ = parameters;
    }

    void Session::setHostStatus(SessionStatus status)
    {
        const bool moved = status.midiInputs != hostStatus_.midiInputs;
        hostStatus_ = std::move(status);
        if(!moved || std::none_of(parts_.begin(), parts_.end(), [](const Part& p) { return !p.input.devices.empty(); })) return;
        const bool staged = staged_;
        try { editWires([&] { buildParts(); }); if(!staged) commit(); }
        catch(const std::exception& e) { std::fprintf(stderr, "oscairn: device selectors not re-resolved: %s\n", e.what()); }
    }

    void Session::setZones(uint32_t id, const std::vector<NoteZone>& zones)
    {
        checkZones(zones);
        const auto it = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; });
        if(it == wires_.end() || it->kind != WireKind::Notes) throw std::invalid_argument("unknown note wire");
        editWires([&] { std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; })->zones = zones; });
    }

    void Session::removePart(uint32_t id)
    {
        findPart(id);
        editWires([&] {
            std::erase_if(parts_, [&](const Part& p) { return p.id == id; });
            std::erase_if(wires_, [&](const Wire& w) { return w.owner == WireOwner::Part && w.part == id; });
            for(auto& scene : scenes_) std::erase_if(scene.parts, [&](const Scene::PartState& s) { return s.part == id; });
            for(auto& m : macros_) std::erase_if(m.targets, [&](const MacroTarget& t) { return (t.kind == MacroTarget::Kind::PartLevel || t.kind == MacroTarget::Kind::PartMute) && t.part == id; });
            // Parts that played through a removed bus play to the main output; their sends to it end.
            const auto main = graph_.describe().audioLayout.outputs.front().id;
            for(auto& p : parts_)
            {
                if(p.bus == id) { p.bus = 0; p.output = main; }
                std::erase_if(p.sends, [&](const Send& s) { return s.bus == id; });
            }
            buildParts();
        });
    }

    namespace
    {
        Split& splitOf(Part& p, uint32_t id)
        {
            const auto it = std::find_if(p.splits.begin(), p.splits.end(), [&](const Split& s) { return s.id == id; });
            if(it == p.splits.end()) throw std::invalid_argument("unknown split");
            return *it;
        }
        std::vector<NodeId>& chainAt(Part& p, ChainPlace place)
        {
            if(!place.split) return p.chain;
            auto& split = splitOf(p, place.split);
            if(place.lane >= split.lanes.size()) throw std::invalid_argument("unknown lane");
            return split.lanes[place.lane].chain;
        }
        // Every chain in the part: its own and each lane's.
        template<class F> void eachChain(Part& p, F&& f) { f(p.chain); for(auto& s : p.splits) for(auto& l : s.lanes) f(l.chain); }
        bool holds(Part& p, NodeId entry)
        {
            bool found = false;
            eachChain(p, [&](std::vector<NodeId>& c) { found = found || std::find(c.begin(), c.end(), entry) != c.end(); });
            return found;
        }
        // Forgets the splits in a chain that is leaving the part, and those inside them; returns
        // them, so their Split and Join nodes can go too.
        std::vector<Split> forgetSplits(Part& p, const std::vector<NodeId>& chain)
        {
            std::vector<Split> gone;
            for(const auto entry : chain)
                if(isSplit(entry))
                {
                    const auto split = splitOf(p, entry & ~splitMark);
                    for(const auto& l : split.lanes) { auto inner = forgetSplits(p, l.chain); gone.insert(gone.end(), inner.begin(), inner.end()); }
                    std::erase_if(p.splits, [&](const Split& s) { return s.id == split.id; });
                    gone.push_back(split);
                }
            return gone;
        }
        void checkLane(const Lane& l)
        {
            if(!std::isfinite(l.level) || l.level < 0 || !std::isfinite(l.pan) || l.pan < -1 || l.pan > 1) throw std::invalid_argument("invalid lane level or pan");
            if(l.zones.size() > NoteZones::capacity || std::any_of(l.zones.begin(), l.zones.end(), [](const NoteZone& z) { return !z.valid(); }))
                throw std::invalid_argument("invalid or too many note zones");
        }
    }

    bool Session::splitNode(NodeId node) const
    {
        for(const auto& p : parts_) for(const auto& s : p.splits) if(node && (s.splitter == node || s.joiner == node)) return true;
        return false;
    }

    void Session::dropSplitNodes(const Split& split)
    {
        for(const auto node : {split.splitter, split.joiner})
            if(node) { std::erase_if(wires_, [&](const Wire& w) { return w.from == node || w.to == node; }); dropNode(node); }
    }

    void Session::insertIntoPart(uint32_t id, size_t index, NodeId node, ChainPlace place)
    {
        if(isSplit(node) || !graph_.contains(node)) throw std::invalid_argument("unknown node");
        if(splitNode(node)) throw std::invalid_argument("the node belongs to a split");
        auto& part = findPart(id);
        if(index > chainAt(part, place).size()) throw std::invalid_argument("chain position out of range");
        if(holds(part, node)) throw std::invalid_argument("the node is already in this part");
        editWires([&] { auto& c = chainAt(findPart(id), place); c.insert(c.begin() + std::ptrdiff_t(index), node); buildParts(); });
    }

    void Session::removeFromPart(uint32_t id, NodeId node)
    {
        auto& part = findPart(id);
        if(isSplit(node) || !holds(part, node)) throw std::invalid_argument("the node is not in this part");
        editWires([&] { eachChain(findPart(id), [&](std::vector<NodeId>& c) { std::erase(c, node); }); buildParts(); });
    }

    void Session::movePartNode(uint32_t id, size_t from, size_t to, ChainPlace place)
    {
        auto& part = findPart(id);
        const auto size = chainAt(part, place).size();
        if(from >= size || to >= size) throw std::invalid_argument("chain position out of range");
        editWires([&] {
            auto& c = chainAt(findPart(id), place);
            const auto entry = c[from];
            c.erase(c.begin() + std::ptrdiff_t(from));
            c.insert(c.begin() + std::ptrdiff_t(to), entry);
            buildParts();
        });
    }

    uint32_t Session::addSplit(uint32_t id, size_t index, ChainPlace place, Split::Kind kind, NodeId splitter, NodeId joiner)
    {
        auto& part = findPart(id);
        if(index > chainAt(part, place).size()) throw std::invalid_argument("chain position out of range");
        // Kinds other than copy need their own Split node, and left/right and mid/side a Join node.
        auto pluginOf = [&](NodeId node) {
            for(const auto& n : graph_.describe().nodes) if(n.id == node) return n.plugin.id;
            return std::string();
        };
        auto free = [&](NodeId node) { return !splitNode(node) && std::none_of(parts_.begin(), parts_.end(), [&](Part& p) { return holds(p, node); }); };
        const bool joins = kind == Split::Kind::LeftRight || kind == Split::Kind::MidSide;
        if(kind == Split::Kind::Copy ? (splitter || joiner) :
           (pluginOf(splitter) != splitPlugin || !free(splitter) || (joins ? pluginOf(joiner) != joinPlugin || !free(joiner) || joiner == splitter : joiner != 0)))
            throw std::invalid_argument("this kind of split needs its own Split node, and a Join node for left/right and mid/side");
        uint32_t split = 1;
        for(const auto& s : part.splits) split = std::max(split, s.id + 1);
        if(split >= splitMark) throw std::invalid_argument("too many splits");
        editWires([&] {
            auto& p = findPart(id);
            auto& c = chainAt(p, place);
            c.insert(c.begin() + std::ptrdiff_t(index), splitMark | split);
            p.splits.push_back({split, kind, {Lane{}, Lane{}}, splitter, joiner});
            buildParts();
        });
        // The nodes' modes (plugins/splitters): Split 0 left/right, 1 mid/side, 2 bands, with the band count; Join likewise, 2 a sum.
        if(splitter) { setParameter(splitter, 1, kind == Split::Kind::LeftRight ? 0 : kind == Split::Kind::MidSide ? 1 : 2); setParameter(splitter, 2, 2); }
        if(joiner) setParameter(joiner, 1, kind == Split::Kind::LeftRight ? 0 : 1);
        return split;
    }

    void Session::removeSplit(uint32_t id, uint32_t split)
    {
        splitOf(findPart(id), split);
        editWires([&] {
            auto& p = findPart(id);
            const auto removed = splitOf(p, split);
            const auto& lanes = removed.lanes;
            for(size_t i = 1; i < lanes.size(); ++i) for(const auto& gone : forgetSplits(p, lanes[i].chain)) dropSplitNodes(gone);
            dropSplitNodes(removed);
            std::erase_if(p.splits, [&](const Split& s) { return s.id == split; });
            eachChain(p, [&](std::vector<NodeId>& c) {
                if(const auto at = std::find(c.begin(), c.end(), splitMark | split); at != c.end())
                    c.insert(c.erase(at), lanes[0].chain.begin(), lanes[0].chain.end());
            });
            buildParts();
        });
    }

    uint32_t Session::addLane(uint32_t id, uint32_t split)
    {
        uint32_t lane = 0;
        const auto& s = splitOf(findPart(id), split);
        if(s.kind == Split::Kind::LeftRight || s.kind == Split::Kind::MidSide) throw std::invalid_argument("left/right and mid/side splits have two lanes");
        if(s.lanes.size() >= (s.kind == Split::Kind::Bands ? 4u : 16u)) throw std::invalid_argument(s.kind == Split::Kind::Bands ? "at most 4 bands" : "at most 16 lanes");
        editWires([&] { auto& lanes = splitOf(findPart(id), split).lanes; lane = uint32_t(lanes.size()); lanes.emplace_back(); buildParts(); });
        if(s.kind == Split::Kind::Bands) setParameter(s.splitter, 2, double(lane + 1));
        return lane;
    }

    void Session::removeLane(uint32_t id, uint32_t split, uint32_t lane)
    {
        const auto& s = splitOf(findPart(id), split);
        if(lane >= s.lanes.size()) throw std::invalid_argument("unknown lane");
        if(s.kind != Split::Kind::Copy && s.lanes.size() <= 2) throw std::invalid_argument("this split keeps at least two lanes; remove the split instead");
        const auto kind = s.kind; const auto splitter = s.splitter;
        editWires([&] {
            auto& p = findPart(id);
            const auto chain = splitOf(p, split).lanes[lane].chain;
            for(const auto& gone : forgetSplits(p, chain)) dropSplitNodes(gone);
            auto& lanes = splitOf(p, split).lanes;
            lanes.erase(lanes.begin() + std::ptrdiff_t(lane));
            if(lanes.empty())
            {
                std::erase_if(p.splits, [&](const Split& s) { return s.id == split; });
                eachChain(p, [&](std::vector<NodeId>& c) { std::erase(c, splitMark | split); });
            }
            buildParts();
        });
        if(kind == Split::Kind::Bands) setParameter(splitter, 2, double(splitOf(findPart(id), split).lanes.size()));
    }

    void Session::setLane(uint32_t id, uint32_t split, uint32_t lane, const Lane& settings)
    {
        checkLane(settings);
        if(lane >= splitOf(findPart(id), split).lanes.size()) throw std::invalid_argument("unknown lane");
        editWires([&] {
            auto& l = splitOf(findPart(id), split).lanes[lane];
            l.zones = settings.zones; l.level = settings.level; l.pan = settings.pan; l.mute = settings.mute; l.solo = settings.solo;
            buildParts();
        });
    }

    uint32_t Session::connectPorts(const Wire& request)
    {
        Wire wire = request;
        const auto layout = graph_.describe().audioLayout;
        wire.owner = WireOwner::User; wire.part = 0; wire.id = nextWire_;
        if(!std::isfinite(wire.level) || wire.level < 0 || !std::isfinite(wire.pan) || wire.pan < -1 || wire.pan > 1) throw std::invalid_argument("invalid level or pan");
        checkZones(wire.zones);
        if(wire.kind == WireKind::Audio && !wire.zones.empty()) throw std::invalid_argument("audio wires have no note zones");
        if(wire.kind == WireKind::Notes)
        {
            if(!sendsNotes(graph_, wire.from)) throw std::invalid_argument("the source has no note outputs");
            if(!takesNotes(graph_, wire.to)) throw std::invalid_argument("the destination has no note inputs");
        }
        else if(!pairs(graph_, layout, wire, 1))
        {
            if(!channelsOf(graph_, layout, wire.from, wire.fromPort, false) || !channelsOf(graph_, layout, wire.to, wire.toPort, true)) throw std::invalid_argument("no such audio port");
            throw std::invalid_argument("these ports' channels need an explicit channel choice");
        }
        editWires([&] { wires_.push_back(wire); });
        return nextWire_++;
    }

    void Session::disconnectWire(uint32_t id)
    {
        if(std::none_of(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; })) throw std::invalid_argument("unknown wire");
        editWires([&] { std::erase_if(wires_, [&](const Wire& w) { return w.id == id; }); });
    }

    void Session::setMix(uint32_t id, double level, double pan, bool mute, bool solo)
    {
        if(!std::isfinite(level) || level < 0 || !std::isfinite(pan) || pan < -1 || pan > 1) throw std::invalid_argument("invalid level or pan");
        const auto it = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; });
        if(it == wires_.end() || it->kind != WireKind::Audio) throw std::invalid_argument("unknown audio wire");
        if(it->split) throw std::invalid_argument("this wire joins a split's lane: set the lane's Mix");
        editWires([&] {
            auto& w = *std::find_if(wires_.begin(), wires_.end(), [&](const Wire& x) { return x.id == id; });
            w.level = level; w.pan = pan; w.mute = mute; w.solo = solo;
            // A part's output and send wires carry its fader: setting them sets the part and send.
            if(w.owner == WireOwner::Part && (w.fader || w.send))
            {
                auto& p = findPart(w.part);
                if(w.send) { for(auto& s : p.sends) if(s.bus == w.send) s.level = p.level > 0 ? level / p.level : level; }
                else { p.level = level; p.mute = mute; }
                buildParts();
            }
        });
    }

    // The channel-level and node-level connection calls are user wires too.
    void Session::connectAudio(NodeId from, NodeId to)
    {
        const auto layout = graph_.describe().audioLayout;
        if(!sendsAudio(graph_, layout, from)) throw std::invalid_argument(from == graphInput || graph_.contains(from) ? "the source has no audio outputs" : "no such source node");
        if(!takesAudio(graph_, layout, to)) throw std::invalid_argument(to == graphOutput || graph_.contains(to) ? "the destination has no audio inputs" : "no such destination node");
        Wire w; w.from = from; w.to = to; w.fromPort = mainPort(graph_, layout, from, false); w.toPort = mainPort(graph_, layout, to, true);
        connectPorts(w);
    }

    // Removes channel pairs between two ports from every wire carrying them; a wire on the
    // channel rules becomes explicit first.
    void Session::removePairs(const AudioConnection& c)
    {
        const auto layout = graph_.describe().audioLayout;
        std::set<std::pair<uint32_t, uint32_t>> gone;
        for(uint32_t i = 0; i < c.channels; ++i) gone.insert({c.from.channel + i, c.to.channel + i});
        for(auto& w : wires_)
        {
            if(w.kind != WireKind::Audio || w.from != c.from.node || w.to != c.to.node || w.fromPort != c.from.port || w.toPort != c.to.port) continue;
            if(w.channels.empty()) if(const auto compiled = pairs(graph_, layout, w, 1)) for(const auto& p : *compiled) w.channels.push_back({p.from, p.to});
            std::erase_if(w.channels, [&](const auto& p) { return gone.count(p); });
            if(w.channels.empty()) w.id = 0; // emptied
        }
        std::erase_if(wires_, [](const Wire& w) { return w.kind == WireKind::Audio && w.id == 0; });
    }

    void Session::connectAudio(const AudioConnection& c)
    {
        if(c.channels == 0 || c.channels > Graph::maxAudioChannels || !std::isfinite(c.gain)) throw std::invalid_argument("invalid audio connection");
        Wire w; w.from = c.from.node; w.to = c.to.node; w.fromPort = c.from.port; w.toPort = c.to.port; w.level = c.gain;
        for(uint32_t i = 0; i < c.channels; ++i) w.channels.push_back({c.from.channel + i, c.to.channel + i});
        const auto layout = graph_.describe().audioLayout;
        if(!pairs(graph_, layout, w, 1)) throw std::invalid_argument("no such audio port or channel range");
        // Reconnecting channel pairs replaces their gain, as in the graph.
        editWires([&] { removePairs(c); w.id = nextWire_++; wires_.push_back(w); });
    }

    void Session::disconnectAudio(const AudioConnection& c) { editWires([&] { removePairs(c); }); }

    void Session::connectNotes(NodeId from, NodeId to)
    {
        if(std::any_of(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.kind == WireKind::Notes && w.owner == WireOwner::User && w.from == from && w.to == to; })) return;
        Wire w; w.kind = WireKind::Notes; w.from = from; w.to = to;
        connectPorts(w);
    }

    void Session::disconnectAudio(NodeId from, NodeId to)
    {
        editWires([&] { std::erase_if(wires_, [&](const Wire& w) { return w.kind == WireKind::Audio && w.from == from && w.to == to; }); });
    }

    void Session::disconnectNotes(NodeId from, NodeId to)
    {
        editWires([&] { std::erase_if(wires_, [&](const Wire& w) { return w.kind == WireKind::Notes && w.from == from && w.to == to; }); });
    }

    void Session::remove(NodeId node, UserWires userWires)
    {
        if(!graph_.contains(node)) throw std::invalid_argument("unknown node");
        if(splitNode(node)) throw std::invalid_argument("the node belongs to a split: remove the split instead");
        editWires([&] {
            if(userWires == UserWires::Reconnect)
            {
                std::vector<Wire> joined;
                for(const auto& in : wires_) if(in.owner == WireOwner::User && in.to == node)
                    for(const auto& out : wires_) if(out.owner == WireOwner::User && out.from == node && out.kind == in.kind)
                    {
                        Wire w = in; w.to = out.to; w.toPort = out.toPort; w.channels.clear(); w.id = nextWire_++;
                        joined.push_back(w);
                    }
                wires_.insert(wires_.end(), joined.begin(), joined.end());
            }
            std::erase_if(wires_, [&](const Wire& w) { return w.from == node || w.to == node; });
            dropNode(node);
            for(auto& p : parts_) eachChain(p, [&](std::vector<NodeId>& c) { std::erase(c, node); });
            buildParts();
            std::erase_if(sceneParameters_, [&](const SceneParameter& at) { return at.node == node; });
            for(auto& m : macros_) std::erase_if(m.targets, [&](const MacroTarget& t) { return t.kind == MacroTarget::Kind::Parameter && t.node == node; });
            for(auto& scene : scenes_) std::erase_if(scene.values, [&](const Scene::Value& v) { return v.at.node == node; });
        });
    }

    // Wires on a replaced node move from each old port to the new port in the same role: main
    // to main, the other ports in order. One with no counterpart stays, unconnected.
    void Session::remapPorts(NodeId node, const std::vector<kick::clap::Port>& oldInputs, const std::vector<kick::clap::Port>& oldOutputs,
                             const std::vector<uint32_t>& oldMainIn, const std::vector<uint32_t>& oldMainOut)
    {
        const auto& host = graph_.host(node);
        auto counterpart = [&](uint32_t port, bool input) -> uint32_t {
            const auto& before = input ? oldInputs : oldOutputs;
            const auto& oldMain = input ? oldMainIn : oldMainOut;
            std::vector<uint32_t> newMain;
            try { newMain = host.mainAudioPorts(input); } catch(const std::exception&) {}
            if(port == unmappedMainPort || std::find(oldMain.begin(), oldMain.end(), port) != oldMain.end()) return newMain.empty() ? unmappedMainPort : newMain.front();
            std::vector<uint32_t> others, newOthers;
            for(const auto& p : before) if(std::find(oldMain.begin(), oldMain.end(), p.id) == oldMain.end()) others.push_back(p.id);
            for(const auto& p : input ? host.inputPorts() : host.outputPorts()) if(std::find(newMain.begin(), newMain.end(), p.id) == newMain.end()) newOthers.push_back(p.id);
            const auto at = std::find(others.begin(), others.end(), port);
            if(at == others.end() || size_t(at - others.begin()) >= newOthers.size()) return unmappedPort;
            return newOthers[size_t(at - others.begin())];
        };
        for(auto& w : wires_)
        {
            if(w.kind != WireKind::Audio) continue;
            if(w.from == node) w.fromPort = counterpart(w.fromPort, false);
            if(w.to == node) w.toPort = counterpart(w.toPort, true);
        }
        buildParts();
    }
}

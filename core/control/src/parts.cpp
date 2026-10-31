// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 1: parts, wire ownership, implicit Mixes and port-level routes. The Session
// keeps the wires; every edit compiles them into the graph's channel routes and note edges.
#include "oscairn/session.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
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

    // Compiles the wires into the graph's routes, changing only what differs, and marks each
    // wire connected or not.
    void Session::compileWires(Graph& g, std::vector<Wire>& wires)
    {
        const auto layout = g.describe().audioLayout;
        using Key = std::tuple<NodeId, uint32_t, uint32_t, NodeId, uint32_t, uint32_t>;
        std::map<Key, double> audio;
        std::set<std::pair<NodeId, NodeId>> notes;
        // Each input's implicit Mix: a soloed source silences the unsoloed ones.
        std::set<std::pair<NodeId, uint32_t>> soloed;
        for(const auto& w : wires) if(w.kind == WireKind::Audio && w.solo) soloed.insert({w.to, w.toPort});
        for(auto& w : wires)
        {
            if(w.kind == WireKind::Notes)
            {
                w.connected = sendsNotes(g, w.from) && takesNotes(g, w.to);
                if(w.connected) notes.insert({w.from, w.to});
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
        for(const auto& e : present.notes) if(!notes.count(e)) g.disconnectNotes(e.first, e.second);
        for(const auto& e : notes) if(std::find(present.notes.begin(), present.notes.end(), e) == present.notes.end()) g.connectNotes(e.first, e.second);
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

    // Rebuilds one part's own wires: its input into the first box, box to box, the last box to
    // its output. A rebuilt wire keeps the Mix settings of the one it replaces.
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
        };
        auto audioWire = [&](NodeId from, uint32_t fromPort, NodeId to, uint32_t toPort) { Wire w; w.from = from; w.fromPort = fromPort; w.to = to; w.toPort = toPort; add(w); };
        auto noteWire = [&](NodeId from, NodeId to) { Wire w; w.kind = WireKind::Notes; w.from = from; w.to = to; add(w); };
        // The previous box, or the input; whether it sends audio and notes.
        NodeId previousNode = graphInput;
        bool audio = part.input.kind == PartInput::Kind::Audio, notes = part.input.kind == PartInput::Kind::Notes;
        uint32_t audioPort = part.input.port;
        for(const auto node : part.chain)
        {
            if(audio && takesAudio(graph_, layout, node)) audioWire(previousNode, audioPort, node, mainPort(graph_, layout, node, true));
            if(notes && takesNotes(graph_, node)) noteWire(previousNode, node);
            previousNode = node;
            audio = sendsAudio(graph_, layout, node); notes = sendsNotes(graph_, node);
            if(audio) audioPort = mainPort(graph_, layout, node, false);
        }
        if(audio) audioWire(previousNode, audioPort, graphOutput, part.output);
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
        Part part{nextPart_, name, input, {}, output.value_or(layout.outputs.front().id)};
        if(!channelsOf(graph_, layout, graphOutput, part.output, true)) throw std::invalid_argument("no such graph output port");
        if(input.kind == PartInput::Kind::Audio && !channelsOf(graph_, layout, graphInput, input.port, false)) throw std::invalid_argument("no such graph input port");
        editWires([&] { parts_.push_back(part); buildPart(parts_.back()); });
        return nextPart_++;
    }

    void Session::removePart(uint32_t id)
    {
        findPart(id);
        editWires([&] {
            std::erase_if(parts_, [&](const Part& p) { return p.id == id; });
            std::erase_if(wires_, [&](const Wire& w) { return w.owner == WireOwner::Part && w.part == id; });
        });
    }

    void Session::insertIntoPart(uint32_t id, size_t index, NodeId node)
    {
        if(!graph_.contains(node)) throw std::invalid_argument("unknown node");
        auto& part = findPart(id);
        if(index > part.chain.size()) throw std::invalid_argument("chain position out of range");
        if(std::find(part.chain.begin(), part.chain.end(), node) != part.chain.end()) throw std::invalid_argument("the node is already in this part");
        editWires([&] { auto& p = findPart(id); p.chain.insert(p.chain.begin() + std::ptrdiff_t(index), node); buildPart(p); });
    }

    void Session::removeFromPart(uint32_t id, NodeId node)
    {
        auto& part = findPart(id);
        if(std::find(part.chain.begin(), part.chain.end(), node) == part.chain.end()) throw std::invalid_argument("the node is not in this part");
        editWires([&] { auto& p = findPart(id); std::erase(p.chain, node); buildPart(p); });
    }

    void Session::movePartNode(uint32_t id, size_t from, size_t to)
    {
        auto& part = findPart(id);
        if(from >= part.chain.size() || to >= part.chain.size()) throw std::invalid_argument("chain position out of range");
        editWires([&] {
            auto& p = findPart(id);
            const auto node = p.chain[from];
            p.chain.erase(p.chain.begin() + std::ptrdiff_t(from));
            p.chain.insert(p.chain.begin() + std::ptrdiff_t(to), node);
            buildPart(p);
        });
    }

    uint32_t Session::connectPorts(const Wire& request)
    {
        Wire wire = request;
        const auto layout = graph_.describe().audioLayout;
        wire.owner = WireOwner::User; wire.part = 0; wire.id = nextWire_;
        if(!std::isfinite(wire.level) || wire.level < 0 || !std::isfinite(wire.pan) || wire.pan < -1 || wire.pan > 1) throw std::invalid_argument("invalid level or pan");
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
        editWires([&] { auto& w = *std::find_if(wires_.begin(), wires_.end(), [&](const Wire& x) { return x.id == id; }); w.level = level; w.pan = pan; w.mute = mute; w.solo = solo; });
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
            for(auto& p : parts_) if(std::find(p.chain.begin(), p.chain.end(), node) != p.chain.end()) { std::erase(p.chain, node); buildPart(p); }
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
        for(auto& p : parts_) if(std::find(p.chain.begin(), p.chain.end(), node) != p.chain.end()) buildPart(p);
    }
}

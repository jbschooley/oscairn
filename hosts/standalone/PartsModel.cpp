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
            row.edited = !p.from.sound.empty() && p.from.modified;
            row.draining = std::any_of(g.wires.begin(), g.wires.end(), [&](const Wire& w) {
                return w.owner == WireOwner::Part && w.part == p.id && w.from == graphInput && wireState(w, g.topology) == WireState::Draining; });
            row.level = p.level; row.pan = p.pan; row.mute = p.mute; row.solo = p.solo; row.through = p.bus;
            // Its own mute, or someone else's solo: the wires where it plays are muted either way.
            row.silenced = !p.mute && std::any_of(g.wires.begin(), g.wires.end(), [&](const Wire& w) { return w.owner == WireOwner::Part && w.part == p.id && w.fader && w.mute; });
            row.notes = p.input.kind == PartInput::Kind::Notes; row.devices = p.input.devices; row.maxNotes = p.input.maxNotes;
            for(const auto& s : p.sends) row.sends[s.bus] = s.level;
            if(!p.input.zones.empty())
            {
                const auto& z = p.input.zones.front();
                row.lowKey = z.lowKey; row.highKey = z.highKey; row.transpose = z.transpose;
                row.lowVelocity = int(std::lround(z.lowVelocity * 127)); row.highVelocity = int(std::lround(z.highVelocity * 127));
            }
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

    WireState wireState(const Wire& w, const Graph::Description& topology)
    {
        if(w.kind == WireKind::Notes && topology.draining.count({w.from, w.to})) return WireState::Draining;
        return w.mute ? WireState::Disabled : WireState::Enabled;
    }

    RoutingState readRouting(const Control::GraphState& g)
    {
        RoutingState state;
        state.wires = g.wires;
        const auto owner = owners(g);
        state.nodes.push_back({graphInput, notesPort, "Keys & MIDI", "Notes"});
        for(const auto& p : g.topology.audioLayout.inputs) state.nodes.push_back({graphInput, p.id, p.name.empty() ? "Audio in" : p.name, "Audio"});
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
            r.splitNode = splitter;
            if(const auto o = owner.find(id); o != owner.end()) { r.part = o->second->id; r.partName = o->second->name; }
            r.userWires = hasUserWires(g, id);
            state.nodes.push_back(r);
        }
        for(const auto& p : g.topology.audioLayout.outputs) state.nodes.push_back({graphOutput, p.id, p.name.empty() ? "Output" : p.name, "Output"});
        return state;
    }

    namespace
    {
        // One layered placement: elements with their sizes (first: the first column; last: the
        // last), and edges between them, each with its points from exit to entry.
        struct Element { int w = 0, h = 0; bool first = false, last = false; };
        struct Edge { int from = 0, to = 0; };
        struct Placed { std::vector<RoutingRect> rects; std::vector<std::vector<RoutingPoint>> paths; int width = 0, height = 0; };

        Placed place(const std::vector<Element>& elements, const std::vector<Edge>& edges, const RoutingMetrics& m)
        {
            const int n = int(elements.size());
            Placed out; out.rects.resize(static_cast<size_t>(n)); out.paths.resize(edges.size());
            if(!n) return out;
            std::vector<std::vector<int>> preds(static_cast<size_t>(n)), succs(static_cast<size_t>(n));
            for(const auto& e : edges) if(e.from != e.to) { preds[size_t(e.to)].push_back(e.from); succs[size_t(e.from)].push_back(e.to); }
            // Ranks: the longest path from the first column; a loop's elements follow what was ranked.
            std::vector<int> indegree(static_cast<size_t>(n)), order;
            for(int v = 0; v < n; ++v) indegree[size_t(v)] = int(preds[size_t(v)].size());
            for(int v = 0; v < n; ++v) if(!indegree[size_t(v)]) order.push_back(v);
            for(size_t i = 0; i < order.size(); ++i) for(const auto s : succs[size_t(order[i])]) if(--indegree[size_t(s)] == 0) order.push_back(s);
            for(int v = 0; v < n; ++v) if(std::find(order.begin(), order.end(), v) == order.end()) order.push_back(v);
            const bool anyFirst = std::any_of(elements.begin(), elements.end(), [](const Element& e) { return e.first; });
            std::vector<int> rank(static_cast<size_t>(n));
            for(int v = 0; v < n; ++v) rank[size_t(v)] = elements[size_t(v)].first || !anyFirst ? 0 : 1;
            for(const auto u : order) for(const auto s : succs[size_t(u)])
                if(!elements[size_t(s)].first && std::find(order.begin(), order.end(), s) > std::find(order.begin(), order.end(), u)) rank[size_t(s)] = std::max(rank[size_t(s)], rank[size_t(u)] + 1);
            int top = 0;
            for(int v = 0; v < n; ++v) if(!elements[size_t(v)].last) top = std::max(top, rank[size_t(v)]);
            for(int v = 0; v < n; ++v) if(elements[size_t(v)].last) rank[size_t(v)] = top + 1;
            const int columns = *std::max_element(rank.begin(), rank.end()) + 1;
            // Layers, with a slot for each wire in each column it crosses.
            struct Slot { int element = -1, edge = -1; };
            std::vector<std::vector<Slot>> layers(static_cast<size_t>(columns));
            for(int v = 0; v < n; ++v) layers[size_t(rank[size_t(v)])].push_back({v, -1});
            for(size_t i = 0; i < edges.size(); ++i)
                for(int r = rank[size_t(edges[i].from)] + 1; r < rank[size_t(edges[i].to)]; ++r) layers[size_t(r)].push_back({-1, int(i)});
            auto position = [&](int r, Slot s) {
                const auto& l = layers[size_t(r)];
                for(size_t i = 0; i < l.size(); ++i) if(l[i].element == s.element && l[i].edge == s.edge) return double(i);
                return -1.0;
            };
            // What a slot is joined to in the column before (up) or after.
            auto neighbours = [&](int r, const Slot& s, bool up) {
                std::vector<double> at;
                const int other = up ? r - 1 : r + 1;
                if(other < 0 || other >= columns) return at;
                if(s.element < 0)
                {
                    const auto& e = edges[size_t(s.edge)];
                    const int end = up ? e.from : e.to;
                    at.push_back(rank[size_t(end)] == other ? position(other, {end, -1}) : position(other, {-1, s.edge}));
                    return at;
                }
                for(size_t i = 0; i < edges.size(); ++i)
                {
                    const auto& e = edges[i];
                    const int self = up ? e.to : e.from, end = up ? e.from : e.to;
                    if(self != s.element || e.from == e.to) continue;
                    const bool forward = up ? rank[size_t(end)] < r : rank[size_t(end)] > r;
                    if(!forward) continue;
                    at.push_back(rank[size_t(end)] == other ? position(other, {end, -1}) : position(other, {-1, int(i)}));
                }
                return at;
            };
            for(int sweep = 0; sweep < 8; ++sweep)
            {
                const bool up = sweep % 2 == 0;
                for(int step = 0; step < columns; ++step)
                {
                    const int r = up ? step : columns - 1 - step;
                    auto& l = layers[size_t(r)];
                    std::vector<std::pair<double, Slot>> keyed;
                    for(size_t i = 0; i < l.size(); ++i)
                    {
                        const auto at = neighbours(r, l[i], up);
                        double key = double(i);
                        if(!at.empty()) { key = 0; for(const auto a : at) key += a; key /= double(at.size()); }
                        keyed.push_back({key, l[i]});
                    }
                    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                    for(size_t i = 0; i < l.size(); ++i) l[i] = keyed[i].second;
                }
            }
            // Pixels: columns left to right, each centred on the tallest.
            std::vector<int> x(static_cast<size_t>(columns)), width(static_cast<size_t>(columns), 0), height(static_cast<size_t>(columns), 0);
            for(int r = 0; r < columns; ++r)
            {
                for(const auto& s : layers[size_t(r)]) width[size_t(r)] = std::max(width[size_t(r)], s.element < 0 ? 0 : elements[size_t(s.element)].w);
                for(size_t i = 0; i < layers[size_t(r)].size(); ++i)
                    height[size_t(r)] += (i ? m.row : 0) + (layers[size_t(r)][i].element < 0 ? m.slot : elements[size_t(layers[size_t(r)][i].element)].h);
                x[size_t(r)] = r ? x[size_t(r) - 1] + width[size_t(r) - 1] + m.gap : 0;
            }
            out.width = x.back() + width.back();
            out.height = *std::max_element(height.begin(), height.end());
            std::map<std::pair<int, int>, double> dummyY; // (edge, column) -> its slot's middle
            for(int r = 0; r < columns; ++r)
            {
                int y = (out.height - height[size_t(r)]) / 2;
                for(const auto& s : layers[size_t(r)])
                {
                    if(s.element < 0) { dummyY[{s.edge, r}] = y + m.slot / 2.0; y += m.slot + m.row; continue; }
                    const auto& e = elements[size_t(s.element)];
                    out.rects[size_t(s.element)] = {x[size_t(r)], y, e.w, e.h};
                    y += e.h + m.row;
                }
            }
            // Ports: each wire its own place on an edge, in the order of what is at its other end.
            auto towards = [&](size_t i, bool exit) {
                const auto& e = edges[i];
                const int r = rank[size_t(exit ? e.from : e.to)], next = exit ? r + 1 : r - 1;
                if(const auto d = dummyY.find({int(i), next}); d != dummyY.end()) return d->second;
                const auto& o = out.rects[size_t(exit ? e.to : e.from)];
                return o.y + o.h / 2.0;
            };
            std::vector<double> exitY(edges.size()), entryY(edges.size());
            for(int v = 0; v < n; ++v)
            {
                for(const bool exit : {true, false})
                {
                    std::vector<size_t> mine;
                    for(size_t i = 0; i < edges.size(); ++i) if((exit ? edges[i].from : edges[i].to) == v && edges[i].from != edges[i].to) mine.push_back(i);
                    std::stable_sort(mine.begin(), mine.end(), [&](size_t a, size_t b) { return towards(a, exit) < towards(b, exit); });
                    const auto& rect = out.rects[size_t(v)];
                    for(size_t k = 0; k < mine.size(); ++k) (exit ? exitY : entryY)[mine[k]] = rect.y + rect.h * double(k + 1) / double(mine.size() + 1);
                }
            }
            bool back = false;
            for(size_t i = 0; i < edges.size(); ++i)
            {
                const auto& e = edges[i];
                if(e.from == e.to) continue;
                const auto& a = out.rects[size_t(e.from)]; const auto& b = out.rects[size_t(e.to)];
                const int ra = rank[size_t(e.from)], rb = rank[size_t(e.to)];
                auto& p = out.paths[i];
                p.push_back({double(a.x + a.w), exitY[i]});
                const double right = x[size_t(ra)] + width[size_t(ra)];
                if(right > a.x + a.w) p.push_back({right, exitY[i]}); // along its own row to the gap
                if(rb > ra)
                {
                    for(int r = ra + 1; r < rb; ++r) { const auto y = dummyY.at({int(i), r}); p.push_back({double(x[size_t(r)]), y}); p.push_back({double(x[size_t(r)] + width[size_t(r)]), y}); }
                }
                else
                {
                    // Back to an earlier column: below everything, through the gaps either side.
                    back = true;
                    const double below = out.height + m.row;
                    p.push_back({right + m.gap / 2.0, exitY[i]}); p.push_back({right + m.gap / 2.0, below});
                    p.push_back({b.x - m.gap / 2.0, below}); p.push_back({b.x - m.gap / 2.0, entryY[i]});
                }
                p.push_back({double(b.x), entryY[i]});
            }
            if(back) out.height += m.row * 2;
            return out;
        }
    }

    std::vector<RoutingPoint> routingCurve(const std::vector<RoutingPoint>& points, int steps)
    {
        std::vector<RoutingPoint> out;
        for(size_t i = 0; i + 1 < points.size(); ++i)
        {
            const auto p = points[i], q = points[i + 1];
            const double half = (q.x - p.x) / 2;
            for(int s = i ? 1 : 0; s <= steps; ++s)
            {
                const double t = double(s) / steps, u = 1 - t;
                const double cx1 = p.x + half, cx2 = q.x - half;
                out.push_back({u * u * u * p.x + 3 * u * u * t * cx1 + 3 * u * t * t * cx2 + t * t * t * q.x,
                               u * u * u * p.y + 3 * u * u * t * p.y + 3 * u * t * t * q.y + t * t * t * q.y});
            }
        }
        return out;
    }

    RoutingLayout layoutRouting(const Control::GraphState& g, bool collapsed, const std::set<uint32_t>& expanded, const RoutingMetrics& m)
    {
        RoutingLayout out;
        const auto routing = readRouting(g);
        const auto rows = readParts(g);
        auto open = [&](uint32_t part) { return !collapsed || expanded.count(part) != 0; };
        // The elements of the whole view: graph inputs and outputs, loose plugins and parts.
        struct Top { int node = -1; uint32_t part = 0; };  // node: an index into routing.nodes
        std::vector<Top> tops; std::vector<Element> elements;
        std::map<std::pair<NodeId, uint32_t>, int> topOf;  // (node, port) of an end, (node, 0) of a plugin
        std::map<uint32_t, int> topOfPart;
        std::map<NodeId, uint32_t> partOf;
        for(size_t i = 0; i < routing.nodes.size(); ++i)
        {
            const auto& n = routing.nodes[i];
            if(n.part)
            {
                partOf[n.node] = n.part;
                if(!topOfPart.count(n.part)) { topOfPart[n.part] = int(tops.size()); tops.push_back({-1, n.part}); elements.push_back({m.part, m.partHeight}); }
                topOf[{n.node, 0}] = topOfPart[n.part];
                continue;
            }
            const bool input = n.node == graphInput, output = n.node == graphOutput;
            topOf[{n.node, input || output ? n.port : 0}] = int(tops.size());
            tops.push_back({int(i), 0});
            elements.push_back({m.node, m.nodeHeight, input, output});
        }
        auto endOf = [&](const Wire& w, bool from) {
            const NodeId node = from ? w.from : w.to;
            if(node == graphInput) return topOf.count({node, w.kind == WireKind::Notes ? notesPort : w.fromPort}) ? topOf.at({node, w.kind == WireKind::Notes ? notesPort : w.fromPort}) : -1;
            if(node == graphOutput) return topOf.count({node, w.toPort}) ? topOf.at({node, w.toPort}) : -1;
            return topOf.count({node, 0}) ? topOf.at({node, 0}) : -1;
        };
        // Wires: between elements of the view, or inside an expanded part.
        struct Inner { std::vector<NodeId> nodes; std::vector<Element> elements; std::vector<Edge> edges; std::map<NodeId, int> index; int in = 0, out = 0; Placed placed; };
        std::map<uint32_t, Inner> inner;
        for(const auto& [part, top] : topOfPart)
        {
            if(!open(part)) continue;
            auto& in = inner[part];
            for(const auto& n : routing.nodes) if(n.part == part) { in.index[n.node] = int(in.nodes.size()); in.nodes.push_back(n.node); in.elements.push_back({m.node, m.nodeHeight}); }
            in.in = int(in.elements.size()); in.elements.push_back({0, m.nodeHeight, true, false});
            in.out = int(in.elements.size()); in.elements.push_back({0, m.nodeHeight, false, true});
        }
        std::vector<Edge> topEdges; std::vector<size_t> topWire;
        std::map<size_t, int> enterEdge, leaveEdge, innerEdge; // wire index -> edge index inside its part
        for(size_t i = 0; i < g.wires.size(); ++i)
        {
            const auto& w = g.wires[i];
            const int a = endOf(w, true), b = endOf(w, false);
            if(a < 0 || b < 0) continue;
            if(a == b)
            {
                if(!tops[size_t(a)].part || !open(tops[size_t(a)].part)) continue; // inside a collapsed part
                auto& in = inner[tops[size_t(a)].part];
                innerEdge[i] = int(in.edges.size()); in.edges.push_back({in.index.at(w.from), in.index.at(w.to)});
                continue;
            }
            topWire.push_back(i); topEdges.push_back({a, b});
            if(const auto pa = tops[size_t(a)].part; pa && open(pa)) { auto& in = inner[pa]; leaveEdge[i] = int(in.edges.size()); in.edges.push_back({in.index.at(w.from), in.out}); }
            if(const auto pb = tops[size_t(b)].part; pb && open(pb)) { auto& in = inner[pb]; enterEdge[i] = int(in.edges.size()); in.edges.push_back({in.in, in.index.at(w.to)}); }
        }
        auto inside = m; inside.gap = m.gap / 2; // a frame's plugins sit closer
        for(auto& [part, in] : inner)
        {
            in.placed = place(in.elements, in.edges, inside);
            elements[size_t(topOfPart.at(part))] = {in.placed.width + 2 * m.frame, in.placed.height + m.frameHead + m.frame};
        }
        const auto placed = place(elements, topEdges, m);
        out.width = placed.width; out.height = placed.height;
        // Boxes: frames first, then plugins, parts and ends over them.
        auto origin = [&](uint32_t part) { const auto& r = placed.rects[size_t(topOfPart.at(part))]; return RoutingPoint{double(r.x + m.frame), double(r.y + m.frameHead)}; };
        for(const auto& [part, in] : inner)
        {
            RoutingLayout::Box frame; frame.kind = RoutingLayout::Box::Kind::Frame; frame.part = part; frame.bounds = placed.rects[size_t(topOfPart.at(part))];
            for(const auto& r : rows.parts) if(r.id == part) frame.name = r.name;
            for(const auto& r : rows.buses) if(r.id == part) frame.name = r.name;
            out.boxes.push_back(frame);
        }
        for(size_t t = 0; t < tops.size(); ++t)
        {
            const auto& top = tops[t];
            if(top.part && open(top.part)) continue;
            RoutingLayout::Box box; box.bounds = placed.rects[t];
            if(top.part)
            {
                box.kind = RoutingLayout::Box::Kind::Part; box.part = top.part;
                for(const auto* list : {&rows.parts, &rows.buses}) for(const auto& r : *list) if(r.id == top.part)
                {
                    box.name = r.name; box.detail = r.bus ? "bus" : "";
                    for(const auto& c : r.chain) box.chips.push_back(c.split ? c.name + " · " + std::to_string(c.lanes.size()) + " lanes" : c.name);
                }
            }
            else
            {
                const auto& n = routing.nodes[size_t(top.node)];
                box.node = n.node; box.port = n.port; box.name = n.name; box.detail = n.node == graphInput || n.node == graphOutput ? "" : "no part";
            }
            out.boxes.push_back(box);
        }
        for(const auto& [part, in] : inner)
        {
            const auto o = origin(part);
            for(size_t k = 0; k < in.nodes.size(); ++k)
            {
                RoutingLayout::Box box; box.node = in.nodes[k]; box.part = part;
                for(const auto& n : routing.nodes) if(n.node == in.nodes[k]) { box.name = n.name; box.detail = n.partName; }
                const auto& r = in.placed.rects[k];
                box.bounds = {int(o.x) + r.x, int(o.y) + r.y, r.w, r.h};
                out.boxes.push_back(box);
            }
        }
        // Paths: inside a part to its frame's edge, across the view, into the other part.
        auto shifted = [&](uint32_t part, const std::vector<RoutingPoint>& points) {
            const auto o = origin(part); std::vector<RoutingPoint> p;
            for(const auto& q : points) p.push_back({q.x + o.x, q.y + o.y});
            return p;
        };
        for(size_t k = 0; k < topEdges.size(); ++k)
        {
            const auto i = topWire[k];
            const auto& w = g.wires[i];
            std::vector<RoutingPoint> points;
            const auto pa = tops[size_t(topEdges[k].from)].part, pb = tops[size_t(topEdges[k].to)].part;
            if(leaveEdge.count(i)) { auto p = shifted(pa, inner.at(pa).placed.paths[size_t(leaveEdge.at(i))]); p.back().x = placed.rects[size_t(topEdges[k].from)].x + placed.rects[size_t(topEdges[k].from)].w; points = p; }
            const auto& across = placed.paths[k];
            points.insert(points.end(), across.begin() + (points.empty() ? 0 : 1), across.end() - (enterEdge.count(i) ? 1 : 0));
            if(enterEdge.count(i)) { auto p = shifted(pb, inner.at(pb).placed.paths[size_t(enterEdge.at(i))]); p.front().x = placed.rects[size_t(topEdges[k].to)].x; points.insert(points.end(), p.begin(), p.end()); }
            out.paths.push_back({w, wireState(w, g.topology), points});
        }
        for(const auto& [i, e] : innerEdge)
        {
            const auto part = partOf.at(g.wires[i].from);
            out.paths.push_back({g.wires[i], wireState(g.wires[i], g.topology), shifted(part, inner.at(part).placed.paths[size_t(e)])});
        }
        return out;
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

    namespace
    {
        // A change to the part's first zone; a zone that passes everything is dropped.
        void editZone(Control& c, uint32_t part, const std::function<void(NoteZone&)>& change)
        {
        const auto parts = c.graphState().parts;
        const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.id == part; });
        if(found == parts.end()) throw std::invalid_argument("unknown part");
        auto input = found->input;
        NoteZone zone = input.zones.empty() ? NoteZone{} : input.zones.front();
        change(zone);
        const bool everything = zone == NoteZone{};
        if(input.zones.empty()) { if(!everything) input.zones.push_back(zone); }
        else if(everything && input.zones.size() == 1) input.zones.clear();
        else input.zones.front() = zone;
        c.setPartInput(part, input);
        c.commit();
        }
    }

    std::vector<PartField> partFields(const PartRow& p, const PartsState& state)
    {
        using K = PartField::Kind;
        std::vector<PartField> fields;
        // What plays it and how: only for a part that plays notes.
        if(p.notes) fields.insert(fields.end(), {{K::Devices, "Plays from"}, {K::NoteLimit, "Notes at once"}, {K::Zone, "Keys and velocity", 0}, {K::Transpose, "Transpose"}});
        fields.insert(fields.end(), {{K::Output, "Plays to"}, {K::Level, "Level"}});
        for(const auto& b : state.buses) if(b.id != p.id) fields.push_back({K::Send, "Send to " + b.name, 1, b.id});
        return fields;
    }

    void setZone(Control& c, uint32_t part, int lowKey, int highKey, int transpose)
    {
        editZone(c, part, [&](NoteZone& zone) {
            zone.lowKey = uint8_t(std::clamp(lowKey, 0, 127)); zone.highKey = uint8_t(std::clamp(highKey, int(zone.lowKey), 127));
            zone.transpose = int8_t(std::clamp(transpose, -48, 48));
        });
    }

    void setVelocities(Control& c, uint32_t part, int low, int high)
    {
        editZone(c, part, [&](NoteZone& zone) {
            low = std::clamp(low, 0, 127); high = std::clamp(high, low, 127);
            zone.lowVelocity = low / 127.0; zone.highVelocity = high / 127.0;
        });
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

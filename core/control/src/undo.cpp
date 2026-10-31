// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Undo and redo (RFC 0011). The Session keeps a baseline: its model (parts, wires, scenes,
// controls, pads, parameter mappings, and its nodes, held) as it was after the last edit. At each
// commit, and at each control tick when nothing is staged, a model that differs becomes a step
// with both. Undoing applies only what the step changed onto the model as it is now, so changes
// that are not edits (a scene switch since) stay. A removed or replaced node is held by the
// steps, so it comes back with its state and prepared sounds. Parameter values set through the
// control interface are steps of their own.
#include "oscairn/session.h"

#include <algorithm>
#include <set>
#include <tuple>

namespace oscairn
{
    namespace
    {
        constexpr size_t historyDepth = 100;
        constexpr auto mergeWindow = std::chrono::milliseconds(1500);

        // A part's wires are rebuilt from the part, with new IDs; they are known by where they go.
        using WireKey = std::tuple<uint32_t, int, NodeId, uint32_t, NodeId, uint32_t, uint32_t, uint32_t, uint32_t, bool>;
        WireKey keyOf(const Wire& w) { return {w.part, int(w.kind), w.from, w.fromPort, w.to, w.toPort, w.send, w.split, w.lane, w.fader}; }
        std::map<WireKey, Wire> partWires(const std::vector<Wire>& wires)
        {
            std::map<WireKey, Wire> out;
            for(const auto& w : wires) if(w.owner == WireOwner::Part) { auto x = w; x.id = 0; out[keyOf(w)] = x; }
            return out;
        }
        std::vector<Wire> userWires(const std::vector<Wire>& wires)
        {
            std::vector<Wire> out;
            for(const auto& w : wires) if(w.owner == WireOwner::User) out.push_back(w);
            return out;
        }
        // A control's value moves when it is played; its name and targets are what is edited.
        bool sameMacro(const Macro& a, const Macro& b) { return a.id == b.id && a.name == b.name && a.targets == b.targets; }
        bool sameMacros(const std::vector<Macro>& a, const std::vector<Macro>& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), sameMacro);
        }
        bool sameMappings(const std::vector<Graph::Mapping>& a, const std::vector<Graph::Mapping>& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const Graph::Mapping& x, const Graph::Mapping& y) {
                return x.source == y.source && x.lane == y.lane && x.target == y.target && x.param == y.param;
            });
        }
        template<class Map> bool sameNodes(const Map& a, const Map& b)
        {
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const auto& x, const auto& y) {
                return x.first == y.first && x.second.first == y.second.first && x.second.second.host == y.second.second.host;
            });
        }

        // Applies the change from `from` to `to` onto `now`, entity by entity (by ID), keeping
        // `to`'s order for an entity put back. `equal` says whether two versions are the same;
        // `assign(now, from, to)` changes an entity that is there now (from: its version before,
        // or null if the step added it).
        template<class T, class Id, class Equal, class Assign>
        void patch(std::vector<T>& now, const std::vector<T>& from, const std::vector<T>& to, Id id, Equal equal, Assign assign)
        {
            std::set<decltype(id(std::declval<const T&>()))> keys;
            for(const auto& x : from) keys.insert(id(x));
            for(const auto& x : to) keys.insert(id(x));
            for(const auto& k : keys)
            {
                const auto f = std::find_if(from.begin(), from.end(), [&](const T& x) { return id(x) == k; });
                const auto t = std::find_if(to.begin(), to.end(), [&](const T& x) { return id(x) == k; });
                if(f != from.end() && t != to.end() && equal(*f, *t)) continue;
                const auto n = std::find_if(now.begin(), now.end(), [&](const T& x) { return id(x) == k; });
                if(t == to.end()) { if(n != now.end()) now.erase(n); continue; }
                if(n != now.end()) assign(*n, f != from.end() ? &*f : nullptr, *t);
                else now.insert(now.begin() + std::ptrdiff_t(std::min<size_t>(size_t(t - to.begin()), now.size())), *t);
            }
        }
        template<class T, class Id> void patch(std::vector<T>& now, const std::vector<T>& from, const std::vector<T>& to, Id id)
        {
            patch(now, from, to, id, [](const T& a, const T& b) { return a == b; }, [](T& n, const T*, const T& t) { n = t; });
        }
    }

    Session::Quiet::Quiet(Session& session) : s(session) { s.record(); ++s.quiet_; }
    Session::Quiet::~Quiet()
    {
        --s.quiet_;
        try { s.baseline_ = std::make_shared<const Model>(s.model()); } catch(...) {}
    }

    Session::Model Session::model() const
    {
        Model m{parts_, wires_, scenes_, sceneParameters_, macros_, padMap_, graph_.describe().mappings, {}};
        for(const auto& [id, name] : nodes_) m.nodes.emplace(id, std::pair{name, graph_.node(id)});
        return m;
    }

    void Session::record()
    {
        if(quiet_) return;
        auto now = std::make_shared<const Model>(model());
        if(!baseline_) { baseline_ = now; return; }
        const auto& b = *baseline_;
        const bool same = b.parts == now->parts && userWires(b.wires) == userWires(now->wires) && partWires(b.wires) == partWires(now->wires) &&
                          b.scenes == now->scenes && b.sceneParameters == now->sceneParameters && sameMacros(b.macros, now->macros) &&
                          b.pads == now->pads && sameMappings(b.mappings, now->mappings) && sameNodes(b.nodes, now->nodes);
        if(!same)
        {
            Step step;
            std::tie(step.label, step.key) = summarize(b, *now);
            step.before = baseline_; step.after = now;
            push(std::move(step));
        }
        baseline_ = now;
    }

    void Session::push(Step step)
    {
        step.when = std::chrono::steady_clock::now();
        if(mergeable_ && !undo_.empty() && !step.key.empty())
        {
            auto& last = undo_.back();
            if(last.key == step.key && step.when - last.when < mergeWindow)
            {
                if(step.after) last.after = step.after; else last.to = step.to;
                last.when = step.when;
                redo_.clear();
                return;
            }
        }
        undo_.push_back(std::move(step));
        if(undo_.size() > historyDepth) undo_.erase(undo_.begin()); // its held nodes go with it
        redo_.clear();
        mergeable_ = true;
    }

    bool Session::setParameter(NodeId node, uint32_t param, double value)
    {
        const double before = parameter(node, param);
        double applied = value;
        const bool ok = applyParameter(node, param, value, &applied);
        if(ok && !quiet_ && applied != before)
        {
            record(); // an edit waiting for its step comes first
            Step step;
            std::string name = std::to_string(param);
            for(const auto& p : parameters(node)) if(p.id == param) name = p.name;
            step.label = (nodes_.count(node) ? nodes_.at(node) + " " : std::string()) + name;
            step.key = "param:" + std::to_string(node) + ":" + std::to_string(param);
            step.node = node; step.param = param; step.from = before; step.to = applied;
            push(std::move(step));
        }
        return ok;
    }

    Control::History Session::history() const
    {
        return {undo_.empty() ? std::string() : undo_.back().label, redo_.empty() ? std::string() : redo_.back().label};
    }

    bool Session::undo()
    {
        record();
        if(undo_.empty()) return false;
        auto step = std::move(undo_.back()); undo_.pop_back();
        mergeable_ = false;
        if(step.before) apply(*step.after, *step.before);
        else if(graph_.contains(step.node)) applyParameter(step.node, step.param, step.from);
        redo_.push_back(std::move(step));
        return true;
    }

    bool Session::redo()
    {
        record();
        if(redo_.empty()) return false;
        auto step = std::move(redo_.back()); redo_.pop_back();
        mergeable_ = false;
        if(step.before) apply(*step.before, *step.after);
        else if(graph_.contains(step.node)) applyParameter(step.node, step.param, step.to);
        undo_.push_back(std::move(step));
        return true;
    }

    void Session::apply(const Model& from, const Model& to)
    {
        ++quiet_;
        try
        {
            // Nodes first: chains and wires refer to them. A node put back is the one held, with
            // its state; one that goes is held by the step for redo.
            std::set<NodeId> ids;
            for(const auto& [id, n] : from.nodes) ids.insert(id);
            for(const auto& [id, n] : to.nodes) ids.insert(id);
            for(const auto id : ids)
            {
                const auto f = from.nodes.find(id), t = to.nodes.find(id);
                if(f != from.nodes.end() && t != to.nodes.end() && f->second.first == t->second.first && f->second.second.host == t->second.second.host) continue;
                if(t == to.nodes.end())
                {
                    if(graph_.contains(id) && graph_.node(id).host == f->second.second.host)
                    {
                        std::erase_if(wires_, [&](const Wire& w) { return w.from == id || w.to == id; });
                        dropNode(id);
                    }
                    continue;
                }
                if(!graph_.contains(id) || graph_.node(id).host != t->second.second.host)
                {
                    graph_.restore(id, t->second.second);
                    auto& host = graph_.host(id);
                    for(const auto& p : host.parameters()) observed_[{id, p.id}] = host.parameterValue(p.id);
                }
                nodes_[id] = t->second.first;
            }

            // A part changes only in what the step changed: undoing a rename keeps a level a scene set since.
            patch(parts_, from.parts, to.parts, [](const Part& p) { return p.id; }, [](const Part& a, const Part& b) { return a == b; },
                  [](Part& n, const Part* f, const Part& t) {
                      if(!f) { n = t; return; }
                      auto field = [](auto& now, const auto& before, const auto& after) { if(!(before == after)) now = after; };
                      field(n.name, f->name, t.name); field(n.input, f->input, t.input); field(n.chain, f->chain, t.chain);
                      field(n.splits, f->splits, t.splits); field(n.output, f->output, t.output); field(n.bus, f->bus, t.bus);
                      field(n.sends, f->sends, t.sends); field(n.level, f->level, t.level); field(n.pan, f->pan, t.pan); field(n.mute, f->mute, t.mute); field(n.solo, f->solo, t.solo);
                  });
            // User wires by ID; a part's own wires are rebuilt from it, keeping a changed Mix.
            auto user = userWires(wires_);
            patch(user, userWires(from.wires), userWires(to.wires), [](const Wire& w) { return w.id; });
            const auto fromPart = partWires(from.wires), toPart = partWires(to.wires);
            std::vector<Wire> rebuilt = user;
            for(auto w : wires_)
            {
                if(w.owner != WireOwner::Part) continue;
                const auto k = keyOf(w);
                const auto f = fromPart.find(k), t = toPart.find(k);
                if(t != toPart.end() && (f == fromPart.end() || f->second.level != t->second.level || f->second.pan != t->second.pan ||
                                         f->second.mute != t->second.mute || f->second.solo != t->second.solo))
                    { w.level = t->second.level; w.pan = t->second.pan; w.mute = t->second.mute; w.solo = t->second.solo; }
                rebuilt.push_back(w);
            }
            wires_ = std::move(rebuilt);
            for(const auto& w : wires_) nextWire_ = std::max(nextWire_, w.id + 1);

            patch(scenes_, from.scenes, to.scenes, [](const Scene& s) { return s.id; });
            if(from.sceneParameters != to.sceneParameters) sceneParameters_ = to.sceneParameters;
            patch(macros_, from.macros, to.macros, [](const Macro& m) { return m.id; }, sameMacro,
                  [](Macro& n, const Macro*, const Macro& t) { const auto value = n.value; n = t; n.value = value; });
            if(!(from.pads == to.pads)) { padMap_ = to.pads; heldPads_ = {}; }
            if(!sameMappings(from.mappings, to.mappings))
            {
                for(const auto& m : graph_.describe().mappings) graph_.unmapParamTarget(m.source, m.lane);
                for(const auto& m : to.mappings) if(graph_.contains(m.source) && graph_.contains(m.target)) graph_.mapParamTarget(m.source, m.lane, m.target, m.param);
            }
            for(const auto& p : parts_) nextPart_ = std::max(nextPart_, p.id + 1);
            for(const auto& s : scenes_) nextScene_ = std::max(nextScene_, s.id + 1);
            for(const auto& m : macros_) nextMacro_ = std::max(nextMacro_, m.id + 1);

            buildParts();
            syncWires();
            // A split's nodes follow its kind and lanes.
            for(const auto& p : parts_)
                for(const auto& s : p.splits)
                {
                    const double mode = s.kind == Split::Kind::LeftRight ? 0 : s.kind == Split::Kind::MidSide ? 1 : 2;
                    if(s.splitter && graph_.contains(s.splitter)) { applyParameter(s.splitter, 1, mode); applyParameter(s.splitter, 2, double(std::clamp<size_t>(s.lanes.size(), 2, 4))); }
                    if(s.joiner && graph_.contains(s.joiner)) applyParameter(s.joiner, 1, mode);
                }
            commit();
        }
        catch(...)
        {
            --quiet_;
            baseline_ = std::make_shared<const Model>(model());
            throw;
        }
        --quiet_;
        baseline_ = std::make_shared<const Model>(model());
    }

    // What a step did, for the Undo button, and the one thing it changed, for merging.
    std::pair<std::string, std::string> Session::summarize(const Model& a, const Model& b)
    {
        {
            auto nameOf = [&](NodeId id) {
                if(const auto i = b.nodes.find(id); i != b.nodes.end()) return i->second.first;
                if(const auto i = a.nodes.find(id); i != a.nodes.end()) return i->second.first;
                return std::string("a plugin");
            };
            for(const auto& [id, n] : b.nodes)
                if(!a.nodes.count(id)) return {"Add " + n.first, ""};
                else if(a.nodes.at(id).second.host != n.second.host) return {"Replace " + n.first, ""};
            for(const auto& [id, n] : a.nodes) if(!b.nodes.count(id)) return {"Remove " + nameOf(id), ""};

            // Parts: the one changed, and what about it.
            std::vector<std::pair<const Part*, const Part*>> changed;
            for(const auto& p : b.parts)
            {
                const auto o = std::find_if(a.parts.begin(), a.parts.end(), [&](const Part& x) { return x.id == p.id; });
                if(o == a.parts.end()) changed.push_back({nullptr, &p});
                else if(!(*o == p)) changed.push_back({&*o, &p});
            }
            for(const auto& p : a.parts)
                if(std::none_of(b.parts.begin(), b.parts.end(), [&](const Part& x) { return x.id == p.id; })) changed.push_back({&p, nullptr});
            if(changed.size() == 1)
            {
                const auto [o, p] = changed.front();
                if(!o) return {"Add " + p->name, ""};
                if(!p) return {"Remove " + o->name, ""};
                const auto key = "part:" + std::to_string(p->id) + ":";
                Part rest = *o;
                rest.level = p->level; rest.mute = p->mute;
                if(rest == *p) return {p->name + " level", key + "level"};
                rest = *o; rest.pan = p->pan;
                if(rest == *p) return {p->name + " pan", key + "pan"};
                rest = *o; rest.sends = p->sends;
                if(rest == *p) return {p->name + " sends", key + "sends"};
                rest = *o; rest.solo = p->solo;
                if(rest == *p) return {p->name + " solo", ""};
                rest = *o; rest.name = p->name;
                if(rest == *p) return {"Rename " + p->name, key + "name"};
                rest = *o; rest.splits = p->splits;
                if(rest == *p) return {p->name + " lanes", key + "lanes"};
                rest = *o; rest.input = p->input;
                if(rest == *p) return {p->name + " input", ""};
                return {"Edit " + p->name, ""};
            }
            if(!changed.empty()) return {"Edit parts", ""};

            if(userWires(a.wires) != userWires(b.wires))
            {
                const auto x = userWires(a.wires), y = userWires(b.wires);
                if(x.size() == y.size())
                    for(size_t i = 0; i < x.size(); ++i)
                        if(!(x[i] == y[i]) && x[i].id == y[i].id) return {"Mix of " + nameOf(y[i].from), "wire:" + std::to_string(y[i].id)};
                return {"Wiring", ""};
            }
            if(partWires(a.wires) != partWires(b.wires)) return {"Mix", ""};
            if(a.scenes != b.scenes)
            {
                if(a.scenes.size() < b.scenes.size()) return {"Add scene " + b.scenes.back().name, ""};
                if(a.scenes.size() > b.scenes.size()) return {"Remove a scene", ""};
                for(size_t i = 0; i < b.scenes.size(); ++i) if(!(a.scenes[i] == b.scenes[i])) return {"Scene " + b.scenes[i].name, ""};
            }
            if(a.sceneParameters != b.sceneParameters) return {"Scene controls", ""};
            if(!sameMacros(a.macros, b.macros))
            {
                if(a.macros.size() < b.macros.size()) return {"Add control " + b.macros.back().name, ""};
                if(a.macros.size() > b.macros.size()) return {"Remove a control", ""};
                for(size_t i = 0; i < b.macros.size(); ++i)
                    if(!sameMacro(a.macros[i], b.macros[i])) return {"Control " + b.macros[i].name, "macro:" + std::to_string(b.macros[i].id)};
            }
            if(!(a.pads == b.pads)) return {"Pads", "pads"};
            return {"Parameter mapping", ""};
        }
    }
}

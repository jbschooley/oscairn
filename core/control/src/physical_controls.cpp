// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Physical controls (RFC 0011): the default layers, the banks the knobs page through, and the
// Performance's overrides.
#include "oscairn/physical_controls.h"
#include "oscairn/session.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>

namespace oscairn
{
    bool validPhysicalControl(const std::string& control)
    {
        const auto colon = control.find(':');
        if(colon == std::string::npos) return false;
        const auto kind = control.substr(0, colon), number = control.substr(colon + 1);
        if(kind != "knob" && kind != "slider" && kind != "button") return false;
        return !number.empty() && number.size() <= 3 && number[0] != '0' && std::all_of(number.begin(), number.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    namespace
    {
        const Part* partOf(const AssignmentSource& s, uint32_t id)
        {
            for(const auto& p : s.parts) if(p.id == id) return &p;
            return nullptr;
        }
        // A chain's nodes in order, each split's lanes one after another.
        void flatten(const Part& p, const std::vector<NodeId>& chain, std::vector<NodeId>& out)
        {
            for(const auto entry : chain)
            {
                if(!isSplit(entry)) { out.push_back(entry); continue; }
                for(const auto& split : p.splits) if(split.id == (entry & ~splitMark)) for(const auto& lane : split.lanes) flatten(p, lane.chain, out);
            }
        }
        std::vector<const AssignableNode*> nodesOf(const AssignmentSource& s, const Part& p)
        {
            std::vector<NodeId> ids; flatten(p, p.chain, ids);
            std::vector<const AssignableNode*> out;
            for(const auto id : ids) if(const auto it = s.nodes.find(id); it != s.nodes.end()) out.push_back(&it->second);
            return out;
        }
        // The order knobs take a node's parameters in: by oscairn.ui priority, then the plugin's order.
        std::vector<const AssignableParam*> byPriority(const AssignableNode& n)
        {
            std::vector<const AssignableParam*> out;
            for(const auto& p : n.params) out.push_back(&p);
            std::stable_sort(out.begin(), out.end(), [](const AssignableParam* a, const AssignableParam* b) { return (a->priority ? a->priority : ~0u) < (b->priority ? b->priority : ~0u); });
            return out;
        }
        using Item = std::pair<Assignment, std::string>;
        // Items in banks of four: "Assign 1–4", "Assign 5–8".
        void inFours(std::vector<Bank>& banks, const std::string& name, AssignmentLayer layer, const std::vector<Item>& items)
        {
            for(size_t i = 0; i < items.size(); i += 4)
            {
                Bank b; b.layer = layer;
                b.name = name + " " + std::to_string(i + 1) + "\xE2\x80\x93" + std::to_string(i + 4);
                for(size_t k = 0; k < 4 && i + k < items.size(); ++k) { b.slots[k] = items[i + k].first; b.labels[k] = items[i + k].second; }
                banks.push_back(std::move(b));
            }
        }
        Item parameter(const AssignableNode& n, const AssignableParam& p) { Assignment a; a.kind = Assignment::Kind::Parameter; a.node = n.node; a.param = p.id; return {a, n.name + ": " + p.name}; }
        Item of(Assignment::Kind kind, uint32_t id, const std::string& label) { Assignment a; a.kind = kind; a.id = id; return {a, label}; }
        // Parts first, then buses: what sliders and buttons take in turn.
        std::vector<const Part*> faderOrder(const AssignmentSource& s)
        {
            std::vector<const Part*> out;
            for(const bool bus : {false, true}) for(const auto& p : s.parts) if((p.input.kind == PartInput::Kind::Bus) == bus) out.push_back(&p);
            return out;
        }
    }

    std::string assignmentName(const AssignmentSource& s, const Assignment& a)
    {
        switch(a.kind)
        {
        case Assignment::Kind::None: return {};
        case Assignment::Kind::Parameter:
            if(const auto n = s.nodes.find(a.node); n != s.nodes.end())
                for(const auto& p : n->second.params) if(p.id == a.param) return n->second.name + ": " + p.name;
            return {};
        case Assignment::Kind::Control: for(const auto& m : s.controls) if(m.id == a.id) return m.name; return {};
        case Assignment::Kind::PartLevel: if(const auto* p = partOf(s, a.id)) return p->name + " level"; return {};
        case Assignment::Kind::PartMute: if(const auto* p = partOf(s, a.id)) return p->name + " mute"; return {};
        case Assignment::Kind::Scene: for(const auto& [id, name] : s.scenes) if(id == a.id) return name; return {};
        }
        return {};
    }

    std::vector<Bank> knobBanks(const AssignmentSource& s, uint32_t partId)
    {
        std::vector<Bank> banks;
        const auto* part = partOf(s, partId);
        if(!part)
        {
            // No part selected: the Performance's own controls (Super Knobs and the others).
            std::vector<Item> items;
            for(const auto& m : s.controls) if(!m.part) items.push_back(of(Assignment::Kind::Control, m.id, m.name));
            inFours(banks, "Controls", AssignmentLayer::Convention, items);
            return banks;
        }
        const auto nodes = nodesOf(s, *part);
        // 2. The sound's own assignments: its controls.
        std::vector<Item> own;
        for(const auto& m : s.controls) if(m.part == partId) own.push_back(of(Assignment::Kind::Control, m.id, m.name));
        inFours(banks, "Assign", AssignmentLayer::Sound, own);
        // 3. The role banks, from its instruments' declared roles; a role none declares is blank.
        bool declared = false;
        for(const auto& n : nodes) for(const auto& p : n->params) declared = declared || !p.role.empty();
        if(declared)
        {
            for(const auto& rb : roleBanks)
            {
                Bank b; b.name = rb.name; b.layer = AssignmentLayer::Role;
                bool any = false;
                for(size_t k = 0; k < rb.roles.size(); ++k)
                    for(const auto* n : nodes)
                    {
                        if(n->effect || b.slots[k].kind != Assignment::Kind::None) continue;
                        for(const auto& p : n->params) if(p.role == rb.roles[k]) { std::tie(b.slots[k], b.labels[k]) = parameter(*n, p); any = true; break; }
                    }
                if(any) banks.push_back(std::move(b));
            }
        }
        else if(own.empty())
        {
            // Nothing declared and nothing assigned: Assign banks from its instrument's first parameters.
            const AssignableNode* first = nullptr;
            for(const auto* n : nodes) if(!first && n->instrument && !n->params.empty()) first = n;
            for(const auto* n : nodes) if(!first && !n->effect && !n->params.empty()) first = n;
            for(const auto* n : nodes) if(!first && !n->params.empty()) first = n;
            std::vector<Item> items;
            if(first) for(const auto* p : byPriority(*first)) if(items.size() < 8) items.push_back(parameter(*first, *p));
            inFours(banks, "Assign", AssignmentLayer::Role, items);
        }
        // 5. The effects bank: each effect's main control, in chain order.
        std::vector<Item> effects;
        for(const auto* n : nodes)
        {
            if(!n->effect || n->params.empty()) continue;
            const AssignableParam* main = nullptr;
            for(const auto& p : n->params) if(!main && p.role == mixRole) main = &p;
            if(!main) main = byPriority(*n).front();
            effects.push_back(parameter(*n, *main));
        }
        inFours(banks, "Effects", AssignmentLayer::Effects, effects);
        return banks;
    }

    uint32_t knobPages(const AssignmentSource& s, uint32_t knobs, uint32_t part)
    {
        const size_t perPage = std::max<size_t>(1, (knobs + 3) / 4), banks = knobBanks(s, part).size();
        return uint32_t(std::max<size_t>(1, (banks + perPage - 1) / perPage));
    }

    std::vector<ResolvedControl> resolveControls(const AssignmentSource& s, const PhysicalControls& device, uint32_t part, uint32_t page)
    {
        std::vector<ResolvedControl> out;
        // 1. Overrides, where what they name is still there.
        auto overridden = [&](ResolvedControl& r) {
            for(const auto& o : s.overrides)
                if(o.control == r.control)
                    if(const auto name = assignmentName(s, o.target); !name.empty()) { r.target = o.target; r.layer = AssignmentLayer::Override; r.label = name; r.bank.clear(); return true; }
            return false;
        };
        const auto banks = knobBanks(s, part);
        const size_t perPage = std::max<size_t>(1, (device.knobs + 3) / 4), pages = std::max<size_t>(1, (banks.size() + perPage - 1) / perPage);
        for(uint32_t i = 0; i < device.knobs; ++i)
        {
            ResolvedControl r; r.control = "knob:" + std::to_string(i + 1);
            const size_t bank = (page % pages) * perPage + i / 4;
            if(bank < banks.size()) { r.target = banks[bank].slots[i % 4]; r.label = banks[bank].labels[i % 4]; r.bank = banks[bank].name; if(r.target.kind != Assignment::Kind::None) r.layer = banks[bank].layer; }
            overridden(r);
            out.push_back(std::move(r));
        }
        // 4. Conventions: sliders play part levels, buttons part mutes, parts first, then buses.
        const auto order = faderOrder(s);
        for(const auto [kind, count, name, suffix] : {std::tuple{Assignment::Kind::PartLevel, device.sliders, "slider:", " level"}, std::tuple{Assignment::Kind::PartMute, device.buttons, "button:", " mute"}})
            for(uint32_t i = 0; i < count; ++i)
            {
                ResolvedControl r; r.control = name + std::to_string(i + 1);
                if(i < order.size()) { std::tie(r.target, r.label) = of(kind, order[i]->id, order[i]->name + suffix); r.layer = AssignmentLayer::Convention; }
                overridden(r);
                out.push_back(std::move(r));
            }
        return out;
    }

    AssignmentSource assignmentSource(const Control::GraphState& g, const std::vector<AssignmentOverride>& overrides,
                                      const std::function<std::vector<Control::ParamInfo>(NodeId)>& parameters,
                                      const std::function<std::string(NodeId, const Control::ParamInfo&)>& roles)
    {
        AssignmentSource s;
        s.parts = g.parts; s.controls = g.macros; s.overrides = overrides;
        for(const auto& scene : g.scenes) s.scenes.emplace_back(scene.id, scene.name);
        for(const auto& p : g.parts)
        {
            std::vector<NodeId> ids; flatten(p, p.chain, ids);
            for(const auto id : ids)
            {
                if(s.nodes.count(id)) continue;
                AssignableNode n; n.node = id;
                const auto name = g.names.find(id); n.name = name == g.names.end() ? "Plugin" : name->second;
                // An effect takes audio in and gives audio out; an instrument only gives it.
                for(const auto& info : g.topology.nodes)
                    if(info.id == id)
                    {
                        uint32_t in = 0, out = 0;
                        for(const auto& port : info.inputs) in += port.channels;
                        for(const auto& port : info.outputs) out += port.channels;
                        n.effect = in && out; n.instrument = out && !in;
                    }
                if(parameters)
                    for(const auto& p : parameters(id)) n.params.push_back({p.id, p.name, roles ? roles(id, p) : std::string(), p.priority});
                s.nodes.emplace(id, std::move(n));
            }
        }
        return s;
    }

    std::vector<AssignmentOverride> Session::assignments() const { return assignments_; }

    void Session::assign(const std::string& control, const Assignment& target)
    {
        if(!validPhysicalControl(control)) throw std::invalid_argument("a physical control is knob:N, slider:N or button:N");
        switch(target.kind)
        {
        case Assignment::Kind::None: break;
        case Assignment::Kind::Parameter:
        {
            if(!nodes_.count(target.node)) throw std::invalid_argument("unknown node");
            const auto& all = graph_.host(target.node).parameters();
            if(std::none_of(all.begin(), all.end(), [&](const auto& p) { return p.id == target.param; })) throw std::invalid_argument("unknown parameter");
            break;
        }
        case Assignment::Kind::Control: findMacro(target.id); break;
        case Assignment::Kind::PartLevel: case Assignment::Kind::PartMute: findPart(target.id); break;
        case Assignment::Kind::Scene: if(std::none_of(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == target.id; })) throw std::invalid_argument("unknown scene"); break;
        }
        std::erase_if(assignments_, [&](const AssignmentOverride& o) { return o.control == control; });
        if(target.kind != Assignment::Kind::None) assignments_.push_back({control, target});
    }
}

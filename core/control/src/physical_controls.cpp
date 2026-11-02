// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Physical controls (RFC 0011): the default layers, the banks the knobs page through, and the
// Performance's overrides.
#include "oscairn/physical_controls.h"
#include "oscairn/session.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <stdexcept>
#include <tuple>

namespace oscairn
{
    bool validPhysicalControl(const std::string& control)
    {
        const auto colon = control.find(':');
        if(colon == std::string::npos) return false;
        const auto kind = control.substr(0, colon), number = control.substr(colon + 1);
        static const std::set<std::string> kinds{"knob", "fader", "button", "wheel", "strip", "jog", "pedal", "footswitch", "aftertouch", "breath"};
        if(!kinds.count(kind)) return false;
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
        std::string numbered(const std::string& name, size_t from, size_t to) { return name + " " + std::to_string(from) + "\xE2\x80\x93" + std::to_string(to); }
        Bank blank(const std::string& name, BankKind kind, uint32_t width) { Bank b; b.name = name; b.kind = kind; b.slots.resize(width); b.labels.resize(width); return b; }
        // Items in banks as wide as the group: "Assign 1–6", "Assign 7–12".
        void chunk(std::vector<Bank>& banks, const std::string& name, BankKind kind, AssignmentLayer layer, const std::vector<Item>& items, uint32_t width)
        {
            for(size_t i = 0; i < items.size(); i += width)
            {
                auto b = blank(numbered(name, i + 1, i + width), kind, width); b.layer = layer;
                for(size_t k = 0; k < width && i + k < items.size(); ++k) { b.slots[k] = items[i + k].first; b.labels[k] = items[i + k].second; }
                banks.push_back(std::move(b));
            }
        }
        Item parameter(const AssignableNode& n, const AssignableParam& p) { Assignment a; a.kind = Assignment::Kind::Parameter; a.node = n.node; a.param = p.id; return {a, n.name + ": " + p.name}; }
        Item of(Assignment::Kind kind, uint32_t id, const std::string& label) { Assignment a; a.kind = kind; a.id = id; return {a, label}; }
        // Parts first, then buses: what faders and buttons take in turn.
        std::vector<const Part*> faderOrder(const AssignmentSource& s)
        {
            std::vector<const Part*> out;
            for(const bool bus : {false, true}) for(const auto& p : s.parts) if((p.input.kind == PartInput::Kind::Bus) == bus) out.push_back(&p);
            return out;
        }
        // A part's banks by kind, `width` wide.
        struct PartBanks { std::vector<Bank> assign, roles, effects; };
        PartBanks banksOf(const AssignmentSource& s, uint32_t partId, uint32_t width)
        {
            PartBanks out;
            width = std::max(1u, width);
            const auto* part = partOf(s, partId);
            if(!part)
            {
                // No part selected: the Performance's own controls (Super Knobs and the others).
                std::vector<Item> items;
                for(const auto& m : s.controls) if(!m.part) items.push_back(of(Assignment::Kind::Control, m.id, m.name));
                chunk(out.assign, "Controls", BankKind::Assign, AssignmentLayer::Convention, items, width);
                return out;
            }
            const auto nodes = nodesOf(s, *part);
            // 2. The sound's own assignments: its controls.
            std::vector<Item> own;
            for(const auto& m : s.controls) if(m.part == partId) own.push_back(of(Assignment::Kind::Control, m.id, m.name));
            chunk(out.assign, "Assign", BankKind::Assign, AssignmentLayer::Sound, own, width);
            // 3. The role banks, from its instruments' roles, each taking as many roles as the
            // group is wide; a role none has is blank, and a bank with none of its roles left out.
            for(const auto& rb : roleBanks)
            {
                auto b = blank(rb.name, rb.kind, width); b.layer = AssignmentLayer::Role;
                bool any = false;
                for(size_t k = 0; k < width && k < rb.roles.size(); ++k)
                    for(const auto* n : nodes)
                    {
                        if(n->effect || b.slots[k].kind != Assignment::Kind::None) continue;
                        for(const auto& p : n->params) if(p.role == rb.roles[k]) { std::tie(b.slots[k], b.labels[k]) = parameter(*n, p); any = true; break; }
                    }
                if(any) out.roles.push_back(std::move(b));
            }
            if(out.roles.empty() && own.empty())
            {
                // No role bank filled (none declared, or none within the width) and nothing assigned: two Assign banks from its instrument's first parameters.
                const AssignableNode* first = nullptr;
                for(const auto* n : nodes) if(!first && n->instrument && !n->params.empty()) first = n;
                for(const auto* n : nodes) if(!first && !n->effect && !n->params.empty()) first = n;
                for(const auto* n : nodes) if(!first && !n->params.empty()) first = n;
                std::vector<Item> items;
                if(first) for(const auto* p : byPriority(*first)) if(items.size() < 2 * width) items.push_back(parameter(*first, *p));
                chunk(out.assign, "Assign", BankKind::Assign, AssignmentLayer::Role, items, width);
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
            chunk(out.effects, "Effects", BankKind::Effects, AssignmentLayer::Effects, effects, width);
            return out;
        }
        // A group's controls' identity: "knob" for knobs and encoders, and so on; empty for notes.
        std::string identity(const std::string& kind)
        {
            static const std::map<std::string, std::string> names{{"knobs", "knob"}, {"encoders", "knob"}, {"faders", "fader"}, {"buttons", "button"}, {"wheels", "wheel"},
                                                                 {"touchstrip", "strip"}, {"jog", "jog"}, {"pedal", "pedal"}, {"expression", "pedal"}, {"footswitches", "footswitch"},
                                                                 {"aftertouch", "aftertouch"}, {"breath", "breath"}};
            const auto it = names.find(kind);
            return it == names.end() ? std::string() : it->second;
        }
        const char* bankName(BankKind kind)
        {
            for(const auto& rb : roleBanks) if(rb.kind == kind) return rb.name;
            return kind == BankKind::Effects ? "Effects" : "Assign";
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

    std::vector<Bank> knobBanks(const AssignmentSource& s, uint32_t part, uint32_t width)
    {
        auto b = banksOf(s, part, width);
        auto out = std::move(b.assign);
        for(auto* list : {&b.roles, &b.effects}) for(auto& bank : *list) out.push_back(std::move(bank));
        return out;
    }

    Bank bankOf(const AssignmentSource& s, uint32_t part, uint32_t width, BankKind kind, uint32_t index)
    {
        auto b = banksOf(s, part, width);
        const auto& list = kind == BankKind::Assign ? b.assign : kind == BankKind::Effects ? b.effects : b.roles;
        size_t seen = 0;
        for(const auto& bank : list) if(bank.kind == kind && seen++ == index) return bank;
        return blank(bankName(kind), kind, std::max(1u, width));
    }

    uint32_t groupPositions(const AssignmentSource& s, const ControlGroup& g, uint32_t part)
    {
        if(g.role != ControlGroup::Role::Screen && g.role != ControlGroup::Role::Assign) return 1;
        if(g.kind == "faders") return uint32_t(std::max<size_t>(1, g.modes.size()));
        if(g.kind == "buttons" || !g.functions.empty() || (g.role == ControlGroup::Role::Screen && !s.screen.params.empty())) return 1;
        if(!g.rows.empty()) return uint32_t(g.rows.size());
        if(!g.paged && g.kind != "knobs" && g.kind != "encoders") return 1;
        return uint32_t(std::max<size_t>(1, knobBanks(s, part, g.count).size()));
    }

    std::vector<ResolvedControl> resolveControls(const AssignmentSource& s, const std::vector<ControlGroup>& groups, uint32_t part, const GroupPositions& positions)
    {
        std::vector<ResolvedControl> out;
        // 1. Overrides, where what they name is still there; 2. the selected part's Sound's own,
        // or for a control with a function, each part's.
        auto overridden = [&](ResolvedControl& r) {
            for(const auto layer : {AssignmentLayer::Override, AssignmentLayer::Sound})
                for(const auto& o : s.overrides)
                {
                    if(o.control != r.control || (layer == AssignmentLayer::Override) != (o.part == 0) || (o.part && !partOf(s, o.part))) continue;
                    const auto name = assignmentName(s, o.target);
                    if(name.empty()) continue;
                    if(layer == AssignmentLayer::Sound && !r.function.empty()) { r.parts.emplace_back(o.part, o.target); continue; }
                    if(layer == AssignmentLayer::Sound && o.part != part) continue;
                    r.target = o.target; r.layer = layer; r.label = name; r.bank.clear(); return;
                }
        };
        std::map<std::string, uint32_t> numbers; // per identity, across groups
        const auto order = faderOrder(s);
        for(size_t g = 0; g < groups.size(); ++g)
        {
            const auto& group = groups[g];
            const bool screen = group.role == ControlGroup::Role::Screen;
            const auto name = screen ? std::string("screen") : identity(group.kind);
            if((!screen && group.role != ControlGroup::Role::Assign) || name.empty()) continue;
            const uint32_t at = g < positions.size() ? positions[g] : 0;
            // What the group plays now, a slot per control.
            Bank b = blank({}, BankKind::Assign, std::max(1u, group.count));
            auto levels = [&](const std::vector<const Part*>& parts, Assignment::Kind kind, const char* suffix) {
                for(size_t i = 0; i < group.count && i < parts.size(); ++i) std::tie(b.slots[i], b.labels[i]) = of(kind, parts[i]->id, parts[i]->name + suffix);
                b.layer = AssignmentLayer::Convention;
            };
            if(screen && !s.screen.params.empty())
            {
                // An editor page shown: its top parameters by priority.
                for(size_t i = 0; i < group.count && i < s.screen.params.size(); ++i) { b.slots[i] = s.screen.params[i]; b.labels[i] = assignmentName(s, b.slots[i]); }
                b.name = s.screen.name; b.layer = AssignmentLayer::Editor;
            }
            else if(group.kind == "faders")
            {
                // 4. Faders by their mode: part levels, the part's Levels bank, or the audio parts' levels.
                const auto mode = group.modes.empty() ? std::string("part") : group.modes[at % group.modes.size()];
                if(mode == "element") b = bankOf(s, part, group.count, BankKind::Levels, 0);
                else if(mode == "audio")
                {
                    std::vector<const Part*> audio;
                    for(const auto* p : order) if(p->input.kind == PartInput::Kind::Audio) audio.push_back(p);
                    levels(audio, Assignment::Kind::PartLevel, " level");
                }
                else levels(order, Assignment::Kind::PartLevel, " level");
                b.name = mode;
            }
            else if(group.kind == "buttons" && group.scenes)
            {
                for(size_t i = 0; i < group.count && i < s.scenes.size(); ++i) std::tie(b.slots[i], b.labels[i]) = of(Assignment::Kind::Scene, s.scenes[i].first, s.scenes[i].second);
                b.layer = AssignmentLayer::Convention;
            }
            else if(group.kind == "buttons" && group.switches) {} // assignable switches: nothing until assigned
            else if(group.kind == "buttons") levels(order, Assignment::Kind::PartMute, " mute");
            else if(!group.functions.empty() || (group.rows.empty() && !group.paged && group.kind != "knobs" && group.kind != "encoders")) {} // passed through, or nothing until assigned
            else if(!group.rows.empty())
            {
                // A hardware row: its bank, the Nth of its kind for the Nth row naming that kind.
                const auto row = at % group.rows.size();
                uint32_t index = 0;
                for(size_t r = 0; r < row; ++r) index += group.rows[r].bank == group.rows[row].bank;
                b = bankOf(s, part, group.count, group.rows[row].bank, index);
                b.name = group.rows[row].name;
            }
            else if(const auto banks = knobBanks(s, part, group.count); !banks.empty()) b = banks[at % banks.size()];
            for(uint32_t i = 0; i < group.count; ++i)
            {
                // Switches are button:1 up on every device; the other buttons count on from 101, so a switch's number never depends on them.
                const bool convention = name == "button" && !group.switches;
                ResolvedControl r; r.control = name + ":" + std::to_string(convention ? firstConventionButton - 1 + ++numbers["button>"] : ++numbers[name]);
                r.target = b.slots[i]; r.label = b.labels[i]; r.bank = b.name; r.group = g; r.index = i;
                if(i < group.functions.size()) r.label = r.function = group.functions[i];
                if(r.target.kind != Assignment::Kind::None) r.layer = b.layer;
                if(!screen) overridden(r); // screen knobs play what the screen shows
                out.push_back(std::move(r));
            }
        }
        return out;
    }

    std::string roleFromName(const std::string& name, const std::string& module)
    {
        auto lower = [](std::string t) { for(auto& c : t) c = char(std::tolower((unsigned char)c)); return t; };
        auto key = lower(name);
        // shortcut: exact names only, and a filter's envelope only by its module; per-oscillator levels and other spellings fall to priority until plugins declare roles.
        static const std::map<std::string, std::string> names{
            {"cutoff", "cutoff"}, {"filter cutoff", "cutoff"}, {"cutoff frequency", "cutoff"}, {"filter frequency", "cutoff"},
            {"resonance", "resonance"}, {"filter resonance", "resonance"}, {"reso", "resonance"},
            {"filter envelope", "filterEnvelope"}, {"filter env", "filterEnvelope"}, {"filter env amount", "filterEnvelope"}, {"env amount", "filterEnvelope"}, {"envelope amount", "filterEnvelope"},
            {"key tracking", "keyTracking"}, {"filter key tracking", "keyTracking"}, {"key track", "keyTracking"},
            {"filter attack", "filterAttack"}, {"filter decay", "filterDecay"}, {"filter sustain", "filterSustain"}, {"filter release", "filterRelease"},
            {"attack", "attack"}, {"decay", "decay"}, {"sustain", "sustain"}, {"release", "release"}, {"hold", "hold"},
            {"amp attack", "attack"}, {"amp decay", "decay"}, {"amp sustain", "sustain"}, {"amp release", "release"},
            {"velocity", "velocity"}, {"level", "level"}, {"volume", "level"},
            {"low gain", "lowGain"}, {"mid gain", "midGain"}, {"high gain", "highGain"}, {"low frequency", "lowFrequency"}, {"mid frequency", "midFrequency"}, {"high frequency", "highFrequency"}, {"mid q", "midQ"},
            {"mix", "mix"}, {"dry/wet", "mix"}, {"wet", "mix"}, {"pan", "pan"}};
        const auto slash = module.rfind('/');
        if(lower(slash == std::string::npos ? module : module.substr(slash + 1)).find("filter") != std::string::npos &&
           (key == "attack" || key == "decay" || key == "sustain" || key == "release")) key = "filter " + key;
        const auto it = names.find(key);
        return it == names.end() ? std::string() : it->second;
    }

    AssignmentSource assignmentSource(const Control::GraphState& g, const std::vector<AssignmentOverride>& overrides,
                                      const std::function<std::vector<Control::ParamInfo>(NodeId)>& parameters)
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
                // Declared roles; a plugin declaring none, the roles its names suggest.
                const auto params = parameters ? parameters(id) : std::vector<Control::ParamInfo>{};
                const bool declared = std::any_of(params.begin(), params.end(), [](const Control::ParamInfo& p) { return p.role != 0; });
                for(const auto& p : params) n.params.push_back({p.id, p.name, declared ? roleName(p.role) : roleFromName(p.name, p.module), p.priority});
                s.nodes.emplace(id, std::move(n));
            }
        }
        return s;
    }

    std::vector<AssignmentOverride> Session::assignments() const { return assignments_; }

    void Session::assign(const std::string& control, const Assignment& target, uint32_t part)
    {
        if(!validPhysicalControl(control)) throw std::invalid_argument("a physical control is knob:N, fader:N, button:N, wheel:N, strip:N, jog:N, pedal:N, footswitch:N, aftertouch:N or breath:N");
        if(part)
        {
            // A Sound's own assignment plays only its own plugins and controls.
            const auto in = nodesOf(findPart(part));
            const bool own = target.kind == Assignment::Kind::None || (target.kind == Assignment::Kind::Parameter && in.count(target.node)) ||
                             (target.kind == Assignment::Kind::Control && std::any_of(macros_.begin(), macros_.end(), [&](const Macro& m) { return m.id == target.id && m.part == part; }));
            if(!own) throw std::invalid_argument("a Sound's assignment plays its own plugins and controls");
        }
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
        std::erase_if(assignments_, [&](const AssignmentOverride& o) { return o.control == control && o.part == part; });
        if(target.kind != Assignment::Kind::None) assignments_.push_back({control, target, part});
    }
}

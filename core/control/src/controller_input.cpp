// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011's instant controls: the controller-mapping service on the engine's MIDI input threads.
#include "oscairn/controller_input.h"
#include "oscairn/session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oscairn
{

    void ControllerInput::set(std::vector<ControlGroup> groups, std::vector<Control::ControllerPort> ports)
    {
        std::lock_guard lock(lock_);
        groups_ = std::move(groups); ports_ = std::move(ports);
        positions_.assign(groups_.size(), 0);
        mapping_ = {};
        dirty_ = true;
    }

    void ControllerInput::view(const Control::ControlView& view)
    {
        std::lock_guard lock(lock_);
        if(view.part) part_ = *view.part;
        if(view.screen) { screen_ = *view.screen; source_.screen = screen_; }
        for(const auto& [group, position] : view.positions)
        {
            if(group >= groups_.size()) throw std::invalid_argument("no such control group");
            if(positions_.size() <= group) positions_.resize(group + 1);
            positions_[group] = position;
        }
    }

    Control::ControllerState ControllerInput::state() const
    {
        std::lock_guard lock(lock_);
        return {positions_, touched_, touches_};
    }

    void ControllerInput::midi(uint8_t source, const uint8_t* bytes, size_t size) noexcept
    {
        std::function<void()> wake;
        try
        {
            std::lock_guard lock(lock_);
            const auto bound = bindings_.find(source);
            if(bound == bindings_.end()) return;
            const auto moves = moves_;
            for(const auto& [device, port] : bound->second)
                for(const auto& action : mapping_.handle(source_, groups_, part_, positions_, device, port, bytes, size, [this](const Assignment& a) { return value(a); }))
                    play(action);
            if(moves_ != moves) wake = wake_;
        }
        catch(const std::exception& e) { std::fprintf(stderr, "oscairn: controller input: %s\n", e.what()); }
        if(wake) wake();
    }

    double ControllerInput::value(const Assignment& a) const
    {
        switch(a.kind)
        {
        case Assignment::Kind::Control: for(const auto& m : macros_) if(m.id == a.id) return m.value; return 0;
        case Assignment::Kind::Scene: return scene_ == a.id ? 1 : 0;
        case Assignment::Kind::None: case Assignment::Kind::Panic: return 0;
        default: { const auto it = values_.find(key(a)); return it == values_.end() ? 0 : it->second; }
        }
    }

    void ControllerInput::setFader(uint32_t part, std::optional<double> level, std::optional<bool> mute)
    {
        const Key l{int(Assignment::Kind::PartLevel), 0, 0, part}, m{int(Assignment::Kind::PartMute), 0, 0, part};
        if(level) { values_[l] = *level; unapplied_.insert(l); }
        if(mute) { values_[m] = *mute ? 1 : 0; unapplied_.insert(m); }
        // A live fader is heard at the next block; any other waits for the control thread.
        if(const auto slot = slots_.find(part); slot != slots_.end()) write(slot->second, values_[m] >= .5 ? 0 : values_[l]);
    }

    void ControllerInput::write(uint8_t slot, double gain)
    {
        session_.graph_.setLiveGain(slot, float(gain));
        gains_[slot] = float(gain);
    }

    // A parameter heard at the next block, its plain value as Session::applyParameter makes it.
    void ControllerInput::send(const Key& k)
    {
        const auto range = ranges_.find(k);
        if(range == ranges_.end()) return;
        const auto& r = range->second;
        double v = std::clamp(r.minimum + values_[k] * (r.maximum - r.minimum), r.minimum, r.maximum);
        if(r.stepped) v = std::trunc(v);
        queue(k, v);
    }

    void ControllerInput::queue(const Key& k, double plain)
    {
        sent_[k] = plain;
        session_.graph_.queueParameter(std::get<1>(k), std::get<2>(k), plain); // full: the control thread's value follows
    }

    void ControllerInput::setPlain(const Key& k, double plain)
    {
        const auto range = ranges_.find(k);
        if(range == ranges_.end()) return;
        const auto& r = range->second;
        plain = std::clamp(plain, r.minimum, r.maximum);
        if(r.stepped) plain = std::trunc(plain);
        const double v = r.maximum > r.minimum ? (plain - r.minimum) / (r.maximum - r.minimum) : 0;
        if(const auto now = values_.find(k); now != values_.end() && std::abs(now->second - v) < 1e-12) return;
        values_[k] = v; unapplied_.insert(k);
        queue(k, plain);
    }

    // A control and its targets by the summing rule, as the Session drives them: part levels,
    // mutes, parameters and morphs now.
    void ControllerInput::drive(uint32_t control, double value)
    {
        auto current = [&](const MacroTarget& m) -> std::optional<double> {
            switch(m.kind)
            {
            case MacroTarget::Kind::Parameter: { const auto it = values_.find({int(Assignment::Kind::Parameter), m.node, m.param, 0}); if(it == values_.end()) return {}; return it->second; }
            case MacroTarget::Kind::PartLevel: { const auto it = values_.find({int(Assignment::Kind::PartLevel), 0, 0, m.part}); return it == values_.end() ? 1.0 : std::clamp(it->second, 0.0, 1.0); }
            case MacroTarget::Kind::PartMute: { const auto it = values_.find({int(Assignment::Kind::PartMute), 0, 0, m.part}); return it == values_.end() ? 0.0 : it->second; }
            case MacroTarget::Kind::Macro: for(const auto& x : macros_) if(x.id == m.macro) return x.value; return 0.0;
            case MacroTarget::Kind::Morph: return 0.0;
            }
            return 0.0;
        };
        auto apply = [&](const MacroTarget& m, double x) {
            switch(m.kind)
            {
            case MacroTarget::Kind::Parameter: { const Key k{int(Assignment::Kind::Parameter), m.node, m.param, 0}; if(values_.count(k)) { values_[k] = x; unapplied_.insert(k); send(k); } break; }
            case MacroTarget::Kind::PartLevel: setFader(m.part, x, {}); break;
            case MacroTarget::Kind::PartMute: setFader(m.part, {}, x >= .5); break;
            case MacroTarget::Kind::Morph: morph(m.sceneFrom, m.sceneTo, x); break;
            case MacroTarget::Kind::Macro: break;
            }
        };
        Session::drive(macros_, written_, mappingScene_, control, value, current, apply);
    }

    // As Session::morphScenes, of what blend() sets: the control thread then switches what does
    // not play live.
    void ControllerInput::morph(uint32_t fromId, uint32_t toId, double t)
    {
        if(blending_) return;
        const auto f = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == fromId; });
        const auto g = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == toId; });
        if(f == scenes_.end() || g == scenes_.end()) return;
        if(g->scope & Scene::Mappings) mappingScene_ = t >= .5 ? g->id : f->id;
        blend(*f, *g, t, g->scope);
    }

    // What Session::blendScenes sets at t between two scenes that plays without a plan: the
    // controls the scenes hold, live faders, pans, sends and the master, and parameter values
    // (a stepped one switching halfway); at an end, that scene as it is. Lanes, user wires,
    // solos, faders that are not live, and plugins and wires switched on or off wait for the
    // control thread.
    void ControllerInput::blend(const Scene& from, const Scene& to, double t, uint32_t scope)
    {
        const Scene* end = t >= 1 ? &to : t <= 0 ? &from : nullptr;
        const Scene& mid = end ? *end : to;
        const double eased = to.curve == Scene::Curve::Ease ? t * t * (3 - 2 * t) : t;
        const auto mix = [&](double a, double b) { return a + (b - a) * eased; };
        const auto gain = [&](double a, double b) { return to.curve == Scene::Curve::EqualPower ? std::sqrt((1 - t) * a * a + t * b * b) : mix(a, b); };
        blending_ = true;
        struct Restore { bool& flag; ~Restore() { flag = false; } } restore{blending_};
        if(scope & Scene::Parameters)
            for(const auto& [id, value] : mid.macros)
            {
                const auto m = std::find_if(macros_.begin(), macros_.end(), [&](const Macro& x) { return x.id == id; });
                if(m == macros_.end() || std::any_of(m->targets.begin(), m->targets.end(), [](const MacroTarget& x) { return x.kind == MacroTarget::Kind::Morph; })) continue;
                double v = value;
                if(!end) for(const auto& [other, was] : from.macros) if(other == id) v = mix(was, value);
                if(v != m->value) { drive(id, v); unapplied_.insert({int(Assignment::Kind::Control), 0, 0, id}); }
            }
        for(const auto& p : mid.parts)
        {
            if(!values_.count({int(Assignment::Kind::PartLevel), 0, 0, p.part})) continue;
            const Scene::PartState* f = nullptr;
            if(!end) for(const auto& x : from.parts) if(x.part == p.part) f = &x;
            std::optional<double> level; std::optional<bool> mute;
            if(scope & Scene::Mix) level = f ? gain(f->mute ? 0 : f->level, p.mute ? 0 : p.level) : p.level;
            if(scope & Scene::Mutes) mute = f ? f->mute && p.mute : p.mute;
            if(level || mute) setFader(p.part, level, mute);
            if(!(scope & Scene::Mix)) continue;
            if(const auto s = pans_.find(p.part); s != pans_.end())
            {
                const double pan = f ? mix(f->pan, p.pan) : p.pan;
                write(s->second[0], std::min(1.0, 1 - pan)); write(s->second[1], std::min(1.0, 1 + pan));
            }
            for(const auto& x : p.sends)
                if(const auto s = sends_.find({p.part, x.bus}); s != sends_.end())
                {
                    double v = x.level;
                    if(f) for(const auto& was : f->sends) if(was.bus == x.bus) v = gain(was.level, x.level);
                    write(s->second, v);
                }
        }
        if((scope & Scene::Mix) && master_)
        {
            const double level = end ? end->masterLevel : gain(from.masterLevel, to.masterLevel), pan = end ? end->masterPan : mix(from.masterPan, to.masterPan);
            write(Session::masterSlots[0], level * (stereo_ ? std::min(1.0, 1 - pan) : 1)); write(Session::masterSlots[1], level * std::min(1.0, 1 + pan));
        }
        if(scope & Scene::Parameters)
            for(size_t i = 0; i < mid.values.size(); ++i)
            {
                const auto& v = mid.values[i];
                const Key k{int(Assignment::Kind::Parameter), v.at.node, v.at.param, 0};
                const auto r = ranges_.find(k);
                if(r == ranges_.end()) continue;
                double value = v.value;
                if(!end)
                {
                    // Scenes list their values in one order: the same place first.
                    auto was = i < from.values.size() && from.values[i].at == v.at ? from.values.begin() + std::ptrdiff_t(i) : std::find_if(from.values.begin(), from.values.end(), [&](const Scene::Value& x) { return x.at == v.at; });
                    if(was != from.values.end()) value = r->second.stepped ? (t >= .5 ? v.value : was->value) : mix(was->value, v.value);
                }
                setPlain(k, value);
            }
    }

    void ControllerInput::play(const ControllerMapping::Action& action)
    {
        if(!action.control.empty()) { touched_ = action.control; ++touches_; }
        ++moves_;
        const auto& target = action.target;
        const double v = action.value;
        switch(target.kind)
        {
        case Assignment::Kind::PartLevel: setFader(target.id, v, {}); break;
        case Assignment::Kind::PartMute: setFader(target.id, {}, v >= .5); break;
        case Assignment::Kind::Parameter: values_[key(target)] = v; unapplied_.insert(key(target)); send(key(target)); break;
        case Assignment::Kind::Control: drive(target.id, v); unapplied_.insert(key(target)); break;
        case Assignment::Kind::Panic: session_.allNotesOff(); break; // here on the MIDI thread: two atomic flags, heard at the next block
        case Assignment::Kind::Scene: case Assignment::Kind::None: break;
        }
        played_.push_back({target, v});
    }

    void ControllerInput::service()
    {
        std::vector<Played> played;
        uint64_t moves = 0;
        { std::lock_guard lock(lock_); played.swap(played_); moves = moves_; unapplied_.clear(); sent_.clear(); gains_.clear(); }
        if(!played.empty())
        {
            // The latest value per continuous target, mutes and scenes each in order.
            std::vector<ControllerMapping::Action> actions;
            for(const auto& p : played) actions.push_back({{}, p.target, p.value});
            for(const auto& a : ControllerMapping::latest(std::move(actions)))
                try { apply({a.target, a.value}); }
                catch(const std::exception& e) { std::fprintf(stderr, "oscairn: controller input: %s\n", e.what()); }
            // The model has every move now. With no newer move on its way, the live gains take the
            // model's values: the same, unless a control was rebased by something else moving its
            // destination. Otherwise those the newer moves wrote win over the older a scene's
            // edit has just written.
            std::lock_guard lock(lock_);
            if(moves_ == moves) session_.writeLiveGains();
            else for(const auto& [slot, gain] : gains_) session_.graph_.setLiveGain(slot, gain);
        }
        refresh(!played.empty());
    }

    void ControllerInput::resend()
    {
        std::lock_guard lock(lock_);
        for(const auto& k : unapplied_)
            // A value still waiting in the Session is replaced by this one at the next service().
            if(const auto sent = sent_.find(k); sent != sent_.end() && !session_.pending_.count({std::get<1>(k), std::get<2>(k)})) queue(k, sent->second);
    }

    void ControllerInput::apply(const Played& p)
    {
        const auto& t = p.target;
        auto part = [&](uint32_t id) -> const Part* { for(const auto& x : session_.parts_) if(x.id == id) return &x; return nullptr; };
        switch(t.kind)
        {
        case Assignment::Kind::Parameter:
            if(session_.hasParameter(t.node, t.param))
            {
                const auto info = session_.parameterInfo(t.node, t.param);
                session_.setParameter(t.node, t.param, info.minimum + p.value * (info.maximum - info.minimum));
            }
            break;
        case Assignment::Kind::Control: session_.playMacro(t.id, p.value, false); break;
        case Assignment::Kind::PartLevel: if(const auto* x = part(t.id)) session_.playPartLevel(t.id, p.value, x->mute); break;
        case Assignment::Kind::PartMute: if(const auto* x = part(t.id)) session_.playPartLevel(t.id, x->level, p.value >= .5); break;
        case Assignment::Kind::Scene: session_.selectScene(t.id); break;
        case Assignment::Kind::None: case Assignment::Kind::Panic: break; // a panic was played at once
        }
    }

    // The snapshot: compiled again when the Session, the inputs or the groups changed.
    void ControllerInput::refresh(bool moved)
    {
        const auto& inputs = session_.hostStatus_.midiInputs;
        auto live = session_.liveValues();
        if(!moved && !dirty_ && session_.sequence_ == sequence_ && session_.revision_ == revision_ && inputs == inputs_ && live == liveSeen_) return;
        liveSeen_ = std::move(live);
        std::vector<Control::ControllerPort> ports;
        { std::lock_guard lock(lock_); ports = ports_; }
        // shortcut: compiled whole on every change of the Session (a parameter turned on screen too); compile only what changed if this shows in the control thread's load.
        const auto state = session_.graphState();
        auto source = assignmentSource(state, session_.assignments_, [&](NodeId n) { return session_.parameters(n); });
        std::map<Key, double> values;
        for(const auto& p : state.parts)
        {
            values[{int(Assignment::Kind::PartLevel), 0, 0, p.id}] = p.level;
            values[{int(Assignment::Kind::PartMute), 0, 0, p.id}] = p.mute ? 1 : 0;
        }
        std::map<Key, Range> ranges;
        for(const auto& [id, node] : source.nodes)
            for(const auto& param : node.params)
            {
                const auto info = session_.parameterInfo(id, param.id);
                values[{int(Assignment::Kind::Parameter), id, param.id, 0}] = info.maximum > info.minimum ? (session_.parameter(id, param.id) - info.minimum) / (info.maximum - info.minimum) : 0;
                if(!(info.flags & CLAP_PARAM_IS_READONLY)) ranges[{int(Assignment::Kind::Parameter), id, param.id, 0}] = {info.minimum, info.maximum, info.stepped};
            }
        std::map<uint32_t, uint8_t> slots;
        for(const auto& [part, slot] : session_.faderSlots_) if(session_.liveFader(part)) slots[part] = slot;
        std::map<uint32_t, std::array<uint8_t, 2>> pans;
        for(const auto& [part, slot] : session_.panSlots_) if(session_.livePan(part)) pans[part] = slot;
        std::map<std::pair<uint32_t, uint32_t>, uint8_t> sends;
        for(const auto& [send, slot] : session_.sendSlots_) if(session_.liveSend(send.first, send.second)) sends[send] = slot;
        const auto& outputs = session_.graph_.audioLayout().outputs;
        // shortcut: every scene copied at each compile (as Session::model() copies them for undo); copy only those morphs reach if this shows in the control thread's load.
        auto scenes = session_.scenes_;
        std::map<uint8_t, std::vector<std::pair<std::string, uint32_t>>> bindings;
        for(const auto& input : inputs)
            for(const auto& port : ports) if(port.input == input.name) bindings[input.source].push_back({port.device, port.port});
        std::lock_guard lock(lock_);
        source.screen = screen_;
        source_ = std::move(source);
        // A value a MIDI thread set since the moves were taken is newer than the model's.
        for(const auto& k : unapplied_) if(const auto it = values_.find(k); it != values_.end()) values[k] = it->second;
        values_ = std::move(values);
        auto macros = session_.macros_;
        for(auto& m : macros)
            if(unapplied_.count({int(Assignment::Kind::Control), 0, 0, m.id}))
                for(const auto& old : macros_) if(old.id == m.id) m.value = old.value;
        macros_ = std::move(macros);
        written_ = session_.written_;
        mappingScene_ = session_.mappingScene_; scene_ = session_.scene_;
        slots_ = std::move(slots); pans_ = std::move(pans); sends_ = std::move(sends);
        master_ = !session_.staticMaster_; stereo_ = !outputs.empty() && outputs.front().channels == 2;
        scenes_ = std::move(scenes);
        ranges_ = std::move(ranges);
        bindings_ = std::move(bindings);
        positions_.resize(groups_.size());
        sequence_ = session_.sequence_; revision_ = session_.revision_; inputs_ = inputs; dirty_ = false;
    }
}

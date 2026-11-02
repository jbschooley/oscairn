// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Controllers.h"
#include "PlayView.h"
#include <algorithm>
#include <cstdio>

namespace oscairn::standalone
{
    namespace
    {
        double now() { return juce::Time::getMillisecondCounterHiRes() * .001; }
        auto key(const Assignment& a) { return std::tuple{int(a.kind), a.node, a.param, a.id}; }
    }

    Controllers::Controllers(Engine& engine, std::vector<ControlGroup> device, std::vector<Profile> profiles)
        : engine_(engine), device_(std::move(device)), profiles_(std::move(profiles))
    {
        open();
        changed_ = juce::MidiDeviceListConnection::make([this] { dirty_ = true; });
        startTimer(10);
    }

    Controllers::~Controllers()
    {
        stopTimer();
        for(auto& input : inputs_) input->stop();
    }

    void Controllers::open()
    {
        {
            // Stopped outside the lock: a callback waiting for it must not block stop().
            std::vector<std::unique_ptr<juce::MidiInput>> old;
            { std::lock_guard lock(lock_); old.swap(inputs_); ports_.clear(); queue_.clear(); }
            for(auto& input : old) input->stop();
        }
        const auto available = juce::MidiInput::getAvailableDevices();
        std::vector<std::string> names;
        for(const auto& info : available) names.push_back(info.name.toStdString());
        std::vector<std::unique_ptr<juce::MidiInput>> inputs;
        std::vector<std::pair<std::string, uint32_t>> ports;
        std::vector<bool> joined(profiles_.size());
        for(const auto& m : matchPorts(profiles_, names))
            if(auto input = juce::MidiInput::openDevice(available[int(m.input)].identifier, this))
            {
                std::fprintf(stderr, "oscairn: MIDI input \"%s\" is %s's port %d\n", names[m.input].c_str(), profiles_[m.profile].name.c_str(), m.port);
                inputs.push_back(std::move(input)); ports.push_back({profiles_[m.profile].id, uint32_t(m.port)}); joined[m.profile] = true;
            }
        // The connected controllers' groups join the device's, after them (RFC 0024).
        auto groups = device_;
        for(size_t p = 0; p < profiles_.size(); ++p) if(joined[p]) for(auto& g : controlGroups(profiles_[p])) groups.push_back(std::move(g));
        engine_.setControlGroups(std::move(groups));
        // A port that duplicates another's would play each note twice: the engine leaves it out.
        if(auto ignored = ignoredInputs(profiles_, names); ignored != ignored_) { ignored_ = ignored; engine_.ignoreMidiInputs(std::move(ignored)); }
        engine_.positions().clear();
        { std::lock_guard lock(lock_); inputs_ = std::move(inputs); ports_ = std::move(ports); }
        for(auto& input : inputs_) input->start();
    }

    void Controllers::handleIncomingMidiMessage(juce::MidiInput* input, const juce::MidiMessage& message)
    {
        if(!message.isController() && !message.isNoteOnOrOff() && !message.isPitchWheel()) return;
        Incoming in{input, {}};
        std::copy_n(message.getRawData(), 3, in.bytes);
        std::lock_guard lock(lock_);
        if(queue_.size() < 1024) queue_.push_back(in); // shortcut: a mutex on the MIDI thread; the app's, not the audio thread's
    }

    void Controllers::timerCallback()
    {
        if(dirty_.exchange(false)) open();
        std::vector<std::pair<std::pair<std::string, uint32_t>, Incoming>> taken;
        {
            std::lock_guard lock(lock_);
            for(const auto& in : queue_)
                for(size_t i = 0; i < inputs_.size(); ++i) if(inputs_[i].get() == in.input) taken.push_back({ports_[i], in});
            queue_.clear();
        }
        if(taken.empty()) return;
        // What the controls play, read again at most five times a second while they move.
        if(now() - sourceTime_ > .2)
        {
            try
            {
                graph_ = engine_.graph();
                source_ = assignmentSource(graph_, engine_.assignments(), [this](NodeId n) { return parametersOf(n); });
                sourceTime_ = now();
            }
            catch(const std::exception&) { return; }
        }
        source_.screen = engine_.screenPage(); // screen knobs play the editor page shown
        for(const auto& [port, in] : taken)
            for(const auto& action : mapping_.handle(source_, engine_.controlGroups(), engine_.knobPart(), engine_.positions(), port.first, port.second, in.bytes, 3,
                                                     [this](const Assignment& a) { return value(a); }))
                apply(action);
    }

    std::vector<Control::ParamInfo> Controllers::parametersOf(NodeId node)
    {
        // Read once per plugin, as Play does: a node replaced by another plugin is read again.
        std::string plugin;
        for(const auto& n : graph_.topology.nodes) if(n.id == node) plugin = n.plugin.id;
        auto& cached = parameters_[node];
        if(cached.first != plugin || plugin.empty()) { try { cached = {plugin, engine_.parameters(node)}; } catch(const std::exception&) { cached = {plugin, {}}; } }
        return cached.second;
    }

    double Controllers::value(const Assignment& t)
    {
        // A value set here in the last half second is newer than what reads back.
        if(const auto it = recent_.find(key(t)); it != recent_.end() && now() - it->second.second < .5) return it->second.first;
        const auto name = graph_.names.count(t.node) ? graph_.names.at(t.node) : std::string();
        switch(t.kind)
        {
        case Assignment::Kind::Parameter:
        {
            if(!engine_.hasParam(name, t.param)) return 0;
            const auto info = engine_.paramInfo(name, t.param);
            return info.maximum > info.minimum ? (engine_.param(name, t.param) - info.minimum) / (info.maximum - info.minimum) : 0;
        }
        case Assignment::Kind::Control: for(const auto& m : engine_.controls()) if(m.id == t.id) return m.value; return 0;
        case Assignment::Kind::PartLevel: for(const auto& p : graph_.parts) if(p.id == t.id) return p.level; return 0;
        case Assignment::Kind::PartMute: for(const auto& p : graph_.parts) if(p.id == t.id) return p.mute ? 1 : 0; return 0;
        case Assignment::Kind::Scene: return graph_.scene == t.id ? 1 : 0;
        case Assignment::Kind::None: break;
        }
        return 0;
    }

    void Controllers::apply(const ControllerMapping::Action& a)
    {
        const auto& t = a.target;
        const double v = a.value;
        recent_[key(t)] = {v, now()};
        engine_.touched(a.control); // for learn
        const auto name = graph_.names.count(t.node) ? graph_.names.at(t.node) : std::string();
        switch(t.kind)
        {
        case Assignment::Kind::Parameter:
            if(engine_.hasParam(name, t.param)) { const auto info = engine_.paramInfo(name, t.param); engine_.setParam(name, t.param, info.minimum + v * (info.maximum - info.minimum)); }
            break;
        case Assignment::Kind::Control: engine_.setControl(t.id, v); break;
        case Assignment::Kind::PartLevel:
        {
            const bool mute = value({Assignment::Kind::PartMute, 0, 0, t.id}) >= .5; const auto id = t.id;
            engine_.edit([id, v, mute](Control& c) { c.setPartLevel(id, v, mute); c.commit(); });
            break;
        }
        case Assignment::Kind::PartMute:
        {
            const double level = value({Assignment::Kind::PartLevel, 0, 0, t.id});
            const auto id = t.id; const bool mute = v >= .5;
            engine_.edit([id, level, mute](Control& c) { c.setPartLevel(id, level, mute); c.commit(); });
            break;
        }
        case Assignment::Kind::Scene: { const auto id = t.id; engine_.edit([id](Control& c) { c.selectScene(id); }); break; }
        case Assignment::Kind::None: break;
        }
    }
}

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
            { std::lock_guard lock(lock_); old.swap(inputs_); ports_.clear(); mackie_.clear(); queue_.clear(); }
            for(auto& input : old) input->stop();
        }
        const auto available = juce::MidiInput::getAvailableDevices();
        std::vector<std::string> names;
        for(const auto& info : available) names.push_back(info.name.toStdString());
        std::vector<std::unique_ptr<juce::MidiInput>> inputs;
        std::vector<std::pair<std::string, uint32_t>> ports;
        std::vector<bool> joined(profiles_.size()), mackie;
        std::vector<std::string> surfaceInputs;
        surfaces_.clear(); outputs_.clear(); knobs_.reset();
        for(const auto* protocol : {"midi", "mackie"})
            for(const auto& m : matchPorts(profiles_, names, protocol))
                if(auto input = juce::MidiInput::openDevice(available[int(m.input)].identifier, this))
                {
                    std::fprintf(stderr, "oscairn: MIDI input \"%s\" is %s's port %d (%s)\n", names[m.input].c_str(), profiles_[m.profile].name.c_str(), m.port, protocol);
                    inputs.push_back(std::move(input)); ports.push_back({profiles_[m.profile].id, uint32_t(m.port)}); joined[m.profile] = true;
                    mackie.push_back(std::string(protocol) == "mackie");
                    if(mackie.back())
                    {
                        surfaceInputs.push_back(names[m.input]);
                        surfaces_[ports.back()].second = profiles_[m.profile].midi.ports[size_t(m.port - 1)].display;
                    }
                }
        // Back to a controller: a Mackie port's output, and a midi port's when a group on it takes values back.
        const auto outputDevices = juce::MidiOutput::getAvailableDevices();
        std::vector<std::string> outputNames;
        for(const auto& info : outputDevices) outputNames.push_back(info.name.toStdString());
        for(const auto* protocol : {"midi", "mackie"})
            for(const auto& m : matchPorts(profiles_, outputNames, protocol))
            {
                const auto& p = profiles_[m.profile];
                const std::pair<std::string, uint32_t> key{p.id, uint32_t(m.port)};
                const bool wanted = std::string(protocol) == "mackie" ? surfaces_.count(key) != 0
                    : joined[m.profile] && std::any_of(p.controls.begin(), p.controls.end(), [&](const Profile::Control& c) { return c.midi.feedback && c.midi.port == m.port; });
                if(!wanted || outputs_.count(key)) continue;
                if(auto output = juce::MidiOutput::openDevice(outputDevices[int(m.input)].identifier))
                {
                    std::fprintf(stderr, "oscairn: MIDI output \"%s\" takes %s's port %d back\n", outputNames[m.input].c_str(), p.name.c_str(), m.port);
                    outputs_[key] = std::move(output);
                }
            }
        // The connected controllers' groups join the device's, after them (RFC 0024).
        auto groups = device_;
        for(size_t p = 0; p < profiles_.size(); ++p) if(joined[p]) for(auto& g : controlGroups(profiles_[p])) groups.push_back(std::move(g));
        engine_.setControlGroups(std::move(groups));
        // A port that duplicates another's would play each note twice, and a Mackie port's buttons
        // are notes: the engine leaves them out.
        auto ignored = ignoredInputs(profiles_, names);
        ignored.insert(ignored.end(), surfaceInputs.begin(), surfaceInputs.end());
        if(ignored != ignored_) { ignored_ = ignored; engine_.ignoreMidiInputs(std::move(ignored)); }
        engine_.positions().clear();
        { std::lock_guard lock(lock_); inputs_ = std::move(inputs); ports_ = std::move(ports); mackie_ = std::move(mackie); }
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
        std::vector<std::tuple<std::pair<std::string, uint32_t>, Incoming, bool>> taken;
        {
            std::lock_guard lock(lock_);
            for(const auto& in : queue_)
                for(size_t i = 0; i < inputs_.size(); ++i) if(inputs_[i].get() == in.input) taken.push_back({ports_[i], in, mackie_[i]});
            queue_.clear();
        }
        if(taken.empty() && outputs_.empty()) return;
        // What the controls play, read again at most five times a second while they move, and ten
        // times a second for what goes back.
        const bool fresh = now() - sourceTime_ > (taken.empty() ? .1 : .2);
        if(fresh)
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
        for(const auto& [port, in, mackie] : taken)
        {
            if(mackie)
            {
                const MackieControl::State state{graph_.parts, playing()};
                for(const auto& a : surfaces_[port].first.handle(state, source_, in.bytes, 3, [this](const Assignment& t) { return value(t); }))
                    switch(a.kind)
                    {
                    case MackieControl::Action::Kind::Set: apply({{}, a.target, a.value}); break;
                    case MackieControl::Action::Kind::Play: engine_.setPlaying(true); break;
                    case MackieControl::Action::Kind::Stop: engine_.setPlaying(false); break;
                    case MackieControl::Action::Kind::Solo: { const auto id = a.part; const bool on = a.on; engine_.edit([id, on](Control& c) { c.setPartSolo(id, on); c.commit(); }); break; }
                    }
                continue;
            }
            for(const auto& action : mapping_.handle(source_, engine_.controlGroups(), engine_.knobPart(), engine_.positions(), port.first, port.second, in.bytes, 3,
                                                     [this](const Assignment& a) { return value(a); }))
                apply(action);
        }
        if(fresh || !taken.empty()) feedback();
    }

    bool Controllers::playing()
    {
        try { return engine_.status().playing; } catch(const std::exception&) { return false; }
    }

    void Controllers::feedback()
    {
        if(outputs_.empty() || sourceTime_ < 0) return;
        auto send = [&](const std::pair<std::string, uint32_t>& port, const uint8_t* bytes, size_t size) {
            if(const auto out = outputs_.find(port); out != outputs_.end()) out->second->sendMessageNow(juce::MidiMessage(bytes, int(size)));
        };
        const MackieControl::State state{graph_.parts, playing()};
        for(auto& [port, surface] : surfaces_)
            for(const auto& m : surface.first.feedback(state, surface.second)) send(port, m.data(), m.size());
        const auto& groups = engine_.controlGroups();
        const auto resolved = resolveControls(source_, groups, engine_.knobPart(), engine_.positions());
        for(const auto& m : knobs_.update(groups, resolved, engine_.positions(), [this](const Assignment& a) { return value(a); }))
            send({m.device, m.port}, m.bytes.data(), m.bytes.size());
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
        if(!a.control.empty()) engine_.touched(a.control); // for learn; a Mackie surface's controls are not the device's
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

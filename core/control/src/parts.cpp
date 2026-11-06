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

    std::vector<NoteZone> inputZones(const PartInput& input, const std::vector<MidiEndpoint>& endpoints, uint32_t part)
    {
        auto zones = input.zones;
        if(input.devices.empty() && !input.maxNotes && !input.reserve && !input.priority) return zones;
        if(zones.empty()) zones.emplace_back(); // a zone passing every note, to carry the devices, limit, reserve and priority
        const auto mask = resolveDevices(input.devices, endpoints);
        for(auto& z : zones) { z.sources = mask; z.maxNotes = input.maxNotes; z.part = part; z.reserve = input.reserve; z.priority = input.priority; }
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
    // wire connected or not. Off wires and wires into an off plugin carry no new notes (their
    // connection drains when no other wire on it is on) and no sound; sound from an off plugin goes
    // on until it has rung out, and its notes out drain too.
    void Session::compileWires(Graph& g, std::vector<Wire>& wires, const std::set<NodeId>& off, const std::set<NodeId>& rungOut, const std::set<NodeId>& leaving, double masterLevel, double masterPan, Live* liveGains)
    {
        const auto layout = g.describe().audioLayout;
        // The Performance's level and pan, on whatever plays into its main output: baked, or with
        // live gains its slot (left, or right on a stereo output).
        const auto mainOut = [&](const Wire& w) { return w.to == graphOutput && !layout.outputs.empty() && w.toPort == layout.outputs.front().id; };
        const auto master = [&](const Wire& w, const Pair& p) {
            if(!mainOut(w)) return p.gain;
            const double balance = layout.outputs.front().channels == 2 ? (p.to ? std::min(1.0, 1 + masterPan) : std::min(1.0, 1 - masterPan)) : 1;
            return p.gain * masterLevel * balance;
        };
        using Key = std::tuple<NodeId, uint32_t, uint32_t, NodeId, uint32_t, uint32_t>;
        using Slots = std::array<uint8_t, 4>;
        // Each route: its gain and live slots, and its gain all baked, for routes whose ways do not
        // share their slots (then baked, and their faders compile plans).
        struct Route { double gain = 0; Slots slots{}; double baked = 0; bool mixed = false; std::vector<uint32_t> faders, pans; std::vector<std::pair<uint32_t, uint32_t>> sends; bool started = false; };
        std::map<Key, Route> audio;
        // Note connections and their zones: none when any wire on the connection passes every note.
        std::map<std::pair<NodeId, NodeId>, std::vector<NoteZone>> notes;
        std::set<std::pair<NodeId, NodeId>> passAll, live; // live: a connection with a wire that takes new notes
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
                if(w.enabled && !off.count(w.from) && !off.count(w.to)) live.insert({w.from, w.to});
                if(w.zones.empty()) passAll.insert({w.from, w.to});
                else if(zones.size() + w.zones.size() > NoteZones::capacity) w.connected = false; // over the connection's zones
                else zones.insert(zones.end(), w.zones.begin(), w.zones.end());
                continue;
            }
            // With live gains a wire's own faders do not silence it: their slots do.
            Slots slots{};
            size_t used = 0;
            bool liveWire = liveGains && liveGains->slots && w.live && !w.faders.empty();
            if(liveWire)
                for(const auto part : w.faders)
                {
                    const auto slot = liveGains->slots->find(part);
                    if(slot == liveGains->slots->end() || used == slots.size() - 1) { liveWire = false; break; } // one left for the master
                    slots[used++] = slot->second;
                }
            if(!liveWire) { slots = {}; used = 0; }
            // A part's own fader wire whose pan is the part's alone takes its pan's slots, and its own
            // send wire its send's slot, where a slot is left beside the master's; otherwise baked.
            // The pans and sends of empty buses on the way are in the wire's fixed gain.
            if(liveGains) for(size_t i = 1; i < w.faders.size(); ++i) liveGains->passed.insert(w.faders[i]);
            const bool own = w.owner == WireOwner::Part, stereo = channelsOf(g, layout, w.to, w.toPort, true) == 2u;
            const std::array<uint8_t, 2>* panSlots = nullptr;
            uint8_t sendSlot = 0;
            if(liveGains && own && w.fader && !w.send && stereo)
            {
                const auto at = liveGains->pans ? liveGains->pans->find(w.part) : decltype(liveGains->pans->end()){};
                if(liveWire && w.share == 0 && used + 2 <= slots.size() && liveGains->pans && at != liveGains->pans->end()) panSlots = &at->second;
                else liveGains->staticPans.insert(w.part);
            }
            if(liveGains && own && w.send)
            {
                const auto at = liveGains->sends ? liveGains->sends->find({w.part, w.send}) : decltype(liveGains->sends->end()){};
                if(liveWire && used + 2 <= slots.size() && liveGains->sends && at != liveGains->sends->end()) sendSlot = at->second;
                else liveGains->staticSends.insert({w.part, w.send});
            }
            const bool structural = (soloed.count({w.to, w.toPort}) && !w.solo) || !w.enabled || off.count(w.to) || rungOut.count(w.from);
            const bool silent = structural || (liveWire ? w.fixedMute : w.mute);
            const auto compiled = pairs(g, layout, w, 1);
            auto centred = w; centred.pan = 0;
            const auto unpanned = panSlots ? pairs(g, layout, centred, 1) : compiled; // the pan is the slots'
            w.connected = compiled.has_value();
            // A baked wire's faders stay static while it is silent too: unmuting it must compile the
            // route back, not only write a live gain it has none of.
            if(compiled && liveGains && !liveWire) liveGains->staticFaders.insert(w.faders.begin(), w.faders.end());
            if(!compiled || silent) continue;
            const bool liveMaster = liveGains && liveGains->slots && mainOut(w);
            for(size_t i = 0; i < compiled->size(); ++i)
            {
                const auto& p = (*compiled)[i];
                auto& r = audio[{w.from, w.fromPort, p.from, w.to, w.toPort, p.to}];
                Slots s = slots;
                size_t next = used;
                if(panSlots) s[next++] = (*panSlots)[p.to ? 1 : 0];
                if(sendSlot) s[next++] = sendSlot;
                if(liveMaster) s[next] = masterSlots[layout.outputs.front().channels == 2 && p.to ? 1 : 0];
                if(!r.started) { r.slots = s; r.started = true; }
                else if(r.slots != s) r.mixed = true;
                r.gain += (liveWire ? (sendSlot ? w.unsent : w.fixed) : w.level) * (*unpanned)[i].gain * (liveMaster ? 1 : master(w, Pair{p.from, p.to, 1}));
                r.baked += (w.mute ? 0 : w.level) * master(w, p);
                r.faders.insert(r.faders.end(), w.faders.begin(), w.faders.end());
                if(panSlots) r.pans.push_back(w.part);
                if(sendSlot) r.sends.push_back({w.part, w.send});
            }
        }
        for(auto& [k, r] : audio)
            if(r.mixed)
            {
                r.gain = r.baked; r.slots = {};
                if(liveGains) { liveGains->staticFaders.insert(r.faders.begin(), r.faders.end()); liveGains->staticPans.insert(r.pans.begin(), r.pans.end()); liveGains->staticSends.insert(r.sends.begin(), r.sends.end()); if(std::get<3>(k) == graphOutput && !layout.outputs.empty() && std::get<4>(k) == layout.outputs.front().id) liveGains->staticMaster = true; }
            }
        const auto present = g.describe();
        std::map<Key, std::pair<double, Slots>> existing;
        for(const auto& r : present.audioRoutes)
            if(!leaving.count(r.from.node) && !leaving.count(r.to.node))
            for(uint32_t i = 0; i < r.channels; ++i) existing[{r.from.node, r.from.port, r.from.channel + i, r.to.node, r.to.port, r.to.channel + i}] = {r.gain, r.live};
        auto connection = [](const Key& k, double gain, const Slots& slots) {
            return AudioConnection{{std::get<0>(k), std::get<1>(k), std::get<2>(k)}, {std::get<3>(k), std::get<4>(k), std::get<5>(k)}, 1, gain, slots};
        };
        for(const auto& [k, e] : existing) if(!audio.count(k)) g.disconnectAudio(connection(k, e.first, e.second));
        for(const auto& [k, r] : audio) if(auto e = existing.find(k); e == existing.end() || e->second != std::pair{r.gain, r.slots}) g.connectAudio(connection(k, r.gain, r.slots));
        for(auto& [e, zones] : notes) if(passAll.count(e)) zones.clear();
        for(const auto& e : present.notes) if(!notes.count(e) && !leaving.count(e.first) && !leaving.count(e.second)) g.disconnectNotes(e.first, e.second);
        for(const auto& [e, zones] : notes)
        {
            const bool exists = std::find(present.notes.begin(), present.notes.end(), e) != present.notes.end();
            const auto had = present.noteZones.find(e);
            if(!exists || (had == present.noteZones.end() ? !zones.empty() : had->second != zones)) g.connectNotes(e.first, e.second, zones);
            if(present.draining.count(e) != !live.count(e)) g.setDraining(e.first, e.second, !live.count(e));
        }
    }

    void Session::syncWires() { compileWires(); graph_.setMidiTaps(midiTaps(graph_, parts_, wires_)); graph_.setHarmonies(harmonies(parts_, key_)); staged_ = true; }

    // The open Performance's wires, with each part's fader and the master as live gains.
    void Session::compileWires()
    {
        assignSlots();
        Live live{&faderSlots_, &panSlots_, &sendSlots_};
        compileWires(graph_, wires_, disabled_, rungOut_, leaving_, masterLevel_, masterPan_, &live);
        staticFaders_ = std::move(live.staticFaders); staticMaster_ = live.staticMaster;
        staticPans_ = std::move(live.staticPans); staticSends_ = std::move(live.staticSends);
        staticPans_.insert(live.passed.begin(), live.passed.end());
        for(const auto bus : live.passed) for(const auto& [send, slot] : sendSlots_) if(send.first == bus) staticSends_.insert(send);
        writeLiveGains(); // the plan playing names the same slots, so they apply at once
    }

    void Session::assignSlots()
    {
        std::erase_if(faderSlots_, [&](const auto& s) { return std::none_of(parts_.begin(), parts_.end(), [&](const Part& p) { return p.id == s.first; }); });
        std::erase_if(panSlots_, [&](const auto& s) { return !faderSlots_.count(s.first); });
        std::erase_if(sendSlots_, [&](const auto& s) {
            return std::none_of(parts_.begin(), parts_.end(), [&](const Part& p) { return p.id == s.first.first && std::any_of(p.sends.begin(), p.sends.end(), [&](const Send& x) { return x.bus == s.first.second; }); }); });
        std::set<uint8_t> used;
        for(const auto& [part, slot] : faderSlots_) used.insert(slot);
        for(const auto& [part, slots] : panSlots_) used.insert(slots.begin(), slots.end());
        for(const auto& [send, slot] : sendSlots_) used.insert(slot);
        uint32_t next = firstPartSlot;
        for(const auto& p : parts_)
        {
            if(faderSlots_.count(p.id)) continue;
            while(next < Graph::liveGains && used.count(uint8_t(next))) ++next;
            if(next == Graph::liveGains) break; // past 253 parts the rest are baked, and compile plans when moved
            faderSlots_[p.id] = uint8_t(next); used.insert(uint8_t(next)); freshSlots_.insert(uint8_t(next));
        }
        // Then each part's pan (two) and each send's level: past the table's end they are baked.
        auto give = [&]() -> uint8_t {
            next = firstPartSlot;
            while(next < Graph::liveGains && used.count(uint8_t(next))) ++next;
            if(next == Graph::liveGains) return 0;
            used.insert(uint8_t(next)); freshSlots_.insert(uint8_t(next)); return uint8_t(next);
        };
        for(const auto& p : parts_)
        {
            if(faderSlots_.count(p.id) && !panSlots_.count(p.id))
                if(const auto l = give()) { if(const auto r = give()) panSlots_[p.id] = {l, r}; else { used.erase(l); freshSlots_.erase(l); } }
            for(const auto& s : p.sends)
                if(!sendSlots_.count({p.id, s.bus})) if(const auto slot = give()) sendSlots_[{p.id, s.bus}] = slot;
        }
    }

    void Session::writeLiveGains()
    {
        const auto& outputs = graph_.audioLayout().outputs;
        const bool stereo = !outputs.empty() && outputs.front().channels == 2;
        graph_.setLiveGain(masterSlots[0], float(masterLevel_ * (stereo ? std::min(1.0, 1 - masterPan_) : 1)), !freshSlots_.count(masterSlots[0]));
        graph_.setLiveGain(masterSlots[1], float(masterLevel_ * std::min(1.0, 1 + masterPan_)), !freshSlots_.count(masterSlots[1]));
        for(const auto& p : parts_)
        {
            if(const auto s = faderSlots_.find(p.id); s != faderSlots_.end()) graph_.setLiveGain(s->second, p.mute ? 0.f : float(p.level), !freshSlots_.count(s->second));
            if(const auto s = panSlots_.find(p.id); s != panSlots_.end())
            {
                graph_.setLiveGain(s->second[0], float(std::min(1.0, 1 - p.pan)), !freshSlots_.count(s->second[0]));
                graph_.setLiveGain(s->second[1], float(std::min(1.0, 1 + p.pan)), !freshSlots_.count(s->second[1]));
            }
            for(const auto& x : p.sends) if(const auto s = sendSlots_.find({p.id, x.bus}); s != sendSlots_.end()) graph_.setLiveGain(s->second, float(x.level), !freshSlots_.count(s->second));
        }
        freshSlots_.clear();
    }

    // A change of levels and mutes only: the wires the views read are built again, the slots
    // written, and no plan compiled (Session::commit leaves it out).
    template<class Edit> void Session::editFaders(Edit&& edit, bool live)
    {
        auto parts = parts_; auto wires = wires_;
        try { edit(); buildParts(); }
        catch(...) { parts_.swap(parts); wires_.swap(wires); throw; }
        if(live) writeLiveGains();
        liveEdit_ = true;
        for(size_t i = 0; i < parts.size() && i < parts_.size(); ++i) liveMute_ = liveMute_ || parts[i].mute != parts_[i].mute;
    }

    void Session::playPartLevel(uint32_t id, double level, bool mute)
    {
        findPart(id);
        if(!std::isfinite(level) || level < 0) throw std::invalid_argument("invalid part level");
        auto change = [&] { auto& p = findPart(id); p.level = level; p.mute = mute; };
        if(liveFader(id)) editFaders(change, false);
        else editWires([&] { change(); buildParts(); });
        commit();
    }

    // Each part's harmony (RFC 0033): its key or the Performance's, for every plugin in its chain
    // and lanes, with the chord held at its note input, through its zones.
    std::vector<Graph::Harmony> Session::harmonies(const std::vector<Part>& parts, const MusicalKey& key) const
    {
        std::vector<Graph::Harmony> out;
        for(const auto& part : parts)
        {
            const auto k = part.key.value_or(key);
            Graph::Harmony h{part.id, {}, part.input.kind == PartInput::Kind::Notes, {}, k.root, k.scale};
            if(h.input) h.zones = inputZones(part.input, hostStatus_.midiInputs);
            auto add = [&](const std::vector<NodeId>& chain) { for(const auto n : chain) if(!isSplit(n)) h.nodes.push_back(n); };
            add(part.chain);
            for(const auto& s : part.splits) for(const auto& l : s.lanes) add(l.chain);
            out.push_back(std::move(h));
        }
        return out;
    }

    void Session::setKey(const MusicalKey& key)
    {
        if(!key.valid()) throw std::invalid_argument("a key's root is 0 to 11, and its scale holds its root");
        key_ = key;
        graph_.setHarmonies(harmonies(parts_, key_)); staged_ = true;
    }

    void Session::setPartKey(uint32_t id, const std::optional<MusicalKey>& key)
    {
        findPart(id);
        if(key && !key->valid()) throw std::invalid_argument("a key's root is 0 to 11, and its scale holds its root");
        editWires([&] { findPart(id).key = key; });
    }

    // Each part's MIDI out (parts.h) as graph taps: the note wires into its instruments (plugins
    // taking notes and sending none), or with none, the last plugin in its chain sending notes,
    // else its input. Identical wires (layered lanes) send once.
    std::vector<Graph::MidiTap> Session::midiTaps(const Graph& g, const std::vector<Part>& parts, const std::vector<Wire>& wires) const
    {
        std::vector<Graph::MidiTap> taps;
        for(const auto& part : parts)
        {
            if(part.midiOut.devices.empty()) continue;
            const auto ports = resolveDevices(part.midiOut.devices, hostStatus_.midiOutputs) & ~uint64_t(1); // outputs are numbered from 1
            if(!ports) continue;
            const auto first = taps.size();
            std::string chosen = std::to_string(part.midiOut.channel);
            for(const auto& r : part.midiOut.devices) chosen += '\n' + std::to_string(int(r.kind)) + ' ' + std::to_string(int(r.match)) + ' ' + std::to_string(r.port) + ' ' + r.pattern;
            const uint64_t destination = std::hash<std::string>{}(chosen);
            auto add = [&](NodeId from, const std::vector<NoteZone>& zones) {
                Graph::MidiTap t{part.id, from, zones, ports, part.midiOut.channel, destination};
                if(std::find(taps.begin() + long(first), taps.end(), t) == taps.end()) taps.push_back(std::move(t));
            };
            for(const auto& w : wires)
                if(w.owner == WireOwner::Part && w.part == part.id && w.kind == WireKind::Notes && w.connected && takesNotes(g, w.to) && !g.host(w.to).noteOutputPorts())
                    add(w.from, w.zones);
            if(taps.size() > first) continue;
            const auto last = std::find_if(part.chain.rbegin(), part.chain.rend(), [&](NodeId n) { return !isSplit(n) && n != graphInput && sendsNotes(g, n); });
            if(last != part.chain.rend()) add(*last, {});
            else if(part.input.kind == PartInput::Kind::Notes) add(graphInput, inputZones(part.input, hostStatus_.midiInputs));
        }
        return taps;
    }

    void Session::enable(NodeId node, bool enabled)
    {
        if(enabled) { disabled_.erase(node); rungOut_.erase(node); }
        else disabled_.insert(node);
        graph_.setRetiring(node, !enabled); // counts the notes it holds, for ringOut
    }

    void Session::setNodeEnabled(NodeId node, bool enabled)
    {
        if(!graph_.contains(node)) throw std::invalid_argument("unknown node");
        if(enabled != (disabled_.count(node) != 0)) return;
        enable(node, enabled);
        try { editWires([&] { buildParts(); }); }
        catch(...) { enable(node, !enabled); throw; }
    }

    void Session::setWireEnabled(uint32_t id, bool enabled)
    {
        editWires([&] {
            const auto it = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; });
            if(it == wires_.end()) throw std::invalid_argument("unknown wire");
            it->enabled = enabled;
        });
    }

    // An off plugin stops sounding once it has rung out: no note sounding through it, and asleep
    // (it said so) or three seconds of silence for plugins that never say, longer than a gap
    // between echoes (the bundled Delay's longest is two); or, with no note held, 30 seconds after
    // it was switched off, whatever it sounds like, for a sound that never ends; or two minutes
    // after, notes held or not, for a key held long through the change or a release that never
    // came. Not an edit.
    // The last Performance's plugins go the same way, once rung out (RFC 0021), and all at once on
    // a panic (allNotesOff). One cut short while it still sounds is removed, not kept warm, so its
    // stuck voice ends with the instance and cannot come back with a later change taking it over.
    void Session::ringOut()
    {
        if(staged_) return;
        bool done = false;
        const auto quiet = uint64_t(sampleRate_ * 3), limit = uint64_t(sampleRate_ * 30), held = uint64_t(sampleRate_ * 120);
        const bool panic = endRinging_.exchange(false, std::memory_order_acquire);
        for(const auto n : disabled_)
            if(!rungOut_.count(n) && graph_.idle(n, quiet, limit, held)) { rungOut_.insert(n); done = true; }
        for(auto it = leaving_.begin(); it != leaving_.end(); )
            if(panic || graph_.idle(*it, quiet, limit, held))
            {
                // A node of a shared engine: its sound goes, the instance plays on for the others (RFC 0021).
                if(const auto* c = dynamic_cast<const SoundContext*>(&graph_.host(*it))) retireShared(*c);
                if(graph_.silent(*it, quiet)) keepWarm(*it); else graph_.remove(*it);
                it = leaving_.erase(it); done = true;
            }
            else ++it;
        if(!done) return;
        evictOpportunistic();
        compileWires();
        graph_.commit();
        ++revision_; publish(ChangeKind::Graph);
    }

    // A scene's edit (its own buildParts included): when it changed only what live gains play, those
    // are written and no plan is compiled (a glide's steps); otherwise it is compiled as editWires does.
    template<class Edit> void Session::editMix(bool compile, Edit&& edit)
    {
        auto parts = parts_; auto wires = wires_;
        const double level = masterLevel_, pan = masterPan_;
        try { edit(); }
        catch(...) { parts_.swap(parts); wires_.swap(wires); syncWires(); throw; }
        if(compile || !liveOnly(parts, wires, level, pan))
        {
            try { syncWires(); }
            catch(...) { parts_.swap(parts); wires_.swap(wires); syncWires(); throw; }
            return;
        }
        writeLiveGains();
        liveEdit_ = true;
        for(size_t i = 0; i < parts.size(); ++i) liveMute_ = liveMute_ || parts[i].mute != parts_[i].mute;
    }

    bool Session::liveOnly(const std::vector<Part>& before, const std::vector<Wire>& wiresBefore, double level, double pan) const
    {
        if((level != masterLevel_ || pan != masterPan_) && staticMaster_) return false;
        if(before.size() != parts_.size() || wiresBefore.size() != wires_.size()) return false;
        for(size_t i = 0; i < parts_.size(); ++i)
        {
            const auto& now = parts_[i];
            auto was = before[i]; // with what live gains play taken from now: then nothing else may differ
            if(was.id != now.id) return false;
            if(liveFader(now.id)) { was.level = now.level; was.mute = now.mute; }
            if(livePan(now.id)) was.pan = now.pan;
            if(was.sends.size() == now.sends.size())
                for(size_t k = 0; k < was.sends.size(); ++k) if(was.sends[k].bus == now.sends[k].bus && liveSend(now.id, now.sends[k].bus)) was.sends[k].level = now.sends[k].level;
            if(!(was == now)) return false;
        }
        // The user's wires as they were; the parts' wires built again from parts that differ only so,
        // each still on or off as it was.
        for(size_t i = 0; i < wires_.size(); ++i)
        {
            const auto& a = wiresBefore[i]; const auto& b = wires_[i];
            if(a.owner != b.owner || a.kind != b.kind || a.from != b.from || a.to != b.to || a.fromPort != b.fromPort || a.toPort != b.toPort || a.enabled != b.enabled) return false;
            if(a.owner == WireOwner::User && !(a == b)) return false;
        }
        return true;
    }

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
    // the lane's Mix. With no box to enter, wherever the bus itself plays, through its fader.
    // Bus edits are refused if they would loop, so this ends.
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
                        x.fixed *= lane.level; x.fixedMute = x.fixedMute || lane.mute || (solo && !lane.solo);
                        for(auto& v : x.via) { v.level *= lane.level; v.pan = std::clamp(v.pan + lane.pan, -1.0, 1.0); v.mute = v.mute || lane.mute || (solo && !lane.solo); }
                        if(!x.split) { x.split = split.id; x.lane = k; }
                        out.push_back(x);
                    }
                }
                return out;
            }
            // No box: the sound goes on, through the bus's own fader, pan and mute, and into
            // the buses it sends to, post-fader (its sends keep their own pan). Each way records
            // this bus, then the empty buses past it, for their meters.
            auto out = bus.bus ? busEntries(bus.bus) : std::vector<BusEntry>{{graphOutput, bus.output}};
            const bool mute = bus.mute || !heard(bus);
            const WireVia here{id, bus.level, bus.pan, mute};
            for(auto& x : out)
            {
                x.level *= bus.level; x.pan = std::clamp(x.pan + bus.pan, -1.0, 1.0); x.mute = x.mute || mute; x.fader = true;
                x.fixedMute = x.fixedMute || !heard(bus); x.faders.insert(x.faders.begin(), id); // its level and mute are its live gain's
                for(auto& v : x.via) { v.level *= bus.level; v.pan = std::clamp(v.pan + bus.pan, -1.0, 1.0); v.mute = v.mute || mute; }
                x.via.insert(x.via.begin(), here);
            }
            for(const auto& send : bus.sends)
                for(auto x : busEntries(send.bus))
                {
                    x.level *= bus.level * send.level; x.mute = x.mute || mute; x.fader = true;
                    x.fixed *= send.level; x.fixedMute = x.fixedMute || !heard(bus); x.faders.insert(x.faders.begin(), id);
                    for(auto& v : x.via) { v.level *= bus.level * send.level; v.mute = v.mute || mute; }
                    x.via.insert(x.via.begin(), here);
                    out.push_back(x);
                }
            return out;
        };
        return reach(bus.chain);
    }

    // Rebuilds every part's own wires; each depends only on its own chain and the buses it plays
    // into, so the order does not matter.
    void Session::buildParts() { for(const auto& p : parts_) buildPart(p); }

    // Rebuilds one part's own wires: its input into the first box, box to box, through each
    // split's lanes and joining after them, the last boxes to its output or bus, and its sends.
    // A rebuilt wire keeps the Mix settings of the one it replaces, and a note wire its zones
    // unless the part's input or a lane sets them; a wire joining from a lane takes the lane's
    // Mix, and the part's fader and sends set theirs.
    void Session::buildPart(const Part& part)
    {
        const auto layout = graph_.describe().audioLayout;
        std::vector<Wire> previous;
        std::erase_if(wires_, [&](const Wire& w) { const bool own = w.owner == WireOwner::Part && w.part == part.id; if(own) previous.push_back(w); return own; });
        auto add = [&](Wire w, bool keepZones = false) {
            w.owner = WireOwner::Part; w.part = part.id; w.id = nextWire_++;
            for(const auto& p : previous)
                if(p.kind == w.kind && p.from == w.from && p.to == w.to && p.fromPort == w.fromPort && p.toPort == w.toPort)
                    { w.level = p.level; w.pan = p.pan; w.mute = p.mute; w.solo = p.solo; w.enabled = p.enabled; w.connected = p.connected; if(keepZones) w.zones = p.zones; break; }
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
            bool zoned = false; // its zones come from the part's input or a lane, not the wire
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
                if(e.notes && takesNotes(graph_, node)) { Wire w; w.kind = WireKind::Notes; w.from = e.node; w.to = node; w.zones = e.zones; add(w, !e.zoned); }
            }
            End next; next.node = node; next.audio = sendsAudio(graph_, layout, node); next.notes = sendsNotes(graph_, node);
            if(next.audio) next.port = mainPort(graph_, layout, node, false);
            // An off plugin is passed around: what reached it goes on, beside its own sound ringing out.
            if(disabled_.count(node)) { auto on = ends; on.push_back(next); return on; }
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
                        e.zoned = true;
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
        End input; input.audio = part.input.kind == PartInput::Kind::Audio; input.notes = part.input.kind == PartInput::Kind::Notes; input.zoned = true;
        input.port = part.input.port; input.zones = inputZones(part.input, hostStatus_.midiInputs, part.id);
        // A wire into a bus's lane also takes that lane's Mix.
        // pan: the part's, which its fader wires carry, for the empty buses' meters on the way.
        // fixed: its level apart from the faders (a send's own level), for the live gains.
        const bool unheard = !heard(part);
        auto enter = [&](Wire* w, const End& e, const BusEntry& d, double level, bool mute, double pan, double fixed = 1) {
            w->level = level * e.level * d.level; w->mute = mute || e.mute || d.mute;
            w->fixed = fixed * e.level * d.fixed; w->fixedMute = unheard || e.mute || d.fixedMute; w->live = true;
            w->faders = d.faders; w->faders.insert(w->faders.begin(), part.id);
            if(d.split || d.fader) w->pan = std::clamp(e.pan + d.pan, -1.0, 1.0);
            if(d.split && !e.split) { w->split = d.split; w->lane = d.lane; }
            w->via = d.via;
            for(auto& v : w->via) { v.level *= level * e.level; v.pan = std::clamp(pan + e.pan + v.pan, -1.0, 1.0); v.mute = v.mute || mute || e.mute; }
        };
        // Two ways to one place (an empty bus's output and its send to another empty bus playing
        // there too) are one wire, as scenes, undo and turning a wire off know a wire by its ends
        // and place: their levels summed where heard, their via lists joined.
        // shortcut: two ways with different pans take their level-weighted pan, not each its own; give such wires a key of their own if that misleads.
        auto merge = [&](Wire* w) {
            const auto last = wires_.end() - 1;
            const auto same = std::find_if(wires_.begin(), last, [&](const Wire& x) {
                return x.owner == WireOwner::Part && x.part == part.id && x.kind == WireKind::Audio && x.from == w->from && x.fromPort == w->fromPort &&
                       x.to == w->to && x.toPort == w->toPort && x.send == w->send && x.split == w->split && x.lane == w->lane && x.fader == w->fader;
            });
            if(same == last) return;
            const double a = same->mute ? 0 : same->level, b = w->mute ? 0 : w->level;
            if(a + b > 0) { same->pan = (same->pan * a + w->pan * b) / (a + b); same->share = (same->share * a + w->share * b) / (a + b); }
            same->level = same->mute == w->mute ? same->level + w->level : a + b;
            same->mute = same->mute && w->mute;
            if(same->live && w->live && same->faders == w->faders)
            {
                const double x = same->fixedMute ? 0 : same->fixed, y = w->fixedMute ? 0 : w->fixed;
                same->fixed = same->fixedMute == w->fixedMute ? same->fixed + w->fixed : x + y;
                same->unsent = same->fixedMute == w->fixedMute ? same->unsent + w->unsent : (same->fixedMute ? 0 : same->unsent) + (w->fixedMute ? 0 : w->unsent);
                same->fixedMute = same->fixedMute && w->fixedMute;
            }
            else
            {
                // Two ways through different faders cannot share one live gain: baked, and those faders compile plans.
                same->live = false;
                for(const auto f : w->faders) if(std::find(same->faders.begin(), same->faders.end(), f) == same->faders.end()) same->faders.push_back(f);
            }
            same->via.insert(same->via.end(), w->via.begin(), w->via.end());
            wires_.pop_back();
        };
        const auto destinations = part.bus ? busEntries(part.bus) : std::vector<BusEntry>{{graphOutput, part.output}};
        const bool mute = part.mute || !heard(part);
        for(auto e : walk(part.chain, {input}))
        {
            if(!e.audio) continue;
            for(const auto& d : destinations)
            {
                auto* w = audioWire(e, d.node, d.port); w->fader = true; enter(w, e, d, part.level, mute, part.pan);
                w->share = e.pan + d.pan; w->pan = std::clamp(part.pan + w->share, -1.0, 1.0); // the part's pan, with its lane's and the bus lane's
                // Through empty buses its meter is at its own fader, before theirs.
                if(!w->via.empty()) w->via.insert(w->via.begin(), WireVia{part.id, part.level * e.level, std::clamp(part.pan + e.pan, -1.0, 1.0), mute || e.mute});
                merge(w);
            }
            for(const auto& send : part.sends)
                for(const auto& d : busEntries(send.bus))
                {
                    auto* s = audioWire(e, d.node, d.port);
                    s->send = send.bus; enter(s, e, d, send.level * part.level, mute, 0, send.level); // post-fader
                    s->unsent = e.level * d.fixed;
                    merge(s);
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
            if(input.maxNotes > 128) throw std::invalid_argument("a part plays at most 128 notes at once");
            if(input.reserve > 128 || (input.maxNotes && input.reserve > input.maxNotes)) throw std::invalid_argument("a part's reserve is at most 128 notes and within its note limit");
            if(input.priority > 10) throw std::invalid_argument("a part's priority is 0 to 10");
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

    void Session::setPartMidiOut(uint32_t id, const PartMidiOut& midiOut)
    {
        findPart(id);
        if(midiOut.devices.size() > 16 || std::any_of(midiOut.devices.begin(), midiOut.devices.end(), [](const DeviceRule& r) { return !r.valid(); }))
            throw std::invalid_argument("invalid or too many device rules");
        if(midiOut.channel > 16) throw std::invalid_argument("a MIDI channel is 1 to 16, or 0 to keep each message's own");
        editWires([&] { findPart(id).midiOut = midiOut; });
    }

    void Session::setClockOutputs(const std::vector<DeviceRule>& devices)
    {
        if(devices.size() > 16 || std::any_of(devices.begin(), devices.end(), [](const DeviceRule& r) { return !r.valid(); }))
            throw std::invalid_argument("invalid or too many device rules");
        clockOutputs_ = devices;
        clockPorts_.store(devices.empty() ? 0 : resolveDevices(devices, hostStatus_.midiOutputs) & ~uint64_t(1), std::memory_order_release);
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
        const auto& sends = findPart(id).sends;
        if(level && liveSend(id, bus) && std::any_of(sends.begin(), sends.end(), [&](const Send& s) { return s.bus == bus; }))
        {
            // A send's level alone: its live gain, no plan; the views read the wires again for it.
            editFaders([&] { for(auto& s : findPart(id).sends) if(s.bus == bus) s.level = *level; });
            liveMute_ = true;
            return;
        }
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

    void Session::setPartPan(uint32_t id, double pan)
    {
        findPart(id);
        if(!std::isfinite(pan) || pan < -1 || pan > 1) throw std::invalid_argument("pan is -1..1");
        if(livePan(id)) editFaders([&] { findPart(id).pan = pan; });
        else editWires([&] { findPart(id).pan = pan; buildParts(); });
    }

    void Session::setMaster(double level, double pan)
    {
        if(!std::isfinite(level) || level < 0 || level > 2 || !std::isfinite(pan) || pan < -1 || pan > 1) throw std::invalid_argument("the master level is 0..2 and its pan -1..1");
        if(!staticMaster_) { masterLevel_ = level; masterPan_ = pan; writeLiveGains(); liveEdit_ = true; return; }
        editWires([&] { masterLevel_ = level; masterPan_ = pan; });
    }

    void Session::setNodeRole(NodeId node, const std::string& role)
    {
        if(!nodes_.count(node)) throw std::invalid_argument("unknown node");
        if(!role.empty() && !validNodeRole(role)) throw std::invalid_argument("a node's role is insertion-a, insertion-b, part-eq, reverb, variation, master-effect, master-eq or arpeggiator");
        if(role.empty()) roles_.erase(node); else roles_[node] = role;
    }

    void Session::setPartSolo(uint32_t id, bool solo)
    {
        findPart(id);
        editWires([&] { findPart(id).solo = solo; buildParts(); });
    }

    // Each audio wire's meter has a value per channel of the sound it carries from its source's
    // port: that channel's peak times the gain the wire plays it with (level and pan), the largest
    // where a channel reaches the destination twice (mono into stereo). A muted wire shows nothing.
    // A part's meter is its fader wires' meters together; an empty bus's, the wires passing it,
    // each at its point there. Held values fall by about 17 dB a second at the desktop's 5 ms tick.
    // shortcut: lanes are not summed, so a part with several lanes shows its loudest per channel; measure the summed Mix if that misleads.
    void Session::meter()
    {
        const auto& layout = graph_.audioLayout();
        std::map<NodeId, Graph::Peaks> peaks; // taken once a tick for all meters: taking resets them
        for(const auto& w : wires_) if(w.kind == WireKind::Audio && !peaks.count(w.from)) peaks[w.from] = graph_.takePeaks(w.from);
        std::map<uint32_t, std::vector<double>> wires, parts;
        for(const auto& w : wires_)
        {
            if(w.kind != WireKind::Audio) continue;
            const auto channels = channelsOf(graph_, layout, w.from, w.fromPort, false);
            if(!channels) continue;
            auto& bars = wires[w.id];
            bars.assign(std::min(*channels, Graph::meterChannels), 0.0);
            uint32_t offset = 0; // the port's first channel among the node's outputs
            if(const auto* list = portsOf(graph_, layout, w.from, false)) for(const auto& p : *list) { if(p.id == w.fromPort) break; offset += p.channels; }
            if(!w.mute)
                if(const auto compiled = pairs(graph_, layout, w, w.level))
                    for(const auto& p : *compiled)
                        if(p.from < bars.size() && offset + p.from < Graph::meterChannels) bars[p.from] = std::max(bars[p.from], double(peaks[w.from][offset + p.from]) * p.gain);
            if(w.fader && w.owner == WireOwner::Part && w.via.empty())
            {
                auto& part = parts[w.part];
                part.resize(std::max(part.size(), bars.size()));
                for(size_t c = 0; c < bars.size(); ++c) part[c] = std::max(part[c], bars[c]);
            }
            // Sound through an empty bus rides another part's wire: meter it on its part and on
            // each empty bus the wire passed, at its level, pan and mute up to there.
            if(w.owner == WireOwner::Part && w.enabled)
                for(const auto& v : w.via)
                {
                    auto& bus = parts[v.bus];
                    bus.resize(std::max(bus.size(), bars.size()));
                    if(v.mute) continue;
                    auto at = w; at.pan = v.pan;
                    if(const auto compiled = pairs(graph_, layout, at, v.level))
                        for(const auto& p : *compiled)
                            if(p.from < bus.size() && offset + p.from < Graph::meterChannels) bus[p.from] = std::max(bus[p.from], double(peaks[w.from][offset + p.from]) * p.gain);
                }
        }
        auto hold = [](std::map<uint32_t, std::vector<double>>& held, std::map<uint32_t, std::vector<double>>& now) {
            for(auto& [id, bars] : now)
                if(const auto it = held.find(id); it != held.end())
                    for(size_t c = 0; c < bars.size() && c < it->second.size(); ++c) bars[c] = std::max(bars[c], it->second[c] * .99);
            held.swap(now);
        };
        for(const auto& p : parts_) parts[p.id]; // every part has a meter, if empty
        hold(meters_, parts); hold(wireMeters_, wires);
    }

    // Recording taps (RFC 0020). A part's post-fader tap sums its fader wires as compileWires
    // gives them to the graph, so it records the part's share of where it plays, in that
    // destination's channels; pre-fader, the same wires built again without the part's level,
    // pan, mute and solo. Not strict: a tap that has gone records silence, keeping its file.
    Recorder::Taps Session::compileTaps(const std::vector<RecordTap>& taps, bool strict)
    {
        const auto& layout = graph_.audioLayout();
        Recorder::Taps out;
        auto unknown = [&](const char* what) { if(strict) throw std::invalid_argument(what); };
        // A port's first channel among its node's outputs (graphInput, graphOutput: the layout's ports).
        auto first = [&](NodeId node, uint32_t port, bool input) -> std::optional<std::pair<uint32_t, const kick::clap::Port*>> {
            uint32_t offset = 0;
            if(const auto* list = portsOf(graph_, layout, node, input))
                for(const auto& p : *list) { if(p.id == port) return std::pair{offset, &p}; offset += p.channels; }
            return {};
        };
        auto port = [&](NodeId node, uint32_t id, bool input, const std::string& name) {
            Recorder::AudioTap tap{name, 0, {}};
            if(const auto at = first(node, id, input))
            {
                tap.channels = at->second->channels;
                if(tap.name.empty()) tap.name = at->second->name;
                for(uint32_t c = 0; c < tap.channels; ++c) tap.sources.push_back({node, at->first + c, c, 1.f});
            }
            else unknown("no such audio port");
            out.audio.push_back(std::move(tap));
        };
        for(const auto& t : taps)
        {
            using Kind = RecordTap::Kind;
            if(t.kind == Kind::Output) port(graphOutput, t.port, true, {});
            else if(t.kind == Kind::Input) port(graphInput, t.port, false, {});
            else if(t.kind == Kind::Node)
            {
                if(!graph_.contains(t.node)) { unknown("unknown node"); out.audio.push_back({"Node " + std::to_string(t.node), 0, {}}); continue; }
                auto id = t.port;
                const auto& ports = graph_.host(t.node).outputPorts();
                if(!t.port && std::none_of(ports.begin(), ports.end(), [](const auto& p) { return p.id == 0; }) && sendsAudio(graph_, layout, t.node)) id = mainPort(graph_, layout, t.node, false);
                std::string name = nodes_.count(t.node) && !nodes_.at(t.node).empty() ? nodes_.at(t.node) : graph_.host(t.node).descriptor().name;
                if(ports.size() > 1) for(const auto& p : ports) if(p.id == id) name += " " + p.name;
                port(t.node, id, false, name);
            }
            else if(t.kind == Kind::Part)
            {
                const auto part = std::find_if(parts_.begin(), parts_.end(), [&](const Part& p) { return p.id == t.part; });
                if(part == parts_.end()) { unknown("unknown part"); out.audio.push_back({"Part " + std::to_string(t.part), 0, {}}); continue; }
                std::vector<Wire> wires = wires_;
                if(t.preFader)
                {
                    const auto parts = parts_; const auto next = nextWire_;
                    for(auto& p : parts_) p.solo = false;
                    auto open = *part; open.level = 1; open.pan = 0; open.mute = false;
                    buildPart(open);
                    wires.swap(wires_);
                    parts_ = parts; nextWire_ = next;
                }
                std::set<std::pair<NodeId, uint32_t>> soloed;
                for(const auto& w : wires) if(w.kind == WireKind::Audio && w.solo) soloed.insert({w.to, w.toPort});
                std::map<std::tuple<NodeId, uint32_t, uint32_t, std::array<uint8_t, 4>>, double> gains;
                Recorder::AudioTap tap{part->name.empty() ? "Part " + std::to_string(part->id) : part->name, 0, {}};
                if(t.preFader) tap.name += " pre-fader";
                for(const auto& w : wires)
                {
                    if(w.kind != WireKind::Audio || w.owner != WireOwner::Part || w.part != part->id || !w.fader) continue;
                    const auto m = channelsOf(graph_, layout, w.to, w.toPort, true);
                    if(m) tap.channels = std::max(tap.channels, *m);
                    // Post-fader, a live wire follows its faders' live gains, and its pan's and send's
                    // where compileWires gave it them, as the graph plays it.
                    std::array<uint8_t, 4> slots{};
                    bool live = !t.preFader && w.live && w.faders.size() <= slots.size();
                    for(size_t i = 0; live && i < w.faders.size(); ++i) if(const auto s = faderSlots_.find(w.faders[i]); s != faderSlots_.end()) slots[i] = s->second; else live = false;
                    if(!live) slots = {};
                    const std::array<uint8_t, 2>* pan = nullptr;
                    uint8_t send = 0;
                    if(live && !w.send && m == 2u && w.share == 0 && w.faders.size() + 2 <= slots.size() && livePan(part->id)) pan = &panSlots_.at(part->id);
                    if(live && w.send && w.faders.size() + 2 <= slots.size() && liveSend(part->id, w.send)) send = sendSlots_.at({part->id, w.send});
                    const bool silent = (live ? w.fixedMute : w.mute) || (soloed.count({w.to, w.toPort}) && !w.solo) || !w.enabled || disabled_.count(w.to) || rungOut_.count(w.from);
                    const auto at = first(w.from, w.fromPort, false);
                    auto centred = w; if(pan) centred.pan = 0; // the pan is the slots'
                    const auto compiled = pairs(graph_, layout, centred, live ? (send ? w.unsent : w.fixed) : w.level);
                    if(silent || !at || !compiled) continue;
                    for(const auto& p : *compiled)
                    {
                        auto s = slots;
                        size_t next = w.faders.size();
                        if(pan) s[next++] = (*pan)[p.to ? 1 : 0];
                        if(send) s[next] = send;
                        gains[{w.from, at->first + p.from, p.to, s}] += p.gain;
                    }
                }
                for(const auto& [k, gain] : gains) tap.sources.push_back({std::get<0>(k), std::get<1>(k), std::get<2>(k), float(gain), std::get<3>(k)});
                if(!tap.channels) tap.channels = 2; // plays nowhere now: silent
                out.audio.push_back(std::move(tap));
            }
            else if(t.part)
            {
                const auto part = std::find_if(parts_.begin(), parts_.end(), [&](const Part& p) { return p.id == t.part; });
                if(part == parts_.end()) unknown("unknown part");
                Recorder::NoteTap tap{part == parts_.end() || part->name.empty() ? "Part " + std::to_string(t.part) : part->name, {}, false};
                // What the part receives: what its first plugins (each lane's, after a split) take from the graph input.
                for(const auto& w : wires_) if(w.kind == WireKind::Notes && w.owner == WireOwner::Part && w.part == t.part && w.from == graphInput) tap.nodes.push_back(w.to);
                out.notes.push_back(std::move(tap));
            }
            else if(t.node)
            {
                if(!graph_.contains(t.node)) unknown("unknown node");
                const auto name = nodes_.count(t.node) && !nodes_.at(t.node).empty() ? nodes_.at(t.node) : "Node " + std::to_string(t.node);
                out.notes.push_back({name, {t.node}, true});
            }
            else out.notes.push_back({"Input", {graphInput}, false});
        }
        return out;
    }

    std::vector<PartMeter> Session::partMeters() const
    {
        std::vector<PartMeter> out;
        for(const auto& [part, channels] : meters_)
        {
            out.push_back({part, channels.empty() ? 0.0 : *std::max_element(channels.begin(), channels.end()), channels});
            const auto p = std::find_if(parts_.begin(), parts_.end(), [&](const Part& q) { return q.id == part; });
            if(p != parts_.end()) for(const auto n : nodesOf(*p)) { const auto c = graph_.noteCost(n); out.back().stolen += c.stolen; out.back().hardChokes += c.hardChokes; }
        }
        return out;
    }

    std::vector<WireMeter> Session::wireMeters() const
    {
        std::vector<WireMeter> out;
        for(const auto& [wire, channels] : wireMeters_) out.push_back({wire, channels.empty() ? 0.0 : *std::max_element(channels.begin(), channels.end()), channels});
        return out;
    }

    void Session::setPartLevel(uint32_t id, double level, bool mute)
    {
        findPart(id);
        if(!std::isfinite(level) || level < 0) throw std::invalid_argument("invalid part level");
        auto change = [&] { auto& p = findPart(id); p.level = level; p.mute = mute; };
        if(liveFader(id)) editFaders(change);
        else editWires([&] { change(); buildParts(); });
    }

    void Session::setPartName(uint32_t id, const std::string& name)
    {
        findPart(id);
        editWires([&] { findPart(id).name = name; });
    }

    double MacroTarget::shape(double value) const
    {
        const double x = std::clamp(value, 0.0, 1.0);
        switch(curve)
        {
        case Curve::Linear: break;
        case Curve::Exponential: return (std::exp(4 * x) - 1) / (std::exp(4.0) - 1);
        case Curve::Logarithmic: return std::log1p((std::exp(4.0) - 1) * x) / 4;
        case Curve::SCurve: return x * x * (3 - 2 * x);
        case Curve::Db: return x > 0 ? std::pow(10.0, (low + (high - low) * x) / 20) : 0;
        case Curve::Power: return std::pow(x, exponent);
        }
        return x;
    }

    bool MacroTarget::validCurve() const
    {
        if(curve == Curve::Db) return std::isfinite(low) && std::isfinite(high) && low >= -144 && low < high && high <= 0;
        if(curve == Curve::Power) return std::isfinite(exponent) && exponent >= .1 && exponent <= 10;
        return true;
    }

    // Its point in the range, start at 0 and end at 1. Relative: that point less a half, so the
    // middle is no change. Absolute: how far that point is from the base, so alone it sets the value.
    double MacroTarget::offset(double value) const
    {
        const double point = start + (end - start) * shape(value);
        return point - (relative ? .5 : base);
    }

    double MacroTarget::map(double value) const { return std::clamp(base + offset(value), 0.0, 1.0); }

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
        case MacroTarget::Kind::Morph: return 0;
        }
        return 0;
    }

    uint32_t Session::addMacro(const std::string& name, uint32_t part)
    {
        if(part) findPart(part);
        macros_.push_back({nextMacro_, name, 0, {}, part});
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
        const auto owner = findMacro(id).part;
        const auto inside = owner ? nodesOf(findPart(owner)) : std::set<NodeId>{};
        auto targets = requested;
        if(targets.size() > 256) throw std::invalid_argument("at most 256 targets per control");
        for(auto& t : targets)
        {
            if(!std::isfinite(t.start) || !std::isfinite(t.end) || t.start < 0 || t.start > 1 || t.end < 0 || t.end > 1) throw std::invalid_argument("a target's range is within 0..1");
            if(!t.validCurve()) throw std::invalid_argument("a dB curve runs from -144 to 0 dB, low below high; a power curve's exponent is 0.1 to 10");
            switch(t.kind)
            {
            case MacroTarget::Kind::Parameter:
                if(!graph_.contains(t.node) || !hasParameter(t.node, t.param) || (parameterInfo(t.node, t.param).flags & CLAP_PARAM_IS_READONLY)) throw std::invalid_argument("not a writable parameter");
                if(owner && !inside.count(t.node)) throw std::invalid_argument("a part's control targets only the part's plugins");
                break;
            case MacroTarget::Kind::PartLevel: case MacroTarget::Kind::PartMute:
                if(owner) throw std::invalid_argument("a part's control targets only the part's plugins and controls");
                findPart(t.part); break;
            case MacroTarget::Kind::Morph:
                if(owner) throw std::invalid_argument("a part's control targets only the part's plugins and controls");
                if(t.sceneFrom == t.sceneTo) throw std::invalid_argument("a morph moves between two scenes");
                findScene(t.sceneFrom); findScene(t.sceneTo); break;
            case MacroTarget::Kind::Macro:
            {
                if(owner && findMacro(t.macro).part != owner) throw std::invalid_argument("a part's control targets only the part's own controls");
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
            // A destination's base is shared: a destination some control already drives keeps its
            // base (taken again first if something else has moved it since), so editing a control's
            // targets does not shift it; a new one starts from its value now.
            if(t.sums())
            {
                const MacroTarget* other = nullptr;
                for(const auto& m : macros_) for(const auto& o : m.targets) if(!other && o.sameDestination(t)) other = &o;
                const double now = currentFraction(t);
                const auto w = written_.find(destination(t));
                if(!other) t.base = now;
                else if(w != written_.end() && std::abs(w->second - now) > 1e-6) t.base = contributions(t, 0, 0).baseFor(now, other->base);
                else t.base = other->base;
                written_[destination(t)] = now;
            }
        }
        findMacro(id).targets = std::move(targets);
        for(const auto& t : findMacro(id).targets) if(t.sums()) setBase(t, t.base);
    }

    Session::Sum Session::contributions(const MacroTarget& t, uint32_t macro, double value) const { return contributions(macros_, mappingScene_, t, macro, value); }
    Session::Sum Session::contributions(const std::vector<Macro>& macros, uint32_t mappingScene, const MacroTarget& t, uint32_t macro, double value)
    {
        Sum sum;
        for(const auto& m : macros)
            for(const auto& o : m.targets)
                if(o.sameDestination(t) && !(mappingScene && std::find(o.offIn.begin(), o.offIn.end(), mappingScene) != o.offIn.end()))
                {
                    const double v = m.id == macro ? value : m.value;
                    sum.fixed += o.relative ? o.offset(v) : o.start + (o.end - o.start) * o.shape(v);
                    sum.absolute += !o.relative;
                }
        return sum;
    }

    void Session::setBase(const MacroTarget& t, double base) { setBase(macros_, t, base); }
    void Session::setBase(std::vector<Macro>& macros, const MacroTarget& t, double base)
    {
        for(auto& m : macros) for(auto& o : m.targets) if(o.sameDestination(t)) o.base = base;
    }

    void Session::noteBases()
    {
        written_.clear();
        for(const auto& m : macros_)
            for(const auto& t : m.targets)
                if(t.sums() && !written_.count(destination(t)))
                {
                    setBase(t, t.base);
                    try { written_[destination(t)] = currentFraction(t); } catch(const std::exception&) {}
                }
    }

    // Sets a control and, through its targets, everything below it; part faders are rebuilt once.
    void Session::driveMacro(uint32_t id, double value, bool& faders)
    {
        auto current = [&](const MacroTarget& t) -> std::optional<double> {
            if(t.kind == MacroTarget::Kind::Parameter && !(graph_.contains(t.node) && hasParameter(t.node, t.param))) return {};
            try { return currentFraction(t); } catch(const std::exception&) { return {}; }
        };
        drive(macros_, written_, mappingScene_, id, value, current, [&](const MacroTarget& t, double v) {
            switch(t.kind)
            {
            case MacroTarget::Kind::Parameter:
                if(graph_.contains(t.node) && hasParameter(t.node, t.param))
                {
                    const auto info = parameterInfo(t.node, t.param);
                    applyParameter(t.node, t.param, info.minimum + (info.maximum - info.minimum) * v);
                }
                break;
            case MacroTarget::Kind::PartLevel: for(auto& p : parts_) if(p.id == t.part) { p.level = v; faders = true; } break;
            case MacroTarget::Kind::PartMute: for(auto& p : parts_) if(p.id == t.part) { p.mute = v >= .5; faders = true; } break;
            case MacroTarget::Kind::Morph: morphScenes(t.sceneFrom, t.sceneTo, v); break;
            case MacroTarget::Kind::Macro: break; // drive's own
            }
        });
    }

    void Session::drive(std::vector<Macro>& macros, std::map<Destination, double>& written, uint32_t mappingScene, uint32_t id, double value, const MacroCurrent& current, const MacroApply& apply)
    {
        auto find = [&](uint32_t m) -> Macro& {
            const auto it = std::find_if(macros.begin(), macros.end(), [&](const Macro& x) { return x.id == m; });
            if(it == macros.end()) throw std::invalid_argument("unknown control");
            return *it;
        };
        auto& macro = find(id);
        const double was = macro.value;
        value = std::clamp(value, 0.0, 1.0);
        const auto targets = macro.targets; // `macro` may move as children are driven
        // Where each destination goes with this control at its new value: its base plus every
        // control's offset, clamped. A destination something else has moved since the controls
        // last set it (an edit, a scene, a glide) takes that as its base, less the offsets.
        std::vector<double> to(targets.size());
        for(size_t i = 0; i < targets.size(); ++i)
        {
            auto t = targets[i];
            if(!t.sums()) continue;
            const auto now = current(t);
            if(!now) continue;
            if(const auto w = written.find(destination(t)); w != written.end() && std::abs(w->second - *now) > 1e-6) { t.base = contributions(macros, mappingScene, t, id, was).baseFor(*now, t.base); setBase(macros, t, t.base); }
            to[i] = std::clamp(contributions(macros, mappingScene, t, id, value).at(t.base), 0.0, 1.0);
        }
        find(id).value = value;
        for(size_t i = 0; i < targets.size(); ++i)
        {
            const auto& t = targets[i];
            if(mappingScene && std::find(t.offIn.begin(), t.offIn.end(), mappingScene) != t.offIn.end()) continue; // off in this scene
            const double v = t.sums() ? to[i] : t.map(value);
            if(t.kind == MacroTarget::Kind::Macro) drive(macros, written, mappingScene, t.macro, v, current, apply);
            else apply(t, v);
            if(t.sums()) if(const auto now = current(t)) written[destination(t)] = *now;
        }
    }

    void Session::setMacro(uint32_t id, double value) { playMacro(id, value, true); }

    void Session::playMacro(uint32_t id, double value, bool liveGains)
    {
        if(!std::isfinite(value)) throw std::invalid_argument("a control's value is 0..1");
        findMacro(id);
        Quiet playing(*this); // moving a control is playing, not editing
        bool faders = false;
        const auto before = faderLevels();
        driveMacro(id, value, faders);
        if(!faders) return;
        // Faders with live gains move without a plan; any other compiles one, its gains ramping.
        const auto after = faderLevels();
        bool live = true;
        for(size_t i = 0; i < parts_.size() && i < before.size(); ++i) if(before[i] != after[i] && !liveFader(parts_[i].id)) live = false;
        bool muted = false;
        for(size_t i = 0; i < before.size() && i < after.size(); ++i) muted = muted || before[i].second != after[i].second;
        if(live) { buildParts(); if(liveGains) writeLiveGains(); if(muted) { ++revision_; publish(ChangeKind::Graph); } } // levels go out as values
        else { editWires([&] { buildParts(); }); rampGains_ = true; commit(); }
    }

    Scene& Session::findScene(uint32_t id)
    {
        const auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == id; });
        if(it == scenes_.end()) throw std::invalid_argument("unknown scene");
        return *it;
    }

    // Whether scenes touch a parameter: every automatable, writable one the user has not set aside,
    // except a split's own nodes, which follow its kind and lanes.
    bool Session::touched(NodeId node, const ParamInfo& p) const
    {
        return (p.flags & CLAP_PARAM_IS_AUTOMATABLE) && !(p.flags & CLAP_PARAM_IS_READONLY) && !splitNode(node) &&
               std::find(untouched_.begin(), untouched_.end(), SceneParameter{node, p.id}) == untouched_.end();
    }

    // A scene records all of it as it is now, pending edits included.
    void Session::capture(Scene& scene) const
    {
        scene.off.assign(disabled_.begin(), disabled_.end());
        scene.offWires.clear(); scene.wires.clear();
        for(const auto& w : wires_)
        {
            if(!w.enabled) scene.offWires.push_back(WireRef::of(w));
            if(w.owner == WireOwner::User && w.kind == WireKind::Audio) scene.wires.push_back({WireRef::of(w), w.level, w.pan, w.mute, w.solo});
        }
        scene.parts.clear();
        for(const auto& p : parts_)
        {
            Scene::PartState state{p.id, p.level, p.pan, p.mute, p.solo, p.sends, {}};
            for(const auto& sp : p.splits)
                for(uint32_t i = 0; i < sp.lanes.size(); ++i) state.lanes.push_back({sp.id, i, sp.lanes[i].level, sp.lanes[i].pan, sp.lanes[i].mute, sp.lanes[i].solo});
            scene.parts.push_back(std::move(state));
        }
        scene.values.clear();
        for(const auto& [node, name] : nodes_)
            for(const auto& p : parameters(node)) if(touched(node, p)) scene.values.push_back({{node, p.id}, parameter(node, p.id)});
        scene.macros.clear();
        for(const auto& m : macros_) scene.macros.push_back({m.id, m.value});
        scene.masterLevel = masterLevel_; scene.masterPan = masterPan_;
    }

    uint32_t Session::addScene(const std::string& name)
    {
        Scene scene; scene.id = nextScene_; scene.name = name;
        capture(scene);
        scenes_.push_back(std::move(scene)); scene_ = nextScene_;
        return nextScene_++;
    }

    void Session::storeScene(uint32_t id) { capture(findScene(id)); scene_ = id; }

    // The kinds in the scene's scope, each from what the scene holds; a part, lane, wire or plugin
    // added since keeps what it has. Plugins and wires switched off ring out (compileWires); faders
    // change route gains in a new plan and parameters arrive as events at the next block, so held
    // notes and tails go on. Staged graph edits are committed with it.
    void Session::applyScene(const Scene& scene, uint32_t scope)
    {
        const auto has = [&](Scene::Kind kind) { return (scope & kind) != 0; };
        const bool was = applyingScene_; applyingScene_ = true; // a morph control it moves does not morph meanwhile
        struct Restore { bool& flag; bool value; ~Restore() { flag = value; } } restore{applyingScene_, was};
        // Controls first: the scene's own faders and parameter values then have the last word. A
        // control that morphs between scenes keeps its place: it is the hand moving it.
        bool faders = false;
        if(has(Scene::Parameters))
            for(const auto& [macro, value] : scene.macros)
                if(const auto m = std::find_if(macros_.begin(), macros_.end(), [&](const Macro& x) { return x.id == macro; }); m != macros_.end() &&
                   std::none_of(m->targets.begin(), m->targets.end(), [](const MacroTarget& t) { return t.kind == MacroTarget::Kind::Morph; }))
                    driveMacro(macro, value, faders);
        bool switched = false; // a plugin on or off: its wires compile
        if(has(Scene::Enabled))
            for(const auto& [node, name] : nodes_)
            {
                const bool on = std::find(scene.off.begin(), scene.off.end(), node) == scene.off.end();
                if(on == (disabled_.count(node) != 0)) { enable(node, on); switched = true; }
            }
        editMix(switched, [&] {
            if(has(Scene::Mix)) { masterLevel_ = scene.masterLevel; masterPan_ = scene.masterPan; }
            for(const auto& state : scene.parts)
                for(auto& p : parts_)
                {
                    if(p.id != state.part) continue;
                    if(has(Scene::Mutes)) { p.mute = state.mute; p.solo = state.solo; }
                    if(!has(Scene::Mix)) continue;
                    p.level = state.level; p.pan = state.pan;
                    for(auto& send : p.sends) for(const auto& was : state.sends) if(was.bus == send.bus) send.level = was.level;
                    for(auto& sp : p.splits)
                        for(const auto& lane : state.lanes)
                            if(lane.split == sp.id && lane.lane < sp.lanes.size())
                            { auto& l = sp.lanes[lane.lane]; l.level = lane.level; l.pan = lane.pan; l.mute = lane.mute; l.solo = lane.solo; }
                }
            buildParts();
            for(auto& w : wires_)
            {
                const auto ref = WireRef::of(w);
                if(has(Scene::Enabled)) w.enabled = std::find(scene.offWires.begin(), scene.offWires.end(), ref) == scene.offWires.end();
                if(has(Scene::Mix) && w.owner == WireOwner::User)
                    for(const auto& m : scene.wires) if(m.wire == ref) { w.level = m.level; w.pan = m.pan; w.mute = m.mute; w.solo = m.solo; }
            }
        });
        commit();
        // Only values that differ are sent, past the pending commands' limit if need be: a plugin
        // takes what it can each tick and the rest follow at the next.
        if(has(Scene::Parameters))
            for(const auto& v : scene.values)
                if(graph_.contains(v.at.node) && hasParameter(v.at.node, v.at.param) && parameter(v.at.node, v.at.param) != v.value) applyParameter(v.at.node, v.at.param, v.value, nullptr, false);
    }

    // Selecting a scene: at once, or at the next beat or bar while the transport plays (it waits,
    // and selecting another replaces it); then over its glide, or at once.
    void Session::selectScene(uint32_t id)
    {
        const auto scene = findScene(id);
        if(scene.launch != Scene::Launch::Now && statusPlaying_.load())
        {
            // shortcut: bars of 4 beats, the transport's only meter, checked at each service tick (about 5 ms); schedule at the sample when scenes must land exactly.
            const double unit = scene.launch == Scene::Launch::Bar ? 4 : 1;
            pendingScene_ = PendingScene{id, (std::floor(statusBeat_.load() / unit + 1e-9) + 1) * unit};
            return;
        }
        pendingScene_.reset();
        startScene(scene);
    }

    void Session::startScene(const Scene& scene)
    {
        Quiet playing(*this); // switching scenes is playing, not editing
        transition_.reset();
        if(scene.scope & Scene::Mappings) mappingScene_ = scene.id;
        scene_ = scene.id;
        if(!scene.glide) { applyScene(scene, scene.scope); return; }
        Transition t;
        capture(t.from);
        t.to = scene; t.start = framesProcessed_.load();
        t.frames = std::max<uint64_t>(1, uint64_t(scene.glide * sampleRate_ / 1000));
        transition_ = std::move(t);
        blendScenes(transition_->from, scene, 0, scene.curve, 0, scene.scope); // what cannot glide changes now
        transition_->applied = faderLevels();
    }

    // Each service tick: a scene waiting for its beat, and a glide's next step. Both wait while
    // graph edits are staged, rather than committing them.
    void Session::advanceScenes()
    {
        if(staged_) return;
        if(pendingScene_ && (!statusPlaying_.load() || statusBeat_.load() >= pendingScene_->at))
        {
            const auto id = pendingScene_->scene;
            pendingScene_.reset();
            if(std::any_of(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == id; })) startScene(findScene(id));
        }
        if(!transition_) return;
        auto& t = *transition_;
        // A fader moved by hand since the last step: the glide stops where it is.
        if(faderLevels() != t.applied) { transition_.reset(); return; }
        const double at = double(framesProcessed_.load() - t.start) / double(t.frames);
        Quiet playing(*this);
        if(at >= 1) { applyScene(t.to, t.to.scope); transition_.reset(); return; }
        blendScenes(t.from, t.to, at, t.to.curve, 0, t.to.scope);
        t.applied = faderLevels();
    }

    std::vector<std::pair<double, bool>> Session::faderLevels() const
    {
        std::vector<std::pair<double, bool>> out;
        for(const auto& p : parts_) out.push_back({p.level, p.mute});
        return out;
    }

    // The state at t (0..1) between two scenes, for the kinds in scope. Levels, pans, sends, Mix
    // rows, controls and smooth parameters move along the curve (equal power moves levels by their
    // power); what cannot glide (plugins and wires on or off, solos, stepped parameters) is `to`'s
    // from switchAt on. A part, lane or user wire muted on one side glides from or to silence,
    // unmuted on the way.
    void Session::blendScenes(const Scene& from, const Scene& to, double t, Scene::Curve curve, double switchAt, uint32_t scope)
    {
        if(t >= 1) { applyScene(to, scope); return; }
        if(t <= 0 && switchAt > 0) { applyScene(from, scope); return; }
        const double eased = curve == Scene::Curve::Ease ? t * t * (3 - 2 * t) : t;
        const auto mix = [&](double a, double b) { return a + (b - a) * eased; };
        const auto gain = [&](double a, double b) { return curve == Scene::Curve::EqualPower ? std::sqrt((1 - t) * a * a + t * b * b) : mix(a, b); };
        const bool switched = t >= switchAt;
        Scene mid = to;
        if(!switched) { mid.off = from.off; mid.offWires = from.offWires; }
        for(auto& p : mid.parts)
        {
            const auto f = std::find_if(from.parts.begin(), from.parts.end(), [&](const auto& x) { return x.part == p.part; });
            if(f == from.parts.end()) continue;
            p.level = gain(f->mute ? 0 : f->level, p.mute ? 0 : p.level);
            p.mute = f->mute && p.mute;
            if(!switched) p.solo = f->solo;
            p.pan = mix(f->pan, p.pan);
            for(auto& s : p.sends)
                for(const auto& was : f->sends) if(was.bus == s.bus) s.level = gain(was.level, s.level);
            for(auto& l : p.lanes)
                for(const auto& was : f->lanes)
                    if(was.split == l.split && was.lane == l.lane)
                    {
                        l.level = gain(was.mute ? 0 : was.level, l.mute ? 0 : l.level); l.mute = was.mute && l.mute;
                        if(!switched) l.solo = was.solo;
                        l.pan = mix(was.pan, l.pan);
                    }
        }
        for(auto& w : mid.wires)
            for(const auto& was : from.wires)
                if(was.wire == w.wire)
                {
                    w.level = gain(was.mute ? 0 : was.level, w.mute ? 0 : w.level); w.mute = was.mute && w.mute;
                    if(!switched) w.solo = was.solo;
                    w.pan = mix(was.pan, w.pan);
                }
        for(auto& v : mid.values)
            for(const auto& was : from.values)
                if(was.at == v.at)
                {
                    const bool stepped = graph_.contains(v.at.node) && hasParameter(v.at.node, v.at.param) && (parameterInfo(v.at.node, v.at.param).flags & CLAP_PARAM_IS_STEPPED);
                    v.value = stepped ? (switched ? v.value : was.value) : mix(was.value, v.value);
                }
        for(auto& [macro, value] : mid.macros)
            for(const auto& [m, was] : from.macros) if(m == macro) value = mix(was, value);
        mid.masterLevel = gain(from.masterLevel, to.masterLevel); mid.masterPan = mix(from.masterPan, to.masterPan);
        applyScene(mid, scope);
    }

    // A control moving between two scenes by hand: the state at its value, what cannot glide
    // switching halfway. Not while a scene is being applied (that control keeps its place).
    void Session::morphScenes(uint32_t fromId, uint32_t toId, double t)
    {
        if(applyingScene_) return;
        const auto f = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == fromId; });
        const auto g = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == toId; });
        if(f == scenes_.end() || g == scenes_.end()) return;
        const auto from = *f, to = *g;
        transition_.reset(); pendingScene_.reset();
        if(to.scope & Scene::Mappings) mappingScene_ = t >= .5 ? to.id : from.id;
        blendScenes(from, to, t, to.curve, .5, to.scope);
    }

    void Session::removeScene(uint32_t id)
    {
        findScene(id);
        std::erase_if(scenes_, [&](const Scene& s) { return s.id == id; });
        for(auto& m : macros_)
        {
            for(auto& t : m.targets) std::erase(t.offIn, id);
            std::erase_if(m.targets, [&](const MacroTarget& t) { return t.kind == MacroTarget::Kind::Morph && (t.sceneFrom == id || t.sceneTo == id); });
        }
        if(pendingScene_ && pendingScene_->scene == id) pendingScene_.reset();
        if(scene_ == id) scene_ = 0;
        if(mappingScene_ == id) mappingScene_ = 0;
    }

    void Session::setSceneName(uint32_t id, const std::string& name) { findScene(id).name = name; }

    void Session::setSceneTransition(uint32_t id, uint32_t glide, Scene::Curve curve, Scene::Launch launch)
    {
        if(glide > 60000) throw std::invalid_argument("a scene's glide is at most a minute");
        auto& scene = findScene(id);
        scene.glide = glide; scene.curve = curve; scene.launch = launch;
    }

    void Session::setSceneScope(uint32_t id, uint32_t scope)
    {
        if(scope & ~uint32_t(Scene::Everything)) throw std::invalid_argument("unknown scene kind");
        findScene(id).scope = scope;
    }

    void Session::setSceneUntouched(const std::vector<SceneParameter>& list)
    {
        for(size_t i = 0; i < list.size(); ++i)
        {
            const auto& at = list[i];
            if(!graph_.contains(at.node) || !hasParameter(at.node, at.param)) throw std::invalid_argument("unknown parameter");
            if(std::find(list.begin(), list.begin() + std::ptrdiff_t(i), at) != list.begin() + std::ptrdiff_t(i)) throw std::invalid_argument("a parameter listed twice");
        }
        // Set aside: its values go from every scene. Taken back: it gets its current value in each.
        untouched_ = list;
        for(auto& scene : scenes_)
        {
            std::erase_if(scene.values, [&](const Scene::Value& v) { return std::find(untouched_.begin(), untouched_.end(), v.at) != untouched_.end(); });
            for(const auto& [node, name] : nodes_)
                for(const auto& p : parameters(node))
                    if(touched(node, p) && std::none_of(scene.values.begin(), scene.values.end(), [&](const Scene::Value& v) { return v.at == SceneParameter{node, p.id}; }))
                        scene.values.push_back({{node, p.id}, parameter(node, p.id)});
        }
    }

    void Session::ignoreMidiInputs(const std::vector<std::string>& names)
    {
        ignoredInputs_ = names;
        uint64_t mask = 0;
        for(const auto& input : hostStatus_.midiInputs)
            if(input.source < maxSources && std::find(names.begin(), names.end(), input.name) != names.end()) mask |= uint64_t(1) << input.source;
        ignoredSources_.store(mask, std::memory_order_release);
    }

    void Session::setHostStatus(SessionStatus status)
    {
        const bool moved = status.midiInputs != hostStatus_.midiInputs, outputs = status.midiOutputs != hostStatus_.midiOutputs;
        hostStatus_ = std::move(status);
        if(moved) ignoreMidiInputs(ignoredInputs_); // the ignored names' sources now
        if(outputs) setClockOutputs(clockOutputs_);
        // Selectors resolve again: inputs' for parts choosing inputs or sending MIDI from their input, outputs' for parts sending MIDI.
        const bool sending = std::any_of(parts_.begin(), parts_.end(), [](const Part& p) { return !p.midiOut.devices.empty(); });
        if(!(moved && (sending || std::any_of(parts_.begin(), parts_.end(), [](const Part& p) { return !p.input.devices.empty(); }))) && !(outputs && sending)) return;
        const bool staged = staged_;
        Quiet devices(*this); // devices coming and going are not edits
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
            // Its controls go with it, and every target on them.
            std::set<uint32_t> owned; for(const auto& m : macros_) if(m.part == id) owned.insert(m.id);
            std::erase_if(macros_, [&](const Macro& m) { return owned.count(m.id) != 0; });
            for(auto& m : macros_) std::erase_if(m.targets, [&](const MacroTarget& t) { return t.kind == MacroTarget::Kind::Macro && owned.count(t.macro); });
            for(auto& scene : scenes_) std::erase_if(scene.macros, [&](const auto& v) { return owned.count(v.first) != 0; });
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
        // A node belongs to one part (RFC 0011): in two, it would play twice and both would own it.
        if(std::any_of(parts_.begin(), parts_.end(), [&](Part& p) { return holds(p, node); })) throw std::invalid_argument("the node is already in a part");
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
        if(splitter) { applyParameter(splitter, 1, kind == Split::Kind::LeftRight ? 0 : kind == Split::Kind::MidSide ? 1 : 2); applyParameter(splitter, 2, 2); }
        if(joiner) applyParameter(joiner, 1, kind == Split::Kind::LeftRight ? 0 : 1);
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
        if(s.kind == Split::Kind::Bands) applyParameter(s.splitter, 2, double(lane + 1));
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
        if(kind == Split::Kind::Bands) applyParameter(splitter, 2, double(splitOf(findPart(id), split).lanes.size()));
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
            const auto was = w; // what it carried before, including any empty buses' level, pan and mute
            w.level = level; w.pan = pan; w.mute = mute; w.solo = solo;
            // A part's output and send wires carry its fader: setting them sets the part and send.
            // The wire also carries the level, pan and mute of any empty buses it passes through,
            // and the Routing view sends a row's own pan and mute with each level: the part takes
            // the change asked, so the wire lands where asked and the buses keep their share.
            // (A wire through a level-0 bus carries 0 and cannot be divided: it takes the asked level.)
            if(w.owner == WireOwner::Part && (w.fader || w.send))
            {
                auto& p = findPart(w.part);
                if(w.send) { for(auto& s : p.sends) if(s.bus == w.send) s.level = was.level > 0 ? level * s.level / was.level : (p.level > 0 ? level / p.level : level); }
                else
                {
                    p.level = was.level > 0 ? level * p.level / was.level : level;
                    if(mute != was.mute) p.mute = mute;
                    p.pan = std::clamp(pan - was.share, -1.0, 1.0); // the unclamped share: a wire pinned at the edge still moves
                }
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
            for(auto& m : macros_) std::erase_if(m.targets, [&](const MacroTarget& t) { return t.kind == MacroTarget::Kind::Parameter && t.node == node; });
            for(auto& scene : scenes_)
            {
                auto on = [&](const WireRef& r) { return r.from == node || r.to == node; };
                std::erase_if(scene.values, [&](const Scene::Value& v) { return v.at.node == node; });
                std::erase(scene.off, node);
                std::erase_if(scene.offWires, on); std::erase_if(scene.wires, [&](const Scene::WireMix& m) { return on(m.wire); });
            }
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

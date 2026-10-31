// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 1: parts and wire ownership, implicit Mixes, port-level routes with channel
// rules, plugin replacement and Performance persistence.
//   oscairn-parts-test FIXTURE.clap AUDIO_FIXTURE.clap DELAY.clap
#include "oscairn/session.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    constexpr uint32_t frames = 64;
    const AudioLayout stereoIn{{{0, "In", 2, 0}}, {{0, "Main", 2, 0}}};

    struct Audio
    {
        std::array<float, frames> inL{}, inR{}, l{}, r{};
        const float* in[2]{inL.data(), inR.data()};
        float* out[2]{l.data(), r.data()};
        void run(Session& s, uint32_t inputs = 0)
        {
            s.service(); forbidAllocation = true;
            const bool ok = s.process(inputs ? in : nullptr, inputs, out, 2, frames);
            forbidAllocation = false; check(ok, "render");
        }
    };
    std::vector<uint32_t> ids(const Session& s, WireOwner owner)
    {
        std::vector<uint32_t> r;
        for(const auto& w : s.wires()) if(w.owner == owner) r.push_back(w.id);
        return r;
    }
    const Wire& wire(const Session& s, NodeId from, NodeId to, WireKind kind = WireKind::Audio)
    {
        for(const auto& w : s.wires()) if(w.from == from && w.to == to && w.kind == kind) return w;
        throw std::runtime_error("missing wire");
    }
    bool near(float a, double b) { return std::abs(a - b) < 1e-6; }

    // Part edits change only part wires; a part's wires follow its chain.
    void ownership(const char* fixture, const char* delay)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay), send = s.add("send", delay);
        const auto keys = s.addPart("Keys");
        s.insertIntoPart(keys, 0, synth);
        check(wire(s, graphInput, synth, WireKind::Notes).owner == WireOwner::Part && wire(s, synth, graphOutput).owner == WireOwner::Part, "a new part listens to the keyboard and plays to Main");
        s.connectAudio(synth, send); s.connectAudio(send, graphOutput); // a send drawn in Routing
        const auto user = ids(s, WireOwner::User);
        check(user.size() == 2, "two user wires");
        s.insertIntoPart(keys, 1, fx);
        check(wire(s, synth, fx).owner == WireOwner::Part && wire(s, fx, graphOutput).owner == WireOwner::Part, "inserting replaces synth→Main with synth→fx→Main");
        check(std::none_of(s.wires().begin(), s.wires().end(), [&](const Wire& w) { return w.from == synth && w.to == graphOutput; }), "the old part wire is gone");
        check(ids(s, WireOwner::User) == user, "inserting keeps the user wires");
        s.movePartNode(keys, 1, 0); check(ids(s, WireOwner::User) == user, "reordering keeps the user wires");
        s.removeFromPart(keys, synth); check(ids(s, WireOwner::User) == user, "removing from the chain keeps the user wires");
        s.insertIntoPart(keys, 0, synth); s.movePartNode(keys, 1, 0); s.movePartNode(keys, 0, 1);
        s.removePart(keys);
        check(ids(s, WireOwner::User) == user && ids(s, WireOwner::Part).empty(), "removing the part keeps the user wires and its nodes");
        check(s.hasNode(synth) && s.hasNode(fx), "the part's nodes stay");
        bool refused = false; try { s.insertIntoPart(keys, 0, synth); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "unknown part refused");
        // A node belongs to one part: in two it would play twice (the 2026-10-31 audit).
        const auto a = s.addPart("A"), b = s.addPart("B");
        s.insertIntoPart(a, 0, synth);
        refused = false; try { s.insertIntoPart(b, 0, synth); } catch(const std::invalid_argument&) { refused = true; }
        check(refused && s.parts().back().chain.empty(), "a node already in another part is refused");
        s.commit();
    }

    // Port-level routes: mono to stereo copied with pan, stereo to mono summed at -3 dB, explicit
    // pairs, and anything else refused without a choice. Each input's implicit Mix: level, pan,
    // mute and solo per source.
    void channels(const char* audio)
    {
        Session s(48000, frames, {}, stereoIn);
        const auto a = s.add("a", audio, "org.oscairn.test.audio-ports");
        // The input's stereo into a's mono sidechain (port 20), a's mono aux (port 40) to Main.
        Wire side; side.from = graphInput; side.fromPort = 0; side.to = a; side.toPort = 20;
        Wire aux; aux.from = a; aux.fromPort = 40; aux.to = graphOutput; aux.toPort = 0;
        s.connectPorts(side); const auto auxId = s.connectPorts(aux); s.commit();
        Audio x; x.inL.fill(.2f); x.inR.fill(.6f);
        x.run(s, 2);
        const double summed = (.2 + .6) * M_SQRT1_2;
        check(near(x.l[0], summed) && near(x.r[0], summed), "stereo to mono sums at -3 dB, mono to stereo copies: " + std::to_string(x.l[0]));
        s.setMix(auxId, 1, -1, false, false); s.commit(); x.run(s, 2);
        check(near(x.l[0], summed) && near(x.r[0], 0), "pan left on a mono-to-stereo wire");
        // Explicit pairs: only the input's right channel into the sidechain.
        Wire right = side; right.channels = {{1, 0}};
        s.disconnectAudio(graphInput, a); s.connectPorts(right); s.setMix(auxId, 1, 0, false, false); s.commit(); x.run(s, 2);
        check(near(x.l[0], .6) && near(x.r[0], .6), "an explicit channel pair");
        // A second source into Main makes a Mix: the input's stereo straight through.
        Wire direct; direct.from = graphInput; direct.fromPort = 0; direct.to = graphOutput; direct.toPort = 0;
        const auto directId = s.connectPorts(direct); s.commit(); x.run(s, 2);
        check(near(x.l[0], .8) && near(x.r[0], 1.2), "two sources sum in the Mix");
        s.setMix(directId, .5, 0, false, false); s.commit(); x.run(s, 2);
        check(near(x.l[0], .7) && near(x.r[0], .9), "a source's level");
        s.setMix(directId, .5, 0, false, true); s.commit(); x.run(s, 2);
        check(near(x.l[0], .1) && near(x.r[0], .3), "solo silences the other sources");
        s.setMix(directId, .5, 0, true, true); s.commit(); x.run(s, 2);
        check(near(x.l[0], 0) && near(x.r[0], 0), "a muted source stays silent when soloed, and its solo still silences the others");
        // Anything else needs a choice.
        Session quad(48000, frames, {}, {{{0, "In", 2, 0}}, {{0, "Quad", 4, 0}}});
        Wire four; four.from = graphInput; four.to = graphOutput;
        bool refused = false; try { quad.connectPorts(four); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "stereo to four channels needs an explicit choice");
        four.channels = {{0, 0}, {1, 1}, {0, 2}, {1, 3}}; quad.connectPorts(four); quad.commit();
        check(quad.graphState().topology.audioRoutes.size() >= 1, "with pairs it connects");
    }

    // Replacing a plugin keeps or flags every wire; removing a node can reconnect its user wires.
    void replacement(const char* audio, const char* delay)
    {
        Session s(48000, frames, {}, stereoIn);
        const auto a = s.add("a", audio, "org.oscairn.test.audio-ports");
        const auto guitar = s.addPart("Guitar", {PartInput::Kind::Audio, 0});
        s.insertIntoPart(guitar, 0, a);
        Wire side; side.from = graphInput; side.fromPort = 0; side.to = a; side.toPort = 20; s.connectPorts(side);
        s.commit();
        s.replace(a, delay, "org.oscairn.delay");
        check(s.wires().size() == 3, "replacement keeps every wire");
        size_t connected = 0, flagged = 0;
        for(const auto& w : s.wires()) (w.connected ? connected : flagged)++;
        check(connected == 2 && flagged == 1, "the part wires move to the delay's main ports; the sidechain wire stays, unconnected");
        for(const auto& w : s.wires()) if(!w.connected) check(w.owner == WireOwner::User && w.to == a, "the flagged wire is the sidechain one");
        Audio x; x.inL.fill(.5f); x.inR.fill(.5f);
        for(int i = 0; i < 4; ++i) x.run(s, 2);
        check(x.l[frames - 1] != 0, "the delay plays in the part");

        // Removing a node between two user wires, reconnecting them.
        Session r(48000, frames, {}, stereoIn);
        const auto b = r.add("b", delay);
        r.connectAudio(graphInput, b); r.connectAudio(b, graphOutput); r.commit();
        r.remove(b, UserWires::Reconnect); r.commit();
        check(r.wires().size() == 1 && r.wires()[0].from == graphInput && r.wires()[0].to == graphOutput && r.wires()[0].owner == WireOwner::User, "reconnected around the removed node");
        Audio y; y.inL.fill(.25f); y.inR.fill(.75f); y.run(r, 2);
        check(near(y.l[0], .25) && near(y.r[0], .75), "the input plays straight to Main");
    }

    // Two parts split the keyboard by zone; moving the split while notes are held strands none.
    void zones(const char* fixture)
    {
        Session s(48000, frames);
        const auto low = s.add("low", fixture, "org.kick.test.synth"), high = s.add("high", fixture, "org.kick.test.synth");
        s.setParameter(low, 7, .25); s.setParameter(high, 7, .5);
        NoteZone below; below.highKey = 59;
        NoteZone above; above.lowKey = 60;
        const auto bass = s.addPart("Bass", {PartInput::Kind::Notes, 0, {below}}), lead = s.addPart("Lead", {PartInput::Kind::Notes, 0, {above}});
        s.insertIntoPart(bass, 0, low); s.insertIntoPart(lead, 0, high); s.commit();
        Audio x; x.run(s);
        s.note(true, 40, 1); x.run(s); check(near(x.l[0], .25), "a low key plays only the bass part: " + std::to_string(x.l[0]));
        s.note(true, 80, 1); x.run(s); check(near(x.l[0], .75), "a high key adds the lead part");
        s.note(false, 40, 0); x.run(s); check(near(x.l[0], .5), "releasing the low key leaves the lead");
        above.lowKey = 30; s.setPartInput(lead, {PartInput::Kind::Notes, 0, {above}}); s.commit();
        s.note(false, 80, 0); x.run(s); check(near(x.l[0], 0), "the lead's held note is released after its zone moved");
        s.note(true, 40, 1); x.run(s); check(near(x.l[0], .75), "the moved split: 40 now plays both parts");
        s.note(false, 40, 0); x.run(s); check(near(x.l[0], 0), "and releases both");
        // A part's fader and mute.
        s.setPartLevel(bass, .5, false); s.commit();
        s.note(true, 20, 1); x.run(s); check(near(x.l[0], .125), "the bass part at half level: " + std::to_string(x.l[0]));
        s.setPartLevel(bass, .5, true); s.commit(); x.run(s); check(near(x.l[0], 0), "muted");
        for(const auto& w : s.wires()) if(w.fader && w.part == bass) s.setMix(w.id, 1, 0, false, false);
        s.commit(); x.run(s);
        check(near(x.l[0], .25) && s.parts()[0].level == 1 && !s.parts()[0].mute, "the output wire's Mix sets the part's fader");
        s.note(false, 20, 0); x.run(s);
        s.setPartName(bass, "Low end"); check(s.parts()[0].name == "Low end", "renamed");
        bool refused = false; NoteZone bad; bad.curve = 0;
        try { s.setPartInput(lead, {PartInput::Kind::Notes, 0, {bad}}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "an invalid zone is refused");
    }

    // RFC 0012 device selectors: rules resolve against the host's MIDI inputs, again whenever
    // they change, and a part hears only the devices its selector chooses.
    void devices(const char* fixture, const fs::path& root)
    {
        using K = DeviceRule::Kind; using M = DeviceRule::Match; using E = MidiEndpoint::Kind;
        const std::vector<MidiEndpoint> table{{E::Usb, 1, "Arturia KeyLab 61"}, {E::Usb, 2, "nanoKONTROL2"}, {E::Port, 3, "MIDI 2", 2}, {E::Network, 4, "Session 1"}};
        auto mask = [&](std::vector<DeviceRule> rules) { return resolveDevices(rules, table); };
        check(mask({}) == ~0ull && mask({{K::All}}) == ~0ull, "no rules, or All: everything");
        check(mask({{K::BuiltIn}}) == 1, "built in: the panel's own notes");
        check(mask({{K::External}}) == 0b11110, "all external");
        check(mask({{K::UsbNamed, M::Contains, 0, "keylab"}}) == 0b10, "USB named, containing a text, ignoring case");
        check(mask({{K::UsbNamed, M::Regex, 0, "^nano"}}) == 0b100, "USB named by a regular expression");
        check(mask({{K::UsbNamed, M::Equals, 0, "Arturia KeyLab"}}) == 0, "equals is the whole name");
        check(mask({{K::Port, M::Contains, 2}}) == 0b1000 && mask({{K::Port, M::Contains, 1}}) == 0, "a numbered port");
        check(mask({{K::Network}, {K::BuiltIn}}) == 0b10001, "network sessions, and a list of rules");
        check(!DeviceRule{K::UsbNamed, M::Regex, 0, "("}.valid() && !DeviceRule{K::UsbNamed}.valid(), "a bad pattern, or none, is invalid");

        Session s(48000, frames);
        const auto inside = s.add("inside", fixture, "org.kick.test.synth"), outside = s.add("outside", fixture, "org.kick.test.synth");
        s.setParameter(inside, 7, .25); s.setParameter(outside, 7, .5);
        Control::SessionStatus status; status.midiInputs = {{E::Usb, 5, "Arturia KeyLab 61"}};
        s.setHostStatus(status);
        const auto keys = s.addPart("Keys", {PartInput::Kind::Notes, 0, {}, {{K::BuiltIn}}});
        const auto ext = s.addPart("External", {PartInput::Kind::Notes, 0, {}, {{K::UsbNamed, M::Contains, 0, "keylab"}}});
        s.insertIntoPart(keys, 0, inside); s.insertIntoPart(ext, 0, outside); s.commit();
        Audio x; x.run(s);
        auto play = [&](bool on, int key, uint8_t source) {
            const auto e = kick::clap::Event::midi(on ? 0x90 : 0x80, uint8_t(key), on ? 127 : 0);
            s.service(); forbidAllocation = true;
            const bool ok = s.process(nullptr, 0, x.out, 2, frames, {&e, 1}, {}, {&source, 1});
            forbidAllocation = false; check(ok, "render with sources");
        };
        s.note(true, 60, 1); x.run(s); check(near(x.l[0], .25), "the panel plays only Keys: " + std::to_string(x.l[0]));
        play(true, 62, 5); check(near(x.l[0], .75), "the KeyLab, on the same channel, plays External");
        play(true, 64, 6); check(near(x.l[0], .75), "another device plays neither");
        play(false, 62, 5); s.note(false, 60, 0); x.run(s); check(near(x.l[0], 0), "both released");
        // Plugged in again under another number: the selector follows it, applied at once.
        status.midiInputs = {{E::Usb, 7, "Arturia KeyLab 61"}}; s.setHostStatus(status);
        play(true, 62, 7); check(near(x.l[0], .5) && !s.graphState().staged, "the KeyLab as source 7 plays External, no commit needed");
        play(false, 62, 7); play(true, 62, 5); check(near(x.l[0], 0), "its old number no longer does");
        play(false, 62, 5);
        // With edits staged, the new devices wait for their commit.
        s.setPartLevel(ext, .5, false);
        status.midiInputs = {{E::Usb, 8, "Arturia KeyLab 61"}}; s.setHostStatus(status);
        check(s.graphState().staged, "staged edits stay staged");
        s.commit(); play(true, 62, 8); check(near(x.l[0], .25), "committed together: source 8 at half level");
        play(false, 62, 8);
        // Unplugged: the part hears nothing, and keeps its selector.
        status.midiInputs = {}; s.setHostStatus(status);
        play(true, 62, 8); check(near(x.l[0], 0) && s.parts()[1].input.devices.size() == 1, "no device: silent, selector kept");
        play(false, 62, 8);
        bool refused = false;
        try { s.setPartInput(ext, {PartInput::Kind::Notes, 0, {}, {{K::UsbNamed, M::Regex, 0, "["}}}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "an invalid pattern is refused");

        // Performances store selectors; another machine resolves them against its own devices.
        const auto saved = root / "devices";
        s.savePerformance(saved.string());
        Session t(48000, frames);
        status.midiInputs = {{E::Usb, 9, "KeyLab mkII 49"}}; t.setHostStatus(status);
        t.loadPerformance(saved.string());
        check(t.parts()[1].input.devices == s.parts()[1].input.devices, "selectors round-trip");
        Audio y; y.run(t);
        const auto e = kick::clap::Event::midi(0x90, 62, 127); const uint8_t nine = 9;
        t.service(); const bool ok = t.process(nullptr, 0, y.out, 2, frames, {&e, 1}, {}, {&nine, 1});
        check(ok && near(y.l[0], .25), "a different KeyLab there plays External: " + std::to_string(y.l[0]));
        const auto off = kick::clap::Event::midi(0x80, 62, 0), other = kick::clap::Event::midi(0x90, 64, 127); const uint8_t five = 5;
        t.process(nullptr, 0, y.out, 2, frames, {&off, 1}, {}, {&nine, 1}); t.process(nullptr, 0, y.out, 2, frames, {&other, 1}, {}, {&five, 1});
        check(near(y.l[0], 0), "a device that does not match there plays nothing");
    }

    // RFC 0011 step 3: splits. Lanes built with part edits render exactly as the same graph wired
    // by hand; lanes' zones, Mix, solo and nesting; edits; persistence.
    void splits(const char* fixture, const char* delay, const fs::path& root)
    {
        { // Two zoned layers joining into one effect, inside the part's own zone.
            Session s(48000, frames);
            const auto low = s.add("low", fixture, "org.kick.test.synth"), high = s.add("high", fixture, "org.kick.test.synth"), fx = s.add("fx", delay);
            s.setParameter(low, 7, .25); s.setParameter(high, 7, .5);
            NoteZone keyboard; keyboard.lowKey = 36; keyboard.highKey = 96;
            const auto keys = s.addPart("Keys", {PartInput::Kind::Notes, 0, {keyboard}});
            const auto split = s.addSplit(keys, 0);
            s.insertIntoPart(keys, 0, low, {split, 0}); s.insertIntoPart(keys, 0, high, {split, 1});
            s.insertIntoPart(keys, 1, fx); // after the split: both lanes join into it
            NoteZone below; below.highKey = 59; NoteZone above; above.lowKey = 60;
            Lane a; a.zones = {below}; a.level = .5; s.setLane(keys, split, 0, a);
            Lane b; b.zones = {above}; b.level = .8; s.setLane(keys, split, 1, b);
            s.commit();

            auto byHand = [&] {
                auto g = std::make_unique<Graph>(48000, frames);
                const auto gl = g->add(fixture, "org.kick.test.synth"), gh = g->add(fixture, "org.kick.test.synth"), gr = g->add(delay);
                g->host(gl).setParameter(7, .25); g->host(gh).setParameter(7, .5);
                NoteZone lowKeys = keyboard; lowKeys.highKey = 59; NoteZone highKeys = keyboard; highKeys.lowKey = 60;
                g->connectNotes(graphInput, gl, {lowKeys}); g->connectNotes(graphInput, gh, {highKeys});
                for(const auto [node, gain] : {std::pair{gl, .5}, std::pair{gh, .8}})
                    for(uint32_t c = 0; c < 2; ++c) g->connectAudio({{node, 0, 0}, {gr, g->host(gr).mainAudioPorts(true).front(), c}, 1, gain});
                g->connectAudio(gr, graphOutput); g->commit();
                return g;
            };
            // Renders a session against a fresh hand-wired graph, with keys outside, below and above the split.
            auto compare = [&](Session& x) {
                auto g = byHand();
                Audio a; std::array<float, frames> rl{}, rr{}; float* ref[2]{rl.data(), rr.data()};
                bool same = true; double energy = 0;
                for(int block = 0; block < 300; ++block)
                {
                    std::vector<kick::clap::Event> events;
                    auto note = [&](bool on, int key) { x.note(on, key, 1); events.push_back(kick::clap::Event::note(on, key, 1)); };
                    // The fixture sounds the same for one key or several, so the high key plays alone first.
                    if(block == 1) note(true, 72);
                    if(block == 10) { note(true, 30); note(true, 40); note(true, 100); }
                    if(block == 30) { note(false, 30); note(false, 40); note(false, 72); note(false, 100); }
                    a.run(x);
                    g->process(ref, frames, nullptr, events.data(), uint32_t(events.size()));
                    same = same && a.l == rl && a.r == rr;
                    for(float v : a.l) energy += std::abs(v);
                }
                return same && energy > 0;
            };
            check(compare(s), "two zoned layers joining into an effect render exactly as wired by hand");

            // Saved and opened again, it plays the same.
            const auto saved = root / "splits";
            s.savePerformance(saved.string());
            Session t(48000, frames); t.loadPerformance(saved.string());
            check(t.parts().size() == 1 && t.parts()[0].splits.size() == 1 && t.parts()[0].splits[0].lanes[1].level == .8 &&
                  t.parts()[0].splits[0].lanes[0].chain.size() == 1 && t.parts()[0].chain.size() == 2 && isSplit(t.parts()[0].chain[0]), "the split round-trips");
            check(compare(t), "the opened Performance also renders as wired by hand");
        }
        { // A dry lane and a wet lane after an instrument, joining where the part plays.
            Session s(48000, frames);
            const auto synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay);
            s.setParameter(synth, 7, .5);
            const auto lead = s.addPart("Lead");
            s.insertIntoPart(lead, 0, synth);
            const auto split = s.addSplit(lead, 1);
            s.insertIntoPart(lead, 0, fx, {split, 1});
            Lane dry; dry.level = .5; s.setLane(lead, split, 0, dry);
            Lane wet; wet.level = .25; s.setLane(lead, split, 1, wet);
            s.commit();
            Graph g(48000, frames);
            const auto gs = g.add(fixture, "org.kick.test.synth"), gr = g.add(delay);
            g.host(gs).setParameter(7, .5);
            g.connectNotes(graphInput, gs);
            for(uint32_t c = 0; c < 2; ++c)
            {
                g.connectAudio({{gs, 0, 0}, {graphOutput, 0, c}, 1, .5});
                g.connectAudio({{gs, 0, 0}, {gr, g.host(gr).mainAudioPorts(true).front(), c}, 1, 1});
            }
            g.connectAudio({{gr, g.host(gr).mainAudioPorts(false).front(), 0}, {graphOutput, 0, 0}, 2, .25}); g.commit();
            Audio x; std::array<float, frames> rl{}, rr{}; float* ref[2]{rl.data(), rr.data()};
            bool same = true;
            for(int block = 0; block < 200; ++block)
            {
                std::vector<kick::clap::Event> events;
                if(block == 1) { s.note(true, 60, 1); events.push_back(kick::clap::Event::note(true, 60, 1)); }
                if(block == 20) { s.note(false, 60, 0); events.push_back(kick::clap::Event::note(false, 60, 0)); }
                x.run(s); g.process(ref, frames, nullptr, events.data(), uint32_t(events.size()));
                same = same && x.l == rl && x.r == rr;
            }
            check(same, "dry and wet lanes render exactly as wired by hand");
        }
        { // Lane Mix, solo, nesting and edits, heard through the fixture's steady output.
            Session s(48000, frames);
            const auto low = s.add("low", fixture, "org.kick.test.synth"), high = s.add("high", fixture, "org.kick.test.synth");
            s.setParameter(low, 7, .25); s.setParameter(high, 7, .5);
            const auto part = s.addPart("Layers");
            const auto split = s.addSplit(part, 0);
            s.insertIntoPart(part, 0, low, {split, 0}); s.insertIntoPart(part, 0, high, {split, 1}); s.commit();
            Audio x; x.run(s);
            s.note(true, 60, 1); x.run(s); check(near(x.l[0], .75), "both layers: " + std::to_string(x.l[0]));
            Lane solo; solo.solo = true; s.setLane(part, split, 1, solo); s.commit(); x.run(s);
            check(near(x.l[0], .5), "the soloed lane alone: " + std::to_string(x.l[0]));
            Lane muted; muted.mute = true; s.setLane(part, split, 1, muted); s.commit(); x.run(s);
            check(near(x.l[0], .25), "the muted lane silent");
            s.setLane(part, split, 1, {}); s.commit();
            bool refused = false;
            for(const auto& w : s.wires()) if(w.split == split && w.kind == WireKind::Audio) { try { s.setMix(w.id, 1, 0, false, false); } catch(const std::invalid_argument&) { refused = true; } break; }
            check(refused, "a lane's wire takes its Mix from the lane");
            // A split inside a lane: the high layer reaches Main twice, at half level each.
            const auto inner = s.addSplit(part, 1, {split, 1});
            Lane half; half.level = .5; s.setLane(part, inner, 0, half); s.setLane(part, inner, 1, half); s.commit(); x.run(s);
            check(near(x.l[0], .75), "a nested split joins at its lanes' levels: " + std::to_string(x.l[0]));
            s.removeLane(part, inner, 1); s.commit(); x.run(s);
            check(near(x.l[0], .5), "one nested lane left at half level");
            // Removing the outer split keeps its first lane's chain; the rest leave the part.
            s.removeSplit(part, split); s.commit(); x.run(s);
            check(s.parts()[0].chain == std::vector<NodeId>{low} && s.parts()[0].splits.empty(), "the first lane's chain stays, the nested split goes");
            check(near(x.l[0], .25), "only the low layer plays");
            // A bus that splits: a send enters each lane at its Mix; empty lanes go on to where the bus plays.
            const auto room = s.addPart("Room", {PartInput::Kind::Bus});
            const auto busSplit = s.addSplit(room, 0);
            Lane wide; wide.level = .5; s.setLane(room, busSplit, 0, wide);
            Lane narrow; narrow.level = .25; s.setLane(room, busSplit, 1, narrow);
            s.setSend(part, room, 1); s.commit(); x.run(s);
            check(near(x.l[0], .25 + .25 * .75), "a send into a split bus: the low layer dry plus both bus lanes: " + std::to_string(x.l[0]));
            Lane gone; gone.mute = true; s.setLane(room, busSplit, 1, gone); s.commit(); x.run(s);
            check(near(x.l[0], .25 + .25 * .5), "a muted bus lane silences its share of the send");
            s.setPartOutput(part, room); s.commit(); x.run(s);
            check(near(x.l[0], .25 * .5 + .25 * .5), "playing through the split bus too: " + std::to_string(x.l[0]));
            bool joinRefused = false;
            for(const auto& w : s.wires()) if(w.part == part && w.split == busSplit) { try { s.setMix(w.id, 1, 0, false, false); } catch(const std::invalid_argument&) { joinRefused = true; } break; }
            check(joinRefused, "a wire into a bus lane takes its Mix from the lane");
            s.note(false, 60, 0); x.run(s);
        }
        { // Zones in turn: the lane sees the part zone's transposed keys and shaped velocity.
            NoteZone a; a.lowKey = 36; a.highKey = 96; a.transpose = 12; a.curve = 2;
            NoteZone b; b.lowKey = 60; b.lowVelocity = .25;
            const auto z = intersectZones({a}, {b});
            check(z.size() == 1 && z[0].lowKey == 48 && z[0].highKey == 96 && z[0].transpose == 12 && std::abs(z[0].lowVelocity - .5) < 1e-12 && z[0].curve == 2,
                  "keys 48-96 reach 60 and up after +12; velocity .5 reaches .25 after the curve");
            NoteZone c; c.highKey = 40;
            check(intersectZones({a}, {c}).empty(), "no overlap: no zone");
        }
    }

    // Splits that divide the sound with Split and Join nodes (plugins/splitters): left/right,
    // mid/side and bands, against the same graph wired by hand.
    void soundSplits(const char* delay, const char* splitters, const fs::path& root)
    {
        Session s(48000, frames, {}, stereoIn);
        const auto fx = s.add("fx", delay), splitter = s.add("split", splitters, splitPlugin), joiner = s.add("join", splitters, joinPlugin);
        const auto guitar = s.addPart("Guitar", {PartInput::Kind::Audio, 0});
        bool refused = false;
        try { s.addSplit(guitar, 0, {}, Split::Kind::LeftRight, splitter); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a left/right split needs its Join node");
        const auto split = s.addSplit(guitar, 0, {}, Split::Kind::LeftRight, splitter, joiner);
        s.insertIntoPart(guitar, 0, fx, {split, 0}); // the delay on the left side only
        Lane right; right.level = .5; s.setLane(guitar, split, 1, right);
        s.commit();
        auto byHand = [&] {
            auto g = std::make_unique<Graph>(48000, frames, ProcessingMode::Realtime, Lv2BufferLimits{}, stereoIn);
            const auto gd = g->add(delay), gs = g->add(splitters, splitPlugin), gj = g->add(splitters, joinPlugin);
            g->connectAudio({{graphInput, 0, 0}, {gs, 0, 0}, 2, 1});
            g->connectAudio({{gs, 0, 0}, {gd, g->host(gd).mainAudioPorts(true).front(), 0}, 2, 1});
            g->connectAudio({{gd, g->host(gd).mainAudioPorts(false).front(), 0}, {gj, 0, 0}, 2, 1});
            g->connectAudio({{gs, 1, 0}, {gj, 1, 0}, 2, .5});
            g->connectAudio({{gj, 0, 0}, {graphOutput, 0, 0}, 2, 1});
            g->host(gs).setParameter(1, 0); g->host(gj).setParameter(1, 0);
            g->commit();
            return g;
        };
        auto compare = [&](Session& x) {
            auto g = byHand();
            Audio a; std::array<float, frames> rl{}, rr{}; float* ref[2]{rl.data(), rr.data()};
            bool same = true; double energy = 0;
            for(int block = 0; block < 200; ++block)
            {
                for(uint32_t i = 0; i < frames; ++i) { const double t = (block * frames + i) / 48000.0; a.inL[i] = float(.5 * std::sin(2 * M_PI * 440 * t)); a.inR[i] = float(.3 * std::sin(2 * M_PI * 660 * t)); }
                if(block > 100) { a.inL.fill(0); a.inR.fill(0); } // the delay's tail
                a.run(x, 2);
                g->process(a.in, 2, ref, 2, frames, nullptr);
                same = same && a.l == rl && a.r == rr;
                for(float v : a.l) energy += std::abs(v);
            }
            return same && energy > 0;
        };
        check(compare(s), "a left/right split with a delay on one side renders exactly as wired by hand");
        const auto saved = root / "sound-splits";
        s.savePerformance(saved.string());
        Session t(48000, frames, {}, stereoIn); t.loadPerformance(saved.string());
        check(t.parts()[0].splits.size() == 1 && t.parts()[0].splits[0].kind == Split::Kind::LeftRight && t.parts()[0].splits[0].splitter && t.parts()[0].splits[0].joiner, "the split and its nodes round-trip");
        check(compare(t), "the opened Performance also renders as wired by hand");

        refused = false; try { s.addLane(guitar, split); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "left/right keeps two lanes");
        refused = false; try { s.remove(splitter); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a split's node goes with the split, not alone");
        const auto nodes = s.graphState().topology.nodes.size();
        s.removeSplit(guitar, split); s.commit();
        check(s.graphState().topology.nodes.size() == nodes - 2 && s.parts()[0].chain == std::vector<NodeId>{fx}, "removing the split removes its nodes; the left lane's delay stays");
        s.removeFromPart(guitar, fx); s.commit();

        // Mid/side with the side lane muted: mono.
        const auto ms = s.add("ms", splitters, splitPlugin), msJoin = s.add("ms join", splitters, joinPlugin);
        const auto sideSplit = s.addSplit(guitar, 0, {}, Split::Kind::MidSide, ms, msJoin);
        Lane silent; silent.mute = true; s.setLane(guitar, sideSplit, 1, silent); s.commit();
        Audio x; x.inL.fill(.5f); x.inR.fill(.25f); x.run(s, 2); x.run(s, 2);
        check(std::abs(x.l[10] - .375f) < 1e-6 && x.l[10] == x.r[10], "mid/side with the side muted is mono: " + std::to_string(x.l[10]) + " " + std::to_string(x.r[10]));
        s.removeSplit(guitar, sideSplit); s.commit();

        // Bands: the band count follows the lanes; a steady input lives in the lowest band.
        const auto bands = s.add("bands", splitters, splitPlugin);
        const auto bandSplit = s.addSplit(guitar, 0, {}, Split::Kind::Bands, bands);
        s.addLane(guitar, bandSplit); s.commit(); x.run(s, 2);
        check(s.parameter(bands, 1) == 2 && s.parameter(bands, 2) == 3, "bands mode, three bands");
        Lane off; off.mute = true; s.setLane(guitar, bandSplit, 0, off); s.commit();
        for(int i = 0; i < 200; ++i) x.run(s, 2);
        check(std::abs(x.l[10]) < 1e-3, "muting the lowest band removes a steady input: " + std::to_string(x.l[10]));
        s.setLane(guitar, bandSplit, 0, {}); s.setLane(guitar, bandSplit, 1, off); s.setLane(guitar, bandSplit, 2, off); s.commit();
        for(int i = 0; i < 200; ++i) x.run(s, 2);
        check(std::abs(x.l[10] - .5f) < 1e-3 && std::abs(x.r[10] - .25f) < 1e-3, "the lowest band alone keeps it: " + std::to_string(x.l[10]));
        s.removeLane(guitar, bandSplit, 2); s.commit(); x.run(s, 2);
        check(s.parameter(bands, 2) == 2, "two bands again");
        refused = false; try { s.removeLane(guitar, bandSplit, 1); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "bands keep at least two lanes");
    }

    // The Mixer: solo keeps what a soloed part feeds and what feeds it; meters follow the faders.
    void mixer(const char* fixture, const char* delay, const fs::path& root)
    {
        Session s(48000, frames);
        const auto low = s.add("low", fixture, "org.kick.test.synth"), high = s.add("high", fixture, "org.kick.test.synth");
        s.setParameter(low, 7, .25); s.setParameter(high, 7, .5);
        const auto a = s.addPart("A"), b = s.addPart("B"), room = s.addPart("Room", {PartInput::Kind::Bus});
        s.insertIntoPart(a, 0, low); s.insertIntoPart(b, 0, high);
        const auto dry = s.add("dry", delay); s.setParameter(dry, 3, 0); // no echoes: the bus passes its input
        s.insertIntoPart(room, 0, dry);
        s.setSend(a, room, .5); s.commit();
        auto meter = [&](uint32_t part) { for(const auto& m : s.partMeters()) if(m.part == part) return m.peak; return -1.0; };
        Audio x; x.run(s);
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25 + .5 + .125), "A, B and A's send: " + std::to_string(x.l[0]));
        check(std::abs(meter(a) - .25) < 1e-6 && std::abs(meter(b) - .5) < 1e-6 && std::abs(meter(room) - .125) < 1e-6,
              "meters show each part and bus after its fader: " + std::to_string(meter(a)) + " " + std::to_string(meter(b)) + " " + std::to_string(meter(room)));
        // Pan: A hard left takes it out of the right channel; its send keeps its own image.
        s.setPartPan(a, -1); s.commit(); x.run(s);
        check(near(x.l[0], .875) && near(x.r[0], .5 + .125), "A panned left: " + std::to_string(x.l[0]) + " " + std::to_string(x.r[0]));
        check(s.history().undo == "A pan", "the pan is an edit: " + s.history().undo);
        s.setPartPan(a, 0); s.commit();
        bool refusedPan = false; try { s.setPartPan(a, 2); } catch(const std::invalid_argument&) { refusedPan = true; }
        check(refusedPan, "pan beyond -1..1 is refused");
        s.setPartPan(b, .25); s.commit();
        s.setPartSolo(b, true); s.commit(); x.run(s);
        check(near(x.l[0], .5 * .75), "B soloed: A and its send are silent: " + std::to_string(x.l[0]));
        s.setPartSolo(b, false); s.setPartSolo(a, true); s.commit(); x.run(s);
        check(near(x.l[0], .25 + .125), "A soloed: the bus it sends to stays, B is silent: " + std::to_string(x.l[0]));
        s.setPartSolo(a, false); s.setPartSolo(room, true); s.commit(); x.run(s);
        check(near(x.l[0], .25 + .125), "the bus soloed: the part feeding it stays: " + std::to_string(x.l[0]));
        s.setPartSolo(room, false); s.setPartSolo(b, true); s.commit();
        for(int i = 0; i < 700; ++i) x.run(s); // the held peak falls by 1% a tick
        check(meter(a) < 1e-3 && std::abs(meter(b) - .5) < 1e-6, "a silenced part's meter falls to nothing: " + std::to_string(meter(a)));
        s.setPartLevel(b, .5, false); s.commit();
        for(int i = 0; i < 300; ++i) x.run(s);
        check(std::abs(meter(b) - .25) < 1e-6, "the meter follows the fader: " + std::to_string(meter(b)));
        const auto saved = root / "mixer";
        s.note(false, 60, 0); x.run(s); s.savePerformance(saved.string());
        Session t(48000, frames); t.loadPerformance(saved.string());
        check(t.parts()[1].solo && !t.parts()[0].solo && t.parts()[1].pan == .25, "solo and pan round-trip");
    }

    // RFC 0018 step 2: a part's note limit reaches its input wire, also after saving and opening.
    void noteLimits(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys");
        s.insertIntoPart(keys, 0, synth);
        PartInput limited; limited.maxNotes = 4;
        s.setPartInput(keys, limited); s.commit();
        auto zones = wire(s, graphInput, synth, WireKind::Notes).zones;
        check(zones.size() == 1 && zones[0].maxNotes == 4 && zones[0] .lowKey == 0 && zones[0].highKey == 127, "the limit rides on a zone passing every note");
        bool refused = false; PartInput tooMany; tooMany.maxNotes = 129;
        try { s.setPartInput(keys, tooMany); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "more than 128 notes at once is refused");
        const auto saved = root / "limits";
        s.savePerformance(saved.string());
        Session t(48000, frames); t.loadPerformance(saved.string());
        check(t.parts()[0].input.maxNotes == 4 && wire(t, graphInput, t.parts()[0].chain[0], WireKind::Notes).zones.at(0).maxNotes == 4, "the limit round-trips onto the wire");
        s.setPartInput(keys, {}); s.commit();
        check(wire(s, graphInput, synth, WireKind::Notes).zones.empty(), "no limit: no zone needed");
    }

    // RFC 0011 step 2's gate: a two-layer split and a shared reverb (a delay here) built with part
    // edits only render exactly as the same graph wired by hand on the engine's Graph.
    void buses(const char* fixture, const char* delay)
    {
        Session s(48000, frames);
        const auto low = s.add("low", fixture, "org.kick.test.synth"), high = s.add("high", fixture, "org.kick.test.synth"), reverb = s.add("reverb", delay);
        s.setParameter(low, 7, .25); s.setParameter(high, 7, .5);
        NoteZone below; below.highKey = 59;
        NoteZone above; above.lowKey = 60;
        const auto room = s.addPart("Room", {PartInput::Kind::Bus});
        s.insertIntoPart(room, 0, reverb);
        const auto bass = s.addPart("Bass", {PartInput::Kind::Notes, 0, {below}}), lead = s.addPart("Lead", {PartInput::Kind::Notes, 0, {above}});
        s.insertIntoPart(bass, 0, low); s.insertIntoPart(lead, 0, high);
        s.setSend(bass, room, .5); s.setSend(lead, room, .3);
        s.commit();

        Graph g(48000, frames);
        const auto gl = g.add(fixture, "org.kick.test.synth"), gh = g.add(fixture, "org.kick.test.synth"), gr = g.add(delay);
        g.host(gl).setParameter(7, .25); g.host(gh).setParameter(7, .5);
        g.connectNotes(graphInput, gl, {below}); g.connectNotes(graphInput, gh, {above});
        for(const auto [node, send] : {std::pair{gl, .5}, std::pair{gh, .3}})
            for(uint32_t c = 0; c < 2; ++c)
            {
                g.connectAudio({{node, 0, 0}, {graphOutput, 0, c}, 1, 1});
                g.connectAudio({{node, 0, 0}, {gr, g.host(gr).mainAudioPorts(true).front(), c}, 1, send});
            }
        g.connectAudio(gr, graphOutput); g.commit();

        Audio x; std::array<float, frames> rl{}, rr{}; float* ref[2]{rl.data(), rr.data()};
        bool same = true; double wet = 0;
        for(int block = 0; block < 400; ++block)
        {
            std::vector<kick::clap::Event> events;
            auto note = [&](bool on, int key) { s.note(on, key, 1); events.push_back(kick::clap::Event::note(on, key, 1)); };
            if(block == 1) { note(true, 40); note(true, 72); }
            if(block == 30) { note(false, 40); note(false, 72); }
            x.run(s);
            g.process(ref, frames, nullptr, events.data(), uint32_t(events.size()));
            same = same && x.l == rl && x.r == rr;
            if(block > 40) for(float v : x.l) wet += std::abs(v);
        }
        check(same, "the split and shared reverb built from parts render exactly as wired by hand");
        check(wet > 0, "the reverb's tail sounds after the notes end");
        // Through the bus: the lead's dry sound goes only through the reverb.
        s.setPartOutput(lead, room); s.commit();
        size_t toMain = 0; for(const auto& w : s.wires()) toMain += w.from == high && w.to == graphOutput;
        check(toMain == 0, "playing through the bus leaves no wire to Main");
        // Loops are refused, and removing a bus returns its parts to Main.
        const auto other = s.addPart("Other", {PartInput::Kind::Bus});
        s.setPartOutput(other, room);
        bool refused = false; try { s.setSend(room, other, 1); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a bus may not loop into itself");
        s.removePart(room); s.commit();
        size_t sends = 0; for(const auto& p : s.parts()) sends += p.sends.size();
        check(sends == 0 && std::all_of(s.parts().begin(), s.parts().end(), [](const Part& p) { return p.bus == 0; }), "removing the bus ends its sends and outputs");
        toMain = 0; for(const auto& w : s.wires()) toMain += w.from == high && w.to == graphOutput;
        check(toMain == 1, "the lead plays to Main again");
    }

    // Scenes: part faders and scene parameters, switched while a note is held without a gap,
    // and saved with the Performance.
    void scenes(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .25); s.setParameter(b, 7, .5);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b); s.commit();
        Audio x; x.run(s);
        s.setSceneParameters({{b, 7}});
        const auto verse = s.addScene("Verse"); // A .25, B .5
        s.setPartLevel(pa, .5, false); s.setPartLevel(pb, 1, true); s.setParameter(b, 7, .75); s.commit(); x.run(s);
        const auto chorus = s.addScene("Chorus"); // A at half, B muted with gain .75
        check(s.graphState().scenes.size() == 2 && s.graphState().scene == chorus, "two scenes, the last stored selected");
        s.note(true, 60, 1); x.run(s);
        check(near(x.l[0], .125), "chorus: A at half, B muted: " + std::to_string(x.l[0]));
        s.selectScene(verse); x.run(s);
        check(near(x.l[0], .75), "verse: both at full, B's gain back to .5: " + std::to_string(x.l[0]));
        // Switching back and forth while the note is held never goes silent.
        bool gap = false;
        for(int i = 0; i < 6; ++i) { s.selectScene(i % 2 ? verse : chorus); for(int k = 0; k < 3; ++k) { x.run(s); gap = gap || x.l[0] <= 0; } }
        check(!gap, "the held note sounds through every scene change");
        s.selectScene(chorus); x.run(s);
        s.setPartLevel(pb, 1, false); s.commit(); x.run(s);
        check(near(x.l[0], .125 + .75), "B unmuted with the chorus gain: " + std::to_string(x.l[0]));
        // Scene parameters: removing one drops its values; adding one takes the current value in every scene.
        s.setSceneParameters({});
        check(s.graphState().scenes[0].values.empty(), "values dropped with their parameter");
        s.setSceneParameters({{a, 7}});
        check(s.graphState().scenes[0].values.size() == 1 && s.graphState().scenes[1].values[0].value == .25, "a new scene parameter takes its current value in every scene");
        bool refused = false; try { s.setSceneParameters({{a, 7}, {a, 7}}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a parameter listed twice is refused");
        s.setSceneName(verse, "Intro"); check(s.graphState().scenes[0].name == "Intro", "renamed");
        // Saved and loaded with the Performance.
        s.note(false, 60, 0); x.run(s);
        s.savePerformance((root / "scenes").string());
        Session t(48000, frames);
        t.loadPerformance((root / "scenes").string());
        const auto g = t.graphState();
        check(g.scenes.size() == 2 && g.scenes[0].name == "Intro" && g.scenes[1].parts.size() == 2 && g.scenes[1].parts[1].mute && g.scene == chorus && g.sceneParameters.size() == 1,
              "scenes round-trip with the Performance");
        t.selectScene(verse); t.note(true, 60, 1); Audio y; y.run(t); y.run(t);
        check(near(y.l[0], .25 + .75), "the loaded verse plays: " + std::to_string(y.l[0]));
        s.removeScene(chorus); check(s.graphState().scenes.size() == 1 && s.graphState().scene == 0, "removing the selected scene");
    }

    // The control tree: a Super control drives an Assign control, which drives a gain and (reversed)
    // a part's level; the Super also moves another gain around where it was. Values, loops,
    // last-moved-wins, scenes and saving.
    void controls(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .5); s.setParameter(b, 7, .5);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b); s.commit();
        Audio x; x.run(s);
        const auto assign = s.addMacro("Assign 1"), super = s.addMacro("Super");
        MacroTarget gain; gain.node = a; gain.param = 7; gain.start = .2; gain.end = .8;
        MacroTarget level; level.kind = MacroTarget::Kind::PartLevel; level.part = pb; level.start = 1; level.end = 0;
        s.setMacroTargets(assign, {gain, level});
        MacroTarget child; child.kind = MacroTarget::Kind::Macro; child.macro = assign; child.curve = MacroTarget::Curve::Exponential;
        MacroTarget around; around.node = b; around.param = 7; around.relative = true; // base: b's gain now, .5
        s.setMacroTargets(super, {child, around});
        auto render = [&] { s.note(true, 60, 1); x.run(s); return x.l[0]; };
        const double e = (std::exp(2.0) - 1) / (std::exp(4.0) - 1); // the exponential curve at .5
        s.setMacro(super, .5);
        double expected = (.2 + .6 * e) * 1 + .5 * (1 - e);
        check(near(render(), expected), "Super at .5: A's gain " + std::to_string(.2 + .6 * e) + ", B at its gain .5 and level " + std::to_string(1 - e) + ": " + std::to_string(x.l[0]));
        s.setMacro(super, 1);
        check(near(render(), .8 + 1.0 * 0), "Super at 1: A's gain .8, B's level 0: " + std::to_string(x.l[0]));
        s.setMacro(super, 0);
        check(near(render(), .2 + 0.0 * 1), "Super at 0: A's gain .2, B's gain 0 around its base: " + std::to_string(x.l[0]));
        check(s.graphState().macros[0].value == 0, "the Assign control follows the Super");
        // The control moved last sets a shared target: Assign 1 moved by itself.
        s.setMacro(assign, 1);
        check(near(render(), .8), "Assign 1 moved directly: A .8, B's level 0: " + std::to_string(x.l[0]));
        // No loops.
        MacroTarget back; back.kind = MacroTarget::Kind::Macro; back.macro = super;
        bool refused = false; try { s.setMacroTargets(assign, {back}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a control may not drive the one above it");
        refused = false; try { MacroTarget wide = gain; wide.end = 2; s.setMacroTargets(assign, {wide}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a range beyond 0..1 is refused");
        // Scenes store control values.
        s.setMacro(super, .5); const auto verse = s.addScene("Verse");
        s.setMacro(super, 1); s.selectScene(verse);
        check(near(render(), expected) && s.graphState().macros[1].value == .5, "a scene restores the controls: " + std::to_string(x.l[0]));
        // Saved with the Performance.
        s.note(false, 60, 0); x.run(s);
        s.savePerformance((root / "controls").string());
        Session t(48000, frames);
        t.loadPerformance((root / "controls").string());
        const auto m = t.graphState().macros;
        check(m.size() == 2 && m[1].name == "Super" && m[1].targets.size() == 2 && m[1].targets[0].curve == MacroTarget::Curve::Exponential && m[1].targets[1].relative && m[1].targets[1].base == .5 &&
              m[0].targets[1].kind == MacroTarget::Kind::PartLevel && m[0].targets[1].start == 1, "controls round-trip with the Performance");
        t.setMacro(m[1].id, 1); t.note(true, 60, 1); Audio y; y.run(t); y.run(t);
        check(near(y.l[0], .8), "the loaded Super still drives the tree: " + std::to_string(y.l[0]));
        s.removeMacro(assign); check(s.graphState().macros.size() == 1 && s.graphState().macros[0].targets.size() == 1, "removing a control drops it from the controls above it");
    }

    // Parts, wires with owners and Mix settings, and flagged wires survive save and load, and play
    // the same; an earlier format version is refused.
    void persistence(const char* fixture, const char* audio, const char* delay, const fs::path& root)
    {
        Session s(48000, frames, {}, stereoIn);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), a = s.add("a", audio, "org.oscairn.test.audio-ports"), fx = s.add("fx", delay);
        NoteZone upper; upper.lowKey = 48; upper.transpose = -12; upper.curve = 1.5;
        const auto keys = s.addPart("Keys", {PartInput::Kind::Notes, 0, {upper}});
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(keys, 1, fx);
        const auto guitar = s.addPart("Guitar", {PartInput::Kind::Audio, 0});
        s.insertIntoPart(guitar, 0, a);
        const auto room = s.addPart("Room", {PartInput::Kind::Bus});
        s.setSend(keys, room, .4); s.setPartOutput(guitar, room);
        s.connectAudio(synth, a); // synth's mono into a's stereo main: a user wire into a Mix with the guitar's input
        s.setMix(wire(s, synth, a).id, .5, .25, false, false);
        Wire side; side.from = graphInput; side.fromPort = 0; side.to = a; side.toPort = 20; side.channels = {{1, 0}}; s.connectPorts(side);
        s.setParameter(synth, 7, .5); s.commit();
        Audio warm; warm.run(s, 2);
        const auto saved = root / "saved";
        s.savePerformance(saved.string());

        Session t(48000, frames, {}, stereoIn);
        t.loadPerformance(saved.string());
        auto describe = [](const Session& x) {
            std::vector<std::string> out;
            const auto names = x.graphState().names;
            auto name = [&](NodeId n) { return n == graphInput ? std::string("input") : n == graphOutput ? std::string("output") : names.at(n); };
            auto zones = [](const std::vector<NoteZone>& list) { std::string z; for(const auto& n : list) z += std::to_string(n.lowKey) + "-" + std::to_string(n.highKey) + "/" + std::to_string(n.transpose) + "/" + std::to_string(n.curve) + ";"; return z; };
            for(const auto& p : x.parts()) { std::string chain; for(auto n : p.chain) chain += name(n) + ","; out.push_back("part " + std::to_string(p.id) + " " + p.name + " " + chain + " " + std::to_string(int(p.input.kind)) + " " + zones(p.input.zones) + " " + std::to_string(p.output) + " bus " + std::to_string(p.bus) + " sends " + [&]{ std::string t; for(const auto& x : p.sends) t += std::to_string(x.bus) + "@" + std::to_string(x.level) + ","; return t; }()); }
            for(const auto& w : x.wires())
            {
                std::string pairs; for(const auto& [f, g] : w.channels) pairs += std::to_string(f) + ">" + std::to_string(g) + ",";
                out.push_back("wire " + std::to_string(int(w.kind)) + " " + std::to_string(int(w.owner)) + " " + std::to_string(w.part) + " " + name(w.from) + ":" + std::to_string(w.fromPort) + " " + name(w.to) + ":" + std::to_string(w.toPort)
                              + " " + pairs + " " + zones(w.zones) + " send " + std::to_string(w.send) + " " + std::to_string(w.level) + " " + std::to_string(w.pan) + " " + std::to_string(w.mute) + std::to_string(w.solo) + std::to_string(w.connected));
            }
            std::sort(out.begin(), out.end());
            return out;
        };
        check(describe(t) == describe(s), "parts and wires round-trip");
        check(!t.parts().empty() && t.parts()[0].input.zones.size() == 1 && t.parts()[0].input.zones[0].transpose == -12, "the part's zone round-trips");
        Audio x, y; x.inL.fill(.3f); x.inR.fill(.1f); y.inL = x.inL; y.inR = x.inR;
        s.note(true, 60, 1); t.note(true, 60, 1);
        bool same = true; double energy = 0;
        for(int i = 0; i < 20; ++i) { x.run(s, 2); y.run(t, 2); same = same && x.l == y.l && x.r == y.r; for(float v : x.l) energy += std::abs(v); }
        check(same && energy > 0, "the loaded Performance plays the same");
        // Saving it again gives the same manifest.
        const auto again = root / "again";
        t.savePerformance(again.string());
        const auto first = state_files::read(saved / "performance.json", 1 << 22), second = state_files::read(again / "performance.json", 1 << 22);
        check(first == second, "saving the loaded Performance gives the same manifest");

        // A file listing one node in two parts is refused, the session untouched.
        {
            auto text = std::string(first.begin(), first.end());
            auto keyAfter = [&](const std::string& name) {
                const auto at = text.find("\"chain\":[\"", text.find("\"name\":\"" + name + "\""));
                return std::pair{at + 10, text.find('"', at + 10)};
            };
            const auto [keyStart, keyEnd] = keyAfter("Keys");
            const auto key = text.substr(keyStart, keyEnd - keyStart);
            const auto [guitarStart, guitarEnd] = keyAfter("Guitar");
            text.replace(guitarStart, guitarEnd - guitarStart, key);
            const auto twice = root / "twice";
            fs::copy(saved, twice, fs::copy_options::recursive);
            state_files::write(twice / "performance.json", text.data(), text.size());
            const auto before = describe(t);
            bool refusedTwice = false; try { t.loadPerformance(twice.string()); } catch(const std::runtime_error& e) { refusedTwice = std::string(e.what()).find("two parts") != std::string::npos; }
            check(refusedTwice && describe(t) == before, "a node in two parts is refused in a file: " + key);
        }

        // An earlier format version is refused clearly, the session untouched.
        const auto old = root / "version-1";
        fs::copy(saved, old, fs::copy_options::recursive);
        auto text = std::string(first.begin(), first.end());
        text.replace(text.find("\"version\":2"), 11, "\"version\":1");
        state_files::write(old / "performance.json", text.data(), text.size());
        const auto before = describe(t);
        bool refused = false; try { t.loadPerformance(old.string()); } catch(const std::runtime_error& e) { refused = std::string(e.what()).find("unsupported format version") != std::string::npos; }
        check(refused && describe(t) == before, "a version 1 Performance is refused and the session kept");
    }
}

int main(int argc, char** argv)
{
    if(argc < 5) { std::fprintf(stderr, "usage: %s FIXTURE AUDIO_FIXTURE DELAY SPLITTERS\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory temp;
        ownership(argv[1], argv[3]);
        channels(argv[2]);
        replacement(argv[2], argv[3]);
        zones(argv[1]);
        devices(argv[1], temp.path);
        splits(argv[1], argv[3], temp.path);
        soundSplits(argv[3], argv[4], temp.path);
        mixer(argv[1], argv[3], temp.path);
        noteLimits(argv[1], temp.path);
        buses(argv[1], argv[3]);
        scenes(argv[1], temp.path);
        controls(argv[1], temp.path);
        persistence(argv[1], argv[2], argv[3], temp.path);
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: part and wire ownership, Mix, channel rules, replacement, devices and persistence");
    return failures ? 1 : 0;
}

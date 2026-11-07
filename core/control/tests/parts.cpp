// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 1: parts and wire ownership, implicit Mixes, port-level routes with channel
// rules, plugin replacement and Performance persistence.
//   oscairn-parts-test FIXTURE.clap AUDIO_FIXTURE.clap DELAY.clap SPLITTERS.clap TRANSPOSE.clap FOLLOWER.clap VOICES.clap
#include "oscairn/session.h"
#include "oscairn/package.h"
#include <oscairn-clap/harmony.h>
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <regex>
#include <cstring>
#include <thread>
#ifdef __linux__
#include <unistd.h>
#endif
#include <string>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    constexpr uint32_t frames = 64;
    const AudioLayout stereoIn{{{0, "In", 2, 0}}, {{0, "Main", 2, 0}}};

    // A fader moved with live gains ramps across the next block: a check after one reads the block's last sample.
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
        s.note(true, 20, 1); x.run(s); check(near(x.l[frames - 1], .125), "the bass part at half level: " + std::to_string(x.l[frames - 1]));
        s.setPartLevel(bass, .5, true); s.commit(); x.run(s); check(near(x.l[frames - 1], 0), "muted");
        for(const auto& w : s.wires()) if(w.fader && w.part == bass) s.setMix(w.id, 1, 0, false, false);
        s.commit(); x.run(s);
        check(near(x.l[frames - 1], .25) && s.parts()[0].level == 1 && !s.parts()[0].mute, "the output wire's Mix sets the part's fader");
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
        // A controller's port duplicating another's, ignored by name (RFC 0024): its notes are left
        // out, named before or after it appears; the other input still plays.
        s.ignoreMidiInputs({"KeyLab 61 Private"});
        status.midiInputs = {{E::Usb, 5, "Arturia KeyLab 61"}, {E::Usb, 6, "KeyLab 61 Private"}}; s.setHostStatus(status);
        play(true, 62, 6); check(near(x.l[0], 0), "an ignored input's note is not delivered: " + std::to_string(x.l[0]));
        play(false, 62, 6); play(true, 62, 5); check(near(x.l[0], .5), "the other input plays");
        play(false, 62, 5); s.ignoreMidiInputs({}); play(true, 62, 6); check(near(x.l[0], .5), "no longer ignored, it plays");
        play(false, 62, 6);
        status.midiInputs = {{E::Usb, 5, "Arturia KeyLab 61"}}; s.setHostStatus(status);
        // Plugged in again under another number: the selector follows it, applied at once.
        status.midiInputs = {{E::Usb, 7, "Arturia KeyLab 61"}}; s.setHostStatus(status);
        play(true, 62, 7); check(near(x.l[0], .5) && !s.graphState().staged, "the KeyLab as source 7 plays External, no commit needed");
        play(false, 62, 7); play(true, 62, 5); check(near(x.l[0], 0), "its old number no longer does");
        play(false, 62, 5);
        // With edits staged, the new devices wait for their commit.
        s.setPartLevel(ext, .5, false); s.setPartSolo(ext, false); // the level is live; the solo is an edit to commit
        status.midiInputs = {{E::Usb, 8, "Arturia KeyLab 61"}}; s.setHostStatus(status);
        check(s.graphState().staged, "staged edits stay staged");
        s.commit(); play(true, 62, 8); check(near(x.l[frames - 1], .25), "committed together: source 8 at half level");
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

    // RFC 0012 step 1: a part's MIDI to hardware outputs chosen by name: what its instrument is
    // played with (through its zones), with controllers, on a channel of its own; a plugin's notes;
    // note offs when it stops sending; outputs plugged in again; Performances keeping the selector.
    void midiOut(const char* fixture, const char* transpose, const fs::path& root)
    {
        using K = DeviceRule::Kind; using M = DeviceRule::Match; using E = MidiEndpoint::Kind;
        using Out = std::vector<std::array<uint32_t, 5>>; // time, ports, status, data 1, data 2
        Session s(48000, frames);
        Control::SessionStatus status; status.midiOutputs = {{E::Usb, 1, "SMK-37 Elite -Master"}, {E::Usb, 2, "volca keys"}}; s.setHostStatus(status);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth");
        NoteZone upper; upper.lowKey = 60; upper.transpose = 12;
        const auto keys = s.addPart("Keys", {PartInput::Kind::Notes, 0, {upper}});
        s.insertIntoPart(keys, 0, synth);
        s.setPartMidiOut(keys, {{{K::UsbNamed, M::Contains, 0, "volca"}}, 3});
        s.commit();
        Audio x;
        auto run = [&](std::vector<kick::clap::Event> events) {
            s.service(); forbidAllocation = true;
            const bool ok = s.process(nullptr, 0, x.out, 2, frames, events);
            forbidAllocation = false; check(ok, "render");
            Out out;
            for(const auto& m : s.midiOutput()) out.push_back({m.time, uint32_t(m.ports), m.bytes[0], m.size > 1 ? m.bytes[1] : 0u, m.size > 2 ? m.bytes[2] : 0u});
            return out;
        };
        using kick::clap::Event;
        auto shown = [](const Out& out) { std::string t; for(const auto& m : out) t += " " + std::to_string(m[0]) + ":" + std::to_string(m[1]) + ":" + std::to_string(m[2]) + "," + std::to_string(m[3]) + "," + std::to_string(m[4]); return t; };
        auto out = run({Event::midi(0x90, 64, 100, 10), Event::midi(0x90, 40, 100, 12), Event::midi(0xB0, 1, 77, 20), Event::midi(0xE0, 0, 80, 30)});
        check(out == Out{{10, 4, 0x92, 76, 100}, {20, 4, 0xB2, 1, 77}, {30, 4, 0xE2, 0, 80}},
              "the part's notes through its zone, its controllers and pitch bend, at their samples, on channel 3, to the volca only:" + shown(out));
        check(x.l[frames - 1] != 0, "the instrument plays too");
        s.setPartMidiOut(keys, {}); s.commit();
        out = run({});
        check(out == Out{{0, 4, 0x82, 76, 0}}, "sending no more, the held note is released there:" + shown(out));
        out = run({Event::midi(0x80, 64, 0, 3)}); check(out.empty(), "and nothing more is sent");
        s.setPartMidiOut(keys, {{{K::UsbNamed, M::Contains, 0, "juno"}}, 0}); s.commit();
        out = run({Event::midi(0x90, 64, 100, 0), Event::midi(0x80, 64, 0, 1)}); check(out.empty(), "a selector matching no output sends nothing");
        bool refused = false;
        try { s.setPartMidiOut(keys, {{{K::All}}, 17}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "channel 17 is refused");

        // A plugin's notes: Transpose before the synth; what reaches the synth goes out, the part's
        // own notes not again.
        const auto tr = s.add("transpose", transpose);
        s.insertIntoPart(keys, 0, tr); s.setParameter(tr, 1, 7);
        s.setPartMidiOut(keys, {{{K::UsbNamed, M::Equals, 0, "SMK-37 Elite -Master"}}, 0}); s.commit();
        run({});
        out = run({Event::midi(0x90, 64, 100, 5)});
        check(out == Out{{5, 2, 0x90, 83, 100}}, "Transpose's notes, on their own channel:" + shown(out));
        // Plugged in again under another number: the selector follows it, no commit needed.
        status.midiOutputs = {{E::Usb, 5, "SMK-37 Elite -Master"}}; s.setHostStatus(status);
        out = run({Event::midi(0x80, 64, 0, 2)});
        check(out == Out{{2, 1u << 5, 0x80, 83, 0}} && !s.graphState().staged, "the release goes to output 5 now:" + shown(out));
        // Moved to another device while a key is held: the note was played on the SMK-37, so its
        // release goes there; the new device never took the note.
        status.midiOutputs = {{E::Usb, 5, "SMK-37 Elite -Master"}, {E::Usb, 6, "TR-8S"}}; s.setHostStatus(status);
        out = run({Event::midi(0x90, 64, 100, 5)});
        s.setPartMidiOut(keys, {{{K::UsbNamed, M::Equals, 0, "TR-8S"}}, 0}); s.commit();
        out = run({Event::midi(0x80, 64, 0, 2)});
        check(out == Out{{0, 1u << 5, 0x80, 83, 0}}, "moved while held, the release goes to the device it was played on:" + shown(out));
        s.setPartMidiOut(keys, {{{K::UsbNamed, M::Equals, 0, "SMK-37 Elite -Master"}}, 0}); s.commit();

        // Performances keep the selector and channel, resolved again where they open.
        s.setPartMidiOut(keys, {{{K::UsbNamed, M::Contains, 0, "smk"}}, 16}); s.commit();
        const auto saved = root / "midi-out";
        s.savePerformance(saved.string());
        Session t(48000, frames);
        status.midiOutputs = {{E::Usb, 9, "SMK-37 Elite -Master"}}; t.setHostStatus(status);
        t.loadPerformance(saved.string());
        check(t.parts()[0].midiOut == s.parts()[0].midiOut, "MIDI out round-trips");
        t.service(); forbidAllocation = true;
        const auto on = Event::midi(0x90, 64, 100, 0);
        const bool ok = t.process(nullptr, 0, x.out, 2, frames, {&on, 1});
        forbidAllocation = false;
        std::string seen; for(const auto& m : t.midiOutput()) seen += " " + std::to_string(m.ports) + ":" + std::to_string(m.bytes[0]) + "," + std::to_string(m.bytes[1]) + "," + std::to_string(m.bytes[2]);
        // Transpose, newly loaded, first lifts its pedals (CC 64 0 on each channel, all on 16 here).
        const auto sent = t.midiOutput();
        check(ok && !sent.empty() && sent.back().bytes[0] == 0x9F && sent.back().bytes[1] == 83 && std::all_of(sent.begin(), sent.end(), [](const auto& m) { return m.ports == 1u << 9; }),
              "opened elsewhere, it sends to that machine's output, on channel 16:" + seen);
    }

    // RFC 0033: the Performance's key and a part's own, refused when invalid, undone as a step,
    // and saved and opened again with the Performance; a plugin in the part reads its key and the
    // chord its keys hold (the follower fixture saves what the host extension gives it).
    void harmonyKeys(const char* fixture, const char* follower, const fs::path& root)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), other = s.add("other", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys", {PartInput::Kind::Notes});
        const auto bass = s.addPart("Bass", {PartInput::Kind::Notes});
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(bass, 0, other); s.commit();
        check(s.key() == MusicalKey{} && MusicalKey{}.scale == 0xAB5, "a new Performance is in C major");
        const MusicalKey dDorian{2, 1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10}, aMinor{9, 1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 8 | 1 << 10};
        s.setKey(dDorian); s.commit();
        check(s.key() == dDorian && s.history().undo == "Key", "the key is set, an undoable step: " + s.history().undo);
        s.undo(); s.service();
        check(s.key() == MusicalKey{}, "and undone");
        s.redo(); s.service();
        s.setPartKey(bass, aMinor); s.commit();
        check(s.parts()[1].key == aMinor && !s.parts()[0].key, "the bass part has its own key; the keys follow the Performance's");
        for(const auto& bad : {MusicalKey{12, 1}, MusicalKey{0, 0xAB4}, MusicalKey{0, 0x1001}})
        {
            bool refused = false;
            try { s.setKey(bad); } catch(const std::invalid_argument&) { refused = true; }
            check(refused && s.key() == dDorian, "a root past B, a scale without its root or past twelve notes is refused");
        }
        const auto saved = root / "keys";
        s.savePerformance(saved.string());
        Session t(48000, frames);
        t.loadPerformance(saved.string());
        check(t.key() == dDorian && t.parts()[1].key == aMinor && !t.parts()[0].key, "the key and the part's own key round-trip");

        // The follower in a part of its own, in A minor: C E G held on it make C major.
        const auto f = s.add("follower", follower);
        const auto lead = s.addPart("Lead", {PartInput::Kind::Notes});
        s.insertIntoPart(lead, 0, f); s.setPartKey(lead, aMinor); s.commit();
        Audio x;
        const std::vector<kick::clap::Event> chord{kick::clap::Event::note(true, 48, .8, 3), kick::clap::Event::note(true, 52, .8, 3), kick::clap::Event::note(true, 55, .8, 3)};
        s.service(); check(s.process(nullptr, 0, x.out, 2, frames, chord), "render");
        const auto dir = root / "follower";
        s.saveState(f, dir.string());
        const auto bytes = state_files::read(dir / "state.clap", 1024);
        oscairn_harmony_t h{};
        check(bytes.size() == sizeof h, "the follower read a harmony");
        if(bytes.size() == sizeof h) std::memcpy(&h, bytes.data(), sizeof h);
        check(h.key_root == 9 && h.scale == aMinor.scale && h.chord_root == 0 && h.chord_type == OSCAIRN_CHORD_MAJOR,
              "a plugin in the part reads its own key, A minor, and the C major chord held on it");
        s.setPartKey(lead, std::nullopt); s.setPartKey(bass, std::nullopt); s.commit();
        check(!s.parts()[1].key && !s.parts()[2].key, "a part's own key cleared");
        s.service(); check(s.process(nullptr, 0, x.out, 2, frames, {}), "render");
        s.saveState(f, (root / "follower-2").string());
        const auto again = state_files::read(root / "follower-2" / "state.clap", 1024);
        if(again.size() == sizeof h) std::memcpy(&h, again.data(), sizeof h);
        check(again.size() == sizeof h && h.key_root == 2 && h.scale == dDorian.scale, "cleared, the part follows the Performance's D Dorian");
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
        // A value per channel of the part's sound: the fixture synth is mono, the Delay stereo.
        for(const auto& m : s.partMeters())
        {
            if(m.part == b) check(m.channels.size() == 1 && std::abs(m.channels[0] - .5) < 1e-6, "a mono part has one meter: " + std::to_string(m.channels.size()));
            if(m.part == room) check(m.channels.size() == 2 && std::abs(m.channels[0] - .125) < 1e-6 && std::abs(m.channels[1] - .125) < 1e-6, "a stereo bus has two: " + std::to_string(m.channels.size()));
        }
        // A wire's meter: what it carries after its level, per channel its source sends.
        const auto state = s.graphState();
        auto wireMeter = [&](const std::function<bool(const Wire&)>& which) {
            for(const auto& w : state.wires) if(which(w)) for(const auto& m : s.wireMeters()) if(m.wire == w.id) return m;
            return WireMeter{0, -1, {}};
        };
        const auto send = wireMeter([&](const Wire& w) { return w.part == a && w.send == room; });
        check(send.channels.size() == 1 && std::abs(send.peak - .125) < 1e-6, "A's send meters after its level: " + std::to_string(send.peak));
        const auto fader = wireMeter([&](const Wire& w) { return w.part == room && w.fader; });
        check(fader.channels.size() == 2 && std::abs(fader.peak - .125) < 1e-6, "the bus's fader wire meters both channels: " + std::to_string(fader.channels.size()));
        // Pan: A hard left takes it out of the right channel; its send keeps its own image.
        s.setPartPan(a, -1); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .875) && near(x.r[frames - 1], .5 + .125), "A panned left (a live gain, by the block's end): " + std::to_string(x.l[frames - 1]) + " " + std::to_string(x.r[frames - 1]));
        check(s.history().undo == "A pan", "the pan is an edit: " + s.history().undo);
        s.setPartPan(a, 0); s.commit();
        bool refusedPan = false; try { s.setPartPan(a, 2); } catch(const std::invalid_argument&) { refusedPan = true; }
        check(refusedPan, "pan beyond -1..1 is refused");
        s.setPartPan(b, .25); s.commit();
        s.setPartSolo(b, true); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .5 * .75), "B soloed: A and its send are silent: " + std::to_string(x.l[frames - 1]));
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
        PartInput limited; limited.maxNotes = 4; limited.reserve = 2; limited.priority = 3;
        s.setPartInput(keys, limited); s.commit();
        auto zones = wire(s, graphInput, synth, WireKind::Notes).zones;
        check(zones.size() == 1 && zones[0].maxNotes == 4 && zones[0] .lowKey == 0 && zones[0].highKey == 127, "the limit rides on a zone passing every note");
        check(zones[0].part == keys, "counted for the whole part, across its lanes");
        check(zones[0].reserve == 2 && zones[0].priority == 3, "the reserve and priority ride on the zone for the shared budget");
        for(auto bad : {PartInput{.maxNotes = 4, .reserve = 5}, PartInput{.reserve = 129}, PartInput{.priority = 11}})
        {
            bool no = false; try { s.setPartInput(keys, bad); } catch(const std::invalid_argument&) { no = true; }
            check(no, "a reserve past the limit or 128, or a priority past 10, is refused");
        }
        bool refused = false; PartInput tooMany; tooMany.maxNotes = 129;
        try { s.setPartInput(keys, tooMany); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "more than 128 notes at once is refused");
        const auto saved = root / "limits";
        s.savePerformance(saved.string());
        Session t(48000, frames); t.loadPerformance(saved.string());
        check(t.parts()[0].input.maxNotes == 4 && wire(t, graphInput, t.parts()[0].chain[0], WireKind::Notes).zones.at(0).maxNotes == 4, "the limit round-trips onto the wire");
        check(t.parts()[0].input.reserve == 2 && t.parts()[0].input.priority == 3, "the reserve and priority round-trip");
        check(wire(t, graphInput, t.parts()[0].chain[0], WireKind::Notes).zones.at(0).part == t.parts()[0].id, "and the part's one count");
        s.setPartInput(keys, {}); s.commit();
        check(wire(s, graphInput, synth, WireKind::Notes).zones.empty(), "no limit: no zone needed");
    }

    // RFC 0018 step 3: with a CPU budget set, the shared budget steals notes across parts while
    // they play, and each part's meter counts them. Two parts on instruments whose voices cost
    // 30 us a block each, under a budget of 266 us: twelve notes are more than it holds. B gives
    // way first, but reserves four of its six.
    void voiceBudget(const char* voices, const fs::path& root)
    {
        ::setenv("OSCAIRN_DATA_HOME", (root / "voice-budget-home").c_str(), 1);
        fs::create_directories(root / "voice-budget-home");
        ::setenv("OSCAIRN_FIXTURE_VOICE_US", "60", 1);
        Session s(48000, frames);
        s.setBudget({.2, 0});
        const auto a = s.addPart("A"), b = s.addPart("B");
        s.insertIntoPart(a, 0, s.add("a", voices, "org.oscairn.test.voices"));
        s.insertIntoPart(b, 0, s.add("b", voices, "org.oscairn.test.voices"));
        NoteZone first; first.channels = 1;
        NoteZone second; second.channels = 2;
        s.setPartInput(a, {.zones = {first}, .priority = 5}); s.setPartInput(b, {.zones = {second}, .reserve = 4}); s.commit();
        Audio x;
        for(int i = 0; i < 40; ++i) x.run(s); // measured at rest
        for(int channel : {1, 0}) for(int i = 0; i < 6; ++i) { s.note(true, 60 + i, 1, channel); x.run(s); x.run(s); }
        for(int i = 0; i < 40; ++i) x.run(s);
        uint64_t stolenA = 0, stolenB = 0;
        for(const auto& m : s.partMeters()) { if(m.part == a) stolenA = m.stolen + m.hardChokes; if(m.part == b) stolenB = m.stolen + m.hardChokes; }
        check(stolenA > 0, "past B's reserve, A gives notes up too: " + std::to_string(stolenA));
        check(stolenB <= 2, "B, playing six, keeps its reserve of four: " + std::to_string(stolenB));
        s.setBudget({0, 0}); x.run(s);
        ::unsetenv("OSCAIRN_FIXTURE_VOICE_US");
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

    // A bus with no box still applies its fader: sends and parts into it play at its level,
    // muted with it, panned with it.
    // A part whose wire is baked into the plan (merged ways through different faders: C into an
    // empty bus that plays to Main and sends to another empty bus on Main) comes back when unmuted.
    void unmuteBaked(const char* fixture)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth");
        s.setParameter(synth, 7, .25);
        const auto c = s.addPart("C"), e1 = s.addPart("E1", {PartInput::Kind::Bus}), e2 = s.addPart("E2", {PartInput::Kind::Bus});
        s.insertIntoPart(c, 0, synth);
        s.setPartOutput(c, e1); s.setSend(e1, e2, 1); s.commit();
        Audio x;
        s.note(true, 60, 1); for(int i = 0; i < 5; ++i) x.run(s);
        const float before = x.l[frames - 1];
        check(before > .4f, "C plays through E1 and E2: " + std::to_string(before));
        s.setPartLevel(c, 1, true); s.commit(); for(int i = 0; i < 5; ++i) x.run(s);
        check(x.l[frames - 1] == 0, "muted, C is silent: " + std::to_string(x.l[frames - 1]));
        s.setPartLevel(c, 1, false); s.commit(); for(int i = 0; i < 5; ++i) x.run(s);
        check(near(x.l[frames - 1], before), "unmuted, C plays again: " + std::to_string(x.l[frames - 1]) + " for " + std::to_string(before));
    }

    void emptyBus(const char* fixture)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth");
        s.setParameter(synth, 7, .25);
        const auto keys = s.addPart("Keys"), room = s.addPart("Room", {PartInput::Kind::Bus});
        s.insertIntoPart(keys, 0, synth);
        s.setSend(keys, room, 1); s.setPartLevel(room, 1, true); s.commit();
        Audio x;
        s.note(true, 60, 1); x.run(s); check(near(x.l[0], .25), "a send into a muted empty bus adds nothing: " + std::to_string(x.l[0]));
        s.setPartLevel(room, .5, false); s.commit(); x.run(s); check(near(x.l[0], .375), "at level 0.5 it adds half: " + std::to_string(x.l[0]));
        s.setPartPan(room, -1); s.commit(); x.run(s); check(x.r[0] < x.l[0], "panned left with it");
        s.setPartPan(room, 0); s.setSend(keys, room, {}); s.setPartOutput(keys, room); s.commit(); x.run(s);
        check(near(x.l[0], .125), "a part playing into it plays at its level: " + std::to_string(x.l[0]));
        s.setMaster(.5, 0); s.commit(); x.run(s); check(near(x.l[frames - 1], .0625), "the master level applies once after it: " + std::to_string(x.l[frames - 1]));
        s.setPartLevel(room, .5, true); s.commit(); x.run(s); check(near(x.l[frames - 1], 0), "and is muted with it");
        // Its own sends carry too: an empty bus sends what plays into it, after its level and mute.
        s.setMaster(1, 0); s.setPartLevel(room, 1, false); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .25), "the keys play through the bus at unity: " + std::to_string(x.l[frames - 1]));
        const auto hall = s.addPart("Hall", {PartInput::Kind::Bus});
        s.setSend(room, hall, 1); s.commit(); x.run(s);
        check(near(x.l[0], .5), "the bus's own send plays beside it: " + std::to_string(x.l[0]));
        s.setPartLevel(hall, .5, false); s.commit(); x.run(s);
        check(near(x.l[0], .375), "the send's bus plays at its own level: " + std::to_string(x.l[0]));
        s.setPartLevel(hall, 1, false); s.setPartLevel(room, 1, true); s.commit(); x.run(s);
        check(near(x.l[0], 0), "muting the bus mutes its send too");
        // A Routing level on a wire through a bus with a fader sets what was asked: the wire also
        // carries the bus's gain, so the part takes the asked level divided by it.
        s.setSend(room, hall, {}); s.setPartLevel(room, .5, false); s.setPartLevel(keys, .5, false); s.commit();
        auto keysWire = [&](uint32_t send) { for(const auto& w : s.wires()) if(w.part == keys && w.from == synth && w.to == graphOutput && w.send == send) return w.id; throw std::runtime_error("no keys wire"); };
        s.setMix(keysWire(0), .5, 0, false, false); s.commit(); x.run(s);
        double dryLevel = -1; for(const auto& w : s.wires()) if(w.part == keys && !w.send && w.to == graphOutput) dryLevel = w.level;
        check(near(dryLevel, .5) && near(s.parts()[0].level, 1) && near(x.l[frames - 1], .125), "asked .5 past the bus's half: the wire shows .5: " + std::to_string(dryLevel));
        s.setMix(keysWire(0), .5, 0, false, false); s.commit(); x.run(s);
        double again = -1; for(const auto& w : s.wires()) if(w.part == keys && !w.send && w.to == graphOutput) again = w.level;
        check(near(again, .5), "asking again lands in the same place: " + std::to_string(again));
        // A pan past the edge: the part's .8 and the bus's .5 sum past 1, so the wire shows 1. Asked
        // .9, the part takes .4 at once, the wire's share of the bus kept.
        s.setPartPan(keys, .8); s.setPartPan(room, .5); s.commit();
        s.setMix(keysWire(0), .5, .9, false, false); s.commit();
        double wirePan = -1; for(const auto& w : s.wires()) if(w.part == keys && !w.send && w.to == graphOutput) wirePan = w.pan;
        check(near(wirePan, .9) && near(s.parts()[0].pan, .4), "a pan asked past a clamped sum lands at once: wire " + std::to_string(wirePan) + " part " + std::to_string(s.parts()[0].pan));
        s.setPartPan(keys, 0); s.setPartPan(room, 0); s.commit();
        // A send wire's level: the send keeps its own share of the asked level, past its bus's fader.
        s.setPartLevel(hall, .5, false); s.setSend(keys, hall, 1); s.commit();
        s.setMix(keysWire(hall), .4, 0, false, false); s.commit(); x.run(s);
        double sendLevel = -1; double sendWire = -1;
        for(const auto& p : s.parts()) if(p.id == keys) for(const auto& sd : p.sends) if(sd.bus == hall) sendLevel = sd.level;
        for(const auto& w : s.wires()) if(w.part == keys && w.send == hall) sendWire = w.level;
        check(near(sendLevel, .8) && near(sendWire, .4) && near(x.l[0], .225), "the send wire shows the asked .4 past the bus's half: send " + std::to_string(sendLevel) + " wire " + std::to_string(sendWire) + " l " + std::to_string(x.l[0]));
        // Sound passing through an empty bus meters on the bus.
        s.setSend(keys, hall, {}); s.setPartLevel(keys, 1, false); s.setPartLevel(room, 1, false); s.setPartLevel(hall, 1, false); s.commit();
        auto meter = [&](uint32_t part) { for(const auto& m : s.partMeters()) if(m.part == part) return m.peak; return -1.0; };
        for(int i = 0; i < 3; ++i) x.run(s);
        check(std::abs(meter(keys) - .25) < 1e-6 && std::abs(meter(room) - .25) < 1e-6,
              "the bus meters what plays through it: keys " + std::to_string(meter(keys)) + " room " + std::to_string(meter(room)));
        s.setPartLevel(room, 1, true); s.commit();
        for(int i = 0; i < 800; ++i) x.run(s);
        check(meter(room) < 1e-3, "a muted empty bus meters nothing: " + std::to_string(meter(room)));
        // Each empty bus meters its own point on the way: Room what passes its fader, and Hall,
        // fed by Room's send at .1, a tenth of that, and nothing once Hall is muted.
        auto settle = [&] { for(int i = 0; i < 800; ++i) x.run(s); };
        auto near6 = [](double a, double b) { return std::abs(a - b) < 1e-6; };
        s.setPartLevel(room, 1, false); s.setSend(room, hall, .1); s.commit(); settle();
        check(near6(meter(room), .25) && near6(meter(hall), .025), "a send at .1 meters a tenth on its bus: room " + std::to_string(meter(room)) + " hall " + std::to_string(meter(hall)));
        s.setPartLevel(hall, 1, true); s.commit(); settle();
        check(near6(meter(room), .25) && meter(hall) < 1e-3, "a muted bus past it meters nothing, the bus before it its own: room " + std::to_string(meter(room)) + " hall " + std::to_string(meter(hall)));
        // Nested: Keys into Room into Hall, at .5. Room shows its own level, Hall its half; muting
        // Hall leaves Room's.
        s.setSend(room, hall, {}); s.setPartOutput(room, hall); s.setPartLevel(hall, .5, false); s.commit(); settle();
        check(near6(meter(room), .25) && near6(meter(hall), .125) && near6(meter(keys), .25), "nested empty buses each meter their own point: room " + std::to_string(meter(room)) + " hall " + std::to_string(meter(hall)));
        s.setPartLevel(hall, .5, true); s.commit(); settle();
        check(near6(meter(room), .25) && meter(hall) < 1e-3, "muting the outer bus leaves the inner one's meter: room " + std::to_string(meter(room)) + " hall " + std::to_string(meter(hall)));
        // A bus whose chain takes no sound (a synth on it) passes it on through its fader, and meters it.
        s.setPartOutput(room, 0); s.setPartLevel(hall, 1, false);
        const auto pad = s.add("pad", fixture, "org.kick.test.synth");
        s.insertIntoPart(room, 0, pad); s.commit(); settle();
        check(near6(meter(room), .25), "a bus whose box takes no sound meters what passes it: " + std::to_string(meter(room)));
        s.removeFromPart(room, pad); s.commit();

        // The Routing view sends a row's own pan and mute with each level drag: the bus's share in
        // them stays the bus's.
        auto dry = [&]() -> const Wire& { for(const auto& w : s.wires()) if(w.part == keys && w.from == synth && w.to == graphOutput && !w.send) return w; throw std::runtime_error("no keys wire"); };
        auto drag = [&](double level) { const auto w = dry(); s.setMix(w.id, level, w.pan, w.mute, w.solo); s.commit(); };
        s.setPartPan(room, .5); s.commit();
        drag(.8); drag(.6);
        check(near(dry().pan, .5) && near(s.parts()[0].pan, 0), "level drags keep the bus's pan out of the part: wire " + std::to_string(dry().pan) + " part " + std::to_string(s.parts()[0].pan));
        { const auto w = dry(); s.setMix(w.id, w.level, 0, w.mute, w.solo); s.commit(); }
        check(near(dry().pan, 0) && near(s.parts()[0].pan, -.5), "a pan asked of the wire lands on it: wire " + std::to_string(dry().pan) + " part " + std::to_string(s.parts()[0].pan));
        s.setPartPan(keys, 0); s.setPartPan(room, 0); s.setPartLevel(room, 1, true); s.commit();
        drag(.8);
        check(!s.parts()[0].mute && dry().mute, "a level drag through a muted bus leaves the part unmuted");
        s.setPartLevel(room, 1, false); s.commit();
        check(!dry().mute, "and it plays again with the bus");
        { const auto w = dry(); s.setMix(w.id, w.level, w.pan, true, w.solo); s.commit(); }
        check(s.parts()[0].mute, "muting the wire itself mutes the part");
        { const auto w = dry(); s.setMix(w.id, w.level, w.pan, false, w.solo); s.commit(); }
        check(!s.parts()[0].mute, "and unmuting it unmutes the part");

        // Keys into Room into Main, and Room's send into Hall into Main: two ways from the synth to
        // Main, one wire, so turning it off, scenes and undo, which know a wire by its ends, see one.
        s.setPartLevel(keys, 1, false); s.setPartLevel(room, 1, false); s.setPartLevel(hall, 1, false); s.setSend(room, hall, .1); s.commit(); x.run(s);
        size_t ways = 0; for(const auto& w : s.wires()) ways += w.part == keys && w.from == synth && w.to == graphOutput && w.fader;
        check(ways == 1 && near(dry().level, 1.1) && near(x.l[0], .275), "two ways to one place are one wire at their sum: " + std::to_string(ways) + " " + std::to_string(x.l[0]));
        s.setWireEnabled(dry().id, false); s.commit(); x.run(s);
        check(near(x.l[0], 0), "turning it off silences both ways: " + std::to_string(x.l[0]));
        s.setPartPan(keys, 0); s.setPartLevel(keys, 1, false); s.commit(); x.run(s);
        check(near(x.l[0], 0) && !dry().enabled, "and it stays off through a rebuild: " + std::to_string(x.l[0]));
        s.setWireEnabled(dry().id, true); s.commit(); x.run(s);
        check(near(x.l[0], .275), "and plays again: " + std::to_string(x.l[0]));
        s.setPartLevel(room, 1, true); s.commit(); x.run(s);
        check(near(x.l[0], 0) && dry().mute, "a muted bus mutes both ways");
        s.setPartLevel(room, .5, false); s.commit(); settle();
        check(near6(meter(keys), .25) && near6(meter(room), .125), "the part meters at its own fader, not the sum of its ways or the bus's level: keys " + std::to_string(meter(keys)) + " room " + std::to_string(meter(room)));
    }

    // Zones on a wire inside a part's chain (note limits after a note plugin) survive a rebuild
    // of the part's wires and a save and reload; a wire the rebuild drops takes its zones along.
    void innerZones(const char* fixture, const char* transpose, const char* delay, const fs::path& root)
    {
        Session s(48000, frames);
        const auto tr = s.add("transpose", transpose), synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay);
        const auto keys = s.addPart("Keys");
        s.insertIntoPart(keys, 0, tr); s.insertIntoPart(keys, 1, synth);
        NoteZone high; high.lowKey = 60;
        s.setZones(wire(s, tr, synth, WireKind::Notes).id, {high});
        auto kept = [&](const Session& t) { const auto& z = wire(t, t.parts()[0].chain[0], t.parts()[0].chain[1], WireKind::Notes).zones; return z.size() == 1 && z[0] == high; };
        s.setPartPan(keys, .5); check(kept(s), "a part setting keeps them");
        s.insertIntoPart(keys, 2, fx); s.commit(); check(kept(s), "inserting a node keeps them");
        const auto saved = root / "inner-zones";
        s.savePerformance(saved.string());
        Session t(48000, frames); t.loadPerformance(saved.string());
        check(kept(t), "saved and opened again");
        t.setPartPan(t.parts()[0].id, 0); check(kept(t), "and kept by an edit there");
        s.movePartNode(keys, 1, 0); s.movePartNode(keys, 0, 1);
        check(wire(s, tr, synth, WireKind::Notes).zones.empty(), "a wire the chain lost comes back without them");
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
        const auto verse = s.addScene("Verse"); // A .25, B .5
        s.setPartLevel(pa, .5, false); s.setPartLevel(pb, 1, true); s.setParameter(b, 7, .75); s.commit(); x.run(s);
        const auto chorus = s.addScene("Chorus"); // A at half, B muted with gain .75
        check(s.graphState().scenes.size() == 2 && s.graphState().scene == chorus, "two scenes, the last stored selected");
        check(s.graphState().scenes[0].scope == Scene::Everything, "a scene applies everything by default");
        s.note(true, 60, 1); x.run(s);
        check(near(x.l[0], .125), "chorus: A at half, B muted: " + std::to_string(x.l[0]));
        s.selectScene(verse); x.run(s);
        check(near(x.l[frames - 1], .75), "verse: both at full, B's gain back to .5 (every automatable parameter): " + std::to_string(x.l[frames - 1]));
        // Switching back and forth while the note is held never goes silent.
        bool gap = false;
        for(int i = 0; i < 6; ++i) { s.selectScene(i % 2 ? verse : chorus); for(int k = 0; k < 3; ++k) { x.run(s); gap = gap || x.l[0] <= 0; } }
        check(!gap, "the held note sounds through every scene change");
        s.selectScene(chorus); x.run(s);
        s.setPartLevel(pb, 1, false); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .125 + .75), "B unmuted with the chorus gain: " + std::to_string(x.l[frames - 1]));
        // A parameter set aside: no scene touches it; taken back, it gets its current value in every scene.
        s.setSceneUntouched({{b, 7}});
        s.selectScene(verse); x.run(s);
        check(near(s.parameter(b, 7), .75) && near(x.l[frames - 1], .25 + .75), "B's gain left alone by the verse: " + std::to_string(x.l[frames - 1]));
        s.setSceneUntouched({});
        for(const auto& sc : s.graphState().scenes)
            check(std::any_of(sc.values.begin(), sc.values.end(), [&](const Scene::Value& v) { return v.at == SceneParameter{b, 7} && near(v.value, .75); }), "taken back at its current value in " + sc.name);
        bool refused = false; try { s.setSceneUntouched({{a, 7}, {a, 7}}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a parameter listed twice is refused");
        // Scope: a chorus that only mutes leaves levels and parameters as they are.
        s.selectScene(verse); s.setParameter(b, 7, .5); s.storeScene(verse);
        s.setSceneScope(chorus, Scene::Mutes);
        s.selectScene(chorus); x.run(s);
        check(near(x.l[frames - 1], .25), "the chorus mutes B, A stays at full: " + std::to_string(x.l[frames - 1]));
        refused = false; try { s.setSceneScope(chorus, 64); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "an unknown kind is refused");
        s.setSceneScope(chorus, Scene::Everything);
        // A full pending store does not stop a scene: its values wait their turn.
        s.setParameter(a, 7, .1); x.run(s); x.run(s); // a's gain away from the chorus's .25
        size_t queued = 0;
        while(s.parameterGesture(b, 7, queued % 2 == 0)) ++queued;
        check(queued == Session::commandCapacity, "the pending store filled: " + std::to_string(queued));
        s.selectScene(chorus);
        for(int i = 0; i < 400; ++i) x.run(s);
        check(near(s.parameter(a, 7), .25) && near(x.l[0], .125), "the chorus's values applied past a full store: " + std::to_string(s.parameter(a, 7)) + " " + std::to_string(x.l[0]));
        s.setSceneName(verse, "Intro"); check(s.graphState().scenes[0].name == "Intro", "renamed");
        s.note(false, 60, 0); x.run(s);
        s.removeScene(chorus); check(s.graphState().scenes.size() == 1 && s.graphState().scene == 0, "removing the selected scene");
    }

    // Scenes switch plugins off and on, ringing out; mappings are off in the scenes they are off
    // in; all of it is saved.
    void sceneSwitching(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .25); s.setParameter(b, 7, .5);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b);
        s.setNodeEnabled(b, false); s.commit();
        const auto verse = s.addScene("Verse"); // a on, b off
        s.setNodeEnabled(a, false); s.setNodeEnabled(b, true); s.commit();
        const auto chorus = s.addScene("Chorus"); // a off, b on
        const auto knob = s.addMacro("Knob");
        MacroTarget t; t.node = a; t.param = 7; t.offIn = {chorus};
        s.setMacroTargets(knob, {t});
        Audio x;
        s.selectScene(verse); s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "the verse plays a: " + std::to_string(x.l[0]));
        s.selectScene(chorus); x.run(s);
        check(near(x.l[0], .25) && s.graphState().ringing.count(a), "into the chorus, a's held note plays on: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s); s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .5), "a new note in the chorus plays b alone: " + std::to_string(x.l[0]));
        s.setMacro(knob, 1);
        check(near(s.parameter(a, 7), .25), "the knob's mapping is off in the chorus");
        s.selectScene(verse); s.setMacro(knob, 1);
        check(near(s.parameter(a, 7), 1), "and on in the verse");
        s.note(false, 62, 0); x.run(s);
        s.setSceneScope(chorus, Scene::Enabled | Scene::Mappings);
        s.savePerformance((root / "scenes").string());
        Session u(48000, frames);
        u.loadPerformance((root / "scenes").string());
        const auto g = u.graphState();
        const auto& c = g.scenes[1];
        check(g.scenes.size() == 2 && g.scenes[0].off.size() == 1 && c.off.size() == 1 && c.scope == (Scene::Enabled | Scene::Mappings) &&
              g.macros.size() == 1 && g.macros[0].targets[0].offIn == std::vector<uint32_t>{chorus} && g.scene == verse,
              "scenes round-trip with the Performance: what is off, the scope and the mappings' scenes");
        u.selectScene(chorus); u.note(true, 60, 1); Audio y; y.run(u); y.run(u);
        check(near(y.l[0], .5), "the loaded chorus plays b alone: " + std::to_string(y.l[0]));
    }

    // Seamless Performance changes (RFC 0021): the one before rings out beside the new one.
    void performanceChange(const char* fixture, const fs::path& root)
    {
        auto save = [&](const char* name, double gain) {
            Session p(48000, frames);
            const auto synth = p.add("synth", fixture, "org.kick.test.synth"); p.setParameter(synth, 7, gain);
            const auto part = p.addPart("Part"); p.insertIntoPart(part, 0, synth); p.commit();
            Audio x; x.run(p);
            p.savePerformance((root / name).string());
        };
        save("change-a", .25); save("change-b", .5);
        Session s(48000, frames);
        s.loadPerformance((root / "change-a").string());
        Audio x;
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "A plays: " + std::to_string(x.l[0]));
        s.loadPerformance((root / "change-b").string()); x.run(s); x.run(s);
        check(near(x.l[0], .25) && s.leaving() == 1 && s.graphState().topology.nodes.size() == 1, "A's held note plays on, A hidden: " + std::to_string(x.l[0]));
        const auto part = s.graphState().parts.front().id;
        s.setPartLevel(part, .5, false); s.commit(); x.run(s);
        check(near(x.l[0], .25), "an edit to B leaves A ringing: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s);
        check(near(x.l[0], 0), "A's note released: " + std::to_string(x.l[0]));
        s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "a new note plays B alone, at half: " + std::to_string(x.l[0]));
        s.note(false, 62, 0);
        for(int i = 0; i < int(48000 * 3.2 / frames) && s.leaving(); ++i) x.run(s);
        check(s.leaving() == 0, "A goes after three seconds of silence");
    }

    // A voice that never ends (a plugin deaf to releases, a release lost on the way) keeps the last
    // Performance ringing: with a key held through the change (or a lost release) it goes 2 minutes
    // after the change; with every key up, 30 s after; at once on a panic. Uses performanceChange's
    // Performances.
    void stuckRingOut(const fs::path& root)
    {
        ::setenv("KICK_FIXTURE_STUCK", "1", 1);
        Session s(48000, frames);
        Audio x;
        auto change = [&](int key) {
            s.loadPerformance((root / "change-a").string()); s.note(true, key, 1); x.run(s); x.run(s);
            s.loadPerformance((root / "change-b").string()); x.run(s); x.run(s);
        };
        auto seconds = [&](double t) { for(int i = 0; i < int(48000 * t / frames); ++i) x.run(s); };
        change(60);
        check(s.leaving() == 1 && near(x.l[0], .25), "A's stuck voice rings on: " + std::to_string(x.l[0]));
        seconds(60);
        check(s.leaving() == 1 && near(x.l[0], .25), "a key held through the change still sounds 60 s after it: " + std::to_string(x.l[0]));
        seconds(59);
        check(s.leaving() == 1, "and 119 s after");
        for(int i = 0; i < int(48000 * 1.2 / frames) && s.leaving(); ++i) x.run(s);
        x.run(s);
        check(s.leaving() == 0 && near(x.l[0], 0), "gone 2 minutes after the change: " + std::to_string(x.l[0]));
        change(61); s.note(false, 61, 0); x.run(s);
        check(s.leaving() >= 1 && near(x.l[0], .25), "a released key's stuck voice rings on: " + std::to_string(x.l[0]));
        seconds(29);
        check(s.leaving() >= 1 && near(x.l[0], .25), "still ringing 29 s after the change: " + std::to_string(x.l[0]));
        for(int i = 0; i < int(48000 * 1.2 / frames) && s.leaving(); ++i) x.run(s);
        x.run(s);
        check(s.leaving() == 0 && near(x.l[0], 0), "with no key held, gone 30 s after the change: " + std::to_string(x.l[0]));
        change(62);
        check(s.leaving() == 2 && near(x.l[0], .25), "a third stuck voice rings on, beside the B it left: " + std::to_string(x.l[0]));
        s.allNotesOff(); x.run(s); x.run(s);
        check(s.leaving() == 0 && near(x.l[0], 0), "a panic ends it at once: " + std::to_string(x.l[0]));
        ::unsetenv("KICK_FIXTURE_STUCK");
    }

    // Panic on a hardware button (RFC 0011's panic assignment), over the engine's MIDI path with
    // no app on the way: a switch the Performance assigns ends a held note; a button whose
    // profile group plays Panic ends a stuck voice ringing out after a change, on every press.
    void hardwarePanic(const char* fixture, const fs::path& root)
    {
        auto controller = [](Session& s) {
            ControlGroup switches{ControlGroup::Role::Assign, "buttons", 1}; switches.switches = true;
            switches.midi.device = "pad"; switches.midi.numbers = {20};
            ControlGroup panic{ControlGroup::Role::Assign, "buttons", 1}; panic.panic = true;
            panic.midi.device = "pad"; panic.midi.numbers = {21}; panic.midi.latch = true;
            s.setControllers({switches, panic}, {{"Pad", "pad", 1}});
            Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, 5, "Pad"}}; s.setHostStatus(status);
            s.service();
        };
        auto press = [](Session& s, uint8_t cc, uint8_t value) { const uint8_t bytes[3]{0xB0, cc, value}; s.controllerMidi(5, bytes, 3); };
        {
            Session s(48000, frames);
            const auto synth = s.add("synth", fixture, "org.kick.test.synth"); s.setParameter(synth, 7, .5);
            const auto part = s.addPart("Part"); s.insertIntoPart(part, 0, synth); s.commit();
            bool refused = false; try { s.assign("knob:1", {Assignment::Kind::Panic}); } catch(const std::invalid_argument&) { refused = true; }
            check(refused, "Panic is a button's or footswitch's, not a knob's");
            s.assign("button:1", {Assignment::Kind::Panic});
            controller(s);
            Audio x;
            s.note(true, 60, 1); x.run(s); x.run(s);
            check(near(x.l[0], .5), "a held note plays: " + std::to_string(x.l[0]));
            press(s, 20, 127); x.run(s); x.run(s);
            check(near(x.l[0], 0), "a switch assigned Panic ends it: " + std::to_string(x.l[0]));
            press(s, 20, 0); s.note(true, 62, 1); x.run(s); x.run(s);
            check(near(x.l[0], .5), "its release does nothing, and the next note plays: " + std::to_string(x.l[0]));
            press(s, 20, 127); x.run(s); x.run(s);
            check(near(x.l[0], 0), "and the next press ends that too: " + std::to_string(x.l[0]));
            press(s, 20, 0);
        }
        ::setenv("KICK_FIXTURE_STUCK", "1", 1);
        Session s(48000, frames);
        Audio x;
        controller(s);
        for(const uint8_t value : {127, 0}) // a latching button: each message is a press
        {
            s.loadPerformance((root / "change-a").string()); s.note(true, 60, 1); x.run(s); x.run(s);
            s.loadPerformance((root / "change-b").string()); x.run(s); x.run(s);
            check(s.leaving() >= 1 && near(x.l[0], .25), "a stuck voice rings out after the change: " + std::to_string(x.l[0]));
            press(s, 21, value); x.run(s); x.run(s);
            check(s.leaving() == 0 && near(x.l[0], 0), "a button whose group plays Panic ends it at once: " + std::to_string(x.l[0]));
        }
        ::unsetenv("KICK_FIXTURE_STUCK");
    }

    // A plugin that is not here (RFC 0006): the Performance opens, the plugin silent in its place
    // saying why, its state kept byte for byte through a save, and back once it is installed.
    void unavailable(const char* fixture, const fs::path& root)
    {
        {
            Session p(48000, frames);
            const auto synth = p.add("synth", fixture, "org.kick.test.synth"); p.setParameter(synth, 7, .5);
            const auto part = p.addPart("Part"); p.insertIntoPart(part, 0, synth); p.commit();
            const auto bright = p.addMacro("Bright", 0); p.setMacroTargets(bright, {MacroTarget{.kind = MacroTarget::Kind::Parameter, .node = synth, .param = 7}}); p.commit();
            p.savePerformance((root / "gone").string());
        }
        auto read = [](const fs::path& f) { std::ifstream in(f, std::ios::binary); return std::string((std::istreambuf_iterator<char>(in)), {}); };
        const auto doc = root / "gone" / "performance.json";
        const auto original = read(doc);
        auto moved = original; const auto at = moved.find(std::string(fixture)); check(at != std::string::npos, "the saved path hint");
        moved.replace(at, std::string(fixture).size(), "/nowhere/missing.clap");
        std::ofstream(doc, std::ios::binary | std::ios::trunc) << moved;
        Session s(48000, frames);
        s.loadPerformance((root / "gone").string());
        const auto status = s.sessionStatus();
        check(status.failures.size() == 1 && status.failures[0].reason.find("not here") != std::string::npos, "the plugin not here is reported: " + (status.failures.empty() ? std::string("none") : status.failures[0].reason));
        check(s.graphState().parts.size() == 1 && s.graphState().parts[0].chain.size() == 1 && s.graphState().macros.size() == 1 && s.graphState().macros[0].targets.size() == 1, "its part, chain and control kept");
        Audio x; s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], 0), "silent in its place");
        s.note(false, 60, 0); x.run(s);
        s.savePerformance((root / "gone-again").string());
        const auto node = [&](const fs::path& folder) { return read(folder / "nodes" / "node-1" / "snapshot"); };
        check(node(root / "gone-again") == node(root / "gone") && read(root / "gone-again" / "performance.json").find("org.kick.test.synth") != std::string::npos,
              "saved again with its identity and its state as they were");
        // Installed again (its path back): it plays, with the saved state.
        auto back = read(root / "gone-again" / "performance.json");
        const auto hint = back.find("/nowhere/missing.clap"); check(hint != std::string::npos, "the missing path saved");
        back.replace(hint, std::string("/nowhere/missing.clap").size(), fixture);
        std::ofstream(root / "gone-again" / "performance.json", std::ios::binary | std::ios::trunc) << back;
        Session t(48000, frames);
        t.loadPerformance((root / "gone-again").string());
        check(t.sessionStatus().failures.empty(), "back, nothing missing");
        Audio y; t.note(true, 60, 1); y.run(t); y.run(t);
        check(near(y.l[0], .5), "and playing with its saved state: " + std::to_string(y.l[0]));
        t.note(false, 60, 0); y.run(t);
    }

    // All notes off (RFC 0030): a held note stops at the next block, and the next note plays.
    void allNotesOff(const char* fixture)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"); s.setParameter(synth, 7, .5);
        const auto part = s.addPart("Part"); s.insertIntoPart(part, 0, synth); s.commit();
        Audio x;
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .5), "a held note plays: " + std::to_string(x.l[0]));
        s.allNotesOff(); x.run(s); x.run(s);
        check(near(x.l[0], 0), "all notes off: silent: " + std::to_string(x.l[0]));
        s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .5), "and the next note plays: " + std::to_string(x.l[0]));
        s.note(false, 62, 0); x.run(s);
    }

    // Audition (RFC 0021): a Sound in a part's place and a Performance in this one's, each put
    // back as it was (the same plugins: a note held through it still sounds) or kept.
    void audition(const char* fixture, const fs::path& root)
    {
        const char* previousHome = std::getenv("OSCAIRN_DATA_HOME"); const std::string savedHome = previousHome ? previousHome : "";
        ::setenv("OSCAIRN_DATA_HOME", (root / "audition-home").c_str(), 1);
        std::string b, half;
        {
            Session p(48000, frames);
            const auto synth = p.add("synth", fixture, "org.kick.test.synth"); p.setParameter(synth, 7, .5);
            const auto part = p.addPart("Part"); p.insertIntoPart(part, 0, synth); p.commit();
            half = p.savePartAsSound(part, "Half").id;
            b = p.savePerformanceToLibrary("B", true);
        }
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"); s.setParameter(synth, 7, .25);
        const auto part = s.addPart("A"); s.insertIntoPart(part, 0, synth); s.commit();
        s.setPerformanceInfo({"", "A", "", {}, ""});
        const auto undoBefore = s.history().undo;
        Audio x;
        auto ring = [&] { for(int i = 0; i < int(48000 * 3.2 / frames) && s.leaving(); ++i) x.run(s); };
        // Played first, as on a device: a parameter change reaches a plugin with its next block.
        s.note(true, 61, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "A plays before any audition: " + std::to_string(x.l[0]));
        s.note(false, 61, 0); x.run(s); x.run(s);
        bool refused = false; try { s.auditionStart("missing"); } catch(const std::exception&) { refused = true; }
        check(refused && s.audition().id.empty(), "an item not in the library is refused");

        // A Sound in the part's place, then stopped: the part's own chain is back.
        s.auditionStart(half, part);
        check(s.audition().id == half && s.audition().kind == "sound" && s.audition().part == part, "auditioning the Sound");
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .5), "the part plays the Sound: " + std::to_string(x.l[0]));
        s.auditionStop();
        x.run(s); x.run(s);
        check(near(x.l[0], .5) && s.leaving() == 1, "its held note rings on: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); ring();
        const auto back = s.graphState().parts.front();
        check(back.chain == std::vector<NodeId>{synth} && s.audition().id.empty() && s.history().undo == undoBefore, "the part's own plugin is back, the history as it was");
        Control::LibraryQuery recent; recent.recent = true;
        auto used = [&] { const auto items = s.libraryQuery(recent).items; return std::any_of(items.begin(), items.end(), [&](const auto& i) { return i.id == half; }); };
        check(!used(), "a Sound auditioned and put back is not marked used");
        s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "and plays: " + std::to_string(x.l[0]));
        s.note(false, 62, 0); x.run(s);
        // Kept: the Sound's load is one step to undo.
        s.auditionStart(half, part); s.auditionStop(true);
        check(s.graphState().parts.front().chain != std::vector<NodeId>{synth} && s.audition().id.empty(), "kept, the Sound is the part's");
        check(used(), "kept, it is");
        check(s.undo() && s.graphState().parts.front().chain == std::vector<NodeId>{synth}, "and undone, the part's own plugin is back");
        s.commit();

        // A Performance in this one's place: A's held note plays on through it and back.
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "A plays: " + std::to_string(x.l[0]));
        s.auditionStart(b);
        check(s.audition().kind == "performance" && s.performanceInfo().name == "B" && s.graphState().topology.nodes.size() == 1, "B open, A set aside");
        s.note(true, 64, 1); x.run(s); x.run(s);
        check(near(x.l[0], .75), "A's held note and B's new one: " + std::to_string(x.l[0]));
        s.note(false, 64, 0); x.run(s);
        s.auditionStop(); x.run(s); x.run(s);
        check(near(x.l[0], .25) && s.performanceInfo().name == "A" && s.graphState().parts.front().chain == std::vector<NodeId>{synth},
              "stopped, A is back with its note still held, the same plugin: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s);
        check(near(x.l[0], 0), "A's note released: " + std::to_string(x.l[0]));
        ring();
        check(s.leaving() == 0 && s.graphState().topology.nodes.size() == 1, "B gone once rung out");
        s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "A plays new notes: " + std::to_string(x.l[0]));
        s.note(false, 62, 0); x.run(s);
        // A opened from its file and silent, so its synth could be taken over (RFC 0021): an
        // audition takes over nothing it sets aside, and stopping puts back A's own sound.
        s.savePerformance((root / "audition-a").string()); s.loadPerformance((root / "audition-a").string()); ring();
        for(int i = 0; i < int(48000 * 3.2 / frames); ++i) x.run(s);
        s.auditionStart(b); s.auditionStop(); x.run(s); x.run(s);
        s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "a silent A auditioned over plays its own sound again: " + std::to_string(x.l[0]));
        s.note(false, 62, 0); x.run(s); ring();
        // Kept: B is the Performance, A rings out and goes.
        s.auditionStart(b); s.auditionStop(true); ring();
        check(s.performanceInfo().name == "B" && s.leaving() == 0 && s.graphState().topology.nodes.size() == 1 && s.audition().id.empty(), "kept, B is open and A gone");
        if(previousHome) ::setenv("OSCAIRN_DATA_HOME", savedHome.c_str(), 1); else ::unsetenv("OSCAIRN_DATA_HOME"); // back to the test's own folder, never the real one
    }

    // Scene transitions: a glide over time along a curve, a part muted on the way fading to
    // silence, a hand on a fader stopping the glide, a scene waiting for the next beat, and a
    // control morphing between two scenes; all saved.
    void sceneTransitions(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .5); s.setParameter(b, 7, .5);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b); s.setPartLevel(pb, 1, true); s.commit();
        const auto verse = s.addScene("Verse");                                 // A at 1, B muted
        s.setPartLevel(pa, .2, false); s.commit(); const auto low = s.addScene("Low");        // A at .2
        s.setPartLevel(pa, 1, true); s.commit(); const auto quiet = s.addScene("Quiet");     // A muted
        s.setPartLevel(pa, 1, false); s.setPartLevel(pb, 1, false); s.setNodeEnabled(a, false); s.commit();
        const auto other = s.addScene("Other");                                 // A off, B on
        Audio x;
        auto run = [&](double seconds) { for(int i = 0; i < int(48000 * seconds / frames); ++i) x.run(s); };
        const auto stepTime = double(frames) / 48000;
        s.selectScene(verse); s.note(true, 60, 1); run(.01);
        check(near(x.l[0], .5), "the verse: A alone at full: " + std::to_string(x.l[0]));

        // A glide of 100 ms: halfway, halfway there; then exactly there.
        s.setSceneTransition(low, 100, Scene::Curve::Linear, Scene::Launch::Now);
        s.selectScene(low); run(.05);
        // Each step reads the glide before its block, so it lags by up to a block or two of 64 frames.
        check(std::abs(x.l[0] - .5 * .6) < .5 * .8 * 2.5 * stepTime / .1, "halfway through the glide: " + std::to_string(x.l[0]));
        run(.06);
        check(near(x.l[0], .1) && s.graphState().parts[0].level == .2, "the glide ends exactly on the scene: " + std::to_string(x.l[0]));

        // Equal power: halfway from silence to full is at half the power, not half the level.
        s.setSceneTransition(verse, 100, Scene::Curve::EqualPower, Scene::Launch::Now);
        s.selectScene(quiet); run(.01); // at once: no glide on Quiet
        check(near(x.l[0], 0), "Quiet mutes A at once: " + std::to_string(x.l[0]));
        s.selectScene(verse); run(.05);
        check(std::abs(x.l[0] - .5 * std::sqrt(.5)) < .02 && !s.graphState().parts[0].mute, "unmuted, A fades in by power: " + std::to_string(x.l[0]));
        run(.06);
        check(near(x.l[0], .5), "and arrives: " + std::to_string(x.l[0]));

        // A part muted on the way fades to silence, then is muted at its scene's level.
        s.setSceneTransition(quiet, 100, Scene::Curve::Linear, Scene::Launch::Now);
        s.selectScene(quiet); run(.05);
        check(std::abs(x.l[0] - .25) < .02, "fading out: " + std::to_string(x.l[0]));
        run(.06);
        check(near(x.l[0], 0) && s.graphState().parts[0].mute && s.graphState().parts[0].level == 1, "muted at the end, its level kept: " + std::to_string(x.l[0]));

        // A hand on a fader stops the glide where it is.
        s.setSceneTransition(verse, 1000, Scene::Curve::Linear, Scene::Launch::Now);
        s.selectScene(verse); run(.05);
        s.setPartLevel(pa, .7, false); s.commit(); run(.2);
        check(near(x.l[0], .35), "the fader moved during the glide stays where it was put: " + std::to_string(x.l[0]));

        // On the beat: while playing, the scene waits for the next beat (120 BPM: every half second).
        s.setSceneTransition(low, 0, Scene::Curve::Linear, Scene::Launch::Beat);
        s.setPlaying(true); run(.1);
        const double before = s.transportState().beat;
        s.selectScene(low); run(.02);
        check(near(x.l[0], .35) && s.transportState().beat < std::floor(before) + 1, "it waits for the beat: " + std::to_string(x.l[0]));
        run(.5);
        check(near(x.l[0], .1) && s.graphState().scene == low, "and comes on it: " + std::to_string(x.l[0]));
        s.setPlaying(false);

        // A control morphing between two scenes: levels in between, A's plugin switching halfway.
        const auto morph = s.addMacro("Morph");
        MacroTarget t; t.kind = MacroTarget::Kind::Morph; t.sceneFrom = verse; t.sceneTo = other;
        s.setMacroTargets(morph, {t});
        s.setMacro(morph, 0); s.commit(); run(.01);
        check(near(x.l[0], .5) && !s.graphState().disabled.count(a), "at 0: the verse");
        s.setMacro(morph, .25); s.commit(); run(.01);
        check(near(x.l[0], .5 + .5 * .25) && !s.graphState().disabled.count(a), "a quarter of the way: B fading in, A still on: " + std::to_string(x.l[0]));
        s.setMacro(morph, .75); run(.01);
        check(s.graphState().disabled.count(a) && s.graphState().macros[0].value == .75, "past halfway: A off, the control where it was put");
        s.setMacro(morph, 1); run(.01);
        bool same = false; try { MacroTarget bad = t; bad.sceneTo = verse; s.setMacroTargets(morph, {bad}); } catch(const std::invalid_argument&) { same = true; }
        check(same, "a morph between a scene and itself is refused");
        s.note(false, 60, 0); x.run(s);

        // Saved and loaded.
        s.savePerformance((root / "transitions").string());
        Session u(48000, frames);
        u.loadPerformance((root / "transitions").string());
        const auto g = u.graphState();
        const auto saved = std::find_if(g.scenes.begin(), g.scenes.end(), [&](const Scene& x) { return x.id == low; });
        check(saved != g.scenes.end() && saved->launch == Scene::Launch::Beat && g.scenes[0].glide == 1000 && g.scenes[0].curve == Scene::Curve::Linear &&
              g.macros.size() == 1 && g.macros[0].targets[0].kind == MacroTarget::Kind::Morph && g.macros[0].targets[0].sceneTo == other,
              "transitions and the morph round-trip");
        s.removeScene(other);
        check(s.graphState().macros[0].targets.empty(), "removing a scene removes morphs to it");
    }

    // Resource budgets (RFC 0025): plugins measured while they play and kept for the next time;
    // a plugin or a Performance over the budget refused, saying what does not fit; a change that
    // fits alone but not beside what plays now becomes a cut.
    void budgets(const char* fixture, const fs::path& root)
    {
        ::setenv("OSCAIRN_DATA_HOME", (root / "budget-home").c_str(), 1);
        fs::create_directories(root / "budget-home");
        {
            Session s(48000, frames);
            check(s.estimate().fits && s.estimate().budget.cpu == 0, "with no budget, everything fits");
            s.setBudget({.9, 0});
            const auto a = s.add("a", fixture, "org.kick.test.synth");
            const auto part = s.addPart("A"); s.insertIntoPart(part, 0, a); s.commit();
            auto e = s.estimate();
            check(e.plugins.size() == 1 && !e.plugins[0].measured && near(e.cpu, .02), "a plugin not yet measured counts at the default: " + std::to_string(e.cpu));
            Audio x; s.note(true, 60, 1);
            for(int i = 0; i < 64; ++i) x.run(s);
            e = s.estimate();
            check(e.plugins[0].measured && e.plugins[0].plugin == "org.kick.test.synth", "measured while it plays");
            s.note(false, 60, 0); x.run(s);
            s.savePerformance((root / "budget-one").string());
            const auto lead = s.savePartAsSound(part, "Lead").id;
            // Over the budget: refused, unloaded again, saying what.
            s.setBudget({1e-9, 0});
            std::string why;
            try { s.add("b", fixture, "org.kick.test.synth"); } catch(const std::invalid_argument& e) { why = e.what(); }
            check(why.find("of the audio time") != std::string::npos && s.graphState().topology.nodes.size() == 1, "a plugin over the budget refused: " + why);
            why.clear();
            try { s.loadSound(part, lead); } catch(const std::invalid_argument& e) { why = e.what(); }
            check(why.find("of the audio time") != std::string::npos && s.graphState().topology.nodes.size() == 1 && s.sessionStatus().failures.empty(),
                  "a Sound over the budget refused, not opened with a stand-in: " + why);
            why.clear();
            try { s.loadPerformance((root / "budget-one").string()); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("of the audio time") != std::string::npos, "a Performance over the budget refused: " + why);
        }
        check(fs::exists(root / "budget-home" / "measurements.json"), "measurements kept in the data folder");
        {
            // A held note splits the cost: something per voice, the most voices seen.
            std::ifstream in(root / "budget-home" / "measurements.json"); const std::string text((std::istreambuf_iterator<char>(in)), {});
            check(text.find("\"voice\":") != std::string::npos && text.find("\"voices\":1") != std::string::npos, "its cost per voice measured while a note sounded: " + text);
        }
        {
            // At rest plus per voice, for as many notes as the part may play; no limit, the busiest block.
            const auto home = root / "budget-voices";
            fs::create_directories(home);
            ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1);
            std::string key;
            {
                std::ifstream in(root / "budget-home" / "measurements.json"); const std::string text((std::istreambuf_iterator<char>(in)), {});
                const auto at = text.find("{\"plugins\":{\"") + 13; key = text.substr(at, text.find('"', at) - at);
            }
            std::ofstream(home / "measurements.json") << "{\"plugins\":{\"" << key << "\":{\"cpu\":0.3,\"rest\":0.05,\"voice\":0.01,\"voices\":3,\"memory\":1000}}}";
            Session v(48000, frames);
            v.setBudget({.9, 0});
            const auto a = v.add("a", fixture, "org.kick.test.synth");
            PartInput limited; limited.maxNotes = 4;
            const auto part = v.addPart("A", limited); v.insertIntoPart(part, 0, a); v.commit();
            check(near(v.estimate().cpu, .05 + .01 * 4), "at rest plus four voices: " + std::to_string(v.estimate().cpu));
            v.setPartInput(part, {}); v.commit();
            check(near(v.estimate().cpu, .3), "with no limit, the busiest block seen: " + std::to_string(v.estimate().cpu));
            v.setPartInput(part, limited); v.commit();
            v.savePerformance((root / "budget-limited").string());
            check(near(v.estimate((root / "budget-limited").string()).cpu, .09), "and from a saved Performance's parts: " + std::to_string(v.estimate((root / "budget-limited").string()).cpu));
            ::setenv("OSCAIRN_DATA_HOME", (root / "budget-home").c_str(), 1);
        }
        {
            Session t(48000, frames);
            t.setBudget({.9, 0});
            check(t.estimate((root / "budget-one").string()).plugins.at(0).measured, "and known to the next session before it loads");
        }
        // Seamless only when both fit: a fresh device, every plugin at the default (.02).
        ::setenv("OSCAIRN_DATA_HOME", (root / "budget-fresh").c_str(), 1);
        fs::create_directories(root / "budget-fresh");
        Session u(48000, frames);
        u.setBudget({.03, 0});
        u.loadPerformance((root / "budget-one").string());
        const auto e = u.estimate((root / "budget-one").string());
        check(e.fits && !e.seamless, "it fits alone, not beside itself");
        check(!u.preparePerformance((root / "budget-one").string()), "nor is it prepared ahead beside it");
        u.loadPerformance((root / "budget-one").string());
        check(u.leaving() == 0, "so the change is a cut");
        u.setBudget({.05, 0});
        check(u.preparePerformance((root / "budget-one").string()), "with room for both, it is prepared ahead");
        u.loadPerformance((root / "budget-one").string());
        check(u.leaving() == 1, "and rings out");
    }

    // A build changed without a new version (0.1.0 stays 0.1.0) is measured again: measurements
    // are kept by the digest of the build's files, not its version alone.
    void rebuilt(const char* fixture, const fs::path& root)
    {
        const auto home = root / "rebuilt-home", plugin = root / "rebuilt.clap";
        fs::create_directories(home);
        ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1);
        fs::copy_file(fixture, plugin);
        auto measured = [&] { // whether it was measured before it played, and after
            Session s(48000, frames);
            s.setBudget({.9, 0});
            const auto a = s.add("a", plugin.string(), "org.kick.test.synth");
            const auto part = s.addPart("A"); s.insertIntoPart(part, 0, a); s.commit();
            const bool before = s.estimate().plugins.at(0).measured;
            Audio x; s.note(true, 60, 1);
            for(int i = 0; i < 64; ++i) x.run(s);
            s.note(false, 60, 0); x.run(s);
            return std::pair{before, s.estimate().plugins.at(0).measured};
        };
        check(measured() == std::pair{false, true}, "a new build is measured while it plays");
        check(measured().first, "and known to the next session");
        // Rebuilt in place, its version unchanged: a new file, as a linker writes one.
        { const auto next = root / "rebuilt.next"; fs::copy_file(fixture, next); std::ofstream(next, std::ios::app | std::ios::binary) << "rebuilt"; fs::rename(next, plugin); }
        check(!measured().first, "rebuilt with the same version, it is measured anew");
        ::setenv("OSCAIRN_DATA_HOME", (root / "budget-home").c_str(), 1);
    }

    // Declared memory (RFC 0025, oscairn.memory): the fixture's memory effect declares 1 MiB shared
    // and its gain times 16 MiB per context (9 MiB at its default .5). Measurements are kept per
    // declaration, so a plugin's modes do not share one figure; admission counts the larger of the
    // declaration and the measurement; a plugin or a saved state declaring more than the budget
    // leaves is refused before it activates; effect contexts count their own bytes.
    void declaredMemory(const char* fixture, const char* delay, const fs::path& root)
    {
        constexpr uint64_t MiB = 1ull << 20;
        const auto home = root / "memory-home";
        fs::create_directories(home);
        ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1);
        // Two modes measured: the default (9 MiB declared) at 10 MiB, a big one (17 MiB declared) at 100.
        const auto build = "org.kick.test.memory@1 " + bundleDigest(fixture) + "#"; // measurements' key (Session::pluginKey)
        std::ofstream(home / "measurements.json") << "{\"plugins\":{"
            "\"" << build << 9 * MiB << "\":{\"cpu\":0.01,\"rest\":0.01,\"voice\":0,\"voices\":0,\"memory\":" << 10 * MiB << "},"
            "\"" << build << 17 * MiB << "\":{\"cpu\":0.01,\"rest\":0.01,\"voice\":0,\"voices\":0,\"memory\":" << 100 * MiB << "}}}";
        {
            Session s(48000, frames);
            s.setBudget({.9, 20 * MiB});
            const auto a = s.add("a", fixture, "org.kick.test.memory");
            auto e = s.estimate();
            check(e.plugins.size() == 1 && e.plugins[0].measured && e.memory == 10 * MiB,
                  "the default mode counts at its own measurement, not the big mode's: " + std::to_string(e.memory >> 20) + " MiB");
            // A second at 9 MiB declared fits beside it (10 left); a third has 1 MiB left and is refused before it activates.
            s.setBudget({.9, 21 * MiB});
            const auto b = s.add("b", fixture, "org.kick.test.memory");
            std::string why;
            try { s.add("c", fixture, "org.kick.test.memory"); } catch(const std::exception& x) { why = x.what(); }
            check(why.find("9 MiB of memory where 1 MiB is left") != std::string::npos && s.graphState().topology.nodes.size() == 2,
                  "a plugin declaring more than the budget leaves is refused before it activates: " + why);
            // Saved louder: its state declares 13 MiB.
            s.remove(b); s.commit();
            s.setParameter(a, 7, .75);
            Audio x; for(int i = 0; i < 4; ++i) x.run(s);
            s.savePerformance((root / "memory-loud").string());
        }
        {
            // Not open, it counts at the most its declarations were measured at.
            Session t(48000, frames);
            t.setBudget({.9, 200 * MiB});
            check(t.estimate((root / "memory-loud").string()).memory == 100 * MiB, "a Performance not open counts the build at its largest measurement");
        }
        {
            // Only the default mode measured: the saved Performance's estimate fits a 12 MiB budget, but
            // its state declares 13 MiB, refused when the state loads, before activating with it.
            const auto other = root / "memory-default";
            fs::create_directories(other);
            ::setenv("OSCAIRN_DATA_HOME", other.c_str(), 1);
            std::ofstream(other / "measurements.json") << "{\"plugins\":{\"" << build << 9 * MiB
                << "\":{\"cpu\":0.01,\"rest\":0.01,\"voice\":0,\"voices\":0,\"memory\":" << 9 * MiB << "}}}";
            Session u(48000, frames);
            u.setBudget({.9, 12 * MiB});
            check(u.estimate((root / "memory-loud").string()).fits, "its estimate, from the default mode's measurement, fits");
            std::string why;
            try { u.loadPerformance((root / "memory-loud").string()); } catch(const std::exception& x) { why = x.what(); }
            check(why.find("13 MiB of memory where 12 MiB is left") != std::string::npos && u.graphState().topology.nodes.empty(),
                  "a saved state declaring more than the budget is refused before it activates: " + why);
            u.setBudget({.9, 13 * MiB});
            u.loadPerformance((root / "memory-loud").string());
            const auto e = u.estimate();
            check(e.memory == 13 * MiB && !e.plugins[0].measured, "with room it opens, counting at its declaration until measured in this mode: " + std::to_string(e.memory));
        }
        {
            // Effect contexts without oscairn.memory: the engine counts its shared and context bytes,
            // each further context its context bytes, not a whole instance's default.
            const auto fresh = root / "memory-fresh";
            fs::create_directories(fresh);
            ::setenv("OSCAIRN_DATA_HOME", fresh.c_str(), 1);
            Session v(48000, frames);
            v.setBudget({.9, 512 * MiB});
            const auto d = v.add("d", delay, "org.oscairn.delay");
            const auto d2 = v.addEffectContext("d2", d);
            const auto r = *v.effectResources(d2);
            const auto e = v.estimate();
            const auto cost = [&](const char* name) { for(const auto& c : e.plugins) if(c.name == name) return c.memory; return uint64_t(0); };
            check(cost("d") == r.shared_bytes + r.context_bytes && cost("d2") == r.context_bytes && e.memory == r.shared_bytes + 2 * r.context_bytes,
                  "effect contexts counted from their resources: " + std::to_string(cost("d")) + ", " + std::to_string(cost("d2")));
        }
        ::setenv("OSCAIRN_DATA_HOME", (root / "budget-home").c_str(), 1);
    }

    // Memory classes (RFC 0025), simulated with the fixture's memory effect (1 MiB shared and its
    // gain times 16 MiB, declared and allocated): reserved (the system, running and always-available
    // apps), committed (the open Performance, what rings out, the prepared next entry) and
    // opportunistic (a next entry prepared to cut). The main test of the RFC, in small: memory
    // filled with opportunistic data, every app opened up to the floor, a committed Performance
    // loaded; nothing refused that fits, nothing committed let go, a launch past the floor refused.
    void memoryClasses(const char* fixture, const fs::path& root)
    {
        constexpr uint64_t MiB = 1ull << 20;
        // Performances of the memory effect: A two at .5 (18 MiB), B one at .5 (9), C four at .9 (61.6).
        auto save = [&](const std::string& name, int count, double gain) {
            Session s(48000, frames);
            std::vector<NodeId> nodes;
            for(int i = 0; i < count; ++i) { nodes.push_back(s.add(name + std::to_string(i), fixture, "org.kick.test.memory")); s.setParameter(nodes.back(), 7, gain); }
            s.commit();
            Audio x; for(int i = 0; i < 4; ++i) x.run(s);
            s.savePerformance((root / ("classes-" + name)).string());
            return (root / ("classes-" + name)).string();
        };
        const auto a = save("a", 2, .5), b = save("b", 1, .5), c = save("c", 4, .9);
        const auto home = root / "classes-home";
        fs::create_directories(home);
        ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1);
        // Both modes measured at what they declare, so a Performance not open (which counts each
        // build at its largest measurement) is estimated, not taken at the 32 MiB default.
        const uint64_t half = MiB + uint64_t(.5 * (16 * MiB)), most = MiB + uint64_t(.9 * (16 * MiB));
        const auto build = "org.kick.test.memory@1 " + bundleDigest(fixture) + "#";
        std::ofstream(home / "measurements.json") << "{\"plugins\":{"
            "\"" << build << half << "\":{\"cpu\":0.001,\"rest\":0.001,\"voice\":0,\"voices\":0,\"memory\":" << half << "},"
            "\"" << build << most << "\":{\"cpu\":0.001,\"rest\":0.001,\"voice\":0,\"voices\":0,\"memory\":" << most << "}}}";
        Session s(48000, frames);
        // A 110 MiB device: 20 for the system, a floor of 40, a margin of 5; the Performance app (6) and the Library (4) always available.
        s.setBudget({.9, 0, 110 * MiB, 20 * MiB, 40 * MiB, 5 * MiB});
        s.setAlwaysAvailable({{"oscairn.performance", 6 * MiB}, {"oscairn.library", 4 * MiB}});
        auto m = s.memoryState();
        check(m.reserved == 30 * MiB && m.share == 75 * MiB, "reserved: the system and the always-available apps, running or not; the committed share what is left less the margin");
        s.loadPerformance(a);
        check(s.memoryState().committed == 18 * MiB, "the open Performance committed, at its plugins' declarations: " + std::to_string(s.memoryState().committed));
        // Fill: C fits alone (61.6 of 75) but not beside A (79.6), so it is prepared to cut, as opportunistic data.
        check(!s.preparePerformance(c), "C will cut");
        m = s.memoryState();
        check(m.nextPrepared && !m.nextCommitted && m.opportunistic > 61 * MiB && m.opportunistic < 62 * MiB, "and is prepared anyway, opportunistic: " + std::to_string(m.opportunistic));
        // Every app, as the shell would ask before starting each.
        auto launch = [&](const std::string& app, uint64_t reserve) { return s.launchApp({app, reserve * MiB}); };
        check(launch("oscairn.performance", 0).admitted, "an always-available app always opens");
        m = s.memoryState();
        check(!m.nextPrepared && m.committed == 18 * MiB, "its share, filled with opportunistic data, is let go, the committed untouched");
        check(launch("oscairn.library", 0).admitted && launch("oscairn.setlist", 6).admitted && launch("oscairn.groovebox", 10).admitted && launch("sound-editor", 9).admitted &&
              launch("sequencer", 8).admitted, "every app that fits beside the floor opens");
        const auto big = launch("third-party", 12);
        check(!big.admitted && !big.offerNext && big.reason == "needs 12 MiB, 2 free for apps; close oscairn.groovebox (10)",
              "a launch past the floor is refused, saying what closing would free: " + big.reason);
        m = s.memoryState();
        check(m.share == 42 * MiB && m.running.size() == 6, "the committed share shrank to what the apps leave: " + std::to_string(m.share >> 20));
        // A committed next entry fits beside A within that share, and is kept through a refused launch.
        check(s.preparePerformance(b), "B is prepared ahead, committed");
        check(!launch("third-party", 12).admitted && s.memoryState().nextCommitted, "a refused launch lets nothing committed go");
        // C no longer fits the share while those apps run: refused, naming what closing would free.
        std::string why;
        try { s.loadPerformance(c); } catch(const std::exception& e) { why = e.what(); }
        check(why.find("closing oscairn.groovebox (10 MiB) and sound-editor (9 MiB) and sequencer (8 MiB) would free enough") != std::string::npos, "a Performance past the share names the apps to close: " + why);
        s.loadPerformance(b);
        check(s.leaving() == 2 && s.memoryState().committed == 27 * MiB, "the committed Performance loads, A rings out, both committed");
        // Offered: a launch refused only because of the committed next entry.
        s.appClosed("oscairn.groovebox"); s.appClosed("sound-editor"); s.appClosed("sequencer");
        check(s.preparePerformance(a), "A prepared ahead beside B and A's tail");
        m = s.memoryState();
        const auto offered = launch("synth-editor", 27);
        check(!offered.admitted && offered.offerNext, "a launch refused only for the committed next entry offers to let it go: " + offered.reason);
        check(launch("synth-editor", 27).offerNext && s.launchApp({"synth-editor", 27 * MiB}, true).admitted && !s.memoryState().nextPrepared,
              "letting it go opens the app");
        // Memory short (PSI on Linux): one more item let go, the opportunistic first, then a committed next entry.
        check(!s.memoryPressure(), "with nothing prepared, nothing to let go");
        s.appClosed("synth-editor");
        check(s.preparePerformance(a) && s.memoryPressure() && !s.memoryState().nextPrepared, "under pressure even a committed next entry goes, last");
#ifdef __linux__
        // Real pressure, with a delegated cgroup subtree (OSCAIRN_CGROUP): this process in its
        // engine cgroup, memory.high just above what it holds, then allocating; the throttling
        // stalls it, the PSI trigger fires at a service and the prepared entry is let go.
        if(const char* cgroup = std::getenv("OSCAIRN_CGROUP"))
        {
            const fs::path group(cgroup);
            fs::create_directories(group / "engine");
            auto put = [](const fs::path& file, const std::string& text) { std::ofstream out(file); out << text << std::flush; return bool(out); };
            check(put(group / "engine" / "cgroup.procs", std::to_string(getpid())), "this process in the subtree's engine cgroup");
            Session p(48000, frames);
            p.setBudget({.9, 0, 4096 * MiB, 0, 0, 0}); // memory classes on: the trigger armed
            p.loadPerformance(a);
            check(p.preparePerformance(b) && p.memoryState().nextPrepared, "a next entry prepared");
            uint64_t current = 0; { std::ifstream in(group / "memory.current"); in >> current; }
            put(group / "memory.high", std::to_string(current + 16 * MiB));
            std::vector<std::unique_ptr<char[]>> hog; size_t allocated = 0;
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while(p.memoryState().nextPrepared && std::chrono::steady_clock::now() < until && hog.size() < 256)
            {
                hog.emplace_back(new char[4 * MiB]); std::memset(hog.back().get(), 1, 4 * MiB); allocated += 4;
                p.service();
            }
            for(int i = 0; i < 300 && p.memoryState().nextPrepared; ++i) { p.service(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            const bool evicted = !p.memoryState().nextPrepared;
            hog.clear();
            put(group / "memory.high", "max");
            check(evicted, "memory pressure (PSI) lets the prepared entry go, after " + std::to_string(allocated) + " MiB allocated");
        }
#endif
        ::setenv("OSCAIRN_DATA_HOME", (root / "budget-home").c_str(), 1);
    }

    // Switching plugins and wires off and on (RFC 0011's full scene model): an off plugin takes
    // no new notes but plays out the ones it holds, then stops; an off effect in a chain is
    // passed around; an off note wire takes no new notes; undo and saving keep it.
    void onOff(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .25); s.setParameter(b, 7, .5);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b); s.commit();
        Audio x;
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .75), "both play: " + std::to_string(x.l[0]));
        s.setNodeEnabled(a, false); s.commit(); x.run(s);
        check(near(x.l[0], .75) && s.graphState().disabled.count(a) && s.graphState().ringing.count(a), "a held note plays on through the switch: " + std::to_string(x.l[0]));
        check(s.history().undo == "Turn off a", "an edit: " + s.history().undo);
        s.note(false, 60, 0); x.run(s);
        check(near(x.l[0], 0), "its release reaches it: " + std::to_string(x.l[0]));
        s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .5), "a new note goes only to what is on: " + std::to_string(x.l[0]));
        s.note(false, 62, 0);
        for(int i = 0; i < int(48000 * 3.2 / frames) && s.graphState().ringing.count(a); ++i) x.run(s);
        check(!s.graphState().ringing.count(a), "it has rung out after three seconds of silence");
        s.undo(); s.commit(); s.note(true, 64, 1); x.run(s); x.run(s);
        check(!s.graphState().disabled.count(a) && near(x.l[0], .75), "undone: on again and playing: " + std::to_string(x.l[0]));
        s.note(false, 64, 0); x.run(s);

        // An effect off is passed around: the part sounds dry.
        const auto fx = s.add("fx", fixture, "org.kick.test.effect"); s.setParameter(fx, 7, .5);
        s.insertIntoPart(pb, 1, fx); s.commit();
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25 + .25), "B through the effect at half: " + std::to_string(x.l[0]));
        s.setNodeEnabled(fx, false); s.commit(); x.run(s); x.run(s);
        check(near(x.l[0], .25 + .5), "the effect off: B dry: " + std::to_string(x.l[0]));
        s.setNodeEnabled(fx, true); s.commit(); x.run(s); x.run(s);
        check(near(x.l[0], .25 + .25), "and on again: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s);

        // An off note wire: the held note is released, a new one does not pass.
        const auto input = [&] { for(const auto& w : s.wires()) if(w.part == pa && w.kind == WireKind::Notes && w.from == graphInput) return w.id; return 0u; };
        s.note(true, 60, 1); x.run(s);
        s.setWireEnabled(input(), false); s.commit(); x.run(s);
        check(near(x.l[0], .5), "A's held note plays on: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s); s.note(true, 62, 1); x.run(s); x.run(s);
        check(near(x.l[0], .25), "A's input off: only B takes the new note: " + std::to_string(x.l[0]));
        s.note(false, 62, 0); x.run(s);

        // Saved and loaded.
        s.setNodeEnabled(b, false); s.commit();
        s.savePerformance((root / "on-off").string());
        Session t(48000, frames);
        t.loadPerformance((root / "on-off").string());
        const auto g = t.graphState();
        bool inputOff = false;
        for(const auto& w : g.wires) if(w.kind == WireKind::Notes && w.from == graphInput && !w.enabled) inputOff = true;
        check(g.disabled.size() == 1 && g.ringing.empty() && inputOff, "the off plugin and wire round-trip, not ringing");
        t.note(true, 60, 1); Audio y; y.run(t); y.run(t);
        check(near(y.l[0], 0), "nothing on plays: " + std::to_string(y.l[0]));
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
        // A control moving a part level ramps it over 5 ms (four 64-frame blocks): read past the ramp.
        auto render = [&] { s.note(true, 60, 1); for(int i = 0; i < 5; ++i) x.run(s); return x.l[0]; };
        const double e = (std::exp(2.0) - 1) / (std::exp(4.0) - 1); // the exponential curve at .5
        s.setMacro(super, .5);
        double expected = (.2 + .6 * e) * 1 + .5 * (1 - e);
        check(near(render(), expected), "Super at .5: A's gain " + std::to_string(.2 + .6 * e) + ", B at its gain .5 and level " + std::to_string(1 - e) + ": " + std::to_string(x.l[0]));
        // Swept from .5 to 1, B's level ramps down rather than stepping to 0: the first sample after the
        // move still has nearly all of B's .5 * (1 - e) (A's gain is a plugin parameter, so it steps).
        s.setMacro(super, 1); x.run(s);
        check(x.l[0] > .8 + .45 * (1 - e), "a control moving a part level ramps it, not steps: " + std::to_string(x.l[0]));
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

    // RFC 0011's summing rule: two controls on one parameter add their offsets to its base, clamped
    // to its range; the base survives the clamp; a direct edit moves the base under the offsets.
    void summing(const char* fixture)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .4);
        const auto pa = s.addPart("A");
        s.insertIntoPart(pa, 0, a); s.commit();
        Audio x; x.run(s);
        const auto wheel = s.addMacro("Wheel"), knob = s.addMacro("Knob");
        MacroTarget up; up.node = a; up.param = 7; up.relative = true; up.start = .5; up.end = .9; // no change at 0, +.4 at 1
        s.setMacroTargets(wheel, {up}); s.setMacroTargets(knob, {up});
        auto gain = [&] { return s.parameter(a, 7); };
        auto close = [](double v, double e) { return std::abs(v - e) < 1e-9; };
        s.setMacro(wheel, 1);
        check(close(gain(), .8), "one control: the base .4 and +.4: " + std::to_string(gain()));
        s.setMacro(knob, .5);
        check(close(gain(), 1), "two controls add up: .4 + .4 + .2: " + std::to_string(gain()));
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], 1), "and play the sum: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s);
        s.setMacro(knob, 1);
        check(close(gain(), 1), "the sum is clamped to the range: " + std::to_string(gain()));
        s.setMacro(wheel, 0);
        check(close(gain(), .8), "the base survives the clamp: .4 + .4: " + std::to_string(gain()));
        s.setMacro(knob, .5);
        s.setParameter(a, 7, .5);
        s.setMacro(wheel, 1);
        check(close(gain(), .9), "a direct edit to .5 sets the base under the knob's +.2, to .3: .3 + .2 + .4: " + std::to_string(gain()));
        // An absolute target sets the value; a relative one beside it adds its offset.
        MacroTarget set; set.node = a; set.param = 7;
        s.setMacroTargets(knob, {set});
        s.setMacro(knob, .25);
        check(close(gain(), .25 + .4), "an absolute .25 and the wheel's +.4: " + std::to_string(gain()));
        check(s.graphState().macros[0].targets[0].base == s.graphState().macros[1].targets[0].base, "one base per destination");
    }

    // The Performance's level and pan on its main output: after every part, stored by scenes,
    // undone, saved and loaded.
    void master(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .5);
        const auto pa = s.addPart("A");
        s.insertIntoPart(pa, 0, a); s.commit();
        Audio x; x.run(s);
        auto render = [&] { s.note(true, 60, 1); x.run(s); x.run(s); const std::pair<double, double> out{x.l[0], x.r[0]}; s.note(false, 60, 0); x.run(s); return out; };
        const auto full = render();
        check(near(full.first, .5) && near(full.second, .5), "at the master's defaults: " + std::to_string(full.first));
        s.setMaster(.5, 0); s.commit();
        auto out = render();
        check(near(out.first, .25) && near(out.second, .25), "the master at half halves both sides: " + std::to_string(out.first));
        s.setMaster(1, .5); s.commit();
        out = render();
        check(near(out.first, .25) && near(out.second, .5), "panned right: the left at half: " + std::to_string(out.first) + " " + std::to_string(out.second));
        check(s.graphState().masterLevel == 1 && s.graphState().masterPan == .5, "the graph state shows it");
        bool refused = false; try { s.setMaster(3, 0); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a level beyond 2 is refused");
        // Scenes store it with the mix.
        const auto loud = s.addScene("Loud");
        s.setMaster(.25, 0); s.commit();
        const auto quiet = s.addScene("Quiet");
        s.selectScene(loud);
        check(s.graphState().masterLevel == 1 && s.graphState().masterPan == .5, "a scene restores the master");
        s.setSceneScope(quiet, Scene::Mutes);
        s.selectScene(quiet);
        check(s.graphState().masterLevel == 1, "a scene without the mix in its scope leaves it");
        s.setMaster(.75, 0); s.commit();
        check(s.history().undo == "Master" && s.undo() && s.graphState().masterLevel == 1, "an edit, undone: " + s.history().undo);
        s.redo();
        s.savePerformance((root / "master").string());
        Session t(48000, frames);
        t.loadPerformance((root / "master").string());
        const auto g = t.graphState();
        check(g.masterLevel == .75 && g.masterPan == 0 && g.scenes.size() == 2 && g.scenes[0].masterLevel == 1 && g.scenes[0].masterPan == .5 && g.scenes[1].masterLevel == .25,
              "the master and the scenes' round-trip");
        t.note(true, 60, 1); Audio y; y.run(t); y.run(t);
        check(near(y.l[0], .5 * .75), "and the loaded master plays: " + std::to_string(y.l[0]));
    }

    // RFC 0016's node roles: set, checked, undone, saved and loaded, and gone with their node.
    void roles(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        const auto pa = s.addPart("A");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pa, 1, b); s.commit();
        s.setNodeRole(a, "arpeggiator"); s.setNodeRole(b, "insertion-a"); s.commit();
        check(s.graphState().roles == std::map<NodeId, std::string>{{a, "arpeggiator"}, {b, "insertion-a"}}, "both roles set");
        bool refused = false; try { s.setNodeRole(a, "insertion-c"); } catch(const std::invalid_argument&) { refused = true; }
        check(refused && s.graphState().roles.at(a) == "arpeggiator", "an unknown role is refused");
        s.setNodeRole(b, "reverb"); s.commit();
        check(s.history().undo == "Node role" && s.undo() && s.graphState().roles.at(b) == "insertion-a", "a role is an edit, undone");
        s.setNodeRole(b, ""); s.commit();
        check(!s.graphState().roles.count(b), "an empty role clears it");
        s.setNodeRole(b, "part-eq"); s.commit();
        s.savePerformance((root / "roles").string());
        Session t(48000, frames);
        t.loadPerformance((root / "roles").string());
        check(t.graphState().roles == std::map<NodeId, std::string>{{t.nodeId("a"), "arpeggiator"}, {t.nodeId("b"), "part-eq"}}, "roles round-trip with their nodes");
        s.removeFromPart(pa, b); s.remove(b, UserWires::Remove); s.commit();
        check(!s.graphState().roles.count(b), "a removed node takes its role");
    }

    // An optional asset reference that does not resolve: the Performance loads, with a notice; a
    // required one refuses it, leaving what plays.
    void optionalAssets(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth");
        const auto pa = s.addPart("A");
        s.insertIntoPart(pa, 0, a); s.commit();
        s.savePerformance((root / "assets").string());
        const auto manifest = root / "assets" / "performance.json";
        std::string text; { std::ifstream in(manifest); text.assign(std::istreambuf_iterator<char>(in), {}); }
        auto with = [&](const std::string& flag) {
            auto t = text;
            const auto at = t.find("\"assets\":[]");
            t.replace(at, 11, "\"assets\":[{\"library\":\"nowhere\",\"path\":\"phrases/gone.ophr\",\"sha256\":\"" + std::string(64, '0') + "\"" + flag + "}]");
            std::ofstream(manifest) << t;
        };
        with(",\"optional\":true");
        Session t(48000, frames);
        t.loadPerformance((root / "assets").string());
        const auto notices = t.sessionStatus().notices;
        check(t.graphState().parts.size() == 1 && notices.size() == 1 && notices[0].find("gone.ophr") != std::string::npos, "an optional asset missing: loaded, with a notice");
        with("");
        std::string why; const auto revision = t.graphState().revision;
        try { t.loadPerformance((root / "assets").string()); } catch(const std::exception& e) { why = e.what(); }
        check(why.find("missing sound assets") != std::string::npos && t.graphState().revision == revision, "a required one missing: refused, what plays kept: " + why);
    }

    // Level curves: a part's fader in dB, and a power law; their numbers checked; saved and loaded.
    void levelCurves(const char* fixture, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth");
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.commit();
        const auto volume = s.addMacro("Volume");
        MacroTarget db; db.kind = MacroTarget::Kind::PartLevel; db.part = pa; db.curve = MacroTarget::Curve::Db; db.low = -96; db.high = 0;
        MacroTarget power = db; power.part = pb; power.curve = MacroTarget::Curve::Power; power.exponent = 2;
        s.setMacroTargets(volume, {db, power});
        auto level = [&](uint32_t part) { for(const auto& p : s.graphState().parts) if(p.id == part) return p.level; return -1.0; };
        auto close = [](double v, double e) { return std::abs(v - e) < 1e-9; };
        s.setMacro(volume, .5);
        check(close(level(pa), std::pow(10.0, -48.0 / 20)), "dB: half way is -48 dB: " + std::to_string(level(pa)));
        check(close(level(pb), .25), "power 2: half way is .25: " + std::to_string(level(pb)));
        s.setMacro(volume, 1);
        check(close(level(pa), 1) && close(level(pb), 1), "both at 0 dB at the top");
        s.setMacro(volume, .75);
        check(close(level(pa), std::pow(10.0, -24.0 / 20)), "dB: three quarters is -24 dB: " + std::to_string(level(pa)));
        s.setMacro(volume, 0);
        check(level(pa) == 0 && level(pb) == 0, "silence at the bottom");
        for(auto [low, high, exponent] : {std::tuple{0.0, -6.0, 2.0}, {-200.0, 0.0, 2.0}, {-96.0, 6.0, 2.0}})
        {
            MacroTarget wrong = db; wrong.low = low; wrong.high = high; wrong.exponent = exponent;
            bool refused = false; try { s.setMacroTargets(volume, {wrong}); } catch(const std::invalid_argument&) { refused = true; }
            check(refused, "a dB curve from " + std::to_string(low) + " to " + std::to_string(high) + " is refused");
        }
        bool refused = false; try { MacroTarget wrong = power; wrong.exponent = 20; s.setMacroTargets(volume, {wrong}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a power curve's exponent beyond 10 is refused");
        s.savePerformance((root / "curves").string());
        Session t(48000, frames);
        t.loadPerformance((root / "curves").string());
        const auto state = t.graphState(); const auto& loaded = state.macros.at(0).targets;
        check(loaded.size() == 2 && loaded[0].curve == MacroTarget::Curve::Db && loaded[0].low == -96 && loaded[0].high == 0 &&
              loaded[1].curve == MacroTarget::Curve::Power && loaded[1].exponent == 2, "both curves round-trip with their numbers");
    }

    // Live gains (RFC 0011's instant controls): a control playing a part's level (as "Drifting"'s
    // mod wheel does), a fader, a mute, the master and an empty bus's fader reach the sound in the
    // next block, ramped across it, and compile no plan; an edit of the structure still does.
    void liveGains(const char* fixture, const char* audio, const fs::path& root)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"); s.setParameter(a, 7, .5);
        const auto part = s.addPart("A"); s.insertIntoPart(part, 0, a); s.commit();
        const auto wheel = s.addMacro("Mod Wheel");
        MacroTarget t; t.kind = MacroTarget::Kind::PartLevel; t.part = part; t.curve = MacroTarget::Curve::Logarithmic; t.start = .707; t.end = .0021;
        s.setMacroTargets(wheel, {t}); s.setMacro(wheel, 0); s.commit();
        Audio x; s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[frames - 1], .5 * .707), "the wheel at 0: the part at .707: " + std::to_string(x.l[frames - 1]));
        const auto plans = s.plansCompiled();
        bool levels = true, ramps = true;
        for(const double v : {.25, .5, 1.0, .1, 0.0})
        {
            const float before = x.l[frames - 1];
            s.setMacro(wheel, v); x.run(s);
            const double expected = .5 * (t.start + (t.end - t.start) * t.shape(v));
            levels = levels && near(x.l[frames - 1], expected);
            // Each sample on the way from the last block's level to the new one, never past it.
            for(uint32_t i = 0; i < frames; ++i) ramps = ramps && x.l[i] >= std::min<double>(before, expected) - 1e-6 && x.l[i] <= std::max<double>(before, expected) + 1e-6;
            ramps = ramps && (before == float(expected) || x.l[0] != float(expected));
        }
        check(levels, "each move of the wheel is the part's level by the end of the next block");
        check(ramps, "ramped across that block, from the last level to the new");
        check(s.plansCompiled() == plans, "and no plan compiled: " + std::to_string(s.plansCompiled() - plans));
        check(std::abs(s.parts()[0].level - t.map(0)) < 1e-9, "the part's level in the model, for the views");
        s.setPartLevel(part, .25, false); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .125), "a fader through the control API, committed: " + std::to_string(x.l[frames - 1]));
        s.setPartLevel(part, .25, true); s.commit(); x.run(s);
        check(near(x.l[frames - 1], 0), "muted");
        s.setPartLevel(part, 1, false); s.setMaster(.5, 0); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .25), "unmuted, the master at half: " + std::to_string(x.l[frames - 1]));
        check(s.plansCompiled() == plans, "none of them compiled a plan");
        for(const auto& w : s.wires()) if(w.fader) check(w.level == 1 && !w.mute, "the fader wire shows the level, for the views");
        // A pan, a send's level and a scene's glide are live gains too.
        s.setPartPan(part, -.5); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .25) && near(x.r[frames - 1], .125), "a pan, heard by the end of the next block: " + std::to_string(x.l[frames - 1]) + " " + std::to_string(x.r[frames - 1]));
        check(s.plansCompiled() == plans, "and no plan compiled for it: " + std::to_string(s.plansCompiled() - plans));
        check(s.liveValues().parts[0].pan == -.5, "the pan among the live values clients are sent");
        s.setPartPan(part, 0); s.commit(); x.run(s);
        // A send into a bus with a box (passing its sound on): routes of its own, so a live gain of its own.
        const auto room = s.addPart("Room", {PartInput::Kind::Bus}), thru = s.add("thru", audio, "org.oscairn.test.audio-ports");
        s.insertIntoPart(room, 0, thru); s.setSend(part, room, .5); s.commit(); x.run(s); x.run(s);
        check(near(x.l[frames - 1], .375), "a send into the bus, with the part: " + std::to_string(x.l[frames - 1]));
        const auto sending = s.plansCompiled();
        s.setSend(part, room, .25); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .3125) && s.plansCompiled() == sending, "a send's level moved, heard at the next block, no plan: " + std::to_string(x.l[frames - 1]));
        s.setSend(part, room, {}); s.removePart(room); s.remove(thru); s.commit(); x.run(s);
        // A scene's glide: every step a live gain, from the last block's value to the next.
        const auto centre = s.addScene("Centre");
        s.setPartPan(part, -1); s.setPartLevel(part, .5, false); s.commit();
        const auto left = s.addScene("Left");
        s.setSceneTransition(left, 40, Scene::Curve::Linear, Scene::Launch::Now);
        s.selectScene(centre); x.run(s); s.service(); x.run(s);
        check(near(x.r[frames - 1], .25), "the centre scene: " + std::to_string(x.r[frames - 1]));
        const auto gliding = s.plansCompiled();
        s.selectScene(left);
        std::vector<float> right;
        for(int i = 0; i < 60; ++i) { x.run(s); s.service(); right.push_back(x.r[frames - 1]); }
        x.run(s);
        const bool between = std::any_of(right.begin(), right.end(), [](float v) { return v > .01 && v < .24; });
        check(between && near(x.r[frames - 1], 0) && near(x.l[frames - 1], .125), "the glide moves through the values between to the scene's: " + std::to_string(x.l[frames - 1]) + " " + std::to_string(x.r[frames - 1]));
        check(s.plansCompiled() == gliding, "its steps compile no plan: " + std::to_string(s.plansCompiled() - gliding));
        s.removeScene(centre); s.removeScene(left); s.setPartPan(part, 0); s.setPartLevel(part, 1, false); s.commit(); x.run(s);
        // Through an empty bus: its fader is a live gain too, multiplied with the part's.
        const auto bus = s.addPart("Bus", {PartInput::Kind::Bus}); s.setPartOutput(part, bus); s.commit(); x.run(s);
        const auto through = s.plansCompiled();
        s.setPartLevel(bus, .5, false); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .125), "the empty bus's fader: " + std::to_string(x.l[frames - 1]));
        s.setPartLevel(part, .5, false); s.commit(); x.run(s);
        check(near(x.l[frames - 1], .0625) && s.plansCompiled() == through, "with the part's, and no plan: " + std::to_string(x.l[frames - 1]));
        // A Performance loaded: its faders are live from the start.
        s.savePerformance((root / "live").string());
        Session u(48000, frames);
        u.loadPerformance((root / "live").string());
        Audio y; u.note(true, 60, 1); y.run(u); y.run(u);
        check(near(y.l[frames - 1], .0625), "loaded, it plays the same: " + std::to_string(y.l[frames - 1]));
        const auto loaded = u.plansCompiled();
        u.setPartLevel(part, 1, false); u.commit(); y.run(u);
        check(near(y.l[frames - 1], .125) && u.plansCompiled() == loaded, "and its faders move without a plan: " + std::to_string(y.l[frames - 1]));
    }

    // A solo undone, committed and saved: the same file as before it, playing the same. Each part
    // soloed and heard (a muted one unmuted), then put back as it was: a copy of the parts, since
    // parts() is the live model and the edits change it.
    void soloRoundTrip(const char* fixture, const char* delay, const fs::path& root)
    {
        Session s(48000, frames);
        const auto low = s.add("low", fixture, "org.kick.test.synth"), high = s.add("high", fixture, "org.kick.test.synth"), third = s.add("third", fixture, "org.kick.test.synth");
        s.setParameter(low, 7, .25); s.setParameter(high, 7, .5); s.setParameter(third, 7, .125);
        const auto a = s.addPart("A"), b = s.addPart("B"), c = s.addPart("C"), room = s.addPart("Room", {PartInput::Kind::Bus});
        s.insertIntoPart(a, 0, low); s.insertIntoPart(b, 0, high); s.insertIntoPart(c, 0, third);
        const auto dry = s.add("dry", delay); s.setParameter(dry, 3, 0);
        s.insertIntoPart(room, 0, dry);
        s.setSend(a, room, .5); s.setPartPan(b, .25); s.setPartLevel(a, .75, false); s.setPartLevel(c, .5, true); s.commit();
        Audio x; x.run(s); x.run(s);
        const auto before = root / "solo-before", after = root / "solo-after";
        s.savePerformance(before.string());
        for(const auto& part : std::vector<Part>(s.parts()))
        {
            s.setPartSolo(part.id, true); s.setPartLevel(part.id, part.level, false); s.commit(); x.run(s);
            s.setPartSolo(part.id, false); s.setPartLevel(part.id, part.level, part.mute); s.commit(); x.run(s);
        }
        s.savePerformance(after.string());
        auto manifest = [](const fs::path& dir) {
            const auto bytes = state_files::read(dir / "performance.json", 1 << 22);
            return std::regex_replace(std::string(bytes.begin(), bytes.end()), std::regex(R"re(,"revision":"([0-9a-f]*)","parent":"([0-9a-f]*)")re"), "");
        };
        const auto m1 = manifest(before), m2 = manifest(after);
        check(m1 == m2, "a solo undone saves the same Performance:\n" + m1 + "\n" + m2);
        Session t(48000, frames), u(48000, frames);
        t.loadPerformance(before.string()); u.loadPerformance(after.string());
        Audio y, z; s.note(true, 60, 1); t.note(true, 60, 1); u.note(true, 60, 1);
        bool same = true, live = true; double energy = 0;
        for(int i = 0; i < 20; ++i)
        {
            x.run(s); y.run(t); z.run(u);
            same = same && y.l == z.l && y.r == z.r; live = live && x.l == y.l && x.r == y.r;
            for(float v : y.l) energy += std::abs(v);
        }
        check(same && energy > 0, "and the two play the same: " + std::to_string(y.l[frames - 1]) + " " + std::to_string(z.l[frames - 1]));
        check(live, "as the Session that saved them: " + std::to_string(x.l[frames - 1]) + " " + std::to_string(y.l[frames - 1]));
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
        // Saving it again gives the same manifest, as the next revision of the one opened.
        const auto again = root / "again";
        t.savePerformance(again.string());
        const auto first = state_files::read(saved / "performance.json", 1 << 22), second = state_files::read(again / "performance.json", 1 << 22);
        const std::string opened(first.begin(), first.end()), resaved(second.begin(), second.end());
        const std::regex revisions(R"re(,"revision":"([0-9a-f]*)","parent":"([0-9a-f]*)")re");
        std::smatch ro, rr;
        const bool found = std::regex_search(opened, ro, revisions) && std::regex_search(resaved, rr, revisions);
        check(found && rr[2] == ro[1] && rr[1] != ro[1], "the save is the next revision of the one opened");
        check(std::regex_replace(opened, revisions, "") == std::regex_replace(resaved, revisions, ""), "saving the loaded Performance gives the same manifest");

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
        const auto old = root / "version-4";
        fs::copy(saved, old, fs::copy_options::recursive);
        auto text = std::string(first.begin(), first.end());
        text.replace(text.find("\"version\":13"), 12, "\"version\":12");
        state_files::write(old / "performance.json", text.data(), text.size());
        const auto before = describe(t);
        bool refused = false; try { t.loadPerformance(old.string()); } catch(const std::runtime_error& e) { refused = std::string(e.what()).find("unsupported format version") != std::string::npos; }
        check(refused && describe(t) == before, "a version 9 Performance is refused and the session kept");
    }
}

int main(int argc, char** argv)
{
    if(argc < 8) { std::fprintf(stderr, "usage: %s FIXTURE AUDIO_FIXTURE DELAY SPLITTERS TRANSPOSE FOLLOWER VOICES\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory temp;
        ownership(argv[1], argv[3]);
        channels(argv[2]);
        replacement(argv[2], argv[3]);
        zones(argv[1]);
        unmuteBaked(argv[1]);
        devices(argv[1], temp.path);
        midiOut(argv[1], argv[5], temp.path);
        harmonyKeys(argv[1], argv[6], temp.path);
        splits(argv[1], argv[3], temp.path);
        soundSplits(argv[3], argv[4], temp.path);
        mixer(argv[1], argv[3], temp.path);
        noteLimits(argv[1], temp.path);
        voiceBudget(argv[7], temp.path);
        buses(argv[1], argv[3]);
        emptyBus(argv[1]);
        innerZones(argv[1], argv[5], argv[3], temp.path);
        scenes(argv[1], temp.path);
        sceneSwitching(argv[1], temp.path);
        onOff(argv[1], temp.path);
        performanceChange(argv[1], temp.path);
        stuckRingOut(temp.path);
        hardwarePanic(argv[1], temp.path);
        audition(argv[1], temp.path);
        allNotesOff(argv[1]);
        unavailable(argv[1], temp.path);
        sceneTransitions(argv[1], temp.path);
        budgets(argv[1], temp.path);
        rebuilt(argv[1], temp.path);
        declaredMemory(argv[1], argv[3], temp.path);
        memoryClasses(argv[1], temp.path);
        controls(argv[1], temp.path);
        summing(argv[1]);
        master(argv[1], temp.path);
        roles(argv[1], temp.path);
        optionalAssets(argv[1], temp.path);
        levelCurves(argv[1], temp.path);
        liveGains(argv[1], argv[2], temp.path);
        soloRoundTrip(argv[1], argv[3], temp.path);
        persistence(argv[1], argv[2], argv[3], temp.path);
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: part and wire ownership, Mix, channel rules, replacement, devices, MIDI out and persistence");
    return failures ? 1 : 0;
}

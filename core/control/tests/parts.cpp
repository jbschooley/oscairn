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
    if(argc < 4) { std::fprintf(stderr, "usage: %s FIXTURE AUDIO_FIXTURE DELAY\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory temp;
        ownership(argv[1], argv[3]);
        channels(argv[2]);
        replacement(argv[2], argv[3]);
        zones(argv[1]);
        buses(argv[1], argv[3]);
        scenes(argv[1], temp.path);
        persistence(argv[1], argv[2], argv[3], temp.path);
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: part and wire ownership, Mix, channel rules, replacement and persistence");
    return failures ? 1 : 0;
}

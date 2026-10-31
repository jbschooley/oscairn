// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Undo and redo (RFC 0011): every edit can be undone and redone; a removed plugin comes back with
// its state; repeated changes to one thing are one step; playing (scenes, controls) is not an
// edit and undo leaves it be.
//   oscairn-undo-test FIXTURE.clap SPLITTERS.clap
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

using namespace oscairn;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 64;
    struct Audio
    {
        std::array<float, frames> l{}, r{};
        float* out[2]{l.data(), r.data()};
        float run(Session& s) { s.service(); check(s.process(nullptr, 0, out, 2, frames), "render"); return l[0]; }
    };
    bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
    const Part* partNamed(const Session& s, const std::string& name)
    {
        for(const auto& p : s.parts()) if(p.name == name) return &p;
        return nullptr;
    }
}

int main(int argc, char** argv)
{
    if(argc < 3) return 2;
    const char* fixture = argv[1];
    const char* splitters = argv[2];
    try
    {
        Session s(48000, frames);
        Audio x;
        check(s.history().undo.empty() && !s.undo(), "nothing to undo at first");

        // One action, many calls, one commit: one step.
        const auto synth = s.add("synth", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys");
        s.insertIntoPart(keys, 0, synth); s.commit();
        check(s.history().undo == "Add synth", "the step is named for what it did: " + s.history().undo);
        x.run(s);
        const double initial = s.parameter(synth, 7);
        s.setParameter(synth, 7, .3);
        check(s.history().undo.rfind("synth ", 0) == 0, "a parameter change is a step: " + s.history().undo);
        s.setParameter(synth, 7, .35); s.setParameter(synth, 7, .4); // a knob turned: one step
        x.run(s); s.note(true, 60, 1); x.run(s);
        check(near(x.l[0], .4), "the synth plays at its gain");
        check(s.undo() && s.history().undo == "Add synth", "one undo takes back the whole turn of the knob");
        x.run(s); x.run(s);
        check(near(s.parameter(synth, 7), initial), "the parameter is back where it was: " + std::to_string(s.parameter(synth, 7)));
        const double before = x.l[0];
        check(s.redo() && s.history().redo.empty(), "and redo puts it back");
        x.run(s); x.run(s);
        check(near(x.l[0], .4) && !near(before, .4), "heard: " + std::to_string(before) + " then " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s);

        // Removing a plugin, undone: the same plugin comes back with its state, in its place.
        s.removeFromPart(keys, synth); s.remove(synth, UserWires::Reconnect); s.commit();
        check(!s.hasNode(synth) && s.parts()[0].chain.empty() && s.history().undo == "Remove synth", "removed: " + s.history().undo);
        check(s.undo(), "undo the removal");
        check(s.hasNode(synth) && s.parts()[0].chain == std::vector<NodeId>{synth}, "the same node is back in the part");
        x.run(s); s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .4), "with its gain, which only the plugin held: " + std::to_string(x.l[0]));
        s.note(false, 60, 0); x.run(s);
        check(s.redo() && !s.hasNode(synth), "redo removes it again");
        check(s.undo() && s.hasNode(synth), "and undo restores it again");

        // Replacing a plugin, undone: the old one comes back with its state.
        s.replace(synth, fixture, "org.kick.test.synth"); s.service();
        check(s.history().undo == "Replace synth", "the replacement is a step: " + s.history().undo);
        x.run(s); x.run(s);
        check(!near(s.parameter(synth, 7), .4), "the new plugin starts from its defaults");
        s.undo(); x.run(s); x.run(s);
        check(near(s.parameter(synth, 7), .4) && s.parts()[0].chain == std::vector<NodeId>{synth}, "the old plugin is back, with its gain: " + std::to_string(s.parameter(synth, 7)));
        s.note(true, 60, 1); x.run(s); x.run(s);
        check(near(x.l[0], .4), "and plays");
        s.note(false, 60, 0); x.run(s);

        // A fader dragged: one step back to where it started.
        for(const double level : {.9, .8, .7, .6}) { s.setPartLevel(keys, level, false); s.commit(); }
        check(s.history().undo == "Keys level", "a drag is one step: " + s.history().undo);
        s.undo();
        check(near(s.parts()[0].level, 1), "back to where the drag started: " + std::to_string(s.parts()[0].level));

        // Switching scenes is playing: not a step, and undo leaves it be.
        const auto loud = s.addScene("Loud");
        s.setPartLevel(keys, .5, false); s.commit();
        const auto soft = s.addScene("Soft");
        s.setPartName(keys, "Piano"); s.commit();
        const auto renamed = s.history().undo;
        s.selectScene(loud);
        check(s.history().undo == renamed && near(s.parts()[0].level, 1), "selecting a scene is not an edit");
        s.undo();
        check(partNamed(s, "Keys") && near(s.parts()[0].level, 1), "undoing the rename keeps the scene's level");
        s.redo(); s.selectScene(soft);

        // Moving a control is playing too.
        const auto knob = s.addMacro("Knob");
        MacroTarget level; level.kind = MacroTarget::Kind::PartLevel; level.part = keys;
        s.setMacroTargets(knob, {level}); s.service();
        const auto targeted = s.history().undo;
        s.setMacro(knob, .25); s.service();
        check(s.history().undo == targeted && targeted.find("Knob") != std::string::npos, "moving a control is not an edit: " + targeted);

        // Changes that need no commit become steps at the next tick: pads, here.
        auto pads = s.padMap(); pads.assignments[0][0].note = 30; s.setPadMap(pads); s.service();
        check(s.history().undo == "Pads", "the pad map edit is a step: " + s.history().undo);
        s.undo(); s.service();
        check(s.padMap().assignments[0][0].note != 30, "and undone");

        // A new edit drops what could be redone.
        check(!s.history().redo.empty(), "something to redo");
        s.setPartName(keys, "Rhodes"); s.commit();
        check(s.history().redo.empty(), "a new edit clears redo");

        // A split with its own nodes: undone, its nodes go; redone, they come back with their modes.
        const auto sp = s.add("split", splitters, splitPlugin), jn = s.add("join", splitters, joinPlugin);
        s.addSplit(keys, 1, {}, Split::Kind::MidSide, sp, jn); s.commit();
        check(s.parts()[0].splits.size() == 1, "a mid/side split");
        s.undo();
        check(s.parts()[0].splits.empty() && !s.hasNode(sp) && !s.hasNode(jn), "undone with its nodes");
        s.redo(); x.run(s);
        check(s.parts()[0].splits.size() == 1 && s.hasNode(sp) && s.parameter(sp, 1) == 1, "redone with its mode");

        // Trying plugins one after another: the history keeps at most eight removed ones alive,
        // dropping the oldest steps (the 2026-10-31 audit: a sampler each, for a hundred steps).
        {
            Session t(48000, frames);
            for(int i = 0; i < 12; ++i) { const auto n = t.add("try" + std::to_string(i), fixture, "org.kick.test.synth"); t.commit(); t.remove(n); t.commit(); }
            int undos = 0, restored = 0;
            while(t.undo()) { ++undos; restored = int(t.graphState().topology.nodes.size()); }
            check(undos <= 2 * 8 + 1 && undos >= 2 * 8 - 1, "the history holds about eight removed plugins: " + std::to_string(undos) + " steps");
            check(restored <= 1, "and undoes them in order back to where it can: " + std::to_string(restored));
        }

        // Opening a Performance starts a new history.
        state_files::TemporaryDirectory temp;
        s.savePerformance((temp.path / "p").string());
        s.loadPerformance((temp.path / "p").string());
        check(s.history().undo.empty() && s.history().redo.empty(), "opening a Performance clears the history");
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: undo and redo every edit; removed plugins return with their state; playing is not an edit");
    return failures ? 1 : 0;
}

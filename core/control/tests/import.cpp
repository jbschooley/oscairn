// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021: importing a Performance into another. All of it renders sample for sample as the
// Performance opened on its own; into a Performance whose names it shares, scenes, controls and
// plugins take the next free name, its sends follow its own bus, and the open Performance's
// scenes keep the arriving parts; one step to undo. Chosen parts bring their buses, their
// controls and the Performance's controls aimed only at them, and only the scenes chosen; a
// partial import takes at least one part and one scene. A note held through an import plays on.
//   oscairn-import-test FIXTURE.clap DELAY.clap
#include "oscairn/session.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

using namespace oscairn;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 64;

    bool same(Session& x, Session& y)
    {
        std::array<float, frames> xl{}, xr{}, yl{}, yr{};
        float* xo[2]{xl.data(), xr.data()}; float* yo[2]{yl.data(), yr.data()};
        bool equal = true; double energy = 0;
        for(int block = 0; block < 300; ++block)
        {
            for(Session* s : {&x, &y})
            {
                if(block == 1) s->note(true, 72, 1);
                if(block == 10) { s->note(true, 40, 1); s->note(true, 100, 1); }
                if(block == 30) { s->note(false, 40, 0); s->note(false, 72, 0); s->note(false, 100, 0); }
                s->service();
            }
            forbidAllocation = true;
            const bool ok = x.process(nullptr, 0, xo, 2, frames) && y.process(nullptr, 0, yo, 2, frames);
            forbidAllocation = false;
            equal = equal && ok && xl == yl && xr == yr;
            for(float v : xl) energy += std::abs(v);
        }
        return equal && energy > 0;
    }
    std::optional<Macro> control(const Session& s, const std::string& name) { for(const auto& m : s.graphState().macros) if(m.name == name) return m; return std::nullopt; }
    const Part* partNamed(const std::vector<Part>& parts, const std::string& name) { for(const auto& p : parts) if(p.name == name) return &p; return nullptr; }
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s FIXTURE DELAY\n", argv[0]); return 2; }
    const char* fixture = argv[1]; const char* delay = argv[2];
    try
    {
        state_files::TemporaryDirectory home;
        ::setenv("OSCAIRN_DATA_HOME", home.path.c_str(), 1);
        const auto folder = std::filesystem::path(home.path) / "Gig.oscairn";

        // The Performance to import: Keys (a split, an echo after it, a user wire, two controls of
        // its own) sending to a Hall bus, a Lead, a Super on Keys and its Tone, an Other on Lead,
        // and two scenes.
        uint32_t keys = 0, hall = 0, lead = 0;
        {
            Session a(48000, frames);
            const auto low = a.add("Low", fixture, "org.kick.test.synth"), high = a.add("High", fixture, "org.kick.test.synth"), fx = a.add("Echo", delay);
            const auto room = a.add("Room", delay), solo = a.add("Solo", fixture, "org.kick.test.synth");
            a.setParameter(low, 7, .25); a.setParameter(high, 7, .5); a.setParameter(solo, 7, .3);
            keys = a.addPart("Keys");
            const auto split = a.addSplit(keys, 0);
            a.insertIntoPart(keys, 0, low, {split, 0}); a.insertIntoPart(keys, 0, high, {split, 1});
            a.insertIntoPart(keys, 1, fx);
            NoteZone below; below.highKey = 59; NoteZone above; above.lowKey = 60;
            Lane la; la.zones = {below}; la.level = .5; a.setLane(keys, split, 0, la);
            Lane lb; lb.zones = {above}; lb.level = .8; a.setLane(keys, split, 1, lb);
            a.connectAudio(low, fx);
            hall = a.addPart("Hall", {PartInput::Kind::Bus});
            a.insertIntoPart(hall, 0, room);
            a.setSend(keys, hall, .4);
            lead = a.addPart("Lead");
            a.insertIntoPart(lead, 0, solo);
            a.setPartLevel(lead, .6, false);
            a.commit();
            MacroTarget tone; tone.node = low; tone.param = 7;
            const auto toneId = a.addMacro("Tone", keys); a.setMacroTargets(toneId, {tone});
            MacroTarget level; level.kind = MacroTarget::Kind::PartLevel; level.part = keys;
            MacroTarget onTone; onTone.kind = MacroTarget::Kind::Macro; onTone.macro = toneId;
            a.setMacroTargets(a.addMacro("Super"), {level, onTone});
            MacroTarget leadGain; leadGain.node = solo; leadGain.param = 7;
            a.setMacroTargets(a.addMacro("Other"), {leadGain});
            a.addScene("Verse"); a.addScene("Scene 3");
            a.savePerformance(folder.string());
        }

        // What it holds, to choose from.
        Session probe(48000, frames);
        const auto listed = probe.performanceContents(folder.string());
        check(listed.parts.size() == 3 && listed.parts[0].name == "Keys" && listed.parts[1].bus && !listed.parts[2].bus, "its parts listed, the bus marked");
        check(listed.scenes.size() == 2 && listed.scenes[0].name == "Verse", "and its scenes");

        // All of it into an empty Performance sounds as the Performance itself.
        {
            Session opened(48000, frames); opened.loadPerformance(folder.string());
            Session into(48000, frames);
            const auto r = into.importPerformance(folder.string());
            check(r.parts.size() == 3 && r.notes.empty(), "three parts arrive, nothing renamed");
            check(same(opened, into), "imported whole, it renders as the Performance opened");
        }

        // Into a Performance that shares its names.
        Session b(48000, frames);
        const auto bass = b.add("Low", fixture, "org.kick.test.synth");
        const auto bassPart = b.addPart("Bass"); b.insertIntoPart(bassPart, 0, bass); b.commit();
        b.addMacro("Super");
        const auto verse = b.addScene("Verse"); b.addScene("Scene 3");
        const auto before = b.parts().size();
        const auto r = b.importPerformance(folder.string());
        auto noted = [&](const std::string& n) { return std::find(r.notes.begin(), r.notes.end(), n) != r.notes.end(); };
        check(noted("the scene Verse is Verse 2 here") && noted("the scene Scene 3 is Scene 4 here") && noted("the control Super is Super 2 here"), "clashing names take the next number");
        const auto g = b.graphState();
        auto sceneNamed = [&](const std::string& n) { return std::any_of(g.scenes.begin(), g.scenes.end(), [&](const Scene& s) { return s.name == n; }); };
        check(g.scenes.size() == 4 && sceneNamed("Verse 2") && sceneNamed("Scene 4"), "its scenes added beside these");
        check(std::any_of(g.names.begin(), g.names.end(), [](const auto& n) { return n.second == "Low 2"; }), "a plugin whose name is taken takes the next number");
        const auto parts = b.parts();
        const auto* k = partNamed(parts, "Keys"); const auto* h = partNamed(parts, "Hall");
        check(parts.size() == before + 3 && k && h && k->sends.size() == 1 && k->sends[0].bus == h->id && k->splits.size() == 1, "its parts arrive, sending to its own bus");
        const auto super2 = control(b, "Super 2"); const auto tone2 = control(b, "Tone");
        check(super2 && tone2 && tone2->part == k->id && super2->part == 0 && super2->targets.size() == 2 && super2->targets[0].part == k->id && super2->targets[1].macro == tone2->id,
              "its controls follow its parts and each other");
        const auto& v = *std::find_if(g.scenes.begin(), g.scenes.end(), [&](const Scene& s) { return s.id == verse; });
        const auto* l = partNamed(parts, "Lead");
        check(l && std::any_of(v.parts.begin(), v.parts.end(), [&](const auto& st) { return st.part == l->id && st.level == .6; }), "this Performance's scenes keep the arriving parts as they arrive");
        b.undo();
        check(b.parts().size() == before && b.graphState().scenes.size() == 2 && b.graphState().macros.size() == 1, "one step to undo");

        // A partial import takes at least one part and one scene.
        Session none(48000, frames);
        bool refusedPart = false, refusedScene = false;
        try { none.importPerformance(folder.string(), {}, {listed.scenes[0].id}); } catch(const std::exception&) { refusedPart = true; }
        try { none.importPerformance(folder.string(), {keys}); } catch(const std::exception&) { refusedScene = true; }
        check(refusedPart && refusedScene && none.parts().empty(), "a partial import without a part or a scene is refused");

        // Just Keys with Verse: its bus comes, and the controls aimed only at it; not Lead or Other.
        Session c(48000, frames);
        const auto only = c.importPerformance(folder.string(), {keys}, {listed.scenes[0].id});
        const auto cp = c.parts();
        check(only.parts.size() == 2 && partNamed(cp, "Keys") && partNamed(cp, "Hall") && !partNamed(cp, "Lead"), "a chosen part brings the bus it sends to");
        check(control(c, "Tone") && control(c, "Super") && !control(c, "Other") && c.graphState().scenes.size() == 1, "its controls and those aimed only at it; the scene chosen");
        bool refused = false; try { c.importPerformance(folder.string(), {99}, {listed.scenes[0].id}); } catch(const std::exception&) { refused = true; }
        check(refused && c.parts().size() == 2, "a part that is not there is refused, nothing changed");

        // A note held while importing plays on as if nothing happened (the imported parts had no
        // note-on, so its release does nothing there).
        {
            auto held = [&](bool importing) {
                Session h(48000, frames);
                const auto synth = h.add("Held", fixture, "org.kick.test.synth"), echo = h.add("Echo", delay);
                const auto p = h.addPart("Held"); h.insertIntoPart(p, 0, synth); h.insertIntoPart(p, 1, echo); h.commit();
                std::vector<float> out; std::array<float, frames> l{}, r{}; float* o[2]{l.data(), r.data()};
                for(int block = 0; block < 300; ++block)
                {
                    if(block == 2) h.note(true, 60, 1);
                    if(block == 10 && importing) h.importPerformance(folder.string());
                    if(block == 40) h.note(false, 60, 0);
                    h.service();
                    forbidAllocation = true; const bool ok = h.process(nullptr, 0, o, 2, frames); forbidAllocation = false;
                    check(ok, "render");
                    out.insert(out.end(), l.begin(), l.end());
                }
                return out;
            };
            const auto plain = held(false), through = held(true);
            double energy = 0; for(float v : plain) energy += std::abs(v);
            check(energy > 0 && plain == through, "a note held through an import plays on unchanged");
        }

        // Chosen parts and chosen scenes: Keys and only Verse, holding only what came.
        Session e(48000, frames);
        e.importPerformance(folder.string(), {keys}, {listed.scenes[0].id});
        const auto es = e.graphState().scenes;
        const auto ep = e.parts();
        check(es.size() == 1 && es[0].name == "Verse" && es[0].parts.size() == 2 && ep.size() == 2, "a chosen scene comes, holding the parts that came");
        refused = false; try { e.importPerformance(folder.string(), {keys}, {99}); } catch(const std::exception&) { refused = true; }
        check(refused && e.parts().size() == 2, "a scene that is not there is refused");
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: importing a Performance, whole and in parts");
    return failures ? 1 : 0;
}

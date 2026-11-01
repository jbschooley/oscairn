// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021 step 1: a part with a split, three plugins, an internal user wire and two controls
// it owns, saved as a Sound and loaded into another Performance's part, renders sample for
// sample as the original part does. The wire crossing its edge is listed and not saved; a
// Performance control on the old part's control moves to the Sound's of the same name, one on
// the old plugins is flagged; the part keeps its seat; undo puts the old part back; a new part
// from the Sound; the library lists Sounds apart from Performances.
//   oscairn-sounds-test FIXTURE.clap DELAY.clap
#include "oscairn/session.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace oscairn;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 64;

    // The Keys part: a copy split, low and high layers in their lanes, joining into an echo, a
    // user wire from the low layer straight into the echo, and two controls of its own.
    uint32_t keys(Session& s, const char* fixture, const char* delay)
    {
        const auto low = s.add("Low", fixture, "org.kick.test.synth"), high = s.add("High", fixture, "org.kick.test.synth"), fx = s.add("Echo", delay);
        s.setParameter(low, 7, .25); s.setParameter(high, 7, .5);
        const auto part = s.addPart("Keys");
        const auto split = s.addSplit(part, 0);
        s.insertIntoPart(part, 0, low, {split, 0}); s.insertIntoPart(part, 0, high, {split, 1});
        s.insertIntoPart(part, 1, fx);
        NoteZone below; below.highKey = 59; NoteZone above; above.lowKey = 60;
        Lane a; a.zones = {below}; a.level = .5; s.setLane(part, split, 0, a);
        Lane b; b.zones = {above}; b.level = .8; s.setLane(part, split, 1, b);
        s.connectAudio(low, fx);
        s.setPartLevel(part, .7, false);
        s.commit();
        MacroTarget tone; tone.node = low; tone.param = 7; tone.start = .1; tone.end = .9;
        MacroTarget bright; bright.node = high; bright.param = 7;
        s.setMacroTargets(s.addMacro("Tone", part), {tone});
        s.setMacroTargets(s.addMacro("Bright", part), {bright});
        return part;
    }

    // Both sessions, the same notes, block by block; true when every sample matches and there is sound.
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
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s FIXTURE DELAY\n", argv[0]); return 2; }
    const char* fixture = argv[1]; const char* delay = argv[2];
    try
    {
        state_files::TemporaryDirectory home;
        ::setenv("OSCAIRN_DATA_HOME", home.path.c_str(), 1); // a library of its own

        // Saved from a Performance where a wire leaves the part for another part's echo.
        Session s(48000, frames);
        const auto part = keys(s, fixture, delay);
        const auto drums = s.addPart("Drums"), echo = s.add("Drums Echo", delay);
        s.insertIntoPart(drums, 0, echo); s.connectAudio(s.nodeId("High"), echo); s.commit();
        const auto saved = s.savePartAsSound(part, "Split Keys");
        check(saved.left.size() == 1 && saved.left[0] == "High to Drums Echo", "the crossing wire is listed: " + (saved.left.empty() ? std::string() : saved.left[0]));
        const auto sound = std::filesystem::path(home.path) / "Sounds/Split Keys.osound/sound.json";
        check(std::filesystem::is_regular_file(sound), "saved in the library's Sounds folder");
        const auto text = state_files::read(sound, 1 << 22);
        const std::string json(text.begin(), text.end());
        check(json.find("Drums") == std::string::npos && json.find("\"seat\"") == std::string::npos && json.find("\"level\":0.7") == std::string::npos,
              "nothing of the other part, and not the part's fader");

        // Into another Performance's part, which held something else and had controls on it.
        Session t(48000, frames);
        const auto old = t.add("Old", fixture, "org.kick.test.synth");
        const auto seat = t.addPart("Seat"); t.insertIntoPart(seat, 0, old); t.setPartLevel(seat, .7, false); t.commit();
        MacroTarget oldTone; oldTone.node = old; oldTone.param = 7;
        const auto toneOld = t.addMacro("Tone", seat); t.setMacroTargets(toneOld, {oldTone});
        MacroTarget onTone; onTone.kind = MacroTarget::Kind::Macro; onTone.macro = toneOld;
        const auto super = t.addMacro("Super"); t.setMacroTargets(super, {onTone});
        const auto other = t.addMacro("Other"); t.setMacroTargets(other, {oldTone, MacroTarget{MacroTarget::Kind::PartLevel, MacroTarget::Curve::Linear, 0, 0, seat}});
        const auto before = t.parts();
        const auto flagged = t.loadSound(seat, saved.id);
        check(std::find(flagged.begin(), flagged.end(), "Other: a target on the old plugins was removed") != flagged.end(), "a Performance control on the old plugins is flagged");
        const auto g = t.graphState();
        auto macro = [&](const std::string& name) { for(const auto& m : g.macros) if(m.name == name) return m; throw std::runtime_error("no control " + name); };
        check(macro("Super").targets.size() == 1 && macro("Super").targets[0].macro == macro("Tone").id && macro("Tone").id != toneOld && macro("Tone").targets[0].node == t.nodeId("Low"),
              "Super now drives the Sound's Tone");
        check(macro("Other").targets.size() == 1 && g.macros.size() == 4, "Other keeps its part level target; the old Tone went, the Sound's two came");
        const auto now = t.parts();
        check(now.size() == 1 && now[0].id == seat && now[0].name == "Split Keys" && now[0].level == .7 && now[0].splits.size() == 1 && now[0].chain.size() == 2,
              "the part keeps its seat and fader, with the Sound's chain");

        // Sample for sample as the original part.
        Session reference(48000, frames);
        keys(reference, fixture, delay);
        check(same(reference, t), "the loaded Sound renders as the part it was saved from");

        // Undo puts the old part back; redo the Sound.
        t.undo();
        check(t.parts()[0].chain == before[0].chain && t.parts()[0].name == "Seat", "undo puts the old part back");
        t.redo();
        check(t.parts()[0].name == "Split Keys" && t.parts()[0].splits.size() == 1, "and redo the Sound");

        // A new part from it, playing to Main.
        const auto added = t.partFromSound(saved.id);
        check(t.parts().size() == 2 && t.parts()[1].id == added && t.parts()[1].chain.size() == 2, "a new part from the Sound");

        // The library tells Sounds from Performances.
        s.savePerformanceToLibrary("Gig");
        Control::LibraryQuery q; q.kind = "sound";
        const auto sounds = s.libraryQuery(q);
        q.kind = "performance";
        const auto performances = s.libraryQuery(q);
        check(sounds.total == 1 && sounds.items[0].kind == "sound" && performances.total == 1 && performances.items[0].name == "Gig", "Sounds and Performances listed apart");
        bool refused = false; try { t.loadSound(seat, performances.items[0].id); } catch(const std::exception&) { refused = true; }
        check(refused, "a Performance is not loaded as a Sound");
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a part saved as a Sound and loaded into another renders the same; edges, controls, undo and the library");
    return failures ? 1 : 0;
}

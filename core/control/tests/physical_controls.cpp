// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 6: physical controls resolved in layers (overrides, the sound's own controls,
// role banks or its first parameters, the Performance's conventions, the effects bank), banks
// in fours paged on four- and eight-knob devices, overrides checked, undone, saved and loaded.
//   oscairn-physical-controls-test FIXTURE.clap DELAY.clap
#include "oscairn/physical_controls.h"
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include <array>
#include <cstdio>
#include <string>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 64;
    void run(Session& s) { std::array<float, frames> l{}, r{}; float* out[2]{l.data(), r.data()}; s.service(); s.process(nullptr, 0, out, 2, frames); }
    bool refused(const std::function<void()>& f) { try { f(); } catch(const std::invalid_argument&) { return true; } return false; }
    Assignment target(Assignment::Kind kind, uint32_t id) { Assignment a; a.kind = kind; a.id = id; return a; }
    Assignment parameter(NodeId node, uint32_t param) { Assignment a; a.kind = Assignment::Kind::Parameter; a.node = node; a.param = param; return a; }
    const ResolvedControl& find(const std::vector<ResolvedControl>& all, const std::string& control)
    {
        for(const auto& r : all) if(r.control == control) return r;
        throw std::runtime_error("no " + control);
    }

    void layers(const char* fixture, const char* delay, const fs::path& root)
    {
        Session s(48000, frames);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay), bass = s.add("bass", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys"), lead = s.addPart("Lead"), hall = s.addPart("Hall", {PartInput::Kind::Bus});
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(keys, 1, fx); s.insertIntoPart(lead, 0, bass); s.commit();
        const auto super = s.addMacro("Super"), swell = s.addMacro("Swell"), bright = s.addMacro("Brightness", lead);
        const auto verse = s.addScene("Verse");
        run(s);
        auto source = [&](std::function<std::string(NodeId, const Control::ParamInfo&)> roles = {}) {
            return assignmentSource(s.graphState(), s.assignments(), [&](NodeId n) { return s.parameters(n); }, roles);
        };

        // No part selected: the Performance's own controls.
        auto banks = knobBanks(source(), 0);
        check(banks.size() == 1 && banks[0].name == "Controls 1\xE2\x80\x93" "4" && banks[0].layer == AssignmentLayer::Convention &&
              banks[0].slots[0] == target(Assignment::Kind::Control, super) && banks[0].slots[1] == target(Assignment::Kind::Control, swell) &&
              banks[0].slots[2].kind == Assignment::Kind::None, "with no part selected, the knobs follow the Performance's controls");
        // A sound with controls of its own: they are its Assign bank, and nothing comes from its parameters.
        banks = knobBanks(source(), lead);
        check(banks.size() == 1 && banks[0].layer == AssignmentLayer::Sound && banks[0].slots[0] == target(Assignment::Kind::Control, bright) && banks[0].labels[0] == "Brightness",
              "a part's own controls are its Assign bank");
        // Nothing assigned and no roles declared: Assign from the instrument's first parameters, then
        // the effects bank with each effect's main control.
        banks = knobBanks(source(), keys);
        check(banks.size() == 2 && banks[0].name == "Assign 1\xE2\x80\x93" "4" && banks[0].layer == AssignmentLayer::Role &&
              banks[0].slots[0] == parameter(synth, 7) && banks[0].labels[0] == "synth: Gain" && banks[0].slots[1] == parameter(synth, 99) && banks[0].slots[2].kind == Assignment::Kind::None,
              "Assign from the sound's first parameters: " + banks[0].labels[0]);
        check(banks.size() == 2 && banks[1].layer == AssignmentLayer::Effects && banks[1].slots[0].node == fx && banks[1].slots[1].kind == Assignment::Kind::None,
              "the effects bank: the delay's main control");
        // Declared roles fill the fixed role banks; a role it lacks is blank, and no first parameters come.
        banks = knobBanks(source([&](NodeId n, const Control::ParamInfo& p) { return n == synth && p.id == 7 ? std::string("resonance") : n == synth && p.id == 99 ? std::string("release") : std::string(); }), keys);
        check(banks.size() == 3 && banks[0].name == "Filter" && banks[0].layer == AssignmentLayer::Role && banks[0].slots[0].kind == Assignment::Kind::None && banks[0].slots[1] == parameter(synth, 7) &&
              banks[1].name == "Amp EG" && banks[1].slots[3] == parameter(synth, 99) && banks[1].slots[0].kind == Assignment::Kind::None && banks[2].layer == AssignmentLayer::Effects,
              "declared roles fill Filter and Amp EG in their places");

        // A four-knob device pages through the banks; an eight-knob device shows two at once.
        const PhysicalControls kick{4, 3, 2}, eight{8, 0, 0};
        auto resolved = resolveControls(source(), kick, keys, 0);
        check(resolved.size() == 9 && find(resolved, "knob:1").target == parameter(synth, 7) && find(resolved, "knob:1").bank == "Assign 1\xE2\x80\x93" "4" &&
              find(resolved, "knob:3").layer == AssignmentLayer::None, "page 1: Assign");
        resolved = resolveControls(source(), kick, keys, 1);
        check(find(resolved, "knob:1").target.node == fx && find(resolved, "knob:1").layer == AssignmentLayer::Effects, "page 2: Effects");
        check(resolveControls(source(), kick, keys, 2)[0].target == parameter(synth, 7) && knobPages(source(), 4, keys) == 2 && knobPages(source(), 8, keys) == 1, "pages wrap");
        resolved = resolveControls(source(), eight, keys, 0);
        check(find(resolved, "knob:1").target == parameter(synth, 7) && find(resolved, "knob:5").target.node == fx, "eight knobs show two banks");
        // Conventions: sliders play part levels and buttons part mutes, parts before buses.
        resolved = resolveControls(source(), kick, 0, 0);
        check(find(resolved, "slider:1").target == target(Assignment::Kind::PartLevel, keys) && find(resolved, "slider:2").target == target(Assignment::Kind::PartLevel, lead) &&
              find(resolved, "slider:3").target == target(Assignment::Kind::PartLevel, hall) && find(resolved, "slider:3").label == "Hall level" &&
              find(resolved, "button:2").target == target(Assignment::Kind::PartMute, lead) && find(resolved, "button:1").layer == AssignmentLayer::Convention,
              "sliders play part levels and buttons mutes");

        // Overrides come first, on any control, and are checked.
        s.assign("knob:2", target(Assignment::Kind::Control, super));
        s.assign("slider:1", parameter(fx, s.parameters(fx).front().id));
        s.assign("button:2", target(Assignment::Kind::Scene, verse));
        resolved = resolveControls(source(), kick, keys, 0);
        check(find(resolved, "knob:2").target == target(Assignment::Kind::Control, super) && find(resolved, "knob:2").layer == AssignmentLayer::Override && find(resolved, "knob:2").label == "Super" &&
              find(resolved, "knob:1").layer == AssignmentLayer::Role, "an override takes its knob, the others keep their banks");
        check(find(resolved, "slider:1").target.node == fx && find(resolved, "button:2").label == "Verse" && find(resolved, "button:1").layer == AssignmentLayer::Convention, "on sliders and buttons too");
        check(refused([&] { s.assign("pedal:1", target(Assignment::Kind::Control, super)); }) && refused([&] { s.assign("knob:0", target(Assignment::Kind::Control, super)); }) &&
              refused([&] { s.assign("knob:1", target(Assignment::Kind::Control, 999)); }) && refused([&] { s.assign("knob:1", parameter(synth, 12345)); }) &&
              refused([&] { s.assign("knob:1", target(Assignment::Kind::PartLevel, 999)); }), "an unknown control or target is refused");
        auto stale = source(); stale.overrides.push_back({"knob:1", target(Assignment::Kind::Control, 999)});
        check(find(resolveControls(stale, kick, keys, 0), "knob:1").layer == AssignmentLayer::Role, "an override whose target is gone falls through to the defaults");

        // An override is an edit: undone and redone.
        run(s);
        s.assign("knob:3", target(Assignment::Kind::PartMute, lead)); run(s);
        check(s.history().undo == "Physical controls" && s.undo() && s.assignments().size() == 3, "undo takes back an override: " + s.history().undo);
        check(s.redo() && s.assignments().size() == 4, "and redo brings it back");
        s.assign("knob:3", {}); run(s);
        check(s.assignments().size() == 3, "assigning nothing removes an override");

        // Saved with the Performance by the control's identity, and loaded.
        const auto saved = root / "assigned";
        s.savePerformance(saved.string());
        Session t(48000, frames);
        t.loadPerformance(saved.string());
        const auto loaded = t.assignments();
        const auto names = t.graphState().names;
        auto named = [&](const AssignmentOverride& o) { return o.target.kind == Assignment::Kind::Parameter ? names.at(o.target.node) : std::string(); };
        check(loaded.size() == 3 && loaded[0].control == "knob:2" && loaded[0].target == target(Assignment::Kind::Control, super) &&
              loaded[1].control == "slider:1" && loaded[1].target.kind == Assignment::Kind::Parameter && named(loaded[1]) == "fx" &&
              loaded[2].control == "button:2" && loaded[2].target == target(Assignment::Kind::Scene, verse), "overrides round-trip with the Performance");
        // An override to a plugin since removed is not saved.
        s.removeFromPart(keys, fx); s.remove(fx); s.commit();
        s.savePerformance((root / "removed").string());
        Session u(48000, frames);
        u.loadPerformance((root / "removed").string());
        check(u.assignments().size() == 2 && u.assignments()[0].control == "knob:2", "an override to a removed plugin is left out");
    }
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s FIXTURE DELAY\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory temp;
        layers(argv[1], argv[2], temp.path);
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: physical controls resolve in layers, page in banks of four, and save");
    return failures ? 1 : 0;
}

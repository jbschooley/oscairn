// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011 step 6: physical controls resolved in layers (overrides, the sound's own controls,
// role banks or its first parameters, the Performance's conventions, the effects bank), in banks
// as wide as their group, on the KICK (four knobs paged), the MODX M (six screen knobs, four knobs
// in six rows, eight faders in three modes) and an eight-knob device; overrides checked, undone,
// saved and loaded.
// Roles declared through oscairn.ui and from names, filling the Basic Synth's role banks; screen
// knobs following an editor page.
//   oscairn-physical-controls-test FIXTURE.clap DELAY.clap BASIC_SYNTH.clap
#include "oscairn/physical_controls.h"
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include <array>
#include <cmath>
#include <map>
#include <tuple>
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
        Session s(48000, frames, {}, stereoIn);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay), bass = s.add("bass", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys"), lead = s.addPart("Lead"), mic = s.addPart("Mic", {PartInput::Kind::Audio, 0}), hall = s.addPart("Hall", {PartInput::Kind::Bus});
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(keys, 1, fx); s.insertIntoPart(lead, 0, bass); s.commit();
        const auto super = s.addMacro("Super"), swell = s.addMacro("Swell"), bright = s.addMacro("Brightness", lead);
        const auto verse = s.addScene("Verse");
        run(s);
        // Roles as if the fixture declared them.
        auto source = [&](std::function<std::string(NodeId, const Control::ParamInfo&)> roles = {}) {
            return assignmentSource(s.graphState(), s.assignments(), [&](NodeId n) {
                auto params = s.parameters(n);
                if(roles) for(auto& p : params) p.role = roleNamed(roles(n, p));
                return params;
            });
        };

        // No part selected: the Performance's own controls, in banks as wide as the group.
        auto banks = knobBanks(source(), 0, 4);
        check(banks.size() == 1 && banks[0].name == "Controls 1\xE2\x80\x93" "4" && banks[0].layer == AssignmentLayer::Convention && banks[0].slots.size() == 4 &&
              banks[0].slots[0] == target(Assignment::Kind::Control, super) && banks[0].slots[1] == target(Assignment::Kind::Control, swell) &&
              banks[0].slots[2].kind == Assignment::Kind::None, "with no part selected, the knobs follow the Performance's controls");
        check(knobBanks(source(), 0, 6)[0].name == "Controls 1\xE2\x80\x93" "6" && knobBanks(source(), 0, 6)[0].slots.size() == 6, "a group of six gets banks of six");
        // A sound with controls of its own: they are its Assign bank, and nothing comes from its parameters.
        banks = knobBanks(source(), lead, 4);
        check(banks.size() == 1 && banks[0].layer == AssignmentLayer::Sound && banks[0].slots[0] == target(Assignment::Kind::Control, bright) && banks[0].labels[0] == "Brightness",
              "a part's own controls are its Assign bank");
        // Nothing assigned and no roles declared: Assign from the instrument's first parameters, then
        // the effects bank with each effect's main control.
        banks = knobBanks(source(), keys, 4);
        check(banks.size() == 2 && banks[0].name == "Assign 1\xE2\x80\x93" "4" && banks[0].layer == AssignmentLayer::Role &&
              banks[0].slots[0] == parameter(synth, 7) && banks[0].labels[0] == "synth: Gain" && banks[0].slots[1] == parameter(synth, 99) && banks[0].slots[2].kind == Assignment::Kind::None,
              "Assign from the sound's first parameters: " + banks[0].labels[0]);
        check(banks.size() == 2 && banks[1].layer == AssignmentLayer::Effects && banks[1].slots[0].node == fx && banks[1].slots[1].kind == Assignment::Kind::None,
              "the effects bank: the delay's main control");
        {
            // An effect whose first parameter is stepped (an EQ's Mode) and that names no mix: its first continuous one.
            auto fxParams = s.parameters(fx);
            const auto steppedFirst = assignmentSource(s.graphState(), s.assignments(), [&](NodeId n) {
                auto params = s.parameters(n);
                if(n == fx) for(auto& p : params) { p.role = 0; p.name = "Param " + std::to_string(p.id); } // no mix, declared or guessed
                if(n == fx) params[0].stepped = true;
                return params;
            });
            check(fxParams.size() > 1 && knobBanks(steppedFirst, keys, 4)[1].slots[0] == parameter(fx, fxParams[1].id),
                  "the effects bank skips a stepped first parameter for the first continuous one");
        }
        // Declared roles fill the role banks, taking as many roles as the group is wide: Amp EG's
        // fifth and sixth roles reach a group of six; a role it lacks is blank.
        auto roles = [&](NodeId n, const Control::ParamInfo& p) { return n == synth && p.id == 7 ? std::string("resonance") : n == synth && p.id == 99 ? std::string("velocity") : std::string(); };
        banks = knobBanks(source(roles), keys, 4);
        check(banks.size() == 2 && banks[0].name == "Filter" && banks[0].slots[1] == parameter(synth, 7) && banks[0].slots[0].kind == Assignment::Kind::None && banks[1].layer == AssignmentLayer::Effects,
              "four wide: Filter, and Amp EG left out, its velocity past the fourth role");
        banks = knobBanks(source(roles), keys, 6);
        check(banks.size() == 3 && banks[0].name == "Filter" && banks[1].name == "Amp EG" && banks[1].slots.size() == 6 && banks[1].slots[5] == parameter(synth, 99) &&
              banks[1].slots[0].kind == Assignment::Kind::None && banks[2].layer == AssignmentLayer::Effects, "six wide: Amp EG takes its sixth role, velocity");

        // The KICK: four assignable knobs, P1 to P4, paged on the screen; fixed knobs, a tempo
        // wheel, pads, buttons and a touch strip play nothing assigned.
        using Role = ControlGroup::Role;
        const std::vector<ControlGroup> kick{{Role::Assign, "knobs", 4}, {Role::Fixed, "knobs", 3}, {Role::Fixed, "jog", 1}, {Role::Notes, "pads", 16}, {Role::Fixed, "buttons", 31}, {Role::Fixed, "touchstrip", 1}};
        auto resolved = resolveControls(source(), kick, keys, {0});
        check(resolved.size() == 4 && find(resolved, "knob:1").target == parameter(synth, 7) && find(resolved, "knob:1").bank == "Assign 1\xE2\x80\x93" "4" &&
              find(resolved, "knob:3").layer == AssignmentLayer::None, "KICK, page 1: Assign; only its four assignable knobs resolve");
        resolved = resolveControls(source(), kick, keys, {1});
        check(find(resolved, "knob:1").target.node == fx && find(resolved, "knob:1").layer == AssignmentLayer::Effects && groupPositions(source(), kick[0], keys) == 2, "KICK, page 2: Effects");
        check(resolveControls(source(), kick, keys, {2})[0].target == parameter(synth, 7), "pages wrap");

        // The MODX M: six screen knobs, four control knobs whose rows the panel picks, eight faders in three modes.
        const std::vector<ControlGroup> modx{
            {Role::Screen, "knobs", 6},
            {Role::Assign, "knobs", 4, {{"Assign 1", BankKind::Assign}, {"Assign 2", BankKind::Assign}, {"Tone", BankKind::Filter}, {"EG", BankKind::AmpEG}, {"EQ", BankKind::EQ}, {"Effects", BankKind::Effects}}},
            {Role::Assign, "faders", 8, {}, {"part", "element", "audio"}},
            {Role::Fixed, "knobs", 1}, {Role::Fixed, "buttons", 16}, {Role::Notes, "keys", 61}};
        resolved = resolveControls(source(), modx, keys, {0, 0, 0});
        check(resolved.size() == 6 + 4 + 8 && find(resolved, "screen:1").target == parameter(synth, 7) && find(resolved, "screen:1").bank == "Assign 1\xE2\x80\x93" "6" &&
              find(resolved, "screen:3").target.kind == Assignment::Kind::None, "MODX: six screen knobs play six-wide banks");
        check(find(resolved, "knob:1").target == parameter(synth, 7) && find(resolved, "knob:1").bank == "Assign 1" && groupPositions(source(), modx[1], keys) == 6, "row Assign 1");
        resolved = resolveControls(source(), modx, keys, {0, 1, 0});
        check(find(resolved, "knob:1").target.kind == Assignment::Kind::None && find(resolved, "knob:1").bank == "Assign 2", "row Assign 2: blank, two parameters fill one bank");
        resolved = resolveControls(source(roles), modx, keys, {0, 2, 0});
        check(find(resolved, "knob:2").target == parameter(synth, 7) && find(resolved, "knob:1").bank == "Tone", "row Tone: the Filter roles");
        resolved = resolveControls(source(), modx, keys, {0, 5, 0});
        check(find(resolved, "knob:1").target.node == fx && find(resolved, "knob:1").bank == "Effects", "row Effects");
        // Faders: part levels (parts, then buses), the part's Levels bank, the audio parts' levels.
        resolved = resolveControls(source(), modx, keys, {0, 0, 0});
        check(find(resolved, "fader:1").target == target(Assignment::Kind::PartLevel, keys) && find(resolved, "fader:3").target == target(Assignment::Kind::PartLevel, mic) &&
              find(resolved, "fader:4").target == target(Assignment::Kind::PartLevel, hall) && find(resolved, "fader:4").label == "Hall level" &&
              find(resolved, "fader:5").layer == AssignmentLayer::None && find(resolved, "fader:1").bank == "part" && groupPositions(source(), modx[2], keys) == 3, "fader mode part: part levels");
        resolved = resolveControls(source([&](NodeId n, const Control::ParamInfo& p) { return n == synth && p.id == 7 ? std::string("level1") : std::string(); }), modx, keys, {0, 0, 1});
        check(find(resolved, "fader:1").target == parameter(synth, 7) && find(resolved, "fader:2").target.kind == Assignment::Kind::None && find(resolved, "fader:1").bank == "element",
              "fader mode element: the sound's Levels roles");
        resolved = resolveControls(source(), modx, keys, {0, 0, 2});
        check(find(resolved, "fader:1").target == target(Assignment::Kind::PartLevel, mic) && find(resolved, "fader:2").target.kind == Assignment::Kind::None, "fader mode audio: parts playing audio inputs");

        // An eight-knob device: banks eight wide, two Assign banks of the first parameters in one.
        const std::vector<ControlGroup> eight{{Role::Assign, "encoders", 8}, {Role::Assign, "buttons", 4}};
        resolved = resolveControls(source(), eight, keys, {0, 0});
        check(find(resolved, "knob:1").target == parameter(synth, 7) && find(resolved, "knob:2").target == parameter(synth, 99) && find(resolved, "knob:1").bank == "Assign 1\xE2\x80\x93" "8" &&
              groupPositions(source(), eight[0], keys) == 2, "eight knobs: Assign 1–8, then Effects");
        check(find(resolved, "button:102").target == target(Assignment::Kind::PartMute, lead) && find(resolved, "button:101").layer == AssignmentLayer::Convention, "assignable buttons play part mutes");

        // Overrides come first, on any assignable control but a screen knob, and are checked.
        s.assign("knob:2", target(Assignment::Kind::Control, super));
        s.assign("fader:1", parameter(fx, s.parameters(fx).front().id));
        s.assign("button:102", target(Assignment::Kind::Scene, verse));
        resolved = resolveControls(source(), modx, keys, {0, 0, 0});
        check(find(resolved, "knob:2").target == target(Assignment::Kind::Control, super) && find(resolved, "knob:2").layer == AssignmentLayer::Override && find(resolved, "knob:2").label == "Super" &&
              find(resolved, "knob:1").layer == AssignmentLayer::Role && find(resolved, "screen:2").layer == AssignmentLayer::Role, "an override takes its knob, not the screen knob of the same number");
        check(find(resolved, "fader:1").target.node == fx && find(resolveControls(source(), eight, keys, {0, 0}), "button:102").label == "Verse", "on faders and buttons too");
        check(refused([&] { s.assign("slider:1", target(Assignment::Kind::Control, super)); }) && refused([&] { s.assign("knob:0", target(Assignment::Kind::Control, super)); }) &&
              refused([&] { s.assign("screen:1", target(Assignment::Kind::Control, super)); }) &&
              refused([&] { s.assign("knob:1", target(Assignment::Kind::Control, 999)); }) && refused([&] { s.assign("knob:1", parameter(synth, 12345)); }) &&
              refused([&] { s.assign("knob:1", target(Assignment::Kind::PartLevel, 999)); }), "an unknown control or target is refused");
        auto stale = source(); stale.overrides.push_back({"knob:1", target(Assignment::Kind::Control, 999)});
        check(find(resolveControls(stale, kick, keys, {0}), "knob:1").layer == AssignmentLayer::Role, "an override whose target is gone falls through to the defaults");

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
        Session t(48000, frames, {}, stereoIn);
        t.loadPerformance(saved.string());
        const auto loaded = t.assignments();
        const auto names = t.graphState().names;
        auto named = [&](const AssignmentOverride& o) { return o.target.kind == Assignment::Kind::Parameter ? names.at(o.target.node) : std::string(); };
        check(loaded.size() == 3 && loaded[0].control == "knob:2" && loaded[0].target == target(Assignment::Kind::Control, super) &&
              loaded[1].control == "fader:1" && loaded[1].target.kind == Assignment::Kind::Parameter && named(loaded[1]) == "fx" &&
              loaded[2].control == "button:102" && loaded[2].target == target(Assignment::Kind::Scene, verse), "overrides round-trip with the Performance");
        // An override to a plugin since removed is not saved.
        s.removeFromPart(keys, fx); s.remove(fx); s.commit();
        s.savePerformance((root / "removed").string());
        Session u(48000, frames, {}, stereoIn);
        u.loadPerformance((root / "removed").string());
        check(u.assignments().size() == 2 && u.assignments()[0].control == "knob:2", "an override to a removed plugin is left out");
    }

    // External controllers (RFC 0024): their groups join the device's, numbered on; their messages
    // played through the mapping service, with pickup, relative encodings, buttons and rows; the
    // mod wheel passed through, or the Sound's own assignment.
    void controllers(const char* fixture, const char* delay, const fs::path& root)
    {
        Session s(48000, frames, {}, stereoIn);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay), bass = s.add("bass", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys"), lead = s.addPart("Lead");
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(keys, 1, fx); s.insertIntoPart(lead, 0, bass); s.commit();
        const auto super = s.addMacro("Super"), bright = s.addMacro("Brightness", lead);
        const auto verse = s.addScene("Verse"); s.addScene("Chorus");
        run(s);
        auto source = [&] { return assignmentSource(s.graphState(), s.assignments(), [&](NodeId n) { return s.parameters(n); }); };
        using Role = ControlGroup::Role; using Message = ControlGroup::Midi::Message;
        auto midi = [](ControlGroup g, const std::string& device, Message m, std::vector<uint8_t> numbers, std::vector<Encoding> encodings = {}) {
            g.midi.device = device; g.midi.message = m; g.midi.numbers = std::move(numbers); g.midi.encodings = std::move(encodings); return g; };
        ControlGroup panel{Role::Assign, "encoders", 4}; panel.paged = true;
        ControlGroup knobs{Role::Assign, "knobs", 8}; knobs.paged = true;
        const auto abs = Encoding::Absolute, sm = Encoding::SignMagnitude;
        knobs = midi(knobs, "smk", Message::CC, {74, 71, 76, 77, 93, 18, 19, 16}, {abs, abs, abs, abs, abs, abs, sm, sm});
        ControlGroup bend{Role::Assign, "wheels", 1}; bend.functions = {"pitchBend"}; bend = midi(bend, "smk", Message::PitchBend, {});
        ControlGroup mod{Role::Assign, "wheels", 1}; mod.functions = {"modulation"}; mod = midi(mod, "smk", Message::CC, {1});
        ControlGroup scenes{Role::Assign, "buttons", 2}; scenes.scenes = true; scenes = midi(scenes, "smk", Message::CC, {25, 26});
        ControlGroup mutes = midi({Role::Assign, "buttons", 1}, "smk", Message::CC, {30});
        ControlGroup rows{Role::Assign, "knobs", 2, {{"Assign 1", BankKind::Assign}, {"Assign 2", BankKind::Assign}}}; rows = midi(rows, "modx", Message::CC, {17, 18, 21, 22});
        const std::vector<ControlGroup> groups{panel, knobs, midi({Role::Assign, "faders", 4}, "smk", Message::CC, {73, 75, 79, 72}), bend, mod, scenes, mutes, rows};

        auto resolved = resolveControls(source(), groups, keys, {});
        check(find(resolved, "knob:1").group == 0 && find(resolved, "knob:5").group == 1 && find(resolved, "knob:5").index == 0 && find(resolved, "knob:5").target == parameter(synth, 7) &&
              find(resolved, "knob:5").bank == "Assign 1\xE2\x80\x93" "8" && find(resolved, "knob:12").group == 1, "a controller's knobs are numbered after the panel's and play eight-wide banks");
        check(find(resolved, "fader:1").target == target(Assignment::Kind::PartLevel, keys) && find(resolved, "fader:2").target == target(Assignment::Kind::PartLevel, lead), "its faders play part levels");
        check(find(resolved, "wheel:1").function == "pitchBend" && find(resolved, "wheel:2").function == "modulation" && find(resolved, "wheel:2").target.kind == Assignment::Kind::None &&
              find(resolved, "wheel:2").parts.empty(), "its wheels pass pitch bend and modulation through");
        check(find(resolved, "button:101").target == target(Assignment::Kind::Scene, verse) && find(resolved, "button:101").label == "Verse" && find(resolved, "button:103").target == target(Assignment::Kind::PartMute, keys),
              "buttons play scenes, or part mutes");

        // Relative encodings, each scheme.
        check(relativeSteps(Encoding::TwosComplement, 1) == 1 && relativeSteps(Encoding::TwosComplement, 127) == -1 && relativeSteps(Encoding::TwosComplement, 65) == -63 &&
              relativeSteps(Encoding::BinaryOffset, 65) == 1 && relativeSteps(Encoding::BinaryOffset, 63) == -1 && relativeSteps(Encoding::BinaryOffset, 64) == 0 &&
              relativeSteps(Encoding::SignMagnitude, 1) == 1 && relativeSteps(Encoding::SignMagnitude, 65) == -1 && relativeSteps(Encoding::SignMagnitude, 69) == -5 &&
              relativeSteps(Encoding::Absolute, 100) == 0, "relative encodings decode");

        // The mapping service, with values kept here as a host would.
        std::map<std::tuple<int, NodeId, uint32_t, uint32_t>, double> values;
        auto key = [](const Assignment& a) { return std::tuple{int(a.kind), a.node, a.param, a.id}; };
        auto value = [&](const Assignment& a) { return values.count(key(a)) ? values[key(a)] : 0.0; };
        ControllerMapping mapping;
        GroupPositions positions;
        auto send = [&](const std::string& device, std::vector<uint8_t> bytes) {
            auto out = mapping.handle(source(), groups, keys, positions, device, 1, bytes.data(), bytes.size(), value);
            for(const auto& a : out) values[key(a.target)] = a.value;
            return out;
        };
        values[key(parameter(synth, 7))] = .5;
        check(send("smk", {0xB0, 74, 0}).empty() && send("smk", {0xB0, 74, 30}).empty(), "an absolute knob away from the value waits to pick up");
        auto out = send("smk", {0xB0, 74, 64});
        check(out.size() == 1 && out[0].control == "knob:5" && out[0].target == parameter(synth, 7) && std::abs(out[0].value - 64 / 127.0) < 1e-9, "it picks up at the value");
        out = send("smk", {0xB0, 74, 80});
        check(out.size() == 1 && std::abs(out[0].value - 80 / 127.0) < 1e-9, "and follows");
        values[key(parameter(synth, 7))] = .2; // moved from the screen
        check(send("smk", {0xB0, 74, 81}).empty(), "moved elsewhere, it waits again");
        out = send("smk", {0xB0, 74, 20});
        check(out.size() == 1 && std::abs(out[0].value - 20 / 127.0) < 1e-9, "and picks up crossing the value");
        check(send("other", {0xB0, 74, 21}).empty() && send("smk", {0xB1, 74, 21}).empty(), "another device or channel is not this knob");
        s.assign("knob:11", target(Assignment::Kind::Control, super)); run(s);
        values[key(target(Assignment::Kind::Control, super))] = .5;
        out = send("smk", {0xB0, 19, 3});
        check(out.size() == 1 && out[0].control == "knob:11" && std::abs(out[0].value - (.5 + 3 / 127.0)) < 1e-9, "a relative knob steps from the value, with no pickup");
        out = send("smk", {0xB0, 19, 66});
        check(out.size() == 1 && std::abs(out[0].value - (.5 + 1 / 127.0)) < 1e-9, "sign-magnitude 66 steps down two");
        // The SMK37 Elite's knobs: 1 to 6 on channel 6, 7 and 8 on channel 1 sending 69 for a step up and 1 for a step down.
        check(relativeSteps(Encoding::Steps, 69, {69, 1}) == 1 && relativeSteps(Encoding::Steps, 1, {69, 1}) == -1 && relativeSteps(Encoding::Steps, 64, {69, 1}) == 0,
              "explicit steps decode 69 and 1 to +1 and -1, and nothing else");
        {
            auto smk = midi(knobs, "smk", Message::CC, {48, 49, 50, 51, 52, 53, 38, 37}, {abs, abs, abs, abs, abs, abs, Encoding::Steps, Encoding::Steps});
            smk.midi.channels = {6, 6, 6, 6, 6, 6, 1, 1}; smk.midi.steps.assign(8, {69, 1});
            const std::vector<ControlGroup> mixed{panel, smk};
            ControllerMapping m; GroupPositions at;
            auto play = [&](std::vector<uint8_t> bytes) { return m.handle(source(), mixed, keys, at, "smk", 1, bytes.data(), bytes.size(), value); };
            values[key(target(Assignment::Kind::Control, super))] = .5;
            auto up = play({0xB0, 38, 69}), down = play({0xB0, 38, 1});
            check(up.size() == 1 && up[0].control == "knob:11" && std::abs(up[0].value - (.5 + 1 / 127.0)) < 1e-9 && down.size() == 1 && std::abs(down[0].value - (.5 - 1 / 127.0)) < 1e-9,
                  "knob 7 on channel 1: 69 steps up one, 1 down one");
            check(play({0xB5, 38, 69}).empty() && play({0xB0, 48, 64}).empty(), "a number on another control's channel is not that control");
            values[key(parameter(synth, 7))] = 64 / 127.0;
            out = play({0xB5, 48, 64});
            check(out.size() == 1 && out[0].control == "knob:5", "knob 1 on channel 6");
        }
        values[key(target(Assignment::Kind::PartLevel, keys))] = .7;
        out = send("smk", {0xB0, 73, 89});
        check(out.size() == 1 && out[0].target == target(Assignment::Kind::PartLevel, keys), "a fader picks up its part's level");
        out = send("smk", {0xB0, 25, 127});
        check(out.size() == 1 && out[0].target == target(Assignment::Kind::Scene, verse) && send("smk", {0xB0, 25, 0}).empty(), "a scene button selects its scene on the press");
        out = send("smk", {0xB0, 30, 127}); send("smk", {0xB0, 30, 0});
        const auto again = send("smk", {0xB0, 30, 127});
        check(out.size() == 1 && out[0].value == 1 && again.size() == 1 && again[0].value == 0, "a momentary button toggles a mute");
        out = send("modx", {0xB0, 21, 64});
        check(positions.size() == 8 && positions[7] == 1 && out.empty(), "a row's own numbers move its group to that row (blank here)");
        check(send("smk", {0xB0, 1, 64}).empty(), "the mod wheel with nothing assigned is passed through, not played here");

        // Aftertouch (channel pressure) and breath (CC 2) are sources like the wheels: passed
        // through, or assigned by identity. Assignable switches are button:1 up whatever other
        // buttons a device has; its scene and mute buttons count on from 101.
        {
            ControlGroup touch{Role::Assign, "aftertouch", 1}; touch.functions = {"aftertouch"}; touch = midi(touch, "smk", Message::Pressure, {});
            ControlGroup breath{Role::Assign, "breath", 1}; breath.functions = {"breath"}; breath = midi(breath, "smk", Message::CC, {2});
            ControlGroup switches{Role::Assign, "buttons", 2}; switches.switches = true; switches = midi(switches, "smk", Message::CC, {86, 87});
            const std::vector<ControlGroup> expressive{scenes, touch, breath, switches};
            auto r = resolveControls(source(), expressive, keys, {});
            check(find(r, "aftertouch:1").function == "aftertouch" && find(r, "aftertouch:1").target.kind == Assignment::Kind::None && find(r, "breath:1").function == "breath",
                  "aftertouch and breath pass through with nothing assigned");
            check(find(r, "button:1").group == 3 && find(r, "button:2").index == 1 && find(r, "button:1").target.kind == Assignment::Kind::None &&
                  find(r, "button:101").target == target(Assignment::Kind::Scene, verse), "switches are button:1 and button:2, nothing until assigned; scene buttons count from 101");
            const auto fewer = resolveControls(source(), {scenes}, keys, {});
            check(std::none_of(fewer.begin(), fewer.end(), [](const ResolvedControl& c) { return c.control == "button:1"; }) && find(fewer, "button:101").label == "Verse",
                  "a device without switches has no button:1; its scene buttons keep their numbers");
            s.assign("aftertouch:1", parameter(synth, 7)); s.assign("breath:1", target(Assignment::Kind::Control, super)); s.assign("button:1", target(Assignment::Kind::PartMute, lead)); run(s);
            check(find(resolveControls(source(), {scenes}, keys, {}), "button:101").target == target(Assignment::Kind::Scene, verse), "a switch override on a device without switches plays nothing else");
            ControllerMapping m; GroupPositions at;
            auto play = [&](std::vector<uint8_t> bytes) {
                auto o = m.handle(source(), expressive, keys, at, "smk", 1, bytes.data(), bytes.size(), value);
                for(const auto& a : o) values[key(a.target)] = a.value;
                return o;
            };
            values[key(parameter(synth, 7))] = 0;
            play({0xD0, 0});
            auto o = play({0xD0, 64});
            check(o.size() == 1 && o[0].control == "aftertouch:1" && o[0].target == parameter(synth, 7) && std::abs(o[0].value - 64 / 127.0) < 1e-9, "channel pressure, two bytes, plays aftertouch's target");
            check(play({0xD1, 70}).empty(), "pressure on another channel is not it");
            values[key(target(Assignment::Kind::Control, super))] = 64 / 127.0;
            o = play({0xB0, 2, 64}); o = play({0xB0, 2, 100});
            check(o.size() == 1 && o[0].control == "breath:1" && std::abs(o[0].value - 100 / 127.0) < 1e-9, "CC 2 plays breath's target");
            values[key(target(Assignment::Kind::PartMute, lead))] = 0;
            o = play({0xB0, 86, 127});
            check(o.size() == 1 && o[0].control == "button:1" && o[0].target == target(Assignment::Kind::PartMute, lead) && o[0].value == 1, "switch 1 toggles what it is assigned");
            s.savePerformance((root / "expressive").string());
            Session e(48000, frames, {}, stereoIn);
            e.loadPerformance((root / "expressive").string());
            size_t found = 0; for(const auto& a : e.assignments()) found += a.control == "aftertouch:1" || a.control == "breath:1" || a.control == "button:1";
            check(found == 3, "aftertouch, breath and switch overrides round-trip with the Performance");
            s.assign("aftertouch:1", {}); s.assign("breath:1", {}); s.assign("button:1", {}); run(s);
        }

        // Panic (all notes off) on a button: a group's default (a profile's `target: panic`) or a
        // switch the Performance assigns it to; on every press, momentary or latching; saved with
        // the Performance; and while a button plays it, the desktop's top bar leaves its own out.
        {
            ControlGroup switches{Role::Assign, "buttons", 2}; switches.switches = true; switches = midi(switches, "smk", Message::CC, {86, 87});
            ControlGroup panic{Role::Assign, "buttons", 1}; panic.panic = true; panic = midi(panic, "smk", Message::CC, {88});
            const std::vector<ControlGroup> both{switches, panic};
            auto r = resolveControls(source(), both, keys, {});
            check(find(r, "button:101").target.kind == Assignment::Kind::Panic && find(r, "button:101").label == "Panic" && find(r, "button:1").target.kind == Assignment::Kind::None,
                  "a group playing Panic by default; the switches nothing until assigned");
            check(panicOnHardware(both, {}) && !panicOnHardware({switches}, {}), "a Panic button is on the hardware with that group, not without it");
            bool refused = false; try { s.assign("knob:1", target(Assignment::Kind::Panic, 0)); } catch(const std::invalid_argument&) { refused = true; }
            check(refused, "a knob cannot play Panic");
            s.assign("button:2", target(Assignment::Kind::Panic, 0)); run(s);
            check(find(resolveControls(source(), {switches}, keys, {}), "button:2").target.kind == Assignment::Kind::Panic && panicOnHardware({switches}, s.assignments()) &&
                  !panicOnHardware({scenes}, s.assignments()), "a switch the Performance assigns Panic plays it; one the device lacks does not count");
            ControllerMapping m; GroupPositions at;
            auto play = [&](const std::vector<ControlGroup>& g, std::vector<uint8_t> bytes) { return m.handle(source(), g, keys, at, "smk", 1, bytes.data(), bytes.size(), value); };
            bool every = true;
            for(int i = 0; i < 2; ++i)
            {
                const auto on = play(both, {0xB0, 87, 127}), off = play(both, {0xB0, 87, 0});
                every = every && on.size() == 1 && on[0].target.kind == Assignment::Kind::Panic && on[0].control == "button:2" && off.empty();
            }
            check(every, "a momentary switch plays Panic on every press, not on the release");
            auto latching = both; latching[1].midi.latch = true;
            const auto on = play(latching, {0xB0, 88, 127}), off = play(latching, {0xB0, 88, 0});
            check(on.size() == 1 && off.size() == 1 && off[0].target.kind == Assignment::Kind::Panic, "a latching button plays it on each message, each a press");
            s.savePerformance((root / "panic").string());
            Session e(48000, frames, {}, stereoIn);
            e.loadPerformance((root / "panic").string());
            const auto saved = e.assignments();
            check(std::any_of(saved.begin(), saved.end(), [](const AssignmentOverride& a) { return a.control == "button:2" && a.target.kind == Assignment::Kind::Panic; }),
                  "a Panic override round-trips with the Performance");
            s.assign("button:2", {}); run(s);
        }

        // A footswitch is a momentary button (RFC 0011): its scene or mute fires on each press,
        // not on the release.
        {
            ControlGroup pedal = midi({Role::Assign, "footswitches", 1}, "smk", Message::CC, {64});
            s.assign("footswitch:1", target(Assignment::Kind::Scene, verse)); run(s);
            values[key(target(Assignment::Kind::Scene, verse))] = 0; // as if Verse were not playing: an absolute CC would wait to pick it up
            ControllerMapping m; GroupPositions at;
            auto play = [&](std::vector<uint8_t> bytes) {
                auto o = m.handle(source(), {pedal}, keys, at, "smk", 1, bytes.data(), bytes.size(), value);
                for(const auto& a : o) values[key(a.target)] = a.value;
                return o;
            };
            auto o = play({0xB0, 64, 127});
            check(o.size() == 1 && o[0].control == "footswitch:1" && o[0].target == target(Assignment::Kind::Scene, verse) && o[0].value == 1, "a footswitch selects its scene on the press");
            check(play({0xB0, 64, 0}).empty(), "the release plays nothing");
            o = play({0xB0, 64, 127});
            check(o.size() == 1 && o[0].value == 1, "every press selects it again");
            s.assign("footswitch:1", target(Assignment::Kind::PartMute, lead)); run(s);
            values[key(target(Assignment::Kind::PartMute, lead))] = 0;
            o = play({0xB0, 64, 127});
            check(o.size() == 1 && o[0].target == target(Assignment::Kind::PartMute, lead) && o[0].value == 1, "a mute toggles on the press");
            check(play({0xB0, 64, 0}).empty(), "not on the release");
            o = play({0xB0, 64, 127});
            check(o.size() == 1 && o[0].value == 0, "and back off on the next press");
            // A latching footswitch keeps its state: down mutes, up unmutes.
            pedal.midi.latch = true; m = ControllerMapping{};
            values[key(target(Assignment::Kind::PartMute, lead))] = 0;
            o = play({0xB0, 64, 127});
            check(o.size() == 1 && o[0].target == target(Assignment::Kind::PartMute, lead) && o[0].value == 1, "a latching footswitch mutes while down");
            o = play({0xB0, 64, 0});
            check(o.size() == 1 && o[0].value == 0, "and unmutes when let up");
            o = play({0xB0, 64, 127});
            check(o.size() == 1 && o[0].value == 1, "every time");
            // A latching pedal sends 127 and 0 on alternate presses: each one selects its scene.
            s.assign("footswitch:1", target(Assignment::Kind::Scene, verse)); run(s); m = ControllerMapping{};
            bool every = true;
            for(const uint8_t v : {127, 0, 127, 0}) { o = play({0xB0, 64, v}); every = every && o.size() == 1 && o[0].target == target(Assignment::Kind::Scene, verse) && o[0].value == 1; }
            check(every, "a latching footswitch selects its scene on every press");
            s.assign("footswitch:1", {}); run(s);
        }

        // The Sound's own assignments: the mod wheel to a parameter of its own, played in its part;
        // a knob's when its part is selected. Only its own plugins and controls.
        s.assign("wheel:2", parameter(bass, 7), lead);
        s.assign("knob:5", target(Assignment::Kind::Control, bright), lead); run(s);
        check(refused([&] { s.assign("wheel:2", parameter(synth, 7), lead); }) && refused([&] { s.assign("wheel:2", target(Assignment::Kind::PartLevel, keys), lead); }) &&
              refused([&] { s.assign("wheel:2", target(Assignment::Kind::Control, super), lead); }), "a Sound's assignment plays only its own plugins and controls");
        resolved = resolveControls(source(), groups, keys, {});
        check(find(resolved, "wheel:2").parts.size() == 1 && find(resolved, "wheel:2").parts[0] == std::pair{lead, parameter(bass, 7)} && find(resolved, "wheel:2").target.kind == Assignment::Kind::None &&
              find(resolved, "knob:5").layer != AssignmentLayer::Sound, "the wheel plays the Lead's assignment in the Lead; the knob is not the Lead's when Keys is selected");
        const auto selected = resolveControls(source(), groups, lead, {});
        check(find(selected, "knob:5").layer == AssignmentLayer::Sound && find(selected, "knob:5").label == "Brightness", "with the Lead selected, its knob assignment plays");
        values[key(parameter(bass, 7))] = .5;
        out = send("smk", {0xB0, 1, 64});
        check(out.size() == 1 && out[0].target == parameter(bass, 7), "moving the mod wheel plays the Sound's target");
        s.assign("wheel:2", target(Assignment::Kind::Control, super)); run(s);
        check(find(resolveControls(source(), groups, keys, {}), "wheel:2").target == target(Assignment::Kind::Control, super), "a Performance override plays in place of every part's");

        // Saved with the Performance, each with its part.
        s.savePerformance((root / "controllers").string());
        Session t(48000, frames, {}, stereoIn);
        t.loadPerformance((root / "controllers").string());
        size_t sounds = 0; for(const auto& o : t.assignments()) sounds += o.part == lead;
        check(t.assignments().size() == 4 && sounds == 2, "a part's own assignments round-trip with the Performance");
        (void)fx;
    }

    // Roles: declared through oscairn.ui (the Basic Synth's and the Delay's), else from names, and
    // the role banks they fill, a role the plugin lacks left blank.
    void roles(const char* fixture, const char* delay, const char* basicSynth)
    {
        check(roleFromName("Cutoff") == "cutoff" && roleFromName("RESONANCE") == "resonance" && roleFromName("Attack") == "attack" &&
              roleFromName("Attack", "Filter envelope") == "filterAttack" && roleFromName("Wet") == "mix" && roleFromName("Gain").empty() && roleFromName("Cutoff 2").empty(),
              "roles from names: cutoff, resonance, attack (a filter envelope's in its module), wet; none for others");
        Session s(48000, frames, {}, stereoIn);
        const auto synth = s.add("synth", basicSynth), fx = s.add("fx", delay), plain = s.add("plain", fixture, "org.kick.test.synth");
        const auto keys = s.addPart("Keys"), other = s.addPart("Other");
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(keys, 1, fx); s.insertIntoPart(other, 0, plain); s.commit();
        run(s);
        auto info = [&](NodeId n, const char* name) { for(const auto& p : s.parameters(n)) if(p.name == name) return p; throw std::runtime_error(std::string("no ") + name); };
        check(info(synth, "Brightness").role == OSCAIRN_ROLE_CUTOFF && info(synth, "Level").role == OSCAIRN_ROLE_LEVEL && info(synth, "Detune").role == 0 && info(fx, "Wet").role == OSCAIRN_ROLE_MIX,
              "the Basic Synth and the Delay declare their roles");
        auto source = [&](std::function<void(NodeId, std::vector<Control::ParamInfo>&)> change = {}) {
            return assignmentSource(s.graphState(), s.assignments(), [&](NodeId n) { auto p = s.parameters(n); if(change) change(n, p); return p; });
        };
        // Four wide: Filter with its cutoff, the rest blank (no resonance, envelope or key tracking);
        // Amp EG's only role, level, is its eighth, so it is left out; the Delay's Wet in Effects.
        auto banks = knobBanks(source(), keys, 4);
        check(banks.size() == 2 && banks[0].name == "Filter" && banks[0].layer == AssignmentLayer::Role && banks[0].slots[0] == parameter(synth, info(synth, "Brightness").id) &&
              banks[0].labels[0] == "synth: Brightness" && banks[0].slots[1].kind == Assignment::Kind::None && banks[0].slots[3].kind == Assignment::Kind::None &&
              banks[1].layer == AssignmentLayer::Effects && banks[1].slots[0] == parameter(fx, info(fx, "Wet").id),
              "Basic Synth, four wide: Filter (cutoff, then blanks), and the Delay's Wet: " + (banks.empty() ? std::string() : banks[0].name));
        banks = knobBanks(source(), keys, 8);
        check(banks.size() == 3 && banks[1].name == "Amp EG" && banks[1].slots[7] == parameter(synth, info(synth, "Level").id) && banks[1].slots[0].kind == Assignment::Kind::None,
              "eight wide: Amp EG with its level, attack to ampKeyTracking blank");
        // A plugin declaring no roles gets them from its names; a plugin declaring any keeps only those.
        auto renamed = [&](NodeId n, std::vector<Control::ParamInfo>& p) { if(n == plain) p[0].name = "Cutoff"; };
        banks = knobBanks(source(renamed), other, 4);
        check(!banks.empty() && banks[0].name == "Filter" && banks[0].slots[0] == parameter(plain, 7), "from names: the fixture's \"Cutoff\" fills Filter");
        banks = knobBanks(source([&](NodeId n, std::vector<Control::ParamInfo>& p) { renamed(n, p); if(n == plain) p[1].role = OSCAIRN_ROLE_RESONANCE; }), other, 4);
        check(!banks.empty() && banks[0].name == "Filter" && banks[0].slots[0].kind == Assignment::Kind::None && banks[0].slots[1] == parameter(plain, 99),
              "declared roles come first: a declared resonance, and the name \"Cutoff\" ignored");
        banks = knobBanks(source(), other, 4);
        check(!banks.empty() && banks[0].name == "Assign 1\xE2\x80\x93" "4", "no roles either way: Assign from its first parameters");
    }

    // Screen knobs (RFC 0011): Play's knob row, or an editor page's top parameters while one shows.
    void screen(const char* fixture, const char* delay)
    {
        Session s(48000, frames, {}, stereoIn);
        const auto synth = s.add("synth", fixture, "org.kick.test.synth"), fx = s.add("fx", delay);
        const auto keys = s.addPart("Keys");
        s.insertIntoPart(keys, 0, synth); s.insertIntoPart(keys, 1, fx); s.commit();
        run(s);
        auto source = assignmentSource(s.graphState(), s.assignments(), [&](NodeId n) { return s.parameters(n); });
        using Role = ControlGroup::Role;
        const std::vector<ControlGroup> modx{{Role::Screen, "knobs", 6}, {Role::Assign, "knobs", 4, {{"Assign 1", BankKind::Assign}}}};
        auto resolved = resolveControls(source, modx, keys, {1, 0});
        check(find(resolved, "screen:1").target == parameter(fx, 3) && find(resolved, "screen:1").bank == "Effects 1\xE2\x80\x93" "6" && groupPositions(source, modx[0], keys) == 2,
              "in Play, screen knobs play the knob row's bank");
        // The Delay's editor page, its parameters by priority: the screen knobs take the first six.
        source.screen = {"Main", {parameter(fx, 1), parameter(fx, 3), parameter(fx, 2)}};
        resolved = resolveControls(source, modx, keys, {1, 0});
        check(find(resolved, "screen:1").target == parameter(fx, 1) && find(resolved, "screen:1").layer == AssignmentLayer::Editor && find(resolved, "screen:1").bank == "Main" &&
              find(resolved, "screen:1").label == "fx: Delay beats" && find(resolved, "screen:3").target == parameter(fx, 2) && find(resolved, "screen:4").target.kind == Assignment::Kind::None &&
              groupPositions(source, modx[0], keys) == 1, "an editor page shown: screen knobs play its top parameters");
        check(find(resolved, "knob:1").target == parameter(synth, 7) && find(resolved, "knob:1").bank == "Assign 1", "assign knobs keep their banks");
        source.screen = {};
        check(find(resolveControls(source, modx, keys, {1, 0}), "screen:1").bank == "Effects 1\xE2\x80\x93" "6", "back in Play, the knob row again");
    }
}

namespace
{
    // A swept wheel sends a CC every millisecond or so: a batch keeps each continuous target's last
    // value only, and mutes and scenes in order.
    void coalesced()
    {
        using A = ControllerMapping::Action; using K = Assignment::Kind;
        const Assignment control{K::Control, 0, 0, 1}, mute{K::PartMute, 0, 0, 2}, scene{K::Scene, 0, 0, 3}, level{K::PartLevel, 0, 0, 4};
        std::vector<A> sweep;
        for(int i = 0; i <= 100; ++i) sweep.push_back({"wheel:1", control, i / 100.0});
        auto kept = ControllerMapping::latest(sweep);
        check(kept.size() == 1 && kept[0].value == 1, "a sweep of 101 moves applies once, at its last value");
        kept = ControllerMapping::latest({{"", control, .1}, {"", mute, 1}, {"", control, .5}, {"", scene, 1}, {"", mute, 0}, {"", control, .9}, {"", level, .3}, {"", level, .7}});
        check(kept.size() == 5 && kept[0].target == mute && kept[0].value == 1 && kept[1].target == scene && kept[2].target == mute && kept[2].value == 0
              && kept[3].target == control && kept[3].value == .9 && kept[4].target == level && kept[4].value == .7,
              "mutes and scenes keep every press in order; each control and level keeps its last value");
    }
}

int main(int argc, char** argv)
{
    if(argc < 4) { std::fprintf(stderr, "usage: %s FIXTURE DELAY BASIC_SYNTH\n", argv[0]); return 2; }
    try
    {
        state_files::TemporaryDirectory temp;
        layers(argv[1], argv[2], temp.path);
        controllers(argv[1], argv[2], temp.path);
        roles(argv[1], argv[2], argv[3]);
        screen(argv[1], argv[2]);
        coalesced();
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: physical controls resolve in layers and groups, by page, row and mode, and save");
    return failures ? 1 : 0;
}

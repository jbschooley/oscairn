// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/control.h" // and control_groups.h

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oscairn
{
    // RFC 0011's physical controls: what every knob, fader and button plays, so a new
    // Performance works with no programming. A device's controls come in groups (its profile,
    // RFC 0024), each with a role: notes (keys, pads), screen (playing what the screen shows),
    // assign (playing banks), fixed (a dedicated function: volume, tempo). Assignments resolve in
    // layers, each filling only what the layers above it left empty:
    //   1. Performance overrides, by the control's logical identity ("knob:1", "fader:3");
    //   2. the sound's own assignments: the selected part's controls, as Assign banks;
    //   3. role banks (Filter, Amp EG, EQ, Levels) from the roles its plugins declare (oscairn.ui),
    //      or for a plugin declaring none, the roles its parameters' names suggest ("Cutoff"); with
    //      no roles either way and no controls of its own, Assign banks from its first parameters;
    //   4. Performance conventions: faders play part levels (in their part mode), buttons part
    //      mutes, and with no part selected the knobs follow the Performance's own controls;
    //   5. the effects bank: the main control of each effect in the selected part, in chain order.
    // A bank is as wide as the group playing it: a group of six gets banks of six. Screen knobs
    // play what the screen shows instead: Play's knob row, or an editor page's top parameters.
    enum class AssignmentLayer { None, Override, Sound, Role, Convention, Effects, Editor };

    // A parameter as resolution sees it: its role (empty: none declared) and oscairn.ui priority
    // (1 first; 0 none, after the numbered ones).
    struct AssignableParam { uint32_t id = 0; std::string name, role; uint32_t priority = 0; bool stepped = false; }; // stepped: CLAP_PARAM_IS_STEPPED
    struct AssignableNode { NodeId node = 0; std::string name; bool effect = false, instrument = false; std::vector<AssignableParam> params; }; // effect: audio in and out; instrument: audio out only

    // What resolution reads: the Performance's parts, controls, scenes and overrides, each part's
    // nodes with their parameters, and the editor page shown (none: Play shows, and screen knobs
    // play its knob row).
    struct AssignmentSource
    {
        std::vector<Part> parts;
        std::vector<Macro> controls;
        std::vector<std::pair<uint32_t, std::string>> scenes;
        std::map<NodeId, AssignableNode> nodes;
        std::vector<AssignmentOverride> overrides;
        ScreenPage screen;
    };
    // From a graph state; `parameters` gives a node's parameters (Control::parameters, or a cache
    // of it), with the roles its plugin declares; a plugin declaring none gets roles from names.
    AssignmentSource assignmentSource(const Control::GraphState&, const std::vector<AssignmentOverride>& overrides,
                                      const std::function<std::vector<Control::ParamInfo>(NodeId)>& parameters);
    // The role a parameter's name suggests, for plugins that declare none: "Cutoff", "Resonance",
    // "Attack" (a filter envelope's stages in a module named for the filter), "Wet"; empty: none.
    std::string roleFromName(const std::string& name, const std::string& module = {});

    // The role banks, the same on every instrument: each role in order, longer than any group
    // needs, so a group of any width takes the first as many roles and fills its row.
    struct RoleBank { BankKind kind; const char* name; std::array<const char*, 8> roles; };
    inline constexpr std::array<RoleBank, 4> roleBanks{{
        {BankKind::Filter, "Filter", {"cutoff", "resonance", "filterEnvelope", "keyTracking", "filterAttack", "filterDecay", "filterSustain", "filterRelease"}},
        {BankKind::AmpEG, "Amp EG", {"attack", "decay", "sustain", "release", "hold", "velocity", "ampKeyTracking", "level"}},
        {BankKind::EQ, "EQ", {"lowGain", "midGain", "highGain", "midFrequency", "lowFrequency", "highFrequency", "midQ", "eqOutput"}},
        {BankKind::Levels, "Levels", {"level1", "level2", "level3", "level4", "level5", "level6", "level7", "level8"}}, // elements, operators, oscillators
    }};
    inline constexpr const char* mixRole = "mix"; // an effect's main control, when it declares one

    struct Bank
    {
        std::string name;
        BankKind kind = BankKind::Assign;
        AssignmentLayer layer = AssignmentLayer::None;
        std::vector<Assignment> slots;   // as many as the group is wide; None: blank
        std::vector<std::string> labels; // what each slot plays, by name
    };
    // The banks a group `width` wide pages through: the part's (Assign, roles with any role
    // declared, effects), or for part 0 the Performance's own controls.
    std::vector<Bank> knobBanks(const AssignmentSource&, uint32_t part, uint32_t width);
    // The bank of one kind (the `index`th Assign or Effects bank), blank and named for its kind
    // when there is none: what a hardware row shows.
    Bank bankOf(const AssignmentSource&, uint32_t part, uint32_t width, BankKind kind, uint32_t index);

    struct ResolvedControl
    {
        std::string control;          // "knob:1"; "screen:1" for a screen knob, which is never overridden and plays the editor page shown, else Play's banks
        Assignment target;            // None: nothing
        AssignmentLayer layer = AssignmentLayer::None;
        std::string label, bank;      // what it plays, and the bank (or row, or mode) it is in
        size_t group = 0; uint32_t index = 0; // its group, in the order given, and its place there
        std::string function;         // a standard controller passed to every part when nothing is assigned ("modulation")
        std::vector<std::pair<uint32_t, Assignment>> parts; // a control with a function: what each part's Sound assigns it to
    };
    // Every assignable control of the device (screen and assign groups, faders, assign buttons)
    // with what it plays now. Controls are numbered by identity across groups: assign knobs and
    // encoders "knob:N", faders "fader:N", wheels "wheel:N", touch strips "strip:N", pedals
    // "pedal:N", aftertouch "aftertouch:N" and breath controllers "breath:N". Buttons: assignable
    // switches are "button:1" up, the same on every device, and the other assign buttons (scenes,
    // part mutes) "button:101" up (firstConventionButton); an override for a switch the device
    // lacks plays nothing and is kept. An override whose target is gone falls through to the defaults.
    // Overrides with a part are that part's Sound's own (layer 2): they play when the part is
    // selected, and for a control with a function, in that part whenever it moves.
    inline constexpr uint32_t firstConventionButton = 101;
    std::vector<ResolvedControl> resolveControls(const AssignmentSource&, const std::vector<ControlGroup>&, uint32_t part, const GroupPositions& = {});
    // How many positions a group has: its pages, rows or modes (1 for the rest).
    uint32_t groupPositions(const AssignmentSource&, const ControlGroup&, uint32_t part);
    // RFC 0011's controller-mapping service: a MIDI message from a controller's port to values
    // for what the control it comes from plays, 0 to 1 across each target's range. Absolute
    // controls pick up (soft takeover); relative ones step from the current value; buttons
    // toggle, latch or select. Pickup is per control. Not thread-safe: one caller.
    int relativeSteps(Encoding, uint8_t value, std::array<uint8_t, 2> steps = {}); // the signed step a relative value sends; Steps: 1, -1 or 0
    // The control a controller's message comes from: its group, its place there and, for a group
    // of rows sending a number per row, the row (-1: none); nothing when no control sends it.
    struct MidiControl { size_t group = 0; uint32_t index = 0; int row = -1; };
    std::optional<MidiControl> midiControl(const std::vector<ControlGroup>&, const std::string& device, uint32_t port, const uint8_t* bytes, size_t size);
    class ControllerMapping
    {
    public:
        struct Action { std::string control; Assignment target; double value = 0; };
        // `positions` gains the row a message names; `value` reads a target's value, 0 to 1.
        std::vector<Action> handle(const AssignmentSource&, const std::vector<ControlGroup>&, uint32_t part, GroupPositions& positions,
                                   const std::string& device, uint32_t port, const uint8_t* bytes, size_t size,
                                   const std::function<double(const Assignment&)>& value);
        // A batch of actions to apply, with each continuous target (a parameter, control or part
        // level) set once, to its last value, where that last value came; mutes and scenes keep every
        // action in order. A swept wheel sends a CC a millisecond: applying each in turn falls behind.
        static std::vector<Action> latest(std::vector<Action> actions);
    private:
        struct Pickup { Assignment target; bool caught = false; double knob = -1; std::array<double, 32> sent{}; size_t sends = 0; };
        std::map<std::string, Pickup> pickup_;
    };
    // A target's name as the views show it ("Piano level", "Basic Synth: Cutoff"); empty when it is gone.
    std::string assignmentName(const AssignmentSource&, const Assignment&);
}

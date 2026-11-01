// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/control.h"

#include <array>
#include <functional>
#include <map>
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
    //   3. role banks (Filter, Amp EG, EQ, Levels) from the roles its plugins declare, or, when
    //      they declare none and the sound has no controls, Assign banks from its first parameters;
    //   4. Performance conventions: faders play part levels (in their part mode), buttons part
    //      mutes, and with no part selected the knobs follow the Performance's own controls;
    //   5. the effects bank: the main control of each effect in the selected part, in chain order.
    // A bank is as wide as the group playing it: a group of six gets banks of six.
    enum class AssignmentLayer { None, Override, Sound, Role, Convention, Effects };

    // A parameter as resolution sees it: its role (empty: none declared) and oscairn.ui priority
    // (1 first; 0 none, after the numbered ones).
    struct AssignableParam { uint32_t id = 0; std::string name, role; uint32_t priority = 0; };
    struct AssignableNode { NodeId node = 0; std::string name; bool effect = false, instrument = false; std::vector<AssignableParam> params; }; // effect: audio in and out; instrument: audio out only

    // What resolution reads: the Performance's parts, controls, scenes and overrides, and each
    // part's nodes with their parameters.
    struct AssignmentSource
    {
        std::vector<Part> parts;
        std::vector<Macro> controls;
        std::vector<std::pair<uint32_t, std::string>> scenes;
        std::map<NodeId, AssignableNode> nodes;
        std::vector<AssignmentOverride> overrides;
    };
    // From a graph state; `parameters` gives a node's parameters (Control::parameters, or a cache
    // of it), and `roles` a parameter's declared role (none yet: plugins have no way to declare one).
    AssignmentSource assignmentSource(const Control::GraphState&, const std::vector<AssignmentOverride>& overrides,
                                      const std::function<std::vector<Control::ParamInfo>(NodeId)>& parameters,
                                      const std::function<std::string(NodeId, const Control::ParamInfo&)>& roles = {});

    // The kinds of bank: a part's Assign banks (its controls, or its first parameters), the role
    // banks, and its effects. With no part selected, Assign banks are the Performance's controls.
    enum class BankKind { Assign, Filter, AmpEG, EQ, Levels, Effects };
    inline constexpr std::array<const char*, 6> bankKinds{"assign", "filter", "ampEG", "eq", "levels", "effects"}; // as profiles name rows
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

    // A group of a device's controls (its profile's): kind (knobs, encoders, faders, buttons,
    // wheels, touchstrip, jog, pedal, ...), count and role. An assign group either has hardware
    // rows, each a bank its row button picks directly, or is paged (by a button or the screen).
    // Faders may have modes: part (part levels), element (the part's Levels bank) and audio (the
    // levels of parts playing audio inputs).
    struct ControlGroup
    {
        enum class Role { Notes, Screen, Assign, Fixed } role = Role::Assign;
        std::string kind;
        uint32_t count = 0;
        struct Row { std::string name; BankKind bank = BankKind::Assign; };
        std::vector<Row> rows;
        std::vector<std::string> modes;
    };
    // Where each group is now: its page, its row, or its mode, by group, in the device's order.
    using GroupPositions = std::vector<uint32_t>;
    struct ResolvedControl
    {
        std::string control;          // "knob:1"; "screen:1" for a screen knob, which is never overridden
        Assignment target;            // None: nothing
        AssignmentLayer layer = AssignmentLayer::None;
        std::string label, bank;      // what it plays, and the bank (or row, or mode) it is in
    };
    // Every assignable control of the device (screen and assign groups, faders, assign buttons)
    // with what it plays now. Controls are numbered by identity across groups: assign knobs and
    // encoders "knob:N", faders "fader:N", buttons "button:N", wheels "wheel:N", touch strips
    // "strip:N", pedals "pedal:N". An override whose target is gone falls through to the defaults.
    std::vector<ResolvedControl> resolveControls(const AssignmentSource&, const std::vector<ControlGroup>&, uint32_t part, const GroupPositions& = {});
    // How many positions a group has: its pages, rows or modes (1 for the rest).
    uint32_t groupPositions(const AssignmentSource&, const ControlGroup&, uint32_t part);
    // A target's name as the views show it ("Piano level", "Basic Synth: Cutoff"); empty when it is gone.
    std::string assignmentName(const AssignmentSource&, const Assignment&);
}

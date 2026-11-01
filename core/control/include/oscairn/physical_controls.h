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
    // RFC 0011's physical controls: what every knob, slider and button plays, so a new
    // Performance works with no programming. Assignments resolve in layers, each filling only
    // what the layers above it left empty:
    //   1. Performance overrides, by the control's logical identity ("knob:1");
    //   2. the sound's own assignments: the selected part's controls, as Assign banks;
    //   3. fixed role banks from the roles its plugins declare (Filter, Amp EG), or, when they
    //      declare none and the sound has no controls, Assign banks from its first parameters;
    //   4. Performance conventions: sliders play part levels, buttons part mutes, and with no
    //      part selected the knobs follow the Performance's own controls;
    //   5. the effects bank: the main control of each effect in the selected part, in chain order.
    // Banks come in fours; a four-knob device pages through them, an eight-knob device shows two.
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

    // The fixed role banks, the same on every instrument: their names and the roles in order.
    struct RoleBank { const char* name; std::array<const char*, 4> roles; };
    inline constexpr std::array<RoleBank, 2> roleBanks{{{"Filter", {"cutoff", "resonance", "filterEnvelope", "keyTracking"}},
                                                         {"Amp EG", {"attack", "decay", "sustain", "release"}}}};
    inline constexpr const char* mixRole = "mix"; // an effect's main control, when it declares one

    struct Bank
    {
        std::string name;
        AssignmentLayer layer = AssignmentLayer::None;
        std::array<Assignment, 4> slots{};  // None: blank (a role the sound lacks, or past the last control)
        std::array<std::string, 4> labels{}; // what each slot plays, by name
    };
    // The banks the knobs page through: the part's (Assign, roles, effects), or for part 0 the
    // Performance's own controls.
    std::vector<Bank> knobBanks(const AssignmentSource&, uint32_t part);

    // A device's assignable controls (its profile's knobs, sliders and buttons).
    struct PhysicalControls { uint32_t knobs = 0, sliders = 0, buttons = 0; };
    struct ResolvedControl
    {
        std::string control;          // "knob:1"
        Assignment target;            // None: nothing
        AssignmentLayer layer = AssignmentLayer::None;
        std::string label, bank;      // what it plays, and the bank it is in (knobs)
    };
    // Every control of the device with what it plays now, knobs on the bank page shown (`page`,
    // wrapping). An override whose target is gone falls through to the defaults.
    std::vector<ResolvedControl> resolveControls(const AssignmentSource&, const PhysicalControls&, uint32_t part, uint32_t page);
    // How many pages the knobs have for a part on a device with that many knobs.
    uint32_t knobPages(const AssignmentSource&, uint32_t knobs, uint32_t part);
    // A target's name as the views show it ("Piano level", "Basic Synth: Cutoff"); empty when it is gone.
    std::string assignmentName(const AssignmentSource&, const Assignment&);
}

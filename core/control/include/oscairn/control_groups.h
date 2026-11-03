// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/parts.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oscairn
{
    // RFC 0011's physical controls (physical_controls.h): what the control API carries of them
    // (Control::setControllers, setControlView).
    // The kinds of bank: a part's Assign banks (its controls, or its first parameters), the role
    // banks, and its effects. With no part selected, Assign banks are the Performance's controls.
    enum class BankKind { Assign, Filter, AmpEG, EQ, Levels, Effects };
    inline constexpr std::array<const char*, 6> bankKinds{"assign", "filter", "ampEG", "eq", "levels", "effects"}; // as profiles name rows

    // A group of a device's controls (its profile's): kind (knobs, encoders, faders, buttons,
    // wheels, touchstrip, jog, pedal, ...), count and role. An assign group either has hardware
    // rows, each a bank its row button picks directly, or is paged (by a button or the screen).
    // Faders may have modes: part (part levels), element (the part's Levels bank) and audio (the
    // levels of parts playing audio inputs).
    // Buttons play part mutes, or with `scenes` the scenes in order, or with `switches` nothing
    // until assigned (assignable switches, numbered apart). A group with `functions`
    // (wheels, pedals, a ribbon, aftertouch, breath: standard controllers such as "modulation") passes each control's
    // controller to every part unless something is assigned to it. Other kinds than knobs and
    // encoders play banks only when `paged` or with rows; else nothing until assigned.
    // An external controller's group says how it talks MIDI (RFC 0024's controllers).
    // Steps: one value for a step up and another for a step down (the group's `steps`).
    enum class Encoding { Absolute, TwosComplement, BinaryOffset, SignMagnitude, Steps };
    inline constexpr std::array<const char*, 4> encodings{"absolute", "twosComplement", "binaryOffset", "signMagnitude"}; // as profiles name them
    struct ControlGroup
    {
        enum class Role { Notes, Screen, Assign, Fixed } role = Role::Assign;
        std::string kind;
        uint32_t count = 0;
        struct Row { std::string name; BankKind bank = BankKind::Assign; };
        std::vector<Row> rows;
        std::vector<std::string> modes;
        bool scenes = false, paged = false, switches = false;
        std::vector<std::string> functions;
        struct Midi
        {
            std::string device;                 // the controller's profile id; empty: not MIDI (a panel's own)
            uint32_t port = 1, channel = 1;     // the profile's port, from 1; channel 1 to 16
            std::vector<uint32_t> channels;     // one per control, when they differ (then `channel` is unused)
            enum class Message { CC, Note, PitchBend, Pressure } message = Message::CC; // Pressure: channel pressure, a group of one
            std::vector<uint8_t> numbers;       // CC or note per control, or per control per row
            std::vector<Encoding> encodings;    // one per control (absolute when missing)
            std::vector<std::array<uint8_t, 2>> steps; // one per control: Steps' increment and decrement values
            bool latch = false;                 // buttons: the controller keeps their state
            bool feedback = false;              // the controller takes each control's value back on its message (control_surface.h)
        } midi;
    };
    // Where each group is now: its page, its row, or its mode, by group, in the device's order.
    using GroupPositions = std::vector<uint32_t>;

    // A generated editor's page on screen (RFC 0019): its name and its parameters by priority.
    struct ScreenPage { std::string name; std::vector<Assignment> params; bool operator==(const ScreenPage&) const = default; };
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/physical_controls.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oscairn
{
    // Control surfaces (RFC 0024's controllers): a port speaking Mackie Control, and values sent
    // back to a controller's knobs. Pure: the host reads and sends the MIDI.
    namespace mackie
    {
        // A Mackie Control (Logic Control, MCU) message from a surface. Buttons are notes (velocity
        // 127 pressed, 0 released); faders are 14-bit pitch bend on their strip's channel (the
        // ninth, the master); V-pots (CC 16 to 23) and the jog wheel (CC 60) turn relatively, bit 6
        // the sign and the rest the steps.
        struct Event
        {
            enum class Kind
            {
                Play, Stop, Record, Rewind, FastForward, // transport
                Up, Down, Left, Right,                   // the cursor keys
                Select, Mute, Solo, Arm,                 // a strip's buttons
                Touch, Fader, VPot, Jog,
                Button                                   // any other button, by `note`
            } kind = Kind::Button;
            uint8_t strip = 0;   // Select, Mute, Solo, Arm, Touch, Fader, VPot: 0 to 7, 8 the master fader
            bool pressed = false; // buttons and Touch
            double value = 0;     // Fader: 0 to 1
            int steps = 0;        // VPot, Jog: clockwise positive
            uint8_t note = 0;     // the button's note number
            bool operator==(const Event&) const = default;
        };
        std::optional<Event> parse(const uint8_t* bytes, size_t size);

        // The note numbers the protocol gives each button, which also light its LED.
        inline constexpr uint8_t arm = 0x00, solo = 0x08, mute = 0x10, select = 0x18, touch = 0x68;
        inline constexpr uint8_t rewind = 0x5B, fastForward = 0x5C, stop = 0x5D, play = 0x5E, record = 0x5F;
        inline constexpr uint8_t up = 0x60, down = 0x61, left = 0x62, right = 0x63;
    }

    // Mackie Control mapped to Oscairn: the transport (Play, Stop); the first eight parts, in order,
    // on the strips: Select selects a part for the surface, Mute and Solo toggle its own, its fader
    // is its level; the V-pots play the selected part's knob banks a step a detent (the
    // Performance's controls with none selected); the cursors and jog wheel navigate: up and down
    // (and the jog wheel) select the part before or after, left and right page the V-pots' bank.
    // And back: the mute, solo, select and transport LEDs, the faders' positions (not a fader
    // being touched) and, on surfaces with a display, the parts' names on the scribble strips.
    // The surface's selection is its own, apart from the screen's. Not thread-safe: one caller.
    class MackieControl
    {
    public:
        static constexpr uint32_t strips = 8;
        struct State
        {
            std::vector<Part> parts; // in order; the strips play the first eight
            bool playing = false;
        };
        struct Action
        {
            enum class Kind { Set, Play, Stop, Solo } kind = Kind::Set;
            Assignment target;    // Set: what to set (a part's level or mute, a V-pot's slot) and its value, 0 to 1
            double value = 0;
            uint32_t part = 0;    // Solo
            bool on = false;      // Solo
            bool operator==(const Action&) const = default;
        };
        // `value` reads a target's value, 0 to 1, as ControllerMapping's does.
        std::vector<Action> handle(const State&, const AssignmentSource&, const uint8_t* bytes, size_t size,
                                   const std::function<double(const Assignment&)>& value);
        // The messages that bring the surface up to date: only what changed since the last call,
        // everything after reset() (a surface connected again). `display`: send the names.
        std::vector<std::vector<uint8_t>> feedback(const State&, bool display);
        void reset() { sent_.clear(); }
        uint32_t selected() const { return selected_; } // the part selected, or 0
        uint32_t page() const { return page_; }

    private:
        uint32_t selected_ = 0, page_ = 0;
        std::array<bool, strips + 1> touched_{};
        std::map<uint32_t, std::vector<uint8_t>> sent_; // by what it sets: an LED's note, 0x100 + a fader, 0x200 the names
    };

    // Values back to a controller's absolute knobs whose profile says it takes them (RFC 0024: the
    // MODX M's endless knobs keep a value each): for each resolved control in a group with
    // `feedback`, its target's value as its CC on its channel, so the knob matches and needs no
    // pickup; only values that changed since the last call. Not thread-safe: one caller.
    class KnobFeedback
    {
    public:
        struct Message { std::string device; uint32_t port = 1; std::array<uint8_t, 3> bytes{}; bool operator==(const Message&) const = default; };
        std::vector<Message> update(const std::vector<ControlGroup>&, const std::vector<ResolvedControl>&, const GroupPositions&,
                                    const std::function<double(const Assignment&)>& value);
        void reset() { sent_.clear(); }

    private:
        std::map<std::tuple<std::string, uint32_t, uint8_t, uint8_t>, uint8_t> sent_; // device, port, channel, CC: value
    };
}

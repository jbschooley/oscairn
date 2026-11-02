// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011's controller-mapping service: a controller's MIDI messages to values for what its
// controls play, with pickup for absolute controls and the relative encodings (RFC 0024).
#include "oscairn/physical_controls.h"

#include <algorithm>
#include <cmath>

namespace oscairn
{
    int relativeSteps(Encoding encoding, uint8_t value, std::array<uint8_t, 2> steps)
    {
        value &= 0x7F;
        switch(encoding)
        {
        case Encoding::Absolute: return 0;
        case Encoding::TwosComplement: return value < 64 ? value : int(value) - 128;
        case Encoding::BinaryOffset: return int(value) - 64;
        case Encoding::SignMagnitude: return value & 0x40 ? -int(value & 0x3F) : int(value);
        case Encoding::Steps: return value == steps[0] ? 1 : value == steps[1] ? -1 : 0;
        }
        return 0;
    }

    std::optional<MidiControl> midiControl(const std::vector<ControlGroup>& groups, const std::string& device, uint32_t port, const uint8_t* bytes, size_t size)
    {
        if(size < 2 || device.empty()) return {};
        const uint8_t kind = bytes[0] & 0xF0, d1 = bytes[1] & 0x7F;
        const uint32_t channel = (bytes[0] & 0x0F) + 1u;
        using Message = ControlGroup::Midi::Message;
        if(kind != 0xB0 && kind != 0x90 && kind != 0x80 && kind != 0xE0 && kind != 0xD0) return {};
        if(size < 3 && kind != 0xD0) return {};
        const auto message = kind == 0xB0 ? Message::CC : kind == 0xE0 ? Message::PitchBend : kind == 0xD0 ? Message::Pressure : Message::Note;
        for(size_t g = 0; g < groups.size(); ++g)
        {
            const auto& m = groups[g].midi;
            if(m.device != device || m.port != port || m.message != message || !groups[g].count) continue;
            // A control's channel: its own, or the group's.
            auto on = [&](uint32_t control) { return (control < m.channels.size() ? m.channels[control] : m.channel) == channel; };
            if(message == Message::PitchBend || message == Message::Pressure) { if(on(0)) return MidiControl{g, 0, -1}; continue; }
            uint32_t k = 0;
            while(k < m.numbers.size() && (m.numbers[k] != d1 || !on(k % groups[g].count))) ++k;
            if(k == m.numbers.size()) continue;
            const bool rows = !groups[g].rows.empty() && m.numbers.size() > groups[g].count;
            return MidiControl{g, k % groups[g].count, rows ? int(k / groups[g].count) : -1};
        }
        return {};
    }

    std::vector<ControllerMapping::Action> ControllerMapping::latest(std::vector<Action> actions)
    {
        using K = Assignment::Kind;
        std::vector<Action> kept;
        std::vector<Assignment> seen;
        for(auto it = actions.rbegin(); it != actions.rend(); ++it)
        {
            const auto k = it->target.kind;
            if(k == K::Parameter || k == K::Control || k == K::PartLevel)
            {
                if(std::find(seen.begin(), seen.end(), it->target) != seen.end()) continue;
                seen.push_back(it->target);
            }
            kept.push_back(std::move(*it));
        }
        std::reverse(kept.begin(), kept.end());
        return kept;
    }

    std::vector<ControllerMapping::Action> ControllerMapping::handle(const AssignmentSource& s, const std::vector<ControlGroup>& groups, uint32_t part, GroupPositions& positions,
                                                                     const std::string& device, uint32_t port, const uint8_t* bytes, size_t size,
                                                                     const std::function<double(const Assignment&)>& value)
    {
        const auto at = midiControl(groups, device, port, bytes, size);
        if(!at) return {};
        const uint8_t kind = bytes[0] & 0xF0, d1 = bytes[1] & 0x7F, d2 = size > 2 ? bytes[2] & 0x7F : 0;
        using Message = ControlGroup::Midi::Message;
        const auto g = at->group; const auto index = at->index;
        if(at->row >= 0) { if(positions.size() <= g) positions.resize(g + 1); positions[g] = uint32_t(at->row); }
        const auto& group = groups[g];
        const auto message = group.midi.message;
        const auto resolved = resolveControls(s, groups, part, positions);
        const auto r = std::find_if(resolved.begin(), resolved.end(), [&](const ResolvedControl& c) { return c.group == g && c.index == index; });
        if(r == resolved.end()) return {};
        // What it plays: its target, or with a function and nothing assigned, each part's Sound's
        // (none: the controller's message reaches the parts on its own).
        std::vector<Assignment> targets;
        if(r->target.kind != Assignment::Kind::None) targets.push_back(r->target);
        else for(const auto& [p, t] : r->parts) targets.push_back(t);
        std::vector<Action> out;
        const bool button = group.kind == "buttons" || group.kind == "footswitches" || message == Message::Note;
        const auto encoding = index < group.midi.encodings.size() ? group.midi.encodings[index] : Encoding::Absolute;
        constexpr double step = 1.0 / 127, tolerance = 1.0 / 127 + 1e-9;
        for(const auto& t : targets)
        {
            const double now = std::clamp(value(t), 0.0, 1.0);
            if(button)
            {
                const bool on = message == Message::Note ? kind == 0x90 && d2 > 0 : d2 >= 64;
                // A latching switch changes state on every press, so each message is a press.
                if(t.kind == Assignment::Kind::Scene) { if(on || group.midi.latch) out.push_back({r->control, t, 1}); continue; }
                if(group.midi.latch) out.push_back({r->control, t, on ? 1.0 : 0.0});
                else if(on) out.push_back({r->control, t, now >= .5 ? 0.0 : 1.0});
                continue;
            }
            if(encoding != Encoding::Absolute)
            {
                const auto steps = index < group.midi.steps.size() ? group.midi.steps[index] : std::array<uint8_t, 2>{};
                out.push_back({r->control, t, std::clamp(now + relativeSteps(encoding, d2, steps) * step, 0.0, 1.0)});
                continue;
            }
            // Absolute: pick up. Lost when the target changes or something else moves it (a value
            // that is none of this control's recent ones: values read back may lag a little).
            const double x = message == Message::PitchBend ? (d1 | (d2 << 7)) / 16383.0 : message == Message::Pressure ? d1 / 127.0 : d2 / 127.0;
            auto& p = pickup_[r->control + "/" + std::to_string(t.node) + "." + std::to_string(t.param) + "." + std::to_string(t.id) + "." + std::to_string(int(t.kind))];
            const bool ours = std::any_of(p.sent.begin(), p.sent.begin() + std::min(p.sends, p.sent.size()), [&](double v) { return std::abs(v - now) <= tolerance; });
            if(p.target != t || (p.caught && !ours)) { p.target = t; p.caught = false; p.sends = 0; }
            if(!p.caught && (std::abs(x - now) <= tolerance || (p.knob >= 0 && (p.knob - now) * (x - now) <= 0))) p.caught = true;
            p.knob = x;
            if(!p.caught) continue;
            p.sent[p.sends++ % p.sent.size()] = x;
            out.push_back({r->control, t, x});
        }
        return out;
    }
}

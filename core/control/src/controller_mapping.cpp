// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011's controller-mapping service: a controller's MIDI messages to values for what its
// controls play, with pickup for absolute controls and the relative encodings (RFC 0024).
#include "oscairn/physical_controls.h"

#include <algorithm>
#include <cmath>

namespace oscairn
{
    int relativeSteps(Encoding encoding, uint8_t value)
    {
        value &= 0x7F;
        switch(encoding)
        {
        case Encoding::Absolute: return 0;
        case Encoding::TwosComplement: return value < 64 ? value : int(value) - 128;
        case Encoding::BinaryOffset: return int(value) - 64;
        case Encoding::SignMagnitude: return value & 0x40 ? -int(value & 0x3F) : int(value);
        }
        return 0;
    }

    std::vector<ControllerMapping::Action> ControllerMapping::handle(const AssignmentSource& s, const std::vector<ControlGroup>& groups, uint32_t part, GroupPositions& positions,
                                                                     const std::string& device, uint32_t port, const uint8_t* bytes, size_t size,
                                                                     const std::function<double(const Assignment&)>& value)
    {
        if(size < 3 || device.empty()) return {};
        const uint8_t kind = bytes[0] & 0xF0, d1 = bytes[1] & 0x7F, d2 = bytes[2] & 0x7F;
        const uint32_t channel = (bytes[0] & 0x0F) + 1u;
        using Message = ControlGroup::Midi::Message;
        if(kind != 0xB0 && kind != 0x90 && kind != 0x80 && kind != 0xE0) return {};
        const auto message = kind == 0xB0 ? Message::CC : kind == 0xE0 ? Message::PitchBend : Message::Note;
        // The control the message comes from: its group, its place there and, for a group of rows
        // sending a number per row, the row.
        size_t g = 0; uint32_t index = 0;
        for(; g < groups.size(); ++g)
        {
            const auto& m = groups[g].midi;
            if(m.device != device || m.port != port || m.channel != channel || m.message != message || !groups[g].count) continue;
            if(message == Message::PitchBend) { index = 0; break; }
            const auto at = std::find(m.numbers.begin(), m.numbers.end(), d1);
            if(at == m.numbers.end()) continue;
            const auto k = uint32_t(at - m.numbers.begin());
            index = k % groups[g].count;
            if(!groups[g].rows.empty() && m.numbers.size() > groups[g].count) { if(positions.size() <= g) positions.resize(g + 1); positions[g] = k / groups[g].count; }
            break;
        }
        if(g == groups.size()) return {};
        const auto& group = groups[g];
        const auto resolved = resolveControls(s, groups, part, positions);
        const auto r = std::find_if(resolved.begin(), resolved.end(), [&](const ResolvedControl& c) { return c.group == g && c.index == index; });
        if(r == resolved.end()) return {};
        // What it plays: its target, or with a function and nothing assigned, each part's Sound's
        // (none: the controller's message reaches the parts on its own).
        std::vector<Assignment> targets;
        if(r->target.kind != Assignment::Kind::None) targets.push_back(r->target);
        else for(const auto& [p, t] : r->parts) targets.push_back(t);
        std::vector<Action> out;
        const bool button = group.kind == "buttons" || message == Message::Note;
        const auto encoding = index < group.midi.encodings.size() ? group.midi.encodings[index] : Encoding::Absolute;
        constexpr double step = 1.0 / 127, tolerance = 1.0 / 127 + 1e-9;
        for(const auto& t : targets)
        {
            const double now = std::clamp(value(t), 0.0, 1.0);
            if(button)
            {
                const bool on = message == Message::Note ? kind == 0x90 && d2 > 0 : d2 >= 64;
                if(t.kind == Assignment::Kind::Scene) { if(on) out.push_back({r->control, t, 1}); continue; }
                if(group.midi.latch) out.push_back({r->control, t, on ? 1.0 : 0.0});
                else if(on) out.push_back({r->control, t, now >= .5 ? 0.0 : 1.0});
                continue;
            }
            if(encoding != Encoding::Absolute) { out.push_back({r->control, t, std::clamp(now + relativeSteps(encoding, d2) * step, 0.0, 1.0)}); continue; }
            // Absolute: pick up. Lost when the target changes or something else moves it (a value
            // that is none of this control's recent ones: values read back may lag a little).
            const double x = message == Message::PitchBend ? (d1 | (d2 << 7)) / 16383.0 : d2 / 127.0;
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

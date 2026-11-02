// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Control surfaces (RFC 0024's controllers): Mackie Control in and back, and values sent back to
// a controller's knobs.
#include "oscairn/control_surface.h"

#include <algorithm>
#include <cmath>

namespace oscairn
{
    std::optional<mackie::Event> mackie::parse(const uint8_t* bytes, size_t size)
    {
        if(!bytes || size < 3) return {};
        const uint8_t kind = bytes[0] & 0xF0, channel = bytes[0] & 0x0F, d1 = bytes[1] & 0x7F, d2 = bytes[2] & 0x7F;
        using K = Event::Kind;
        Event e;
        if(kind == 0xE0)
        {
            if(channel > 8) return {};
            e.kind = K::Fader; e.strip = channel; e.value = (d1 | (d2 << 7)) / 16383.0;
            return e;
        }
        if(kind == 0xB0)
        {
            e.steps = d2 & 0x40 ? -int(d2 & 0x3F) : int(d2 & 0x3F);
            if(d1 >= 16 && d1 < 24) { e.kind = K::VPot; e.strip = uint8_t(d1 - 16); }
            else if(d1 == 60) e.kind = K::Jog;
            else return {};
            return e;
        }
        if(kind != 0x90 && kind != 0x80) return {};
        e.note = d1; e.pressed = kind == 0x90 && d2 > 0;
        if(d1 < solo) { e.kind = K::Arm; e.strip = d1; }
        else if(d1 < mute) { e.kind = K::Solo; e.strip = uint8_t(d1 - solo); }
        else if(d1 < select) { e.kind = K::Mute; e.strip = uint8_t(d1 - mute); }
        else if(d1 < select + 8) { e.kind = K::Select; e.strip = uint8_t(d1 - select); }
        else if(d1 >= touch && d1 <= touch + 8) { e.kind = K::Touch; e.strip = uint8_t(d1 - touch); }
        else switch(d1)
        {
        case play: e.kind = K::Play; break;
        case stop: e.kind = K::Stop; break;
        case record: e.kind = K::Record; break;
        case rewind: e.kind = K::Rewind; break;
        case fastForward: e.kind = K::FastForward; break;
        case up: e.kind = K::Up; break;
        case down: e.kind = K::Down; break;
        case left: e.kind = K::Left; break;
        case right: e.kind = K::Right; break;
        default: e.kind = K::Button; break;
        }
        return e;
    }

    std::vector<MackieControl::Action> MackieControl::handle(const State& state, const AssignmentSource& source, const uint8_t* bytes, size_t size,
                                                             const std::function<double(const Assignment&)>& value)
    {
        const auto e = mackie::parse(bytes, size);
        if(!e) return {};
        using K = mackie::Event::Kind;
        using A = Action::Kind;
        const Part* part = e->strip < strips && e->strip < state.parts.size() ? &state.parts[e->strip] : nullptr;
        auto banks = [&] { return knobBanks(source, selected_, strips); };
        // The part `by` places after the selected one (before, for a negative step), within the list.
        auto move = [&](int by) -> std::vector<Action> {
            if(state.parts.empty() || !by) return {};
            const auto at = std::find_if(state.parts.begin(), state.parts.end(), [&](const Part& p) { return p.id == selected_; });
            const int from = at == state.parts.end() ? (by > 0 ? -1 : int(state.parts.size())) : int(at - state.parts.begin());
            const int to = std::clamp(from + by, 0, int(state.parts.size()) - 1);
            if(to != from) { selected_ = state.parts[size_t(to)].id; page_ = 0; }
            return {};
        };
        switch(e->kind)
        {
        case K::Play: if(e->pressed) return {{A::Play}}; break;
        case K::Stop: if(e->pressed) return {{A::Stop}}; break;
        case K::Select: if(e->pressed && part) { selected_ = part->id; page_ = 0; } break;
        case K::Mute: if(e->pressed && part) return {{A::Set, {Assignment::Kind::PartMute, 0, 0, part->id}, part->mute ? 0.0 : 1.0}}; break;
        case K::Solo: if(e->pressed && part) return {{A::Solo, {}, 0, part->id, !part->solo}}; break;
        case K::Touch:
            touched_[e->strip] = e->pressed;
            if(!e->pressed) sent_.erase(0x100 + e->strip); // where the part's level is now goes back on release
            break;
        case K::Fader:
            if(!part) break;
            // The surface shows this already: no echo.
            sent_[0x100 + e->strip] = {uint8_t(0xE0 | e->strip), bytes[1], bytes[2]};
            return {{A::Set, {Assignment::Kind::PartLevel, 0, 0, part->id}, e->value}};
        case K::VPot:
        {
            const auto list = banks();
            if(list.empty()) break;
            const auto& bank = list[std::min<size_t>(page_, list.size() - 1)];
            if(e->strip >= bank.slots.size() || bank.slots[e->strip].kind == Assignment::Kind::None) break;
            const auto& t = bank.slots[e->strip];
            return {{A::Set, t, std::clamp(std::clamp(value(t), 0.0, 1.0) + e->steps / 127.0, 0.0, 1.0)}};
        }
        case K::Up: if(e->pressed) return move(-1); break;
        case K::Down: if(e->pressed) return move(1); break;
        case K::Jog: return move(e->steps);
        case K::Left: if(e->pressed && page_) --page_; break;
        case K::Right: if(e->pressed) { const auto count = banks().size(); if(page_ + 1 < count) ++page_; } break;
        default: break;
        }
        return {};
    }

    std::vector<std::vector<uint8_t>> MackieControl::feedback(const State& state, bool display)
    {
        std::vector<std::vector<uint8_t>> out;
        auto set = [&](uint32_t key, std::vector<uint8_t> message) {
            auto& last = sent_[key];
            if(last != message) { last = message; out.push_back(std::move(message)); }
        };
        auto led = [&](uint8_t note, bool on) { set(note, {0x90, note, uint8_t(on ? 0x7F : 0)}); };
        for(uint8_t i = 0; i < strips; ++i)
        {
            const Part* p = i < state.parts.size() ? &state.parts[i] : nullptr;
            led(uint8_t(mackie::mute + i), p && p->mute);
            led(uint8_t(mackie::solo + i), p && p->solo);
            led(uint8_t(mackie::select + i), p && p->id == selected_);
            if(touched_[i]) continue; // the hand on it wins
            const auto v = int(std::lround(std::clamp(p ? p->level : 0.0, 0.0, 1.0) * 16383));
            set(0x100 + i, {uint8_t(0xE0 | i), uint8_t(v & 0x7F), uint8_t(v >> 7)});
        }
        led(mackie::play, state.playing);
        led(mackie::stop, !state.playing);
        if(display)
        {
            // The upper row of the scribble strips: seven characters a strip, six of the name and a space.
            std::vector<uint8_t> names{0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00};
            for(uint32_t i = 0; i < strips; ++i)
            {
                const std::string name = i < state.parts.size() ? state.parts[i].name : std::string();
                for(size_t c = 0; c < 7; ++c)
                {
                    const char ch = c < 6 && c < name.size() ? name[c] : ' ';
                    names.push_back(uint8_t(ch >= 0x20 && ch < 0x7F ? ch : '?'));
                }
            }
            names.push_back(0xF7);
            set(0x200, std::move(names));
        }
        return out;
    }

    std::vector<KnobFeedback::Message> KnobFeedback::update(const std::vector<ControlGroup>& groups, const std::vector<ResolvedControl>& resolved, const GroupPositions& positions,
                                                            const std::function<double(const Assignment&)>& value)
    {
        std::vector<Message> out;
        for(const auto& r : resolved)
        {
            if(r.group >= groups.size() || r.target.kind == Assignment::Kind::None) continue;
            const auto& g = groups[r.group];
            const auto& m = g.midi;
            const bool absolute = r.index >= m.encodings.size() || m.encodings[r.index] == Encoding::Absolute;
            if(!m.feedback || m.device.empty() || m.message != ControlGroup::Midi::Message::CC || !absolute || !g.count) continue;
            // Its number on the row the group is on, for rows sending a number each.
            const uint32_t row = m.numbers.size() > g.count && r.group < positions.size() ? positions[r.group] : 0;
            const size_t k = size_t(row) * g.count + r.index;
            if(k >= m.numbers.size()) continue;
            const auto channel = uint8_t((r.index < m.channels.size() ? m.channels[r.index] : m.channel) - 1);
            const auto v = uint8_t(std::lround(std::clamp(value(r.target), 0.0, 1.0) * 127));
            auto [at, added] = sent_.try_emplace({m.device, m.port, channel, m.numbers[k]}, v);
            if(!added && at->second == v) continue;
            at->second = v;
            out.push_back({m.device, m.port, {uint8_t(0xB0 | (channel & 0x0F)), m.numbers[k], v}});
        }
        return out;
    }
}

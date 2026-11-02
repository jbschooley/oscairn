// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Control surfaces (RFC 0024's controllers): Mackie Control's messages parsed, mapped to the
// transport and parts, and fed back (LEDs, faders, scribble strips); values sent back to a
// controller's knobs whose profile takes them. No device: messages in, messages out.
#include "oscairn/control_surface.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace oscairn;
using mackie::Event;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    std::optional<Event> parse(std::vector<uint8_t> m) { return mackie::parse(m.data(), m.size()); }
    bool is(const std::optional<Event>& e, Event::Kind kind, uint8_t strip = 0, bool pressed = true) { return e && e->kind == kind && e->strip == strip && e->pressed == pressed; }
    Assignment parameter(NodeId node, uint32_t param) { Assignment a; a.kind = Assignment::Kind::Parameter; a.node = node; a.param = param; return a; }
    Assignment of(Assignment::Kind kind, uint32_t id) { Assignment a; a.kind = kind; a.id = id; return a; }

    void parsing()
    {
        using K = Event::Kind;
        check(is(parse({0x90, 0x5E, 0x7F}), K::Play) && is(parse({0x90, 0x5E, 0x00}), K::Play, 0, false) && is(parse({0x80, 0x5E, 0x40}), K::Play, 0, false),
              "Play pressed, and released by velocity 0 or a note off");
        check(is(parse({0x90, 0x5D, 0x7F}), K::Stop) && is(parse({0x90, 0x5F, 0x7F}), K::Record) && is(parse({0x90, 0x5B, 0x7F}), K::Rewind) && is(parse({0x90, 0x5C, 0x7F}), K::FastForward),
              "the transport buttons");
        check(is(parse({0x90, 0x60, 0x7F}), K::Up) && is(parse({0x90, 0x61, 0x7F}), K::Down) && is(parse({0x90, 0x62, 0x7F}), K::Left) && is(parse({0x90, 0x63, 0x7F}), K::Right),
              "the cursors");
        check(is(parse({0x90, 0x1B, 0x7F}), K::Select, 3) && is(parse({0x90, 0x11, 0x7F}), K::Mute, 1) && is(parse({0x90, 0x0F, 0x7F}), K::Solo, 7) && is(parse({0x90, 0x02, 0x7F}), K::Arm, 2),
              "a strip's select, mute, solo and record arm");
        check(is(parse({0x90, 0x6A, 0x7F}), K::Touch, 2) && is(parse({0x90, 0x6A, 0x00}), K::Touch, 2, false) && is(parse({0x90, 0x70, 0x7F}), K::Touch, 8), "fader touch, the master's too");
        auto fader = parse({0xE2, 0x7F, 0x7F});
        check(fader && fader->kind == K::Fader && fader->strip == 2 && fader->value == 1, "a fader at the top: 14-bit pitch bend on its strip's channel");
        fader = parse({0xE8, 0x00, 0x40});
        check(fader && fader->kind == K::Fader && fader->strip == 8 && std::abs(fader->value - 8192 / 16383.0) < 1e-9, "the master fader, half way");
        check(!parse({0xE9, 0, 0}), "pitch bend past the master's channel is not a fader");
        auto vpot = parse({0xB0, 0x14, 0x03});
        check(vpot && vpot->kind == K::VPot && vpot->strip == 4 && vpot->steps == 3, "V-pot 5 three steps clockwise");
        vpot = parse({0xB0, 0x17, 0x41});
        check(vpot && vpot->kind == K::VPot && vpot->strip == 7 && vpot->steps == -1, "V-pot 8 a step counterclockwise");
        const auto jog = parse({0xB0, 0x3C, 0x01}), back = parse({0xB0, 0x3C, 0x45});
        check(jog && jog->kind == K::Jog && jog->steps == 1 && back && back->steps == -5, "the jog wheel, relative");
        const auto other = parse({0x90, 0x54, 0x7F});
        check(other && other->kind == K::Button && other->note == 0x54, "another button, by its note");
        check(!parse({0xB0, 0x07, 0x40}) && !parse({0xC0, 0x01, 0x00}) && !parse({0x90, 0x5E}), "other controllers, program changes and short messages are not Mackie's");
    }

    // Three parts, Bass muted, on a part with a synth and a delay.
    MackieControl::State state()
    {
        MackieControl::State s;
        Part keys; keys.id = 1; keys.name = "Keys"; keys.chain = {11, 12}; keys.level = .5;
        Part bass; bass.id = 2; bass.name = "Bass Line"; bass.mute = true; bass.level = 1;
        Part pad; pad.id = 3; pad.name = "Pad"; pad.solo = true; pad.level = 0;
        s.parts = {keys, bass, pad};
        return s;
    }
    AssignmentSource source(const MackieControl::State& st)
    {
        AssignmentSource a;
        a.parts = st.parts;
        a.nodes[11] = {11, "synth", false, true, {{7, "Gain"}, {99, "Tone"}}};
        a.nodes[12] = {12, "delay", true, false, {{1, "Mix"}}};
        return a;
    }

    void mapping()
    {
        using A = MackieControl::Action;
        MackieControl mcu;
        const auto st = state();
        const auto src = source(st);
        auto value = [](const Assignment&) { return .5; };
        auto handle = [&](std::vector<uint8_t> m) { return mcu.handle(st, src, m.data(), m.size(), value); };
        check(handle({0x90, 0x5E, 0x7F}) == std::vector<A>{{A::Kind::Play}} && handle({0x90, 0x5D, 0x7F}) == std::vector<A>{{A::Kind::Stop}} && handle({0x90, 0x5E, 0}).empty(),
              "Play and Stop play and stop; releases do nothing");
        check(handle({0xB0, 0x10, 0x03}).empty() && mcu.selected() == 0, "nothing selected and no Performance controls: the V-pots play nothing");
        check(handle({0x90, 0x19, 0x7F}).empty() && mcu.selected() == 2, "Select on strip 2 selects the second part");
        check(handle({0x90, 0x11, 0x7F}) == std::vector<A>{{A::Kind::Set, of(Assignment::Kind::PartMute, 2), 0}}, "Mute on strip 2 unmutes the muted Bass");
        check(handle({0x90, 0x10, 0x7F}) == std::vector<A>{{A::Kind::Set, of(Assignment::Kind::PartMute, 1), 1}}, "and mutes Keys");
        check(handle({0x90, 0x08, 0x7F}) == std::vector<A>{{A::Kind::Solo, {}, 0, 1, true}} && handle({0x90, 0x0A, 0x7F}) == std::vector<A>{{A::Kind::Solo, {}, 0, 3, false}},
              "Solo toggles a part's solo");
        const auto level = handle({0xE2, 0x00, 0x40});
        check(level.size() == 1 && level[0].kind == A::Kind::Set && level[0].target == of(Assignment::Kind::PartLevel, 3) && std::abs(level[0].value - 8192 / 16383.0) < 1e-9,
              "the third fader is the third part's level");
        check(handle({0x90, 0x1D, 0x7F}).empty() && handle({0xE5, 0, 0x40}).empty() && handle({0xE8, 0, 0x40}).empty(), "strips without a part, and the master fader, do nothing");
        handle({0x90, 0x60, 0x7F}); const auto up = mcu.selected(); handle({0x90, 0x60, 0x7F});
        check(up == 1 && mcu.selected() == 1, "up selects the part before, and stops at the first");
        handle({0x90, 0x61, 0x7F}); check(mcu.selected() == 2, "down the part after");
        handle({0xB0, 0x3C, 0x05}); const auto jogged = mcu.selected(); handle({0xB0, 0x3C, 0x42});
        check(jogged == 3 && mcu.selected() == 1, "the jog wheel moves the selection as far as it turns, stopping at the ends");
        const auto knob = handle({0xB0, 0x10, 0x03});
        check(knob.size() == 1 && knob[0].target == parameter(11, 7) && std::abs(knob[0].value - (.5 + 3 / 127.0)) < 1e-9, "V-pot 1 steps the selected part's first knob: the synth's Gain");
        check(handle({0xB0, 0x12, 0x01}).empty(), "a V-pot with nothing in its slot does nothing");
        handle({0x90, 0x63, 0x7F});
        const auto effect = handle({0xB0, 0x10, 0x41});
        check(mcu.page() == 1 && effect.size() == 1 && effect[0].target == parameter(12, 1) && std::abs(effect[0].value - (.5 - 1 / 127.0)) < 1e-9, "right pages to the effects bank: the delay's mix");
        handle({0x90, 0x63, 0x7F});
        check(mcu.page() == 1, "no page past the last");
        handle({0x90, 0x62, 0x7F});
        check(mcu.page() == 0, "left pages back");
    }

    void feedback()
    {
        MackieControl mcu;
        auto st = state();
        auto all = mcu.feedback(st, true);
        auto has = [&](const std::vector<uint8_t>& m) { return std::find(all.begin(), all.end(), m) != all.end(); };
        check(all.size() == 8 * 3 + 8 + 2 + 1, "everything at first: three LEDs and a fader a strip, Play and Stop, the names: " + std::to_string(all.size()));
        check(has({0x90, 0x11, 0x7F}) && has({0x90, 0x10, 0}) && has({0x90, 0x0A, 0x7F}) && has({0x90, 0x18, 0}) && has({0x90, 0x14, 0}),
              "mute and solo LEDs as the parts are, nothing selected; strips without a part dark");
        const int half = int(std::lround(.5 * 16383));
        check(has({0xE0, uint8_t(half & 0x7F), uint8_t(half >> 7)}) && has({0xE1, 0x7F, 0x7F}) && has({0xE2, 0, 0}) && has({0xE5, 0, 0}), "the faders where the levels are");
        check(has({0x90, 0x5E, 0}) && has({0x90, 0x5D, 0x7F}), "stopped: Stop lit");
        std::vector<uint8_t> names{0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 0x00};
        for(const char c : std::string("Keys   Bass L Pad    ") + std::string(35, ' ')) names.push_back(uint8_t(c));
        names.push_back(0xF7);
        check(has(names), "the names on the scribble strips' upper row, six characters each");
        check(mcu.feedback(st, true).empty(), "nothing again when nothing changed");
        st.parts[1].mute = false; st.playing = true;
        all = mcu.feedback(st, true);
        check(all.size() == 3 && has({0x90, 0x11, 0}) && has({0x90, 0x5E, 0x7F}) && has({0x90, 0x5D, 0}), "only what changed: Bass unmuted, playing");
        {
            const std::vector<uint8_t> select{0x90, 0x1A, 0x7F};
            mcu.handle(st, source(st), select.data(), 3, [](const Assignment&) { return 0.0; });
            all = mcu.feedback(st, true);
            check(all.size() == 1 && has({0x90, 0x1A, 0x7F}), "a part selected: its Select LED");
        }
        // A hand on a fader: its moves set the level, and nothing is sent back to fight it until released.
        auto value = [](const Assignment&) { return 0.0; };
        const auto src = source(st);
        const std::vector<uint8_t> touch{0x90, 0x68, 0x7F}, move{0xE0, 0x00, 0x60}, release{0x90, 0x68, 0x00};
        mcu.handle(st, src, touch.data(), 3, value);
        mcu.handle(st, src, move.data(), 3, value);
        st.parts[0].level = .25;
        check(mcu.feedback(st, true).empty(), "a touched fader is left alone");
        mcu.handle(st, src, release.data(), 3, value);
        all = mcu.feedback(st, true);
        const int quarter = int(std::lround(.25 * 16383));
        check(all.size() == 1 && has({0xE0, uint8_t(quarter & 0x7F), uint8_t(quarter >> 7)}), "released, it goes to the level");
        st.parts[2].name = "Strings";
        check(mcu.feedback(st, false).empty(), "no names for a surface without a display");
        mcu.reset();
        check(mcu.feedback(st, false).size() == 8 * 3 + 8 + 2, "after a reset, everything again");
    }

    void knobs()
    {
        ControlGroup encoders;
        encoders.kind = "encoders"; encoders.count = 4;
        encoders.rows = {{"Assign 1"}, {"Assign 2"}};
        encoders.midi.device = "modx-m-usb"; encoders.midi.numbers = {17, 18, 19, 20, 21, 22, 23, 24}; encoders.midi.feedback = true;
        ControlGroup faders;
        faders.kind = "faders"; faders.count = 1; faders.midi.device = "modx-m-usb"; faders.midi.numbers = {70};
        ControlGroup relative = encoders;
        relative.midi.device = "smk37-elite"; relative.midi.encodings = {Encoding::Steps}; relative.midi.numbers = {38, 37, 36, 35, 34, 33, 32, 31};
        const std::vector<ControlGroup> groups{encoders, faders, relative};
        ResolvedControl one; one.control = "knob:1"; one.target = parameter(11, 7); one.group = 0; one.index = 0;
        ResolvedControl two = one; two.control = "knob:2"; two.target = parameter(11, 99); two.index = 1;
        ResolvedControl blank = one; blank.control = "knob:3"; blank.target = {}; blank.index = 2;
        ResolvedControl fader = one; fader.control = "fader:1"; fader.group = 1;
        ResolvedControl stepped = one; stepped.control = "knob:5"; stepped.group = 2;
        const std::vector<ResolvedControl> resolved{one, two, blank, fader, stepped};
        double gain = .5;
        auto value = [&](const Assignment& a) { return a.param == 7 ? gain : 1.0; };
        KnobFeedback back;
        using M = KnobFeedback::Message;
        auto out = back.update(groups, resolved, {1}, value);
        check(out == std::vector<M>{{"modx-m-usb", 1, {0xB0, 21, 64}}, {"modx-m-usb", 1, {0xB0, 22, 127}}},
              "the knobs taking values get their targets' as their CCs on the row they are on; a blank knob, a group without feedback and a relative knob none");
        check(back.update(groups, resolved, {1}, value).empty(), "nothing again until a value changes");
        gain = .25;
        out = back.update(groups, resolved, {1}, value);
        check(out == std::vector<M>{{"modx-m-usb", 1, {0xB0, 21, 32}}}, "a value changed: that knob's alone");
        out = back.update(groups, resolved, {0}, value);
        check(out.size() == 2 && out[0].bytes[1] == 17 && out[1].bytes[1] == 18, "the other row: its own numbers");
    }
}

int main()
{
    parsing();
    mapping();
    feedback();
    knobs();
    if(!failures) std::puts("ok: Mackie Control parsed, mapped to the transport and parts and fed back; knob values sent back");
    return failures ? 1 : 0;
}

// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011's instant controls: a controller's MIDI into the engine moves what its control plays
// within one block, with no control thread, socket or plan compile on the way; and pickup,
// latching and momentary buttons, relative encoders and groups' rows behave as the app's
// mapping did.
#include "oscairn/session.h"
#include "AllocationGuard.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace oscairn;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    bool near(double a, double b) { return std::abs(a - b) < 1e-5; }

    constexpr uint32_t frames = 64;
    constexpr uint8_t smk = 5, other = 6; // the inputs' source numbers
    using Role = ControlGroup::Role;
    using Message = ControlGroup::Midi::Message;

    ControlGroup group(Role role, const char* kind, uint32_t count, std::vector<uint8_t> numbers)
    {
        ControlGroup g{role, kind, count};
        g.midi.device = "smk"; g.midi.numbers = std::move(numbers);
        return g;
    }

    // One block, as the audio thread renders it: the control thread does nothing meanwhile.
    float render(Session& s)
    {
        std::array<float, frames> l{}, r{};
        float* out[2]{l.data(), r.data()};
        forbidAllocation = true;
        const bool ok = s.process(nullptr, 0, out, 2, frames);
        forbidAllocation = false;
        check(ok, "render");
        return l[frames - 1];
    }
    void cc(Session& s, uint8_t number, uint8_t value, uint8_t source = smk)
    {
        const uint8_t bytes[3]{0xB0, number, value};
        s.controllerMidi(source, bytes, 3);
    }

    void mapping(const char* fixture)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .5); s.setParameter(b, 7, .25);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b); s.commit();
        // The wheel plays a control whose target is A's level, as "Drifting"'s mod wheel.
        const auto wheel = s.addMacro("Mod Wheel");
        MacroTarget t; t.kind = MacroTarget::Kind::PartLevel; t.part = pa; t.curve = MacroTarget::Curve::Logarithmic; t.start = .707; t.end = .0021;
        s.setMacroTargets(wheel, {t});
        s.assign("wheel:1", {Assignment::Kind::Control, 0, 0, wheel});
        s.assign("knob:2", {Assignment::Kind::Parameter, a, 7, 0});
        // An SMK-like controller: two faders, two knobs (the second relative), the mod wheel,
        // latching and momentary mute buttons, and two knobs in two rows of their own numbers.
        auto knobs = group(Role::Assign, "knobs", 2, {48, 49}); knobs.midi.encodings = {Encoding::Absolute, Encoding::TwosComplement};
        auto mod = group(Role::Assign, "wheels", 1, {1}); mod.functions = {"modulation"};
        auto latching = group(Role::Assign, "buttons", 2, {30, 31}); latching.midi.latch = true;
        auto momentary = group(Role::Assign, "buttons", 2, {40, 41});
        auto rows = group(Role::Assign, "knobs", 2, {17, 18, 21, 22}); rows.rows = {{"Assign 1", BankKind::Assign}, {"Assign 2", BankKind::Assign}};
        s.setControllers({group(Role::Assign, "faders", 2, {68, 69}), knobs, mod, latching, momentary, rows}, {{"SMK-37 Elite -Master", "smk", 1}});
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, smk, "SMK-37 Elite -Master"}, {MidiEndpoint::Kind::Usb, other, "Another"}};
        s.setHostStatus(status);
        s.service();
        s.note(true, 60, 1); render(s); render(s);
        check(near(render(s), .75), "both parts sound: A .5, B .25");
        const auto plans = s.plansCompiled();

        // A fader: picked up at its value, then followed, heard at the next block, before the
        // control thread has run at all.
        cc(s, 68, 127); cc(s, 68, 64);
        const double level = 64 / 127.0;
        check(near(render(s), .5 * level + .25), "fader 1 moves A's level within one block");
        check(s.parts()[0].level == 1, "with no control thread on the way: the model has not seen it yet");
        s.service();
        check(near(s.parts()[0].level, level), "then the control thread brings the model up to date");
        const auto state = s.controllerState();
        check(state.touched == "fader:1" && state.touches == 2, "the control moved last, for learn: " + state.touched);
        cc(s, 68, 127, other); check(near(render(s), .5 * level + .25), "another input's message is not the controller's");

        // Pickup: moved on screen, the fader waits until it reaches or crosses the new value.
        s.setPartLevel(pa, .2, false); s.commit(); s.service(); render(s);
        cc(s, 68, 100); check(near(render(s), .5 * .2 + .25), "picked up: the fader away from the new level does nothing");
        cc(s, 68, 10); check(near(render(s), .5 * 10 / 127.0 + .25), "crossing it, the fader takes over");

        // The mod wheel through its control: A's level by the control's curve, at the next block.
        cc(s, 1, 0); cc(s, 1, 127);
        check(near(render(s), .5 * (t.start + (t.end - t.start) * t.shape(1)) + .25), "the wheel's control sets A's level within one block: " + std::to_string(render(s)));
        s.service();
        double value = -1; for(const auto& m : s.graphState().macros) if(m.id == wheel) value = m.value;
        check(value == 1 && near(s.parts()[0].level, t.start + (t.end - t.start) * t.shape(1)), "the control's value and A's level in the model");
        const double quiet = .5 * s.parts()[0].level;

        // Buttons: a latching one sets B's mute from its state; a momentary one toggles it on each press.
        cc(s, 31, 127); check(near(render(s), quiet), "a latching button mutes B within one block");
        cc(s, 31, 0); check(near(render(s), quiet + .25), "and unmutes it when released");
        s.service();
        cc(s, 41, 127); check(near(render(s), quiet), "a momentary button's press mutes B");
        cc(s, 41, 0); check(near(render(s), quiet), "its release does nothing");
        s.service();
        cc(s, 41, 127); check(near(render(s), quiet + .25), "the next press unmutes it");
        check(s.plansCompiled() == plans, "no plan compiled for any of it: " + std::to_string(s.plansCompiled() - plans));

        // A relative knob on a parameter: a step of 1/127 per unit from where it is, set by the
        // control thread (the plugin's parameters are set there).
        s.service();
        const auto info = s.parameterInfo(a, 7);
        const double before = (s.parameter(a, 7) - info.minimum) / (info.maximum - info.minimum);
        cc(s, 49, 3);
        s.service();
        const double after = (s.parameter(a, 7) - info.minimum) / (info.maximum - info.minimum);
        check(near(after, before + 3 / 127.0), "a relative knob steps its parameter by three: " + std::to_string(after - before));
        cc(s, 49, 126); s.service(); // two's complement: 126 is two steps down
        check(near((s.parameter(a, 7) - info.minimum) / (info.maximum - info.minimum), before + 1 / 127.0), "and back by two");

        // A row's own numbers move its group to that row.
        cc(s, 21, 64); s.service();
        const auto positions = s.controllerState().positions;
        check(positions.size() == 6 && positions[5] == 1, "a row's number moves its group to the second row");
        Control::ControlView view; view.positions = {{5, 0}};
        s.setControlView(view);
        check(s.controllerState().positions[5] == 0, "and the screen moves it back");
        bool refused = false; view.positions = {{6, 0}}; try { s.setControlView(view); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a group that is not there is refused");
    }
}

int main(int argc, char** argv)
{
    if(argc < 2) { std::fprintf(stderr, "usage: %s FIXTURE\n", argv[0]); return 2; }
    try { mapping(argv[1]); }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a controller's MIDI into the engine moves its controls' targets within one block");
    return failures ? 1 : 0;
}

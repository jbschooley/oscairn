// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011's instant controls: a controller's MIDI into the engine moves what its control plays
// within one block, with no control thread, socket or plan compile on the way; and pickup,
// latching and momentary buttons, relative encoders and groups' rows behave as the app's
// mapping did; and, with the engine's threads as they run, how long a move takes to be heard
// (MIDI to the rendered level, paced like an audio device, judged in blocks rendered) and to
// reach a client's cached values over the socket (what a UI shows, judged against the clock).
#include "oscairn/session.h"
#include "oscairn/local_socket.h"
#include "oscairn/remote_control.h"
#include "AllocationGuard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fcntl.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <thread>
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

    // Identical controllers (RFC 0024): two inputs with one name are both the controller's port and
    // both play; set to another channel, the second is a profile of its own, with controls of its
    // own (numbered on) and so assignments of its own.
    void identical(const char* fixture)
    {
        Session s(48000, frames);
        const auto a = s.add("a", fixture, "org.kick.test.synth"), b = s.add("b", fixture, "org.kick.test.synth");
        s.setParameter(a, 7, .5); s.setParameter(b, 7, .25);
        const auto pa = s.addPart("A"), pb = s.addPart("B");
        s.insertIntoPart(pa, 0, a); s.insertIntoPart(pb, 0, b); s.commit();
        constexpr uint8_t again = 7; // the second input of the same name
        auto first = group(Role::Assign, "faders", 2, {68, 69}), second = first;
        second.midi.device = "smk-2"; second.midi.channel = 2;
        const std::string name = "SMK-37 Elite -Master";
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, smk, name}, {MidiEndpoint::Kind::Usb, again, name}};
        s.setHostStatus(status);
        // One channel: one device, both inputs playing the same controls.
        s.setControllers({first}, {{name, "smk", 1}});
        s.service();
        s.note(true, 60, 1); render(s);
        cc(s, 68, 127, smk); cc(s, 68, 64, smk);
        check(near(render(s), .5 * 64 / 127.0 + .25), "the first input's fader moves A");
        cc(s, 68, 32, again);
        check(near(render(s), .5 * 32 / 127.0 + .25) && s.controllerState().touched == "fader:1", "the second input of that name moves the same fader");
        // Two channels: two profiles chosen for the name, the second's faders numbered on.
        s.setControllers({first, second}, {{name, "smk", 1}, {name, "smk-2", 1}});
        s.assign("fader:3", {Assignment::Kind::PartLevel, 0, 0, pb});
        s.service();
        const uint8_t onTwo[3]{0xB1, 68, 0};
        s.controllerMidi(again, onTwo, 3); // B is at 1: picked up only at its value
        const uint8_t up[3]{0xB1, 68, 127}, half[3]{0xB1, 68, 64};
        s.controllerMidi(again, up, 3); s.controllerMidi(again, half, 3);
        check(near(render(s), .5 * 32 / 127.0 + .25 * 64 / 127.0) && s.controllerState().touched == "fader:3",
              "on channel 2, the second device's fader is its own control, with its own assignment: B");
        cc(s, 68, 32, again);
        check(near(render(s), .5 * 32 / 127.0 + .25 * 64 / 127.0), "and on channel 1 the first's still plays A, from either input");
    }

    using Clock = std::chrono::steady_clock;
    double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
    struct Spread { double median = 0, worst = 0; };
    Spread spread(std::vector<double> v) { std::sort(v.begin(), v.end()); return v.empty() ? Spread{} : Spread{v[v.size() / 2], v.back()}; }

    // Each block's last samples, as the audio thread renders them, kept by block number. Latency
    // is judged in blocks, not milliseconds: a machine that stops running the audio thread (a
    // CI runner's 35 ms gaps) renders no blocks meanwhile, while a move held up in the engine
    // lets blocks go by without it.
    struct Blocks
    {
        static constexpr size_t size = 4096; // about 11 s at 128 frames; a waiter reads within 1 s
        std::array<std::atomic<float>, size> left{}, right{};
        std::atomic<uint64_t> count{0};
        void push(float l, float r)
        {
            const auto n = count.load(std::memory_order_relaxed);
            left[n % size].store(l, std::memory_order_relaxed); right[n % size].store(r, std::memory_order_relaxed);
            count.store(n + 1, std::memory_order_release);
        }
        // From block `from` on: how many blocks until the first whose last samples pass `ok`
        // (1: the first block to end after `from` was read), or 0 while none has.
        template<typename F> uint64_t until(uint64_t from, uint64_t& next, F ok) const
        {
            if(next < from) next = from;
            for(const auto n = count.load(std::memory_order_acquire); next < n; ++next)
                if(ok(left[next % size].load(std::memory_order_relaxed), right[next % size].load(std::memory_order_relaxed))) return next - from + 1;
            return 0;
        }
    };
    std::string worstOf(const std::vector<uint64_t>& v) { return std::to_string(v.empty() ? 0 : *std::max_element(v.begin(), v.end())); }
    uint64_t medianOf(std::vector<uint64_t> v) { std::sort(v.begin(), v.end()); return v.empty() ? 0 : v[v.size() / 2]; }

    // The engine's threads as they run, with an audio device's pacing (128 frames at 48 kHz),
    // its control thread (the socket polled and service() every 5 ms, woken at once by a
    // control's move), a MIDI thread and a client over the socket. A move of the mod wheel plays
    // a control whose target is a part's level, as on "Drifting". Measured: from the MIDI thread
    // handing the message over to the first block whose output has the new level (heard, in
    // blocks rendered; its milliseconds are printed), and to the client's cached value having it
    // (what a UI shows at its next frame, against the clock). Timing is judged on the best of
    // three runs (main), with CTest running nothing beside it (RUN_SERIAL): a slower path (an
    // extra block) fails every run, other load on the machine seldom three in a row.
    // A run that misses a limit says so and, unless it is the last, counts no failure.
    bool latency(const char* fixture, bool last)
    {
        int missed = 0;
        auto timing = [&](bool ok, const std::string& what) {
            if(ok) return;
            ++missed;
            if(last) check(false, what); else std::fprintf(stderr, "slow, trying again: %s\n", what.c_str());
        };
        constexpr uint32_t block = 128;
        constexpr double rate = 48000, period = block / rate;
        Session s(rate, block);
        const auto a = s.add("a", fixture, "org.kick.test.synth"); s.setParameter(a, 7, 1);
        const auto pa = s.addPart("A"); s.insertIntoPart(pa, 0, a); s.commit();
        const auto wheel = s.addMacro("Mod Wheel");
        MacroTarget t; t.kind = MacroTarget::Kind::PartLevel; t.part = pa; t.curve = MacroTarget::Curve::Logarithmic; t.start = .707; t.end = .0021;
        s.setMacroTargets(wheel, {t});
        s.assign("wheel:1", {Assignment::Kind::Control, 0, 0, wheel});
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, smk, "SMK-37 Elite -Master"}};
        s.setHostStatus(status);
        std::atomic<bool> woken{false}, stop{false};
        s.setControllerWake([&] { woken = true; });
        LocalServer server(s);
        std::array<int, 2> fds{};
        check(socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) == 0, "socketpair");
        for(auto fd : fds) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        server.adopt(fds[0]);
        s.note(true, 60, 1);

        // Audio thread: a block every period, its last sample kept with when it was rendered.
        std::atomic<float> heard{0};
        std::atomic<int64_t> heardAt{0}, longestGap{0}; // the longest time between two blocks' ends
        Blocks blocks;
        std::thread audio([&] {
            std::array<float, block> l{}, r{};
            float* out[2]{l.data(), r.data()};
            auto next = Clock::now();
            while(!stop)
            {
                forbidAllocation = true;
                s.process(nullptr, 0, out, 2, block);
                forbidAllocation = false;
                const auto now = Clock::now().time_since_epoch().count();
                if(heardAt) longestGap = std::max<int64_t>(longestGap, now - heardAt);
                heard = l[block - 1]; heardAt = now;
                blocks.push(l[block - 1], r[block - 1]);
                next += std::chrono::nanoseconds(int64_t(period * 1e9));
                std::this_thread::sleep_until(next);
            }
        });
        // The client: the controllers set over the socket, then its cache watched.
        std::atomic<bool> ready{false};
        std::atomic<double> shown{-1};
        std::atomic<int64_t> shownAt{0};
        std::thread client([&] {
            try
            {
                LocalClient c; c.adopt(fds[1]); RemoteControl remote(c);
                auto mod = group(Role::Assign, "wheels", 1, {1}); mod.functions = {"modulation"};
                remote.setControllers({mod}, {{"SMK-37 Elite -Master", "smk", 1}});
                ready = true;
                while(!stop)
                {
                    remote.service();
                    for(const auto& p : remote.liveValues().parts) if(p.part == pa && p.level != shown) { shown = p.level; shownAt = Clock::now().time_since_epoch().count(); }
                    std::this_thread::sleep_for(std::chrono::microseconds(200));
                }
            }
            catch(const std::exception& e) { check(false, std::string("client: ") + e.what()); ready = true; }
        });
        // The MIDI thread: moves a while apart, at times that fall anywhere in a block.
        std::vector<double> toSound, toClient;
        std::vector<uint64_t> blocksToSound;
        std::atomic<bool> done{false};
        std::thread midi([&] {
            while(!ready) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the controllers in the snapshot
            cc(s, 1, 0); std::this_thread::sleep_for(std::chrono::milliseconds(50)); // picked up
            std::mt19937 random(7);
            for(int i = 0; i < 60; ++i)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(40000 + random() % 3000));
                const uint8_t value = i % 2 ? 0 : 127;
                const double level = t.start + (t.end - t.start) * t.shape(value / 127.0);
                const auto from = blocks.count.load(std::memory_order_acquire);
                const auto sent = Clock::now();
                cc(s, 1, value);
                const auto since = sent.time_since_epoch().count();
                const auto deadline = sent + std::chrono::seconds(1); // a wait, not a limit
                bool sound = false, ui = false, rendered = false;
                uint64_t next = 0;
                while(Clock::now() < deadline && (!sound || !ui || !rendered))
                {
                    if(!rendered) if(const auto n = blocks.until(from, next, [&](float l, float) { return std::abs(l - level) < 1e-4; })) { rendered = true; blocksToSound.push_back(n); }
                    if(!sound && heardAt > since && std::abs(heard - level) < 1e-4) { sound = true; toSound.push_back(ms(Clock::duration(heardAt - since))); }
                    if(!ui && shownAt > since && std::abs(shown - level) < 1e-9) { ui = true; toClient.push_back(ms(Clock::duration(shownAt - since))); }
                    std::this_thread::yield();
                }
                timing(rendered && ui, "move " + std::to_string(i) + " heard and shown within 1 s");
            }
            done = true;
        });
        // The control thread.
        auto tick = Clock::now();
        while(!done)
        {
            if(woken.exchange(false)) s.serviceControllers();
            if(Clock::now() >= tick)
            {
                server.poll(uint64_t(Clock::now().time_since_epoch().count()));
                s.service();
                tick += std::chrono::milliseconds(5);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        stop = true;
        midi.join(); client.join(); audio.join();
        s.setControllerWake({});
        const auto sound = spread(toSound), ui = spread(toClient);
        std::printf("latency: MIDI to the rendered level, median %llu blocks, worst %s (a block is %.2f ms); median %.2f ms, worst %.2f ms, over %zu moves\n",
                    (unsigned long long)medianOf(blocksToSound), worstOf(blocksToSound).c_str(), period * 1000, sound.median, sound.worst, toSound.size());
        std::string slow;
        for(size_t i = 0; i < blocksToSound.size(); ++i) if(blocksToSound[i] > 2) slow += " " + std::to_string(i) + ": " + std::to_string(blocksToSound[i]) + ";";
        std::printf("latency: the longest time between blocks %.2f ms; heard after 2 blocks:%s\n", ms(Clock::duration(longestGap.load())), slow.empty() ? " none" : slow.c_str());
        std::printf("latency: MIDI to a client's cached value, median %.2f ms, worst %.2f ms (a display frame is 16.67 ms), over %zu moves\n", ui.median, ui.worst, toClient.size());
        // Within about one block (a move landing just after a block starts waits for the next);
        // generous on the worst, for a loaded test machine (3 blocks: the 10 ms this once was).
        timing(medianOf(blocksToSound) <= 2, "heard within about a block (median): " + std::to_string(medianOf(blocksToSound)));
        timing(blocksToSound.size() == 60 && std::stoull(worstOf(blocksToSound)) <= 3, "heard within 3 blocks, every move: " + worstOf(blocksToSound));
        timing(ui.worst <= 1000 / 60.0, "a client has it within a display frame, every move");
        return !missed;
    }

    // A part's pan, played two ways with the engine's threads as above: the mod wheel moving a
    // morph control between a scene panned left and one panned right (the control thread, woken
    // at once, blends the scenes), and a scene with a 20 ms glide selected on the control thread
    // as a scene button's move is. Measured in blocks rendered, as above: from the move to the
    // first block with the new right-channel level (the morph), and to the glide's first step
    // heard and its end.
    void panLatency(const char* fixture)
    {
        constexpr uint32_t block = 128;
        constexpr double rate = 48000, period = block / rate;
        Session s(rate, block);
        const auto a = s.add("a", fixture, "org.kick.test.synth"); s.setParameter(a, 7, 1);
        const auto pa = s.addPart("A"); s.insertIntoPart(pa, 0, a); s.commit();
        s.setPartPan(pa, -1); s.commit(); const auto left = s.addScene("Left");
        s.setPartPan(pa, 1); s.commit(); const auto right = s.addScene("Right");
        const auto morph = s.addMacro("Morph");
        MacroTarget t; t.kind = MacroTarget::Kind::Morph; t.sceneFrom = left; t.sceneTo = right;
        s.setMacroTargets(morph, {t});
        s.assign("wheel:1", {Assignment::Kind::Control, 0, 0, morph});
        Control::SessionStatus status; status.midiInputs = {{MidiEndpoint::Kind::Usb, smk, "SMK-37 Elite -Master"}};
        s.setHostStatus(status);
        auto mod = group(Role::Assign, "wheels", 1, {1}); mod.functions = {"modulation"};
        s.setControllers({mod}, {{"SMK-37 Elite -Master", "smk", 1}});
        s.setPartPan(pa, 0); s.commit(); s.service();
        std::atomic<bool> woken{false}, stop{false};
        s.setControllerWake([&] { woken = true; });
        s.note(true, 60, 1);
        std::atomic<float> heard{0}, heardLeft{0};
        std::atomic<int64_t> heardAt{0};
        Blocks blocks;
        std::thread audio([&] {
            std::array<float, block> l{}, r{};
            float* out[2]{l.data(), r.data()};
            auto next = Clock::now();
            while(!stop)
            {
                forbidAllocation = true;
                s.process(nullptr, 0, out, 2, block);
                forbidAllocation = false;
                heardLeft = l[block - 1]; heard = r[block - 1]; heardAt = Clock::now().time_since_epoch().count();
                blocks.push(l[block - 1], r[block - 1]);
                next += std::chrono::nanoseconds(int64_t(period * 1e9));
                std::this_thread::sleep_until(next);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const float centre = heard; // pan 0: the right channel at full
        std::vector<double> toSound, firstStep, glideEnd;
        std::vector<uint64_t> blocksToSound, blocksToStep, blocksToEnd;
        std::atomic<bool> done{false};
        std::thread midi([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            cc(s, 1, 0); std::this_thread::sleep_for(std::chrono::milliseconds(50)); // picked up
            std::mt19937 random(7);
            for(int i = 0; i < 40; ++i)
            {
                std::this_thread::sleep_for(std::chrono::microseconds(40000 + random() % 3000));
                const uint8_t value = i % 2 ? 0 : 127;
                const float goal = value ? centre : 0.f; // panned right: full; left: none
                const auto from = blocks.count.load(std::memory_order_acquire);
                const auto sent = Clock::now();
                cc(s, 1, value);
                const auto since = sent.time_since_epoch().count();
                bool sound = false, rendered = false;
                uint64_t next = 0;
                while(Clock::now() < sent + std::chrono::seconds(1) && !rendered)
                {
                    if(const auto n = blocks.until(from, next, [&](float, float r) { return std::abs(r - goal) < 1e-4; })) { rendered = true; blocksToSound.push_back(n); }
                    if(!sound && heardAt > since && std::abs(heard - goal) < 1e-4) { sound = true; toSound.push_back(ms(Clock::duration(heardAt - since))); }
                    std::this_thread::yield();
                }
                check(rendered, "morph move " + std::to_string(i) + " heard within 1 s");
            }
            done = true;
        });
        auto tick = Clock::now();
        auto serve = [&] {
            if(woken.exchange(false)) s.serviceControllers();
            if(Clock::now() >= tick) { s.service(); tick += std::chrono::milliseconds(5); }
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        };
        while(!done) serve();
        midi.join();
        // The glides, from the control thread, as a scene button's move is applied there. Each is
        // watched on the channel it fades in, for its first step (the balance moves the other only
        // past the centre), and the one it fades out, for its end.
        s.setSceneTransition(left, 20, Scene::Curve::Linear, Scene::Launch::Now);
        s.setSceneTransition(right, 20, Scene::Curve::Linear, Scene::Launch::Now);
        for(int i = 0; i < 20; ++i)
        {
            const auto until = Clock::now() + std::chrono::milliseconds(30);
            while(Clock::now() < until) serve();
            const bool toRight = i % 2 == 0;
            auto channel = [&] { return toRight ? heard.load() : heardLeft.load(); };
            auto other = [&] { return toRight ? heardLeft.load() : heard.load(); };
            const float from = channel();
            const auto start = blocks.count.load(std::memory_order_acquire);
            const auto sent = Clock::now(); const auto since = sent.time_since_epoch().count();
            s.selectScene(toRight ? right : left);
            bool moved = false, arrived = false;
            uint64_t stepped = 0, ended = 0, next = 0, nextEnd = 0;
            auto in = [&](float l, float r) { return toRight ? r : l; };
            auto out = [&](float l, float r) { return toRight ? l : r; };
            while(Clock::now() < sent + std::chrono::seconds(1) && !ended)
            {
                serve();
                if(!stepped) stepped = blocks.until(start, next, [&](float l, float r) { return std::abs(in(l, r) - from) > 1e-3; });
                if(stepped) ended = blocks.until(start + stepped - 1, nextEnd, [&](float l, float r) { return std::abs(out(l, r)) < 1e-4; });
                if(!moved && heardAt > since && std::abs(channel() - from) > 1e-3) { moved = true; firstStep.push_back(ms(Clock::duration(heardAt - since))); }
                if(moved && !arrived && std::abs(other()) < 1e-4) { arrived = true; glideEnd.push_back(ms(Clock::duration(heardAt - since))); }
            }
            if(stepped) blocksToStep.push_back(stepped);
            if(ended) blocksToEnd.push_back(stepped - 1 + ended);
            check(ended, "glide " + std::to_string(i) + " arrives within 1 s");
        }
        stop = true;
        audio.join();
        s.setControllerWake({});
        const auto sound = spread(toSound), step = spread(firstStep), end = spread(glideEnd);
        std::printf("pan latency: MIDI to a morph's pan heard, median %llu blocks, worst %s (a block is %.2f ms); median %.2f ms, worst %.2f ms, over %zu moves\n",
                    (unsigned long long)medianOf(blocksToSound), worstOf(blocksToSound).c_str(), period * 1000, sound.median, sound.worst, toSound.size());
        std::printf("pan latency: a scene's 20 ms glide, first step heard median %llu blocks, worst %s; its end median %llu, worst %s; in ms: first step median %.2f, worst %.2f; end median %.2f, worst %.2f, over %zu glides\n",
                    (unsigned long long)medianOf(blocksToStep), worstOf(blocksToStep).c_str(), (unsigned long long)medianOf(blocksToEnd), worstOf(blocksToEnd).c_str(), step.median, step.worst, end.median, end.worst, firstStep.size());
        // Within a service tick and a block or two; generous on the worst, for a loaded test machine
        // (7 blocks: the 20 ms this once was).
        check(blocksToSound.size() == 40 && std::stoull(worstOf(blocksToSound)) <= 7, "a morph's pan heard within 7 blocks, every move: " + worstOf(blocksToSound));
        check(blocksToStep.size() == 20 && std::stoull(worstOf(blocksToStep)) <= 7, "a glide's first step heard within 7 blocks, every glide: " + worstOf(blocksToStep));
        check(blocksToEnd.size() == 20 && std::stoull(worstOf(blocksToEnd)) <= 30, "a glide's end heard within 30 blocks (the 80 ms this once was), every glide: " + worstOf(blocksToEnd));
    }
}

int main(int argc, char** argv)
{
    if(argc < 2) { std::fprintf(stderr, "usage: %s FIXTURE\n", argv[0]); return 2; }
    try { mapping(argv[1]); identical(argv[1]); for(int run = 1; run <= 3 && !latency(argv[1], run == 3); ++run) {} panLatency(argv[1]); }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a controller's MIDI into the engine moves its controls' targets within one block");
    return failures ? 1 : 0;
}

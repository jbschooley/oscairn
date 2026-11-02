// SPDX-License-Identifier: AGPL-3.0-or-later
//
// oscairn-midi-monitor: lists the MIDI inputs and prints every message from all of them, one line
// each (port, channel, type, number, value), then, on Enter or Ctrl-C, a table with a row per
// control: how often it sent, its lowest and highest value and the values it sent. Move each
// control of a controller in turn and paste the table back to fill in its profile (RFC 0024).
// --seconds N listens for N seconds instead of waiting for Enter, for running it where there is no
// keyboard input (a shell that runs one command and returns its output).
//   oscairn-midi-monitor [--list | --seconds N | --self-test]
#include <juce_audio_devices/juce_audio_devices.h>

#include <csignal>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace
{
    struct Event { std::string port; int channel = 0; std::string type; int number = -1; int value = 0; }; // number -1: none

    // A message as the monitor shows it; nothing for clock, active sensing and other real-time bytes.
    std::optional<Event> describe(const std::string& port, const juce::MidiMessage& m)
    {
        Event e; e.port = port; e.channel = m.getChannel();
        if(m.isNoteOn()) { e.type = "note-on"; e.number = m.getNoteNumber(); e.value = m.getVelocity(); }
        else if(m.isNoteOff()) { e.type = "note-off"; e.number = m.getNoteNumber(); e.value = m.getVelocity(); }
        else if(m.isController()) { e.type = "cc"; e.number = m.getControllerNumber(); e.value = m.getControllerValue(); }
        else if(m.isPitchWheel()) { e.type = "pitch-bend"; e.value = m.getPitchWheelValue() - 8192; }
        else if(m.isAftertouch()) { e.type = "poly-pressure"; e.number = m.getNoteNumber(); e.value = m.getAfterTouchValue(); }
        else if(m.isChannelPressure()) { e.type = "pressure"; e.value = m.getChannelPressureValue(); }
        else if(m.isProgramChange()) { e.type = "program"; e.number = m.getProgramChangeNumber(); }
        else if(m.isSysEx()) { e.channel = 0; e.type = "sysex"; e.value = m.getSysExDataSize(); }
        else return std::nullopt;
        return e;
    }

    std::string line(const Event& e)
    {
        char text[512];
        std::snprintf(text, sizeof text, "%-28s ch %-2d %-13s %-4s %d", e.port.c_str(), e.channel, e.type.c_str(), e.number < 0 ? "-" : std::to_string(e.number).c_str(), e.value);
        return text;
    }

    // A row per control: port, channel, type and number.
    class Summary
    {
    public:
        void add(const Event& e)
        {
            auto& s = rows_[{e.port, e.channel, e.type, e.number}];
            if(!s.count++) s.low = s.high = e.value;
            s.low = std::min(s.low, e.value); s.high = std::max(s.high, e.value);
            s.values.insert(e.value);
        }
        std::string table() const
        {
            std::string out = "port                         ch type          num  count  low  high values\n";
            for(const auto& [k, s] : rows_)
            {
                const auto& [port, channel, type, number] = k;
                char row[512];
                std::snprintf(row, sizeof row, "%-28s %-2d %-13s %-4s %-6d %-4d %-4d", port.c_str(), channel, type.c_str(), number < 0 ? "-" : std::to_string(number).c_str(), s.count, s.low, s.high);
                std::string values;
                size_t shown = 0;
                for(const auto v : s.values) { if(shown++ == 24) { values += " ..."; break; } values += " " + std::to_string(v); }
                out += row + values + "\n";
            }
            return out;
        }
    private:
        struct Stats { int count = 0, low = 0, high = 0; std::set<int> values; };
        std::map<std::tuple<std::string, int, std::string, int>, Stats> rows_;
    };

    class Monitor : public juce::MidiInputCallback
    {
    public:
        void handleIncomingMidiMessage(juce::MidiInput* input, const juce::MidiMessage& m) override
        {
            const auto e = describe(input->getName().toStdString(), m);
            if(!e) return;
            std::lock_guard lock(lock_); // inputs call from threads of their own
            std::printf("%s\n", line(*e).c_str());
            std::fflush(stdout);
            summary_.add(*e);
        }
        std::string table() { std::lock_guard lock(lock_); return summary_.table(); }
    private:
        std::mutex lock_;
        Summary summary_;
    };

    int selfTest()
    {
        int failures = 0;
        auto check = [&](bool ok, const char* what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } };
        const auto cc = describe("SMK37 Elite", juce::MidiMessage::controllerEvent(1, 74, 65));
        check(cc && line(*cc) == "SMK37 Elite                  ch 1  cc            74   65", "a CC's line");
        const auto bend = describe("SMK37 Elite", juce::MidiMessage::pitchWheel(1, 8192 + 100));
        check(bend && bend->type == "pitch-bend" && bend->value == 100 && bend->number == -1, "pitch bend, centered on 0");
        const auto pad = describe("SMK37 Elite", juce::MidiMessage::noteOn(10, 36, (juce::uint8)100));
        check(pad && pad->type == "note-on" && pad->channel == 10 && pad->number == 36 && pad->value == 100, "a pad's note");
        check(!describe("SMK37 Elite", juce::MidiMessage::midiClock()), "clock is left out");
        Summary s;
        for(const int v : {1, 1, 65, 69}) s.add({"SMK37 Elite", 1, "cc", 19, v});
        const auto table = s.table();
        check(table.find("SMK37 Elite                  1  cc            19   4      1    69   1 65 69") != std::string::npos, "a control's row: count, low, high, values");
        if(!failures) std::puts("ok: the MIDI monitor's lines and table");
        return failures ? 1 : 0;
    }

    void stop(int) {} // only interrupts the wait for Enter
}

int main(int argc, char** argv)
{
    const std::string option = argc > 1 ? argv[1] : "";
    if(option == "--self-test") return selfTest();
    int seconds = 0;
    if(option == "--seconds" && argc == 3) seconds = std::atoi(argv[2]);
    if((!option.empty() && option != "--list" && seconds <= 0) || seconds > 3600) { std::fprintf(stderr, "usage: %s [--list | --seconds N | --self-test]\n", argv[0]); return 2; }
    juce::ScopedJuceInitialiser_GUI juce; // JUCE's message manager, which device lists may need
    const auto devices = juce::MidiInput::getAvailableDevices();
    std::printf("MIDI inputs:\n");
    for(const auto& d : devices) std::printf("  %s\n", d.name.toRawUTF8());
    if(devices.isEmpty()) std::printf("  (none)\n");
    if(option == "--list") return 0;
    Monitor monitor;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs;
    for(const auto& d : devices)
        if(auto input = juce::MidiInput::openDevice(d.identifier, &monitor)) { input->start(); inputs.push_back(std::move(input)); }
        else std::fprintf(stderr, "could not open %s\n", d.name.toRawUTF8());
    if(seconds) std::printf("\nMove each control, one at a time; the table comes in %d seconds.\n\n", seconds);
    else std::printf("\nMove each control, one at a time; press Enter (or Ctrl-C) for the table.\n\n");
    std::fflush(stdout);
    struct sigaction action{};
    action.sa_handler = stop; // no SA_RESTART: Ctrl-C ends the wait below
    sigaction(SIGINT, &action, nullptr);
    if(seconds) std::this_thread::sleep_for(std::chrono::seconds(seconds)); // MIDI arrives on JUCE's own threads meanwhile
    else { std::string ignored; std::getline(std::cin, ignored); }
    for(auto& input : inputs) input->stop();
    std::printf("\n%s", monitor.table().c_str());
    return 0;
}

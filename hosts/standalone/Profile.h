// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace oscairn::standalone
{
    /// A device profile (RFC 0024): one product, described once, read by the app and the engine
    /// instead of flags. A device's image names its profile (--profile); the desktop app makes
    /// one of the computer it runs on.
    struct Profile
    {
        std::string id, name;
        struct Platform { std::string os, cpu, runtime, board; } platform;
        // memory and storage in MiB; osMemory and performanceMemory (RFC 0025's memory classes, both
        // or neither): what the OS and everything beside Oscairn need, and the floor for Performances.
        struct Compute { int cores = 0, realtimeCores = 0, memory = 0, storage = 0, osMemory = 0, performanceMemory = 0; } compute;
        struct Audio { int inputs = 0, outputs = 2; std::vector<int> rates; int block = 0; bool headphones = false; } audio; // block 0: the build's default
        // A group of physical controls (RFC 0024, RFC 0011's physical controls): its kind and
        // count, and its role: notes (keys, pads), screen (following what the screen shows),
        // assign (playing banks), fixed (a dedicated function, named in `functions`, one each).
        // An assign group of knobs has hardware `rows` (each a bank its row button picks, named on
        // screen) or `paging` (button or screen); faders may have `modes` (part, element, audio).
        struct Row { std::string name, bank; }; // bank: assign, filter, ampEG, eq, levels or effects
        // An external controller's group says how it talks MIDI (RFC 0024's controllers): its port
        // (from 1), channel (1 to 16; 0: no MIDI described; given as a list, the first control's,
        // with `channels` one per control) and a CC or note per control (per control per row, for
        // rows whose switch changes the numbers), or pitch bend; each CC's encoding ("steps" for
        // explicit increment and decrement values, in `steps`); how buttons send (momentary or latch).
        struct ControlMidi
        {
            int port = 1, channel = 0; std::vector<int> channels, cc, notes; bool pitchBend = false;
            std::vector<std::string> encodings; std::vector<std::array<int, 2>> steps; // steps: one per control, {0, 0} for a named encoding
            std::string button = "momentary";
            bool feedback = false; // the controller takes each control's value back on its CC (absolute CCs only); off unless confirmed
            int channelOf(size_t control) const { return control < channels.size() ? channels[control] : channel; }
        };
        // `target`: what an assign group plays by default (banks, partLevels, partMutes, scenes;
        // empty: its kind's default). `functions` on an assign group: standard controllers passed
        // to every part (pitchBend, modulation, sustain...).
        struct Control
        {
            std::string kind; int count = 0; std::string sensing, role;
            std::vector<Row> rows; std::string paging; std::vector<std::string> modes, functions;
            std::string target; ControlMidi midi;
        };
        std::vector<Control> controls;
        // A screen: its size, touch, the surface drawn on it (RFC 0019) and that surface's layout.
        struct Screen { int width = 0, height = 0; bool touch = false; std::string surface = "desktop", layout = "desktop"; };
        std::vector<Screen> screens;
        // A controller's ports: an input matches when its name contains `name`, ignoring case;
        // `index` picks the Nth such input; `protocol` midi, or mackie (Mackie Control, read by
        // control_surface.h); `ignore`: a duplicate of another port, whose messages the engine
        // leaves out; `display`: a Mackie surface with scribble strips, which get the parts' names.
        // The output of the same name and index takes what goes back to the controller.
        struct Port { std::string name; int index = 1; std::string protocol = "midi"; bool ignore = false, display = false; };
        struct Midi { int din = 0; bool usbDevice = false, usbHost = false; std::vector<Port> ports; } midi;
        struct Cv { int inputs = 0, outputs = 0; } cv;
        struct Network { bool ethernet = false, wifi = false; } network;
        bool battery = false;
        std::string reliability = "desktop"; // desktop, instrument or pass-through (RFC 0027)
        std::vector<std::string> apps;       // RFC 0022's apps installed with it
        // RFC 0025: the apps reserved whether they run or not, each in apps; absent from the file,
        // oscairn.performance and oscairn.library where apps lists them.
        std::vector<std::string> alwaysAvailable;
        std::vector<std::string> estimated;  // sections whose values are estimates, not measured on a unit

        bool offers(const std::string& app) const;
        // An external controller's profile: it has ports, and its controls join a device's while connected.
        bool controller() const { return !midi.ports.empty(); }
        // The engine's budget (RFC 0025): one audio thread's time less the reliability level's
        // margin (desktop 90%, instrument 75%, pass-through 60%), and 80% of its memory (none
        // checked when the profile does not say).
        double cpuBudget() const { return reliability == "pass-through" ? .6 : reliability == "instrument" ? .75 : .9; }
        uint64_t memoryBudget() const { return uint64_t(compute.memory) * (1ull << 20) * 8 / 10; }
        // RFC 0025's memory classes, with osMemory: the margin kept free, 5% of memory at every
        // level (the RFC's illustrative figure; the margins per level are open), in bytes.
        bool memoryClasses() const { return compute.osMemory > 0; }
        uint64_t memoryMargin() const { return uint64_t(compute.memory) * (1ull << 20) / 20; }
        // The memory an app reserves, in MiB: Oscairn's own apps only, until apps declare it in
        // their manifests (RFC 0025 step 8); 0 for any other.
        static int appMemory(const std::string& app);
        // The screen the app draws on: the first; a headless profile has none and draws nothing.
        const Screen* screen() const { return screens.empty() ? nullptr : &screens.front(); }

        // Strict, as themes and layouts are: unknown fields, wrong types and impossible values refuse it.
        static Profile parse(const std::string& json);
        // The computer the desktop app runs on: its cores and memory, its main display, mouse
        // and keyboard, desktop reliability, every app.
        static Profile desktop();
    };

    // The profiles that come with the app, by id: the QEMU image, the Behringer KICK, the Ignite, a
    // MODX M (estimated), and the controllers SMK37 Elite and MODX M over USB (estimated).
    std::vector<std::pair<std::string, std::string>> builtInProfiles(); // id, json
    // A built-in device profile by id, or a profile file by path; "desktop" (or nothing) makes the
    // desktop's. A controller's profile is refused here: it joins when its ports are connected.
    Profile loadProfile(const std::string& nameOrFile);
    // The built-in controllers' profiles, and those in `files`.
    std::vector<Profile> controllerProfiles(const std::vector<std::string>& files = {});
    // The musician's choice where several profiles match one port (RFC 0024): a profile's id by
    // the port's name, kept in the app's settings (controller-profiles.json in its configuration
    // folder; none when it is missing or unreadable).
    using ProfileChoices = std::map<std::string, std::string>;
    ProfileChoices readProfileChoices(const std::string& file);
    void writeProfileChoices(const std::string& file, const ProfileChoices& choices);
    // The profiles (their places) with a port that is the input at `input`, in the order given.
    std::vector<size_t> profilesFor(const std::vector<Profile>& controllers, const std::vector<std::string>& inputs, size_t input);
    // The profile an input is: the one chosen for its name if that one matches it, else the first
    // that does (controllerProfiles sorts them by id); controllers.size() for none.
    size_t profileOf(const std::vector<Profile>& controllers, const std::vector<std::string>& inputs, size_t input, const ProfileChoices& choices = {});
    // Which MIDI inputs (or outputs), by name in the system's order, are which controller's ports
    // of a protocol (midi or mackie): for each match, the input's place, the profile's place and
    // the port (from 1). Ignored ports are left out. An input several profiles match is profileOf's.
    struct PortMatch { size_t input = 0, profile = 0; int port = 1; };
    std::vector<PortMatch> matchPorts(const std::vector<Profile>& controllers, const std::vector<std::string>& inputs, const std::string& protocol = "midi", const ProfileChoices& choices = {});
    // The names of the inputs that are a controller's ignored ports (of the profile each input is):
    // duplicates the engine leaves out.
    std::vector<std::string> ignoredInputs(const std::vector<Profile>& controllers, const std::vector<std::string>& inputs, const ProfileChoices& choices = {});
}

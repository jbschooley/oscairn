// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
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
        struct Compute { int cores = 0, realtimeCores = 0, memory = 0, storage = 0; } compute; // memory and storage in MiB
        struct Audio { int inputs = 0, outputs = 2; std::vector<int> rates; int block = 0; bool headphones = false; } audio; // block 0: the build's default
        struct Control { std::string kind; int count = 0; std::string sensing; };
        std::vector<Control> controls;
        // A screen: its size, touch, the surface drawn on it (RFC 0019) and that surface's layout.
        struct Screen { int width = 0, height = 0; bool touch = false; std::string surface = "desktop", layout = "desktop"; };
        std::vector<Screen> screens;
        struct Midi { int din = 0; bool usbDevice = false, usbHost = false; } midi;
        struct Cv { int inputs = 0, outputs = 0; } cv;
        struct Network { bool ethernet = false, wifi = false; } network;
        bool battery = false;
        std::string reliability = "desktop"; // desktop, instrument or pass-through (RFC 0027)
        std::vector<std::string> apps;       // RFC 0022's apps installed with it
        std::vector<std::string> estimated;  // sections whose values are estimates, not measured on a unit

        bool offers(const std::string& app) const;
        // The engine's budget (RFC 0025): one audio thread's time less the reliability level's
        // margin (desktop 90%, instrument 75%, pass-through 60%), and 80% of its memory (none
        // checked when the profile does not say).
        double cpuBudget() const { return reliability == "pass-through" ? .6 : reliability == "instrument" ? .75 : .9; }
        uint64_t memoryBudget() const { return uint64_t(compute.memory) * (1ull << 20) * 8 / 10; }
        // The screen the app draws on: the first; a headless profile has none and draws nothing.
        const Screen* screen() const { return screens.empty() ? nullptr : &screens.front(); }

        // Strict, as themes and layouts are: unknown fields, wrong types and impossible values refuse it.
        static Profile parse(const std::string& json);
        // The computer the desktop app runs on: its cores and memory, its main display, mouse
        // and keyboard, desktop reliability, every app.
        static Profile desktop();
    };

    // The profiles that come with the app, by id: the QEMU image, the Behringer KICK, the Ignite.
    std::vector<std::pair<std::string, std::string>> builtInProfiles(); // id, json
    // A built-in profile by id, or a profile file by path; "desktop" (or nothing) makes the desktop's.
    Profile loadProfile(const std::string& nameOrFile);
}

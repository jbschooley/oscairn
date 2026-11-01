// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Device profiles (RFC 0024): the built-in ones parse and say what the app uses (layout,
// surface, block, apps); the desktop's is made from the computer; bad ones are refused; and a
// profile's block reaches the engine as an audio option.
#include "../Profile.h"
#include "../../audio/AudioConfiguration.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <cstdio>
#include <string>

using namespace oscairn::standalone;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    bool refused(const std::string& json) { try { Profile::parse(json); } catch(const std::invalid_argument&) { return true; } return false; }
    std::string replaced(std::string s, const std::string& from, const std::string& to)
    {
        const auto at = s.find(from);
        if(at == std::string::npos) { std::fprintf(stderr, "FAIL: test text not found: %s\n", from.c_str()); ++failures; return s; }
        return s.replace(at, from.size(), to);
    }
}

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    try
    {
        const auto all = builtInProfiles();
        check(all.size() == 3, "three built-in profiles: " + std::to_string(all.size()));
        for(const auto& [id, json] : all)
        {
            const auto p = Profile::parse(json);
            check(p.id == id && p.screen() && p.screen()->surface == "panel" && p.audio.block == 128 && p.reliability == "instrument", id + " parses as a panel instrument");
            check(p.offers("oscairn.library") && !p.offers("oscairn.nothing"), id + " offers its apps");
        }
        const auto kick = loadProfile("kick"), ignite = loadProfile("ignite"), qemu = loadProfile("qemu-aarch64");
        check(kick.name == "Behringer KICK" && kick.screen()->layout == "kick-screen" && kick.screen()->touch, "the KICK shows its screen layout, by touch");
        check(ignite.screen()->layout == "ignite" && ignite.cv.outputs > 0, "the Ignite shows its panel and has CV");
        check(!kick.estimated.empty() && qemu.estimated.empty(), "the KICK's values are estimates; QEMU's are its test setup");
        check(kick.cpuBudget() == .75 && kick.memoryBudget() == 2048ull * 1024 * 1024 * 8 / 10, "an instrument's budget: 75% of the audio thread, 80% of its memory");
        const auto desktop = loadProfile("");
        check(desktop.id == "desktop" && desktop.compute.cores >= 1 && desktop.screen() && desktop.screen()->surface == "desktop" && desktop.screen()->layout == "desktop" &&
              desktop.reliability == "desktop" && desktop.offers("oscairn.setlist"), "the desktop's own profile, from the computer");
        bool missing = false; try { loadProfile("no-such-device"); } catch(const std::invalid_argument&) { missing = true; }
        check(missing, "an unknown profile is refused");

        // Refused: each mistake on its own.
        const auto json = all.front().second;
        check(refused(replaced(json, "\"oscairn.profile\"", "\"oscairn.layout\"")), "another format");
        check(refused(replaced(json, "\"version\": 1", "\"version\": 2")), "another version");
        check(refused(replaced(json, "\"reliability\"", "\"shader\": 1, \"reliability\"")), "an unknown field");
        check(refused(replaced(json, "\"instrument\"", "\"unbreakable\"")), "an unknown reliability");
        check(refused(replaced(json, "\"surface\": \"panel\"", "\"surface\": \"hologram\"")), "an unknown surface");
        check(refused(replaced(json, "\"block\": 128", "\"block\": 4")), "a block too small");
        check(refused(replaced(json, "\"cores\": 4, \"realtimeCores\": 1", "\"cores\": 1, \"realtimeCores\": 4")), "more realtime cores than cores");
        check(refused(replaced(json, "\"width\": 1024", "\"width\": 0")), "a screen with no size");
        check(refused("[1, 2]"), "not an object");

        // A profile's block reaches the engine as an audio option, and back.
        oscairn::host::AudioConfiguration audio;
        audio.setOption("--buffer-frames", "128");
        check(audio.bufferFrames == 128 && audio.arguments() == std::vector<std::string>{"--buffer-frames", "128"}, "the block is passed to the engine");
        bool bad = false; try { audio.setOption("--buffer-frames", "8"); } catch(const std::invalid_argument&) { bad = true; }
        check(bad, "a block under 16 frames is refused");
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    if(!failures) std::puts("ok: device profiles: built in, the desktop's, refusals and the block");
    return failures ? 1 : 0;
}

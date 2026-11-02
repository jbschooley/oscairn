// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Device profiles (RFC 0024): the built-in ones parse and say what the app uses (layout,
// surface, block, apps); the desktop's is made from the computer; bad ones are refused; a
// profile's block reaches the engine as an audio option; and controllers' profiles parse, match
// their ports by name and index, and refuse what they must.
#include "../Profile.h"
#include "../../audio/AudioConfiguration.h"
#include <juce_gui_basics/juce_gui_basics.h>
#include <algorithm>
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
        auto all = builtInProfiles();
        check(all.size() == 6, "six built-in profiles: " + std::to_string(all.size()));
        std::erase_if(all, [](const auto& p) { return Profile::parse(p.second).controller(); }); // the devices'
        check(all.size() == 4, "four of them devices'");
        for(const auto& [id, json] : all)
        {
            const auto p = Profile::parse(json);
            check(p.id == id && p.screen() && p.screen()->surface == "panel" && p.audio.block == 128 && p.reliability == "instrument", id + " parses as a panel instrument");
            check(p.offers("oscairn.library") && !p.offers("oscairn.nothing"), id + " offers its apps");
        }
        const auto kick = loadProfile("kick"), ignite = loadProfile("ignite"), qemu = loadProfile("qemu-aarch64"), modx = loadProfile("modx-m");
        check(kick.name == "Behringer KICK" && kick.screen()->layout == "kick-screen" && kick.screen()->touch, "the KICK shows its screen layout, by touch");
        check(ignite.screen()->layout == "ignite" && ignite.cv.outputs > 0, "the Ignite shows its panel and has CV");
        check(!kick.estimated.empty() && qemu.estimated.empty(), "the KICK's values are estimates; QEMU's are its test setup");
        // Their controls in groups (RFC 0024): the KICK's four assignable knobs paged on screen beside
        // fixed ones; the MODX M's screen knobs, knobs in rows and faders in modes.
        check(kick.controls.front().kind == "encoders" && kick.controls.front().count == 4 && kick.controls.front().role == "assign" && kick.controls.front().paging == "screen" &&
              kick.controls[1].functions.size() == 3 && kick.controls[2].functions.front() == "tempo", "the KICK: P1 to P4 paged on screen; VOLUME, MASTER, REC GAIN and the tempo wheel fixed");
        check(modx.controls[1].role == "screen" && modx.controls[1].count == 6 && modx.controls[2].rows.size() == 6 && modx.controls[2].rows[2].bank == "filter" &&
              modx.controls[3].kind == "faders" && modx.controls[3].modes.size() == 3 && modx.screen()->layout == "modx-m-screen", "the MODX M: six screen knobs, four knobs in six rows, eight faders in three modes");
        check(std::none_of(ignite.controls.begin(), ignite.controls.end(), [](const Profile::Control& c) { return c.role == "assign"; }), "the Ignite's knobs are all dedicated");
        check(kick.cpuBudget() == .75 && kick.memoryBudget() == 2048ull * 1024 * 1024 * 8 / 10, "an instrument's budget: 75% of the audio thread, 80% of its memory");
        const auto desktop = loadProfile("");
        check(desktop.id == "desktop" && desktop.compute.cores >= 1 && desktop.screen() && desktop.screen()->surface == "desktop" && desktop.screen()->layout == "desktop" &&
              desktop.reliability == "desktop" && desktop.offers("oscairn.setlist"), "the desktop's own profile, from the computer");
        bool missing = false; try { loadProfile("no-such-device"); } catch(const std::invalid_argument&) { missing = true; }
        check(missing, "an unknown profile is refused");

        // Refused: each mistake on its own.
        const auto json = all.front().second;
        check(refused(replaced(json, "\"oscairn.profile\"", "\"oscairn.layout\"")), "another format");
        check(refused(replaced(json, "\"version\": 2", "\"version\": 1")), "another version");
        check(refused(replaced(json, "\"reliability\"", "\"shader\": 1, \"reliability\"")), "an unknown field");
        check(refused(replaced(json, "\"instrument\"", "\"unbreakable\"")), "an unknown reliability");
        check(refused(replaced(json, "\"surface\": \"panel\"", "\"surface\": \"hologram\"")), "an unknown surface");
        check(refused(replaced(json, "\"block\": 128", "\"block\": 4")), "a block too small");
        check(refused(replaced(json, "\"cores\": 4, \"realtimeCores\": 1", "\"cores\": 1, \"realtimeCores\": 4")), "more realtime cores than cores");
        check(refused(replaced(json, "\"width\": 1024", "\"width\": 0")), "a screen with no size");
        check(refused("[1, 2]"), "not an object");
        // Control groups: a role each; rows or paging for an assign group of knobs; modes for faders only.
        std::string kickJson; for(const auto& [id, text] : all) if(id == "kick") kickJson = text;
        const std::string paged = "\"role\": \"assign\", \"paging\": \"screen\"";
        check(refused(replaced(kickJson, paged, "\"role\": \"assign\"")), "an assign group of knobs with neither rows nor paging");
        check(refused(replaced(kickJson, paged, paged + ", \"rows\": [{\"name\": \"A\", \"bank\": \"assign\"}]")), "with both");
        check(refused(replaced(kickJson, paged, "\"role\": \"assign\", \"rows\": [{\"name\": \"A\", \"bank\": \"tone\"}]")), "a row naming an unknown bank");
        check(refused(replaced(kickJson, paged, "\"role\": \"steer\"")), "an unknown role");
        // Memory classes (RFC 0025): osMemory and performanceMemory together; always-available apps
        // among its apps, by default the Performance app and the Library; all fitting its memory.
        {
            const std::string compute = "\"memory\": 2048, \"storage\": 16384";
            check(!kick.memoryClasses() && kick.alwaysAvailable == std::vector<std::string>{"oscairn.performance", "oscairn.library"},
                  "without them, no memory classes; the Performance app and the Library always available");
            const auto classes = Profile::parse(replaced(kickJson, compute, "\"memory\": 2048, \"storage\": 16384, \"osMemory\": 300, \"performanceMemory\": 1200"));
            check(classes.memoryClasses() && classes.compute.osMemory == 300 && classes.compute.performanceMemory == 1200 &&
                  classes.memoryMargin() == 2048ull * 1024 * 1024 / 20, "osMemory and the floor read, the margin 5% of memory");
            check(refused(replaced(kickJson, compute, compute + ", \"osMemory\": 300")), "osMemory without performanceMemory");
            check(refused(replaced(kickJson, compute, compute + ", \"osMemory\": 0, \"performanceMemory\": 100")), "an osMemory of 0");
            check(refused(replaced(kickJson, compute, compute + ", \"osMemory\": 600, \"performanceMemory\": 1200")),
                  "the OS, the floor and the always-available apps (600 + 1200 + 150 + 120) past its 2048 MiB");
            const auto fewer = replaced(replaced(kickJson, compute, compute + ", \"osMemory\": 600, \"performanceMemory\": 1200"), "\"apps\":", "\"alwaysAvailable\": [\"oscairn.performance\"], \"apps\":");
            check(!refused(fewer) && Profile::parse(fewer).alwaysAvailable.size() == 1, "fitting with only the Performance app always available (600 + 1200 + 150)");
            check(refused(replaced(kickJson, "\"apps\":", "\"alwaysAvailable\": [\"oscairn.recorder\"], \"apps\":")), "an always-available app it does not have");
        }
        check(refused(replaced(kickJson, "\"role\": \"notes\"", "\"role\": \"assign\"")), "pads that do not play notes");
        check(refused(replaced(kickJson, paged, paged + ", \"modes\": [\"part\"]")), "modes on knobs");
        check(refused(replaced(kickJson, "[\"volume\", \"master\", \"recGain\"]", "[\"volume\"]")), "fewer functions than controls");

        // Default targets: buttons playing scenes, wheels passing standard controllers through.
        check(modx.controls[8].role == "assign" && modx.controls[8].functions == std::vector<std::string>{"pitchBend"} && modx.controls[9].functions.front() == "modulation",
              "the MODX M's wheels pass pitch bend and modulation through, and can be reassigned");
        check(refused(replaced(kickJson, paged, paged + ", \"target\": \"scenes\"")), "knobs cannot play scenes");
        check(refused(replaced(kickJson, "\"role\": \"fixed\", \"functions\": [\"tempo\"]", "\"role\": \"assign\", \"functions\": [\"tempo\"]")), "an assign group's function is a standard controller");
        check(refused(replaced(kickJson, paged, paged + ", \"midi\": {\"channel\": 1, \"cc\": [1, 2, 3, 4]}")), "a device's panel does not talk MIDI");

        // Controllers: matched by port name, their controls with their MIDI.
        const auto controllers = controllerProfiles();
        check(controllers.size() == 2 && controllers[0].id == "modx-m-usb" && controllers[1].id == "smk37-elite", "two built-in controllers, by id");
        const auto& smk = controllers[1];
        check(smk.controller() && smk.reliability.empty() && !smk.screen() && smk.midi.ports.size() == 3 && smk.midi.ports[0].name == "SMK-37 Elite -Master" && smk.midi.ports[1].ignore &&
              smk.midi.ports[2].protocol == "mackie" && smk.estimated.empty(), "the SMK37 Elite: a port by name, its duplicate ignored, read from a unit");
        const auto* knobs = &smk.controls[4];
        check(knobs->kind == "knobs" && knobs->count == 8 && knobs->target == "banks" && knobs->midi.channels == std::vector<int>{6, 6, 6, 6, 6, 6, 1, 1} &&
              knobs->midi.cc.size() == 8 && knobs->midi.cc[6] == 38 && knobs->midi.encodings[0] == "absolute" && knobs->midi.encodings[6] == "steps" &&
              knobs->midi.steps[6] == std::array<int, 2>{69, 1} && knobs->midi.steps[0] == std::array<int, 2>{0, 0}, "its eight knobs: 1 to 6 absolute on channel 6, 7 and 8 stepping on channel 1");
        check(smk.controls[2].midi.pitchBend && smk.controls[3].midi.cc == std::vector<int>{1} && smk.controls[5].target == "partLevels" && smk.controls[1].midi.notes.size() == 16 && smk.controls[1].midi.channel == 2,
              "its wheels, faders and pads");
        const auto& usb = controllers[0];
        check(usb.midi.ports.size() == 2 && usb.midi.ports[1].protocol == "mackie" && usb.controls[5].rows.size() == 2 && usb.controls[5].midi.cc.size() == 8 &&
              usb.controls[7].target == "scenes", "the MODX M over USB: a Mackie port declared, knobs sending a number per row, scene buttons");
        bool controllerRefused = false; try { loadProfile("smk37-elite"); } catch(const std::invalid_argument&) { controllerRefused = true; }
        check(controllerRefused, "a controller's profile is not a device's");
        // The SMK-37 Elite's inputs as macOS lists them (read 2026-11-01 with oscairn-midi-monitor --list).
        auto matches = matchPorts(controllers, {"IAC Bus 1", "MODX M Port1", "SMK-37 Elite -Private", "smk-37 elite -master", "MODX M Port2", "SMK-37 Elite -MCP"});
        check(matches.size() == 2 && matches[0].input == 1 && matches[0].profile == 0 && matches[0].port == 1 && matches[1].input == 3 && matches[1].profile == 1 && matches[1].port == 1,
              "ports matched by name, ignoring case, the Nth by index; the Mackie and ignored ports left out");
        check(ignoredInputs(controllers, {"IAC Bus 1", "SMK-37 Elite -Private", "SMK-37 Elite -Master"}) == std::vector<std::string>{"SMK-37 Elite -Private"} &&
              ignoredInputs(controllers, {"SMK-37 Elite -Master"}).empty(), "the duplicate port, by its input's name, when it is there");
        check(matchPorts(controllers, {"Keystation 49"}).empty(), "an unknown controller matches nothing");
        // Two profiles for one unit (stock and custom firmware): the first by id until the musician
        // chooses, per port name (RFC 0024); a choice that no longer matches falls back.
        {
            std::string stock; for(const auto& [id, text] : builtInProfiles()) if(id == "smk37-elite") stock = text;
            auto custom = Profile::parse(replaced(replaced(stock, "\"id\": \"smk37-elite\"", "\"id\": \"smk37-elite-custom\""), "\"ignore\": true", "\"ignore\": false"));
            const std::vector<Profile> both{smk, custom};
            const std::vector<std::string> inputs{"SMK-37 Elite -Private", "SMK-37 Elite -Master"};
            check(profilesFor(both, inputs, 1) == std::vector<size_t>{0, 1} && profileOf(both, inputs, 1) == 0 && matchPorts(both, inputs).size() == 1 && matchPorts(both, inputs)[0].profile == 0,
                  "two profiles match the port: the first by id takes it");
            ProfileChoices chosen{{"SMK-37 Elite -Master", "smk37-elite-custom"}, {"SMK-37 Elite -Private", "smk37-elite-custom"}};
            const auto taken = matchPorts(both, inputs, "midi", chosen);
            check(profileOf(both, inputs, 1, chosen) == 1 && taken.size() == 2 && taken[0].profile == 1 && taken[1].profile == 1, "the chosen profile takes the ports chosen for it");
            check(ignoredInputs(both, inputs) == std::vector<std::string>{"SMK-37 Elite -Private"} && ignoredInputs(both, inputs, chosen).empty(), "and its ports are not ignored where the stock profile's are");
            check(profileOf(both, inputs, 1, {{"SMK-37 Elite -Master", "nonesuch"}}) == 0, "a choice that matches no longer falls back to the first");
            const auto file = juce::File::createTempFile(".json");
            check(readProfileChoices(file.getFullPathName().toStdString()).empty(), "no choices saved: none");
            writeProfileChoices(file.getFullPathName().toStdString(), chosen);
            check(readProfileChoices(file.getFullPathName().toStdString()) == chosen, "the choices saved read back");
            file.deleteFile();
        }
        matches = matchPorts(controllers, {"IAC Bus 1", "MODX M Port1", "SMK-37 Elite -Private", "smk-37 elite -master", "MODX M Port2", "SMK-37 Elite -MCP"}, "mackie");
        check(matches.size() == 2 && matches[0].input == 4 && matches[0].profile == 0 && matches[0].port == 2 && matches[1].input == 5 && matches[1].profile == 1 && matches[1].port == 3,
              "the Mackie ports, asked for: the MODX M's second, the SMK37's -MCP");
        check(std::none_of(controllers.begin(), controllers.end(), [](const Profile& c) {
                  return std::any_of(c.controls.begin(), c.controls.end(), [](const Profile::Control& g) { return g.midi.feedback; }) ||
                         std::any_of(c.midi.ports.begin(), c.midi.ports.end(), [](const Profile::Port& p) { return p.display; }); }),
              "no built-in controller takes values back or has a display: unconfirmed, so off");

        // Refused: each mistake in a controller's profile on its own.
        std::string smkJson; for(const auto& [id, text] : builtInProfiles()) if(id == "smk37-elite") smkJson = text;
        check(!refused(smkJson), "the SMK37's profile parses as text");
        check(refused(replaced(smkJson, "\"midi\": { \"ports\"", "\"reliability\": \"desktop\", \"midi\": { \"ports\"")), "a controller has no reliability");
        check(refused(replaced(smkJson, "\"midi\": { \"ports\"", "\"alwaysAvailable\": [], \"midi\": { \"ports\"")), "nor always-available apps");
        check(refused(replaced(smkJson, "\"protocol\": \"mackie\"", "\"protocol\": \"osc\"")), "an unknown protocol");
        check(refused(replaced(smkJson, "[68, 69, 70, 71]", "[68, 69, 70]")), "fewer CCs than controls");
        check(refused(replaced(smkJson, "[68, 69, 70, 71]", "[68, 69, 70, 70]")), "two controls sending the same message");
        check(refused(replaced(smkJson, "\"channel\": 1, \"cc\": [68", "\"channel\": 17, \"cc\": [68")), "a channel past 16");
        check(refused(replaced(smkJson, "\"channel\": 1, \"cc\": [68", "\"port\": 4, \"channel\": 1, \"cc\": [68")), "a port it does not have");
        check(refused(replaced(smkJson, "\"channel\": 1, \"cc\": [68", "\"port\": 2, \"channel\": 1, \"cc\": [68")), "a control on an ignored port");
        check(refused(replaced(smkJson, "\"ignore\": true", "\"ignore\": true, \"protocol\": \"midi\"")), "an ignored port with a protocol");
        check(refused(replaced(smkJson, "[6, 6, 6, 6, 6, 6, 1, 1]", "[6, 6, 6, 6, 6, 6, 1]")), "a channel list of the wrong length");
        check(refused(replaced(smkJson, "[6, 6, 6, 6, 6, 6, 1, 1]", "[6, 6, 6, 6, 6, 6, 1, 0]")), "a channel 0 in the list");
        check(refused(replaced(smkJson, "[48, 49, 50, 51, 52, 53, 38, 37]", "[48, 49, 50, 51, 52, 53, 38, 38]")), "two knobs on one channel sending the same CC");
        check(!refused(replaced(smkJson, "[48, 49, 50, 51, 52, 53, 38, 37]", "[48, 49, 50, 51, 52, 53, 48, 37]")), "one CC on two channels is two messages");
        check(refused(replaced(smkJson, "{ \"increment\": 69, \"decrement\": 1 }", "{ \"increment\": 1, \"decrement\": 1 }")), "an increment equal to its decrement");
        check(refused(replaced(smkJson, "{ \"increment\": 69, \"decrement\": 1 }", "{ \"increment\": 69 }")), "an increment without a decrement");
        check(refused(replaced(smkJson, "{ \"increment\": 69, \"decrement\": 1 }", "{ \"increment\": 69, \"decrement\": 128 }")), "a step value past 127");
        check(refused(replaced(smkJson, "\"absolute\", { \"increment\"", "\"gray\", { \"increment\"")), "an unknown encoding");
        check(refused(replaced(smkJson, "\"channel\": 1, \"cc\": [1]", "\"channel\": 1, \"cc\": [1], \"notes\": [1]")), "both a CC and a note");
        check(refused(replaced(smkJson, "\"channel\": 1, \"cc\": [68, 69, 70, 71]", "\"channel\": 1, \"cc\": [68, 69, 70, 71], \"button\": \"latch\"")), "faders that latch");
        // Values back and a display: on what can take them.
        std::string usbJson; for(const auto& [id, text] : builtInProfiles()) if(id == "modx-m-usb") usbJson = text;
        const auto encoders = "\"cc\": [17, 18, 19, 20, 21, 22, 23, 24] }";
        const auto back = Profile::parse(replaced(usbJson, encoders, "\"cc\": [17, 18, 19, 20, 21, 22, 23, 24], \"feedback\": true }"));
        check(back.controls[5].midi.feedback && !back.controls[6].midi.feedback, "the MODX M's knobs told to take their values back");
        // Aftertouch as channel pressure, and the ASSIGN switches (RFC 0024's control identities).
        check(usb.controls[3].kind == "aftertouch" && usb.controls[3].midi.pressure && usb.controls[3].functions == std::vector<std::string>{"aftertouch"},
              "the MODX M's aftertouch is channel pressure");
        check(usb.controls[9].target == "switches" && usb.controls[9].count == 2 && usb.controls[7].target == "scenes", "its ASSIGN 1 and 2 are switches, beside the scene buttons");
        check(refused(replaced(usbJson, "\"pressure\": true", "\"pitchBend\": true")), "pitch bend from an aftertouch group");
        check(refused(replaced(smkJson, "\"pitchBend\": true }", "\"pressure\": true }")), "pressure from a wheel");
        check(refused(replaced(usbJson, "\"target\": \"switches\"", "\"target\": \"banks\"")), "a buttons group's target is never banks");
        check(refused(replaced(smkJson, "{ \"increment\": 69, \"decrement\": 1 }] }", "{ \"increment\": 69, \"decrement\": 1 }], \"feedback\": true }")), "feedback on knobs that step");
        check(refused(replaced(smkJson, "\"pitchBend\": true }", "\"pitchBend\": true, \"feedback\": true }")), "feedback on pitch bend");
        check(Profile::parse(replaced(smkJson, "\"protocol\": \"mackie\"", "\"protocol\": \"mackie\", \"display\": true")).midi.ports[2].display, "a Mackie port with a display");
        check(refused(replaced(smkJson, "{ \"name\": \"SMK-37 Elite -Master\" }", "{ \"name\": \"SMK-37 Elite -Master\", \"display\": true }")), "a display on a midi port");

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

// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CLAP presets: preset discovery lists a plugin's own presets and its preset files, only those
// for this plugin; loading one sets the plugin's sound; a preset prepares an instant-switch sound.
//   oscairn-clap-presets-test FIXTURE.clap
#include "oscairn/graph.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    const auto folder = std::filesystem::temp_directory_path() / ("oscairn-presets-" + std::to_string(getpid()));
    std::filesystem::create_directories(folder / "more");
    std::ofstream(folder / "Warm.kickpreset") << "0.4";
    std::ofstream(folder / "more" / "Soft.kickpreset") << "0.1";
    std::ofstream(folder / "Ignored.txt") << "0.9";
    setenv("KICK_FIXTURE_PRESETS", folder.c_str(), 1);
    try
    {
        auto host = loadPlugin(argv[1], "org.kick.test.midi", 48000, 128);
        const auto presets = host->presets();
        std::vector<std::string> names;
        for(const auto& p : presets) names.push_back(p.name);
        std::sort(names.begin(), names.end());
        check(names == std::vector<std::string>{"Bright", "Quiet", "Soft", "Warm"}, "the plugin's presets and preset files, only this plugin's");
        auto uri = [&](const std::string& name) { for(const auto& p : presets) if(p.name == name) return p.uri; throw std::runtime_error("missing " + name); };
        host->loadPreset(uri("Bright")); check(host->parameterValue(7) == .75, "a preset inside the plugin loads");
        host->loadPreset(uri("Soft")); check(std::abs(host->parameterValue(7) - .1) < 1e-12, "a preset file in a subfolder loads");
        bool refused = false; try { host->loadPreset("not a preset"); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a malformed preset URI is refused");
        refused = false; try { host->loadPreset("clap-preset:1:#missing"); } catch(const std::runtime_error& e) { refused = std::string(e.what()).find("no such preset") != std::string::npos; }
        check(refused, "the plugin's own error comes back: preset not loaded");

        // A preset prepares an instant-switch sound for an ordinary instrument.
        Graph graph(48000, 128);
        const auto node = graph.add(argv[1], "org.kick.test.midi");
        graph.connectNotes(graphInput, node); graph.connectAudio(node, graphOutput); graph.commit();
        const auto sound = graph.host(node).prepareSound("Warm", {}, {}, uri("Warm"));
        check(graph.host(node).selectSound(sound), "the preset's sound selected");
        std::array<float, 128> l{}, r{}; float* out[2]{l.data(), r.data()};
        graph.process(out, 128, nullptr, nullptr, 0);
        const auto note = kick::clap::Event::midi(0x90, 60, 100);
        graph.process(out, 128, nullptr, &note, 1);
        check(std::abs(l[127] - .4f) < 1e-6f, "it plays the preset: " + std::to_string(l[127]));
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    std::filesystem::remove_all(folder);
    if(!failures) std::puts("ok: CLAP preset discovery and loading");
    return failures ? 1 : 0;
}

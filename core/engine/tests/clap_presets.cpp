// SPDX-License-Identifier: AGPL-3.0-or-later
//
// CLAP presets: preset discovery lists a plugin's own presets and its preset files, only those
// for this plugin; loading one sets the plugin's sound; a preset prepares an instant-switch sound.
// The SDK's preset discovery helper, and the index's limits (RFC 0005, Presets).
//   oscairn-clap-presets-test FIXTURE.clap
#include "oscairn/graph.h"
#include <kick_clap/Discovery.h>
#include <oscairn-clap/preset-discovery-plugin.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

using namespace oscairn;

namespace
{
    // A plugin's presets through the SDK helper: a folder of cartridges (three presets each, by
    // load key) and single-preset files, or, in the other modes, presets inside the plugin.
    enum class Mode { Files, Many, Bad } mode = Mode::Files;
    std::string folder;
    struct TestPresets
    {
        static constexpr clap_preset_discovery_provider_descriptor_t descriptor{CLAP_VERSION_INIT, "org.oscairn.test.presets", "Test presets", "Oscairn"};
        static constexpr const char* pluginId = "org.oscairn.test.cartridges";
        static void declare(oscairn::presets::Declarations& d)
        {
            if(mode != Mode::Files) { d.inside("Factory"); return; }
            d.files("Cartridges", (folder + "/Carts").c_str(), "cart");
            d.files("Patches", (folder + "/Patches").c_str(), "one");
        }
        static void list(uint32_t kind, const char* location, oscairn::presets::Listing& l)
        {
            if(mode == Mode::Many) { for(int i = 0; i < 5000; ++i) if(!l.add(("P" + std::to_string(i)).c_str(), std::to_string(i).c_str())) return; return; }
            if(mode == Mode::Bad)
            {
                l.add(std::string(256, 'n').c_str(), "long"); // a name over 255 bytes
                l.add("No key");                              // inside the plugin, without a load key
                l.add("Kept", "kept");
                return;
            }
            if(kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) return;
            const std::filesystem::path file(location);
            if(file.extension() == ".one") { l.add(nullptr); return; }
            const auto stem = file.stem().string();
            for(int i = 0; i < 3; ++i) l.add((stem + " " + std::to_string(i + 1)).c_str(), std::to_string(i).c_str());
        }
    };
    const clap_plugin_entry_t entry{CLAP_VERSION_INIT, [](const char*) { return true; }, [] {},
        [](const char* id) -> const void* { return std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) ? nullptr : &oscairn::presets::Factory<TestPresets>::factory; }};

    int indexLimits()
    {
        int failures = 0;
        auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
        const auto root = std::filesystem::temp_directory_path() / ("oscairn-preset-index-" + std::to_string(getpid()));
        std::filesystem::create_directories(root / "Carts" / "Bank"); std::filesystem::create_directories(root / "Patches");
        std::ofstream(root / "Carts" / "Bank" / "Brass.cart") << "x";
        std::ofstream(root / "Patches" / "Lead.one") << "x";
        std::ofstream(root / "Patches" / "Notes.txt") << "x";
        folder = root.string();
        const std::string id = TestPresets::pluginId;
        auto index = kick::clap::indexPresets(&entry, {id, "org.oscairn.test.other"});
        std::vector<std::string> names; for(const auto& p : index.presets[id]) names.push_back(p.name + "@" + p.loadKey);
        std::sort(names.begin(), names.end());
        check(names == std::vector<std::string>{"Brass 1@0", "Brass 2@1", "Brass 3@2", "Lead@"}, "cartridges by load key, a single-preset file named after itself, other files ignored");
        check(index.presets["org.oscairn.test.other"].empty() && index.problems.empty(), "only for the plugin they name");
        check(!index.presets[id].empty() && index.presets[id][0].kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE &&
              std::filesystem::path(index.presets[id][0].location).parent_path().filename() == "Bank", "a file preset's location is its file, in a subfolder");
        mode = Mode::Many;
        index = kick::clap::indexPresets(&entry, {id});
        check(index.presets[id].size() == kick::clap::maxPresets && index.problems == std::vector<std::string>{"Presets: stopped at 4096"}, "at most 4096 presets, noted");
        mode = Mode::Bad;
        index = kick::clap::indexPresets(&entry, {id});
        check(index.presets[id].size() == 1 && index.presets[id][0].name == "Kept" && index.problems.empty(), "a name too long, or a preset inside the plugin without a key, is skipped");
        index = kick::clap::indexPresets(&entry, {id}, 0);
        check(index.presets[id].empty() && index.problems == std::vector<std::string>{"Presets: stopped after 0 s"}, "the time budget stops the index, noted");
        std::filesystem::remove_all(root);
        return failures;
    }
}

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    int failures = indexLimits();
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

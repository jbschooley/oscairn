// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A third-party plugin through the whole path a musician's sound takes (RFC 0006): played in a
// part, saved to the library and opened again, exported with its build, installed on a fresh
// device and opened from the plugin store (CLAP: archives do not carry LV2 yet, so an LV2 plugin
// stops after the export). Opt-in, for plugins not in this repository:
//   oscairn-plugin-check PLUGIN ID synth|effect [--param ID=VALUE] [--juce-state NAME=VALUE]
// --juce-state sets an attribute in a JUCE plugin's saved state (its binary XML), as choosing a
// program in its window would; Airwindows Consolidated picks its effect this way
// (currentProcessorName=Chorus). Prints one line per check and the renders' levels.
#include "oscairn/session.h"
#include "oscairn/package.h"
#include "../../plugin-host/src/StateFiles.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { std::printf("%s: %s\n", ok ? "ok" : "FAIL", what.c_str()); if(!ok) ++failures; }
    std::string read(const fs::path& f) { std::ifstream in(f, std::ios::binary); std::ostringstream out; out << in.rdbuf(); return out.str(); }
    void write(const fs::path& f, const std::string& text) { std::ofstream(f, std::ios::binary | std::ios::trunc) << text; }
    void device(const fs::path& home) { fs::create_directories(home); ::setenv("OSCAIRN_DATA_HOME", home.c_str(), 1); }
    constexpr uint32_t block = 256, rate = 48000, seconds = 2;
    const AudioLayout stereoIn{{{0, "In", 2, 0}}, {{0, "Main", 2, 0}}};

    // Two seconds: notes at .1 to .6 s and .8 to 1.1 s for a synth; for an effect a two-tone
    // signal that drops at .6 s and stops at 1.2 s. Interleaved stereo.
    std::vector<float> render(Session& s, bool synth, bool dry = false)
    {
        std::vector<float> inL(block), inR(block), l(block), r(block), out;
        const float* in[2]{inL.data(), inR.data()};
        float* o[2]{l.data(), r.data()};
        for(uint32_t at = 0; at < rate * seconds; at += block)
        {
            std::vector<kick::clap::Event> events;
            const double times[]{.1, .6, .8, 1.1};
            for(int i = 0; i < 4 && synth; ++i)
                if(const auto t = uint32_t(times[i] * rate); t >= at && t < at + block) events.push_back(kick::clap::Event::note(i % 2 == 0, i < 2 ? 60 : 67, .8, t - at));
            for(uint32_t i = 0; i < block; ++i)
            {
                const double t = double(at + i) / rate;
                const float v = t < 1.2 ? float((.2 * std::sin(6.283185307179586 * 220 * t) + .1 * std::sin(6.283185307179586 * 1700 * t)) * (t < .6 ? 1 : .25)) : 0.f;
                inL[i] = inR[i] = v;
            }
            s.service();
            if(!s.process(synth ? nullptr : in, synth ? 0 : 2, o, 2, block, events)) { check(false, "render"); return out; }
            for(uint32_t i = 0; i < block; ++i) { out.push_back(dry ? inL[i] : l[i]); out.push_back(dry ? inR[i] : r[i]); }
        }
        return out;
    }
    double rms(const std::vector<float>& a) { double p = 0; for(const auto x : a) p += double(x) * x; return a.empty() ? 0 : std::sqrt(p / a.size()); }
    double difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        double p = 0; const auto n = std::min(a.size(), b.size());
        for(size_t i = 0; i < n; ++i) p += std::pow(double(a[i]) - b[i], 2);
        return n ? std::sqrt(p / n) : 1;
    }
    bool finite(const std::vector<float>& a) { for(const auto x : a) if(!std::isfinite(x)) return false; return true; }

    // JUCE's binary XML: "VC2!", the text's size with its terminator (32-bit little-endian), the text.
    bool setJuceAttribute(std::string& state, const std::string& name, const std::string& value)
    {
        if(state.size() < 8 || state.compare(0, 4, "VC2!") != 0) return false;
        std::string xml = state.substr(8);
        if(const auto end = xml.find('\0'); end != std::string::npos) xml.resize(end);
        const auto at = xml.find(" " + name + "=\"");
        if(at == std::string::npos) return false;
        const auto from = at + name.size() + 3, to = xml.find('"', from);
        xml.replace(from, to - from, value);
        const uint32_t size = uint32_t(xml.size() + 1);
        state = "VC2!" + std::string(reinterpret_cast<const char*>(&size), 4) + xml + '\0';
        return true;
    }
    // Opened without a stand-in: the plugin itself, not a silent placeholder.
    void opened(Session& s, const std::string& what)
    {
        std::string why;
        for(const auto& f : s.sessionStatus().failures) why += " " + f.name + ": " + f.reason;
        check(why.empty(), what + why);
    }
    std::vector<double> values(Session& s, NodeId node)
    {
        std::vector<double> out;
        for(const auto& p : s.parameters(node)) out.push_back(s.parameter(node, p.id));
        return out;
    }
}

int main(int argc, char** argv)
{
    if(argc < 4) { std::fprintf(stderr, "usage: %s PLUGIN ID synth|effect [--param ID=VALUE] [--juce-state NAME=VALUE]\n", argv[0]); return 2; }
    const std::string plugin = fs::absolute(argv[1]).string(), id = argv[2];
    const bool synth = std::string(argv[3]) == "synth", lv2 = fs::path(plugin).extension() == ".lv2";
    std::vector<std::pair<uint32_t, double>> params;
    std::vector<std::pair<std::string, std::string>> juce;
    for(int i = 4; i + 1 < argc; i += 2)
    {
        const std::string option = argv[i], value = argv[i + 1];
        const auto eq = value.find('=');
        if(eq == std::string::npos) { std::fprintf(stderr, "%s needs NAME=VALUE\n", option.c_str()); return 2; }
        if(option == "--param") params.push_back({uint32_t(std::stoul(value.substr(0, eq))), std::stod(value.substr(eq + 1))});
        else if(option == "--juce-state") juce.push_back({value.substr(0, eq), value.substr(eq + 1)});
        else { std::fprintf(stderr, "unknown option %s\n", option.c_str()); return 2; }
    }
    try
    {
        state_files::TemporaryDirectory temp;
        const auto archive = temp.path / "Check.olib";
        std::vector<float> saved;
        std::vector<double> savedValues;
        std::string performance;
        // Device A: played in a part, saved, opened again, exported.
        {
            device(temp.path / "a");
            Session a(rate, block, {}, stereoIn);
            const auto node = a.add("plugin", plugin, id);
            const auto part = a.addPart("Part", synth ? PartInput{} : PartInput{PartInput::Kind::Audio, 0});
            a.insertIntoPart(part, 0, node); a.commit();
            check(a.sessionStatus().failures.empty(), "loads in a part");
            const auto first = render(a, synth);
            check(finite(first) && rms(first) > 1e-6, "plays: rms " + std::to_string(rms(first)));
            if(!synth) check(difference(first, render(a, synth, true)) > 1e-6, "changes the signal");
            for(const auto& [param, value] : params) check(a.setParameter(node, param, value), "parameter " + std::to_string(param) + " set");
            // Edits reach the plugin at service(), which a render does: so they are in what is saved.
            if(!params.empty()) check(difference(first, render(a, synth)) > 1e-6, "the edit changes the sound");
            performance = a.savePerformanceToLibrary("Check");
            if(!juce.empty())
            {
                // The state as a program would leave it, written into the saved Performance and opened.
                const auto item = fs::path(a.libraryQuery({}).items.front().path);
                bool found = false;
                for(const auto& f : fs::recursive_directory_iterator(item / "nodes"))
                {
                    if(!f.is_regular_file()) continue;
                    auto state = read(f.path());
                    if(state.compare(0, 4, "VC2!") != 0) continue;
                    found = true;
                    for(const auto& [name, value] : juce) check(setJuceAttribute(state, name, value), "state " + name + "=" + value);
                    write(f.path(), state);
                }
                check(found, "a JUCE state in the saved Performance");
                a.libraryRescan();
                a.loadPerformanceFromLibrary(performance);
                opened(a, "opens with that state");
                const auto loaded = a.graphState().topology.nodes.at(0).id;
                const auto chosen = render(a, synth);
                check(finite(chosen) && difference(first, chosen) > 1e-6, "the state changes the sound: rms " + std::to_string(rms(chosen)));
                for(const auto& [param, value] : params) a.setParameter(loaded, param, value);
                if(!params.empty()) check(difference(chosen, render(a, synth)) > 1e-6, "the edit changes the sound");
                performance = a.savePerformanceToLibrary("Check");
            }
            a.loadPerformanceFromLibrary(performance);
            opened(a, "opens as saved");
            const auto once = a.graphState().topology.nodes.at(0).id;
            saved = render(a, synth);
            savedValues = values(a, once);
            check(finite(saved) && rms(saved) > 1e-6, "plays as saved: rms " + std::to_string(rms(saved)));
            for(const auto& [param, value] : params) check(std::abs(a.parameter(once, param) - value) < 1e-6, "parameter " + std::to_string(param) + " as edited");
            a.loadPerformanceFromLibrary(performance);
            const auto again = render(a, synth);
            check(values(a, a.graphState().topology.nodes.at(0).id) == savedValues, "opened again, every parameter as saved");
            std::printf("reopened render differs by rms %g (0: identical)\n", difference(saved, again));
            const auto notes = a.exportPerformance(archive.string());
            if(lv2)
            {
                check(notes.size() == 1, "exported, saying the LV2 plugin was left out: " + (notes.empty() ? std::string() : notes[0]));
                std::printf("%s\n", failures ? "FAILED" : "PASSED");
                return failures ? 1 : 0;
            }
            check(notes.empty(), "exported with nothing left out");
            const auto packed = readPackage(archive, {});
            check(packed.plugins.size() == 1 && !packed.plugins[0].builds.empty(), "the archive carries the plugin's build");
        }
        // Device B: installed fresh, the plugin found in the store.
        {
            device(temp.path / "b");
            Session b(rate, block, {}, stereoIn);
            const auto installed = b.installArchive(archive.string(), false);
            check(installed.notes.empty(), "installs with nothing to report");
            Control::LibraryQuery q; q.kind = "performance";
            const auto item = b.libraryQuery(q).items.at(0);
            const auto doc = fs::path(item.path) / "performance.json";
            auto text = read(doc);
            for(auto at = text.find(plugin); at != std::string::npos; at = text.find(plugin)) text.replace(at, plugin.size(), "/nowhere/plugin.clap");
            write(doc, text);
            b.libraryRescan();
            b.loadPerformanceFromLibrary(item.id);
            const auto node = b.graphState().topology.nodes.at(0);
            check(b.sessionStatus().failures.empty() && node.plugin.path.rfind((temp.path / "b" / "Plugins").string(), 0) == 0, "opens from the plugin store: " + node.plugin.path);
            check(values(b, node.id) == savedValues, "every parameter as saved");
            const auto there = render(b, synth);
            check(finite(there) && rms(there) > 1e-6, "plays there: rms " + std::to_string(rms(there)));
            std::printf("render there differs by rms %g (0: identical)\n", difference(saved, there));
            const auto store = b.installedPlugins();
            check(store.size() == 1 && store[0].open && store[0].usedBy.size() == 1, "the store says the open Performance plays it");
        }
    }
    catch(const std::exception& e) { check(false, e.what()); }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

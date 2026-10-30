// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Modular as a CLAP instrument: it plays, presets change the sound, the same input
// renders the same output, state restores the sound, and rendering does not allocate.
//   oscairn-modular-test oscairn-modular.clap
#include "AllocationGuard.h"
#include <oscairn/plugin_host.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    // Two seconds of a chord, a bend and a release, at 48 kHz in 256-frame blocks.
    std::vector<float> render(oscairn::PluginHost& h)
    {
        constexpr uint32_t frames = 256;
        std::vector<float> l(frames), r(frames), all;
        float* out[]{l.data(), r.data()};
        for(uint32_t block = 0; block < 375; ++block)
        {
            std::vector<kick::clap::Event> ev;
            if(block == 1) { ev.push_back(kick::clap::Event::note(true, 48, .9, 3)); ev.push_back(kick::clap::Event::note(true, 55, .7, 3)); ev.push_back(kick::clap::Event::note(true, 64, .6, 100)); }
            if(block == 100) ev.push_back(kick::clap::Event::midi(0xe0, 0, 80, 10));
            if(block == 200) for(int k : {48, 55, 64}) ev.push_back(kick::clap::Event::note(false, k, 0, 50));
            forbidAllocation = true;
            const bool ok = h.process(nullptr, 0, out, 2, frames, ev.data(), uint32_t(ev.size()), nullptr);
            forbidAllocation = false;
            check(ok, "process block " + std::to_string(block));
            all.insert(all.end(), l.begin(), l.end()); all.insert(all.end(), r.begin(), r.end());
        }
        return all;
    }
    double energy(const std::vector<float>& x) { double e = 0; for(float v : x) { if(!std::isfinite(v)) return -1; e += double(v) * v; } return e; }
}

int main(int argc, char** argv)
{
    if(argc != 2) { std::fprintf(stderr, "usage: %s oscairn-modular.clap\n", argv[0]); return 2; }
    try
    {
        auto load = [&] { return oscairn::loadPlugin(argv[1], "org.oscairn.modular", 48000, 256); };
        auto a = load(), b = load();
        check(a->parameters().size() == 10, "eight macros, level and preset");
        const auto first = render(*a), again = render(*b);
        check(energy(first) > 1, "notes make sound (energy " + std::to_string(energy(first)) + ")");
        check(first == again, "the same input renders the same output");

        clap_id preset = 0; for(const auto& p : a->parameters()) if(p.name == "Preset") preset = p.id;
        check(a->parameterText(preset, 1) != a->parameterText(preset, 0), "presets have names");
        check(a->setParameter(preset, 1), "choose preset 1");
        a->process(nullptr, 0, nullptr, 0, 0, nullptr, 0, nullptr);
        { std::vector<float> l(256), r(256); float* out[]{l.data(), r.data()}; a->process(nullptr, 0, out, 2, 256, nullptr, 0, nullptr); }
        a->service();
        a->panic();
        const auto other = render(*a);
        check(energy(other) > 1 && other != first, "preset " + a->parameterText(preset, 1) + " sounds different");

        const auto dir = std::filesystem::temp_directory_path() / ("oscairn-modular-" + std::to_string(getpid()));
        std::filesystem::remove_all(dir);
        a->saveState(dir.string());
        // Two restores sound the same, differ from the default patch and report the saved preset.
        auto c = load(), d = load(), fresh = load();
        c->loadState(dir.string()); d->loadState(dir.string());
        std::filesystem::remove_all(dir);
        const auto restored = render(*c);
        check(restored == render(*d), "restoring the same state twice sounds the same");
        check(restored != render(*fresh), "the restored patch is not the default one");
        check(c->parameterValue(preset) == 1, "the restored state reports its preset");
        std::printf("modular: energy %.1f, preset %s energy %.1f\n", energy(first), a->parameterText(preset, 1).c_str(), energy(other));
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return failures ? 1 : 0;
}

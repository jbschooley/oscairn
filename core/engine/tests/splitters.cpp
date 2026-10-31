// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Split and Join (plugins/splitters): a Split followed by a Join in the same mode gives back the
// input; summed bands are flat; one band keeps its frequencies and rejects the others.
//   oscairn-splitters-test SPLITTERS.clap
#include "oscairn/graph.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace oscairn;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 256;
    constexpr double rate = 48000;

    // Graph input -> Split -> (the given lanes) -> Join -> output.
    struct Chain
    {
        std::unique_ptr<Graph> g;
        NodeId split = 0, join = 0;
        Chain(const char* plugin, int splitMode, int joinMode, std::vector<uint32_t> lanes, int bands = 2, double crossover = 1000)
        {
            g = std::make_unique<Graph>(rate, frames, ProcessingMode::Realtime, Lv2BufferLimits{}, AudioLayout{{{0, "In", 2, 0}}, {{0, "Main", 2, 0}}});
            split = g->add(plugin, "org.oscairn.split"); join = g->add(plugin, "org.oscairn.join");
            g->connectAudio({{graphInput, 0, 0}, {split, 0, 0}, 2, 1});
            for(const auto k : lanes) g->connectAudio({{split, k, 0}, {join, k, 0}, 2, 1});
            g->connectAudio({{join, 0, 0}, {graphOutput, 0, 0}, 2, 1});
            g->host(split).setParameter(1, splitMode); g->host(split).setParameter(2, bands); g->host(split).setParameter(3, crossover);
            g->host(join).setParameter(1, joinMode);
            g->commit();
        }
        // Renders `blocks` of the signal; returns the largest difference from the input after
        // `settle` blocks, and the RMS of input and output over the same span.
        struct Result { double difference = 0, input = 0, output = 0; };
        template<class Signal> Result run(Signal&& signal, int blocks, int settle)
        {
            std::array<float, frames> il{}, ir{}, ol{}, orr{};
            const float* in[2]{il.data(), ir.data()}; float* out[2]{ol.data(), orr.data()};
            Result r; double si = 0, so = 0; uint64_t n = 0, t = 0;
            for(int b = 0; b < blocks; ++b)
            {
                for(uint32_t i = 0; i < frames; ++i, ++t) { const auto [l, rr] = signal(double(t) / rate); il[i] = float(l); ir[i] = float(rr); }
                check(g->process(in, 2, out, 2, frames, nullptr), "render");
                if(b < settle) continue;
                for(uint32_t i = 0; i < frames; ++i)
                {
                    r.difference = std::max({r.difference, std::abs(double(ol[i]) - il[i]), std::abs(double(orr[i]) - ir[i])});
                    si += double(il[i]) * il[i] + double(ir[i]) * ir[i]; so += double(ol[i]) * ol[i] + double(orr[i]) * orr[i]; ++n;
                }
            }
            r.input = std::sqrt(si / double(n)); r.output = std::sqrt(so / double(n));
            return r;
        }
    };
    double db(double ratio) { return 20 * std::log10(ratio); }
}

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    const char* plugin = argv[1];
    auto music = [](double t) { return std::pair{.5 * std::sin(2 * M_PI * 440 * t) + .2 * std::sin(2 * M_PI * 3100 * t), .3 * std::sin(2 * M_PI * 1234 * t) - .25 * std::sin(2 * M_PI * 97 * t)}; };
    try
    {
        for(const auto [mode, name] : {std::pair{0, "left/right"}, std::pair{1, "mid/side"}})
        {
            Chain c(plugin, mode, mode, {0, 1});
            const auto r = c.run(music, 20, 0);
            check(r.difference < 1e-6, std::string("a ") + name + " split and join give back the input: " + std::to_string(r.difference));
        }
        { // Left/right lanes carry one channel each: with the right lane missing, the right is silent.
            Chain c(plugin, 0, 0, {0});
            std::array<float, frames> il{}, ir{}, ol{}, orr{}; il.fill(.5f); ir.fill(.25f);
            const float* in[2]{il.data(), ir.data()}; float* out[2]{ol.data(), orr.data()};
            c.g->process(in, 2, out, 2, frames, nullptr);
            check(ol[10] == .5f && orr[10] == 0, "the left lane is the left channel");
        }
        { // Mid/side: without the side lane, the output is mono.
            Chain c(plugin, 1, 1, {0});
            std::array<float, frames> il{}, ir{}, ol{}, orr{}; il.fill(.5f); ir.fill(.25f);
            const float* in[2]{il.data(), ir.data()}; float* out[2]{ol.data(), orr.data()};
            c.g->process(in, 2, out, 2, frames, nullptr);
            check(std::abs(ol[10] - .375f) < 1e-7 && ol[10] == orr[10], "mid alone is the mono sum");
        }
        // Bands summed back: flat at every frequency, for two to four bands.
        for(const int bands : {2, 3, 4})
            for(const double f : {40.0, 300.0, 1000.0, 2500.0, 9000.0, 16000.0})
            {
                Chain c(plugin, 2, 2, {0, 1, 2, 3}, bands);
                c.g->host(c.split).setParameter(4, 4000); c.g->host(c.split).setParameter(5, 150); // out of order: sorted by the plugin
                const auto r = c.run([f](double t) { const double x = .5 * std::sin(2 * M_PI * f * t); return std::pair{x, x}; }, 60, 30);
                check(std::abs(db(r.output / r.input)) < .05, std::to_string(bands) + " bands summed are flat at " + std::to_string(int(f)) + " Hz: " + std::to_string(db(r.output / r.input)) + " dB");
            }
        { // Crossovers given out of order are sorted: the lowest band is below the lowest one.
            Chain c(plugin, 2, 2, {0}, 4, 1000);
            c.g->host(c.split).setParameter(4, 4000); c.g->host(c.split).setParameter(5, 150);
            const auto r = c.run([](double t) { const double x = .5 * std::sin(2 * M_PI * 2000 * t); return std::pair{x, x}; }, 60, 30);
            check(db(r.output / r.input) < -60, "2 kHz is far above the lowest band (below 150 Hz): " + std::to_string(db(r.output / r.input)) + " dB");
        }
        { // One band of two: its own frequencies pass, the other band's are rejected.
            for(const auto [f, expect] : {std::pair{100.0, 0.0}, std::pair{10000.0, -60.0}})
            {
                Chain c(plugin, 2, 2, {0}, 2, 1000);
                const auto r = c.run([f](double t) { const double x = .5 * std::sin(2 * M_PI * f * t); return std::pair{x, x}; }, 60, 30);
                const auto level = db(r.output / r.input);
                check(expect == 0 ? std::abs(level) < .1 : level < expect, "the low band at " + std::to_string(int(f)) + " Hz: " + std::to_string(level) + " dB");
            }
        }
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: Split and Join give back their input; bands sum flat and separate");
    return failures ? 1 : 0;
}

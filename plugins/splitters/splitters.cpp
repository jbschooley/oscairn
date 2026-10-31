// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Split and Join (RFC 0011 step 3): the nodes at either end of a part's split lanes, for the
// kinds that divide the sound itself. Their ports never change; the mode decides what flows
// through them, so a split can change kind without being rewired.
//
// Split: one stereo input, four stereo outputs.
//   Left/right: output 1 carries the left channel on both sides, output 2 the right.
//   Mid/side: output 1 carries mid, (L + R) / 2, output 2 side, (L - R) / 2, each on both sides.
//   Bands: outputs 1 to Bands carry frequency bands from Linkwitz-Riley crossovers (24 dB per
//   octave); summed, the bands give back the input with only a phase change.
// Join: four stereo inputs, one stereo output.
//   Left/right: input 1, folded to mono, becomes the left channel, input 2 the right.
//   Mid/side: input 1 (folded) is mid, input 2 side; L = M + S, R = M - S.
//   Sum: every input added.
// A Split followed by a Join in the same mode gives back the input.
#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>
#include <string>

namespace
{
    constexpr uint32_t lanes = 4;
    struct Parameter { const char* name; double minimum, maximum, initial; bool stepped; };
    // Split: mode (0 left/right, 1 mid/side, 2 bands), bands, crossovers in Hz.
    constexpr std::array<Parameter, 5> splitParameters{{
        {"Mode", 0, 2, 0, true}, {"Bands", 2, 4, 2, true}, {"Crossover 1", 20, 20000, 200, false}, {"Crossover 2", 20, 20000, 2000, false}, {"Crossover 3", 20, 20000, 8000, false}}};
    // Join: mode (0 left/right, 1 mid/side, 2 sum).
    constexpr std::array<Parameter, 1> joinParameters{{{"Mode", 0, 2, 0, true}}};

    const char* const features[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, CLAP_PLUGIN_FEATURE_STEREO, nullptr};
    const clap_plugin_descriptor_t splitDescriptor{CLAP_VERSION_INIT, "org.oscairn.split", "Oscairn Split", "Oscairn", "https://github.com/jbschooley/oscairn", "", "", "0.1.0",
                                                   "Divides stereo sound into left and right, mid and side, or frequency bands", features};
    const clap_plugin_descriptor_t joinDescriptor{CLAP_VERSION_INIT, "org.oscairn.join", "Oscairn Join", "Oscairn", "https://github.com/jbschooley/oscairn", "", "", "0.1.0",
                                                  "Joins lanes back into stereo: left and right, mid and side, or a sum", features};

    // A second-order section (RBJ cookbook), transposed direct form II.
    struct Biquad
    {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        enum class Kind { Low, High, All };
        void set(Kind kind, double frequency, double rate)
        {
            const double w = 2 * M_PI * std::clamp(frequency, 1.0, rate * .49) / rate, c = std::cos(w), alpha = std::sin(w) / (2 * M_SQRT1_2);
            const double a0 = 1 + alpha;
            switch(kind)
            {
                case Kind::Low: b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; break;
                case Kind::High: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; break;
                case Kind::All: b0 = 1 - alpha; b1 = -2 * c; b2 = 1 + alpha; break;
            }
            b0 /= a0; b1 /= a0; b2 /= a0; a1 = -2 * c / a0; a2 = (1 - alpha) / a0;
        }
        double run(double x) { const double y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; }
    };

    // Up to four bands from three crossovers. Each band below a crossover also passes through that
    // crossover's allpass (the sum of its low and high halves), so the bands stay in phase.
    struct Crossover
    {
        std::array<std::array<Biquad, 2>, 3> low, high; // two Butterworth sections make one Linkwitz-Riley filter
        std::array<std::array<Biquad, 3>, 3> all;        // band b, crossover c > b
        void set(const std::array<double, 3>& f, uint32_t bands, double rate)
        {
            for(uint32_t c = 0; c + 1 < bands; ++c)
            {
                for(auto& s : low[c]) s.set(Biquad::Kind::Low, f[c], rate);
                for(auto& s : high[c]) s.set(Biquad::Kind::High, f[c], rate);
                for(uint32_t b = 0; b < c; ++b) all[b][c].set(Biquad::Kind::All, f[c], rate);
            }
        }
        void run(double x, uint32_t bands, std::array<double, lanes>& out)
        {
            for(uint32_t c = 0; c + 1 < bands; ++c)
            {
                out[c] = low[c][1].run(low[c][0].run(x));
                x = high[c][1].run(high[c][0].run(x));
            }
            out[bands - 1] = x;
            for(uint32_t b = 0; b + 1 < bands; ++b)
                for(uint32_t c = b + 1; c + 1 < bands; ++c) out[b] = all[b][c].run(out[b]);
        }
    };

    struct Node
    {
        clap_plugin_t plugin{};
        bool split = true;
        std::array<std::atomic<double>, 5> values{};
        double rate = 48000;
        std::array<Crossover, 2> crossovers; // per channel
        std::array<double, 3> frequencies{};  // what the crossovers are set to
        uint32_t bands = 0;
        size_t count() const { return split ? splitParameters.size() : joinParameters.size(); }
        const Parameter& parameter(size_t i) const { return split ? splitParameters[i] : joinParameters[i]; }
        void clear() { crossovers = {}; bands = 0; }
    };
    Node& self(const clap_plugin_t* p) { return *static_cast<Node*>(p->plugin_data); }

    void apply(Node& s, const clap_event_header_t* h)
    {
        if(!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE || h->size < sizeof(clap_event_param_value_t)) return;
        const auto& e = *reinterpret_cast<const clap_event_param_value_t*>(h);
        if(e.param_id < 1 || e.param_id > s.count() || e.note_id != -1 || e.port_index != -1 || e.channel != -1 || e.key != -1 || !std::isfinite(e.value)) return;
        const auto& p = s.parameter(e.param_id - 1);
        s.values[e.param_id - 1].store(std::clamp(p.stepped ? std::round(e.value) : e.value, p.minimum, p.maximum), std::memory_order_relaxed);
    }

    clap_process_status process(const clap_plugin_t* plugin, const clap_process_t* b)
    {
        auto& s = self(plugin);
        for(uint32_t i = 0; b->in_events && i < b->in_events->size(b->in_events); ++i) apply(s, b->in_events->get(b->in_events, i));
        const uint32_t n = b->frames_count;
        const int mode = int(s.values[0].load(std::memory_order_relaxed));
        auto channel = [](const clap_audio_buffer_t& buffer, uint32_t c) -> float* { return buffer.channel_count > c ? buffer.data32[c] : nullptr; };
        if(s.split)
        {
            if(b->audio_inputs_count < 1 || b->audio_outputs_count < lanes) return CLAP_PROCESS_ERROR;
            const float* l = channel(b->audio_inputs[0], 0); const float* r = channel(b->audio_inputs[0], 1);
            std::array<float*, lanes> ol{}, orr{};
            for(uint32_t k = 0; k < lanes; ++k) { ol[k] = channel(b->audio_outputs[k], 0); orr[k] = channel(b->audio_outputs[k], 1); }
            if(!l || !r || std::find(ol.begin(), ol.end(), nullptr) != ol.end() || std::find(orr.begin(), orr.end(), nullptr) != orr.end()) return CLAP_PROCESS_ERROR;
            const uint32_t bands = mode == 2 ? uint32_t(s.values[1].load(std::memory_order_relaxed)) : 2;
            if(mode == 2)
            {
                const std::array<double, 3> f{s.values[2].load(), s.values[3].load(), s.values[4].load()};
                std::array<double, 3> sorted = f; std::sort(sorted.begin(), sorted.begin() + (bands - 1)); // crossovers in rising order
                // shortcut: new crossover frequencies apply at the next block, without smoothing; smooth them if sweeps click.
                if(bands != s.bands || sorted != s.frequencies)
                {
                    if(bands != s.bands) s.crossovers = {};
                    for(auto& c : s.crossovers) c.set(sorted, bands, s.rate);
                    s.bands = bands; s.frequencies = sorted;
                }
            }
            // Read the input first: outputs may share its buffers.
            for(uint32_t i = 0; i < n; ++i)
            {
                const double left = l[i], right = r[i];
                std::array<double, lanes> a{}, z{};
                if(mode == 0) { a[0] = left; z[0] = left; a[1] = right; z[1] = right; }
                else if(mode == 1) { const double m = (left + right) / 2, d = (left - right) / 2; a[0] = z[0] = m; a[1] = z[1] = d; }
                else { s.crossovers[0].run(left, bands, a); s.crossovers[1].run(right, bands, z); }
                for(uint32_t k = 0; k < lanes; ++k) { ol[k][i] = float(a[k]); orr[k][i] = float(z[k]); }
            }
        }
        else
        {
            if(b->audio_inputs_count < lanes || b->audio_outputs_count < 1) return CLAP_PROCESS_ERROR;
            float* l = channel(b->audio_outputs[0], 0); float* r = channel(b->audio_outputs[0], 1);
            if(!l || !r) return CLAP_PROCESS_ERROR;
            for(uint32_t i = 0; i < n; ++i)
            {
                std::array<double, lanes> il{}, ir{};
                for(uint32_t k = 0; k < lanes; ++k)
                {
                    const auto* a = channel(b->audio_inputs[k], 0); const auto* z = channel(b->audio_inputs[k], 1);
                    il[k] = a ? a[i] : 0; ir[k] = z ? z[i] : (a ? a[i] : 0);
                }
                double left, right;
                if(mode == 0) { left = (il[0] + ir[0]) / 2; right = (il[1] + ir[1]) / 2; }
                else if(mode == 1) { const double m = (il[0] + ir[0]) / 2, d = (il[1] + ir[1]) / 2; left = m + d; right = m - d; }
                else { left = il[0] + il[1] + il[2] + il[3]; right = ir[0] + ir[1] + ir[2] + ir[3]; }
                l[i] = float(left); r[i] = float(right);
            }
        }
        return CLAP_PROCESS_CONTINUE;
    }

    const clap_plugin_audio_ports_t audioPorts{
        [](const clap_plugin_t* p, bool input) -> uint32_t { return self(p).split == input ? 1 : lanes; },
        [](const clap_plugin_t* p, uint32_t index, bool input, clap_audio_port_info_t* info) -> bool {
            if(index >= (self(p).split == input ? 1 : lanes)) return false;
            *info = {}; info->id = index; info->channel_count = 2; info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
            info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0;
            std::snprintf(info->name, sizeof(info->name), (self(p).split == input) ? "Main" : "Lane %u", index + 1);
            return true;
        }};

    const clap_plugin_params_t params{
        [](const clap_plugin_t* p) -> uint32_t { return uint32_t(self(p).count()); },
        [](const clap_plugin_t* p, uint32_t i, clap_param_info_t* out) -> bool {
            const auto& s = self(p);
            if(i >= s.count()) return false;
            const auto& x = s.parameter(i);
            *out = {}; out->id = i + 1; out->flags = CLAP_PARAM_IS_AUTOMATABLE | (x.stepped ? CLAP_PARAM_IS_STEPPED : 0);
            out->min_value = x.minimum; out->max_value = x.maximum; out->default_value = x.initial;
            std::snprintf(out->name, sizeof(out->name), "%s", x.name);
            return true;
        },
        [](const clap_plugin_t* p, clap_id id, double* value) -> bool { if(id < 1 || id > self(p).count()) return false; *value = self(p).values[id - 1].load(); return true; },
        [](const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t n) -> bool {
            const auto& s = self(p);
            if(id < 1 || id > s.count() || !std::isfinite(value)) return false;
            if(id == 1)
            {
                static const char* const splitModes[]{"Left / right", "Mid / side", "Bands"};
                static const char* const joinModes[]{"Left / right", "Mid / side", "Sum"};
                std::snprintf(text, n, "%s", (s.split ? splitModes : joinModes)[std::clamp(int(std::lround(value)), 0, 2)]);
            }
            else if(id == 2) std::snprintf(text, n, "%.0f", value);
            else std::snprintf(text, n, value >= 1000 ? "%.2f kHz" : "%.0f Hz", value >= 1000 ? value / 1000 : value);
            return true;
        },
        [](const clap_plugin_t* p, clap_id id, const char* text, double* value) -> bool {
            const auto& s = self(p);
            if(id < 1 || id > s.count()) return false;
            char* end = nullptr; const double v = std::strtod(text, &end);
            const auto& x = s.parameter(id - 1);
            if(end == text || !std::isfinite(v) || v < x.minimum || v > x.maximum) return false;
            *value = v; return true;
        },
        [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) { for(uint32_t i = 0; in && i < in->size(in); ++i) apply(self(p), in->get(in, i)); }};

    // Text state, independent of byte order and locale: the parameter values.
    const clap_plugin_state_t state{
        [](const clap_plugin_t* p, const clap_ostream_t* stream) -> bool {
            const auto& s = self(p);
            std::string data = s.split ? "oscairn-split-1" : "oscairn-join-1";
            for(size_t i = 0; i < s.count(); ++i)
            {
                char number[32]; const auto result = std::to_chars(number, number + sizeof(number), s.values[i].load(), std::chars_format::general, 17);
                if(result.ec != std::errc{}) return false;
                data += ' '; data.append(number, result.ptr);
            }
            data += '\n';
            for(size_t at = 0; at < data.size();) { const auto written = stream->write(stream, data.data() + at, data.size() - at); if(written <= 0 || size_t(written) > data.size() - at) return false; at += size_t(written); }
            return true;
        },
        [](const clap_plugin_t* p, const clap_istream_t* stream) -> bool {
            auto& s = self(p);
            std::string data;
            for(;;) { char byte; const auto got = stream->read(stream, &byte, 1); if(got < 0 || got > 1) return false; if(!got) break; if(data.size() == 256 || !byte) return false; data += byte; }
            std::istringstream in(data); in.imbue(std::locale::classic());
            std::string header; in >> header;
            if(header != (s.split ? "oscairn-split-1" : "oscairn-join-1")) return false;
            std::array<double, 5> v{};
            for(size_t i = 0; i < s.count(); ++i) { const auto& x = s.parameter(i); if(!(in >> v[i]) || !std::isfinite(v[i]) || v[i] < x.minimum || v[i] > x.maximum) return false; }
            in >> std::ws; if(!in.eof()) return false;
            for(size_t i = 0; i < s.count(); ++i) s.values[i].store(v[i]);
            return true;
        }};

    const clap_plugin_t* create(bool split)
    {
        auto* s = new Node;
        s->split = split;
        for(size_t i = 0; i < s->count(); ++i) s->values[i].store(s->parameter(i).initial);
        s->plugin.desc = split ? &splitDescriptor : &joinDescriptor;
        s->plugin.plugin_data = s;
        s->plugin.init = [](const clap_plugin_t*) { return true; };
        s->plugin.destroy = [](const clap_plugin_t* p) { delete &self(p); };
        s->plugin.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t) {
            if(!std::isfinite(rate) || rate < 1 || rate > 768000) return false;
            auto& s = self(p); s.rate = rate; s.clear(); return true;
        };
        s->plugin.deactivate = [](const clap_plugin_t*) {};
        s->plugin.start_processing = [](const clap_plugin_t*) { return true; };
        s->plugin.stop_processing = [](const clap_plugin_t*) {};
        s->plugin.reset = [](const clap_plugin_t* p) { self(p).clear(); };
        s->plugin.process = process;
        s->plugin.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
            if(!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPorts;
            if(!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
            if(!std::strcmp(id, CLAP_EXT_STATE)) return &state;
            return nullptr;
        };
        s->plugin.on_main_thread = [](const clap_plugin_t*) {};
        return &s->plugin;
    }

    const clap_plugin_factory_t factory{
        [](const clap_plugin_factory_t*) -> uint32_t { return 2; },
        [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* { return index == 0 ? &splitDescriptor : index == 1 ? &joinDescriptor : nullptr; },
        [](const clap_plugin_factory_t*, const clap_host_t*, const char* id) -> const clap_plugin_t* {
            if(!std::strcmp(id, splitDescriptor.id)) return create(true);
            if(!std::strcmp(id, joinDescriptor.id)) return create(false);
            return nullptr;
        }};
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION_INIT, [](const char*) { return true; }, []() {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; }};

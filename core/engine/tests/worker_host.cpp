// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 step 2: a plugin in a worker process renders exactly what it renders in the
// engine, with the same metadata, parameters, output events and state.
//   oscairn-worker-host-test WORKER PLUGIN.clap [PLUGIN.clap...]
#include <oscairn/plugin_host.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    uint32_t channels(const std::vector<kick::clap::Port>& ports) { uint32_t n = 0; for(const auto& p : ports) n += p.channels; return n; }

    void compare(const std::string& worker, const std::string& path)
    {
        constexpr double rate = 44100; constexpr uint32_t frames = 128;
        auto local = oscairn::loadPlugin(path, "", rate, frames, oscairn::ProcessingMode::Offline);
        auto remote = oscairn::loadPluginInWorker(worker, path, "", rate, frames, oscairn::ProcessingMode::Offline);
        const auto name = std::filesystem::path(path).filename().string();
        check(local->descriptor().id == remote->descriptor().id, name + ": descriptor");
        check(local->parameters().size() == remote->parameters().size(), name + ": parameters");
        check(channels(local->inputPorts()) == channels(remote->inputPorts()) && channels(local->outputPorts()) == channels(remote->outputPorts()), name + ": ports");
        check(local->noteInputPorts() == remote->noteInputPorts() && local->noteOutputPorts() == remote->noteOutputPorts(), name + ": note ports");

        const uint32_t ni = channels(local->inputPorts()), no = channels(local->outputPorts());
        std::vector<std::vector<float>> in(ni, std::vector<float>(frames)), a(no, std::vector<float>(frames)), b(no, std::vector<float>(frames));
        std::vector<const float*> inp; std::vector<float*> ap, bp;
        for(auto& c : in) inp.push_back(c.data());
        for(auto& c : a) ap.push_back(c.data());
        for(auto& c : b) bp.push_back(c.data());
        const kick::clap::Parameter* writable = nullptr;
        for(const auto& p : local->parameters()) if(!(p.flags & CLAP_PARAM_IS_READONLY)) { writable = &p; break; }

        uint64_t phase = 0; size_t events = 0; bool same = true; double energy = 0;
        clap_event_transport_t transport{};
        transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        transport.flags = CLAP_TRANSPORT_IS_PLAYING | CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE | CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        transport.tempo = 120; transport.tsig_num = 4; transport.tsig_denom = 4;
        for(uint32_t block = 0; block < 300; ++block)
        {
            for(uint32_t i = 0; i < frames; ++i, ++phase)
                for(uint32_t c = 0; c < ni; ++c) in[c][i] = float(std::sin(0.01 * double(phase) * (c + 1)));
            std::vector<kick::clap::Event> ev;
            if(block == 3) ev.push_back(kick::clap::Event::note(true, 60, .8, 17));
            if(block == 5) ev.push_back(kick::clap::Event::note(true, 67, .5, 0));
            if(block == 120) ev.push_back(kick::clap::Event::note(false, 60, 0, 99));
            if(block == 200) ev.push_back(kick::clap::Event::note(false, 67, 0, 3));
            if(writable && block == 40)
            {
                const double v = writable->minimum + 0.3 * (writable->maximum - writable->minimum);
                const double value = (writable->flags & CLAP_PARAM_IS_STEPPED) ? std::round(v) : v;
                check(local->setParameter(writable->id, value) && remote->setParameter(writable->id, value), name + ": set parameter");
            }
            const double seconds = double(block) * frames / rate;
            transport.song_pos_seconds = clap_sectime(seconds * CLAP_SECTIME_FACTOR);
            transport.song_pos_beats = clap_beattime(seconds * 2 * CLAP_BEATTIME_FACTOR);
            transport.bar_start = clap_beattime(std::floor(seconds * 2 / 4) * 4 * CLAP_BEATTIME_FACTOR);
            transport.bar_number = int32_t(seconds * 2 / 4);
            const bool okA = local->process(inp.data(), ni, ap.data(), no, frames, ev.data(), uint32_t(ev.size()), &transport);
            const bool okB = remote->process(inp.data(), ni, bp.data(), no, frames, ev.data(), uint32_t(ev.size()), &transport);
            check(okA == okB, name + ": process result, block " + std::to_string(block));
            same = same && a == b;
            for(const auto& c : a) for(float x : c) energy += double(x) * x;
            uint32_t ca = 0, cb = 0; const auto* ea = local->outputEvents(ca); const auto* eb = remote->outputEvents(cb);
            check(ca == cb, name + ": output event count, block " + std::to_string(block));
            // An event's own bytes; the rest of its fixed-size buffer is unused and may differ.
            for(uint32_t i = 0; i < std::min(ca, cb); ++i)
                same = same && ea[i].header()->size == eb[i].header()->size && std::memcmp(ea[i].header(), eb[i].header(), ea[i].header()->size) == 0;
            events += ca;
            local->service(); remote->service();
        }
        check(same, name + ": identical audio and events");
        if(writable) check(local->parameterValue(writable->id) == remote->parameterValue(writable->id), name + ": parameter value after the block");
        if(writable) check(local->parameterText(writable->id, writable->defaultValue) == remote->parameterText(writable->id, writable->defaultValue), name + ": parameter text");
        if(remote->supportsState())
        {
            const auto dir = std::filesystem::temp_directory_path() / ("oscairn-worker-state-" + std::to_string(::getpid()));
            std::filesystem::remove_all(dir);
            remote->saveState(dir.string());
            local->loadState(dir.string());
            check(std::filesystem::exists(dir / "state.clap"), name + ": worker saved state");
            std::filesystem::remove_all(dir);
        }
        remote->panic();
        check(remote->process(inp.data(), ni, bp.data(), no, frames, nullptr, 0, nullptr) == local->process(inp.data(), ni, ap.data(), no, frames, nullptr, 0, nullptr), name + ": process after panic");
        check(events > 0 || energy > 0, name + ": the comparison must cover sound or events");
        std::printf("%s: %u in, %u out, audio energy %.3g, %zu output events, identical: %s\n", name.c_str(), ni, no, energy, events, same ? "yes" : "no");
    }
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s WORKER PLUGIN...\n", argv[0]); return 2; }
    try { for(int i = 2; i < argc; ++i) compare(argv[1], argv[i]); }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return failures ? 1 : 0;
}

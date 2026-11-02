// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0010 step 2: a plugin in a worker process renders exactly what it renders in the
// engine, with the same metadata, parameters, output events and state; a parameter info rescan
// in the worker reaches the engine.
//   oscairn-worker-host-test WORKER PLUGIN.clap [PLUGIN.clap...]
#include <oscairn/plugin_host.h>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unistd.h>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    // Field by field: events carry alignment padding (a note's between key and velocity) that a
    // plugin may leave uninitialized, which differs between processes.
    bool sameEvent(const kick::clap::Event& x, const kick::clap::Event& y)
    {
        const auto *a = x.header(), *b = y.header();
        if(a->size != b->size || a->time != b->time || a->space_id != b->space_id || a->type != b->type || a->flags != b->flags) return false;
        if(a->space_id != CLAP_CORE_EVENT_SPACE_ID) return std::memcmp(a, b, a->size) == 0;
        switch(a->type)
        {
        case CLAP_EVENT_NOTE_ON: case CLAP_EVENT_NOTE_OFF: case CLAP_EVENT_NOTE_CHOKE: case CLAP_EVENT_NOTE_END:
        {
            const auto *m = reinterpret_cast<const clap_event_note_t*>(a), *n = reinterpret_cast<const clap_event_note_t*>(b);
            return m->note_id == n->note_id && m->port_index == n->port_index && m->channel == n->channel && m->key == n->key && m->velocity == n->velocity;
        }
        case CLAP_EVENT_MIDI:
        {
            const auto *m = reinterpret_cast<const clap_event_midi_t*>(a), *n = reinterpret_cast<const clap_event_midi_t*>(b);
            return m->port_index == n->port_index && std::memcmp(m->data, n->data, 3) == 0;
        }
        case CLAP_EVENT_PARAM_VALUE: // the cookie is a pointer in each process
        {
            const auto *m = reinterpret_cast<const clap_event_param_value_t*>(a), *n = reinterpret_cast<const clap_event_param_value_t*>(b);
            return m->param_id == n->param_id && m->note_id == n->note_id && m->port_index == n->port_index && m->channel == n->channel && m->key == n->key && m->value == n->value;
        }
        default: return std::memcmp(a, b, a->size) == 0;
        }
    }

    uint32_t channels(const std::vector<kick::clap::Port>& ports) { uint32_t n = 0; for(const auto& p : ports) n += p.channels; return n; }

    void compare(const std::string& worker, const std::string& path)
    {
        constexpr double rate = 44100; constexpr uint32_t frames = 128;
        auto local = oscairn::loadPlugin(path, "", rate, frames, oscairn::ProcessingMode::Offline);
        auto remote = oscairn::loadPluginInWorker(worker, path, "", rate, frames, oscairn::ProcessingMode::Offline);
        const auto name = std::filesystem::path(path).filename().string();
        check(local->descriptor().id == remote->descriptor().id, name + ": descriptor");
        check(local->parameters().size() == remote->parameters().size(), name + ": parameters");
        // The same metadata on both sides, editor hints included (the Basic Synth gives them).
        for(size_t i = 0; i < local->parameters().size() && i < remote->parameters().size(); ++i)
        {
            const auto& a = local->parameters()[i]; const auto& b = remote->parameters()[i];
            check(a.id == b.id && a.name == b.name && a.module == b.module && a.widget == b.widget && a.priority == b.priority && a.rangeHigh == b.rangeHigh && a.group == b.group && a.role == b.role,
                  name + ": parameter " + a.name + " and its hints cross the worker");
        }
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
            for(uint32_t i = 0; i < std::min(ca, cb); ++i) same = same && sameEvent(ea[i], eb[i]);
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

// A plugin in a worker that says it is asleep (CLAP_PROCESS_SLEEP) is reported asleep to the
// engine, as one in the engine is, so a plugin switched off goes as soon as it is done.
void sleeping(const std::string& worker, const std::string& fixture)
{
    constexpr uint32_t frames = 64;
    auto remote = oscairn::loadPluginInWorker(worker, fixture, "org.kick.test.sleep", 48000, frames, oscairn::ProcessingMode::Offline);
    std::vector<float> l(frames), r(frames); float* out[]{l.data(), r.data()};
    check(remote->process(nullptr, 0, out, 2, frames, nullptr, 0, nullptr) && remote->asleep(), "silent, it reports sleep through the worker");
    const auto on = kick::clap::Event::note(true, 60, 1);
    check(remote->process(nullptr, 0, out, 2, frames, &on, 1, nullptr) && !remote->asleep(), "a note wakes it");
}

// The stepped fixture's mode 3 hides its Toggle with an info rescan, which the plugin's host takes
// in place; through the worker the engine's side sees the new info and a new revision.
void rescan(const std::string& worker, const std::string& fixture)
{
    constexpr uint32_t frames = 64;
    auto remote = oscairn::loadPluginInWorker(worker, fixture, "org.kick.test.stepped", 48000, frames, oscairn::ProcessingMode::Offline);
    auto toggle = [&] { for(const auto& p : remote->parameters()) if(p.id == 8) return p; return kick::clap::Parameter{}; };
    const auto revision = remote->parameterRevision();
    check(toggle().id == 8 && !(toggle().flags & CLAP_PARAM_IS_HIDDEN), "the Toggle shows at first");
    std::vector<float> l(frames), r(frames); float* out[]{l.data(), r.data()};
    check(remote->setParameter(9, 3) && remote->process(nullptr, 0, out, 2, frames, nullptr, 0, nullptr), "choose mode 3");
    for(int i = 0; i < 50 && remote->parameterRevision() == revision; ++i) { remote->service(); usleep(2000); }
    check(remote->parameterRevision() != revision && (toggle().flags & CLAP_PARAM_IS_HIDDEN) && toggle().group == 5 && remote->parameterValue(9) == 3,
          "the rescan in the worker hides the Toggle on the engine's side, with a new revision");
}

// The plugin's own window (RFC 0019), shown by the worker: the fixture's effect floats one, so
// nothing is drawn; the synth has none. The worker keeps answering while it is open.
void window(const std::string& worker, const std::string& fixture)
{
    auto effect = oscairn::loadPluginInWorker(worker, fixture, "org.kick.test.effect", 48000, 64);
    auto synth = oscairn::loadPluginInWorker(worker, fixture, "org.kick.test.synth", 48000, 64);
    check(effect->hasWindow() && !synth->hasWindow(), "the effect has a window, the synth none");
    bool refused = false;
    try { synth->showWindow(true, "Synth"); } catch(const std::exception&) { refused = true; }
    check(refused, "a plugin without a window refuses to show one");
    effect->showWindow(true, "KICK effect · Lead");
    effect->service();
    check(effect->setParameter(7, .25) && effect->parameterValue(7) == .25, "the worker answers with the window open");
    effect->showWindow(true, "KICK effect · Bass");
    effect->showWindow(false);
    effect->showWindow(true, "KICK effect");
    effect.reset(); // the node goes, and its window with it
}

// CLAP render: an offline render tells the plugin CLAP_RENDER_OFFLINE, in the engine and through a
// worker; a realtime one, or a plugin that must run in real time, CLAP_RENDER_REALTIME.
void renderMode(const std::string& worker, const std::string& fixture)
{
    const auto log = std::filesystem::temp_directory_path() / ("oscairn-render-mode-" + std::to_string(getpid()));
    ::setenv("KICK_FIXTURE_RENDER_LOG", log.c_str(), 1);
    auto told = [&](bool inWorker, oscairn::ProcessingMode mode) {
        std::filesystem::remove(log);
        if(inWorker) oscairn::loadPluginInWorker(worker, fixture, "org.kick.test.effect", 48000, 64, mode);
        else oscairn::loadPlugin(fixture, "org.kick.test.effect", 48000, 64, mode);
        std::ifstream in(log); std::stringstream text; text << in.rdbuf(); return text.str();
    };
    using M = oscairn::ProcessingMode;
    check(told(false, M::Offline) == "offline\n", "offline in the engine");
    check(told(false, M::Realtime) == "realtime\n", "realtime in the engine");
    check(told(true, M::Offline) == "offline\n", "offline in a worker");
    check(told(true, M::Realtime) == "realtime\n", "realtime in a worker");
    ::setenv("KICK_FIXTURE_HARD_REALTIME", "1", 1);
    check(told(false, M::Offline) == "realtime\n", "a plugin that must run in real time stays realtime");
    ::unsetenv("KICK_FIXTURE_HARD_REALTIME"); ::unsetenv("KICK_FIXTURE_RENDER_LOG");
    std::filesystem::remove(log);
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s WORKER PLUGIN...\n", argv[0]); return 2; }
    try { renderMode(argv[1], argv[2]); sleeping(argv[1], argv[2]); rescan(argv[1], argv[2]); window(argv[1], argv[2]); for(int i = 2; i < argc; ++i) compare(argv[1], argv[i]); }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    return failures ? 1 : 0;
}

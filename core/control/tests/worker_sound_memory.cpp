// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0025: a worker's memory limit follows the sound contexts its plugin prepares after
// activating, each at the context bytes it declares (oscairn.memory), so a shared instance that
// fills with sounds is not stopped at the limit its activation set. A prepare that does not fit
// the memory room is refused before the plugin allocates, and its worker plays on; a plugin
// holding more per sound than it declares is still stopped.
//   oscairn-worker-sound-memory-test WORKER GROWING_SYNTH
#include "oscairn/plugin_host.h"
#include "oscairn/session.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>

using namespace oscairn;

#if defined(__SANITIZE_THREAD__)
constexpr bool shadowed = true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr bool shadowed = true;
#else
constexpr bool shadowed = false;
#endif
#else
constexpr bool shadowed = false;
#endif

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 128;
    const char* id = "org.oscairn.basic-synth";

    // Blocks for a while, so the supervisor samples the worker's memory; its failure, if any.
    std::string play(PluginHost& h)
    {
        std::array<float, frames> l{}, r{}; float* out[2]{l.data(), r.data()};
        std::string why;
        for(int i = 0; i < 30 && (why = h.failure()).empty(); ++i)
        { h.process(nullptr, 0, out, 2, frames, nullptr, 0, nullptr); h.service(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
        return why;
    }
    std::string mib(uint64_t bytes) { return std::to_string(bytes >> 20) + " MiB"; }
}

int main(int argc, char** argv)
{
    if(argc < 3) return 2;
    const char* worker = argv[1]; const char* synth = argv[2];
    // ThreadSanitizer's shadow memory triples what a worker holds, past any declared limit.
    if(shadowed) { std::puts("skipped: workers growing by 16 MiB a sound (ThreadSanitizer's shadow memory)"); return 0; }
    const auto folder = std::filesystem::temp_directory_path() / ("oscairn-sound-memory-" + std::to_string(getpid()));
    std::filesystem::remove_all(folder);
    try
    {
        // Seven sounds prepared beside the Default, 112 MiB, where activating set the limit for 17 MiB.
        {
            const auto h = loadPluginInWorker(worker, synth, id, 48000, frames);
            uint32_t prepared = 0;
            for(int i = 0; i < 7; ++i)
                try { if(h->prepareSound("Sound " + std::to_string(i), {}) != CLAP_INVALID_ID) ++prepared; }
                catch(const std::exception& e) { check(false, std::string("a declared sound is prepared: ") + e.what()); break; }
            const auto why = play(*h);
            check(prepared == 7 && why.empty(), "a worker's plugin prepares seven sounds it declares, " + std::to_string(prepared) + " prepared: " + why);
        }
        // A snapshot of six sounds, 96 MiB, beside the Default: prepared; beside it with 40 MiB left, refused.
        {
            const auto h = loadPlugin(synth, id, 48000, frames); // in this process, without a limit
            for(int i = 0; i < 5; ++i) h->prepareSound("Sound " + std::to_string(i), {});
            h->saveSnapshot((folder / "six").string());
        }
        if(std::filesystem::exists(folder / "six"))
        {
            const auto h = loadPluginInWorker(worker, synth, id, 48000, frames);
            std::string why;
            try { h->prepareSnapshot((folder / "six").string()); } catch(const std::exception& e) { why = e.what(); }
            if(why.empty()) why = play(*h);
            check(why.empty() && h->soundContexts().size() == 7, "a snapshot's six sounds are prepared beside the Default in a worker: " + why);
            const auto room = loadPluginInWorker(worker, synth, id, 48000, frames);
            room->setMemoryRoom(uint64_t(40) << 20);
            why.clear();
            try { room->prepareSnapshot((folder / "six").string()); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("96 MiB of memory where 40 MiB is left") != std::string::npos, "a snapshot whose sounds do not fit the room is refused: " + why);
            check(room->soundContexts().size() == 1 && play(*room).empty(), "its sounds are discarded and its worker plays on");
        }
        // One sound past the room is refused before the plugin prepares it.
        {
            const auto h = loadPluginInWorker(worker, synth, id, 48000, frames);
            h->setMemoryRoom(uint64_t(8) << 20);
            std::string why;
            try { h->prepareSound("Too big", {}); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("16 MiB of memory where 8 MiB is left") != std::string::npos, "a sound past the memory room is refused: " + why);
            check(h->soundContexts().size() == 1 && play(*h).empty(), "before it is prepared, and its worker plays on");
            h->setMemoryRoom(uint64_t(20) << 20);
            why.clear();
            try { h->prepareSound("Fits", {}); } catch(const std::exception& e) { why = e.what(); }
            check(why.empty() && h->soundContexts().size() == 2, "one that fits is prepared: " + why);
        }
        // A plugin holding 100 MiB more per sound than it declares is still stopped at its limit.
        {
            setenv("OSCAIRN_GROWING_SYNTH_EXTRA_MB", "100", 1);
            const auto h = loadPluginInWorker(worker, synth, id, 48000, frames);
            for(int i = 0; i < 2 && h->failure().empty(); ++i) try { h->prepareSound("Liar", {}); } catch(const std::exception&) {}
            std::string why;
            for(int i = 0; i < 100 && (why = h->failure()).empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            unsetenv("OSCAIRN_GROWING_SYNTH_EXTRA_MB");
            const bool killed = std::getenv("OSCAIRN_CGROUP") && why.find("Plugin worker crashed (signal 9)") != std::string::npos;
            check(why.find("Plugin used too much memory") != std::string::npos || killed, "a plugin holding more per sound than it declares is stopped: " + why);
        }
        // RFC 0021 slice 3: parts sharing one instance in a worker, seven sounds beside the first part's.
        {
            PlacementPolicy policy; policy.worker = worker; setPlacementPolicy(policy);
            Session s(48000, frames);
            const auto first = s.add("Part 1", synth, id);
            s.connectAudio(first, graphOutput);
            std::vector<NodeId> parts{first};
            std::string why;
            for(int i = 2; i <= 8; ++i)
                try { parts.push_back(s.shareInstrument("Part " + std::to_string(i), first)); }
                catch(const std::exception& e) { why = e.what(); break; }
            s.commit();
            std::array<float, frames> l{}, r{}; float* out[2]{l.data(), r.data()};
            for(int i = 0; i < 30; ++i) { s.service(); s.process(out, frames); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            const auto failed = s.sessionStatus().failures;
            check(why.empty() && parts.size() == 8 && failed.empty(),
                  "eight parts share one instance in a worker: " + why + (failed.empty() ? std::string() : failed[0].reason));
            uint64_t most = 0;
            for(const auto& w : s.sessionStatus().workers) most = std::max(most, w.memory);
            check(most >= uint64_t(128) << 20, "their eight sounds in one worker, " + mib(most));
        }
    }
    catch(const std::exception& e) { check(false, e.what()); }
    std::filesystem::remove_all(folder);
    if(failures) return 1;
    std::puts("worker sound memory ok");
    return 0;
}

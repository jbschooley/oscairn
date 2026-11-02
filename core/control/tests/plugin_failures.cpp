// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A plugin whose worker fails is named in the session status, through the protocol too: its
// node, name and why it is silent (the 2026-10-31 audit: nothing above the host said so). A
// healthy plugin's worker is listed with the CPU time and memory it uses (RFC 0010 step 6).
//   oscairn-plugin-failures-test WORKER PROCESS_CRASH NORMAL SCAN
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 5) return 2;
    setenv("OSCAIRN_PLUGIN_WORKER", argv[1], 1);
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    try
    {
        constexpr uint32_t frames = 128;
        Session s(48000, frames);
        check(s.sessionStatus().failures.empty(), "no failures at first");
        const auto node = s.add("crashy", argv[2], "org.kick.test.midi");
        const auto healthy = s.add("healthy", argv[3], "org.kick.test.midi");
        s.connectNotes(graphInput, node); s.connectAudio(node, graphOutput);
        s.connectNotes(graphInput, healthy); s.connectAudio(healthy, graphOutput); s.commit();
        std::array<float, frames> l{}, r{}; float* out[2]{l.data(), r.data()};
        // Its worker crashes on every block; it is restarted three times, then given up.
        std::vector<Control::SessionStatus::PluginFailure> seen;
        for(int block = 0; block < 2000; ++block)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(int64_t(1e6 * frames / 48000)));
            s.service(); s.process(out, frames);
            seen = s.sessionStatus().failures;
            if(!seen.empty() && seen[0].reason.find("not restarted") != std::string::npos) break;
        }
        const auto workers = s.sessionStatus().workers;
        const auto usage = std::find_if(workers.begin(), workers.end(), [&](const auto& w) { return w.node == healthy; });
        check(usage != workers.end() && usage->name == "healthy" && usage->cpu > 0 && usage->memory > 0, "the healthy plugin's worker reports its CPU time and memory");
        check(seen.size() == 1 && seen[0].node == node && seen[0].name == "crashy" && seen[0].reason.find("crashed") != std::string::npos,
              "the failed plugin is named with its reason: " + (seen.empty() ? std::string("none") : seen[0].reason));
        JsonRpc rpc{s};
        const auto reply = rpc.dispatch(R"({"jsonrpc":"2.0","id":1,"method":"session.status"})").value_or("");
        check(reply.find("\"failures\":[{\"node\":" + std::to_string(node) + ",\"name\":\"crashy\",\"reason\":\"Plugin worker crashed") != std::string::npos,
              "and in the protocol's status: " + reply);
        check(reply.find("\"workers\":[") != std::string::npos && reply.find("{\"node\":" + std::to_string(healthy) + ",\"name\":\"healthy\",\"cpu\":") != std::string::npos,
              "the protocol's status lists the workers' usage: " + reply);
        // RFC 0005: a worker's plugin lists its presets through the scan helper, never by running its
        // provider in this process; without a helper it lists none and says why, in the protocol too.
        {
            const auto listed = s.add("listed", argv[3], "org.kick.test.midi"); s.commit();
            auto names = [](const std::vector<PresetInfo>& p) { std::string all; for(const auto& x : p) all += x.name + ";"; return all; };
            const auto none = rpc.dispatch(R"({"jsonrpc":"2.0","id":2,"method":"presets.list","params":{"node":"listed"}})").value_or("");
            check(none.find("\"error\"") != std::string::npos && none.find("no plugin scan helper") != std::string::npos,
                  "without a scan helper, a worker's plugin lists no presets, saying why: " + none);
            auto policy = placementPolicy(); policy.scan = argv[4]; setPlacementPolicy(policy);
            check(names(s.presets(listed)) == "Quiet;Bright;", "with one, its presets come from the helper");
            const auto some = rpc.dispatch(R"({"jsonrpc":"2.0","id":3,"method":"presets.list","params":{"node":"listed"}})").value_or("");
            check(some.find("\"name\":\"Quiet\"") != std::string::npos, "and through the protocol: " + some);
            // A hung provider is asked once: the helper's timeout, then the failure is kept for that binary.
            const auto directory = std::filesystem::temp_directory_path() / ("oscairn-presets-" + std::to_string(getpid()));
            std::filesystem::remove_all(directory); std::filesystem::create_directories(directory);
            const auto copy = directory / std::filesystem::path(argv[3]).filename();
            std::filesystem::copy(argv[3], copy, std::filesystem::copy_options::recursive);
            const auto hung = s.add("hung", copy.string(), "org.kick.test.midi"); s.commit();
            setenv("KICK_FIXTURE_PRESETS_HANG", "1", 1); setenv("KICK_FIXTURE_PRESETS_LOG", (directory / "asked").c_str(), 1);
            std::string why;
            const auto began = std::chrono::steady_clock::now();
            for(int i = 0; i < 3; ++i) { why.clear(); try { s.presets(hung); } catch(const std::runtime_error& e) { why = e.what(); } }
            const auto took = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began).count();
            unsetenv("KICK_FIXTURE_PRESETS_HANG"); unsetenv("KICK_FIXTURE_PRESETS_LOG");
            std::ifstream log(directory / "asked"); int asked = 0; for(std::string line; std::getline(log, line);) ++asked;
            check(asked == 1 && took < 25, "a hanging provider is asked once for three listings: " + std::to_string(asked) + " times, " + std::to_string(took) + " s");
            check(why.find("Presets not listed: Plugin scan timed out") != std::string::npos, "and each listing says why: " + why);
            s.remove(hung); s.remove(listed); s.commit();
            std::filesystem::remove_all(directory);
        }
        // RFC 0025: a plugin in a worker declares its memory across the worker protocol, and is
        // refused in the worker before it activates when it declares more than the room given.
        {
            const auto declaring = loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames);
            const auto m = declaring->declaredMemory();
            check(m && m->shared_bytes == 1u << 20 && m->context_bytes == 8u << 20, "a worker's plugin declares its memory");
            std::string why;
            try { loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames, ProcessingMode::Realtime, 0, 4u << 20); }
            catch(const std::exception& e) { why = e.what(); }
            check(why.find("9 MiB of memory where 4 MiB is left") != std::string::npos, "and is refused in its worker before activating: " + why);
#ifdef __linux__
            // With a delegated cgroup subtree, the worker's cgroup holds it to its plugin's declaration plus the margin.
            if(const char* root = std::getenv("OSCAIRN_CGROUP"))
            {
                const uint64_t declared = 9u << 20, limit = declared + declared / 2 + (64u << 20);
                bool found = false;
                for(const auto& e : std::filesystem::directory_iterator(std::filesystem::path(root) / "workers"))
                {
                    std::ifstream in(e.path() / "memory.max"); uint64_t max = 0; in >> max;
                    found = found || max == limit;
                }
                check(found, "its worker's cgroup memory.max is the declaration plus the margin, " + std::to_string(limit));
            }
#endif
        }
        // RFC 0025: a plugin that restarts itself into a mode holding far more memory must not be
        // killed for its old declaration: the worker tells the engine what it declares before it
        // activates again, and the engine refits the worker's limit before the plugin allocates,
        // however long the allocation takes (here 400 ms, many of the supervisor's samples).
        {
            setenv("KICK_FIXTURE_MEMORY_SLOW_MS", "400", 1);
            const auto self = loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames);
            unsetenv("KICK_FIXTURE_MEMORY_SLOW_MS");
            std::array<float, frames> m{}; float* out[1]{m.data()};
            uint64_t shared = 0;
            bool killed = false;
            for(int i = 0; i < 100; ++i)
            {
                self->setParameter(7, .95); // gain .95: the fixture's big mode, which restarts it
                self->process(nullptr, 0, out, 1, frames, nullptr, 0, nullptr);
                self->service();
                try { if(const auto d = self->declaredMemory()) shared = d->shared_bytes; } catch(const std::exception&) {}
                killed = !self->failure().empty();
                if(shared > (uint64_t(100) << 20) || killed) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the supervisor's samples since
            killed = killed || !self->failure().empty();
            check(shared == 201u << 20, "the engine re-read the declaration after the plugin's own restart: " + std::to_string(shared >> 20) + " MiB");
            check(!killed, "and the mode change did not kill the worker: " + self->failure());
        }
        // RFC 0025: an engine's shared bytes count toward its worker's limit while any node of it
        // lives, even after the root node the contexts were made from is removed (effect-contexts.h).
        {
            setenv("KICK_FIXTURE_MEMORY_CONTEXTS", "1", 1);
            setenv("KICK_FIXTURE_MEMORY_SHARED_MB", "300", 1);
            auto root = loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames);
            const auto context = root->createEffectContext();
            const auto directory = std::filesystem::temp_directory_path() / ("oscairn-context-state-" + std::to_string(getpid()));
            std::filesystem::remove_all(directory);
            context->saveState(directory.string());
            root = nullptr; // the instrument goes; the context keeps the engine and its 300 MiB alive
            context->loadState(directory.string());
            std::array<float, frames> m{}; float* out[1]{m.data()};
            std::string why;
            for(int i = 0; i < 100 && (why = context->failure()).empty(); ++i)
            { context->process(nullptr, 0, out, 1, frames, nullptr, 0, nullptr); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
            unsetenv("KICK_FIXTURE_MEMORY_SHARED_MB");
            unsetenv("KICK_FIXTURE_MEMORY_CONTEXTS");
            std::filesystem::remove_all(directory);
            check(why.empty(), "the state load on the orphaned context kept the engine's shared bytes in the limit: " + why);
        }
        // RFC 0025 test 4, a wrong declaration: a plugin declaring 9 MiB and holding 200 more is
        // stopped at its worker's limit (the declaration plus the margin, not the policy's 4 GiB),
        // and nothing else is.
        {
            setenv("KICK_FIXTURE_MEMORY_EXTRA_MB", "200", 1);
            const auto liar = loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames);
            unsetenv("KICK_FIXTURE_MEMORY_EXTRA_MB");
            const auto honest = loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames);
            std::string why;
            for(int i = 0; i < 300 && (why = liar->failure()).empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // In its own cgroup the kernel may stop it first, at memory.max.
            const bool killed = std::getenv("OSCAIRN_CGROUP") && why.find("Plugin worker crashed (signal 9)") != std::string::npos;
            check((why.find("Plugin used too much memory") != std::string::npos && why.find("over its limit of 77 MB") != std::string::npos) || killed,
                  "a plugin holding more than it declared is stopped at the declaration's limit: " + why);
            check(honest->failure().empty(), "and a plugin holding what it declared is not");
        }
#ifdef __linux__
        // RFC 0025 test 6, in part: at instrument level a worker locks its memory (VmLck its whole
        // resident memory) and takes its place in the out-of-memory order; so does the engine, or
        // says why not (lowering its order needs CAP_SYS_RESOURCE, locking a high RLIMIT_MEMLOCK).
        {
            setenv("OSCAIRN_RELIABILITY", "instrument", 1);
            const auto missed = applyMemoryPolicy(-999);
            const bool canLock = missed.find("not locked") == std::string::npos;
            const auto locked = loadPluginInWorker(argv[1], argv[3], "org.kick.test.memory", 48000, frames); (void)locked;
            auto status = [](const std::string& dir, const std::string& key) {
                std::ifstream in(dir + "/status"); std::string line;
                while(std::getline(in, line)) if(line.rfind(key + ":", 0) == 0) return std::strtoull(line.c_str() + key.size() + 1, nullptr, 10);
                return 0ull;
            };
            auto read = [](const std::string& file) { std::ifstream in(file); std::string s; std::getline(in, s); return s; };
            std::string worker;
            for(const auto& e : std::filesystem::directory_iterator("/proc"))
            {
                const auto stat = read(e.path().string() + "/stat");
                const auto close = stat.rfind(')');
                if(stat.find("(oscairn-plugin") == std::string::npos || close == std::string::npos) continue;
                unsigned long ppid = 0; char state = 0;
                if(std::sscanf(stat.c_str() + close + 1, " %c %lu", &state, &ppid) == 2 && ppid == unsigned(getpid()) && read(e.path().string() + "/oom_score_adj") == "300")
                    worker = e.path().string();
            }
            check(!worker.empty(), "a worker at instrument level is at 300 in the out-of-memory order");
            const auto lockedKb = worker.empty() ? 0 : status(worker, "VmLck"), residentKb = worker.empty() ? 0 : status(worker, "VmRSS");
            check(!canLock || (lockedKb > 0 && lockedKb + 64 >= residentKb), "and has its memory locked (" + std::to_string(lockedKb) + " of " + std::to_string(residentKb) + " kB)");
            check(missed.empty() ? read("/proc/self/oom_score_adj") == "-999" && status("/proc/self", "VmLck") > 0 : missed.find("oom_score_adj") != std::string::npos || !canLock,
                  "the engine locked and last in the out-of-memory order, or saying why not: " + (missed.empty() ? std::string("done") : missed));
            std::printf("instrument level here: %s\n", missed.empty() ? "locked, oom_score_adj -999" : missed.c_str());
            unsetenv("OSCAIRN_RELIABILITY");
        }
#endif
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a failed plugin is named in the session status");
    return failures ? 1 : 0;
}

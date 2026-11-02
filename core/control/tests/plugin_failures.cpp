// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A plugin whose worker fails is named in the session status, through the protocol too: its
// node, name and why it is silent (the 2026-10-31 audit: nothing above the host said so). A
// healthy plugin's worker is listed with the CPU time and memory it uses (RFC 0010 step 6).
//   oscairn-plugin-failures-test WORKER PROCESS_CRASH NORMAL
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 4) return 2;
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
        }
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a failed plugin is named in the session status");
    return failures ? 1 : 0;
}

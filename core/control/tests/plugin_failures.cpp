// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A plugin whose worker fails is named in the session status, through the protocol too: its
// node, name and why it is silent (the 2026-10-31 audit: nothing above the host said so).
//   oscairn-plugin-failures-test WORKER PROCESS_CRASH
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace oscairn;

int main(int argc, char** argv)
{
    if(argc < 3) return 2;
    setenv("OSCAIRN_PLUGIN_WORKER", argv[1], 1);
    int failures = 0;
    auto check = [&](bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } };
    try
    {
        constexpr uint32_t frames = 128;
        Session s(48000, frames);
        check(s.sessionStatus().failures.empty(), "no failures at first");
        const auto node = s.add("crashy", argv[2], "org.kick.test.midi");
        s.connectNotes(graphInput, node); s.connectAudio(node, graphOutput); s.commit();
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
        check(seen.size() == 1 && seen[0].node == node && seen[0].name == "crashy" && seen[0].reason.find("crashed") != std::string::npos,
              "the failed plugin is named with its reason: " + (seen.empty() ? std::string("none") : seen[0].reason));
        JsonRpc rpc{s};
        const auto reply = rpc.dispatch(R"({"jsonrpc":"2.0","id":1,"method":"session.status"})").value_or("");
        check(reply.find("\"failures\":[{\"node\":" + std::to_string(node) + ",\"name\":\"crashy\",\"reason\":\"Plugin worker crashed") != std::string::npos,
              "and in the protocol's status: " + reply);
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: a failed plugin is named in the session status");
    return failures ? 1 : 0;
}

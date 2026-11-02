// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0005 draft 5: the Basic Synth's factory presets, listed through CLAP preset discovery and
// prepared as native sound contexts over JSON-RPC; a held note keeps its sound; a Performance
// saved with a preset's sound reopens with it.
//   oscairn-sound-presets-test BASIC_SYNTH.clap
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <unistd.h>
using namespace oscairn;
namespace
{
    void check(bool ok, const std::string& why) { if(!ok) throw std::runtime_error(why); }
    struct Fixture
    {
        Session session{48000, 512}; JsonRpc rpc{session}; std::array<float, 512> left{}, right{};
        explicit Fixture(const std::string& synth = {})
        {
            if(synth.empty()) return;
            const auto n = session.add("synth", synth); session.connectNotes(graphInput, n); session.connectAudio(n, graphOutput); session.commit();
        }
        std::string call(const std::string& method, const std::string& params)
        { return rpc.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"" + method + "\",\"params\":" + params + '}').value_or(""); }
        void render() { float* out[]{left.data(), right.data()}; session.service(); check(session.process(out, 512), "render"); session.service(); }
        double brightness() { return session.parameter(session.nodeId("synth"), 1); }
    };
}
int main(int argc, char** argv)
{
    if(argc != 2) return 2;
    const auto root = std::filesystem::temp_directory_path() / ("oscairn-sound-presets-" + std::to_string(getpid()));
    try
    {
        Fixture f(argv[1]);
        check(f.call("session.hello", "{}").find("\"sounds.presets\"") != std::string::npos, "session.hello offers native preset sounds");
        // In a worker, the scan helper lists them off this thread: asked again until they are in.
        auto listed = f.call("presets.list", R"({"node":"synth"})");
        for(int tries = 0; listed.find("Presets are being listed; ask again") != std::string::npos && tries < 1500; ++tries)
        { std::this_thread::sleep_for(std::chrono::milliseconds(20)); listed = f.call("presets.list", R"({"node":"synth"})"); }
        check(listed.find(R"({"uri":"clap-preset:1:#warm","name":"Warm"},{"uri":"clap-preset:1:#bright","name":"Bright"},{"uri":"clap-preset:1:#pressure-swell","name":"Pressure swell"})") != std::string::npos &&
              listed.find("Square vibrato") != std::string::npos, "the Basic Synth lists its factory presets: " + listed);

        // A held note keeps the Default sound; the next note plays the preset's.
        f.session.note(true, 48, .8); f.render();
        const auto prepared = f.call("sounds.prepare", R"({"node":"synth","name":"Warm","preset":"clap-preset:1:#warm"})");
        check(prepared.find(R"("context":1)") != std::string::npos, "a preset prepares a native context: " + prepared);
        check(f.call("sounds.select", R"({"node":"synth","context":1})").find("\"accepted\":true") != std::string::npos, "selected");
        f.render(); f.session.note(true, 60, .8); f.render();
        const auto sounds = f.session.sounds(f.session.nodeId("synth"));
        check(sounds.mode == "native" && sounds.contexts.size() == 2 && sounds.contexts[0].activeVoices == 1 && sounds.contexts[1].name == "Warm" &&
              sounds.contexts[1].activeVoices == 1, "the held note stays on Default while the new one plays Warm");
        check(f.brightness() == .15, "Warm's patch is the preset's");

        const auto missing = f.call("sounds.prepare", R"({"node":"synth","name":"Gone","preset":"clap-preset:1:#gone"})");
        check(missing.find("-32005") != std::string::npos && missing.find("no such preset") != std::string::npos, "an unknown preset says so: " + missing);
        check(f.call("sounds.prepare", R"({"node":"synth","name":"Both","preset":"clap-preset:1:#warm","parameters":[{"id":1,"value":0.5}]})").find("-32602") != std::string::npos,
              "a native preset takes no parameters too");
        check(f.call("sounds.prepare", R"({"node":"synth","name":"File","preset":"clap-preset:0:%2Fnowhere.txt#"})").find("no such preset") != std::string::npos,
              "a file the Basic Synth never listed is refused");

        // Saved as the plugin's snapshot, not as a reference to the preset.
        f.session.note(false, 48, 0); f.session.note(false, 60, 0); f.render();
        std::filesystem::create_directories(root); f.session.savePerformance((root / "saved").string());
        Fixture reopened; reopened.session.loadPerformance((root / "saved").string()); reopened.render();
        const auto again = reopened.session.sounds(reopened.session.nodeId("synth"));
        check(again.contexts.size() == 2 && std::string(again.contexts[1].name) == "Warm" && reopened.brightness() == .15, "the Performance reopens with the preset's sound selected");
        std::filesystem::remove_all(root);
        std::cout << "ok: native sounds from CLAP presets\n";
        return 0;
    }
    catch(const std::exception& e) { std::filesystem::remove_all(root); std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021 slice 3: parts sharing one engine, through the Session. Usage: shared-parts basic-synth.
//
// Three Basic Synth parts, each its own sound on its own keys, saved twice: with an instance each,
// and on one instance (Control::shareInstrument). Opened, the shared one loads one instance and
// sounds as the separate one; saved again and reopened it is the same. Sharing past the room an
// instance has (its sounds) spills to a second instance, and the next share fills that one.
// Adding a sharing part and removing one undo and redo. Changes from the separate layout to the
// shared one and back sound as each opened fresh. The protocol shares and lists the shares.
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "oscairn/plugin_host.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t frames = 128;
    const char* synth = nullptr;

    void block(Session& s, std::vector<float>* into = nullptr, std::span<const kick::clap::Event> events = {})
    {
        std::array<float, frames> l{}, r{}; float* out[2]{l.data(), r.data()};
        s.service(); forbidAllocation = true;
        const bool ok = s.process(out, frames, events);
        forbidAllocation = false; check(ok, "render");
        if(into) { into->insert(into->end(), l.begin(), l.end()); into->insert(into->end(), r.begin(), r.end()); }
    }
    void quiet(Session& s) { for(int i = 0; i < int(48000 * 3.2 / frames); ++i) block(s); }
    // A note in each part's keys, held, then released, with a bend on the keyboard's channel.
    std::vector<float> play(Session& s, int blocks = 300)
    {
        std::vector<float> out;
        for(int i = 0; i < blocks; ++i)
        {
            std::vector<kick::clap::Event> e;
            if(i == 1) for(int k : {20, 60, 100}) e.push_back(kick::clap::Event::note(true, k, .8));
            if(i == 30) e.push_back(kick::clap::Event::midi(0xe0, 0, 90));
            if(i == 120) for(int k : {20, 60, 100}) e.push_back(kick::clap::Event::note(false, k, 0));
            if(i == 200) e.push_back(kick::clap::Event::midi(0xe0, 0, 64));
            block(s, &out, e);
        }
        return out;
    }
    double difference(const std::vector<float>& a, const std::vector<float>& b)
    {
        if(a.size() != b.size()) return 1e9;
        double worst = 0; for(size_t i = 0; i < a.size(); ++i) worst = std::max(worst, double(std::abs(a[i] - b[i])));
        return worst;
    }
    bool sounds(const std::vector<float>& a) { return std::any_of(a.begin(), a.end(), [](float x) { return x != 0; }); }
    const std::vector<std::vector<oscairn_sound_parameter_t>> patches{{{1, .3}, {4, 1}, {5, 11}}, {{1, .7}, {4, 2}, {5, 4}}, {{1, .5}, {4, 0}, {5, 20}, {7, 30}}};

    // Three parts, a third of the keyboard each; shared: the second and third on the first's instance.
    std::vector<NodeId> build(Session& s, bool shared)
    {
        std::vector<NodeId> nodes;
        for(uint32_t i = 0; i < 3; ++i)
        {
            const auto name = "synth " + std::to_string(i);
            const auto n = shared && i ? s.shareInstrument(name, nodes.front()) : s.add(name, synth);
            PartInput keys; keys.zones = {NoteZone{uint8_t(43 * i), uint8_t(std::min(127u, 43 * i + 42))}};
            const auto part = s.addPart("Part " + std::to_string(i), keys);
            s.insertIntoPart(part, 0, n); s.commit();
            check(s.selectSound(n, s.prepareSound(n, "Sound " + std::to_string(i), patches[i])), "a sound for part " + std::to_string(i));
            nodes.push_back(n);
        }
        block(s); block(s);
        return nodes;
    }
    std::string slurp(const fs::path& file) { std::ifstream in(file); std::stringstream b; b << in.rdbuf(); return b.str(); }
    size_t count(const std::string& text, const std::string& what) { size_t n = 0; for(auto at = text.find(what); at != std::string::npos; at = text.find(what, at + 1)) ++n; return n; }
}

int main(int argc, char** argv)
{
    if(argc != 2) return 2;
    synth = argv[1];
    state_files::TemporaryDirectory temp;
    const auto separate = temp.path / "separate", shared = temp.path / "shared", again = temp.path / "again";
    {
        Session s(48000, frames); build(s, false); s.savePerformance(separate.string());
    }
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        const auto nodes = build(s, true);
        check(pluginInstancesLoaded() - before == 1, "three parts on one instance: " + std::to_string(pluginInstancesLoaded() - before));
        const auto g = s.graphState();
        check(g.shared.size() == 3 && g.shared.at(nodes[1]).engine == nodes[0] && g.shared.at(nodes[2]).engine == nodes[0], "the graph lists the three as one engine");
        check(g.shared.at(nodes[0]).channel == 0 && g.shared.at(nodes[1]).channel == 1 && g.shared.at(nodes[2]).channel == 2, "a channel each");
        check(g.shared.at(nodes[0]).bus == 0 && g.shared.at(nodes[1]).bus == 1 && g.shared.at(nodes[2]).bus == 2, "the first keeps the main output; the others a bus each");
        s.savePerformance(shared.string());
    }
    const auto document = slurp(shared / "performance.json");
    check(document.find("\"version\":12") != std::string::npos, "version 12");
    check(count(document, "\"engine\":\"node-1\"") == 3 && count(document, "\"sound\":{\"channel\":") == 3, "three nodes, one engine, each its sound");
    check(fs::exists(shared / "nodes" / "node-1" / "engine" / "snapshot") && !fs::exists(shared / "nodes" / "node-2" / "engine"), "the engine's state with its first node");

    std::vector<float> reference, fresh;
    {
        Session s(48000, frames); s.loadPerformance(separate.string()); block(s);
        reference = play(s);
    }
    check(sounds(reference), "the separate layout sounds");
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        s.loadPerformance(shared.string()); block(s);
        check(pluginInstancesLoaded() - before == 1, "the shared layout opens one instance: " + std::to_string(pluginInstancesLoaded() - before));
        fresh = play(s);
        std::printf("shared parts: largest difference from separate instances %.3g\n", difference(reference, fresh));
        check(difference(reference, fresh) < 1e-6, "three parts on one instance sound as three instances");
        quiet(s);
        s.savePerformance(again.string());
        const auto resaved = slurp(again / "performance.json");
        check(count(resaved, "\"sound\":{\"channel\":") == 3 && count(resaved, "\"engine\":\"node-1\"") == 3, "saved again, still one engine");
    }
    {
        Session s(48000, frames); s.loadPerformance(again.string()); block(s);
        check(difference(reference, play(s)) < 1e-6, "reopened, the same");
    }

    // Spill: a fresh Basic Synth holds eight sounds, its Default one of them; the eighth share needs a
    // second instance, and the ninth goes there too.
    {
        Session s(48000, frames);
        const auto first = s.add("one", synth);
        std::vector<NodeId> all{first};
        const auto before = pluginInstancesLoaded();
        for(int i = 0; i < 9; ++i) all.push_back(s.shareInstrument("more " + std::to_string(i), first));
        s.commit();
        check(pluginInstancesLoaded() - before == 1, "ten parts on two instances: " + std::to_string(pluginInstancesLoaded() - before));
        const auto g = s.graphState();
        check(g.shared.at(all[7]).engine == first && g.shared.at(all[8]).engine == all[8] && g.shared.at(all[9]).engine == all[8], "the eighth share spills to an instance of its own, the ninth joins it");
        s.savePerformance((temp.path / "spill").string());
        Session t(48000, frames);
        const auto opened = pluginInstancesLoaded();
        t.loadPerformance((temp.path / "spill").string());
        check(pluginInstancesLoaded() - opened == 2, "reopened on two instances");
    }

    // Undo: adding a sharing part, and removing one.
    {
        Session s(48000, frames); s.loadPerformance(shared.string()); block(s);
        auto parts = s.parts();
        const auto source = parts.front().chain.front();
        const auto n = s.shareInstrument("fourth", source);
        PartInput keys; keys.zones = {NoteZone{0, 10}};
        const auto part = s.addPart("Part 3", keys); s.insertIntoPart(part, 0, n); s.commit();
        block(s);
        check(s.graphState().shared.size() == 4, "a fourth part on the instance");
        while(s.parts().size() > 3 && s.undo()) {}
        while(s.hasNode(n) && s.undo()) {}
        block(s);
        check(!s.hasNode(n) && s.parts().size() == 3 && s.graphState().shared.size() == 3, "undoing takes the sharing part away");
        check(difference(reference, play(s)) < 1e-6, "and the other three sound as before");
        quiet(s);
        while(!s.hasNode(n) && s.redo()) {}
        while(s.parts().size() < 4 && s.redo()) {}
        block(s);
        check(s.hasNode(n) && s.parts().size() == 4 && s.graphState().shared.size() == 4, "redoing puts it back");

        // Undoing a share that made its source the engine's first node gives the source its own instance back.
        Session u(48000, frames);
        const auto alone = u.add("alone", synth);
        PartInput all; const auto own = u.addPart("Alone", all); u.insertIntoPart(own, 0, alone); u.commit(); block(u);
        const auto solo = play(u); quiet(u);
        const auto joined = u.shareInstrument("joined", alone); u.commit(); block(u);
        check(u.graphState().shared.size() == 2, "the source is the engine's first node");
        while(u.hasNode(joined) && u.undo()) {}
        block(u);
        check(u.graphState().shared.empty(), "undone, the source has its own instance again");
        check(difference(solo, play(u)) < 1e-6, "and sounds as before");

        // Removing the second part and its node, then undoing.
        Session t(48000, frames); t.loadPerformance(shared.string()); block(t);
        const auto second = t.parts()[1];
        const auto node = second.chain.front();
        t.removePart(second.id); t.remove(node); t.commit(); block(t);
        check(!t.hasNode(node) && t.graphState().shared.size() == 2, "the second part and its node removed");
        auto without = play(t);
        check(difference(reference, without) > 1e-3, "without it the output differs");
        quiet(t);
        while((!t.hasNode(node) || t.parts().size() < 3) && t.undo()) {}
        block(t);
        check(t.hasNode(node) && t.parts().size() == 3 && t.graphState().shared.size() == 3, "undone, it is back on the instance");
        check(difference(reference, play(t)) < 1e-6, "and sounds as before");
    }

    // A Performance change into the shared layout and out of it again.
    {
        Session s(48000, frames); s.loadPerformance(separate.string()); block(s);
        play(s); quiet(s);
        s.loadPerformance(shared.string()); quiet(s);
        check(s.graphState().shared.size() == 3 && !s.leaving(), "into the shared layout, the separate instances rung out and gone");
        check(difference(fresh, play(s)) < 1e-6, "sounding as it does opened fresh");
        quiet(s);
        s.loadPerformance(separate.string()); quiet(s);
        check(s.graphState().shared.empty() && !s.leaving(), "and out of it, the shared nodes rung out and gone");
        check(difference(reference, play(s)) < 1e-6, "sounding as the separate layout opened fresh");
    }

    // The protocol: instruments.share, and graph.get's shared.
    {
        Session s(48000, frames);
        JsonRpc rpc(s);
        const auto first = s.add("lead", synth); s.commit();
        const auto reply = rpc.dispatch(R"({"jsonrpc":"2.0","id":1,"method":"instruments.share","params":{"source":"lead","name":"pad"}})");
        check(reply && reply->find("\"node\":") != std::string::npos, "instruments.share answers the new node: " + reply.value_or(""));
        s.commit();
        const auto graph = rpc.dispatch(R"({"jsonrpc":"2.0","id":2,"method":"graph.get"})");
        check(graph && graph->find("\"shared\":{\"engine\":" + std::to_string(first) + ",\"channel\":1,\"bus\":1}") != std::string::npos, "graph.get lists the share");
        const auto refused = rpc.dispatch(R"({"jsonrpc":"2.0","id":3,"method":"instruments.share","params":{"source":"nobody"}})");
        check(refused && refused->find("\"error\"") != std::string::npos, "an unknown source is refused");
    }

    if(failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
    std::puts("ok: parts sharing one engine");
    return 0;
}

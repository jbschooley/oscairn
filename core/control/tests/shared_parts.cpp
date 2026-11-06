// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021 slice 3: parts sharing one engine, through the Session. Usage: shared-parts basic-synth.
//
// Parts whose instruments are Basic Synths share one instance on their own. Three parts saved with
// an instance each open on one instance and sound as the three instances did; saved again they keep
// the shared layout and reopen the same. Nine such parts open on two instances (the ninth spills),
// and so do nine added one by one. Adding a Basic Synth joins the instance one plays; undoing and
// redoing it, and removing a sharing part, sound as before. A sharing part saves as a Sound of its
// own, which joins an instance where it is loaded. Changes between the layouts put the incoming
// sounds into the playing instance and sound as each opened fresh. Asked for, a share spills the
// same way, and the protocol shares and lists the shares.
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "oscairn/plugin_host.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <set>
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
    // Until the last Performance's nodes have rung out and gone.
    void gone(Session& s) { for(int i = 0; i < int(48000 * 31 / frames) && s.leaving(); ++i) block(s); block(s); block(s); }
    // A note in each of three parts' keys, held, then released, with a bend on the keyboard's channel.
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
    PartInput keysOf(uint32_t i, uint32_t n) { const uint32_t w = n == 3 ? 43 : 128 / n; PartInput k; k.zones = {NoteZone{uint8_t(w * i), uint8_t(i + 1 == n ? 127 : std::min(127u, w * i + w - 1))}}; return k; }
    size_t engines(const Session& s) { std::set<NodeId> e; for(const auto& [n, x] : s.graphState().shared) e.insert(x.engine); return e.size(); }

    // `n` Basic Synth parts, each its keys, playing its sound (the first three patches in turn) on
    // its Default, on an instance of its own: each instance is full (seven more sounds prepared) when
    // the next is added, so none is shared, then the extra sounds go. Saved, a Performance with an
    // instance per part, as one from before parts shared or a converted one.
    std::vector<NodeId> separately(Session& s, uint32_t n)
    {
        std::vector<NodeId> nodes;
        for(uint32_t i = 0; i < n; ++i)
        {
            const auto node = s.add("synth " + std::to_string(i), synth);
            const auto part = s.addPart("Part " + std::to_string(i), keysOf(i, n));
            s.insertIntoPart(part, 0, node); s.commit();
            for(const auto& p : patches[i % 3]) check(s.setParameter(node, p.id, p.value), "a value of part " + std::to_string(i));
            for(int k = 0; k < 7; ++k) s.prepareSound(node, "filler", {});
            nodes.push_back(node);
        }
        block(s); block(s);
        for(const auto node : nodes) for(const auto& c : s.sounds(node).contexts) if(c.name == "filler") check(s.discardSound(node, c.id), "a filler discarded");
        block(s);
        check(s.graphState().shared.empty(), "an instance each");
        return nodes;
    }
    // Three parts as `separately`, each sound prepared and selected; the second and third added (on
    // their own, sharing the first's instance) or shared on request.
    std::vector<NodeId> build(Session& s, bool asked)
    {
        std::vector<NodeId> nodes;
        for(uint32_t i = 0; i < 3; ++i)
        {
            const auto name = "synth " + std::to_string(i);
            const auto n = asked && i ? s.shareInstrument(name, nodes.front()) : s.add(name, synth);
            const auto part = s.addPart("Part " + std::to_string(i), keysOf(i, 3));
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
    state_files::TemporaryDirectory temp, home;
    ::setenv("OSCAIRN_DATA_HOME", home.path.c_str(), 1); // a library of its own, for the Sounds
    const auto separate = temp.path / "separate", shared = temp.path / "shared", again = temp.path / "again", nine = temp.path / "nine";

    // Saved with an instance per part, and what those instances play.
    std::vector<float> reference, reference9;
    {
        Session s(48000, frames); separately(s, 3); s.savePerformance(separate.string());
        reference = play(s);
    }
    check(sounds(reference), "the separate layout sounds");
    check(count(slurp(separate / "performance.json"), "\"sound\":null") == 3, "saved with an instance each");
    {
        Session s(48000, frames); separately(s, 9); s.savePerformance(nine.string());
        reference9 = play(s);
    }

    // Opened, the three form one engine on their own and sound as the three instances did.
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        s.loadPerformance(separate.string()); block(s);
        check(pluginInstancesLoaded() - before == 1, "three parts saved separately open on one instance: " + std::to_string(pluginInstancesLoaded() - before));
        const auto g = s.graphState();
        check(g.shared.size() == 3 && engines(s) == 1, "the three as one engine");
        const auto second = s.parts()[1].chain.front();
        check(std::abs(s.parameter(second, 1) - .7) < 1e-9 && std::abs(s.parameter(second, 5) - 4) < 1e-9, "a sharing part shows its own sound's values: " + std::to_string(s.parameter(second, 1)));
        const auto fresh = play(s);
        std::printf("shared parts: largest difference from separate instances %.3g\n", difference(reference, fresh));
        check(difference(reference, fresh) < 1e-6, "three parts on one instance sound as three instances");
        quiet(s);
        s.savePerformance(shared.string());
        const auto document = slurp(shared / "performance.json");
        check(document.find("\"version\":13") != std::string::npos, "version 13");
        check(count(document, "\"engine\":\"node-1\"") == 3 && count(document, "\"sound\":{\"channel\":") == 3, "saved shared: three nodes, one engine, each its sound");
        check(fs::exists(shared / "nodes" / "node-1" / "engine" / "snapshot") && !fs::exists(shared / "nodes" / "node-2" / "engine"), "the engine's state with its first node");
    }
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        s.loadPerformance(shared.string()); block(s);
        check(pluginInstancesLoaded() - before == 1, "reopened on one instance");
        check(difference(reference, play(s)) < 1e-6, "reopened, the same");
        quiet(s);
        s.savePerformance(again.string());
        check(count(slurp(again / "performance.json"), "\"sound\":{\"channel\":") == 3, "saved again, still one engine");
    }

    // Spilling as it is read: an instance holds eight sounds, so the ninth part opens on a second one.
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        s.loadPerformance(nine.string()); block(s);
        check(pluginInstancesLoaded() - before == 2 && s.graphState().shared.size() == 9 && engines(s) == 2, "nine parts open on two instances: " + std::to_string(pluginInstancesLoaded() - before));
        check(difference(reference9, play(s)) < 1e-6, "and sound as nine instances");
    }
    // And added one by one: the ninth has an instance of its own, which the tenth joins.
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        std::vector<NodeId> all;
        for(int i = 0; i < 9; ++i) all.push_back(s.add("synth " + std::to_string(i), synth));
        s.commit();
        auto g = s.graphState();
        check(pluginInstancesLoaded() - before == 2 && g.shared.size() == 8 && !g.shared.count(all[8]), "nine added: eight on one instance, the ninth on its own");
        all.push_back(s.add("synth 9", synth)); s.commit();
        g = s.graphState();
        check(pluginInstancesLoaded() - before == 2 && g.shared.size() == 10 && g.shared.at(all[9]).engine == all[8], "the tenth joins the ninth");
    }

    // Adding a plugin joins the instance one plays, as a sound of its own; undone and redone.
    {
        Session s(48000, frames);
        const auto before = pluginInstancesLoaded();
        const auto nodes = build(s, false);
        check(pluginInstancesLoaded() - before == 1, "three added on one instance: " + std::to_string(pluginInstancesLoaded() - before));
        const auto g = s.graphState();
        check(g.shared.size() == 3 && g.shared.at(nodes[1]).engine == nodes[0] && g.shared.at(nodes[2]).engine == nodes[0], "the graph lists the three as one engine");
        check(g.shared.at(nodes[0]).channel == 0 && g.shared.at(nodes[1]).channel == 1 && g.shared.at(nodes[2]).channel == 2, "a channel each");
        check(g.shared.at(nodes[0]).bus == 0 && g.shared.at(nodes[1]).bus == 1 && g.shared.at(nodes[2]).bus == 2, "the first keeps the main output; the others a bus each");
        check(difference(reference, play(s)) < 1e-6, "added, they sound as three instances");
        quiet(s);
        while(s.hasNode(nodes[2]) && s.undo()) {}
        block(s);
        check(s.graphState().shared.size() == 2, "the third add undone");
        while(!s.hasNode(nodes[2]) && s.redo()) {}
        while(s.parts().size() < 3 && s.redo()) {}
        block(s);
        check(s.hasNode(nodes[2]) && s.graphState().shared.size() == 3, "and redone");
        check(difference(reference, play(s)) < 1e-6, "redone, as before");

        // Undoing the add that made the first node its engine's gives it its own instance back.
        Session u(48000, frames);
        const auto alone = u.add("alone", synth);
        const auto own = u.addPart("Alone", PartInput{}); u.insertIntoPart(own, 0, alone); u.commit(); block(u);
        check(u.graphState().shared.empty(), "one Basic Synth alone keeps an instance of its own");
        const auto solo = play(u); quiet(u);
        const auto joined = u.add("joined", synth); u.commit(); block(u);
        check(u.graphState().shared.size() == 2, "the second add makes the first its engine's first node");
        while(u.hasNode(joined) && u.undo()) {}
        block(u);
        check(u.graphState().shared.empty(), "undone, the first has its own instance again");
        check(difference(solo, play(u)) < 1e-6, "and sounds as before");
    }

    // Removing the second part and its node, then undoing.
    {
        Session t(48000, frames); t.loadPerformance(shared.string()); block(t);
        const auto second = t.parts()[1];
        const auto node = second.chain.front();
        t.removePart(second.id); t.remove(node); t.commit(); block(t);
        check(!t.hasNode(node) && t.graphState().shared.size() == 2, "the second part and its node removed");
        check(difference(reference, play(t)) > 1e-3, "without it the output differs");
        quiet(t);
        while((!t.hasNode(node) || t.parts().size() < 3) && t.undo()) {}
        block(t);
        check(t.hasNode(node) && t.parts().size() == 3 && t.graphState().shared.size() == 3, "undone, it is back on the instance");
        check(difference(reference, play(t)) < 1e-6, "and sounds as before");
    }

    // A sharing part saved as a Sound of its own, which joins the instance where it is loaded.
    {
        Session s(48000, frames); s.loadPerformance(separate.string()); block(s);
        const auto saved = s.savePartAsSound(s.parts()[1].id, "Second");
        const auto sound = slurp(home.path / "Sounds" / "Second.osound" / "sound.json");
        check(!saved.id.empty() && count(sound, "\"sound\":null") == 1 && sound.find("\"engine\":\"node-1\"") != std::string::npos, "a sharing part saved as a Sound, its sound on its own");
        Session t(48000, frames); t.loadPerformance(separate.string()); block(t);
        const auto before = pluginInstancesLoaded();
        const auto part = t.partFromSound(saved.id); block(t);
        const auto node = t.parts().back().chain.front();
        check(t.parts().back().id == part && pluginInstancesLoaded() == before && t.graphState().shared.size() == 4 && engines(t) == 1, "loaded, it joins the playing instance");
        check(std::abs(t.parameter(node, 1) - .7) < 1e-9, "showing its own sound's values");
        Session u(48000, frames);
        u.partFromSound(saved.id); block(u);
        check(u.graphState().shared.empty(), "alone, it has an instance of its own");
    }

    // Performance changes between the layouts: the incoming sounds go into the playing instance.
    {
        Session s(48000, frames); s.loadPerformance(separate.string()); block(s);
        play(s); quiet(s);
        auto before = pluginInstancesLoaded();
        s.loadPerformance(again.string()); gone(s);
        check(pluginInstancesLoaded() == before && s.graphState().shared.size() == 3 && engines(s) == 1, "into the saved shared layout, in the playing instance");
        check(difference(reference, play(s)) < 1e-6, "sounding as it does opened fresh");
        quiet(s);
        before = pluginInstancesLoaded();
        s.loadPerformance(separate.string()); gone(s);
        check(pluginInstancesLoaded() == before && s.graphState().shared.size() == 3 && engines(s) == 1, "and to the separate layout, its parts joining the playing instance");
        check(difference(reference, play(s)) < 1e-6, "sounding as the separate layout opened fresh");
    }

    // Asked for: instruments.share spills past an instance's room, and the protocol lists the shares.
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
        const auto added = rpc.dispatch(R"({"jsonrpc":"2.0","id":4,"method":"graph.add","params":{"name":"bass","path":")" + std::string(synth) + R"("}})");
        s.commit();
        const auto listed = rpc.dispatch(R"({"jsonrpc":"2.0","id":5,"method":"graph.get"})");
        check(added && added->find("\"node\":") != std::string::npos && listed && count(*listed, "\"shared\":{\"engine\":" + std::to_string(first)) == 3, "graph.add of a Basic Synth joins the instance");
    }

    if(failures) { std::fprintf(stderr, "%d failures\n", failures); return 1; }
    std::puts("ok: parts sharing one engine");
    return 0;
}

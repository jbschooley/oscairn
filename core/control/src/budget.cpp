// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0025: resource budgets. Each plugin build's cost is measured on this device while it plays
// (its share of each block's time, its memory) and kept in the data folder, so a Performance can
// be checked against the device's budget before it loads.
#include "oscairn/session.h"
#include "oscairn/library.h"
#include "../../plugin-host/src/StateFiles.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>

namespace oscairn
{
    namespace
    {
        namespace fs = std::filesystem;
        // What a plugin never measured here counts as: small, so a desktop with many new plugins
        // is not refused, and marked, so the user knows it is a guess.
        constexpr double unmeasuredCpu = .02;
        constexpr uint64_t unmeasuredMemory = 32ull << 20;
        // The measured cost is a decaying high-water mark of the block times sampled at each
        // service tick: about 10 minutes' half-life at the desktop's 5 ms tick.
        constexpr double decay = 0.9999942;
        fs::path measurementsFile() { return dataHome() / "measurements.json"; }
        std::string percent(double share) { char text[32]; std::snprintf(text, sizeof(text), "%.0f%%", share * 100); return text; }
    }

    namespace
    {
        // What a plugin declared at its last activation (oscairn.memory); none where a worker is down.
        std::optional<oscairn_memory_t> declaredBy(const PluginHost& host)
        {
            try { return host.declaredMemory(); } catch(const std::exception&) { return {}; }
        }
        uint64_t total(const oscairn_memory_t& m) { return m.shared_bytes + m.context_bytes + m.asset_bytes; }
    }

    // A plugin build, and what it declared for its state: a plugin whose modes need very different
    // memory (a reverb's algorithms) is measured per declaration, not as one figure, the largest.
    std::string Session::pluginKey(const Graph& graph, NodeId node) const
    {
        const auto& host = graph.host(node);
        const auto& d = host.descriptor();
        const auto declared = declaredBy(host);
        return d.id + "@" + d.version + (declared ? "#" + std::to_string(total(*declared)) : std::string());
    }

    // Measured: at rest, plus its cost per voice for as many notes as its part may play at once;
    // with no limit, or before its voices are measured, the busiest block seen. A build asked for
    // without a declaration (a Performance not open) counts at the most of what its declarations
    // were measured at.
    Control::Cost Session::costOf(const std::string& key, const std::string& name, const std::string& plugin, uint32_t limit) const
    {
        Measurement m; bool found = false;
        if(const auto it = measured_.find(key); it != measured_.end()) { m = it->second; found = true; }
        else if(key.find('#') == std::string::npos)
            for(auto it = measured_.lower_bound(key + "#"); it != measured_.end() && it->first.compare(0, key.size() + 1, key + "#") == 0; ++it, found = true)
            {
                const auto& x = it->second;
                m = {std::max(m.cpu, x.cpu), std::max(m.rest, x.rest), std::max(m.voice, x.voice), std::max(m.voices, x.voices), std::max(m.memory, x.memory)};
            }
        if(!found) return {name, plugin, unmeasuredCpu, unmeasuredMemory, false};
        const double cpu = limit && m.voice > 0 ? m.rest + m.voice * limit : m.cpu;
        return {name, plugin, cpu, m.memory, true};
    }

    // Memory: the larger of the measurement and the declaration (RFC 0025: never the plugin's word
    // alone); a plugin not yet measured, its declaration instead of the default. An effect context
    // made from another counts its context bytes, from oscairn.memory or else effect contexts'
    // resources, since its engine's shared bytes are counted with the engine.
    Control::Cost Session::nodeCost(const Graph& graph, NodeId node, const std::string& name, uint32_t limit) const
    {
        const auto& host = graph.host(node);
        auto c = costOf(pluginKey(graph, node), name, host.descriptor().id, limit);
        const auto declared = declaredBy(host);
        std::optional<oscairn_effect_resources_t> resources;
        if(!declared) try { resources = host.effectResources(); } catch(const std::exception&) {}
        if(graph.effectSource(node) != node)
        {
            if(declared) c.memory = declared->context_bytes + declared->asset_bytes;
            else if(resources) c.memory = resources->context_bytes;
            return c;
        }
        const uint64_t own = declared ? total(*declared) : resources ? resources->shared_bytes + resources->context_bytes : 0;
        if(declared || resources) c.memory = c.measured ? std::max(c.memory, own) : own;
        return c;
    }

    uint64_t Session::memoryRoom(const Graph& graph, const std::vector<NodeId>& beside) const
    {
        if(budget_.cpu <= 0 || !budget_.memory) return 0;
        uint64_t used = 0;
        for(const auto n : beside) if(graph.contains(n)) used += nodeCost(graph, n).memory;
        return used < budget_.memory ? budget_.memory - used : 1; // 1 byte: nothing that declares fits
    }

    uint64_t Session::memoryRoom() const
    {
        std::vector<NodeId> open;
        for(const auto& [node, name] : nodes_) open.push_back(node);
        return memoryRoom(graph_, open);
    }

    uint32_t Session::noteLimit(NodeId node) const
    {
        for(const auto& p : parts_) if(nodesOf(p).count(node)) return p.input.maxNotes;
        return 0;
    }

    void Session::loadMeasurements()
    {
        if(measurementsLoaded_) return;
        measurementsLoaded_ = true;
        try
        {
            const auto bytes = state_files::read(measurementsFile(), 4 * 1024 * 1024);
            const std::unique_ptr<json_value_s, decltype(&std::free)> doc{json_parse(bytes.data(), bytes.size()), &std::free};
            const auto* plugins = detail::member(doc.get(), "plugins");
            if(!plugins || plugins->type != json_type_object) return;
            for(auto* e = static_cast<const json_object_s*>(plugins->payload)->start; e; e = e->next)
            {
                auto number = [&](const char* key) {
                    const auto* v = detail::member(e->value, key);
                    return v && v->type == json_type_number ? std::strtod(static_cast<const json_number_s*>(v->payload)->number, nullptr) : 0.0;
                };
                const double cpu = number("cpu"), rest = number("rest"), voice = number("voice"), voices = number("voices"), memory = number("memory");
                if(std::isfinite(cpu) && cpu >= 0 && std::isfinite(rest) && rest >= 0 && std::isfinite(voice) && voice >= 0 &&
                   std::isfinite(voices) && voices >= 0 && voices <= 4096 && std::isfinite(memory) && memory >= 0)
                    measured_[std::string(e->name->string, e->name->string_size)] = {cpu, rest, voice, uint32_t(voices), uint64_t(memory)};
            }
        }
        catch(const std::exception&) {} // none yet, or unreadable: measured afresh
    }

    void Session::saveMeasurements()
    {
        if(!measurementsChanged_) return;
        std::string text = "{\"plugins\":{";
        bool first = true;
        for(const auto& [key, m] : measured_)
        {
            char numbers[160]; std::snprintf(numbers, sizeof(numbers), "{\"cpu\":%.6g,\"rest\":%.6g,\"voice\":%.6g,\"voices\":%u,\"memory\":%llu}", m.cpu, m.rest, m.voice, m.voices, static_cast<unsigned long long>(m.memory));
            text += std::string(first ? "" : ",") + detail::quote(key) + ":" + numbers; first = false;
        }
        text += "}}\n";
        try
        {
            const auto file = measurementsFile();
            fs::create_directories(file.parent_path());
            const auto temporary = file.parent_path() / ".measurements.json.tmp";
            { std::ofstream out(temporary, std::ios::binary | std::ios::trunc); out << text; if(!out) return; }
            fs::rename(temporary, file);
            measurementsChanged_ = false;
        }
        catch(const std::exception& e) { std::fprintf(stderr, "oscairn: measurements not saved: %s\n", e.what()); }
        measurementsSaved_ = std::chrono::steady_clock::now();
    }

    void Session::setBudget(const Budget& budget)
    {
        if(!std::isfinite(budget.cpu) || budget.cpu < 0 || budget.cpu > 64) throw std::invalid_argument("a budget's cpu is a share of the audio thread's time, 0 to 64");
        budget_ = budget;
        if(budget_.cpu > 0) loadMeasurements();
    }

    // Each service tick, once a budget is set: every plugin's last block as a share of the block's
    // time, kept as a slowly falling high-water mark per plugin build (a plugin warming up, its
    // first 32 blocks, is not counted); its memory every second.
    // shortcut: one sample a tick misses blocks in between, so a rare spike may not count; time every block on the audio thread if admission must be exact.
    void Session::measure()
    {
        if(budget_.cpu <= 0) return;
        const auto frames = blockFrames_.load(std::memory_order_relaxed);
        if(!frames) return;
        const double blockMicroseconds = frames / sampleRate_ * 1e6;
        const bool memoryNow = ++measureTicks_ % 200 == 0;
        auto sample = [&](NodeId node) {
            const auto& host = graph_.host(node);
            if(!host.failure().empty()) return; // a failed plugin, or a stand-in for one not here, costs nothing to learn from
            const auto st = host.statistics();
            if(st.blocks < 32) return;
            auto& m = measured_[pluginKey(node)];
            const double was = m.cpu, wasRest = m.rest, wasVoice = m.voice;
            const double share = st.lastMicroseconds / blockMicroseconds;
            m.cpu = std::max(share, m.cpu * decay);
            // Split by the notes sounding through it in that block: at rest, and added per note.
            if(const auto voices = graph_.sounding(node); voices == 0) m.rest = std::max(share, m.rest * decay);
            else if(voices != UINT32_MAX)
            {
                m.voice = std::max(std::max(0.0, share - m.rest) / voices, m.voice * decay);
                m.voices = std::max(m.voices, voices);
            }
            if(std::abs(m.rest - wasRest) > 1e-4 || std::abs(m.voice - wasVoice) > 1e-5) measurementsChanged_ = true;
            if(memoryNow)
            {
                auto bytes = host.residentBytes();
                if(!bytes) if(const auto it = loadedBytes_.find(node); it != loadedBytes_.end()) bytes = it->second;
                if(bytes && bytes != m.memory) { m.memory = bytes; measurementsChanged_ = true; }
            }
            if(std::abs(m.cpu - was) > 1e-4) measurementsChanged_ = true;
        };
        for(const auto& [node, name] : nodes_) sample(node);
        if(measurementsChanged_ && std::chrono::steady_clock::now() - measurementsSaved_ > std::chrono::seconds(30)) saveMeasurements();
    }

    Control::Estimate Session::estimateOf(const std::vector<Cost>& plugins, const std::vector<Cost>& playing) const
    {
        Estimate e;
        e.plugins = plugins; e.budget = budget_;
        for(const auto& c : plugins) { e.cpu += c.cpu; e.memory += c.memory; }
        if(budget_.cpu <= 0) return e;
        double cpu = e.cpu; uint64_t memory = e.memory;
        for(const auto& c : playing) { cpu += c.cpu; memory += c.memory; }
        e.fits = e.cpu <= budget_.cpu && (!budget_.memory || e.memory <= budget_.memory);
        e.seamless = e.fits && cpu <= budget_.cpu && (!budget_.memory || memory <= budget_.memory);
        std::sort(e.plugins.begin(), e.plugins.end(), [](const Cost& a, const Cost& b) { return a.cpu > b.cpu; });
        return e;
    }

    std::string Session::tooMuch(const Estimate& e) const
    {
        std::string why = "needs " + percent(e.cpu) + " of the audio time where this device allows " + percent(budget_.cpu);
        if(budget_.memory && e.memory > budget_.memory) why += ", and " + std::to_string(e.memory >> 20) + " MiB of memory where it allows " + std::to_string(budget_.memory >> 20);
        std::string most;
        for(size_t i = 0; i < e.plugins.size() && i < 3; ++i)
            most += std::string(i ? ", " : "") + (e.plugins[i].name.empty() ? e.plugins[i].plugin : e.plugins[i].name) + " " + percent(e.plugins[i].cpu) + (e.plugins[i].measured ? "" : " (not yet measured)");
        return why + (most.empty() ? "" : "; most: " + most);
    }

    Control::Estimate Session::estimate(const std::string& directory)
    {
        if(budget_.cpu > 0) loadMeasurements();
        // What plays now: this Performance's plugins, and any still ringing out from the last.
        std::vector<Cost> open, ringing;
        for(const auto& [node, name] : nodes_) open.push_back(nodeCost(graph_, node, name, noteLimit(node)));
        for(const auto node : leaving_) if(graph_.contains(node)) ringing.push_back(nodeCost(graph_, node));
        // The open one fits on its own; what rings out from the last only makes it not seamless,
        // so adding a plugin right after a change is not refused for a tail.
        if(directory.empty()) return estimateOf(open, ringing);
        auto playing = open; playing.insert(playing.end(), ringing.begin(), ringing.end());
        // A Performance in a folder: its plugins by identity, from its document.
        const auto bytes = state_files::read(fs::path(directory) / "performance.json", 4 * 1024 * 1024);
        const std::unique_ptr<json_value_s, decltype(&std::free)> doc{json_parse(bytes.data(), bytes.size()), &std::free};
        const auto* nodes = detail::member(doc.get(), "nodes");
        if(!nodes || nodes->type != json_type_array) throw std::runtime_error("not a Performance: " + directory);
        auto text = [](const json_value_s* v) { return v && v->type == json_type_string ? std::string(static_cast<const json_string_s*>(v->payload)->string, static_cast<const json_string_s*>(v->payload)->string_size) : std::string(); };
        // Each node's part's note limit, by its key: in the part's chain or a split's lanes.
        std::map<std::string, uint32_t> limits;
        if(const auto* parts = detail::member(doc.get(), "parts"); parts && parts->type == json_type_array)
            for(auto* p = static_cast<const json_array_s*>(parts->payload)->start; p; p = p->next)
            {
                const auto* limit = detail::member(detail::member(p->value, "input"), "maxNotes");
                const uint32_t n = limit && limit->type == json_type_number ? uint32_t(std::strtoul(static_cast<const json_number_s*>(limit->payload)->number, nullptr, 10)) : 0;
                auto chain = [&](const json_value_s* c) {
                    if(c && c->type == json_type_array) for(auto* k = static_cast<const json_array_s*>(c->payload)->start; k; k = k->next) limits[text(k->value)] = n;
                };
                chain(detail::member(p->value, "chain"));
                if(const auto* splits = detail::member(p->value, "splits"); splits && splits->type == json_type_array)
                    for(auto* s = static_cast<const json_array_s*>(splits->payload)->start; s; s = s->next)
                        if(const auto* lanes = detail::member(s->value, "lanes"); lanes && lanes->type == json_type_array)
                            for(auto* l = static_cast<const json_array_s*>(lanes->payload)->start; l; l = l->next) chain(detail::member(l->value, "chain"));
            }
        std::vector<Cost> coming;
        for(auto* e = static_cast<const json_array_s*>(nodes->payload)->start; e; e = e->next)
        {
            const auto* plugin = detail::member(e->value, "plugin");
            const auto id = text(detail::member(plugin, "id")), key = text(detail::member(e->value, "key"));
            coming.push_back(costOf(id + "@" + text(detail::member(plugin, "version")), text(detail::member(e->value, "name")), id, limits.count(key) ? limits.at(key) : 0));
        }
        return estimateOf(coming, playing);
    }
}

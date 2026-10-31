// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PartsModel.h"
#include "oscairn/pads.h"
#include <algorithm>
#include <filesystem>
#include "oscairn/discovery.h"

namespace oscairn::standalone
{
    namespace
    {
        const Graph::NodeInfo* nodeInfo(const Control::GraphState& g, NodeId id)
        {
            for(const auto& n : g.topology.nodes) if(n.id == id) return &n;
            return nullptr;
        }
        std::string range(int low, int high) { return noteName(low) + "–" + noteName(high); }
    }

    std::string roleOf(const Graph::NodeInfo& n)
    {
        size_t in = 0, out = 0;
        for(const auto& p : n.inputs) in += p.channels;
        for(const auto& p : n.outputs) out += p.channels;
        if(n.noteInputs && n.noteOutputs && !out) return "Notes";
        if(n.noteOutputs && !out) return n.noteInputs ? "Notes" : "Sequencer";
        if(out && !in) return n.noteInputs ? "Instrument" : "Generator";
        if(in && out) return "Effect";
        if(in) return "Analyzer";
        return "Notes";
    }

    PartsState readParts(const Control::GraphState& g)
    {
        PartsState state;
        std::map<uint32_t, std::vector<std::string>> feeders; // bus -> parts sending to it
        for(const auto& p : g.parts)
        {
            if(p.bus) feeders[p.bus].push_back(p.name);
            for(const auto& s : p.sends) feeders[s.bus].push_back(p.name);
        }
        for(const auto& p : g.parts)
        {
            PartRow row;
            row.id = p.id; row.name = p.name; row.bus = p.input.kind == PartInput::Kind::Bus;
            row.level = p.level; row.mute = p.mute; row.through = p.bus;
            for(const auto& s : p.sends) row.sends[s.bus] = s.level;
            if(!p.input.zones.empty()) { const auto& z = p.input.zones.front(); row.lowKey = z.lowKey; row.highKey = z.highKey; row.transpose = z.transpose; }
            for(const auto node : p.chain)
                if(const auto* n = nodeInfo(g, node))
                {
                    row.chain.push_back({node, g.names.count(node) ? g.names.at(node) : std::string(), n->plugin.name, roleOf(*n)});
                    if(row.sound.empty() && row.chain.back().role == "Instrument") row.sound = row.chain.back().name;
                }
            switch(p.input.kind)
            {
            case PartInput::Kind::Notes:
                row.input = "Keyboard · " + (row.lowKey == 0 && row.highKey == 127 ? std::string("all keys") : range(row.lowKey, row.highKey));
                if(row.transpose) row.input += " · " + std::string(row.transpose > 0 ? "+" : "−") + std::to_string(std::abs(row.transpose));
                break;
            case PartInput::Kind::Audio: row.input = "Audio in " + std::to_string(p.input.port + 1); break;
            case PartInput::Kind::None: row.input = "Its own notes"; break;
            case PartInput::Kind::Bus:
            {
                std::string from;
                for(const auto& f : feeders[p.id]) from += (from.empty() ? "" : ", ") + f;
                row.input = from.empty() ? "Nothing sends to it yet" : "From " + from;
                break;
            }
            }
            (row.bus ? state.buses : state.parts).push_back(std::move(row));
        }
        return state;
    }

    std::vector<PluginChoice> bundledPlugins(const std::string& folder, const std::string& helper)
    {
        std::vector<PluginChoice> result;
        for(const auto& d : scanPlugins({folder}, helper).plugins)
        {
            // "Oscairn Basic Synth" from Oscairn is shown as "Basic Synth".
            auto name = d.name;
            if(!d.vendor.empty() && name.size() > d.vendor.size() + 1 && name.compare(0, d.vendor.size() + 1, d.vendor + " ") == 0) name = name.substr(d.vendor.size() + 1);
            result.push_back({name, d.path, d.id});
        }
        std::sort(result.begin(), result.end(), [](const PluginChoice& a, const PluginChoice& b) { return a.name < b.name; });
        return result;
    }

    uint32_t addPart(Control& c, const std::string& name, bool bus)
    {
        const auto id = c.addPart(name, bus ? PartInput{PartInput::Kind::Bus} : PartInput{});
        c.commit();
        return id;
    }

    void insertPlugin(Control& c, uint32_t part, size_t index, const PluginChoice& plugin)
    {
        const auto names = c.graphState().names;
        std::string name;
        for(int n = 1;; ++n)
        {
            name = plugin.name + (n > 1 ? " " + std::to_string(n) : std::string());
            if(std::none_of(names.begin(), names.end(), [&](const auto& e) { return e.second == name; })) break;
        }
        const auto node = c.add(name, plugin.path, plugin.id);
        try { c.insertIntoPart(part, index, node); }
        catch(...) { c.remove(node); throw; }
        c.commit();
    }

    void removeBox(Control& c, uint32_t part, NodeId node)
    {
        c.removeFromPart(part, node);
        c.remove(node, UserWires::Reconnect);
        c.commit();
    }

    void setZone(Control& c, uint32_t part, int lowKey, int highKey, int transpose)
    {
        const auto parts = c.graphState().parts;
        const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& p) { return p.id == part; });
        if(found == parts.end()) throw std::invalid_argument("unknown part");
        auto input = found->input;
        NoteZone zone = input.zones.empty() ? NoteZone{} : input.zones.front();
        zone.lowKey = uint8_t(std::clamp(lowKey, 0, 127)); zone.highKey = uint8_t(std::clamp(highKey, int(zone.lowKey), 127));
        zone.transpose = int8_t(std::clamp(transpose, -48, 48));
        const bool everything = zone == NoteZone{};
        if(input.zones.empty()) { if(!everything) input.zones.push_back(zone); }
        else if(everything && input.zones.size() == 1) input.zones.clear();
        else input.zones.front() = zone;
        c.setPartInput(part, input);
        c.commit();
    }

    void setOutput(Control& c, uint32_t part, uint32_t bus) { c.setPartOutput(part, bus); c.commit(); }

    void setSendLevel(Control& c, uint32_t part, uint32_t bus, double level)
    {
        c.setSend(part, bus, level > 0 ? std::optional<double>(level) : std::nullopt);
        c.commit();
    }
}

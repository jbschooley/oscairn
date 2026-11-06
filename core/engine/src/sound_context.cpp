// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/sound_context.h"
#include "../../plugin-host/src/StateFiles.h"

#include <oscairn-clap/ui.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace oscairn
{
    clap_id SoundContext::busParameter(const PluginHost& host)
    {
        if(std::string(host.soundContextMode()) != "native" || !host.supportsState() || !host.noteInputPorts() || host.noteOutputPorts() ||
           !host.inputPorts().empty() || host.outputPorts().size() < 2)
            return CLAP_INVALID_ID;
        for(const auto& p : host.parameters())
            if(p.role == OSCAIRN_ROLE_OUTPUT_BUS && (p.flags & CLAP_PARAM_IS_STEPPED) && !(p.flags & CLAP_PARAM_IS_READONLY) && p.minimum == 0 && p.maximum >= 1)
                return p.id;
        return CLAP_INVALID_ID;
    }

    uint32_t SoundContext::room(const Engine& e)
    {
        const auto& h = *e.host;
        const auto bus = std::find_if(h.parameters().begin(), h.parameters().end(), [&](const auto& p) { return p.id == e.busParameter; });
        const auto ports = bus == h.parameters().end() ? 0u : std::min<uint32_t>(uint32_t(h.outputPorts().size()), uint32_t(bus->maximum) + 1);
        const auto buses = uint32_t(std::popcount(e.buses)), channels = uint32_t(std::popcount(e.channels));
        const auto sounds = uint32_t(h.soundContexts().size()), capacity = h.soundContextCapacity();
        return std::min({ports > buses ? ports - buses : 0u, 16 - channels, capacity > sounds ? capacity - sounds : 0u});
    }

    SoundContext::SoundContext(std::shared_ptr<Engine> engine, uint16_t channel, uint32_t bus, std::optional<uint32_t> sound)
        : engine_(std::move(engine)), channel_(channel), bus_(bus)
    {
        const auto& h = *engine_->host;
        if(channel > 15 || bus >= h.outputPorts().size() || bus > 31 || (engine_->channels >> channel & 1) || (engine_->buses >> bus & 1))
            throw std::invalid_argument("a shared engine's channel or output bus is taken or missing");
        port_ = {h.outputPorts()[bus]};
        port_.front().flags |= CLAP_AUDIO_PORT_IS_MAIN;
        for(const auto& p : h.parameters()) if(!(p.flags & CLAP_PARAM_IS_READONLY)) values_[p.id] = h.parameterValue(p.id);
        values_[engine_->busParameter] = bus;
        engine_->channels |= 1u << channel; engine_->buses |= 1u << bus;
        if(sound) sound_.store(*sound, std::memory_order_relaxed); else service();
        if((sound && !engine_->host->selectSound(*sound, int16_t(channel))) || !queue(engine_->busParameter, bus))
        {
            engine_->channels &= ~(1u << channel); engine_->buses &= ~(1u << bus);
            throw std::runtime_error("the shared engine's sound could not be selected");
        }
    }

    SoundContext::~SoundContext() { engine_->channels &= ~(1u << channel_); engine_->buses &= ~(1u << bus_); }

    void SoundContext::service()
    {
        engine_->host->service();
        for(const auto& c : engine_->host->soundContexts()) if(c.selected_channels >> channel_ & 1) sound_.store(c.id, std::memory_order_relaxed);
    }

    void SoundContext::setValues(const std::map<clap_id, double>& values)
    {
        for(const auto& [id, value] : values) if(values_.count(id) && id != engine_->busParameter) values_[id] = value;
    }

    uint32_t SoundContext::prepareSound(const std::string& name, const std::vector<oscairn_sound_parameter_t>& parameters, const std::string& state, const std::string& preset,
                                        const std::string& sfz, const std::string& library, const std::string& directory)
    {
        // A sound prepared here plays on this node's bus, whatever it asks.
        auto own = parameters;
        std::erase_if(own, [&](const auto& p) { return p.id == engine_->busParameter; });
        if(preset.empty() && state.empty()) own.push_back({engine_->busParameter, double(bus_)});
        return engine_->host->prepareSound(name, own, state, preset, sfz, library, directory);
    }

    bool SoundContext::selectSound(uint32_t id, int16_t)
    {
        if(!engine_->host->selectSound(id, int16_t(channel_))) return false;
        sound_.store(id, std::memory_order_relaxed);
        queue(engine_->busParameter, bus_); // a preset's sound keeps its own bus otherwise
        return true;
    }

    double SoundContext::parameterValue(clap_id id) const noexcept
    {
        const auto it = values_.find(id);
        return it == values_.end() ? engine_->host->parameterValue(id) : it->second;
    }

    bool SoundContext::setParameter(clap_id id, double value) noexcept
    {
        const auto it = values_.find(id);
        if(it == values_.end() || !std::isfinite(value)) return false;
        if(id == engine_->busParameter) return value == bus_; // the bus is the node's place in the engine
        if(!queue(id, value)) return false;
        it->second = value;
        return true;
    }

    bool SoundContext::queue(clap_id id, double value) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);
        if(head - tail_.load(std::memory_order_acquire) == queue_.size()) return false;
        clap_event_param_value_t v{};
        v.header = {sizeof(v), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        v.param_id = id; v.note_id = -1; v.port_index = -1; v.key = -1; v.channel = int16_t(channel_); v.value = value;
        queue_[head % queue_.size()] = kick::clap::Event::from(v);
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    void SoundContext::saveSnapshot(const std::string& directory)
    {
        state_files::Bundle bundle(directory);
        std::ostringstream out; out.imbue(std::locale::classic()); out << std::setprecision(17) << "parameters 1\n";
        for(const auto& [id, value] : values_) out << id << ' ' << value << '\n';
        const auto text = out.str();
        state_files::write(bundle.temporary.path / "snapshot", text.data(), text.size());
        bundle.publish();
    }

    void SoundContext::loadSnapshot(const std::string& directory)
    {
        const auto data = state_files::read(std::filesystem::path(directory) / "snapshot", 2 * 1024 * 1024);
        std::istringstream in(std::string(data.begin(), data.end())); in.imbue(std::locale::classic());
        std::string kind; unsigned version;
        if(!(in >> kind >> version) || kind != "parameters" || version != 1) throw std::runtime_error("Invalid shared sound snapshot");
        std::map<clap_id, double> values; clap_id id; double value;
        while(in >> std::ws && !in.eof())
        {
            if(!(in >> id >> value) || !std::isfinite(value) || !values_.count(id) || !values.emplace(id, value).second) throw std::runtime_error("Shared sound snapshot does not match the plugin");
        }
        setValues(values);
    }
}

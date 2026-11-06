// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/plugin_host.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>

namespace oscairn
{
    /// RFC 0021 slice 3: parts sharing one engine. A node playing one sound of a plugin instance
    /// several nodes share: one with native sound contexts and per-sound output buses (a parameter
    /// with oscairn.ui's OSCAIRN_ROLE_OUTPUT_BUS). Each node is its own part's instrument, with its
    /// own ID, parameters, meter and wires; the graph renders the instance once a block. Notes
    /// reach it on the node's MIDI channel, which selects the node's sound, and the node's one
    /// output port is the instance's output bus its sound plays on.
    ///
    /// Parameters are the node's sound's: setting one queues it for the instance addressed to the
    /// node's channel (the graph hands it over at the next block), and the node keeps the values it
    /// set, since the contract reads no context's values. Its sound's own output bus is fixed.
    /// The instance's state is the engine's (saveEngine); the node's own snapshot is its values.
    /// Control thread, except where said.
    class SoundContext final : public PluginHost
    {
    public:
        /// What several contexts share: the instance and the channels and buses taken.
        struct Engine
        {
            std::shared_ptr<PluginHost> host;
            clap_id busParameter = CLAP_INVALID_ID;
            uint32_t channels = 0, buses = 0; // bit masks: a channel, an output port index, taken
            std::shared_ptr<char> group = std::make_shared<char>(); // the graph's effect group: one engine
        };
        /// The parameter choosing a sound's output bus, or CLAP_INVALID_ID where the plugin cannot
        /// share: native sound contexts, a snapshot of them, notes in, no notes out, no audio in and
        /// more than one output port.
        static clap_id busParameter(const PluginHost& host);
        /// How many more contexts the engine takes: its free channels and buses, and its free sounds.
        static uint32_t room(const Engine& engine);
        /// A context on `channel` and output port index `bus`, both free, selecting `sound` there, or
        /// with none keeping what the channel selects (a node that played alone, whose selection may
        /// still be queued). Its sound is put on the bus by a parameter event on its channel.
        SoundContext(std::shared_ptr<Engine> engine, uint16_t channel, uint32_t bus, std::optional<uint32_t> sound);
        ~SoundContext() override;
        SoundContext(const SoundContext&) = delete;
        SoundContext& operator=(const SoundContext&) = delete;

        const std::shared_ptr<Engine>& engine() const noexcept { return engine_; }
        uint16_t channel() const noexcept { return channel_; }
        uint32_t bus() const noexcept { return bus_; } // the output port's index on the instance
        // The sound its channel selects, as the instance last said (each service()); any thread.
        uint32_t sound() const noexcept { return sound_.load(std::memory_order_relaxed); }
        /// Values to show before any is set (a context made from a node playing alone, or read
        /// back from its saved values); not sent to the instance, which has them already.
        void setValues(const std::map<clap_id, double>& values);

        /// Audio thread: the parameter events queued since the last call, addressed to the channel.
        template<class Emit> void takeParameters(Emit&& emit) noexcept
        {
            for(auto tail = tail_.load(std::memory_order_relaxed), head = head_.load(std::memory_order_acquire); tail != head; ++tail)
            {
                emit(queue_[tail % queue_.size()]);
                tail_.store(tail + 1, std::memory_order_release);
            }
        }

        void service() override;
        void saveSnapshot(const std::string& directory) override; // the node's values, as a parameter snapshot
        void loadSnapshot(const std::string& directory) override; // read back into the values shown
        std::optional<oscairn_memory_t> declaredMemory() const override { return engine_->host->declaredMemory(); }
        void setMemoryRoom(uint64_t bytes) override { engine_->host->setMemoryRoom(bytes); }
        const char* soundContextMode() const noexcept override { return engine_->host->soundContextMode(); }
        uint32_t soundContextCapacity() const noexcept override { return engine_->host->soundContextCapacity(); }
        std::vector<oscairn_sound_context_info_t> soundContexts() const override { return engine_->host->soundContexts(); }
        uint32_t prepareSound(const std::string& name, const std::vector<oscairn_sound_parameter_t>& parameters, const std::string& state = {}, const std::string& preset = {},
                              const std::string& sfz = {}, const std::string& library = {}, const std::string& directory = {}) override;
        bool selectSound(uint32_t id, int16_t channel = -1) override; // on the node's channel, whatever `channel` says
        bool discardSound(uint32_t id, bool cutTail = false) override { return engine_->host->discardSound(id, cutTail); }
        bool voiceBudget() const noexcept override { return engine_->host->voiceBudget(); }
        bool honorsVolume() const noexcept override { return engine_->host->honorsVolume(); }
        void setAssets(kick::clap::AssetResolver* assets) noexcept override { engine_->host->setAssets(assets); }
        std::vector<kick::clap::AssetReference> assetDependencies() const override { return engine_->host->assetDependencies(); }
        std::vector<PresetInfo> presets() override { return engine_->host->presets(); }
        const kick::clap::Descriptor& descriptor() const noexcept override { return engine_->host->descriptor(); }
        const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return engine_->host->parameters(); }
        const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return none_; }
        const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return port_; }
        uint32_t noteInputPorts() const noexcept override { return engine_->host->noteInputPorts(); }
        bool usesClapNotes(uint32_t port) const noexcept override { return engine_->host->usesClapNotes(port); }
        void panic() noexcept override { engine_->host->panic(); }
        uint32_t noteOutputPorts() const noexcept override { return 0; }
        double parameterValue(clap_id id) const noexcept override;
        uint64_t parameterRevision() const noexcept override { return engine_->host->parameterRevision(); }
        std::string parameterText(clap_id id, double value) const override { return engine_->host->parameterText(id, value); }
        bool setParameter(clap_id id, double value) noexcept override;
        bool parameterGesture(clap_id id, bool begin) noexcept override { return engine_->host->parameterGesture(id, begin); }
        bool popOutput(kick::clap::Event&) noexcept override { return false; }
        uint64_t outputEventDrops() const noexcept override { return 0; }
        const kick::clap::Event* outputEvents(uint32_t& count) const noexcept override { count = 0; return nullptr; }
        // The instance's figures once, on the context with the lowest channel: summed per node, they would count it again.
        kick::clap::Statistics statistics() const noexcept override { return lead() ? engine_->host->statistics() : kick::clap::Statistics{}; }
        std::string failure() const override { return engine_->host->failure(); }
        bool process(const float* const*, uint32_t, float* const*, uint32_t, uint32_t, const kick::clap::Event*, uint32_t, const clap_event_transport_t*) noexcept override { return false; } // the graph renders the instance
        uint64_t residentBytes() const override { return lead() ? engine_->host->residentBytes() : 0; }
        double cpuSeconds() const override { return lead() ? engine_->host->cpuSeconds() : 0; }
        bool inWorker() const noexcept override { return engine_->host->inWorker(); }

    private:
        bool lead() const noexcept { return !(engine_->channels & ((1u << channel_) - 1)); }
        std::shared_ptr<Engine> engine_;
        uint16_t channel_ = 0;
        uint32_t bus_ = 0;
        std::atomic<uint32_t> sound_{0};
        std::vector<kick::clap::Port> port_, none_;
        std::map<clap_id, double> values_;
        std::array<kick::clap::Event, 256> queue_{}; // control thread to audio thread
        std::atomic<uint32_t> head_{0}, tail_{0};
        bool queue(clap_id id, double value) noexcept;
    };
}

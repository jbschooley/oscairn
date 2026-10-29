// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <kick_clap/Host.h>

namespace oscairn
{
    // Selected before instantiation; Offline permits synchronous non-RT work.
    enum class ProcessingMode { Realtime, Offline };

    // Shared graph host contract. Events and metadata retain the existing CLAP-shaped
    // representation; format adapters translate at their boundary. Control methods
    // belong to one main thread; process/outputEvents belong to one audio thread.
    // Destruction happens only after the graph retires all audio plans using the host.
    class PluginHost
    {
    public:
        virtual ~PluginHost() = default;
        virtual void service() = 0;
        virtual const kick::clap::Descriptor& descriptor() const noexcept = 0;
        virtual const std::vector<kick::clap::Parameter>& parameters() const noexcept = 0;
        virtual const std::vector<kick::clap::Port>& inputPorts() const noexcept = 0;
        virtual const std::vector<kick::clap::Port>& outputPorts() const noexcept = 0;
        virtual uint32_t noteInputPorts() const noexcept = 0;
        virtual uint32_t noteOutputPorts() const noexcept = 0;
        virtual double parameterValue(clap_id id) const noexcept = 0;
        virtual std::string parameterText(clap_id id, double value) const = 0;
        virtual bool setParameter(clap_id id, double value) noexcept = 0;
        virtual bool parameterGesture(clap_id id, bool begin) noexcept = 0;
        virtual bool popOutput(kick::clap::Event& event) noexcept = 0;
        virtual uint64_t outputEventDrops() const noexcept = 0;
        virtual const kick::clap::Event* outputEvents(uint32_t& count) const noexcept = 0;
        virtual kick::clap::Statistics statistics() const noexcept = 0;
        virtual bool process(const float* const* inputs, uint32_t ni, float* const* outputs, uint32_t no, uint32_t frames, const kick::clap::Event* events, uint32_t count, const clap_event_transport_t* transport) noexcept = 0;
    };
    // CLAP module or LV2 bundle directory; pluginId is a CLAP ID or an LV2 URI.
    std::shared_ptr<PluginHost> loadPlugin(const std::string& path, const std::string& pluginId,
                                           double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime);
}

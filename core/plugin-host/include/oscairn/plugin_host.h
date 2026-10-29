// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <kick_clap/Host.h>
#include <optional>
#include <stdexcept>
#include <variant>

namespace oscairn
{
    // Selected before instantiation; Offline permits synchronous non-RT work.
    enum class ProcessingMode { Realtime, Offline };

    // Per LV2 instance, chosen before loading; no allocation/resizing in process().
    // Limits count aligned bytes including Atom headers, across input/output ports.
    struct Lv2BufferLimits
    {
        uint32_t defaultBytes = 16 * 1024;
        uint32_t maxPortBytes = 1024 * 1024;
        uint32_t maxTotalBytes = 4 * 1024 * 1024;
    };

    struct FilePath { std::string value; bool operator==(const FilePath&) const = default; };
    using PropertyValue = std::variant<bool, int32_t, int64_t, float, double, std::string, FilePath>;
    enum class PropertyType { Unknown, Bool, Int, Long, Float, Double, String, Path };
    struct PropertyInfo { std::string uri, name; PropertyType type=PropertyType::Unknown; bool readable=false, writable=false; };
    struct PresetInfo { std::string uri, name; };

    // Shared graph host contract. Events and metadata retain the existing CLAP-shaped
    // representation; format adapters translate at their boundary. Control methods
    // belong to one main thread; process/outputEvents belong to one audio thread.
    // Destruction happens only after the graph retires all audio plans using the host.
    class PluginHost
    {
    public:
        virtual ~PluginHost() = default;
        virtual void service() = 0;
        // Control thread. Save to a NEW directory; restore from a saved directory.
        // May silence/reset this instance while excluding rendering/Worker calls.
        // Prepare a spare instance for transitions; this is not seamless switching.
        virtual uint32_t soundContextCapacity() const noexcept { return 0; }
        virtual std::vector<oscairn_sound_context_info_t> soundContexts() const { return {}; }
        virtual uint32_t prepareSound(const std::string&,const std::vector<oscairn_sound_parameter_t>&) { throw std::invalid_argument("Plugin has no native sound contexts"); }
        virtual bool selectSound(uint32_t,int16_t=-1) { throw std::invalid_argument("Plugin has no native sound contexts"); }
        virtual bool discardSound(uint32_t) { throw std::invalid_argument("Plugin has no native sound contexts"); }
        virtual bool supportsState() const noexcept { return false; }
        virtual void saveState(const std::string&) { throw std::runtime_error("Plugin state saving is unsupported"); }
        virtual void loadState(const std::string&) { throw std::runtime_error("Plugin state restoration is unsupported"); }
        virtual std::vector<PresetInfo> presets() { return {}; }
        virtual void loadPreset(const std::string&) { throw std::runtime_error("Plugin presets are unsupported"); }
        virtual std::vector<PropertyInfo> properties() const { return {}; }
        // Acceptance queues a next-block request, not confirmation of plugin success.
        virtual bool setProperty(const std::string&,const PropertyValue&) { return false; }
        virtual bool requestProperty(const std::string&) { return false; }
        // Last plugin-confirmed value, updated by service(); absent until reported.
        virtual std::optional<PropertyValue> propertyValue(const std::string&) const { return {}; }
        virtual const kick::clap::Descriptor& descriptor() const noexcept = 0;
        virtual const std::vector<kick::clap::Parameter>& parameters() const noexcept = 0;
        virtual const std::vector<kick::clap::Port>& inputPorts() const noexcept = 0;
        virtual const std::vector<kick::clap::Port>& outputPorts() const noexcept = 0;
        // Control thread: port IDs forming the unambiguous main audio bus.
        // Format adapters resolve their own bus metadata; ambiguous layouts require explicit routes.
        virtual std::vector<uint32_t> mainAudioPorts(bool input) const;
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
                                           double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime, Lv2BufferLimits lv2Buffers = {});
}

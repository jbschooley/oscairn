// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/local_socket.h"

namespace oscairn
{
    /// Cached Control client. Construct after connecting LocalClient; service regularly
    /// on the same UI/control thread. Reads use snapshots except plugin text formatting.
    /// Mutations use bounded RPC calls and never retry implicitly after a disconnect.
    class RemoteControl final : public Control
    {
    public:
        explicit RemoteControl(LocalClient& client);
        ~RemoteControl() override;
        void service();
        void savePerformance(const std::string& directory) override;
        void loadPerformance(const std::string& directory) override;
        bool supportsState(NodeId node) const override;
        void saveState(NodeId node,const std::string& directory) override;
        std::vector<PresetInfo> presets(NodeId node) override;
        std::vector<PropertyInfo> properties(NodeId node) const override;
        bool setProperty(NodeId node,const std::string& uri,const PropertyValue& value) override;
        bool requestProperty(NodeId node,const std::string& uri) override;
        std::optional<PropertyValue> propertyValue(NodeId node,const std::string& uri) const override;
        Sounds sounds(NodeId node) const override;
        uint32_t prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& state={},const std::string& preset={},const std::string& sfz={},const std::string& library={},const std::string& directory={}) override;
        AssetFolder registerAssetFolder(const std::string& folder) override;
        kick::clap::AssetReference importAsset(const std::string& file) override;
        std::vector<AssetLibrary> assetLibraries() const override;
        bool selectSound(NodeId node,uint32_t id,int16_t channel=-1) override;
        bool discardSound(NodeId node,uint32_t id,bool cutTail=false) override;
        GraphState graphState() const override;
        SessionStatus sessionStatus() const override;
        NodeId add(const std::string& name,const std::string& path,const std::string& pluginId = {}) override;
        NodeId addEffectContext(const std::string& name,NodeId source) override;
        std::optional<oscairn_effect_resources_t> effectResources(NodeId node) const override;
        void replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset = {}) override;
        void remove(NodeId node) override;
        void connectAudio(NodeId from,NodeId to) override;
        void connectAudio(const AudioConnection&) override;
        void disconnectAudio(const AudioConnection&) override;
        void connectNotes(NodeId from,NodeId to) override;
        void disconnectAudio(NodeId from,NodeId to) override;
        void disconnectNotes(NodeId from,NodeId to) override;
        uint32_t addPart(const std::string& name,PartInput input = {},std::optional<uint32_t> output = {}) override;
        void removePart(uint32_t part) override;
        void insertIntoPart(uint32_t part,size_t index,NodeId node) override;
        void removeFromPart(uint32_t part,NodeId node) override;
        void movePartNode(uint32_t part,size_t from,size_t to) override;
        void setPartInput(uint32_t part,const PartInput& input) override;
        void setPartOutput(uint32_t part,uint32_t bus,std::optional<uint32_t> port = {}) override;
        void setSend(uint32_t part,uint32_t bus,std::optional<double> level) override;
        void setPartLevel(uint32_t part,double level,bool mute) override;
        void setPartName(uint32_t part,const std::string& name) override;
        uint32_t connectPorts(const Wire& wire) override;
        void disconnectWire(uint32_t wire) override;
        void setMix(uint32_t wire,double level,double pan,bool mute,bool solo) override;
        void setZones(uint32_t wire,const std::vector<NoteZone>& zones) override;
        uint32_t addScene(const std::string& name) override;
        void storeScene(uint32_t scene) override;
        void selectScene(uint32_t scene) override;
        void removeScene(uint32_t scene) override;
        void setSceneName(uint32_t scene,const std::string& name) override;
        void setSceneParameters(const std::vector<SceneParameter>& parameters) override;
        uint32_t addMacro(const std::string& name) override;
        void removeMacro(uint32_t macro) override;
        void setMacroName(uint32_t macro,const std::string& name) override;
        void setMacroTargets(uint32_t macro,const std::vector<MacroTarget>& targets) override;
        void setMacro(uint32_t macro,double value) override;
        void remove(NodeId node,UserWires userWires) override;
        void mapParameter(NodeId from,uint32_t lane,NodeId to,uint32_t param) override;
        void unmapParameter(NodeId from,uint32_t lane) override;
        void commit() override;
        uint64_t changeSequence() const override;
        Changes changesAfter(uint64_t sequence) const override;
        NodeId nodeId(const std::string& name) const override;
        bool hasNode(NodeId node) const override;
        bool hasParameter(NodeId node,uint32_t param) const override;
        std::vector<ParamInfo> parameters(NodeId node) const override;
        ParamInfo parameterInfo(NodeId node,uint32_t param) const override;
        double parameter(NodeId node,uint32_t param) const override;
        std::string parameterText(NodeId node,uint32_t param,double value) const override;
        bool setParameter(NodeId node,uint32_t param,double value) override;
        bool parameterGesture(NodeId node,uint32_t param,bool begin) override;
        void setPlaying(bool playing) override;
        void setTempo(double bpm) override;
        void setClockSource(Transport::ClockSource source) override;
        Transport::ClockSource clockSource() const override;
        TransportState transportState() const override;
        bool note(bool on,int key,double velocity,int channel=0) override;
        PadMap padMap() const override;
        void setPadMap(const PadMap& map) override;
        bool pad(uint32_t pad,bool on,double velocity,int channel=0) override;
        bool padPressure(uint32_t pad,double pressure) override;
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

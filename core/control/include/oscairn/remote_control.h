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
        bool supportsState(NodeId node) const override;
        void saveState(NodeId node,const std::string& directory) override;
        std::vector<PresetInfo> presets(NodeId node) override;
        std::vector<PropertyInfo> properties(NodeId node) const override;
        bool setProperty(NodeId node,const std::string& uri,const PropertyValue& value) override;
        bool requestProperty(NodeId node,const std::string& uri) override;
        std::optional<PropertyValue> propertyValue(NodeId node,const std::string& uri) const override;
        Sounds sounds(NodeId node) const override;
        uint32_t prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& state={},const std::string& preset={}) override;
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
    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"
#include "oscairn/transport.h"

#include <string>
#include <map>
#include <vector>

namespace oscairn
{
    /// In-process control boundary. All calls belong to one control/main thread;
    /// audio and hardware input do not call this interface. No device or plugin handles
    /// escape it. The socket/JSON adapter will implement the same control operations.
    class Control
    {
    public:
        virtual ~Control() = default;

        struct ParamInfo
        {
            uint32_t    id = 0;
            std::string name;
            double      minimum = 0, maximum = 1, defaultValue = 0;
            bool        stepped = false;
            uint32_t    flags = 0;
        };
        struct TransportState
        {
            bool   playing = false;
            double tempo = 120.0;
            double beat = 0.0;
        };
        struct DeviceChannel { uint32_t deviceChannel=0; std::string name; bool available=false; };
        struct SessionStatus
        {
            std::string audioDevice;
            std::vector<std::string> midiInputs;
            double sampleRate = 0, cpuLoad = 0;
            uint32_t bufferSize = 0;
            uint64_t deadlineMisses = 0;
            std::vector<DeviceChannel> audioInputs, audioOutputs; // logical order -> device channel
            bool audioReady=true;
            uint64_t graphEventDrops=0;
        };
        virtual SessionStatus sessionStatus() const = 0;
        struct GraphState
        {
            Graph::Description topology;
            std::map<NodeId,std::string> names;
            uint64_t revision = 0;
            bool staged = false;
        };
        enum class ChangeKind { Value, GestureBegin, GestureEnd, Graph, Resync };
        struct Change
        {
            uint64_t sequence = 0;
            ChangeKind kind = ChangeKind::Value;
            NodeId node = 0;
            uint32_t param = 0;
            double value = 0;
            bool plugin = false;
        };
        struct Changes { uint64_t latest = 0; bool lost = false; std::vector<Change> events; };

        struct SoundInfo { uint32_t id;std::string name;uint32_t selectedChannels,activeVoices;bool idle; };
        struct Sounds { uint32_t capacity;std::vector<SoundInfo> contexts; };
        virtual Sounds sounds(NodeId node) const = 0;
        virtual uint32_t prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters) = 0;
        virtual bool selectSound(NodeId node,uint32_t id,int16_t channel=-1) = 0;
        virtual bool discardSound(NodeId node,uint32_t id) = 0;

        virtual GraphState graphState() const = 0;
        virtual NodeId add( const std::string& name, const std::string& path, const std::string& pluginId = {} ) = 0;
        virtual void remove( NodeId node ) = 0;
        virtual void connectAudio( NodeId from, NodeId to ) = 0;
        virtual void connectAudio( const AudioConnection& ) = 0;
        virtual void disconnectAudio( const AudioConnection& ) = 0;
        virtual void connectNotes( NodeId from, NodeId to ) = 0;
        virtual void disconnectAudio( NodeId from, NodeId to ) = 0;
        virtual void disconnectNotes( NodeId from, NodeId to ) = 0;
        virtual void mapParameter( NodeId from, uint32_t lane, NodeId to, uint32_t param ) = 0;
        virtual void unmapParameter( NodeId from, uint32_t lane ) = 0;
        virtual void commit() = 0;
        /// Bounded history, independent cursor for every consumer. lost requires resync.
        virtual uint64_t changeSequence() const = 0;
        virtual Changes changesAfter( uint64_t sequence ) const = 0;

        virtual NodeId nodeId( const std::string& name ) const = 0;
        virtual bool hasNode( NodeId node ) const = 0;
        virtual bool hasParameter( NodeId node, uint32_t param ) const = 0;
        virtual std::vector<ParamInfo> parameters( NodeId node ) const = 0;
        virtual ParamInfo parameterInfo( NodeId node, uint32_t param ) const = 0;
        virtual double parameter( NodeId node, uint32_t param ) const = 0;
        virtual std::string parameterText( NodeId node, uint32_t param, double value ) const = 0;
        /// Accepted values coalesce until service(), then retry if the plugin is busy.
        /// False means the bounded pending-command store refused this new command.
        virtual bool setParameter( NodeId node, uint32_t param, double value ) = 0;
        virtual bool parameterGesture( NodeId node, uint32_t param, bool begin ) = 0;

        virtual void setPlaying( bool playing ) = 0;
        virtual void setTempo( double bpm ) = 0;
        virtual void setClockSource( Transport::ClockSource source ) = 0;
        virtual Transport::ClockSource clockSource() const = 0;
        /// Last rendered block's transport. Fields are atomic telemetry, not a transaction.
        virtual TransportState transportState() const = 0;

        /// UI notes at the next block boundary; channel is zero-based. Physical MIDI
        /// bypasses this queue and enters Session::process with sample offsets.
        /// False means overflow; the session discards queued UI notes and broadcasts
        /// release on the next block. This may release other voices on shared destinations.
        virtual bool note( bool on, int key, double velocity, int channel = 0 ) = 0;
    };
}

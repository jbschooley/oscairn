// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/control.h"

#include <array>
#include <atomic>
#include <map>
#include <span>

namespace oscairn
{
    /// Device-independent graph session. Construct/configure/service/destroy on the
    /// control thread. One audio thread calls process(); stop it before destruction.
    /// The caller owns audio I/O, MIDI capture, clocks, output buffers and scheduling.
    class Session final : public Control
    {
    public:
        static constexpr uint32_t maxInputEvents = 256;
        static constexpr uint32_t maxClockEvents = 1024;
        static constexpr size_t noteQueueCapacity = 128;

        Session( double sampleRate, uint32_t maxFrames );
        Control& control() noexcept { return *this; }
        const Control& control() const noexcept { return *this; }

        // Initial graph construction on the control thread. Full transactional graph
        // editing and its wire API are a later control-API slice.
        NodeId add( const std::string& name, const std::string& path, const std::string& pluginId = {} );
        void connectAudio( NodeId from, NodeId to ) { graph_.connectAudio( from, to ); }
        void connectNotes( NodeId from, NodeId to ) { graph_.connectNotes( from, to ); }
        void mapParameter( NodeId from, uint32_t lane, NodeId to, uint32_t param ) { graph_.mapParamTarget( from, lane, to, param ); }
        void commit() { graph_.commit(); }
        void service();

        NodeId nodeId( const std::string& name ) const override;
        bool hasNode( NodeId node ) const override { return graph_.contains( node ); }
        bool hasParameter( NodeId node, uint32_t param ) const override;
        std::vector<ParamInfo> parameters( NodeId node ) const override;
        ParamInfo parameterInfo( NodeId node, uint32_t param ) const override;
        double parameter( NodeId node, uint32_t param ) const override;
        std::string parameterText( NodeId node, uint32_t param, double value ) const override;
        bool setParameter( NodeId node, uint32_t param, double value ) override;

        void setPlaying( bool playing ) override { playing_.store( playing ); }
        void setTempo( double bpm ) override;
        void setClockSource( Transport::ClockSource source ) override { clockSource_.store( source ); }
        Transport::ClockSource clockSource() const override { return clockSource_.load(); }
        TransportState transportState() const override;
        bool note( bool on, int key, double velocity, int channel = 0 ) override;

        struct MidiClockEvent
        {
            uint8_t status = 0;
            int64_t sampleTime = 0; // absolute sample time, in an already rendered block
            uint16_t songPosition = 0;
        };
        /// Audio thread only. Stereo planar output, 1..maxFrames frames. Direct events
        /// must be sorted, with offsets within this block (at most maxInputEvents).
        /// MIDI-clock events (at most maxClockEvents) describe the preceding interval,
        /// as Transport requires.
        /// UI notes precede direct events at sample 0. No wall clock, locks or allocation.
        bool process( float* const* outputs, uint32_t frames,
                      std::span<const kick::clap::Event> events = {},
                      std::span<const MidiClockEvent> clocks = {} ) noexcept;

        uint64_t droppedNotes() const noexcept { return droppedNotes_.load(); }
        double sampleRate() const noexcept { return sampleRate_; }
        uint32_t maxFrames() const noexcept { return maxFrames_; }

    private:
        static ParamInfo describe( const kick::clap::Parameter& param );
        const kick::clap::Parameter& findParameter( NodeId node, uint32_t param ) const;

        double sampleRate_;
        uint32_t maxFrames_;
        Graph graph_;
        std::map<std::string, NodeId> nodes_;
        Transport transport_; // audio thread after construction
        Transport::ClockSource following_ = Transport::ClockSource::Internal;
        std::atomic<bool> playing_{ false };
        std::atomic<double> tempo_{ 120.0 };
        std::atomic<Transport::ClockSource> clockSource_{ Transport::ClockSource::Internal };
        std::atomic<bool> statusPlaying_{ false };
        std::atomic<double> statusTempo_{ 120.0 }, statusBeat_{ 0.0 };

        // Single producer (control), single consumer (audio). A process call drains
        // only the published snapshot, so a busy producer cannot extend its deadline.
        std::array<kick::clap::Event, noteQueueCapacity> notes_{};
        std::atomic<size_t> noteHead_{ 0 }, noteTail_{ 0 };
        std::atomic<bool> panic_{ false };
        std::atomic<uint64_t> droppedNotes_{ 0 };
        std::array<kick::clap::Event, noteQueueCapacity + maxInputEvents + 1> merged_{};
    };
}

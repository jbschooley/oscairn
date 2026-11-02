// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oscairn
{
    /// Recording (RFC 0020): the audio thread copies each tap's block into a ring preallocated
    /// on the control thread; a writer thread turns the rings into a take's files. The audio
    /// thread never allocates, locks or touches a file. One take at a time.
    ///
    /// A tap's sources are compiled on the control thread into a table handed to the audio
    /// thread through an atomic pointer, so a fader move during a take changes what the next
    /// blocks record without touching the take's files.
    class Recorder final : public Graph::Listener
    {
    public:
        /// Channel `to` of the tap gets channel `channel` of `node`'s audio outputs (port order;
        /// graphInput: an external input channel, graphOutput: an output channel), times gain,
        /// summed.
        struct Source { NodeId node = 0; uint32_t channel = 0, to = 0; float gain = 1; };
        struct AudioTap { std::string name; uint32_t channels = 0; std::vector<Source> sources; }; // one WAV file
        /// One track of the take's MIDI file: the events these nodes receive (emitted: send).
        /// graphInput: the events entering the graph, which the session hands over itself.
        struct NoteTap { std::string name; std::vector<NodeId> nodes; bool emitted = false; };
        struct Taps { std::vector<AudioTap> audio; std::vector<NoteTap> notes; };
        struct Overrun { uint64_t start = 0, frames = 0; }; // frames from the take's start, written as silence
        struct Take
        {
            uint32_t take = 0;
            std::string state;    // recording, finishing, done, failed
            std::string folder, error;
            std::vector<std::string> files;
            uint64_t start = 0;   // the first sample on the engine's timeline
            uint64_t frames = 0;  // written to each file so far
            std::vector<Overrun> overruns;
            uint64_t droppedNotes = 0; // the notes ring was full, or the event has no MIDI form
        };

        static constexpr uint32_t noteCapacity = 65536; // events the notes ring holds
        Recorder( double sampleRate, uint32_t maxFrames );
        ~Recorder(); // the audio thread has stopped; a take still recording is closed with what arrived

        // Control thread ----------------------------------------------------------
        /// Allocates and locks the rings, makes `folder` and starts the writer; the take starts at
        /// the next block. Throws std::logic_error while another take records, std::invalid_argument
        /// for no taps.
        uint32_t start( const std::filesystem::path& folder, const Taps& taps, const std::string& description = {} );
        /// New sources for the take recording (after an edit). Names and channel counts stay as
        /// the take started: extra channels are dropped and missing ones are silent.
        void update( const Taps& taps );
        /// Ends the take at the next block; the writer then closes its files.
        void stop();
        uint32_t recording() const noexcept { return recording_; } // the take, or 0
        std::vector<Take> takes() const;
        void service(); // frees the rings of finished takes
        uint64_t committedBytes() const noexcept { return committed_.load(); } // rings now allocated (RFC 0025)
        // Ring length: how long the writer may stall before samples are lost. 10 s by default.
        void setRingSeconds( double seconds ) { ringSeconds_ = seconds; }
        // The writer stops a take before a drive has less than this free (64 MiB).
        void setDiskReserve( uint64_t bytes ) { reserve_ = bytes; }
        void holdWriter( bool hold ) noexcept { hold_.store( hold ); } // for tests: a stalled writer

        // Audio thread ------------------------------------------------------------
        /// Before the graph: `time` is the block's first sample on the engine's timeline.
        void begin( uint64_t time, double tempo, double beat ) noexcept;
        void graphEvents( const kick::clap::Event* events, uint32_t count ) noexcept; // entering the graph
        void events( NodeId node, const kick::clap::Event* events, uint32_t count, bool emitted ) noexcept override;
        /// After the graph: copies the audio taps.
        void end( const Graph& graph, const float* const* outputs, uint32_t outputChannels, uint32_t frames ) noexcept;

        struct Active; // one take: its rings, the writer's state

    private:
        struct Table { std::shared_ptr<Active> take; Taps taps; };
        void publish( std::unique_ptr<Table> table );
        void note( uint16_t track, const kick::clap::Event& event ) noexcept;

        double sampleRate_;
        uint32_t maxFrames_;
        double ringSeconds_ = 10;
        uint64_t reserve_ = 64ull << 20;
        std::atomic<bool> hold_{ false };
        std::atomic<uint64_t> committed_{ 0 };
        uint32_t nextTake_ = 1, recording_ = 0;
        std::vector<std::shared_ptr<Active>> live_;        // control thread: until the writer is done
        std::vector<Take> finished_;
        std::vector<std::unique_ptr<Table>> tables_;       // control thread; freed when neither published nor in use
        std::atomic<Table*> table_{ nullptr }, inUse_{ nullptr }; // inUse_: the audio thread's (a hazard pointer)
        // Audio thread
        Table* block_ = nullptr;
        Active* current_ = nullptr;
    };
}

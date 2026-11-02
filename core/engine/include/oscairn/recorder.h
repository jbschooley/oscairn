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
    /// thread never allocates, locks or touches a file. One take recording at a time; takes of
    /// a set length (a capture) write beside it.
    ///
    /// Rings count on the engine's timeline, so a take is a span of it: its first sample may be
    /// in the past (the retroactive rings, always armed outside a take, hold the last minutes)
    /// or on a coming bar line. A tap's sources are compiled on the control thread into a table
    /// handed to the audio thread through an atomic pointer, so a fader move during a take
    /// changes what the next blocks record without touching the take's files.
    class Recorder final : public Graph::Listener
    {
    public:
        /// Channel `to` of the tap gets channel `channel` of `node`'s audio outputs (port order;
        /// graphInput: an external input channel, graphOutput: an output channel), times gain,
        /// summed.
        struct Source { NodeId node = 0; uint32_t channel = 0, to = 0; float gain = 1; bool operator==( const Source& ) const = default; };
        struct AudioTap { std::string name; uint32_t channels = 0; std::vector<Source> sources; bool operator==( const AudioTap& ) const = default; }; // one WAV file
        /// One track of the take's MIDI file: the events these nodes receive (emitted: send).
        /// graphInput: the events entering the graph, which the session hands over itself.
        struct NoteTap { std::string name; std::vector<NodeId> nodes; bool emitted = false; bool operator==( const NoteTap& ) const = default; };
        struct Taps { std::vector<AudioTap> audio; std::vector<NoteTap> notes; };
        struct Overrun { uint64_t start = 0, frames = 0; }; // frames from the take's start, written as silence
        struct Take
        {
            uint32_t take = 0;
            std::string state;    // waiting, recording, finishing, done, failed
            std::string folder, error;
            std::vector<std::string> files;
            uint64_t start = 0;   // the first sample on the engine's timeline
            uint64_t frames = 0;  // written to each file so far
            std::vector<Overrun> overruns;
            uint64_t droppedNotes = 0; // lapped in the notes ring, or the event has no MIDI form
            double sampleRate = 0;
            bool behind = false;  // the writer is more than half a ring behind: falling behind, nothing lost yet
            bool playing = false; // being played back (the session's)
        };
        /// Where a take starts and ends (RFC 0020, "Start and stop"). A start in the past reads
        /// the retroactive rings, and the take ends now unless keepGoing; a bar line is the
        /// transport's (with it stopped, now). A length ends it by itself.
        struct Start
        {
            enum class From { Now, SecondsAgo, NextBar, LastBar } from = From::Now;
            double seconds = 0;   // SecondsAgo
            enum class Until { Stop, Seconds, Bars } until = Until::Stop;
            double length = 0;    // seconds or bars
            bool keepGoing = false;
        };

        static constexpr uint32_t noteCapacity = 65536; // events a notes ring holds
        Recorder( double sampleRate, uint32_t maxFrames );
        ~Recorder(); // the audio thread has stopped; a take still recording is closed with what arrived

        // Control thread ----------------------------------------------------------
        /// Makes `folder` and starts the writer. A tap the retroactive rings hold reads them; any
        /// other gets a ring of its own, allocated and locked here, which can only start now or
        /// later. Throws std::logic_error while another take records without an end,
        /// std::invalid_argument for no taps, std::out_of_range for a start in the past on a tap not held.
        uint32_t start( const std::filesystem::path& folder, const Taps& taps, const std::string& description, const Start& when );
        uint32_t start( const std::filesystem::path& folder, const Taps& taps, const std::string& description = {} ) { return start( folder, taps, description, Start{} ); }
        /// New sources for a live take's rings of its own (after an edit): the take recording or a
        /// capture of set length. Names and channel counts stay as the take started: extra
        /// channels are dropped and missing ones are silent. False once the take no longer feeds
        /// rings of its own (ended, or all its taps from the retroactive rings, which follow
        /// updateRetro), when there is nothing to update.
        bool update( uint32_t take, const Taps& taps );
        /// Ends the take recording now; the writer then closes its files.
        void stop();
        uint32_t recording() const noexcept { return recording_; } // the take with no end yet, or 0
        std::vector<Take> takes() const; // this run's, oldest first
        void forget( uint32_t take );    // a finished take dropped from takes() (its folder renamed or deleted)
        void service(); // frees the rings of finished takes and a playback that ended
        uint64_t committedBytes() const noexcept { return committed_.load(); } // rings and playback now allocated (RFC 0025)
        // Ring length for a tap of a take's own: how long the writer may stall before samples are lost. 10 s by default.
        void setRingSeconds( double seconds ) { ringSeconds_ = seconds; }
        /// The retroactive rings (RFC 0020): these taps always kept for the last `seconds`, outside
        /// any take, never written unless a take asks. 0 or no taps: none. What they need is
        /// allocated (and counted in committedBytes()) before the old ones go. Throws
        /// std::logic_error while a take recording reads them.
        void setRetro( double seconds, const Taps& taps );
        void updateRetro( const Taps& taps ); // new sources (after an edit)
        double retroSeconds() const noexcept { return retroSeconds_; }
        uint64_t retroCommitted() const { return retroBytes( retroSeconds_, retro_.taps, sampleRate_, maxFrames_ ); }
        static uint64_t retroBytes( double seconds, const Taps& taps, double sampleRate, uint32_t maxFrames ); // what setRetro would allocate
        // The writer stops a take before a drive has less than this free (64 MiB).
        void setDiskReserve( uint64_t bytes ) { reserve_ = bytes; }
        void holdWriter( bool hold ) noexcept { hold_.store( hold ); } // for tests: a stalled writer
        /// Plays a take's first WAV file into the outputs, after the taps (so no take records it),
        /// from its start to its end. Throws std::invalid_argument for a file that is not a float
        /// WAV at this sample rate. play(0, {}) stops.
        void play( uint32_t take, const std::filesystem::path& wav );
        uint32_t playing() const noexcept; // the take playing, or 0
        uint64_t now() const noexcept;     // the engine's timeline at the end of the last block

        // Audio thread ------------------------------------------------------------
        /// Before the graph: `time` is the block's first sample on the engine's timeline.
        void begin( uint64_t time, const clap_event_transport_t& transport ) noexcept;
        void graphEvents( const kick::clap::Event* events, uint32_t count ) noexcept; // entering the graph
        void events( NodeId node, const kick::clap::Event* events, uint32_t count, bool emitted ) noexcept override;
        /// After the graph: copies the audio taps, then adds a playback to the outputs.
        void end( const Graph& graph, float* const* outputs, uint32_t outputChannels, uint32_t frames ) noexcept;

        struct Ring;   // one tap's samples
        struct Notes;  // one set of note taps' events
        struct Active; // one take: its rings, the writer's state
        struct Player; // a take played back

    private:
        struct Feed { std::shared_ptr<Ring> ring; std::vector<Source> sources; };
        struct NoteFeed { std::shared_ptr<Notes> ring; std::vector<NoteTap> taps; };
        struct Table { std::vector<Feed> audio; std::vector<NoteFeed> notes; std::shared_ptr<Player> player; };
        struct Set { std::vector<std::shared_ptr<Ring>> rings; std::shared_ptr<Notes> notes; Taps taps; }; // rings fed by taps
        void publish();
        void recount();
        void note( Notes& ring, uint16_t track, const kick::clap::Event& event ) noexcept;
        std::shared_ptr<Ring> ring( uint32_t channels, uint64_t capacity ) const;

        double sampleRate_;
        uint32_t maxFrames_;
        double ringSeconds_ = 10, retroSeconds_ = 0;
        uint64_t reserve_ = 64ull << 20;
        std::atomic<bool> hold_{ false };
        std::atomic<uint64_t> committed_{ 0 };
        uint32_t nextTake_ = 1, recording_ = 0;
        Set retro_, own_;   // control thread: the retroactive rings; the recording take's own
        std::shared_ptr<Player> player_;
        std::vector<std::shared_ptr<Active>> live_;        // control thread: until the writer is done
        std::vector<Take> finished_;
        std::vector<std::unique_ptr<Table>> tables_;       // control thread; freed when neither published nor in use
        std::atomic<Table*> table_{ nullptr }, inUse_{ nullptr }; // inUse_: the audio thread's (a hazard pointer)
        // The last block's timeline and transport, for the control thread: a sequence lock.
        std::atomic<uint32_t> clockSequence_{ 0 };
        std::atomic<uint64_t> clockTime_{ 0 }, clockEnd_{ 0 };
        std::atomic<double> clockTempo_{ 120 }, clockBeat_{ 0 };
        std::atomic<uint32_t> clockBar_{ 4 };
        std::atomic<bool> clockPlaying_{ false };
        struct Clock { uint64_t time = 0, end = 0; double tempo = 120, beat = 0; uint32_t bar = 4; bool playing = false; };
        Clock clock() const noexcept;
        // Audio thread
        Table* block_ = nullptr;
        uint64_t time_ = 0;
        double tempo_ = 120, beat_ = 0;
        uint32_t bar_ = 4;
        bool playing_ = false;
    };
}

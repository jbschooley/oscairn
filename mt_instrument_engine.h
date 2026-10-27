/*
 * Instrument engines: the sound source behind a Plugin ("instrument") program.
 *
 * An engine only sees notes, parameters and an audio buffer. Pads, sequencer,
 * mixer, sends and inserts are handled by the program around it, so an engine
 * can be written (or ported, e.g. from Rust through a C ABI) without knowing
 * anything else about Kick.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

namespace mt
{
    struct InstrumentParam
    {
        juce::String id;    // stable key, stored in the project
        juce::String name;  // shown in the UI
        juce::String label; // unit, e.g. "Hz", "s", "%"
        float        minValue{ 0.f };
        float        maxValue{ 1.f };
        float        defaultValue{ 0.f };
    };

    class IInstrumentEngine
    {
    public:
        virtual ~IInstrumentEngine() = default;

        /// Called on the message thread before the engine is used, and again if the
        /// sample rate or maximum block size changes.
        virtual void prepare( double SampleRate, int MaxBlockSize ) = 0;

        // Everything below runs on the audio thread.

        virtual void reset() = 0;

        /// Velocity is 0..1.
        virtual void noteOn( int Note, float Velocity ) = 0;
        virtual void noteOff( int Note )                = 0;
        virtual void allNotesOff()                      = 0;

        /// Writes (does not add) NumSamples into each of the NumChannels outputs.
        virtual void process( float* const* Out, int NumChannels, int NumSamples ) = 0;

        /// Fixed for the engine's lifetime; may be read from any thread.
        virtual const std::vector<InstrumentParam>& getParams() const = 0;
        virtual void                                setParam( int Index, float Value ) = 0;
    };

    /// Engines available in this build, by name.
    struct InstrumentEngineRegistry
    {
        using Factory = std::function<std::unique_ptr<IInstrumentEngine>()>;

        static void                               add( const juce::String& Name, Factory F );
        static std::unique_ptr<IInstrumentEngine> create( const juce::String& Name );
        static juce::StringArray                  getNames();
        static juce::String                       getDefault();
    };
} // namespace mt

/*
 * "Modular": an instrument engine whose sound is a patch of modules on a grid
 * (see mt_modular_patch.h), in the spirit of Drambo or Bitwig's grid devices.
 *
 * The patch is built on the message thread into a graph: a flat list of modules
 * with one instance per voice. Structural edits (adding, moving or removing a
 * module or a modulation) build a new graph and swap it in under a short lock,
 * carrying held notes over. Knob and depth edits only write atomics, so they
 * never interrupt the sound.
 *
 * Its fixed parameters are eight macros (modulation sources the patch maps to
 * its knobs) and an output level.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

namespace mt
{
    class ModularEngine : public IInstrumentEngine
    {
    public:
        enum Param
        {
            kMacro1,
            kLevel = kMacro1 + 8,
            kNumParams
        };

        ModularEngine();
        ~ModularEngine() override;

        void prepare( double SampleRate, int MaxBlockSize ) override;
        void reset() override;
        void noteOn( int Note, float Velocity ) override;
        void noteOff( int Note ) override;
        void allNotesOff() override;
        void pitchBend( float Semitones ) override;
        void modWheel( float Amount ) override;
        void aftertouch( int Note, float Amount ) override;
        void channelPressure( float Amount ) override;
        void setTempo( double Bpm ) override;
        void process( float* const* Out, int NumChannels, int NumSamples ) override;

        const std::vector<InstrumentParam>& getParams() const override { return params; }
        void                                setParam( int Index, float Value ) override;

        juce::Identifier  getPatchType() const override { return modular::PatchIDs::MODULAR_PATCH; }
        juce::StringArray getPresetNames() const override { return modular::getPresetNames(); }
        juce::ValueTree   createPreset( int Index ) const override { return modular::createPreset( Index ); }
        void              setPatch( const juce::ValueTree& Patch ) override;
        /// Message thread: a module's or effect's latest output (modulators: their value
        /// in the newest voice; audio: the block's peak). 0 for an unknown id.
        float getActivity( int PartId ) const override;

        // Diagnostics (tests, meters).
        int getBuildCount() const { return buildCount; } // message thread
        int getNumActiveVoices();
        double getTempo() const { return tempo.load(); }

        struct Graph; // mt_modular_engine.cpp

    private:
        std::vector<InstrumentParam> params;

        juce::SpinLock         graphLock; // audio thread vs. graph swaps and prepare()
        std::unique_ptr<Graph> graph;
        std::atomic<double>    sampleRate{ 48000 };
        int                    buildCount{ 0 };

        // Set on the audio thread, also read when a rebuild carries notes over.
        std::array<std::atomic<float>, 8> macros{};
        std::atomic<float>                level{ 0.7f };
        std::atomic<float>                bendSemitones{ 0.f };
        std::atomic<float>                modWheelAmount{ 0.f };
        std::atomic<double>               tempo{ 120.0 };
        float                             lastGain{ 0.f }; // audio thread
    };
} // namespace mt

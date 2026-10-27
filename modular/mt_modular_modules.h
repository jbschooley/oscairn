/*
 * Modular engine: the module library.
 *
 * A module sits in one cell of the patch grid. Sources (oscillators, noise) add
 * to the signal coming down their column, processors (filter, VCA, drive...)
 * transform it, and modulators (envelopes, LFOs...) pass it through untouched
 * and instead offer an output that can modulate any knob in the patch.
 *
 * Every module instance belongs to one voice. Signals are mono inside a column;
 * a Pan module places the column in the stereo field. A cable adds another
 * audio module's output to a module's input (FM, routing between columns).
 *
 * Effects are different: one stereo instance each, in a chain after the voices
 * are mixed (delay, chorus, reverb...). Only voice-independent sources (macros,
 * mod wheel, bend) can modulate them.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#pragma once

namespace mt::modular
{
    /// Samples per control step: modulation is evaluated once per sub-block and
    /// ramped (gains) or held (filter coefficients, pitch) within it.
    constexpr int subBlock = 16;

    enum class Curve
    {
        Linear,
        Log,  // min must be > 0
        Steps // integers min..max; `choices` names them when it is not empty
    };

    struct ParamSpec
    {
        juce::String              id;    // stored in the patch
        juce::String              name;  // shown in the UI
        juce::String              label; // unit
        float                     minValue{ 0.f };
        float                     maxValue{ 1.f };
        float                     defaultValue{ 0.f };
        Curve                     curve{ Curve::Linear };
        std::vector<juce::String> choices{};

        /// Modulation is applied in normalised (0..1) space, so a depth means the same
        /// share of every knob's travel whatever its unit.
        float toNormal( float Value ) const;
        float fromNormal( float Normal ) const;
    };

    enum class Role
    {
        Source,
        Processor,
        Modulator,
        Effect
    };

    /// What a module sees of its voice for one sub-block.
    struct VoiceContext
    {
        double sampleRate{ 48000 };
        float  note{ 60 };     // MIDI note including pitch bend, fractional
        float  velocity{ 1 };  // 0..1
        bool   gate{ false };
    };

    class Module
    {
    public:
        virtual ~Module() = default;

        /// Message thread (or wherever the engine is prepared); may allocate.
        virtual void prepare( double /*SampleRate*/ ) {}

        // Audio thread from here on.

        /// Back to the state of a freshly built module (the voice was freed).
        virtual void reset() {}
        /// Steal: the voice was still sounding; keep phases and levels to avoid clicks.
        virtual void noteOn( const VoiceContext&, bool /*Steal*/ ) {}
        virtual void noteOff() {}

        /// Sources and processors: process Buf in place. P0 holds the parameter values
        /// at the start of the sub-block and P at its end (ramp between them for gains).
        virtual void process( const VoiceContext&, const float* /*P0*/, const float* /*P*/, float* /*Buf*/, int /*N*/ ) {}

        /// Modulators: advance N samples and return the output (N = 0 just reads it).
        virtual float modulate( const VoiceContext&, const float* /*P*/, int /*N*/ ) { return 0.f; }

        /// Effects: process the mixed voices in place, once per block.
        virtual void processStereo( const float* /*P*/, float* /*L*/, float* /*R*/, int /*N*/ ) {}

        /// Envelopes: still running. A voice ends once its gate is off and none is.
        virtual bool isEnvelope() const { return false; }
        virtual bool isActive() const { return false; }
    };

    struct ModuleSpec
    {
        juce::String           type; // stored in the patch
        juce::String           name; // shown in the UI
        Role                   role;
        std::vector<ParamSpec> params;
        std::function<std::unique_ptr<Module>()> create;

        int findParam( const juce::String& Id ) const;
    };

    /// Every module type, in the order a module picker should list them.
    const std::vector<ModuleSpec>& getModuleSpecs();
    /// Effect types for the chain after the voices.
    const std::vector<ModuleSpec>& getEffectSpecs();
    /// Searches modules, then effects.
    const ModuleSpec* findModuleSpec( const juce::String& Type );

    //==============================================================================
    /// Modulation sources other than modulator modules, in source-index order.
    enum BuiltinSource
    {
        kVelocity, // 0..1
        kNote,     // -1..1 over +-5 octaves around C3 (60)
        kGate,     // 0 or 1
        kModWheel, // 0..1
        kBend,     // -1..1 over the bend range
        kMacro1,   // 0..1, eight of them
        kNumBuiltinSources = kMacro1 + 8
    };

    /// Stored names, e.g. "velocity", "macro3"; a module output is "module:<id>".
    juce::String builtinSourceId( int Source );
    int          findBuiltinSource( const juce::String& Id ); // -1 if not a builtin
    /// The same for every voice, so it can modulate effects.
    inline bool isGlobalSource( int Source ) { return Source == kModWheel || Source == kBend || ( Source >= kMacro1 && Source < kNumBuiltinSources ); }
} // namespace mt::modular

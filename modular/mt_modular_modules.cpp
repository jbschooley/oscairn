/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt::modular
{
    float ParamSpec::toNormal( float Value ) const
    {
        Value = juce::jlimit( minValue, maxValue, Value );
        if( maxValue <= minValue )
            return 0.f;
        if( curve == Curve::Log )
            return std::log( Value / minValue ) / std::log( maxValue / minValue );
        return ( Value - minValue ) / ( maxValue - minValue );
    }

    float ParamSpec::fromNormal( float Normal ) const
    {
        Normal = juce::jlimit( 0.f, 1.f, Normal );
        switch( curve )
        {
            case Curve::Log: return minValue * std::pow( maxValue / minValue, Normal );
            case Curve::Steps: return std::round( minValue + Normal * ( maxValue - minValue ) );
            case Curve::Linear: break;
        }
        return minValue + Normal * ( maxValue - minValue );
    }

    int ModuleSpec::findParam( const juce::String& Id ) const
    {
        for( size_t i = 0; i < params.size(); ++i )
            if( params[i].id == Id )
                return (int)i;
        return -1;
    }

    namespace
    {
        constexpr float twoPi = juce::MathConstants<float>::twoPi;

        inline double noteToHz( double Note ) { return 440.0 * std::pow( 2.0, ( Note - 69.0 ) / 12.0 ); }

        // PolyBLEP residual for a discontinuity at phase 0 (phase and increment in cycles).
        inline float polyBlep( double t, double dt )
        {
            if( t < dt )
            {
                t /= dt;
                return (float)( t + t - t * t - 1.0 );
            }
            if( t > 1.0 - dt )
            {
                t = ( t - 1.0 ) / dt;
                return (float)( t * t + t + t + 1.0 );
            }
            return 0.f;
        }

        // Linear ramp from A (exclusive) to B (inclusive) over N samples.
        struct Ramp
        {
            Ramp( float A, float B, int N ) : value( A ), step( ( B - A ) / (float)juce::jmax( 1, N ) ) {}
            float next() { return value += step; }
            float value, step;
        };

        // Small per-instance random generator (xorshift32), audio-thread safe.
        struct Rng
        {
            Rng()
            {
                static std::atomic<uint32_t> seeds{ 0x9E3779B9u };
                state = seeds.fetch_add( 0x6D2B79F5u ) | 1u;
            }
            uint32_t next()
            {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                return state;
            }
            float bipolar() { return (float)( next() >> 8 ) * ( 2.f / 16777216.f ) - 1.f; }
            uint32_t state;
        };

        //==========================================================================
        class Oscillator : public Module
        {
        public:
            enum { kWave, kTune, kFine, kPw, kLevel, kInput, kPm, kUnison, kSpread };
            enum Wave { Sine, Triangle, Saw, Square };
            enum Input { Add, PhaseMod, Replace };
            static constexpr int maxUnison = 7;

            void reset() override { phases.fill( 0.0 ); }
            void noteOn( const VoiceContext&, bool Steal ) override
            {
                if( Steal )
                    return;
                // The first copy starts at 0 for a consistent attack; unison copies start at
                // random phases so they do not begin as one loud, phasing wave.
                phases[0] = 0.0;
                for( size_t u = 1; u < phases.size(); ++u )
                    phases[u] = 0.5 * ( rng.bipolar() + 1.f );
            }

            void process( const VoiceContext& C, const float* P0, const float* P, float* Buf, int N ) override
            {
                const auto wave  = (int)P[kWave];
                const auto mode  = (int)P[kInput];
                const auto copies = juce::jlimit( 1, maxUnison, (int)P[kUnison] );
                const auto note  = C.note + P[kTune] + P[kFine] * 0.01;
                const auto pw    = (double)P[kPw];
                const auto pm    = P[kPm];
                const auto norm  = 1.f / std::sqrt( (float)copies ); // unison keeps about the same loudness

                // Unison copies spread evenly across +-Spread cents.
                std::array<double, maxUnison> dt{};
                for( int u = 0; u < copies; ++u )
                {
                    const auto cents = copies == 1 ? 0.0 : P[kSpread] * ( 2.0 * u / ( copies - 1 ) - 1.0 );
                    dt[(size_t)u]    = juce::jlimit( 0.0, 0.45, noteToHz( note + cents * 0.01 ) / C.sampleRate );
                }

                Ramp level( P0[kLevel], P[kLevel], N );
                for( int i = 0; i < N; ++i )
                {
                    const double offset = mode == PhaseMod ? pm * Buf[i] : 0.0;
                    float        s      = 0.f;
                    for( int u = 0; u < copies; ++u )
                    {
                        auto& phase = phases[(size_t)u];
                        double x    = phase;
                        if( mode == PhaseMod )
                        {
                            x += offset;
                            x -= std::floor( x );
                        }
                        s += shape( wave, x, dt[(size_t)u], pw );
                        phase += dt[(size_t)u];
                        if( phase >= 1.0 )
                            phase -= 1.0;
                    }

                    const auto out = level.next() * s * norm;
                    Buf[i]         = mode == Add ? Buf[i] + out : out;
                }
            }

        private:
            static float shape( int Wave, double X, double Dt, double Pw )
            {
                switch( Wave )
                {
                    case Sine: return std::sin( twoPi * (float)X );
                    case Triangle: return 4.f * std::abs( (float)X - 0.5f ) - 1.f;
                    case Saw: return (float)( 2.0 * X - 1.0 ) - polyBlep( X, Dt );
                    default: break;
                }
                auto s = X < Pw ? 1.f : -1.f;
                s += polyBlep( X, Dt );
                s -= polyBlep( std::fmod( X + 1.0 - Pw, 1.0 ), Dt );
                return s;
            }

            std::array<double, maxUnison> phases{};
            Rng                           rng;
        };

        //==========================================================================
        class Noise : public Module
        {
        public:
            enum { kColor, kLevel };

            void reset() override { lp = 0; }
            void process( const VoiceContext&, const float* P0, const float* P, float* Buf, int N ) override
            {
                // Colour = one-pole low-pass, from white (0) to a dark rumble (1).
                const auto coef = 1.f - 0.98f * P[kColor];
                Ramp       level( P0[kLevel], P[kLevel], N );
                for( int i = 0; i < N; ++i )
                {
                    lp += ( rng.bipolar() - lp ) * coef;
                    Buf[i] += level.next() * lp * ( 1.f + 2.f * P[kColor] ); // keep dark noise audible
                }
            }

        private:
            Rng   rng;
            float lp{ 0 };
        };

        //==========================================================================
        class Filter : public Module
        {
        public:
            enum { kMode, kCutoff, kReso, kTrack };
            enum Mode { LowPass, BandPass, HighPass, Notch, Ladder };

            void reset() override
            {
                ic1 = ic2 = 0;
                ladder.fill( 0.f );
                ladderOut = 0;
            }
            void process( const VoiceContext& C, const float*, const float* P, float* Buf, int N ) override
            {
                // Topology-preserving SVF (Zavalishin / Cytomic), coefficients per sub-block.
                const auto sr   = (float)C.sampleRate;
                const auto fc   = juce::jlimit( 10.f, 0.45f * sr, P[kCutoff] * std::exp2( P[kTrack] * ( C.note - 60.f ) / 12.f ) );
                const auto g    = std::tan( juce::MathConstants<float>::pi * fc / sr );
                if( (int)P[kMode] == Ladder )
                {
                    processLadder( g, P[kReso], Buf, N );
                    return;
                }
                const auto k    = 2.f - 1.96f * P[kReso];
                const auto a1   = 1.f / ( 1.f + g * ( g + k ) );
                const auto a2   = g * a1;
                const auto a3   = g * a2;
                const auto mode = (int)P[kMode];

                for( int i = 0; i < N; ++i )
                {
                    const auto x  = Buf[i];
                    const auto v3 = x - ic2;
                    const auto v1 = a1 * ic1 + a2 * v3;
                    const auto v2 = ic2 + a2 * ic1 + a3 * v3;
                    ic1           = 2.f * v1 - ic1;
                    ic2           = 2.f * v2 - ic2;

                    switch( mode )
                    {
                        case LowPass: Buf[i] = v2; break;
                        case BandPass: Buf[i] = v1; break;
                        case HighPass: Buf[i] = x - k * v1 - v2; break;
                        default: Buf[i] = x - k * v1; break;
                    }
                }
            }

        private:
            // Four one-pole stages with saturated, one-sample-delayed feedback: a 24 dB/oct
            // low-pass in the style of the Moog ladder. It self-oscillates near full resonance.
            void processLadder( float G, float Reso, float* Buf, int N )
            {
                const auto a    = G / ( 1.f + G );
                const auto k    = 3.9f * Reso;
                const auto gain = 1.f + 0.5f * k; // resonance thins the low end; give some back
                for( int i = 0; i < N; ++i )
                {
                    auto u = std::tanh( Buf[i] * gain - k * ladderOut );
                    for( auto& z : ladder )
                    {
                        const auto v = ( u - z ) * a;
                        const auto y = v + z;
                        z            = y + v;
                        u            = y;
                    }
                    ladderOut = u;
                    Buf[i]    = u;
                }
            }

            float                ic1{ 0 }, ic2{ 0 };
            std::array<float, 4> ladder{};
            float                ladderOut{ 0 };
        };

        //==========================================================================
        // Three band-passes at the formants of a sung vowel, morphing A-E-I-O-U.
        class Formant : public Module
        {
        public:
            enum { kVowel, kShift, kReso, kMix };

            void reset() override { bands = {}; }

            void process( const VoiceContext& C, const float* P0, const float* P, float* Buf, int N ) override
            {
                // Formant frequencies (Hz) and levels of A, E, I, O, U (an adult male voice).
                static constexpr float freq[5][3]  = { { 800, 1150, 2900 }, { 400, 1600, 2700 }, { 270, 2140, 2950 }, { 450, 800, 2830 }, { 325, 700, 2530 } };
                static constexpr float level[5][3] = { { 1, 0.5f, 0.25f }, { 1, 0.35f, 0.3f }, { 1, 0.25f, 0.3f }, { 1, 0.55f, 0.15f }, { 1, 0.4f, 0.1f } };

                const auto v     = juce::jlimit( 0.f, 4.f, P[kVowel] );
                const auto i0    = juce::jmin( 3, (int)v );
                const auto t     = v - (float)i0;
                const auto shift = std::exp2( P[kShift] / 12.f );
                const auto sr    = (float)C.sampleRate;
                const auto k     = 1.f / ( 3.f + 17.f * P[kReso] ); // narrower bands as Resonance rises

                std::array<float, 3> a1{}, a2{}, a3{}, amp{};
                for( size_t b = 0; b < 3; ++b )
                {
                    const auto f  = juce::jlimit( 50.f, 0.45f * sr, std::exp( std::log( freq[i0][b] ) * ( 1.f - t ) + std::log( freq[i0 + 1][b] ) * t ) * shift );
                    const auto g  = std::tan( juce::MathConstants<float>::pi * f / sr );
                    a1[b]         = 1.f / ( 1.f + g * ( g + k ) );
                    a2[b]         = g * a1[b];
                    a3[b]         = g * a2[b];
                    amp[b]        = level[i0][b] * ( 1.f - t ) + level[i0 + 1][b] * t;
                }

                Ramp mix( P0[kMix], P[kMix], N );
                for( int i = 0; i < N; ++i )
                {
                    const auto x   = Buf[i];
                    float      sum = 0.f;
                    for( size_t b = 0; b < 3; ++b )
                    {
                        auto&      st = bands[b];
                        const auto v3 = x - st[1];
                        const auto v1 = a1[b] * st[0] + a2[b] * v3;
                        const auto v2 = st[1] + a2[b] * st[0] + a3[b] * v3;
                        st[0]         = 2.f * v1 - st[0];
                        st[1]         = 2.f * v2 - st[1];
                        sum += amp[b] * k * v1 * 2.f; // k * band-pass = unity at the centre
                    }
                    const auto m = mix.next();
                    Buf[i]       = x * ( 1.f - m ) + sum * m * 1.5f; // the bands keep only part of the energy
                }
            }

        private:
            std::array<std::array<float, 2>, 3> bands{};
        };

        //==========================================================================
        // Multiplies the signal by a sine: by default a fifth above the note.
        class RingMod : public Module
        {
        public:
            enum { kTrack, kTune, kFreq, kMix };

            void reset() override { phase = 0; }
            void process( const VoiceContext& C, const float* P0, const float* P, float* Buf, int N ) override
            {
                const auto hz = P[kTrack] > 0.5f ? noteToHz( C.note + P[kTune] ) : (double)P[kFreq];
                const auto dt = juce::jlimit( 0.0, 0.45, hz / C.sampleRate );
                Ramp       mix( P0[kMix], P[kMix], N );
                for( int i = 0; i < N; ++i )
                {
                    const auto m       = mix.next();
                    const auto carrier = std::sin( twoPi * (float)phase );
                    Buf[i]             = Buf[i] * ( 1.f - m + m * carrier );
                    phase += dt;
                    if( phase >= 1.0 )
                        phase -= 1.0;
                }
            }

        private:
            double phase{ 0 };
        };

        //==========================================================================
        class Vca : public Module
        {
        public:
            enum { kGain };

            void process( const VoiceContext&, const float* P0, const float* P, float* Buf, int N ) override
            {
                Ramp gain( P0[kGain], P[kGain], N );
                for( int i = 0; i < N; ++i )
                    Buf[i] *= gain.next();
            }
        };

        //==========================================================================
        class Envelope : public Module
        {
        public:
            enum { kAttack, kDecay, kSustain, kRelease, kVelocity };

            bool isEnvelope() const override { return true; }
            bool isActive() const override { return stage != Stage::Idle; }

            void reset() override
            {
                stage = Stage::Idle;
                level = 0;
            }
            void noteOn( const VoiceContext& C, bool ) override
            {
                stage    = Stage::Attack; // from the current level, so a steal does not click
                velocity = C.velocity;
            }
            void noteOff() override
            {
                if( stage != Stage::Idle )
                    stage = Stage::Release;
            }

            float modulate( const VoiceContext& C, const float* P, int N ) override
            {
                const auto sr = (float)C.sampleRate;
                // Exponential segments reach ~0.1% of the distance in the set time.
                auto coef = [&]( float Seconds ) { return std::exp( -6.9f * (float)N / juce::jmax( 1.f, Seconds * sr ) ); };

                if( N > 0 )
                {
                    switch( stage )
                    {
                        case Stage::Idle: break;
                        case Stage::Attack:
                            level += (float)N / juce::jmax( 1.f, P[kAttack] * sr );
                            if( level >= 1.f )
                            {
                                level = 1.f;
                                stage = Stage::Decay;
                            }
                            break;
                        case Stage::Decay:
                            level = P[kSustain] + ( level - P[kSustain] ) * coef( P[kDecay] );
                            if( std::abs( level - P[kSustain] ) < 1.0e-4f )
                                stage = Stage::Sustain;
                            break;
                        case Stage::Sustain: level = P[kSustain]; break;
                        case Stage::Release:
                            level *= coef( P[kRelease] );
                            if( level < 1.0e-4f )
                                reset();
                            break;
                    }
                }
                return level * ( 1.f - P[kVelocity] + P[kVelocity] * velocity );
            }

        private:
            enum class Stage { Idle, Attack, Decay, Sustain, Release };
            Stage stage{ Stage::Idle };
            float level{ 0 }, velocity{ 1 };
        };

        //==========================================================================
        class Lfo : public Module
        {
        public:
            enum { kShape, kRate, kRetrigger, kPolarity, kAmount, kSync };
            enum Shape { Sine, Triangle, SawUp, Square, Random };

            void reset() override
            {
                phase = 0;
                held  = rng.bipolar();
            }
            void noteOn( const VoiceContext&, bool ) override { retrigger = true; }

            float modulate( const VoiceContext& C, const float* P, int N ) override
            {
                if( retrigger )
                {
                    retrigger = false;
                    if( P[kRetrigger] > 0.5f )
                    {
                        phase = 0;
                        held  = rng.bipolar();
                    }
                }

                const auto beats = syncBeats( (int)P[kSync] );
                const auto rate  = beats > 0.0 ? (float)( C.bpm / 60.0 / beats ) : P[kRate];
                phase += rate * (float)N / (float)C.sampleRate;
                if( phase >= 1.f )
                {
                    phase -= std::floor( phase );
                    held = rng.bipolar();
                }

                float v = 0.f;
                switch( (int)P[kShape] )
                {
                    case Sine: v = std::sin( twoPi * phase ); break;
                    case Triangle: v = 1.f - 4.f * std::abs( phase - 0.5f ); break;
                    case SawUp: v = 2.f * phase - 1.f; break;
                    case Square: v = phase < 0.5f ? 1.f : -1.f; break;
                    default: v = held; break;
                }
                return P[kAmount] * ( P[kPolarity] > 0.5f ? 0.5f * ( v + 1.f ) : v );
            }

        private:
            Rng   rng;
            float phase{ 0 }, held{ 0 };
            bool  retrigger{ false };
        };

        //==========================================================================
        class RandomValue : public Module
        {
        public:
            enum { kPolarity };

            void reset() override { value = 0; }
            void noteOn( const VoiceContext&, bool ) override { value = rng.bipolar(); }
            float modulate( const VoiceContext&, const float* P, int ) override
            {
                return P[kPolarity] > 0.5f ? 0.5f * ( value + 1.f ) : value;
            }

        private:
            Rng   rng;
            float value{ 0 };
        };

        //==========================================================================
        class Math : public Module
        {
        public:
            enum { kGain, kOffset, kShape };
            enum Shape { Linear, Rectify, Clip };

            void process( const VoiceContext&, const float* P0, const float* P, float* Buf, int N ) override
            {
                Ramp       gain( P0[kGain], P[kGain], N ), offset( P0[kOffset], P[kOffset], N );
                const auto shape = (int)P[kShape];
                for( int i = 0; i < N; ++i )
                {
                    auto v = Buf[i] * gain.next() + offset.next();
                    if( shape == Rectify )
                        v = std::abs( v );
                    else if( shape == Clip )
                        v = juce::jlimit( -1.f, 1.f, v );
                    Buf[i] = v;
                }
            }
        };

        //==========================================================================
        class Drive : public Module
        {
        public:
            enum { kDrive, kFold, kBias, kLevel };

            void process( const VoiceContext&, const float* P0, const float* P, float* Buf, int N ) override
            {
                const auto gain = juce::Decibels::decibelsToGain( 36.f * P[kDrive] );
                const auto fold = P[kFold], bias = P[kBias];
                // Remove the offset the bias would add to silence.
                const auto dc = ( 1.f - fold ) * std::tanh( bias ) + fold * std::sin( bias );
                Ramp       level( P0[kLevel], P[kLevel], N );
                for( int i = 0; i < N; ++i )
                {
                    const auto x = gain * Buf[i] + bias;
                    Buf[i]       = level.next() * ( ( 1.f - fold ) * std::tanh( x ) + fold * std::sin( x ) - dc );
                }
            }
        };

        //==========================================================================
        class Delay : public Module
        {
        public:
            enum { kTime, kTrack, kTune, kFeedback, kDamp, kMix };

            void prepare( double SampleRate ) override
            {
                buffer.assign( (size_t)( SampleRate * maxSeconds ) + 4, 0.f );
                reset();
            }
            void reset() override
            {
                std::fill( buffer.begin(), buffer.end(), 0.f );
                write = 0;
                lp    = 0;
            }

            void process( const VoiceContext& C, const float* P0, const float* P, float* Buf, int N ) override
            {
                const auto size = (int)buffer.size();
                if( size < 4 )
                    return;

                // Track = a comb tuned to the note: Karplus-Strong strings and resonators.
                const auto sr    = (float)C.sampleRate;
                auto       delay = P[kTrack] > 0.5f ? sr / (float)noteToHz( C.note + P[kTune] ) : P[kTime] * 0.001f * sr;
                delay            = juce::jlimit( 1.f, (float)( size - 2 ), delay );

                const auto fb   = P[kFeedback];
                const auto damp = 1.f - 0.95f * P[kDamp];
                Ramp       mix( P0[kMix], P[kMix], N );

                for( int i = 0; i < N; ++i )
                {
                    auto read = (float)write - delay;
                    if( read < 0.f )
                        read += (float)size;
                    const auto i0   = (int)read;
                    const auto frac = read - (float)i0;
                    const auto i1   = i0 + 1 < size ? i0 + 1 : 0;
                    const auto y    = buffer[(size_t)i0] + frac * ( buffer[(size_t)i1] - buffer[(size_t)i0] );

                    lp += ( y - lp ) * damp;
                    buffer[(size_t)write] = Buf[i] + fb * lp;
                    if( ++write >= size )
                        write = 0;

                    const auto m = mix.next();
                    Buf[i]       = Buf[i] * ( 1.f - m ) + y * m;
                }
            }

        private:
            static constexpr double maxSeconds = 1.0;
            std::vector<float>      buffer;
            int                     write{ 0 };
            float                   lp{ 0 };
        };

        //==========================================================================
        class Crush : public Module
        {
        public:
            enum { kBits, kRate, kMix };

            void reset() override
            {
                held  = 0;
                count = 0;
            }
            void process( const VoiceContext&, const float* P0, const float* P, float* Buf, int N ) override
            {
                const auto steps = std::exp2( P[kBits] - 1.f );
                const auto hold  = juce::jmax( 1, (int)P[kRate] );
                Ramp       mix( P0[kMix], P[kMix], N );
                for( int i = 0; i < N; ++i )
                {
                    if( ++count >= hold )
                    {
                        count = 0;
                        held  = std::round( Buf[i] * steps ) / steps;
                    }
                    const auto m = mix.next();
                    Buf[i]       = Buf[i] * ( 1.f - m ) + held * m;
                }
            }

        private:
            float held{ 0 };
            int   count{ 0 };
        };

        //==========================================================================
        // Pan only places its column; the engine reads its value after the column runs.
        class Pan : public Module
        {
        };

        //==========================================================================
        // Effects: stereo, one instance each, after the voices are mixed.

        // Linear-interpolated read D samples behind Write in a circular buffer.
        inline float readDelay( const std::vector<float>& Buf, int Write, float D )
        {
            const auto size = (int)Buf.size();
            auto       r    = (float)Write - D;
            while( r < 0.f )
                r += (float)size;
            const auto i0   = (int)r % size;
            const auto frac = r - std::floor( r );
            const auto i1   = i0 + 1 < size ? i0 + 1 : 0;
            return Buf[(size_t)i0] + frac * ( Buf[(size_t)i1] - Buf[(size_t)i0] );
        }

        class StereoDelay : public Module
        {
        public:
            enum { kTime, kFeedback, kPingPong, kDamp, kMix, kSync };

            void prepare( double SampleRate ) override
            {
                sampleRate = SampleRate;
                left.assign( (size_t)( SampleRate * 2.0 ) + 4, 0.f );
                right = left;
                reset();
            }
            void reset() override
            {
                std::fill( left.begin(), left.end(), 0.f );
                std::fill( right.begin(), right.end(), 0.f );
                write = 0;
                lpL = lpR = 0;
            }

            void processStereo( const VoiceContext& C, const float* P, float* L, float* R, int N ) override
            {
                const auto size = (int)left.size();
                if( size < 4 )
                    return;
                const auto beats   = syncBeats( (int)P[kSync] );
                const auto seconds = beats > 0.0 ? (float)( 60.0 / C.bpm * beats ) : P[kTime] * 0.001f;
                const auto d       = juce::jlimit( 1.f, (float)size - 2.f, seconds * (float)sampleRate );
                const auto fb   = P[kFeedback];
                const auto damp = 1.f - 0.9f * P[kDamp];
                const auto mix  = P[kMix];
                const bool ping = P[kPingPong] > 0.5f;

                for( int i = 0; i < N; ++i )
                {
                    const auto yl = readDelay( left, write, d ), yr = readDelay( right, write, d );
                    lpL += ( yl - lpL ) * damp;
                    lpR += ( yr - lpR ) * damp;
                    if( ping ) // the mono input enters on the left and bounces across
                    {
                        left[(size_t)write]  = 0.5f * ( L[i] + R[i] ) + fb * lpR;
                        right[(size_t)write] = fb * lpL;
                    }
                    else
                    {
                        left[(size_t)write]  = L[i] + fb * lpL;
                        right[(size_t)write] = R[i] + fb * lpR;
                    }
                    if( ++write >= size )
                        write = 0;
                    L[i] = L[i] * ( 1.f - mix ) + yl * mix;
                    R[i] = R[i] * ( 1.f - mix ) + yr * mix;
                }
            }

        private:
            double             sampleRate{ 48000 };
            std::vector<float> left, right;
            int                write{ 0 };
            float              lpL{ 0 }, lpR{ 0 };
        };

        class Chorus : public Module
        {
        public:
            enum { kRate, kDepth, kMix };

            void prepare( double SampleRate ) override
            {
                sampleRate = SampleRate;
                left.assign( (size_t)( SampleRate * 0.05 ) + 4, 0.f );
                right = left;
                reset();
            }
            void reset() override
            {
                std::fill( left.begin(), left.end(), 0.f );
                std::fill( right.begin(), right.end(), 0.f );
                write = 0;
                phase = 0;
            }

            void processStereo( const VoiceContext&, const float* P, float* L, float* R, int N ) override
            {
                const auto size = (int)left.size();
                if( size < 4 )
                    return;
                const auto msToSamples = (float)sampleRate * 0.001f;
                const auto inc         = P[kRate] / (float)sampleRate;
                const auto depth       = 6.f * P[kDepth];
                const auto mix         = P[kMix];

                for( int i = 0; i < N; ++i )
                {
                    left[(size_t)write]  = L[i];
                    right[(size_t)write] = R[i];
                    // Two taps a quarter cycle apart, 12 ms +- up to 6 ms.
                    const auto dl = ( 12.f + depth * std::sin( twoPi * phase ) ) * msToSamples;
                    const auto dr = ( 12.f + depth * std::sin( twoPi * ( phase + 0.25f ) ) ) * msToSamples;
                    const auto yl = readDelay( left, write, dl ), yr = readDelay( right, write, dr );
                    if( ++write >= size )
                        write = 0;
                    phase += inc;
                    if( phase >= 1.f )
                        phase -= 1.f;
                    L[i] = L[i] * ( 1.f - 0.5f * mix ) + yl * mix * 0.7f;
                    R[i] = R[i] * ( 1.f - 0.5f * mix ) + yr * mix * 0.7f;
                }
            }

        private:
            double             sampleRate{ 48000 };
            std::vector<float> left, right;
            int                write{ 0 };
            float              phase{ 0 };
        };

        class Reverb : public Module
        {
        public:
            enum { kSize, kDamp, kWidth, kMix };

            void prepare( double SampleRate ) override
            {
                reverb.setSampleRate( SampleRate );
                reverb.reset();
            }
            void reset() override { reverb.reset(); }

            void processStereo( const VoiceContext&, const float* P, float* L, float* R, int N ) override
            {
                juce::Reverb::Parameters p;
                p.roomSize = P[kSize];
                p.damping  = P[kDamp];
                p.width    = P[kWidth];
                p.wetLevel = 0.5f * P[kMix];          // juce scales wet by 3
                p.dryLevel = 0.5f * ( 1.f - P[kMix] ); // and dry by 2
                reverb.setParameters( p );             // smoothed inside
                reverb.processStereo( L, R, N );
            }

        private:
            juce::Reverb reverb;
        };

        class Saturator : public Module
        {
        public:
            enum { kDrive, kTone, kLevel };

            void prepare( double SampleRate ) override { sampleRate = SampleRate; }
            void reset() override { lpL = lpR = 0; }

            void processStereo( const VoiceContext&, const float* P, float* L, float* R, int N ) override
            {
                const auto gain  = juce::Decibels::decibelsToGain( 30.f * P[kDrive] );
                const auto norm  = 1.f / std::tanh( gain ); // a full-scale input stays full scale
                const auto coef  = 1.f - std::exp( -twoPi * P[kTone] / (float)sampleRate );
                const auto level = P[kLevel];
                for( int i = 0; i < N; ++i )
                {
                    lpL += ( std::tanh( gain * L[i] ) * norm - lpL ) * coef;
                    lpR += ( std::tanh( gain * R[i] ) * norm - lpR ) * coef;
                    L[i] = lpL * level;
                    R[i] = lpR * level;
                }
            }

        private:
            double sampleRate{ 48000 };
            float  lpL{ 0 }, lpR{ 0 };
        };

        template <typename T>
        std::function<std::unique_ptr<Module>()> maker()
        {
            return []() { return std::make_unique<T>(); };
        }
    } // namespace

    const std::vector<ModuleSpec>& getModuleSpecs()
    {
        static const std::vector<ModuleSpec> specs = {
            { "osc", "Oscillator", Role::Source,
              {
                  { "wave", "Wave", "", 0, 3, 2, Curve::Steps, { "Sine", "Triangle", "Saw", "Square" } },
                  { "tune", "Tune", "st", -48, 48, 0 },
                  { "fine", "Fine", "ct", -100, 100, 0 },
                  { "pw", "Pulse Width", "", 0.05f, 0.95f, 0.5f },
                  { "level", "Level", "", 0, 1, 0.5f },
                  { "input", "Input", "", 0, 2, 0, Curve::Steps, { "Add", "Phase Mod", "Replace" } },
                  { "pm", "PM Depth", "", 0, 4, 1 },
                  { "unison", "Unison", "", 1, 7, 1, Curve::Steps },
                  { "spread", "Spread", "ct", 0, 100, 20 },
              },
              maker<Oscillator>() },
            { "noise", "Noise", Role::Source,
              {
                  { "color", "Colour", "", 0, 1, 0 },
                  { "level", "Level", "", 0, 1, 0.5f },
              },
              maker<Noise>() },
            { "filter", "Filter", Role::Processor,
              {
                  { "mode", "Mode", "", 0, 4, 0, Curve::Steps, { "Low Pass", "Band Pass", "High Pass", "Notch", "Ladder LP" } },
                  { "cutoff", "Cutoff", "Hz", 20, 20000, 2000, Curve::Log },
                  { "reso", "Resonance", "", 0, 1, 0.2f },
                  { "track", "Key Track", "", 0, 1, 0 },
              },
              maker<Filter>() },
            { "formant", "Formant", Role::Processor,
              {
                  { "vowel", "Vowel", "", 0, 4, 0 }, // 0 A, 1 E, 2 I, 3 O, 4 U; in between morphs
                  { "shift", "Shift", "st", -12, 12, 0 },
                  { "reso", "Resonance", "", 0, 1, 0.5f },
                  { "mix", "Mix", "", 0, 1, 1 },
              },
              maker<Formant>() },
            { "ring", "Ring Mod", Role::Processor,
              {
                  { "track", "Key Track", "", 0, 1, 1, Curve::Steps, { "Off", "On" } },
                  { "tune", "Tune", "st", -24, 48, 7 },
                  { "freq", "Freq", "Hz", 20, 5000, 440, Curve::Log }, // when Key Track is off
                  { "mix", "Mix", "", 0, 1, 1 },
              },
              maker<RingMod>() },
            { "vca", "VCA", Role::Processor,
              {
                  { "gain", "Gain", "", 0, 1, 1 },
              },
              maker<Vca>() },
            { "adsr", "Envelope", Role::Modulator,
              {
                  { "attack", "Attack", "s", 0.001f, 10, 0.005f, Curve::Log },
                  { "decay", "Decay", "s", 0.001f, 10, 0.3f, Curve::Log },
                  { "sustain", "Sustain", "", 0, 1, 0.7f },
                  { "release", "Release", "s", 0.001f, 10, 0.3f, Curve::Log },
                  { "velocity", "Velocity", "", 0, 1, 0.5f },
              },
              maker<Envelope>() },
            { "lfo", "LFO", Role::Modulator,
              {
                  { "shape", "Shape", "", 0, 4, 0, Curve::Steps, { "Sine", "Triangle", "Saw", "Square", "Random" } },
                  { "rate", "Rate", "Hz", 0.02f, 40, 2, Curve::Log },
                  { "retrig", "Retrigger", "", 0, 1, 1, Curve::Steps, { "Off", "On" } },
                  { "polarity", "Polarity", "", 0, 1, 0, Curve::Steps, { "Bipolar", "Unipolar" } },
                  { "amount", "Amount", "", 0, 1, 1 }, // modulate it to fade the LFO in (pressure, envelope)
                  { "sync", "Sync", "", 0, 13, 0, Curve::Steps, syncChoices() },
              },
              maker<Lfo>() },
            { "random", "Random", Role::Modulator,
              {
                  { "polarity", "Polarity", "", 0, 1, 0, Curve::Steps, { "Bipolar", "Unipolar" } },
              },
              maker<RandomValue>() },
            { "math", "Math", Role::Processor,
              {
                  { "gain", "Gain", "", -4, 4, 1 },
                  { "offset", "Offset", "", -1, 1, 0 },
                  { "shape", "Shape", "", 0, 2, 0, Curve::Steps, { "Linear", "Rectify", "Clip" } },
              },
              maker<Math>() },
            { "drive", "Drive", Role::Processor,
              {
                  { "drive", "Drive", "", 0, 1, 0.3f },
                  { "fold", "Fold", "", 0, 1, 0 },
                  { "bias", "Bias", "", -1, 1, 0 },
                  { "level", "Level", "", 0, 1, 0.7f },
              },
              maker<Drive>() },
            { "delay", "Delay", Role::Processor,
              {
                  { "time", "Time", "ms", 0.1f, 1000, 250, Curve::Log },
                  { "track", "Key Track", "", 0, 1, 0, Curve::Steps, { "Off", "On" } },
                  { "tune", "Tune", "st", -24, 24, 0 },
                  { "feedback", "Feedback", "", -0.99f, 0.99f, 0.5f },
                  { "damp", "Damping", "", 0, 1, 0.2f },
                  { "mix", "Mix", "", 0, 1, 0.5f },
              },
              maker<Delay>() },
            { "crush", "Crusher", Role::Processor,
              {
                  { "bits", "Bits", "", 1, 16, 8, Curve::Steps },
                  { "rate", "Downsample", "", 1, 32, 1, Curve::Steps },
                  { "mix", "Mix", "", 0, 1, 1 },
              },
              maker<Crush>() },
            { "pan", "Pan", Role::Processor,
              {
                  { "pan", "Pan", "", -1, 1, 0 },
              },
              maker<Pan>() },
        };
        return specs;
    }

    const std::vector<ModuleSpec>& getEffectSpecs()
    {
        static const std::vector<ModuleSpec> specs = {
            { "fxdelay", "Stereo Delay", Role::Effect,
              {
                  { "time", "Time", "ms", 10, 2000, 375, Curve::Log },
                  { "feedback", "Feedback", "", 0, 0.95f, 0.4f },
                  { "pingpong", "Ping-Pong", "", 0, 1, 1, Curve::Steps, { "Off", "On" } },
                  { "damp", "Damping", "", 0, 1, 0.3f },
                  { "mix", "Mix", "", 0, 1, 0.3f },
                  { "sync", "Sync", "", 0, 13, 0, Curve::Steps, syncChoices() }, // the longest echo is 2 s
              },
              maker<StereoDelay>() },
            { "chorus", "Chorus", Role::Effect,
              {
                  { "rate", "Rate", "Hz", 0.05f, 5, 0.4f, Curve::Log },
                  { "depth", "Depth", "", 0, 1, 0.5f },
                  { "mix", "Mix", "", 0, 1, 0.5f },
              },
              maker<Chorus>() },
            { "reverb", "Reverb", Role::Effect,
              {
                  { "size", "Size", "", 0, 1, 0.6f },
                  { "damp", "Damping", "", 0, 1, 0.4f },
                  { "width", "Width", "", 0, 1, 1 },
                  { "mix", "Mix", "", 0, 1, 0.25f },
              },
              maker<Reverb>() },
            { "fxdrive", "Saturator", Role::Effect,
              {
                  { "drive", "Drive", "", 0, 1, 0.3f },
                  { "tone", "Tone", "Hz", 200, 20000, 8000, Curve::Log },
                  { "level", "Level", "", 0, 1, 0.8f },
              },
              maker<Saturator>() },
        };
        return specs;
    }

    const std::vector<juce::String>& syncChoices()
    {
        static const std::vector<juce::String> choices{ "Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/4 T", "1/8 T", "1/16 T", "1/4 D", "1/8 D" };
        return choices;
    }

    double syncBeats( int Choice )
    {
        static constexpr std::array<double, 14> beats{ 0, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125, 2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0, 1.5, 0.75 };
        return juce::isPositiveAndBelow( Choice, (int)beats.size() ) ? beats[(size_t)Choice] : 0.0;
    }

    const ModuleSpec* findModuleSpec( const juce::String& Type )
    {
        for( auto* list : { &getModuleSpecs(), &getEffectSpecs() } )
            for( auto& s : *list )
                if( s.type == Type )
                    return &s;
        return nullptr;
    }

    juce::String builtinSourceId( int Source )
    {
        switch( Source )
        {
            case kVelocity: return "velocity";
            case kNote: return "note";
            case kGate: return "gate";
            case kModWheel: return "modwheel";
            case kBend: return "bend";
            case kPressure: return "pressure";
            default: break;
        }
        if( Source >= kMacro1 && Source < kNumBuiltinSources )
            return "macro" + juce::String( Source - kMacro1 + 1 );
        return {};
    }

    int findBuiltinSource( const juce::String& Id )
    {
        for( int s = 0; s < kNumBuiltinSources; ++s )
            if( builtinSourceId( s ) == Id )
                return s;
        return -1;
    }
} // namespace mt::modular

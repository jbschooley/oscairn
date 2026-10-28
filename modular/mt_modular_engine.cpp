/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt
{
    using namespace modular;

    namespace
    {
        constexpr float outputHeadroom = 0.3f;   // several voices at full level still fit
        constexpr float endFadeSeconds = 0.005f; // when a voice's last envelope ends
        constexpr float bendRange      = 2.f;    // semitones = bend source 1.0

        /// Per-block snapshot of the values every voice shares.
        struct Globals
        {
            float                bendSemitones{ 0 };
            float                modWheel{ 0 };
            std::array<float, 8> macros{};
            double               bpm{ 120 };
        };

        /// Calls F( index, parameter index, source index, depth ) for every usable
        /// modulation, in patch order: modules first (index into D.modules), then
        /// effects (index D.modules.size() + effect). Mods naming an unknown
        /// parameter or an unusable source are ignored; effects only take sources
        /// that are the same for every voice.
        template <typename Fn>
        void forEachRoute( const PatchDesc& D, Fn&& F )
        {
            const auto numModules = (int)D.modules.size();
            for( int i = 0; i < numModules + (int)D.effects.size(); ++i )
            {
                const bool  isEffect = i >= numModules;
                const auto& node     = isEffect ? D.effects[(size_t)( i - numModules )] : D.modules[(size_t)i];
                for( const auto& m : node.mods )
                {
                    const auto p = node.spec->findParam( m.param );
                    if( p < 0 )
                        continue;

                    auto source = findBuiltinSource( m.source );
                    if( isEffect && source >= 0 && !isGlobalSource( source ) )
                        source = -1;
                    if( source < 0 && !isEffect && m.source.startsWith( "module:" ) )
                    {
                        const auto n = D.indexOf( m.source.fromFirstOccurrenceOf( ":", false, false ).getIntValue() );
                        if( n >= 0 && D.modules[(size_t)n].spec->role == Role::Modulator )
                            source = kNumBuiltinSources + n;
                    }
                    if( source >= 0 )
                        F( i, p, source, m.depth );
                }
            }
        }

        /// Calls F( target module index, source module index, amount ) for every usable
        /// cable, in patch order: both ends must be audio modules, and not the same one.
        template <typename Fn>
        void forEachCable( const PatchDesc& D, Fn&& F )
        {
            auto isAudio = [&D]( int I ) { return I >= 0 && D.modules[(size_t)I].spec->role != Role::Modulator; };
            for( int i = 0; i < (int)D.modules.size(); ++i )
                for( const auto& c : D.modules[(size_t)i].cables )
                {
                    const auto n = c.source.startsWith( "module:" ) ? D.indexOf( c.source.fromFirstOccurrenceOf( ":", false, false ).getIntValue() ) : -1;
                    if( n != i && isAudio( n ) && isAudio( i ) )
                        F( i, n, c.amount );
                }
        }
    } // namespace

    //==============================================================================
    struct ModularEngine::Graph
    {
        struct Node
        {
            const ModuleSpec* spec;
            int               firstParam; // into the flat parameter list
        };

        struct Route
        {
            int source;
            int depthIndex;
        };

        struct Column
        {
            std::vector<int> nodes; // sources and processors, top to bottom
            int              panNode{ -1 };
        };

        struct Cable
        {
            int sourceNode;
            int amountIndex;
        };

        struct Effect
        {
            const ModuleSpec*       spec;
            int                     firstParam;
            std::unique_ptr<Module> processor; // one, shared by all voices
        };

        struct Voice
        {
            std::vector<std::unique_ptr<Module>> modules; // one per node
            std::vector<float>                   cur, prev; // parameter values now / one sub-block ago
            std::vector<float>                   src;       // built-in sources, then one per node
            std::vector<std::array<float, subBlock>> outputs; // each node's last sub-block, for cables
            int                                  note{ 60 };
            float                                pitch{ 60 }, target{ 60 }; // pitch glides to target
            float                                velocity{ 0 };
            bool                                 gate{ false }, active{ false }, fading{ false };
            float                                fade{ 1 };
            uint64_t                             age{ 0 };
        };

        // Fixed once built.
        juce::String                  key;
        double                        sampleRate{ 48000 };
        std::vector<Node>             nodes;
        std::vector<int>              modulators, envelopes;
        std::vector<Column>           columns;
        std::vector<const ParamSpec*> paramSpecs;
        std::vector<int>              routeBegin; // per parameter, plus an end marker
        std::vector<Route>            routes;
        std::vector<int>              modulated; // module parameters with at least one route
        std::vector<std::vector<Cable>> cables;  // per node: the cables into it
        std::vector<char>             feedsCable; // per node: its output is a cable's source
        std::vector<Effect>           effects;   // slot order; their parameters follow the modules'
        int                           numModuleParams{ 0 };

        // Written by the message thread for value-only edits.
        std::unique_ptr<std::atomic<float>[]> base;   // normalised knob positions
        std::unique_ptr<std::atomic<float>[]> depth;  // per route, in patch order
        std::unique_ptr<std::atomic<float>[]> amount; // per cable, in patch order

        std::atomic<int>   playMode{ Poly };
        std::atomic<float> glide{ 0 };
        std::atomic<int>   arpMode{ ArpOff }, arpRate{ 2 }, arpOctaves{ 1 };
        std::atomic<float> arpGate{ 0.5f };

        // Ids of the nodes, then the effects, for the editor's activity meters.
        std::vector<int>                      partIds;
        std::unique_ptr<std::atomic<float>[]> activity;

        // Audio thread.
        struct Held
        {
            int   note;
            float velocity;
        };
        std::array<Held, 32> held{}; // mono modes: notes held down, oldest first
        int                  numHeld{ 0 };
        std::array<float, 128> inputHeld{}; // physical keys, before arpeggiation

        // Arpeggiator: the notes held down (in the order played), and its clock.
        std::array<Held, 32> arpHeld{};
        int                  numArpHeld{ 0 };
        int                  arpStep{ 0 };
        double               arpCountdown{ 0 }; // samples to the next step
        double               arpGateLeft{ 0 };  // samples until the sounding note is released
        int                  arpSounding{ -1 };
        bool                 arpStarting{ false }; // first step due at the start of the next block
        uint32_t             arpRandom{ 0x2545F491u };
        std::vector<float>   peaks;  // per part, this block
        std::vector<float> baseNorm, baseReal, depthNow, amountNow;
        std::vector<float> effectValues; // indexed like the flat parameter list
        std::vector<Voice> voices;
        uint64_t           noteCounter{ 0 };

        int numParams() const { return (int)paramSpecs.size(); }

        void prepare( double SampleRate )
        {
            numHeld = 0;
            resetArp();
            inputHeld.fill( 0.f );
            sampleRate = SampleRate;
            for( auto& v : voices )
            {
                for( auto& m : v.modules )
                    m->prepare( SampleRate );
                freeVoice( v );
            }
            for( auto& e : effects )
                e.processor->prepare( SampleRate );
        }

        void resetEffects()
        {
            for( auto& e : effects )
                e.processor->reset();
        }

        void refreshValues()
        {
            for( int p = 0; p < numParams(); ++p )
            {
                baseNorm[(size_t)p] = base[(size_t)p].load( std::memory_order_relaxed );
                baseReal[(size_t)p] = paramSpecs[(size_t)p]->fromNormal( baseNorm[(size_t)p] );
            }
            for( size_t r = 0; r < depthNow.size(); ++r )
                depthNow[r] = depth[r].load( std::memory_order_relaxed );
            for( size_t c = 0; c < amountNow.size(); ++c )
                amountNow[c] = amount[c].load( std::memory_order_relaxed );
        }

        VoiceContext context( const Voice& V, const Globals& G ) const
        {
            return { sampleRate, V.pitch + G.bendSemitones, V.velocity, V.gate, G.bpm };
        }

        void setGlobalSources( Voice& V, const Globals& G ) const
        {
            V.src[kModWheel] = G.modWheel;
            V.src[kBend]     = juce::jlimit( -1.f, 1.f, G.bendSemitones / bendRange );
            for( int m = 0; m < 8; ++m )
                V.src[(size_t)( modular::kMacro1 + m )] = G.macros[(size_t)m];
        }

        void evaluate( Voice& V ) const
        {
            std::copy( baseReal.begin(), baseReal.end(), V.cur.begin() );
            for( auto p : modulated )
            {
                auto n = baseNorm[(size_t)p];
                for( int r = routeBegin[(size_t)p]; r < routeBegin[(size_t)p + 1]; ++r )
                    n += depthNow[(size_t)routes[(size_t)r].depthIndex] * V.src[(size_t)routes[(size_t)r].source];
                V.cur[(size_t)p] = paramSpecs[(size_t)p]->fromNormal( n );
            }
        }

        void runModulators( Voice& V, const VoiceContext& C, int N )
        {
            for( auto n : modulators )
                V.src[(size_t)( kNumBuiltinSources + n )] = V.modules[(size_t)n]->modulate( C, &V.cur[(size_t)nodes[(size_t)n].firstParam], N );
        }

        void startNote( int Note, float Velocity, const Globals& G )
        {
            if( !juce::isPositiveAndBelow( Note, 128 ) ) return;
            inputHeld[(size_t)Note] = Velocity;
            if( arpMode.load( std::memory_order_relaxed ) != ArpOff )
                arpNoteOn( Note, Velocity, G );
            else
                playNote( Note, Velocity, G );
        }

        void releaseNote( int Note, const Globals& G )
        {
            if( !juce::isPositiveAndBelow( Note, 128 ) ) return;
            inputHeld[(size_t)Note] = 0.f;
            if( arpMode.load( std::memory_order_relaxed ) != ArpOff || numArpHeld > 0 )
                arpNoteOff( Note, G );
            else
                stopNote( Note, G );
        }

        /// A note into the voices (from a key, or from the arpeggiator).
        void playNote( int Note, float Velocity, const Globals& G )
        {
            if( voices.empty() )
                return;
            if( const auto mode = playMode.load( std::memory_order_relaxed ); mode != Poly )
            {
                monoNoteOn( Note, Velocity, G, mode == Legato );
                return;
            }

            // A free voice, else the oldest released one, else the oldest.
            Voice* pick = nullptr;
            for( auto& v : voices )
                if( !v.active )
                {
                    pick = &v;
                    break;
                }
            if( !pick )
                for( auto& v : voices )
                    if( !v.gate && ( !pick || v.age < pick->age ) )
                        pick = &v;
            if( !pick )
                for( auto& v : voices )
                    if( !pick || v.age < pick->age )
                        pick = &v;

            startVoice( *pick, Note, Velocity, pick->active, G );
        }

        void startVoice( Voice& v, int Note, float Velocity, bool Steal, const Globals& G )
        {
            const bool steal = Steal;
            // Glide only between notes of one mono voice; a fresh note starts on pitch.
            if( !steal || playMode.load( std::memory_order_relaxed ) == Poly || glide.load( std::memory_order_relaxed ) <= 0.f )
                v.pitch = (float)Note;
            v.target         = (float)Note;
            v.note           = Note;
            v.velocity       = Velocity;
            v.gate           = true;
            v.active         = true;
            v.fading         = false;
            v.fade           = 1.f;
            v.age            = ++noteCounter;
            v.src[kVelocity] = Velocity;
            v.src[kNote]     = juce::jlimit( -1.f, 1.f, ( (float)Note - 60.f ) / 60.f );
            v.src[kGate]     = 1.f;
            v.src[kPressure] = 0.f;
            setGlobalSources( v, G );
            if( !steal )
                std::copy( baseReal.begin(), baseReal.end(), v.cur.begin() );

            const auto ctx = context( v, G );
            for( auto& m : v.modules )
                m->noteOn( ctx, steal );
            runModulators( v, ctx, 0 ); // starting values, so the first sub-block does not ramp from stale ones
            evaluate( v );
            v.prev = v.cur;
        }

        void stopNote( int Note, const Globals& G )
        {
            if( playMode.load( std::memory_order_relaxed ) != Poly && !voices.empty() )
            {
                monoNoteOff( Note, G, playMode.load( std::memory_order_relaxed ) == Legato );
                return;
            }
            for( auto& v : voices )
                if( v.active && v.gate && v.note == Note )
                    releaseVoice( v );
        }

        //------------------------------------------------------------------------------
        // Mono and legato: one voice, the last note held wins; letting it go returns to
        // the one held before. Mono retriggers the envelopes on every change, legato
        // only when no other note was held.
        void monoNoteOn( int Note, float Velocity, const Globals& G, bool IsLegato )
        {
            removeHeld( Note );
            if( numHeld == (int)held.size() ) // drop the oldest
                std::move( held.begin() + 1, held.end(), held.begin() ), --numHeld;
            held[(size_t)numHeld++] = { Note, Velocity };

            auto& v = voices[0];
            if( IsLegato && v.active && v.gate )
                retarget( v, Note );
            else
                startVoice( v, Note, Velocity, v.active, G );
        }

        void monoNoteOff( int Note, const Globals& G, bool IsLegato )
        {
            removeHeld( Note );
            auto& v = voices[0];
            if( !v.active || !v.gate || v.note != Note )
                return; // a note underneath was let go
            if( numHeld == 0 )
            {
                releaseVoice( v );
                return;
            }
            const auto back = held[(size_t)numHeld - 1];
            if( IsLegato )
                retarget( v, back.note );
            else
                startVoice( v, back.note, back.velocity, true, G );
        }

        void retarget( Voice& V, int Note )
        {
            V.note       = Note;
            V.target     = (float)Note;
            V.src[kNote] = juce::jlimit( -1.f, 1.f, ( (float)Note - 60.f ) / 60.f );
            if( glide.load( std::memory_order_relaxed ) <= 0.f )
                V.pitch = (float)Note;
        }

        void removeHeld( int Note )
        {
            const auto end = std::remove_if( held.begin(), held.begin() + numHeld, [Note]( const Held& H ) { return H.note == Note; } );
            numHeld        = (int)( end - held.begin() );
        }

        void aftertouch( int Note, float Amount )
        {
            for( auto& v : voices )
                if( v.active && v.note == Note )
                    v.src[kPressure] = Amount;
        }

        void channelPressure( float Amount )
        {
            for( auto& v : voices )
                if( v.active )
                    v.src[kPressure] = Amount;
        }

        //------------------------------------------------------------------------------
        // Arpeggiator.
        void arpNoteOn( int Note, float Velocity, const Globals& G )
        {
            const bool starting = numArpHeld == 0;
            arpRemove( Note );
            if( numArpHeld == (int)arpHeld.size() )
                std::move( arpHeld.begin() + 1, arpHeld.end(), arpHeld.begin() ), --numArpHeld;
            arpHeld[(size_t)numArpHeld++] = { Note, Velocity };
            // The first step waits for the next block to start, so a chord's notes (which
            // arrive together, before that block) are all in it; the clock starts from it.
            if( starting )
            {
                arpStep     = 0;
                arpCountdown = 0.0;
                arpStarting = true;
            }
            juce::ignoreUnused( G );
        }

        void arpNoteOff( int Note, const Globals& G )
        {
            arpRemove( Note );
            if( numArpHeld == 0 )
                arpStop( G );
        }

        void arpRemove( int Note )
        {
            const auto end = std::remove_if( arpHeld.begin(), arpHeld.begin() + numArpHeld, [Note]( const Held& H ) { return H.note == Note; } );
            numArpHeld     = (int)( end - arpHeld.begin() );
        }

        void arpStop( const Globals& G )
        {
            if( arpSounding >= 0 )
                stopNote( arpSounding, G );
            arpSounding = -1;
            numArpHeld  = 0;
        }

        double arpStepSamples( const Globals& G ) const
        {
            return juce::jmax( 1.0, 60.0 / juce::jmax( 1.0, G.bpm ) * arpRateBeats( arpRate.load( std::memory_order_relaxed ) ) * sampleRate );
        }

        Held arpNext()
        {
            // The pattern: held notes (sorted, unless "As Played") repeated per octave.
            std::array<Held, 32> notes{};
            std::copy( arpHeld.begin(), arpHeld.begin() + numArpHeld, notes.begin() );
            const auto mode = arpMode.load( std::memory_order_relaxed );
            if( mode != ArpAsPlayed )
                std::sort( notes.begin(), notes.begin() + numArpHeld, []( const Held& A, const Held& B ) { return A.note < B.note; } );

            std::array<Held, 128> pattern{};
            int                   n = 0;
            for( int o = 0; o < arpOctaves.load( std::memory_order_relaxed ); ++o )
                for( int i = 0; i < numArpHeld && n < (int)pattern.size(); ++i )
                    if( notes[(size_t)i].note + 12 * o < 128 )
                        pattern[(size_t)n++] = { notes[(size_t)i].note + 12 * o, notes[(size_t)i].velocity };
            if( n == 0 )
                return { -1, 0.f };

            int index = 0;
            switch( mode )
            {
                case ArpDown: index = n - 1 - arpStep % n; break;
                case ArpUpDown:
                {
                    const auto cycle = juce::jmax( 1, 2 * n - 2 ); // no repeated top or bottom note
                    const auto p     = arpStep % cycle;
                    index            = p < n ? p : cycle - p;
                    break;
                }
                case ArpRandom:
                    arpRandom ^= arpRandom << 13;
                    arpRandom ^= arpRandom >> 17;
                    arpRandom ^= arpRandom << 5;
                    index = (int)( arpRandom % (uint32_t)n );
                    break;
                default: index = arpStep % n; break; // Up, As Played
            }
            ++arpStep;
            return pattern[(size_t)juce::jlimit( 0, n - 1, index )];
        }

        void arpStepNow( const Globals& G )
        {
            const auto next = arpNext();
            if( next.note < 0 )
                return;
            const auto previous = arpSounding;
            if( previous == next.note )
                stopNote( previous, G ); // the same note again: a new attack
            // The new note starts before the old one stops, so legato and glide join them.
            playNote( next.note, next.velocity, G );
            if( previous >= 0 && previous != next.note )
                stopNote( previous, G );

            const auto step = arpStepSamples( G );
            arpSounding     = next.note;
            arpGateLeft     = step * arpGate.load( std::memory_order_relaxed );
            arpCountdown    += step;
        }

        /// Samples until the arpeggiator's next event, or N if none comes sooner.
        int arpSamplesToEvent( int N ) const
        {
            double next = N;
            if( numArpHeld > 0 )
                next = juce::jmin( next, arpCountdown );
            if( arpSounding >= 0 && arpGateLeft > 0.0 )
                next = juce::jmin( next, arpGateLeft );
            return juce::jlimit( 1, N, (int)std::ceil( next ) );
        }

        void arpAdvance( int Samples, const Globals& G )
        {
            if( arpMode.load( std::memory_order_relaxed ) == ArpOff )
            {
                if( arpSounding >= 0 || numArpHeld > 0 )
                    arpStop( G ); // switched off while playing
                return;
            }
            if( arpSounding >= 0 && arpGateLeft > 0.0 )
            {
                arpGateLeft -= Samples;
                if( arpGateLeft <= 0.0 && arpGate.load( std::memory_order_relaxed ) < 1.f )
                {
                    stopNote( arpSounding, G );
                    arpSounding = -1;
                }
            }
            if( numArpHeld == 0 )
                return;
            arpCountdown -= Samples;
            if( arpCountdown <= 0.0 )
                arpStepNow( G );
        }

        void resetArp()
        {
            arpCountdown = arpGateLeft = 0.0;
            numArpHeld  = 0;
            arpSounding = -1;
            arpStep     = 0;
            arpStarting = false;
        }

        void releaseAll()
        {
            inputHeld.fill( 0.f );
            numHeld = 0;
            resetArp();
            for( auto& v : voices )
                if( v.active && v.gate )
                    releaseVoice( v );
        }

        void releaseVoice( Voice& V )
        {
            V.gate       = false;
            V.src[kGate] = 0.f;
            for( auto& m : V.modules )
                m->noteOff();
        }

        void freeVoice( Voice& V )
        {
            V.active = V.gate = V.fading = false;
            V.fade                       = 1.f;
            for( auto& m : V.modules )
                m->reset();
            for( auto& o : V.outputs )
                o.fill( 0.f );
        }

        void renderVoice( Voice& V, const Globals& G, float* L, float* R, int N )
        {
            if( V.pitch != V.target )
            {
                // About 99% of the way in the glide time.
                const auto seconds = glide.load( std::memory_order_relaxed );
                V.pitch            = seconds <= 0.f ? V.target : V.target + ( V.pitch - V.target ) * std::exp( -4.6f * (float)N / ( seconds * (float)sampleRate ) );
                if( std::abs( V.pitch - V.target ) < 1.0e-3f )
                    V.pitch = V.target;
            }
            setGlobalSources( V, G );
            const auto ctx = context( V, G );
            runModulators( V, ctx, N );
            std::swap( V.prev, V.cur );
            evaluate( V );

            std::array<float, subBlock> mixL{}, mixR{}, buf;
            for( const auto& c : columns )
            {
                std::fill( buf.begin(), buf.begin() + N, 0.f );
                for( auto n : c.nodes )
                {
                    // Cables add into the input. A source already run this sub-block gives
                    // this sub-block's audio; one further right gives the previous one.
                    for( const auto& cable : cables[(size_t)n] )
                    {
                        const auto& from = V.outputs[(size_t)cable.sourceNode];
                        const auto  amt  = amountNow[(size_t)cable.amountIndex];
                        for( int i = 0; i < N; ++i )
                            buf[(size_t)i] += amt * from[(size_t)i];
                    }

                    const auto first = (size_t)nodes[(size_t)n].firstParam;
                    V.modules[(size_t)n]->process( ctx, &V.prev[first], &V.cur[first], buf.data(), N );
                    if( feedsCable[(size_t)n] )
                    {
                        std::copy( buf.begin(), buf.begin() + N, V.outputs[(size_t)n].begin() );
                        std::fill( V.outputs[(size_t)n].begin() + N, V.outputs[(size_t)n].end(), 0.f );
                    }
                    const auto range = juce::FloatVectorOperations::findMinAndMax( buf.data(), N );
                    peaks[(size_t)n] = juce::jmax( peaks[(size_t)n], -range.getStart(), range.getEnd() );
                }

                // Equal-power pan.
                const auto pan   = c.panNode >= 0 ? V.cur[(size_t)nodes[(size_t)c.panNode].firstParam] : 0.f;
                const auto angle = ( pan + 1.f ) * juce::MathConstants<float>::pi * 0.25f;
                const auto gl = std::cos( angle ), gr = std::sin( angle );
                for( int i = 0; i < N; ++i )
                {
                    mixL[(size_t)i] += buf[(size_t)i] * gl;
                    mixR[(size_t)i] += buf[(size_t)i] * gr;
                }
            }

            // A runaway patch (NaN/inf) loses its voice rather than poisoning the mix.
            if( std::any_of( mixL.begin(), mixL.begin() + N, []( float x ) { return !std::isfinite( x ) || std::abs( x ) > 32.f; } )
                || std::any_of( mixR.begin(), mixR.begin() + N, []( float x ) { return !std::isfinite( x ) || std::abs( x ) > 32.f; } ) )
            {
                freeVoice( V );
                return;
            }

            // The note ends once its gate is off and every envelope has finished (or
            // the patch has none): fade briefly, then free the voice.
            if( !V.gate && !V.fading )
            {
                bool running = false;
                for( auto n : envelopes )
                    running = running || V.modules[(size_t)n]->isActive();
                V.fading = !running;
            }
            const auto fadeStep = V.fading ? 1.f / ( endFadeSeconds * (float)sampleRate ) : 0.f;

            for( int i = 0; i < N; ++i )
            {
                V.fade = juce::jmax( 0.f, V.fade - fadeStep );
                if( R )
                {
                    L[i] += mixL[(size_t)i] * V.fade;
                    R[i] += mixR[(size_t)i] * V.fade;
                }
                else
                    L[i] += ( mixL[(size_t)i] + mixR[(size_t)i] ) * V.fade;
            }

            if( V.fading && V.fade <= 0.f )
                freeVoice( V );
        }

        void render( float* L, float* R, int N, const Globals& G )
        {
            refreshValues();
            std::fill( L, L + N, 0.f );
            if( R )
                std::fill( R, R + N, 0.f );
            std::fill( peaks.begin(), peaks.end(), 0.f );

            if( arpStarting )
            {
                arpStarting = false;
                if( numArpHeld > 0 && arpMode.load( std::memory_order_relaxed ) != ArpOff )
                    arpStepNow( G );
            }

            // Split the block where the arpeggiator steps, so its notes land on time.
            for( int done = 0; done < N; )
            {
                const auto len = arpMode.load( std::memory_order_relaxed ) != ArpOff || arpSounding >= 0 ? arpSamplesToEvent( N - done ) : N - done;
                for( auto& v : voices )
                    for( int start = done; start < done + len && v.active; start += subBlock )
                        renderVoice( v, G, L + start, R ? R + start : nullptr, juce::jmin( subBlock, done + len - start ) );
                done += len;
                arpAdvance( len, G );
            }

            renderEffects( L, R, N, G );
            publishActivity();
        }

        void publishActivity()
        {
            // Modulators show their value in the newest sounding voice (0 when none).
            const Voice* newest = nullptr;
            for( const auto& v : voices )
                if( v.active && ( !newest || v.age > newest->age ) )
                    newest = &v;
            for( auto n : modulators )
                peaks[(size_t)n] = newest ? newest->src[(size_t)( kNumBuiltinSources + n )] : 0.f;
            for( size_t i = 0; i < peaks.size(); ++i )
                activity[i].store( peaks[i], std::memory_order_relaxed );
        }

        void renderEffects( float* L, float* R, int N, const Globals& G )
        {
            // Effects run every block, voices or not, so their tails ring out.
            if( effects.empty() || !R )
                return; // effects are stereo; a mono render skips them
            const VoiceContext                    ctx{ sampleRate, 60.f, 1.f, false, G.bpm };
            std::array<float, kNumBuiltinSources> global{};
            global[kModWheel] = G.modWheel;
            global[kBend]     = juce::jlimit( -1.f, 1.f, G.bendSemitones / bendRange );
            for( int m = 0; m < 8; ++m )
                global[(size_t)( modular::kMacro1 + m )] = G.macros[(size_t)m];

            for( auto& e : effects )
            {
                for( int p = e.firstParam; p < e.firstParam + (int)e.spec->params.size(); ++p )
                {
                    auto n = baseNorm[(size_t)p];
                    for( int r = routeBegin[(size_t)p]; r < routeBegin[(size_t)p + 1]; ++r )
                        n += depthNow[(size_t)routes[(size_t)r].depthIndex] * global[(size_t)routes[(size_t)r].source];
                    effectValues[(size_t)p] = paramSpecs[(size_t)p]->fromNormal( n );
                }
                e.processor->processStereo( ctx, &effectValues[(size_t)e.firstParam], L, R, N );

                auto& peak = peaks[nodes.size() + (size_t)( &e - effects.data() )];
                for( int i = 0; i < N; ++i )
                    peak = juce::jmax( peak, std::abs( L[i] ), std::abs( R[i] ) );
            }
        }
    };

    namespace
    {
        void writeValues( ModularEngine::Graph& G, const PatchDesc& D )
        {
            for( size_t i = 0; i < D.modules.size(); ++i )
            {
                const auto& m     = D.modules[i];
                const auto  first = (size_t)G.nodes[i].firstParam;
                for( size_t p = 0; p < m.values.size(); ++p )
                    G.base[first + p].store( m.spec->params[p].toNormal( m.values[p] ), std::memory_order_relaxed );
            }
            for( size_t e = 0; e < D.effects.size(); ++e )
            {
                const auto& fx    = D.effects[e];
                const auto  first = (size_t)G.effects[e].firstParam;
                for( size_t p = 0; p < fx.values.size(); ++p )
                    G.base[first + p].store( fx.spec->params[p].toNormal( fx.values[p] ), std::memory_order_relaxed );
            }
            G.playMode.store( D.playMode, std::memory_order_relaxed );
            G.glide.store( D.glide, std::memory_order_relaxed );
            G.arpMode.store( D.arp, std::memory_order_relaxed );
            G.arpRate.store( D.arpRate, std::memory_order_relaxed );
            G.arpOctaves.store( D.arpOctaves, std::memory_order_relaxed );
            G.arpGate.store( D.arpGate, std::memory_order_relaxed );
            size_t k = 0;
            forEachRoute( D, [&]( int, int, int, float Depth ) { G.depth[k++].store( Depth, std::memory_order_relaxed ); } );
            size_t c = 0;
            forEachCable( D, [&]( int, int, float Amount ) { G.amount[c++].store( Amount, std::memory_order_relaxed ); } );
        }

        std::unique_ptr<ModularEngine::Graph> buildGraph( const PatchDesc& D, double SampleRate )
        {
            using Graph = ModularEngine::Graph;
            auto g      = std::make_unique<Graph>();
            g->key        = D.structureKey();
            g->sampleRate = SampleRate;

            int lastCol = 0;
            for( int i = 0; i < (int)D.modules.size(); ++i )
            {
                const auto& m = D.modules[(size_t)i];
                g->nodes.push_back( { m.spec, g->numParams() } );
                for( const auto& p : m.spec->params )
                    g->paramSpecs.push_back( &p );

                if( m.spec->role == Role::Modulator )
                {
                    g->modulators.push_back( i );
                    continue;
                }
                if( g->columns.empty() || m.col != lastCol )
                    g->columns.emplace_back();
                lastCol = m.col;
                g->columns.back().nodes.push_back( i );
                if( m.spec->type == "pan" )
                    g->columns.back().panNode = i;
            }

            g->numModuleParams = g->numParams();
            for( const auto& e : D.effects )
            {
                g->effects.push_back( { e.spec, g->numParams(), e.spec->create() } );
                g->effects.back().processor->prepare( SampleRate );
                for( const auto& p : e.spec->params )
                    g->paramSpecs.push_back( &p );
            }

            // Routes, grouped by the parameter they modulate.
            struct Pending
            {
                int param, source, depthIndex;
            };
            std::vector<Pending> pending;
            const auto           numModules = (int)D.modules.size();
            forEachRoute( D, [&]( int Index, int Param, int Source, float ) {
                const auto first = Index < numModules ? g->nodes[(size_t)Index].firstParam : g->effects[(size_t)( Index - numModules )].firstParam;
                pending.push_back( { first + Param, Source, (int)pending.size() } );
            } );

            g->cables.resize( g->nodes.size() );
            int numCables = 0;
            g->feedsCable.assign( g->nodes.size(), 0 );
            forEachCable( D, [&]( int Target, int Source, float ) {
                g->cables[(size_t)Target].push_back( { Source, numCables++ } );
                g->feedsCable[(size_t)Source] = 1;
            } );
            std::stable_sort( pending.begin(), pending.end(), []( const Pending& A, const Pending& B ) { return A.param < B.param; } );

            g->routeBegin.assign( (size_t)g->numParams() + 1, 0 );
            for( const auto& r : pending )
            {
                g->routes.push_back( { r.source, r.depthIndex } );
                ++g->routeBegin[(size_t)r.param + 1];
            }
            for( int p = 0; p < g->numParams(); ++p )
            {
                if( g->routeBegin[(size_t)p + 1] > 0 && p < g->numModuleParams )
                    g->modulated.push_back( p ); // effects evaluate their own, once per block
                g->routeBegin[(size_t)p + 1] += g->routeBegin[(size_t)p];
            }

            g->base  = std::make_unique<std::atomic<float>[]>( (size_t)g->numParams() );
            g->depth  = std::make_unique<std::atomic<float>[]>( pending.size() );
            g->amount = std::make_unique<std::atomic<float>[]>( (size_t)numCables );
            g->amountNow.assign( (size_t)numCables, 0.f );
            g->effectValues.assign( (size_t)g->numParams(), 0.f );
            g->baseNorm.assign( (size_t)g->numParams(), 0.f );
            g->baseReal.assign( (size_t)g->numParams(), 0.f );
            g->depthNow.assign( pending.size(), 0.f );
            writeValues( *g, D );
            g->refreshValues();

            for( const auto& m : D.modules )
                g->partIds.push_back( m.id );
            for( const auto& e : D.effects )
                g->partIds.push_back( e.id );
            g->activity = std::make_unique<std::atomic<float>[]>( g->partIds.size() );
            g->peaks.assign( g->partIds.size(), 0.f );

            g->voices.resize( (size_t)D.voices );
            for( auto& v : g->voices )
            {
                for( const auto& n : g->nodes )
                {
                    v.modules.push_back( n.spec->create() );
                    v.modules.back()->prepare( SampleRate );
                }
                v.cur = v.prev = g->baseReal;
                v.src.assign( (size_t)( kNumBuiltinSources + (int)g->nodes.size() ), 0.f );
                v.outputs.assign( g->nodes.size(), {} );
            }
            if( !g->voices.empty() )
                for( int n = 0; n < (int)g->nodes.size(); ++n )
                    if( g->voices[0].modules[(size_t)n]->isEnvelope() )
                        g->envelopes.push_back( n );

            return g;
        }
    } // namespace

    //==============================================================================
    ModularEngine::ModularEngine()
    {
        for( int m = 0; m < 8; ++m )
            params.push_back( { "macro" + juce::String( m + 1 ), "Macro " + juce::String( m + 1 ), "", 0.f, 1.f, 0.f } );
        params.push_back( { "level", "Level", "", 0.f, 1.f, 0.7f } );
        jassert( params.size() == kNumParams );

        setPatch( {} ); // preset 0 until the program hands over its patch
    }

    ModularEngine::~ModularEngine() = default;

    void ModularEngine::prepare( double SampleRate, int /*MaxBlockSize*/ )
    {
        sampleRate = SampleRate > 0 ? SampleRate : 48000.0;

        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph && graph->sampleRate != sampleRate.load() )
            graph->prepare( sampleRate.load() );
        else if( graph )
        {
            graph->releaseAll();
            for( auto& v : graph->voices ) graph->freeVoice( v );
            graph->resetEffects();
        }
        crossfadeRemaining = 0;
    }

    void ModularEngine::reset()
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        crossfadeRemaining = 0;
        if( graph )
        {
            for( auto& v : graph->voices )
                graph->freeVoice( v );
            graph->resetEffects();
            graph->resetArp();
            graph->numHeld = 0;
        }
    }

    namespace
    {
        Globals snapshot( const std::atomic<float>& Bend, const std::atomic<float>& Wheel, const std::array<std::atomic<float>, 8>& Macros,
                          const std::atomic<double>& Tempo )
        {
            Globals g;
            g.bpm           = Tempo.load( std::memory_order_relaxed );
            g.bendSemitones = Bend.load( std::memory_order_relaxed );
            g.modWheel      = Wheel.load( std::memory_order_relaxed );
            for( size_t m = 0; m < g.macros.size(); ++m )
                g.macros[m] = Macros[m].load( std::memory_order_relaxed );
            return g;
        }
    } // namespace

    void ModularEngine::noteOn( int Note, float Velocity )
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            graph->startNote( Note, juce::jlimit( 0.f, 1.f, Velocity ), snapshot( bendSemitones, modWheelAmount, macros, tempo ) );
    }

    void ModularEngine::noteOff( int Note )
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            graph->releaseNote( Note, snapshot( bendSemitones, modWheelAmount, macros, tempo ) );
    }

    void ModularEngine::aftertouch( int Note, float Amount )
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            graph->aftertouch( Note, juce::jlimit( 0.f, 1.f, Amount ) );
    }

    void ModularEngine::setTempo( double Bpm )
    {
        if( Bpm > 0.0 )
            tempo = Bpm;
    }

    void ModularEngine::channelPressure( float Amount )
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            graph->channelPressure( juce::jlimit( 0.f, 1.f, Amount ) );
    }

    float ModularEngine::getActivity( int PartId ) const
    {
        // The graph only changes on this thread; the values are atomics.
        if( graph )
            for( size_t i = 0; i < graph->partIds.size(); ++i )
                if( graph->partIds[i] == PartId )
                    return graph->activity[i].load( std::memory_order_relaxed );
        return 0.f;
    }

    void ModularEngine::allNotesOff()
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            graph->releaseAll();
    }

    void ModularEngine::pitchBend( float Semitones ) { bendSemitones = Semitones; }
    void ModularEngine::modWheel( float Amount ) { modWheelAmount = juce::jlimit( 0.f, 1.f, Amount ); }

    void ModularEngine::setParam( int Index, float Value )
    {
        if( juce::isPositiveAndBelow( Index - kMacro1, 8 ) )
            macros[(size_t)( Index - kMacro1 )] = juce::jlimit( 0.f, 1.f, Value );
        else if( Index == kLevel )
            level = juce::jlimit( 0.f, 1.f, Value );
    }

    void ModularEngine::process( float* const* Out, int NumChannels, int NumSamples )
    {
        if( NumChannels <= 0 || NumSamples <= 0 )
            return;

        {
            const juce::SpinLock::ScopedLockType lock( graphLock );
            if( graph )
            {
                graph->render( Out[0], NumChannels > 1 ? Out[1] : nullptr, NumSamples, snapshot( bendSemitones, modWheelAmount, macros, tempo ) );
                // Retain the old graph off the audio thread, but render its short
                // release while the new graph fades in across structural edits.
                std::array<float, 128> oldL{}, oldR{};
                for( int offset = 0; previousGraph && crossfadeRemaining > 0 && offset < NumSamples; )
                {
                    const int n = juce::jmin( 128, NumSamples - offset, crossfadeRemaining );
                    previousGraph->render( oldL.data(), NumChannels > 1 ? oldR.data() : nullptr, n, snapshot( bendSemitones, modWheelAmount, macros, tempo ) );
                    for( int i = 0; i < n; ++i )
                    {
                        const float oldGain = (float)crossfadeRemaining-- / crossfadeLength;
                        Out[0][offset + i] = oldGain * oldL[(size_t)i] + ( 1.f - oldGain ) * Out[0][offset + i];
                        if( NumChannels > 1 ) Out[1][offset + i] = oldGain * oldR[(size_t)i] + ( 1.f - oldGain ) * Out[1][offset + i];
                    }
                    offset += n;
                }
            }
            else
                for( int ch = 0; ch < juce::jmin( 2, NumChannels ); ++ch )
                    juce::FloatVectorOperations::clear( Out[ch], NumSamples );
        }

        // Output level, ramped across the block.
        const auto target = level.load( std::memory_order_relaxed ) * outputHeadroom;
        const auto step   = ( target - lastGain ) / (float)NumSamples;
        for( int ch = 0; ch < juce::jmin( 2, NumChannels ); ++ch )
        {
            auto g = lastGain;
            for( int i = 0; i < NumSamples; ++i )
                Out[ch][i] *= ( g += step );
        }
        lastGain = target;

        for( int ch = 2; ch < NumChannels; ++ch )
            juce::FloatVectorOperations::copy( Out[ch], Out[0], NumSamples );
    }

    void ModularEngine::setPatch( const juce::ValueTree& Patch )
    {
        const auto desc = PatchDesc::fromTree( Patch.isValid() ? Patch : modular::createPreset( 0 ) );

        // Only knob values or depths changed: no rebuild, nothing interrupted.
        {
            const juce::SpinLock::ScopedLockType lock( graphLock );
            if( graph && graph->key == desc.structureKey() )
            {
                const bool modeChanged = graph->playMode.load() != desc.playMode;
                const bool arpChanged = graph->arpMode.load() != desc.arp;
                const auto held = graph->inputHeld;
                if( modeChanged || arpChanged ) graph->releaseAll();
                writeValues( *graph, desc );
                if( arpChanged && !modeChanged )
                {
                    graph->refreshValues();
                    const auto g = snapshot( bendSemitones, modWheelAmount, macros, tempo );
                    for( int n = 0; n < 128; ++n )
                        if( held[(size_t)n] > 0.f ) graph->startNote( n, held[(size_t)n], g );
                }
                return;
            }
        }

        std::unique_ptr<Graph> oldPrevious;
        for( ;; )
        {
            auto fresh = buildGraph( desc, sampleRate.load() );
            const juce::SpinLock::ScopedLockType lock( graphLock );
            if( fresh->sampleRate != sampleRate.load() ) continue; // retry preparation outside the audio lock
            if( graph ) // held notes keep playing through the new patch
            {
                const auto g = snapshot( bendSemitones, modWheelAmount, macros, tempo );
                fresh->refreshValues();
                for( int n = 0; n < 128; ++n )
                    if( graph->inputHeld[(size_t)n] > 0.f ) fresh->startNote( n, graph->inputHeld[(size_t)n], g );
                graph->releaseAll();
            }
            oldPrevious = std::move( previousGraph );
            previousGraph = std::move( graph );
            std::swap( graph, fresh );
            crossfadeLength = juce::jmax( 1, (int)( sampleRate.load() * 0.02 ) );
            crossfadeRemaining = previousGraph ? crossfadeLength : 0;
            break;
        }
        ++buildCount;
        // Any older crossfade graph is freed here, off the audio thread.
    }

    int ModularEngine::getNumActiveVoices()
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        int                                  n = 0;
        if( graph )
            for( const auto& v : graph->voices )
                n += v.active ? 1 : 0;
        return n;
    }
} // namespace mt

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
        };

        /// Calls F( module index, parameter index, source index, depth ) for every
        /// usable modulation, in patch order. Mods naming an unknown parameter or a
        /// source that is not a modulator are ignored.
        template <typename Fn>
        void forEachRoute( const PatchDesc& D, Fn&& F )
        {
            for( int i = 0; i < (int)D.modules.size(); ++i )
                for( const auto& m : D.modules[(size_t)i].mods )
                {
                    const auto p = D.modules[(size_t)i].spec->findParam( m.param );
                    if( p < 0 )
                        continue;

                    auto source = findBuiltinSource( m.source );
                    if( source < 0 && m.source.startsWith( "module:" ) )
                    {
                        const auto n = D.indexOf( m.source.fromFirstOccurrenceOf( ":", false, false ).getIntValue() );
                        if( n >= 0 && D.modules[(size_t)n].spec->role == Role::Modulator )
                            source = kNumBuiltinSources + n;
                    }
                    if( source >= 0 )
                        F( i, p, source, m.depth );
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

        struct Voice
        {
            std::vector<std::unique_ptr<Module>> modules; // one per node
            std::vector<float>                   cur, prev; // parameter values now / one sub-block ago
            std::vector<float>                   src;       // built-in sources, then one per node
            int                                  note{ 60 };
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
        std::vector<int>              modulated; // parameters with at least one route

        // Written by the message thread for value-only edits.
        std::unique_ptr<std::atomic<float>[]> base;  // normalised knob positions
        std::unique_ptr<std::atomic<float>[]> depth; // per route, in patch order

        // Audio thread.
        std::vector<float> baseNorm, baseReal, depthNow;
        std::vector<Voice> voices;
        uint64_t           noteCounter{ 0 };

        int numParams() const { return (int)paramSpecs.size(); }

        void prepare( double SampleRate )
        {
            sampleRate = SampleRate;
            for( auto& v : voices )
            {
                for( auto& m : v.modules )
                    m->prepare( SampleRate );
                freeVoice( v );
            }
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
        }

        VoiceContext context( const Voice& V, const Globals& G ) const
        {
            return { sampleRate, (float)V.note + G.bendSemitones, V.velocity, V.gate };
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
            if( voices.empty() )
                return;

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

            auto&      v     = *pick;
            const bool steal = v.active;
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

        void releaseNote( int Note )
        {
            for( auto& v : voices )
                if( v.active && v.gate && v.note == Note )
                    releaseVoice( v );
        }

        void releaseAll()
        {
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
        }

        void renderVoice( Voice& V, const Globals& G, float* L, float* R, int N )
        {
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
                    const auto first = (size_t)nodes[(size_t)n].firstParam;
                    V.modules[(size_t)n]->process( ctx, &V.prev[first], &V.cur[first], buf.data(), N );
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
            if( !std::isfinite( mixL[(size_t)N - 1] + mixR[(size_t)N - 1] ) )
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

            for( auto& v : voices )
                for( int start = 0; start < N && v.active; start += subBlock )
                    renderVoice( v, G, L + start, R ? R + start : nullptr, juce::jmin( subBlock, N - start ) );
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
            size_t k = 0;
            forEachRoute( D, [&]( int, int, int, float Depth ) { G.depth[k++].store( Depth, std::memory_order_relaxed ); } );
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

            // Routes, grouped by the parameter they modulate.
            struct Pending
            {
                int param, source, depthIndex;
            };
            std::vector<Pending> pending;
            forEachRoute( D, [&]( int Index, int Param, int Source, float ) {
                pending.push_back( { g->nodes[(size_t)Index].firstParam + Param, Source, (int)pending.size() } );
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
                if( g->routeBegin[(size_t)p + 1] > 0 )
                    g->modulated.push_back( p );
                g->routeBegin[(size_t)p + 1] += g->routeBegin[(size_t)p];
            }

            g->base  = std::make_unique<std::atomic<float>[]>( (size_t)g->numParams() );
            g->depth = std::make_unique<std::atomic<float>[]>( pending.size() );
            g->baseNorm.assign( (size_t)g->numParams(), 0.f );
            g->baseReal.assign( (size_t)g->numParams(), 0.f );
            g->depthNow.assign( pending.size(), 0.f );
            writeValues( *g, D );
            g->refreshValues();

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
    }

    void ModularEngine::reset()
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            for( auto& v : graph->voices )
                graph->freeVoice( v );
    }

    namespace
    {
        Globals snapshot( const std::atomic<float>& Bend, const std::atomic<float>& Wheel, const std::array<std::atomic<float>, 8>& Macros )
        {
            Globals g;
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
            graph->startNote( Note, juce::jlimit( 0.f, 1.f, Velocity ), snapshot( bendSemitones, modWheelAmount, macros ) );
    }

    void ModularEngine::noteOff( int Note )
    {
        const juce::SpinLock::ScopedLockType lock( graphLock );
        if( graph )
            graph->releaseNote( Note );
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
                graph->render( Out[0], NumChannels > 1 ? Out[1] : nullptr, NumSamples, snapshot( bendSemitones, modWheelAmount, macros ) );
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
        if( graph && graph->key == desc.structureKey() )
        {
            writeValues( *graph, desc );
            return;
        }

        auto fresh = buildGraph( desc, sampleRate.load() );
        {
            const juce::SpinLock::ScopedLockType lock( graphLock );
            if( fresh->sampleRate != sampleRate.load() ) // prepare() ran while we were building
                fresh->prepare( sampleRate.load() );
            if( graph ) // held notes keep playing through the new patch
            {
                const auto g = snapshot( bendSemitones, modWheelAmount, macros );
                for( const auto& v : graph->voices )
                    if( v.active && v.gate )
                        fresh->startNote( v.note, v.velocity, g );
            }
            std::swap( graph, fresh );
        }
        ++buildCount;
        // `fresh` now holds the old graph and is freed here, off the audio thread.
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

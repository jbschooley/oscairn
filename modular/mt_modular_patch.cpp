/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt::modular
{
    PatchDesc PatchDesc::fromTree( const juce::ValueTree& Patch )
    {
        PatchDesc d;
        d.name   = Patch.getProperty( PatchIDs::name ).toString();
        d.voices   = juce::jlimit( 1, maxVoices, (int)Patch.getProperty( PatchIDs::voices, defaultVoices ) );
        d.playMode = juce::jlimit( (int)Poly, (int)Legato, (int)Patch.getProperty( PatchIDs::playMode, (int)Poly ) );
        d.glide    = juce::jlimit( 0.f, maxGlide, (float)Patch.getProperty( PatchIDs::glide, 0.f ) );
        d.arp        = juce::jlimit( (int)ArpOff, (int)ArpAsPlayed, (int)Patch.getProperty( PatchIDs::arp, (int)ArpOff ) );
        d.arpRate    = juce::jlimit( 0, arpRateNames().size() - 1, (int)Patch.getProperty( PatchIDs::arpRate, 2 ) );
        d.arpOctaves = juce::jlimit( 1, 4, (int)Patch.getProperty( PatchIDs::arpOctaves, 1 ) );
        d.arpGate    = juce::jlimit( 0.05f, 1.f, (float)Patch.getProperty( PatchIDs::arpGate, 0.5f ) );

        for( const auto& m : Patch )
        {
            const bool isEffect = m.hasType( PatchIDs::FX );
            if( !isEffect && !m.hasType( PatchIDs::MODULE ) )
                continue;
            const auto* spec = findModuleSpec( m.getProperty( PatchIDs::type ).toString() );
            if( !spec || isEffect != ( spec->role == Role::Effect ) )
                continue; // from a newer build, or a typo: skip it rather than fail the patch

            Module mod;
            mod.id   = (int)m.getProperty( PatchIDs::id );
            mod.spec = spec;
            mod.col  = (int)m.getProperty( isEffect ? PatchIDs::slot : PatchIDs::col );
            mod.row  = isEffect ? 0 : (int)m.getProperty( PatchIDs::row );
            for( const auto& p : spec->params )
                mod.values.push_back( juce::jlimit( p.minValue, p.maxValue, (float)m.getProperty( juce::Identifier( p.id ), p.defaultValue ) ) );

            for( const auto& r : m )
                if( r.hasType( PatchIDs::MOD ) )
                    mod.mods.push_back( { r.getProperty( PatchIDs::param ).toString(), r.getProperty( PatchIDs::source ).toString(),
                                          juce::jlimit( -1.f, 1.f, (float)r.getProperty( PatchIDs::depth ) ) } );
                else if( r.hasType( PatchIDs::CABLE ) && !isEffect )
                    mod.cables.push_back( { r.getProperty( PatchIDs::source ).toString(), juce::jlimit( -1.f, 1.f, (float)r.getProperty( PatchIDs::amount, 1.f ) ) } );

            ( isEffect ? d.effects : d.modules ).push_back( std::move( mod ) );
        }

        for( auto* list : { &d.modules, &d.effects } )
            std::stable_sort( list->begin(), list->end(), []( const Module& A, const Module& B ) {
                return std::tie( A.col, A.row, A.id ) < std::tie( B.col, B.row, B.id );
            } );
        return d;
    }

    juce::String PatchDesc::structureKey() const
    {
        juce::String k;
        k << "v" << voices;
        for( const auto& m : modules )
        {
            k << "|" << m.id << ":" << m.spec->type << "@" << m.col << "," << m.row;
            for( const auto& r : m.mods )
                k << ";" << r.param << "<" << r.source;
            for( const auto& c : m.cables )
                k << ";in<" << c.source;
        }
        for( const auto& e : effects )
        {
            k << "|fx" << e.id << ":" << e.spec->type << "@" << e.col;
            for( const auto& r : e.mods )
                k << ";" << r.param << "<" << r.source;
        }
        return k;
    }

    int PatchDesc::indexOf( int Id ) const
    {
        for( size_t i = 0; i < modules.size(); ++i )
            if( modules[i].id == Id )
                return (int)i;
        return -1;
    }

    //==============================================================================
    PatchBuilder::PatchBuilder( const juce::String& Name, int Voices )
        : tree( PatchIDs::MODULAR_PATCH )
    {
        tree.setProperty( PatchIDs::name, Name, nullptr );
        tree.setProperty( PatchIDs::voices, Voices, nullptr );
    }

    int PatchBuilder::add( const juce::String& Type, int Col, int Row, std::initializer_list<std::pair<const char*, float>> Values )
    {
        juce::ValueTree m( PatchIDs::MODULE );
        const auto      id = nextId++;
        m.setProperty( PatchIDs::id, id, nullptr );
        m.setProperty( PatchIDs::type, Type, nullptr );
        m.setProperty( PatchIDs::col, Col, nullptr );
        m.setProperty( PatchIDs::row, Row, nullptr );
        for( const auto& [param, value] : Values )
        {
            jassert( findModuleSpec( Type ) && findModuleSpec( Type )->findParam( param ) >= 0 );
            m.setProperty( juce::Identifier( param ), value, nullptr );
        }
        tree.appendChild( m, nullptr );
        return id;
    }

    int PatchBuilder::addEffect( const juce::String& Type, int Slot, std::initializer_list<std::pair<const char*, float>> Values )
    {
        juce::ValueTree e( PatchIDs::FX );
        const auto      id = nextId++;
        e.setProperty( PatchIDs::id, id, nullptr );
        e.setProperty( PatchIDs::type, Type, nullptr );
        e.setProperty( PatchIDs::slot, Slot, nullptr );
        for( const auto& [param, value] : Values )
        {
            jassert( findModuleSpec( Type ) && findModuleSpec( Type )->findParam( param ) >= 0 );
            e.setProperty( juce::Identifier( param ), value, nullptr );
        }
        tree.appendChild( e, nullptr );
        return id;
    }

    PatchBuilder& PatchBuilder::cable( int Target, int Source, float Amount )
    {
        auto m = tree.getChildWithProperty( PatchIDs::id, Target );
        jassert( m.isValid() );
        juce::ValueTree c( PatchIDs::CABLE );
        c.setProperty( PatchIDs::source, module( Source ), nullptr );
        c.setProperty( PatchIDs::amount, Amount, nullptr );
        m.appendChild( c, nullptr );
        return *this;
    }

    PatchBuilder& PatchBuilder::macroName( int Macro, const juce::String& Name )
    {
        tree.setProperty( PatchIDs::macroName( Macro ), Name, nullptr );
        return *this;
    }

    PatchBuilder& PatchBuilder::mod( int Target, const juce::String& Param, const juce::String& Source, float Depth )
    {
        auto m = tree.getChildWithProperty( PatchIDs::id, Target );
        jassert( m.isValid() );
        juce::ValueTree r( PatchIDs::MOD );
        r.setProperty( PatchIDs::param, Param, nullptr );
        r.setProperty( PatchIDs::source, Source, nullptr );
        r.setProperty( PatchIDs::depth, Depth, nullptr );
        m.appendChild( r, nullptr );
        return *this;
    }

    //==============================================================================
    namespace
    {
        using B = PatchBuilder;

        // Column 0 is the sound; the modulators sit in column 1.
        juce::ValueTree init()
        {
            B    b( "Init" );
            auto flt = b.add( "filter", 0, 1, { { "cutoff", 1200 }, { "reso", 0.15f } } );
            auto vca = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto env = b.add( "adsr", 1, 0 );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "level", 0.5f } } );
            b.mod( vca, "gain", B::module( env ), 1 );
            b.mod( flt, "cutoff", "macro1", 0.4f ).mod( flt, "reso", "macro2", 0.8f );
            return b.tree;
        }

        // The Basic Synth engine's default sound, rebuilt from modules.
        juce::ValueTree basicPoly()
        {
            B b( "Basic Poly" );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "fine", -4 }, { "level", 0.35f } } );
            b.add( "osc", 0, 1, { { "wave", 2 }, { "fine", 4 }, { "level", 0.35f } } );
            b.add( "osc", 0, 2, { { "wave", 3 }, { "tune", -12 }, { "level", 0.3f } } );
            auto flt  = b.add( "filter", 0, 3, { { "cutoff", 1800 }, { "reso", 0.25f } } );
            auto vca  = b.add( "vca", 0, 4, { { "gain", 0 } } );
            auto amp  = b.add( "adsr", 1, 0, { { "attack", 0.005f }, { "decay", 0.4f }, { "sustain", 0.6f }, { "release", 0.3f }, { "velocity", 1 } } );
            auto fenv = b.add( "adsr", 1, 1, { { "attack", 0.001f }, { "decay", 0.35f }, { "sustain", 0 }, { "release", 0.35f }, { "velocity", 0 } } );
            b.mod( vca, "gain", B::module( amp ), 1 );
            b.mod( flt, "cutoff", B::module( fenv ), 0.27f ); // ~2.7 octaves, as Basic Synth's default
            b.mod( flt, "cutoff", "macro1", 0.4f ).mod( fenv, "decay", "macro2", 0.4f );
            return b.tree;
        }

        // Karplus-Strong: a noise burst rings a delay line tuned to the note.
        juce::ValueTree pluckString()
        {
            B    b( "Pluck String" );
            auto noise = b.add( "noise", 0, 0, { { "color", 0.1f }, { "level", 1 } } );
            auto burst = b.add( "vca", 0, 1, { { "gain", 0 } } );
            auto comb  = b.add( "delay", 0, 2, { { "track", 1 }, { "feedback", 0.99f }, { "damp", 0.25f }, { "mix", 1 } } );
            auto vca   = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto benv  = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 0.015f }, { "sustain", 0 }, { "release", 0.015f }, { "velocity", 0.8f } } );
            auto amp   = b.add( "adsr", 1, 1, { { "attack", 0.001f }, { "decay", 1 }, { "sustain", 1 }, { "release", 0.5f }, { "velocity", 0.3f } } );
            b.mod( burst, "gain", B::module( benv ), 1 ).mod( vca, "gain", B::module( amp ), 1 );
            b.mod( comb, "damp", "macro1", 0.6f ).mod( noise, "color", "macro2", 1 );
            const auto echo = b.addEffect( "fxdelay", 0, { { "time", 375 }, { "feedback", 0.35f }, { "mix", 0.25f } } );
            b.mod( echo, "mix", "macro3", 0.5f );
            return b.tree;
        }

        // Two-operator FM: the first sine phase-modulates the second.
        juce::ValueTree fmBell()
        {
            B    b( "FM Bell" );
            auto modOsc = b.add( "osc", 0, 0, { { "wave", 0 }, { "tune", 21.686f }, { "level", 0 } } ); // ratio 3.5
            b.add( "osc", 0, 1, { { "wave", 0 }, { "input", 1 }, { "pm", 1 }, { "level", 0.6f } } );
            auto vca   = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp   = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 3 }, { "sustain", 0 }, { "release", 2 }, { "velocity", 0.6f } } );
            auto index = b.add( "adsr", 1, 1, { { "attack", 0.001f }, { "decay", 1 }, { "sustain", 0.05f }, { "release", 1 }, { "velocity", 0.8f } } );
            b.mod( vca, "gain", B::module( amp ), 1 );
            b.mod( modOsc, "level", B::module( index ), 0.6f ).mod( modOsc, "level", "macro1", 0.4f );
            b.mod( modOsc, "tune", "macro2", 0.125f ); // up to an octave higher
            b.macroName( 0, "Brightness" ).macroName( 1, "Ratio" );
            const auto verb = b.addEffect( "reverb", 0, { { "size", 0.75f }, { "mix", 0.3f } } );
            b.mod( verb, "mix", "macro3", 0.5f ).macroName( 2, "Reverb" );
            return b.tree;
        }

        juce::ValueTree wobbleBass()
        {
            B b( "Wobble Bass", 4 );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "level", 0.45f } } );
            b.add( "osc", 0, 1, { { "wave", 3 }, { "tune", -12 }, { "level", 0.35f } } );
            auto flt = b.add( "filter", 0, 2, { { "cutoff", 350 }, { "reso", 0.6f } } );
            b.add( "drive", 0, 3, { { "drive", 0.35f }, { "level", 0.6f } } );
            auto vca = b.add( "vca", 0, 4, { { "gain", 0 } } );
            auto lfo = b.add( "lfo", 1, 0, { { "shape", 0 }, { "rate", 3 } } );
            auto amp = b.add( "adsr", 1, 1, { { "attack", 0.002f }, { "decay", 0.2f }, { "sustain", 1 }, { "release", 0.08f }, { "velocity", 0.3f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", B::module( lfo ), 0.35f );
            b.mod( lfo, "rate", "macro1", 0.4f ).mod( flt, "cutoff", "macro2", 0.4f );
            return b.tree;
        }

        juce::ValueTree foldLead()
        {
            B b( "Fold Lead", 6 );
            b.add( "osc", 0, 0, { { "wave", 0 }, { "level", 0.7f } } );
            auto fold = b.add( "drive", 0, 1, { { "drive", 0.25f }, { "fold", 0.5f }, { "level", 0.6f } } );
            b.add( "filter", 0, 2, { { "cutoff", 4500 }, { "reso", 0.15f } } );
            auto vca = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto lfo = b.add( "lfo", 1, 0, { { "shape", 1 }, { "rate", 0.25f }, { "retrig", 0 } } );
            auto amp = b.add( "adsr", 1, 1, { { "attack", 0.01f }, { "decay", 0.5f }, { "sustain", 0.8f }, { "release", 0.4f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( fold, "fold", B::module( lfo ), 0.3f );
            b.mod( fold, "fold", "macro1", 0.5f ).mod( fold, "drive", "macro2", 0.5f );
            return b.tree;
        }

        juce::ValueTree noiseHat()
        {
            B    b( "Noise Hat", 6 );
            auto flt = b.add( "filter", 0, 1, { { "mode", 2 }, { "cutoff", 7000 }, { "reso", 0.3f } } );
            auto vca = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 0.07f }, { "sustain", 0 }, { "release", 0.06f }, { "velocity", 0.8f } } );
            b.add( "noise", 0, 0, { { "level", 1 } } );
            b.mod( vca, "gain", B::module( amp ), 1 );
            b.mod( flt, "cutoff", "macro1", 0.2f ).mod( amp, "decay", "macro2", 0.4f );
            return b.tree;
        }

        juce::ValueTree randomPad()
        {
            B b( "Random Pad" );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "fine", -9 }, { "level", 0.3f } } );
            b.add( "osc", 0, 1, { { "wave", 2 }, { "fine", 9 }, { "level", 0.3f } } );
            b.add( "osc", 0, 2, { { "wave", 1 }, { "tune", 12 }, { "level", 0.15f } } );
            auto flt = b.add( "filter", 0, 3, { { "cutoff", 900 }, { "reso", 0.2f } } );
            auto vca = b.add( "vca", 0, 4, { { "gain", 0 } } );
            auto pan = b.add( "pan", 0, 5 );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.9f }, { "decay", 1 }, { "sustain", 0.8f }, { "release", 1.8f }, { "velocity", 0.3f } } );
            auto lfo = b.add( "lfo", 1, 1, { { "rate", 0.15f }, { "retrig", 0 } } );
            auto rnd = b.add( "random", 1, 2 );
            b.mod( vca, "gain", B::module( amp ), 1 );
            b.mod( flt, "cutoff", B::module( lfo ), 0.12f ).mod( flt, "cutoff", B::module( rnd ), 0.12f );
            b.mod( pan, "pan", B::module( rnd ), 0.35f );
            b.mod( flt, "cutoff", "macro1", 0.4f ).mod( amp, "attack", "macro2", -0.3f );
            b.addEffect( "chorus", 0, { { "rate", 0.3f }, { "depth", 0.6f }, { "mix", 0.4f } } );
            const auto verb = b.addEffect( "reverb", 1, { { "size", 0.85f }, { "mix", 0.35f } } );
            b.mod( verb, "mix", "macro3", 0.4f );
            return b.tree;
        }

        // Column 1's oscillator phase-modulates column 0's through a cable; the VCA
        // at the bottom of column 1 (gain 0) keeps the modulator out of the mix.
        juce::ValueTree crossFm()
        {
            B    b( "Cross FM", 6 );
            auto carrier = b.add( "osc", 0, 0, { { "wave", 0 }, { "input", 1 }, { "pm", 1 }, { "level", 0.6f } } );
            auto vca     = b.add( "vca", 0, 1, { { "gain", 0 } } );
            auto modOsc  = b.add( "osc", 1, 0, { { "wave", 0 }, { "tune", 12 }, { "level", 0.5f } } );
            b.add( "vca", 1, 1, { { "gain", 0 } } );
            auto amp   = b.add( "adsr", 2, 0, { { "attack", 0.002f }, { "decay", 1.2f }, { "sustain", 0.4f }, { "release", 0.5f } } );
            auto index = b.add( "adsr", 2, 1, { { "attack", 0.001f }, { "decay", 0.4f }, { "sustain", 0.2f }, { "release", 0.4f }, { "velocity", 0.8f } } );
            b.cable( carrier, modOsc, 1.f );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( modOsc, "level", B::module( index ), 0.5f );
            b.mod( modOsc, "level", "macro1", 0.5f ).mod( modOsc, "tune", "macro2", 0.07f );
            b.macroName( 0, "FM Amount" ).macroName( 1, "Ratio" );
            return b.tree;
        }

        // Seven detuned saws through a ladder filter, played legato with glide.
        juce::ValueTree supersawLead()
        {
            B b( "Supersaw Lead", 1 );
            b.tree.setProperty( PatchIDs::playMode, (int)Legato, nullptr ).setProperty( PatchIDs::glide, 0.08f, nullptr );
            auto osc = b.add( "osc", 0, 0, { { "wave", 2 }, { "unison", 7 }, { "spread", 22 }, { "level", 0.6f } } );
            auto flt = b.add( "filter", 0, 1, { { "mode", 4 }, { "cutoff", 2500 }, { "reso", 0.35f }, { "track", 0.5f } } );
            auto vca = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.004f }, { "decay", 0.6f }, { "sustain", 0.8f }, { "release", 0.25f }, { "velocity", 0.3f } } );
            auto fenv = b.add( "adsr", 1, 1, { { "attack", 0.002f }, { "decay", 0.5f }, { "sustain", 0.3f }, { "release", 0.3f }, { "velocity", 0.5f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", B::module( fenv ), 0.2f );
            b.mod( flt, "cutoff", "macro1", 0.4f ).mod( osc, "spread", "macro2", 0.6f ).mod( flt, "cutoff", "pressure", 0.25f );
            b.macroName( 1, "Spread" );
            b.addEffect( "chorus", 0, { { "rate", 0.25f }, { "depth", 0.4f }, { "mix", 0.3f } } );
            auto echo = b.addEffect( "fxdelay", 1, { { "time", 330 }, { "feedback", 0.3f }, { "mix", 0.2f } } );
            b.mod( echo, "mix", "macro3", 0.5f );
            return b.tree;
        }

        // Pressing harder opens the filter and adds vibrato.
        juce::ValueTree pressurePad()
        {
            B    b( "Pressure Pad" );
            auto o1  = b.add( "osc", 0, 0, { { "wave", 2 }, { "unison", 3 }, { "spread", 12 }, { "level", 0.4f } } );
            auto flt = b.add( "filter", 0, 1, { { "cutoff", 500 }, { "reso", 0.3f } } );
            auto vca = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.3f }, { "decay", 1 }, { "sustain", 0.9f }, { "release", 1.2f }, { "velocity", 0.2f } } );
            auto vib = b.add( "lfo", 1, 1, { { "rate", 5.5f }, { "retrig", 0 }, { "amount", 0 } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", "pressure", 0.45f ).mod( vca, "gain", "pressure", 0.2f );
            b.mod( o1, "fine", B::module( vib ), 0.1f ).mod( vib, "amount", "pressure", 1.f );
            b.mod( flt, "cutoff", "macro1", 0.4f );
            b.addEffect( "reverb", 0, { { "size", 0.8f }, { "mix", 0.3f } } );
            return b.tree;
        }

        // Sync choice indices (see syncChoices()).
        constexpr float sync1Bar = 3, sync2Bars = 2, sync8thDotted = 13;

        // A 303-style line: one voice, ladder filter with a snappy envelope, slides on overlap.
        juce::ValueTree acidBass()
        {
            B b( "Acid Bass", 1 );
            b.tree.setProperty( PatchIDs::playMode, (int)Legato, nullptr ).setProperty( PatchIDs::glide, 0.06f, nullptr );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "level", 0.6f } } );
            auto flt  = b.add( "filter", 0, 1, { { "mode", 4 }, { "cutoff", 280 }, { "reso", 0.75f }, { "track", 0.3f } } );
            auto drv  = b.add( "drive", 0, 2, { { "drive", 0.3f }, { "level", 0.6f } } );
            auto vca  = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto amp  = b.add( "adsr", 1, 0, { { "attack", 0.002f }, { "decay", 0.3f }, { "sustain", 0.6f }, { "release", 0.05f }, { "velocity", 0.4f } } );
            auto fenv = b.add( "adsr", 1, 1, { { "attack", 0.001f }, { "decay", 0.18f }, { "sustain", 0 }, { "release", 0.1f }, { "velocity", 0.7f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", B::module( fenv ), 0.45f );
            b.mod( flt, "cutoff", "macro1", 0.4f ).mod( flt, "reso", "macro2", 0.25f ).mod( drv, "drive", "macro4", 0.6f );
            auto echo = b.addEffect( "fxdelay", 0, { { "sync", sync8thDotted }, { "feedback", 0.35f }, { "mix", 0.15f } } );
            b.mod( echo, "mix", "macro3", 0.4f );
            return b.tree;
        }

        // Hold a chord: it plays as 1/16 notes up two octaves, with a dotted-eighth echo.
        juce::ValueTree arpSparkle()
        {
            B b( "Arp Sparkle", 6 );
            b.tree.setProperty( PatchIDs::arp, (int)ArpUp, nullptr ).setProperty( PatchIDs::arpRate, 2, nullptr ).setProperty( PatchIDs::arpOctaves, 2, nullptr ).setProperty( PatchIDs::arpGate, 0.5f, nullptr );
            b.add( "osc", 0, 0, { { "wave", 3 }, { "pw", 0.3f }, { "level", 0.4f } } );
            b.add( "osc", 0, 1, { { "wave", 2 }, { "tune", 12 }, { "level", 0.2f } } );
            auto flt = b.add( "filter", 0, 2, { { "cutoff", 3000 }, { "reso", 0.3f } } );
            auto vca = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 0.15f }, { "sustain", 0.2f }, { "release", 0.2f }, { "velocity", 0.5f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", "macro1", 0.4f );
            b.addEffect( "fxdelay", 0, { { "sync", sync8thDotted }, { "feedback", 0.4f }, { "pingpong", 1 }, { "mix", 0.3f } } );
            auto verb = b.addEffect( "reverb", 1, { { "size", 0.7f }, { "mix", 0.25f } } );
            b.mod( verb, "mix", "macro3", 0.4f );
            return b.tree;
        }

        // A synced LFO walks the formant filter through A-E-I-O-U.
        juce::ValueTree vowelLead()
        {
            B b( "Vowel Lead", 1 );
            b.tree.setProperty( PatchIDs::playMode, (int)Legato, nullptr ).setProperty( PatchIDs::glide, 0.12f, nullptr );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "unison", 3 }, { "spread", 10 }, { "level", 0.6f } } );
            auto vow = b.add( "formant", 0, 1, { { "vowel", 0 }, { "reso", 0.6f } } );
            auto vca = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.02f }, { "decay", 0.5f }, { "sustain", 0.9f }, { "release", 0.3f } } );
            auto lfo = b.add( "lfo", 1, 1, { { "shape", 1 }, { "sync", sync1Bar }, { "polarity", 1 }, { "retrig", 1 } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( vow, "vowel", B::module( lfo ), 0.9f ).mod( vow, "vowel", "macro1", 0.8f );
            b.mod( vow, "shift", "macro2", 0.3f );
            b.macroName( 0, "Vowel" ).macroName( 1, "Formant Shift" );
            b.addEffect( "chorus", 0, { { "mix", 0.3f } } );
            return b.tree;
        }

        // A sine ring-modulated a fifth up: inharmonic and metallic.
        juce::ValueTree metalBell()
        {
            B    b( "Metal Bell" );
            b.add( "osc", 0, 0, { { "wave", 0 }, { "level", 0.7f } } );
            auto ring = b.add( "ring", 0, 1, { { "tune", 7.4f }, { "mix", 0.8f } } );
            b.add( "filter", 0, 2, { { "mode", 2 }, { "cutoff", 180 } } );
            auto vca = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 2.5f }, { "sustain", 0 }, { "release", 1.5f }, { "velocity", 0.6f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( ring, "tune", "macro1", 0.1f ).mod( ring, "mix", "macro2", -0.6f );
            b.macroName( 0, "Ring Tune" ).macroName( 1, "Purity" );
            b.addEffect( "reverb", 0, { { "size", 0.8f }, { "mix", 0.3f } } );
            return b.tree;
        }

        juce::ValueTree subBass()
        {
            B b( "Sub Bass", 1 );
            b.tree.setProperty( PatchIDs::playMode, (int)Mono, nullptr );
            b.add( "osc", 0, 0, { { "wave", 0 }, { "level", 0.9f } } );
            b.add( "osc", 0, 1, { { "wave", 1 }, { "tune", 12 }, { "level", 0.15f } } );
            auto drv = b.add( "drive", 0, 2, { { "drive", 0.2f }, { "level", 0.8f } } );
            auto vca = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.003f }, { "decay", 0.5f }, { "sustain", 0.9f }, { "release", 0.1f }, { "velocity", 0.2f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( drv, "drive", "macro1", 0.6f );
            return b.tree;
        }

        juce::ValueTree strings()
        {
            B    b( "Strings" );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "unison", 5 }, { "spread", 18 }, { "level", 0.5f } } );
            auto flt  = b.add( "filter", 0, 1, { { "cutoff", 2500 }, { "reso", 0.1f } } );
            auto vca  = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp  = b.add( "adsr", 1, 0, { { "attack", 0.6f }, { "decay", 1 }, { "sustain", 0.9f }, { "release", 1.2f }, { "velocity", 0.3f } } );
            auto fenv = b.add( "adsr", 1, 1, { { "attack", 0.5f }, { "decay", 1 }, { "sustain", 0.8f }, { "release", 1.2f }, { "velocity", 0 } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", B::module( fenv ), 0.15f );
            b.mod( flt, "cutoff", "macro1", 0.3f ).mod( amp, "attack", "macro2", -0.4f );
            b.addEffect( "chorus", 0, { { "rate", 0.3f }, { "depth", 0.5f }, { "mix", 0.4f } } );
            b.addEffect( "reverb", 1, { { "size", 0.8f }, { "mix", 0.3f } } );
            return b.tree;
        }

        // An 808-style kick: a sine whose pitch drops fast. Plays two octaves below the pad's note.
        juce::ValueTree kickDrum()
        {
            B    b( "Kick Drum", 4 );
            auto osc  = b.add( "osc", 0, 0, { { "wave", 0 }, { "tune", -24 }, { "level", 1 } } );
            auto drv  = b.add( "drive", 0, 1, { { "drive", 0.2f }, { "level", 0.8f } } );
            auto vca  = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto amp  = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 0.35f }, { "sustain", 0 }, { "release", 0.1f }, { "velocity", 0.5f } } );
            auto pitch = b.add( "adsr", 1, 1, { { "attack", 0.001f }, { "decay", 0.06f }, { "sustain", 0 }, { "release", 0.06f }, { "velocity", 0 } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( osc, "tune", B::module( pitch ), 0.45f );
            b.mod( pitch, "decay", "macro1", 0.3f ).mod( drv, "drive", "macro2", 0.4f ).mod( amp, "decay", "macro3", 0.3f );
            b.macroName( 0, "Punch" ).macroName( 2, "Length" );
            return b.tree;
        }

        juce::ValueTree snare()
        {
            B    b( "Snare", 4 );
            b.add( "noise", 0, 0, { { "level", 0.8f } } );
            b.add( "filter", 0, 1, { { "mode", 1 }, { "cutoff", 1800 }, { "reso", 0.3f } } );
            auto nvca = b.add( "vca", 0, 2, { { "gain", 0 } } );
            auto tone = b.add( "osc", 1, 0, { { "wave", 1 }, { "tune", -12 }, { "level", 0.5f } } );
            auto tvca = b.add( "vca", 1, 1, { { "gain", 0 } } );
            auto nenv = b.add( "adsr", 2, 0, { { "attack", 0.001f }, { "decay", 0.15f }, { "sustain", 0 }, { "release", 0.12f }, { "velocity", 0.6f } } );
            auto tenv = b.add( "adsr", 2, 1, { { "attack", 0.001f }, { "decay", 0.08f }, { "sustain", 0 }, { "release", 0.06f }, { "velocity", 0.6f } } );
            b.mod( nvca, "gain", B::module( nenv ), 1 ).mod( tvca, "gain", B::module( tenv ), 1 ).mod( tone, "tune", B::module( tenv ), 0.12f );
            b.mod( nenv, "decay", "macro1", 0.4f ).mod( tone, "level", "macro2", 0.5f );
            b.macroName( 0, "Snappy" ).macroName( 1, "Body" );
            return b.tree;
        }

        // Drawbars: sines at the fundamental, octave, twelfth and two octaves, with a chorus.
        juce::ValueTree organ()
        {
            B b( "Organ" );
            b.add( "osc", 0, 0, { { "wave", 0 }, { "level", 0.4f } } );
            b.add( "osc", 0, 1, { { "wave", 0 }, { "tune", 12 }, { "level", 0.3f } } );
            auto twelfth = b.add( "osc", 0, 2, { { "wave", 0 }, { "tune", 19 }, { "level", 0.2f } } );
            b.add( "osc", 0, 3, { { "wave", 0 }, { "tune", 24 }, { "level", 0.15f } } );
            auto vca = b.add( "vca", 0, 4, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 0.005f }, { "decay", 0.1f }, { "sustain", 1 }, { "release", 0.05f }, { "velocity", 0 } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( twelfth, "level", "macro1", 0.5f );
            b.macroName( 0, "Drawbar" );
            b.addEffect( "chorus", 0, { { "rate", 5 }, { "depth", 0.3f }, { "mix", 0.4f } } );
            return b.tree;
        }

        // An FM electric piano through a bit crusher.
        juce::ValueTree lofiKeys()
        {
            B    b( "Lo-Fi Keys" );
            auto modOsc = b.add( "osc", 0, 0, { { "wave", 0 }, { "tune", 12 }, { "level", 0 } } );
            b.add( "osc", 0, 1, { { "wave", 0 }, { "input", 1 }, { "pm", 1 }, { "level", 0.6f } } );
            auto crush = b.add( "crush", 0, 2, { { "bits", 10 }, { "rate", 3 }, { "mix", 0.5f } } );
            auto vca   = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto amp   = b.add( "adsr", 1, 0, { { "attack", 0.001f }, { "decay", 1.5f }, { "sustain", 0.3f }, { "release", 0.4f }, { "velocity", 0.6f } } );
            auto index = b.add( "adsr", 1, 1, { { "attack", 0.001f }, { "decay", 0.5f }, { "sustain", 0.1f }, { "release", 0.4f }, { "velocity", 0.8f } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( modOsc, "level", B::module( index ), 0.4f );
            b.mod( crush, "mix", "macro1", 0.5f ).mod( modOsc, "level", "macro2", 0.4f );
            b.macroName( 0, "Lo-Fi" ).macroName( 1, "Bark" );
            b.addEffect( "chorus", 0, { { "rate", 0.8f }, { "depth", 0.4f }, { "mix", 0.35f } } );
            return b.tree;
        }

        // Slow, wide and evolving: a two-bar LFO on the filter into a big reverb.
        juce::ValueTree drone()
        {
            B b( "Drone" );
            b.add( "osc", 0, 0, { { "wave", 2 }, { "level", 0.35f } } );
            b.add( "osc", 0, 1, { { "wave", 2 }, { "fine", 7 }, { "tune", -12 }, { "level", 0.35f } } );
            auto flt = b.add( "filter", 0, 2, { { "cutoff", 700 }, { "reso", 0.5f } } );
            auto vca = b.add( "vca", 0, 3, { { "gain", 0 } } );
            auto amp = b.add( "adsr", 1, 0, { { "attack", 2 }, { "decay", 1 }, { "sustain", 1 }, { "release", 3 }, { "velocity", 0.2f } } );
            auto lfo = b.add( "lfo", 1, 1, { { "sync", sync2Bars }, { "retrig", 0 } } );
            b.mod( vca, "gain", B::module( amp ), 1 ).mod( flt, "cutoff", B::module( lfo ), 0.25f ).mod( flt, "cutoff", "macro1", 0.4f );
            b.addEffect( "fxdelay", 0, { { "sync", 5 }, { "feedback", 0.5f }, { "mix", 0.25f } } );
            b.addEffect( "reverb", 1, { { "size", 0.95f }, { "mix", 0.45f } } );
            return b.tree;
        }

        struct Preset
        {
            const char* name;
            juce::ValueTree ( *make )();
        };

        const std::array<Preset, 22> presets{ {
            { "Init", init },
            { "Basic Poly", basicPoly },
            { "Pluck String", pluckString },
            { "FM Bell", fmBell },
            { "Wobble Bass", wobbleBass },
            { "Fold Lead", foldLead },
            { "Noise Hat", noiseHat },
            { "Random Pad", randomPad },
            { "Cross FM", crossFm },
            { "Supersaw Lead", supersawLead },
            { "Pressure Pad", pressurePad },
            { "Acid Bass", acidBass },
            { "Arp Sparkle", arpSparkle },
            { "Vowel Lead", vowelLead },
            { "Metal Bell", metalBell },
            { "Sub Bass", subBass },
            { "Strings", strings },
            { "Kick Drum", kickDrum },
            { "Snare", snare },
            { "Organ", organ },
            { "Lo-Fi Keys", lofiKeys },
            { "Drone", drone },
        } };
    } // namespace

    juce::StringArray getPresetNames()
    {
        juce::StringArray names;
        for( const auto& p : presets )
            names.add( p.name );
        return names;
    }

    juce::ValueTree createPreset( int Index )
    {
        return presets[(size_t)juce::jlimit( 0, (int)presets.size() - 1, Index )].make();
    }

    //==============================================================================
    namespace
    {
        bool gridBefore( const juce::ValueTree& A, const juce::ValueTree& B )
        {
            return std::make_tuple( (int)A.getProperty( PatchIDs::col ), (int)A.getProperty( PatchIDs::row ), (int)A.getProperty( PatchIDs::id ) )
                 < std::make_tuple( (int)B.getProperty( PatchIDs::col ), (int)B.getProperty( PatchIDs::row ), (int)B.getProperty( PatchIDs::id ) );
        }

        juce::String builtinSourceName( int Source )
        {
            switch( Source )
            {
                case kVelocity: return "Velocity";
                case kNote: return "Note";
                case kGate: return "Gate";
                case kModWheel: return "Mod Wheel";
                case kBend: return "Pitch Bend";
                case kPressure: return "Pressure";
                default: break;
            }
            return "Macro " + juce::String( Source - kMacro1 + 1 );
        }
    } // namespace

    juce::ValueTree PatchEditor::module( int Id ) const
    {
        for( const auto& m : patch )
            if( ( m.hasType( PatchIDs::MODULE ) || m.hasType( PatchIDs::FX ) ) && (int)m.getProperty( PatchIDs::id ) == Id )
                return m;
        return {};
    }

    juce::ValueTree PatchEditor::effectAt( int Slot ) const
    {
        for( const auto& m : patch )
            if( m.hasType( PatchIDs::FX ) && (int)m.getProperty( PatchIDs::slot ) == Slot )
                return m;
        return {};
    }

    int PatchEditor::addEffect( const juce::String& Type, int Slot )
    {
        const auto* spec = findModuleSpec( Type );
        if( !patch.isValid() || !spec || spec->role != Role::Effect || !juce::isPositiveAndBelow( Slot, effectSlots ) || effectAt( Slot ).isValid() )
            return 0;

        markEdited();
        juce::ValueTree e( PatchIDs::FX );
        const auto      id = nextId();
        e.setProperty( PatchIDs::id, id, nullptr );
        e.setProperty( PatchIDs::type, Type, nullptr );
        e.setProperty( PatchIDs::slot, Slot, nullptr );
        patch.appendChild( e, um );
        return id;
    }

    bool PatchEditor::isAudioModule( int Id ) const
    {
        const auto  m    = module( Id );
        const auto* spec = specOf( m );
        return m.hasType( PatchIDs::MODULE ) && spec && ( spec->role == Role::Source || spec->role == Role::Processor );
    }

    juce::ValueTree PatchEditor::addCable( int Target, const juce::String& Source, float Amount )
    {
        const auto sourceId = Source.fromFirstOccurrenceOf( ":", false, false ).getIntValue();
        if( !isAudioModule( Target ) || !Source.startsWith( "module:" ) || !isAudioModule( sourceId ) || sourceId == Target )
            return {};

        markEdited();
        juce::ValueTree c( PatchIDs::CABLE );
        c.setProperty( PatchIDs::source, Source, nullptr );
        c.setProperty( PatchIDs::amount, juce::jlimit( -1.f, 1.f, Amount ), nullptr );
        module( Target ).appendChild( c, um );
        return c;
    }

    void PatchEditor::setCableAmount( juce::ValueTree Cable, float Amount )
    {
        if( !Cable.isValid() )
            return;
        markEdited();
        Cable.setProperty( PatchIDs::amount, juce::jlimit( -1.f, 1.f, Amount ), um );
    }

    std::vector<std::pair<juce::String, juce::String>> PatchEditor::getCableSources( int Target ) const
    {
        std::vector<juce::ValueTree> audio;
        for( const auto& m : patch )
            if( const int id = m.getProperty( PatchIDs::id ); id != Target && isAudioModule( id ) )
                audio.push_back( m );
        std::sort( audio.begin(), audio.end(), gridBefore );

        std::vector<std::pair<juce::String, juce::String>> sources;
        for( const auto& m : audio )
        {
            const int id = m.getProperty( PatchIDs::id );
            sources.push_back( { PatchBuilder::module( id ), moduleLabel( id ) } );
        }
        return sources;
    }

    juce::String PatchEditor::macroLabel( int Macro ) const
    {
        if( const auto stored = patch.getProperty( PatchIDs::macroName( Macro ) ).toString(); stored.isNotEmpty() )
            return stored;

        // What it modulates, in grid order, then the effects.
        std::vector<juce::ValueTree> nodes;
        for( const auto& m : patch )
            if( m.hasType( PatchIDs::MODULE ) )
                nodes.push_back( m );
        std::sort( nodes.begin(), nodes.end(), gridBefore );
        for( const auto& m : patch )
            if( m.hasType( PatchIDs::FX ) )
                nodes.push_back( m );

        const auto        source = builtinSourceId( kMacro1 + Macro );
        juce::StringArray targets;
        for( const auto& m : nodes )
            if( const auto* spec = specOf( m ) )
                for( const auto& r : m )
                    if( r.hasType( PatchIDs::MOD ) && r.getProperty( PatchIDs::source ).toString() == source )
                        if( const auto pi = spec->findParam( r.getProperty( PatchIDs::param ).toString() ); pi >= 0 )
                            targets.addIfNotAlreadyThere( spec->params[(size_t)pi].name );

        if( targets.isEmpty() )
            return "Macro " + juce::String( Macro + 1 );
        return targets[0] + ( targets.size() > 1 ? " +" + juce::String( targets.size() - 1 ) : juce::String() );
    }

    void PatchEditor::setMacroName( int Macro, const juce::String& Name )
    {
        if( !patch.isValid() || !juce::isPositiveAndBelow( Macro, 8 ) )
            return;
        markEdited();
        if( Name.trim().isEmpty() )
            patch.removeProperty( PatchIDs::macroName( Macro ), um );
        else
            patch.setProperty( PatchIDs::macroName( Macro ), Name.trim(), um );
    }

    juce::ValueTree PatchEditor::moduleAt( int Col, int Row ) const
    {
        for( const auto& m : patch )
            if( m.hasType( PatchIDs::MODULE ) && (int)m.getProperty( PatchIDs::col ) == Col && (int)m.getProperty( PatchIDs::row ) == Row )
                return m;
        return {};
    }

    const ModuleSpec* PatchEditor::specOf( const juce::ValueTree& Module ) { return findModuleSpec( Module.getProperty( PatchIDs::type ).toString() ); }

    int PatchEditor::nextId() const
    {
        int id = 0;
        for( const auto& m : patch )
            id = juce::jmax( id, (int)m.getProperty( PatchIDs::id ) );
        return id + 1;
    }

    void PatchEditor::markEdited()
    {
        // Once: a preset (factory or the user's) no longer reads as the untouched one.
        const auto name = patch.getProperty( PatchIDs::name ).toString();
        if( name.isNotEmpty() && !name.endsWith( " *" ) )
            patch.setProperty( PatchIDs::name, name + " *", um );
    }

    int PatchEditor::addModule( const juce::String& Type, int Col, int Row )
    {
        if( !patch.isValid() || !findModuleSpec( Type ) || moduleAt( Col, Row ).isValid() )
            return 0;

        markEdited();
        juce::ValueTree m( PatchIDs::MODULE );
        const auto      id = nextId();
        m.setProperty( PatchIDs::id, id, nullptr );
        m.setProperty( PatchIDs::type, Type, nullptr );
        m.setProperty( PatchIDs::col, Col, nullptr );
        m.setProperty( PatchIDs::row, Row, nullptr );
        patch.appendChild( m, um ); // values are left out: they read as the defaults
        return id;
    }

    void PatchEditor::removeModule( int Id )
    {
        auto m = module( Id );
        if( !m.isValid() )
            return;

        markEdited();
        const auto source = PatchBuilder::module( Id );
        for( auto other : patch )
            for( int i = other.getNumChildren(); --i >= 0; )
                if( auto r = other.getChild( i ); ( r.hasType( PatchIDs::MOD ) || r.hasType( PatchIDs::CABLE ) ) && r.getProperty( PatchIDs::source ).toString() == source )
                    other.removeChild( r, um );
        patch.removeChild( m, um );
    }

    void PatchEditor::moveModule( int Id, int Col, int Row )
    {
        auto m = module( Id );
        if( !m.isValid() )
            return;
        const int fromCol = m.getProperty( PatchIDs::col ), fromRow = m.getProperty( PatchIDs::row );
        if( fromCol == Col && fromRow == Row )
            return;

        markEdited();
        if( auto other = moduleAt( Col, Row ); other.isValid() )
        {
            other.setProperty( PatchIDs::col, fromCol, um );
            other.setProperty( PatchIDs::row, fromRow, um );
        }
        m.setProperty( PatchIDs::col, Col, um );
        m.setProperty( PatchIDs::row, Row, um );
    }

    float PatchEditor::getValue( int Id, const juce::String& Param ) const
    {
        const auto  m    = module( Id );
        const auto* spec = specOf( m );
        const auto  p    = spec ? spec->findParam( Param ) : -1;
        if( p < 0 )
            return 0.f;
        const auto& ps = spec->params[(size_t)p];
        return juce::jlimit( ps.minValue, ps.maxValue, (float)m.getProperty( juce::Identifier( Param ), ps.defaultValue ) );
    }

    void PatchEditor::setValue( int Id, const juce::String& Param, float Value )
    {
        auto        m    = module( Id );
        const auto* spec = specOf( m );
        const auto  p    = spec ? spec->findParam( Param ) : -1;
        if( p < 0 )
            return;
        const auto& ps = spec->params[(size_t)p];
        Value          = juce::jlimit( ps.minValue, ps.maxValue, ps.curve == Curve::Steps ? std::round( Value ) : Value );
        if( Value == getValue( Id, Param ) && m.hasProperty( juce::Identifier( Param ) ) )
            return;
        markEdited();
        m.setProperty( juce::Identifier( Param ), Value, um );
    }

    juce::ValueTree PatchEditor::addMod( int Target, const juce::String& Param, const juce::String& Source, float Depth )
    {
        auto        m    = module( Target );
        const auto* spec = specOf( m );
        if( !spec || spec->findParam( Param ) < 0 )
            return {};

        markEdited();
        juce::ValueTree r( PatchIDs::MOD );
        r.setProperty( PatchIDs::param, Param, nullptr );
        r.setProperty( PatchIDs::source, Source, nullptr );
        r.setProperty( PatchIDs::depth, juce::jlimit( -1.f, 1.f, Depth ), nullptr );
        m.appendChild( r, um );
        return r;
    }

    void PatchEditor::setModDepth( juce::ValueTree Mod, float Depth )
    {
        if( !Mod.isValid() )
            return;
        markEdited();
        Mod.setProperty( PatchIDs::depth, juce::jlimit( -1.f, 1.f, Depth ), um );
    }

    void PatchEditor::removeMod( juce::ValueTree Mod )
    {
        if( auto parent = Mod.getParent(); parent.isValid() )
        {
            markEdited();
            parent.removeChild( Mod, um );
        }
    }

    int PatchEditor::getPlayMode() const { return juce::jlimit( (int)Poly, (int)Legato, (int)patch.getProperty( PatchIDs::playMode, (int)Poly ) ); }

    void PatchEditor::setPlayMode( int Mode )
    {
        Mode = juce::jlimit( (int)Poly, (int)Legato, Mode );
        if( Mode == getPlayMode() )
            return;
        markEdited();
        patch.setProperty( PatchIDs::playMode, Mode, um );
    }

    float PatchEditor::getGlide() const { return juce::jlimit( 0.f, maxGlide, (float)patch.getProperty( PatchIDs::glide, 0.f ) ); }

    void PatchEditor::setGlide( float Seconds )
    {
        Seconds = juce::jlimit( 0.f, maxGlide, Seconds );
        if( Seconds == getGlide() )
            return;
        markEdited();
        patch.setProperty( PatchIDs::glide, Seconds, um );
    }

    const juce::StringArray& arpModeNames()
    {
        static const juce::StringArray names{ "Off", "Up", "Down", "Up-Down", "Random", "As Played" };
        return names;
    }

    const juce::StringArray& arpRateNames()
    {
        static const juce::StringArray names{ "1/4", "1/8", "1/16", "1/32", "1/8 T", "1/16 T" };
        return names;
    }

    double arpRateBeats( int Rate )
    {
        static constexpr std::array<double, 6> beats{ 1, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
        return beats[(size_t)juce::jlimit( 0, (int)beats.size() - 1, Rate )];
    }

    int   PatchEditor::getArp() const { return juce::jlimit( (int)ArpOff, (int)ArpAsPlayed, (int)patch.getProperty( PatchIDs::arp, (int)ArpOff ) ); }
    int   PatchEditor::getArpRate() const { return juce::jlimit( 0, arpRateNames().size() - 1, (int)patch.getProperty( PatchIDs::arpRate, 2 ) ); }
    int   PatchEditor::getArpOctaves() const { return juce::jlimit( 1, 4, (int)patch.getProperty( PatchIDs::arpOctaves, 1 ) ); }
    float PatchEditor::getArpGate() const { return juce::jlimit( 0.05f, 1.f, (float)patch.getProperty( PatchIDs::arpGate, 0.5f ) ); }

    void PatchEditor::setArp( int Mode, int Rate, int Octaves, float Gate )
    {
        if( !patch.isValid() )
            return;
        markEdited();
        if( Mode >= 0 )
            patch.setProperty( PatchIDs::arp, juce::jlimit( (int)ArpOff, (int)ArpAsPlayed, Mode ), um );
        if( Rate >= 0 )
            patch.setProperty( PatchIDs::arpRate, juce::jlimit( 0, arpRateNames().size() - 1, Rate ), um );
        if( Octaves >= 0 )
            patch.setProperty( PatchIDs::arpOctaves, juce::jlimit( 1, 4, Octaves ), um );
        if( Gate >= 0.f )
            patch.setProperty( PatchIDs::arpGate, juce::jlimit( 0.05f, 1.f, Gate ), um );
    }

    void PatchEditor::moveEffect( int Id, int Slot )
    {
        auto e = module( Id );
        if( !e.hasType( PatchIDs::FX ) || !juce::isPositiveAndBelow( Slot, effectSlots ) )
            return;
        const int from = e.getProperty( PatchIDs::slot );
        if( from == Slot )
            return;
        markEdited();
        if( auto other = effectAt( Slot ); other.isValid() )
            other.setProperty( PatchIDs::slot, from, um );
        e.setProperty( PatchIDs::slot, Slot, um );
    }

    int PatchEditor::getVoices() const { return juce::jlimit( 1, maxVoices, (int)patch.getProperty( PatchIDs::voices, defaultVoices ) ); }

    void PatchEditor::setVoices( int Voices )
    {
        Voices = juce::jlimit( 1, maxVoices, Voices );
        if( Voices == getVoices() )
            return;
        markEdited();
        patch.setProperty( PatchIDs::voices, Voices, um );
    }

    std::vector<std::pair<juce::String, juce::String>> PatchEditor::getSources( bool GlobalOnly ) const
    {
        std::vector<std::pair<juce::String, juce::String>> sources;
        for( int s = 0; s < kNumBuiltinSources; ++s )
            if( !GlobalOnly || isGlobalSource( s ) )
            {
                // A macro shows what it already does: "Macro 1 (Cutoff)".
                auto name = builtinSourceName( s );
                if( s >= kMacro1 )
                    if( const auto label = macroLabel( s - kMacro1 ); label != name )
                        name << " (" << label << ")";
                sources.push_back( { builtinSourceId( s ), name } );
            }
        if( GlobalOnly )
            return sources;

        std::vector<juce::ValueTree> modulators;
        for( const auto& m : patch )
            if( const auto* spec = specOf( m ); spec && spec->role == Role::Modulator )
                modulators.push_back( m );
        std::sort( modulators.begin(), modulators.end(), gridBefore );
        for( const auto& m : modulators )
        {
            const int id = m.getProperty( PatchIDs::id );
            sources.push_back( { PatchBuilder::module( id ), moduleLabel( id ) } );
        }
        return sources;
    }

    juce::String PatchEditor::moduleLabel( int Id ) const
    {
        const auto  m    = module( Id );
        const auto* spec = specOf( m );
        if( !spec )
            return {};

        int number = 1;
        for( const auto& other : patch )
            if( other.hasType( PatchIDs::MODULE ) && other.getProperty( PatchIDs::type ) == m.getProperty( PatchIDs::type ) && gridBefore( other, m ) )
                ++number;
        return spec->name + " " + juce::String( number );
    }

    juce::String PatchEditor::sourceLabel( const juce::String& Source ) const
    {
        if( const auto b = findBuiltinSource( Source ); b >= 0 )
            return builtinSourceName( b );
        if( Source.startsWith( "module:" ) )
            if( const auto label = moduleLabel( Source.fromFirstOccurrenceOf( ":", false, false ).getIntValue() ); label.isNotEmpty() )
                return label;
        return "?";
    }

    //==============================================================================
    namespace
    {
        juce::File& presetDirOverride()
        {
            static juce::File dir;
            return dir;
        }
    } // namespace

    juce::File UserPresets::getDirectory()
    {
        const auto& over = presetDirOverride();
        return over != juce::File() ? over : getAppDirs( MTPrefs ).getChildFile( "ModularPresets" );
    }

    void UserPresets::setDirectory( const juce::File& Dir ) { presetDirOverride() = Dir; }

    juce::StringArray UserPresets::getNames()
    {
        juce::StringArray names;
        for( const auto& f : getDirectory().findChildFiles( juce::File::findFiles, false, "*.xml" ) )
            if( auto xml = juce::XmlDocument::parse( f ); xml && xml->hasTagName( PatchIDs::MODULAR_PATCH.toString() ) )
                names.addIfNotAlreadyThere( xml->getStringAttribute( PatchIDs::name.toString() ) );
        names.removeEmptyStrings();
        names.sortNatural();
        return names;
    }

    juce::ValueTree UserPresets::load( const juce::String& Name )
    {
        for( const auto& f : getDirectory().findChildFiles( juce::File::findFiles, false, "*.xml" ) )
            if( auto xml = juce::XmlDocument::parse( f ); xml && xml->hasTagName( PatchIDs::MODULAR_PATCH.toString() ) )
                if( xml->getStringAttribute( PatchIDs::name.toString() ) == Name )
                    return juce::ValueTree::fromXml( *xml );
        return {};
    }

    bool UserPresets::save( const juce::ValueTree& Patch, const juce::String& Name )
    {
        const auto name = Name.trim();
        if( name.isEmpty() || !Patch.hasType( PatchIDs::MODULAR_PATCH ) || getPresetNames().contains( name ) )
            return false;

        auto copy = Patch.createCopy();
        copy.setProperty( PatchIDs::name, name, nullptr );
        const auto dir = getDirectory();
        if(dir.createDirectory().failed()) return false;
        auto legalName = juce::File::createLegalFileName(name).substring(0, 100);
        if(legalName.isEmpty()) legalName = "Preset";
        auto target = dir.getChildFile(legalName + ".xml").getNonexistentSibling();
        juce::Array<juce::File> previous;
        for(const auto& file : dir.findChildFiles(juce::File::findFiles, false, "*.xml"))
            if(auto xml = juce::XmlDocument::parse(file); xml && xml->hasTagName(PatchIDs::MODULAR_PATCH.toString()) && xml->getStringAttribute(PatchIDs::name.toString()) == name)
                previous.add(file);
        if(!previous.isEmpty()) target = previous[0];
        juce::TemporaryFile temporary(target);
        auto xml = copy.createXml();
        if(!xml || !xml->writeTo(temporary.getFile()) || !temporary.overwriteTargetFileWithTemporary()) return false;
        for(const auto& file : previous) if(file != target) file.deleteFile();
        return true;
    }

    bool UserPresets::remove( const juce::String& Name )
    {
        bool removed = false;
        for( const auto& f : getDirectory().findChildFiles( juce::File::findFiles, false, "*.xml" ) )
            if( auto xml = juce::XmlDocument::parse( f ); xml && xml->hasTagName(PatchIDs::MODULAR_PATCH.toString()) && xml->getStringAttribute( PatchIDs::name.toString() ) == Name )
                removed = f.deleteFile() || removed;
        return removed;
    }
} // namespace mt::modular

/*
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

namespace mt::modular
{
    PatchDesc PatchDesc::fromTree( const juce::ValueTree& Patch )
    {
        PatchDesc d;
        d.name   = Patch.getProperty( PatchIDs::name ).toString();
        d.voices = juce::jlimit( 1, maxVoices, (int)Patch.getProperty( PatchIDs::voices, defaultVoices ) );

        for( const auto& m : Patch )
        {
            if( !m.hasType( PatchIDs::MODULE ) )
                continue;
            const auto* spec = findModuleSpec( m.getProperty( PatchIDs::type ).toString() );
            if( !spec )
                continue; // from a newer build, or a typo: skip it rather than fail the patch

            Module mod;
            mod.id   = (int)m.getProperty( PatchIDs::id );
            mod.spec = spec;
            mod.col  = (int)m.getProperty( PatchIDs::col );
            mod.row  = (int)m.getProperty( PatchIDs::row );
            for( const auto& p : spec->params )
                mod.values.push_back( juce::jlimit( p.minValue, p.maxValue, (float)m.getProperty( juce::Identifier( p.id ), p.defaultValue ) ) );

            for( const auto& r : m )
                if( r.hasType( PatchIDs::MOD ) )
                    mod.mods.push_back( { r.getProperty( PatchIDs::param ).toString(), r.getProperty( PatchIDs::source ).toString(),
                                          juce::jlimit( -1.f, 1.f, (float)r.getProperty( PatchIDs::depth ) ) } );

            d.modules.push_back( std::move( mod ) );
        }

        std::stable_sort( d.modules.begin(), d.modules.end(), []( const Module& A, const Module& B ) {
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
            return b.tree;
        }

        struct Preset
        {
            const char* name;
            juce::ValueTree ( *make )();
        };

        const std::array<Preset, 8> presets{ {
            { "Init", init },
            { "Basic Poly", basicPoly },
            { "Pluck String", pluckString },
            { "FM Bell", fmBell },
            { "Wobble Bass", wobbleBass },
            { "Fold Lead", foldLead },
            { "Noise Hat", noiseHat },
            { "Random Pad", randomPad },
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
} // namespace mt::modular

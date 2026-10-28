// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Screen.h"

#include <cmath>

namespace oscairn::standalone
{
    namespace
    {
        // Sequencer parameter ids (plugins/step-sequencer): kind * 100 + step.
        constexpr uint32_t onParam = 100, noteParam = 200, lockParam = 300;

        const juce::Colour background{ 0xff0d0f12 }, panel{ 0xff1c2027 }, text{ 0xffe8eaed }, dim{ 0xff7a828e },
            accent{ 0xffff8a3d }, active{ 0xff3ddc97 };

        juce::String noteName( int note )
        {
            return juce::MidiMessage::getMidiNoteName( note, true, true, 4 ); // middle C (60) is C4
        }
    }

    void Screen::refresh()
    {
        status_ = engine_.status();
        for( int i = 0; i < steps; ++i )
        {
            on_[(size_t)i]   = engine_.param( "sequencer", onParam + (uint32_t)i ) >= 0.5;
            note_[(size_t)i] = (int)engine_.param( "sequencer", noteParam + (uint32_t)i );
            lock_[(size_t)i] = engine_.param( "sequencer", lockParam + (uint32_t)i );
        }
        repaint();
    }

    juce::Rectangle<float> Screen::header() const
    {
        return getLocalBounds().toFloat().removeFromTop( getHeight() * 0.12f );
    }

    juce::Rectangle<float> Screen::cell( int step ) const
    {
        auto       area = getLocalBounds().toFloat().withTrimmedTop( header().getHeight() ).reduced( getWidth() * 0.015f );
        const auto w = area.getWidth() / 8.0f, h = area.getHeight() / 2.0f;
        return juce::Rectangle<float>( area.getX() + ( step % 8 ) * w, area.getY() + ( step / 8 ) * h, w, h ).reduced( w * 0.05f );
    }

    juce::Rectangle<float> Screen::lockBar( int step ) const
    {
        auto c = cell( step );
        return c.removeFromBottom( c.getHeight() * 0.3f ).reduced( c.getWidth() * 0.12f, c.getHeight() * 0.04f );
    }

    int Screen::stepAt( juce::Point<float> p ) const
    {
        for( int i = 0; i < steps; ++i )
            if( cell( i ).contains( p ) )
                return i;
        return -1;
    }

    void Screen::paint( juce::Graphics& g )
    {
        g.fillAll( background );
        const float unit = getHeight() / 600.0f; // the KICK's screen is 1024 x 600

        auto top = header().reduced( 14.0f * unit, 0.0f );
        g.setColour( text );
        g.setFont( juce::FontOptions( 30.0f * unit, juce::Font::bold ) );
        g.drawText( "GROOVEBOX", top, juce::Justification::centredLeft );
        g.setFont( juce::FontOptions( 22.0f * unit ) );
        const auto midi = status_.midiInputs.empty() ? juce::String( "no MIDI inputs" )
                                                     : juce::String( (int)status_.midiInputs.size() ) + " MIDI in";
        const auto state = juce::String( status_.playing ? "PLAYING" : "STOPPED" ) + "   " + juce::String( status_.tempo, 1 ) + " BPM"
                         + ( engine_.followsMidiClock() ? "  (MIDI clock)" : "" ) + "   " + midi;
        g.setColour( status_.playing ? active : dim );
        g.drawText( state, top, juce::Justification::centredRight );

        for( int i = 0; i < steps; ++i )
        {
            const auto c       = cell( i );
            const bool playing = i == status_.step;
            g.setColour( on_[(size_t)i] ? panel.brighter( 0.15f ) : panel );
            g.fillRoundedRectangle( c, 8.0f * unit );
            if( playing )
            {
                g.setColour( accent );
                g.drawRoundedRectangle( c.reduced( 1.5f * unit ), 8.0f * unit, 3.0f * unit );
            }

            auto inner = c.reduced( 10.0f * unit );
            g.setColour( dim );
            g.setFont( juce::FontOptions( 18.0f * unit ) );
            g.drawText( juce::String( i + 1 ), inner.removeFromTop( 22.0f * unit ), juce::Justification::topLeft );
            g.setColour( on_[(size_t)i] ? text : dim.darker( 0.5f ) );
            g.setFont( juce::FontOptions( 34.0f * unit, juce::Font::bold ) );
            g.drawText( on_[(size_t)i] ? noteName( note_[(size_t)i] ) : juce::String( "-" ), inner.withTrimmedBottom( c.getHeight() * 0.3f ),
                        juce::Justification::centred );

            const auto bar = lockBar( i );
            g.setColour( background );
            g.fillRoundedRectangle( bar, 4.0f * unit );
            g.setColour( on_[(size_t)i] ? accent.withAlpha( 0.85f ) : dim.withAlpha( 0.4f ) );
            g.fillRoundedRectangle( bar.withWidth( bar.getWidth() * (float)lock_[(size_t)i] ), 4.0f * unit );
        }
    }

    void Screen::setLock( int step, float x )
    {
        const auto bar   = lockBar( step );
        const auto value = juce::jlimit( 0.0f, 1.0f, ( x - bar.getX() ) / bar.getWidth() );
        engine_.setParam( "sequencer", lockParam + (uint32_t)step, value );
        refresh();
    }

    void Screen::mouseDown( const juce::MouseEvent& e )
    {
        dragStep_  = stepAt( e.position );
        dragMoved_ = false;
        if( dragStep_ < 0 )
            drag_ = Drag::None;
        else if( lockBar( dragStep_ ).expanded( 0.0f, 6.0f ).contains( e.position ) )
        {
            drag_ = Drag::Lock;
            setLock( dragStep_, e.position.x );
        }
        else
        {
            drag_     = Drag::Toggle; // becomes a note change if it moves
            dragNote_ = note_[(size_t)dragStep_];
        }
    }

    void Screen::mouseDrag( const juce::MouseEvent& e )
    {
        if( drag_ == Drag::Lock )
            setLock( dragStep_, e.position.x );
        else if( drag_ != Drag::None && std::abs( e.getDistanceFromDragStartY() ) > 4 )
        {
            drag_            = Drag::Note;
            const auto pitch = juce::jlimit( 0, 127, dragNote_ - (int)std::lround( e.getDistanceFromDragStartY() / ( 8.0f * getHeight() / 600.0f ) ) );
            engine_.setParam( "sequencer", noteParam + (uint32_t)dragStep_, pitch );
            refresh();
        }
    }

    void Screen::mouseUp( const juce::MouseEvent& )
    {
        if( drag_ == Drag::Toggle )
        {
            engine_.setParam( "sequencer", onParam + (uint32_t)dragStep_, on_[(size_t)dragStep_] ? 0.0 : 1.0 );
            refresh();
        }
        drag_ = Drag::None;
    }

    void Screen::mouseWheelMove( const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel )
    {
        const int step = stepAt( e.position );
        if( step < 0 || wheel.deltaY == 0.0f )
            return;
        const auto pitch = juce::jlimit( 0, 127, note_[(size_t)step] + ( wheel.deltaY > 0 ? 1 : -1 ) );
        engine_.setParam( "sequencer", noteParam + (uint32_t)step, pitch );
        refresh();
    }
}

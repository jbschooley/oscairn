// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Layout.h"

#include <juce_core/juce_core.h>

#include <stdexcept>

namespace oscairn::standalone
{
    namespace
    {
        [[noreturn]] void invalid( const juce::String& why )
        {
            throw std::runtime_error( ( "layout: " + why ).toStdString() );
        }

        juce::Rectangle<int> rectOf( const juce::var& v, const juce::String& where )
        {
            const auto* a = v.getArray();
            if( !a || a->size() != 4 )
                invalid( where + ": \"rect\" must be [x, y, width, height]" );
            return { (int)( *a )[0], (int)( *a )[1], (int)( *a )[2], (int)( *a )[3] };
        }

        Layout::Anchor anchorOf( const juce::var& v, const juce::String& where )
        {
            Layout::Anchor anchor;
            if( v.isVoid() )
                return anchor;
            const auto words = juce::StringArray::fromTokens( v.toString(), false );
            auto edge = [&]( const juce::String& word, bool horizontal ) {
                if( word == "stretch" ) return Layout::Anchor::Stretch;
                if( word == "centre" || word == "center" ) return Layout::Anchor::Centre;
                if( word == ( horizontal ? "left" : "top" ) ) return Layout::Anchor::Start;
                if( word == ( horizontal ? "right" : "bottom" ) ) return Layout::Anchor::End;
                invalid( where + ": anchor \"" + v.toString() + "\" should be \"left|right|centre|stretch top|bottom|centre|stretch\"" );
            };
            if( words.size() != 2 )
                invalid( where + ": anchor needs a horizontal and a vertical part, like \"right bottom\"" );
            anchor.x = edge( words[0], true );
            anchor.y = edge( words[1], false );
            return anchor;
        }

        Layout::Control::Type typeOf( const juce::String& name, const juce::String& where )
        {
            using Type = Layout::Control::Type;
            if( name == "screen" ) return Type::Screen;
            if( name == "knob" ) return Type::Knob;
            if( name == "button" ) return Type::Button;
            if( name == "pads" ) return Type::Pads;
            if( name == "keys" ) return Type::Keys;
            if( name == "label" ) return Type::Label;
            invalid( where + ": unknown type \"" + name + "\"" );
        }
    }

    juce::Rectangle<int> Layout::Anchor::place( juce::Rectangle<int> r, int dx, int dy ) const
    {
        auto along = []( Edge edge, int start, int size, int extra, int& newStart, int& newSize ) {
            newStart = start + ( edge == End ? extra : edge == Centre ? extra / 2 : 0 );
            newSize  = size + ( edge == Stretch ? extra : 0 );
        };
        int newX, newWidth, newY, newHeight;
        along( x, r.getX(), r.getWidth(), dx, newX, newWidth );
        along( y, r.getY(), r.getHeight(), dy, newY, newHeight );
        return { newX, newY, newWidth, newHeight };
    }

    Layout Layout::parse( const juce::String& json )
    {
        juce::var root;
        if( const auto result = juce::JSON::parse( json, root ); result.failed() )
            invalid( result.getErrorMessage() );
        if( !root.isObject() )
            invalid( "not a JSON object" );

        Layout layout;
        layout.name = root["name"].toString().toStdString();
        const auto* size = root["size"].getArray();
        if( !size || size->size() != 2 || (int)( *size )[0] <= 0 || (int)( *size )[1] <= 0 )
            invalid( "\"size\" must be [width, height]" );
        layout.width  = ( *size )[0];
        layout.height = ( *size )[1];
        if( root.hasProperty( "resize" ) )
        {
            const auto resize = root["resize"].toString();
            if( resize == "fixed" ) layout.resize = Resize::Fixed;
            else if( resize == "scale" ) layout.resize = Resize::Scale;
            else if( resize == "stretch" ) layout.resize = Resize::Stretch;
            else invalid( "\"resize\" must be fixed, scale or stretch" );
        }
        if( root.hasProperty( "background" ) )
            layout.background = juce::Colour::fromString( root["background"].toString().replace( "#", "ff" ) );

        if( const auto* panels = root["panels"].getArray() )
            for( int i = 0; i < panels->size(); ++i )
            {
                const auto& p = ( *panels )[i];
                const auto  where = "panel " + juce::String( i );
                layout.panels.push_back( { rectOf( p["rect"], where ), p["label"].toString().toStdString(), anchorOf( p["anchor"], where ) } );
            }

        const auto* controls = root["controls"].getArray();
        if( !controls )
            invalid( "\"controls\" must be a list" );
        bool screen = false;
        for( int i = 0; i < controls->size(); ++i )
        {
            const auto&  c     = ( *controls )[i];
            const auto   where = "control " + juce::String( i ) + ( c["label"].isVoid() ? "" : " (" + c["label"].toString() + ")" );
            Control      control;
            control.type  = typeOf( c["type"].toString(), where );
            control.rect  = rectOf( c["rect"], where );
            control.label = c["label"].toString().toStdString();
            control.bind  = c["bind"].toString().toStdString();
            control.anchor = anchorOf( c["anchor"], where );
            if( c.hasProperty( "rows" ) ) control.rows = c["rows"];
            if( c.hasProperty( "cols" ) ) control.cols = c["cols"];
            if( c.hasProperty( "note" ) ) control.note = c["note"];
            if( c.hasProperty( "count" ) ) control.count = c["count"];
            if( c.hasProperty( "steps" ) ) control.steps = c["steps"];
            if( c.hasProperty( "mode" ) )
            {
                const auto mode = c["mode"].toString();
                if( mode != "endless" && mode != "absolute" )
                    invalid( where + ": \"mode\" must be endless or absolute" );
                control.endless = mode == "endless";
            }
            if( c.hasProperty( "align" ) )
            {
                const auto align = c["align"].toString();
                if( align == "left" ) control.align = juce::Justification::centredLeft;
                else if( align == "centre" || align == "center" ) control.align = juce::Justification::centred;
                else if( align == "right" ) control.align = juce::Justification::centredRight;
                else invalid( where + ": \"align\" must be left, centre or right" );
            }
            if( control.rows < 1 || control.cols < 1 || control.count < 1 || control.note < 0 || control.note > 127 )
                invalid( where + ": rows, cols and count must be positive, note 0-127" );
            if( control.type == Control::Type::Screen )
            {
                if( screen )
                    invalid( "more than one screen" );
                screen = true;
            }
            layout.controls.push_back( control );
        }
        return layout;
    }
}

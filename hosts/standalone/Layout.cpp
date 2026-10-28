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
        if( root.hasProperty( "resizable" ) )
            layout.resizable = root["resizable"];
        if( root.hasProperty( "background" ) )
            layout.background = juce::Colour::fromString( root["background"].toString().replace( "#", "ff" ) );

        if( const auto* panels = root["panels"].getArray() )
            for( int i = 0; i < panels->size(); ++i )
            {
                const auto& p = ( *panels )[i];
                layout.panels.push_back( { rectOf( p["rect"], "panel " + juce::String( i ) ), p["label"].toString().toStdString() } );
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
            if( c.hasProperty( "rows" ) ) control.rows = c["rows"];
            if( c.hasProperty( "cols" ) ) control.cols = c["cols"];
            if( c.hasProperty( "note" ) ) control.note = c["note"];
            if( c.hasProperty( "count" ) ) control.count = c["count"];
            if( c.hasProperty( "steps" ) ) control.steps = c["steps"];
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

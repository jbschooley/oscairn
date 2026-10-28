// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <map>
#include <optional>

namespace oscairn::standalone
{
    /// What each pointer (a finger on a touchscreen, or the mouse) is holding down, so several
    /// controls can be held at once. Pointers are told apart by an index (JUCE's
    /// MouseInputSource index). Two fingers can hold the same control: it is pressed by the
    /// first and released by the last.
    template<class Control>
    class Presses
    {
    public:
        /// `pointer` presses `control`. True if nothing else was holding it (start the note).
        /// A pointer already holding something releases it first; see release().
        bool press( int pointer, Control control )
        {
            release( pointer );
            const bool first = !held( control );
            held_[pointer]   = control;
            return first;
        }

        /// `pointer` lets go. Returns what it held if no other pointer still holds it (end the
        /// note), otherwise nothing.
        std::optional<Control> release( int pointer )
        {
            const auto it = held_.find( pointer );
            if( it == held_.end() )
                return std::nullopt;
            const auto control = it->second;
            held_.erase( it );
            if( held( control ) )
                return std::nullopt;
            return control;
        }

        bool held( Control control ) const
        {
            for( const auto& [pointer, c] : held_ )
                if( c == control )
                    return true;
            return false;
        }

        bool empty() const { return held_.empty(); }

    private:
        std::map<int, Control> held_;
    };
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/chain.h"

namespace oscairn
{
    void Chain::add( const std::string& path, const std::string& pluginId )
    {
        const NodeId id   = graph_.add( path, pluginId );
        const auto&  host = graph_.host( id );

        if( host.noteInputPorts() > 0 )
        {
            if( nodes_.empty() )
                graph_.connectNotes( graphInput, id );
            else if( graph_.host( nodes_.back() ).noteOutputPorts() > 0 )
                graph_.connectNotes( nodes_.back(), id );
        }
        if( lastAudio_ && !host.inputPorts().empty() )
            graph_.connectAudio( lastAudio_, id );
        if( !host.outputPorts().empty() )
        {
            if( lastAudio_ )
                graph_.disconnectAudio( lastAudio_, graphOutput );
            graph_.connectAudio( id, graphOutput );
            lastAudio_ = id;
        }

        nodes_.push_back( id );
        graph_.commit();
    }
}

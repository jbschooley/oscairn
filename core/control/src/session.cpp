// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"

#include "oscairn/library.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iterator>
#include <cmath>
#include <stdexcept>

namespace oscairn
{
    Session::Session( double sampleRate, uint32_t maxFrames, Lv2BufferLimits lv2Buffers, AudioLayout audio )
        : sampleRate_( sampleRate ), maxFrames_( maxFrames ), graph_( sampleRate, maxFrames, ProcessingMode::Realtime, lv2Buffers, std::move(audio) ), transport_( sampleRate ), recorder_( sampleRate, maxFrames )
    {
        if( !std::isfinite( sampleRate ) || sampleRate <= 0 || maxFrames == 0 )
            throw std::invalid_argument( "session needs a positive sample rate and block size" );
        graph_.setAssets( &assets_ );
        graph_.setListener( &recorder_ );
        transport_.setPlaying( false );
        baseline_ = std::make_shared<const Model>( model() );
    }

    Session::~Session() { graph_.setListener( nullptr ); if( budget_.cpu > 0 ) saveMeasurements(); armPressure( false ); }

    // With a budget (RFC 0025), a plugin that would take the Performance past it is refused and
    // unloaded again; what this process grew by while an in-process plugin loaded is its memory.
    NodeId Session::add( const std::string& name, const std::string& path, const std::string& pluginId )
    {
        if( !name.empty() && std::any_of(nodes_.begin(),nodes_.end(),[&](const auto& n){ return n.second == name; }) )
            throw std::invalid_argument( "duplicate node name: " + name );
        const auto before = budget_.cpu > 0 ? residentBytes() : 0;
        const auto id = graph_.add( path, pluginId, memoryRoom() ); // one declaring more than is left is refused before it activates
        nodes_.emplace( id, name );
        if( budget_.cpu > 0 )
        {
            if( const auto after = residentBytes(); after > before ) loadedBytes_[id] = after - before;
            if( const auto e = estimate(); !e.fits )
            {
                const auto plugin = graph_.host(id).descriptor().name;
                dropNode(id);
                throw std::invalid_argument( "adding " + (name.empty() ? plugin : name) + ": the Performance " + tooMuch(e) );
            }
            evictOpportunistic(); // committed memory grew
        }
        for( const auto& p : graph_.host(id).parameters() ) observed_[{id,p.id}] = graph_.host(id).parameterValue(p.id);
        staged_ = true;
        return id;
    }

    NodeId Session::addEffectContext(const std::string& name,NodeId source)
    {
        if( !name.empty() && std::any_of(nodes_.begin(),nodes_.end(),[&](const auto& n){ return n.second == name; }) )
            throw std::invalid_argument( "duplicate node name: " + name );
        // A context declaring more than the budget leaves is refused before it activates (RFC 0025).
        auto& engine = graph_.host(source);
        const auto room = memoryRoom();
        if( room ) engine.setMemoryRoom(room);
        NodeId id;
        try { id = graph_.addEffectContext(source); } catch(...) { if( room ) engine.setMemoryRoom(0); throw; }
        if( room ) engine.setMemoryRoom(0);
        nodes_.emplace( id, name );
        if( budget_.cpu > 0 )
            if( const auto e = estimate(); !e.fits )
            {
                dropNode(id);
                throw std::invalid_argument( "adding " + (name.empty() ? graph_.host(source).descriptor().name : name) + ": the Performance " + tooMuch(e) );
            }
        for( const auto& p : graph_.host(id).parameters() ) observed_[{id,p.id}] = graph_.host(id).parameterValue(p.id);
        staged_ = true;
        return id;
    }

    NodeId Session::nodeId( const std::string& name ) const
    {
        const auto it = std::find_if(nodes_.begin(),nodes_.end(),[&](const auto& n){ return !name.empty() && n.second == name; });
        if( it == nodes_.end() )
            throw std::invalid_argument( "no node " + name );
        return it->first;
    }

    bool Session::hasParameter( NodeId node, uint32_t param ) const
    {
        if( !graph_.contains( node ) )
            return false;
        const auto& all = graph_.host( node ).parameters();
        return std::any_of( all.begin(), all.end(), [param]( const auto& p ) { return p.id == param; } );
    }

    const kick::clap::Parameter& Session::findParameter( NodeId node, uint32_t param ) const
    {
        if( !graph_.contains( node ) )
            throw std::invalid_argument( "no node " + std::to_string( node ) );
        for( const auto& p : graph_.host( node ).parameters() )
            if( p.id == param )
                return p;
        throw std::invalid_argument( "no parameter " + std::to_string( param ) + " on node " + std::to_string( node ) );
    }

    Control::ParamInfo Session::describe( const kick::clap::Parameter& p )
    {
        return { p.id, p.name, p.minimum, p.maximum, p.defaultValue, ( p.flags & CLAP_PARAM_IS_STEPPED ) != 0, p.flags, p.module, p.widget, p.priority, p.rangeHigh, p.group, p.role };
    }

    std::vector<Control::ParamInfo> Session::parameters( NodeId node ) const
    {
        if( !graph_.contains( node ) )
            throw std::invalid_argument( "no node " + std::to_string( node ) );
        std::vector<ParamInfo> result;
        for( const auto& p : graph_.host( node ).parameters() )
            result.push_back( describe( p ) );
        return result;
    }

    Control::ParamInfo Session::parameterInfo( NodeId node, uint32_t param ) const
    {
        return describe( findParameter( node, param ) );
    }

    double Session::parameter( NodeId node, uint32_t param ) const
    {
        findParameter( node, param );
        if( const auto it = pending_.find({node,param}); it != pending_.end() )
            for( auto command = it->second.rbegin(); command != it->second.rend(); ++command )
                if( command->kind == ChangeKind::Value ) return command->value;
        return graph_.host( node ).parameterValue( param );
    }

    std::string Session::parameterText( NodeId node, uint32_t param, double value ) const
    {
        findParameter( node, param );
        if( !std::isfinite( value ) )
            throw std::invalid_argument( "parameter value must be finite" );
        return graph_.host( node ).parameterText( param, value );
    }

    bool Session::applyParameter( NodeId node, uint32_t param, double value, double* applied, bool bounded )
    {
        const auto& p = findParameter( node, param );
        if( !std::isfinite( value ) || ( p.flags & CLAP_PARAM_IS_READONLY ) != 0 )
            throw std::invalid_argument( "parameter value must be finite and parameter writable" );
        value = std::clamp(value,p.minimum,p.maximum);
        if( p.flags & CLAP_PARAM_IS_STEPPED ) value = std::trunc(value);
        if( applied ) *applied = value;
        return queue(node,param,ChangeKind::Value,value,bounded);
    }

    void Session::setTempo( double bpm )
    {
        if( !std::isfinite( bpm ) || bpm < 1.0 || bpm > 1000.0 )
            throw std::invalid_argument( "tempo must be between 1 and 1000 BPM" );
        tempo_.store( bpm );
    }

    Control::TransportState Session::transportState() const
    {
        return { statusPlaying_.load(), statusTempo_.load(), statusBeat_.load(), clockOutputs_ };
    }

    bool Session::note( bool on, int key, double velocity, int channel )
    {
        if( key < 0 || key > 127 || channel < 0 || channel > 15 || !std::isfinite( velocity ) || velocity < 0 || velocity > 1 )
            throw std::invalid_argument( "invalid note key, channel or velocity" );
        return enqueue( kick::clap::Event::note( on && velocity > 0, key, velocity, 0, channel ) );
    }

    bool Session::enqueue( const kick::clap::Event& event )
    {
        const auto head = noteHead_.load( std::memory_order_relaxed );
        if( head - noteTail_.load( std::memory_order_acquire ) == notes_.size() )
        {
            droppedNotes_.fetch_add( 1 );
            panic_.store( true, std::memory_order_release );
            return false;
        }
        notes_[head % notes_.size()] = event;
        noteHead_.store( head + 1, std::memory_order_release );
        return true;
    }

    void Session::setPadMap( const PadMap& map )
    {
        if( !map.valid() ) throw std::invalid_argument( "invalid pad map" );
        padMap_ = map; // held pads keep the notes they started
    }

    bool Session::pad( uint32_t index, bool on, double velocity, int channel )
    {
        if( index >= PadMap::pads || channel < 0 || channel > 15 || !std::isfinite( velocity ) || velocity < 0 || velocity > 1 )
            throw std::invalid_argument( "invalid pad, channel or velocity" );
        auto& held = heldPads_[index];
        bool ok = true;
        if( held.key >= 0 ) { ok = note( false, held.key, 0, held.channel ); held.key = -1; } // a release, or a press without one
        if( !on ) return ok;
        const auto& a = padMap_.assignments[padMap_.bank][index];
        held = { a.note, int8_t( a.channel >= 0 ? a.channel : channel ), a.pressure };
        return note( true, a.note, a.velocity > 0 ? a.velocity : velocity, held.channel ) && ok;
    }

    bool Session::padPressure( uint32_t index, double pressure )
    {
        if( index >= PadMap::pads || !std::isfinite( pressure ) || pressure < 0 || pressure > 1 ) throw std::invalid_argument( "invalid pad or pressure" );
        const auto& held = heldPads_[index];
        if( held.key < 0 || held.pressure == PadAssignment::Pressure::None ) return true;
        const auto value = uint8_t( std::lround( pressure * 127 ) );
        return enqueue( held.pressure == PadAssignment::Pressure::Poly ? kick::clap::Event::midi( 0xa0 | held.channel, uint8_t( held.key ), value )
                                                                        : kick::clap::Event::midi( 0xd0 | held.channel, value, 0 ) );
    }

    Control::GraphState Session::graphState() const
    {
        std::set<NodeId> ringing;
        std::set_difference(disabled_.begin(), disabled_.end(), rungOut_.begin(), rungOut_.end(), std::inserter(ringing, ringing.end()));
        auto topology = graph_.describe();
        if(!leaving_.empty())
        {
            // The last Performance's plugins ringing out are not part of this one (RFC 0021).
            auto gone = [&](NodeId n) { return leaving_.count(n) != 0; };
            std::erase_if(topology.nodes, [&](const auto& n) { return gone(n.id); });
            std::erase_if(topology.notes, [&](const auto& e) { return gone(e.first) || gone(e.second); });
            std::erase_if(topology.noteZones, [&](const auto& e) { return gone(e.first.first) || gone(e.first.second); });
            std::erase_if(topology.draining, [&](const auto& e) { return gone(e.first) || gone(e.second); });
            std::erase_if(topology.audioRoutes, [&](const auto& r) { return gone(r.from.node) || gone(r.to.node); });
        }
        return {topology,nodes_,revision_,staged_,parts_,wires_,scenes_,untouched_,scene_,macros_,disabled_,ringing};
    }

    void Session::saveState(NodeId node,const std::string& directory)
    {
        auto& host=graph_.host(node);
        service();
        for(const auto& [address,commands]:pending_)if(address.first==node && !commands.empty())
            throw std::runtime_error("Parameter edits are still pending; retry state export after processing");
        host.saveState(directory);
    }

    Control::Sounds Session::sounds(NodeId node) const
    {
        const auto& host=graph_.host(node);Sounds result{host.soundContextCapacity(),{},host.soundContextMode()};
        for(const auto& c:host.soundContexts())result.contexts.push_back({c.id,c.name,c.selected_channels,c.active_voices,c.idle});
        return result;
    }

    Control::SessionStatus Session::sessionStatus() const
    {
        auto result = hostStatus_;
        result.graphEventDrops=graph_.eventDrops();
        if(result.sampleRate == 0) result.sampleRate = sampleRate_;
        if(result.bufferSize == 0) result.bufferSize = maxFrames_;
        for(const auto& [node,name] : nodes_)
        {
            const auto& host = graph_.host(node);
            result.deadlineMisses += host.statistics().deadlineMisses;
            if(auto why = host.failure(); !why.empty()) result.failures.push_back({node, name, std::move(why)});
            if(const auto cpu = host.cpuSeconds(); cpu > 0) result.workers.push_back({node, name, cpu, host.residentBytes()});
        }
        return result;
    }

    void Session::replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset)
    {
        if(staged_)throw std::invalid_argument("Finish pending graph edits before replacing a plugin");
        if(!graph_.contains(node))throw std::invalid_argument("Unknown node");
        if(path.empty() || pluginId.empty())throw std::invalid_argument("Plugin replacement requires an installation path and stable plugin ID");
        // Wires move to the new plugin's ports in the same roles; any that no longer fit stay,
        // unconnected (RFC 0011), so the replacement is never refused for its wiring.
        const auto& old=graph_.host(node);
        const auto inputs=old.inputPorts(),outputs=old.outputPorts();
        auto mains=[&](bool input){try{return old.mainAudioPorts(input);}catch(const std::exception&){return std::vector<uint32_t>{};}};
        const auto mainIn=mains(true),mainOut=mains(false);
        auto parts=parts_;auto wires=wires_;
        try{graph_.replace(node,path,pluginId,preset,[&]{remapPorts(node,inputs,outputs,mainIn,mainOut);compileWires(graph_,wires_,disabled_,rungOut_,leaving_);graph_.setMidiTaps(midiTaps(graph_,parts_,wires_));});}
        catch(...){parts_.swap(parts);wires_.swap(wires);throw;}
        if(disabled_.count(node))graph_.setRetiring(node,true); // the new plugin is off too
        for(auto it=pending_.begin();it!=pending_.end();)
            if(it->first.first==node){commandCount_-=it->second.size();it=pending_.erase(it);}else ++it;
        std::erase_if(observed_,[&](const auto& entry){return entry.first.first==node;});
        outputDrops_.erase(node);parameterRevisions_.erase(node);++revision_;publish(ChangeKind::Graph);
    }

    // Removes a node from the graph and the Session's records of it; its wires are the caller's.
    void Session::dropNode( NodeId node )
    {
        graph_.remove(node);
        nodes_.erase(node);
        disabled_.erase(node); rungOut_.erase(node); loadedBytes_.erase(node);
        std::erase_if(untouched_, [&](const SceneParameter& at) { return at.node == node; });
        outputDrops_.erase(node); parameterRevisions_.erase(node);
        for( auto it = pending_.begin(); it != pending_.end(); )
            if( it->first.first == node ) { commandCount_ -= it->second.size(); it = pending_.erase(it); }
            else ++it;
        std::erase_if(observed_,[&](const auto& entry){ return entry.first.first == node; });
    }

    void Session::commit()
    {
        graph_.commit();
        staged_ = false;
        ++revision_;
        publish(ChangeKind::Graph);
        record(); // the edits up to a commit are one step
    }

    void Session::clearHistory()
    {
        undo_.clear(); redo_.clear(); mergeable_ = false;
        baseline_ = std::make_shared<const Model>(model());
    }

    void Session::publish( ChangeKind kind, NodeId node, uint32_t param, double value, bool plugin )
    {
        if( history_.size() == historyCapacity ) history_.pop_front();
        history_.push_back({++sequence_,kind,node,param,value,plugin});
    }

    Control::Changes Session::changesAfter( uint64_t sequence ) const
    {
        Changes result{sequence_,false,{}};
        result.lost = sequence > sequence_ || (!history_.empty() && sequence < history_.front().sequence - 1);
        for( const auto& change : history_ ) if( change.sequence > sequence ) result.events.push_back(change);
        return result;
    }

    bool Session::queue( NodeId node, uint32_t param, ChangeKind kind, double value, bool bounded )
    {
        const Address address{node,param};
        auto it = pending_.find(address);
        if( kind == ChangeKind::Value && it != pending_.end() && !it->second.empty() && it->second.back().kind == kind )
            it->second.back().value = value;
        else
        {
            if( bounded && commandCount_ >= commandCapacity ) return false;
            pending_[address].push_back({kind,value});
            ++commandCount_;
        }
        publish(kind,node,param,value);
        return true;
    }

    bool Session::parameterGesture( NodeId node, uint32_t param, bool begin )
    {
        if( findParameter(node,param).flags & CLAP_PARAM_IS_READONLY )
            throw std::invalid_argument("parameter is read-only");
        return queue(node,param,begin ? ChangeKind::GestureBegin : ChangeKind::GestureEnd,0);
    }

    void Session::service()
    {
        graph_.service();
        if( prepared_ ) prepared_->next->service(); // its plugins' main-thread callbacks while it waits
        ringOut();
        meter();
        recorder_.service();
        // A recording's taps follow the wires: a fader moved, a plugin replaced, a Performance changed.
        if( recorder_.recording() && ( revision_ != recordRevision_ || wires_ != recordWires_ ) )
        {
            recorder_.update( compileTaps( recordTaps_, false ) );
            recordRevision_ = revision_; recordWires_ = wires_;
        }
        measure();
        pollPressure();
        if( !staged_ ) record(); // changes that need no commit (scenes, controls, pads) are steps too
        advanceScenes();
        for( const auto& [node,name] : nodes_ )
        {
            auto& host = graph_.host(node);
            if( outputDrops_[node] != host.outputEventDrops() )
            {
                outputDrops_[node] = host.outputEventDrops();
                publish(ChangeKind::Resync,node,0,0,true);
            }
            // New parameter info (a mode showing other controls): clients read the graph again.
            if( auto [seen,added] = parameterRevisions_.try_emplace(node,host.parameterRevision()); !added && seen->second != host.parameterRevision() )
            {
                seen->second = host.parameterRevision();
                ++revision_; publish(ChangeKind::Graph);
            }
            kick::clap::Event event;
            // Bounded even if a plugin continuously emits on the audio thread.
            for( size_t i = 0; i < kick::clap::Host::maxEvents && host.popOutput(event); ++i )
            {
                const auto type = event.header()->type;
                if( type == CLAP_EVENT_PARAM_VALUE )
                {
                    const auto* p = reinterpret_cast<const clap_event_param_value_t*>(event.header());
                    if( p->note_id == -1 && p->port_index == -1 && p->channel == -1 && p->key == -1 && hasParameter(node,p->param_id) )
                    {
                        publish(ChangeKind::Value,node,p->param_id,p->value,true);
                        observed_[{node,p->param_id}] = p->value;
                    }
                }
                else if( type == CLAP_EVENT_PARAM_GESTURE_BEGIN || type == CLAP_EVENT_PARAM_GESTURE_END )
                {
                    const auto* p = reinterpret_cast<const clap_event_param_gesture_t*>(event.header());
                    publish(type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? ChangeKind::GestureBegin : ChangeKind::GestureEnd,node,p->param_id,0,true);
                }
            }
            // Rescans may update a value without emitting an event.
            for( const auto& p : host.parameters() )
            {
                const Address address{node,p.id};
                const auto value = host.parameterValue(p.id);
                if( !observed_.contains(address) || observed_[address] != value )
                    publish(ChangeKind::Value,node,p.id,value,true);
                observed_[address] = value;
            }
        }
        for( auto it = pending_.begin(); it != pending_.end(); )
        {
            const auto [node,param] = it->first;
            auto& commands = it->second;
            if( !hasParameter(node,param) || (parameterInfo(node,param).flags & CLAP_PARAM_IS_READONLY) )
            {
                commandCount_ -= commands.size();
                publish(ChangeKind::Resync,node,param);
                it = pending_.erase(it);
                continue;
            }
            auto& host = graph_.host(node);
            bool sentValue = false;
            while( !commands.empty() )
            {
                const auto command = commands.front();
                if( command.kind == ChangeKind::Value )
                {
                    if( sentValue || !host.setParameter(param,command.value) ) break;
                    sentValue = true;
                    observed_[{node,param}] = command.value;
                }
                else if( !host.parameterGesture(param,command.kind == ChangeKind::GestureBegin) ) break;
                commands.pop_front();
                --commandCount_;
            }
            it = commands.empty() ? pending_.erase(it) : std::next(it);
        }
    }

    // RFC 0012 step 2: MIDI clock out from this block's transport. While it plays, a clock at each
    // 24th of a beat, at its sample; Start when it starts from the top, else Song Position and
    // Continue; Stop when it stops; a jump while playing (a rewind, another Performance, an external
    // clock's song position) is Stop, Song Position and Continue. Nothing while stopped.
    void Session::clock( const clap_event_transport_t& t, uint32_t frames ) noexcept
    {
        const auto ports = clockPorts_.load( std::memory_order_acquire );
        auto send = [&]( uint32_t time, uint8_t status, uint8_t a = 0, uint8_t b = 0, uint8_t size = 1 ) {
            if( !ports || midiOutCount_ == midiOut_.size() ) return;
            midiOut_[midiOutCount_++] = { time, ports, size, { status, a, b } };
        };
        const bool playing = ( t.flags & CLAP_TRANSPORT_IS_PLAYING ) != 0;
        const double beats = (double)t.song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
        const double samplesPerBeat = 60.0 * sampleRate_ / t.tempo;
        auto position = [&] {
            const auto sixteenths = (uint16_t)std::clamp<int64_t>( (int64_t)std::floor( beats * 4 + 1e-9 ), 0, 16383 );
            send( 0, 0xF2, uint8_t( sixteenths & 0x7F ), uint8_t( sixteenths >> 7 ), 3 );
        };
        if( !playing )
        {
            if( clockPlaying_ ) send( 0, 0xFC );
            clockPlaying_ = false;
            return;
        }
        const bool jumped = clockPlaying_ && std::abs( beats - clockEnd_ ) > 1.0 / 24;
        if( !clockPlaying_ || jumped )
        {
            if( jumped ) send( 0, 0xFC );
            if( !jumped && beats < 1e-9 ) send( 0, 0xFA );
            else { position(); send( 0, 0xFB ); }
            nextPulse_ = (int64_t)std::ceil( beats * 24 - 1e-6 );
        }
        clockPlaying_ = true;
        clockEnd_ = beats + frames / samplesPerBeat;
        for( ; nextPulse_ / 24.0 < clockEnd_ - 1e-9; ++nextPulse_ )
        {
            const auto at = std::llround( std::max( 0.0, nextPulse_ / 24.0 - beats ) * samplesPerBeat );
            send( (uint32_t)std::min<int64_t>( at, frames - 1 ), 0xF8 );
        }
    }

    bool Session::process( float* const* outputs, uint32_t frames,
                           std::span<const kick::clap::Event> events, std::span<const MidiClockEvent> clocks,
                           std::span<const uint8_t> sources ) noexcept
    {
        return process(nullptr,0,outputs,2,frames,events,clocks,sources);
    }
    bool Session::process(const float* const* inputs,uint32_t ni,float* const* outputs,uint32_t no,uint32_t frames,
                          std::span<const kick::clap::Event> events,std::span<const MidiClockEvent> clocks,
                          std::span<const uint8_t> sources) noexcept
    {
        auto silence=[&]{if(outputs)for(uint32_t c=0;c<no;++c)if(outputs[c])std::fill_n(outputs[c],frames,0.f);};
        if(!frames || frames>maxFrames_ || events.size()>maxInputEvents || clocks.size()>maxClockEvents || (!sources.empty() && sources.size()!=events.size()) ||
           ni>Graph::maxAudioChannels || no>Graph::maxAudioChannels || (ni && !inputs) || (no && !outputs) || no<graph_.outputChannels())
        {silence();return false;}
        for(uint32_t c=0;c<graph_.outputChannels();++c)if(!outputs[c]){silence();return false;}

        // A new Performance: the transport starts again. Queued notes still play: the last
        // Performance's plugins ring out beside it (RFC 0021), so a release must reach them.
        if(graph_.beginBlock()) {
            transport_.rewind();
            clocks={}; // prior-block clock messages belong to the outgoing Performance
        }
        const auto source = clockSource_.load();
        if( source != following_ )
        {
            transport_.setClockSource( source );
            following_ = source;
        }
        if( source == Transport::ClockSource::MidiClock )
            for( const auto& e : clocks )
                transport_.midiClock( e.status, e.sampleTime, e.songPosition );
        else
        {
            transport_.setTempo( tempo_.load() );
            transport_.setPlaying( playing_.load() );
        }

        size_t count = 0;
        auto tail = noteTail_.load( std::memory_order_relaxed );
        // Consume the overflow flag before taking the queue snapshot. If it is set,
        // the snapshot must include all notes published before that overflow; taking
        // the snapshot first could leave a new note behind its emergency release.
        const bool panic = panic_.exchange( false, std::memory_order_acquire );
        const auto head = noteHead_.load( std::memory_order_acquire );
        if( panic )
        {
            // Fail safe after a lost UI event: discard the queued burst and broadcast
            // release to graph-input destinations. Voices from other sources on the
            // same destinations can also be released by this emergency operation.
            graph_.panicInput();
            tail = head;
        }
        else
            while( tail != head )
            {
                mergedSources_[count] = builtInSource;
                merged_[count++] = notes_[tail % notes_.size()];
                ++tail;
            }
        noteTail_.store( tail, std::memory_order_release );
        const auto ignored = ignoredSources_.load( std::memory_order_acquire );
        for( size_t i = 0; i < events.size(); ++i )
        {
            if( !sources.empty() && sources[i] < maxSources && ( ignored >> sources[i] & 1 ) )
                continue; // a duplicate input's (RFC 0024)
            mergedSources_[count] = sources.empty() ? untaggedSource : sources[i];
            merged_[count++] = events[i];
        }

        const auto t = transport_.current();
        statusPlaying_.store( ( t.flags & CLAP_TRANSPORT_IS_PLAYING ) != 0 );
        statusTempo_.store( t.tempo );
        statusBeat_.store( (double)t.song_pos_beats / (double)CLAP_BEATTIME_FACTOR );
        recorder_.begin( framesProcessed_.load( std::memory_order_relaxed ), t.tempo, statusBeat_.load() );
        recorder_.graphEvents( merged_.data(), (uint32_t)count );
        const bool ok = graph_.process( inputs, ni, outputs, no, frames, &t, merged_.data(), (uint32_t)count, false, mergedSources_.data() );
        recorder_.end( graph_, outputs, no, frames );
        // MIDI for hardware outputs: the parts', with any note offs a tap removed sent.
        const auto taps = graph_.midiOut();
        std::copy( taps.begin(), taps.end(), midiOut_.begin() );
        midiOutCount_ = (uint32_t)taps.size();
        graph_.clearMidiOut();
        clock( t, frames );
        // In time order, each source's own order kept (insertion sort: no allocation, nearly sorted).
        for( uint32_t i = 1; i < midiOutCount_; ++i )
            for( uint32_t j = i; j > 0 && midiOut_[j - 1].time > midiOut_[j].time; --j )
                std::swap( midiOut_[j - 1], midiOut_[j] );
        transport_.advance( frames );
        framesProcessed_.fetch_add( frames, std::memory_order_relaxed );
        blockFrames_.store( frames, std::memory_order_relaxed );
        return ok;
    }

    uint32_t Session::recordStart( const std::vector<RecordTap>& asked, const std::string& name )
    {
        auto taps = asked;
        if( taps.empty() )
        {
            if( graph_.audioLayout().outputs.empty() ) throw std::invalid_argument( "no graph output to record" );
            taps = { { RecordTap::Kind::Output, graph_.audioLayout().outputs.front().id }, { RecordTap::Kind::Notes } };
        }
        auto compiled = compileTaps( taps, true );
        // A folder per take, named by its local start time and the Performance (RFC 0020).
        const auto now = std::chrono::system_clock::to_time_t( std::chrono::system_clock::now() );
        std::tm local{};
        localtime_r( &now, &local );
        char stamp[32];
        std::strftime( stamp, sizeof stamp, "%Y-%m-%d %H-%M-%S", &local );
        std::string title = name.empty() ? info_.name : name;
        for( auto& c : title ) if( static_cast<unsigned char>( c ) < 0x20 || std::strchr( "<>:\"/\\|?*", c ) ) c = '_';
        const auto base = dataHome() / "recordings" / ( title.empty() ? std::string( stamp ) : std::string( stamp ) + " " + title );
        auto folder = base;
        for( int n = 2; std::filesystem::exists( folder ); ++n ) folder = base.string() + " " + std::to_string( n );
        const auto take = recorder_.start( folder, compiled, title );
        recordTaps_ = taps; recordWires_ = wires_; recordRevision_ = revision_;
        return take;
    }

    void Session::recordStop( uint32_t take )
    {
        if( !take || take != recorder_.recording() ) throw std::invalid_argument( "that take is not recording" );
        recorder_.stop();
    }
}

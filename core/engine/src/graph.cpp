// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "NoteRoute.h"
#include "PreparedInstrument.h"

#include <oscairn-clap/param-target.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace oscairn
{
    /// A compiled graph: nodes in dependency order with everything the audio thread needs
    /// preallocated, so processing never allocates or locks.
    struct Graph::Plan
    {
        uint64_t replacementSerial=0;
        size_t noteCutoff=0;
        struct AudioRoute { int source; uint32_t from, to; float gain; bool first=false; }; // -1: external input
        std::vector<AudioRoute> outputRoutes;
        std::vector<float> externalInputs;
        std::vector<uint32_t> silentOutputs;
        struct Step
        {
            std::shared_ptr<PluginHost> host; // keeps the plugin alive while any plan uses it
            uint32_t                          audioInputs = 0, audioOutputs = 0;
            std::vector<AudioRoute>           audioSources;
            std::vector<uint32_t> silentInputs;
            struct NoteSource {int step;std::shared_ptr<NoteRoute> route;NoteZones zones;};
            std::vector<NoteSource> noteSources;
            std::array<kick::clap::Event,kick::clap::Host::maxBlockEvents> releases;
            uint32_t releaseCount=0;
            bool releaseOverflow=false;
            std::vector<float> inputBuffer, outputBuffer;
            std::vector<float*> out;
            std::vector<const float*> in;

            struct Route // where one of this step's param-target lanes goes
            {
                uint32_t lane;
                int      target;
                clap_id  param;
                double   minimum, maximum;
                bool     stepped;
                NoteRoute* notes=nullptr;
            };
            std::vector<Route>             routes;
            std::vector<NoteRoute*> paramNotes;
            std::vector<kick::clap::Event> paramIn; // converted events waiting for this step this block
            uint32_t                       paramCount = 0;
            uint64_t paramDrops = 0;
        };

        struct Key // orders merged notes: by time, then by source and position
        {
            uint32_t                 time;
            uint32_t                 order;
            const kick::clap::Event* event;
            NoteRoute* route=nullptr;
            bool afterNotes=false;
            const NoteZones* zones=nullptr;
            uint8_t source=untaggedSource;
        };

        std::vector<Step>              steps;
        std::vector<Key>               keys;
        std::vector<kick::clap::Event> merged;
    };

    //==============================================================================
    void Graph::Retired::push( Plan* p ) noexcept
    {
        const auto head = head_.load( std::memory_order_relaxed );
        slots_[head % slots_.size()] = p;
        head_.store( head + 1, std::memory_order_release );
    }

    Graph::Plan* Graph::Retired::pop() noexcept
    {
        const auto tail = tail_.load( std::memory_order_relaxed );
        if( tail == head_.load( std::memory_order_acquire ) )
            return nullptr;
        Plan* p = slots_[tail % slots_.size()];
        tail_.store( tail + 1, std::memory_order_release );
        return p;
    }

    //==============================================================================
    static uint32_t countChannels(const std::vector<kick::clap::Port>& ports)
    {
        uint32_t count=0;std::vector<uint32_t> ids;
        for(const auto& p:ports)
        {
            if(!p.channels || p.channels>Graph::maxAudioChannels-count || std::find(ids.begin(),ids.end(),p.id)!=ids.end())
                throw std::invalid_argument("Invalid audio ports: zero/too many channels or duplicate port ID");
            count+=p.channels;ids.push_back(p.id);
        }
        return count;
    }

    Graph::Graph( double sampleRate, uint32_t maxFrames, ProcessingMode mode, Lv2BufferLimits lv2Buffers, AudioLayout audio )
        : sampleRate_( sampleRate )
        , maxFrames_( maxFrames )
        , mode_( mode )
        , lv2Buffers_( lv2Buffers ), audioLayout_(std::move(audio))
    {
        if(!std::isfinite(sampleRate) || sampleRate<=0 || !maxFrames || maxFrames>1048576)
            throw std::invalid_argument("Invalid graph sample rate or block size");
        inputChannels_=countChannels(audioLayout_.inputs);outputChannels_=countChannels(audioLayout_.outputs);
    }

    Graph::~Graph()
    {
        // The audio thread must have stopped calling process().
        delete pending_.exchange( nullptr );
        delete current_;
        collect();
    }

    NodeId Graph::add( const std::string& path, const std::string& pluginId )
    {
        if(nextId_>=graphInput)throw std::overflow_error("Graph node IDs exhausted");
        Node node;
        auto loaded=loadPlugin(path,pluginId,sampleRate_,maxFrames_,mode_,lv2Buffers_);loaded->setAssets(assets_);
        node.host = preparedInstrument(std::move(loaded),sampleRate_,maxFrames_,mode_,lv2Buffers_);
        node.audioInputs=countChannels(node.host->inputPorts());
        node.audioOutputs=countChannels(node.host->outputPorts());

        const NodeId id = nextId_++;
        nodes_.emplace( id, std::move( node ) );
        return id;
    }

    NodeId Graph::addEffectContext(NodeId source)
    {
        if(nextId_>=graphInput)throw std::overflow_error("Graph node IDs exhausted");
        Node node;node.host=host(source).createEffectContext();node.effectGroup=nodes_.at(source).effectGroup;
        node.audioInputs=countChannels(node.host->inputPorts());node.audioOutputs=countChannels(node.host->outputPorts());
        const NodeId id=nextId_++;nodes_.emplace(id,std::move(node));return id;
    }

    NodeId Graph::effectSource(NodeId id) const {
        const auto& group=nodes_.at(id).effectGroup;
        for(const auto& [other,node]:nodes_)if(node.effectGroup==group)return other;
        return id;
    }
    std::unique_ptr<Graph> Graph::prepareReplacement() const {
        auto next=std::make_unique<Graph>(sampleRate_,maxFrames_,mode_,lv2Buffers_,audioLayout_);
        next->nextId_=nextId_;next->assets_=assets_;return next;
    }
    void Graph::publishReplacement(Graph& next,size_t cutoff,const std::function<void()>& onPublish) {
        if(next.sampleRate_!=sampleRate_ || next.maxFrames_!=maxFrames_ || next.inputChannels_!=inputChannels_ || next.outputChannels_!=outputChannels_)
            throw std::invalid_argument("Replacement device layout mismatch");
        next.replacementSerial_=replacementSerial_+1;next.replacementNoteCutoff_=cutoff;
        std::unique_ptr<Plan> plan(next.compile());
        removedHosts_.reserve(removedHosts_.size()+nodes_.size());
        onPublish(); // caller supplies a nonthrowing metadata/transport commit
        for(const auto& [id,node]:nodes_)removedHosts_.push_back(node.host);
        nodes_.swap(next.nodes_);audioRoutes_.swap(next.audioRoutes_);noteEdges_.swap(next.noteEdges_);noteZones_.swap(next.noteZones_);paramTargets_.swap(next.paramTargets_);
        nextId_=next.nextId_;replacementSerial_=next.replacementSerial_;replacementNoteCutoff_=cutoff;
        delete pending_.exchange(plan.release(),std::memory_order_acq_rel);
    }

    void Graph::replace(NodeId id,const std::string& path,const std::string& pluginId,const std::string& preset,const std::function<void()>& rewire)
    {
        const auto old=nodes_.at(id);
        for(const auto& [from,to]:paramTargets_)
            if(from.first==id || to.first==id)throw std::invalid_argument("Remove parameter mappings on this node before replacing its plugin");
        Node next;
        auto loaded=loadPlugin(path,pluginId,sampleRate_,maxFrames_,mode_,lv2Buffers_);loaded->setAssets(assets_);
        if(!preset.empty())loaded->loadPreset(preset);
        next.host=preparedInstrument(std::move(loaded),sampleRate_,maxFrames_,mode_,lv2Buffers_);
        next.audioInputs=countChannels(next.host->inputPorts());next.audioOutputs=countChannels(next.host->outputPorts());
        const auto routes=audioRoutes_;const auto notes=noteEdges_;const auto zones=noteZones_;
        std::erase_if(audioRoutes_,[&](const auto& e){return e.from.node==id || e.to.node==id;});
        std::erase_if(noteEdges_,[&](const auto& edge){return edge.first.first==id || edge.first.second==id;});
        std::erase_if(noteZones_,[&](const auto& edge){return edge.first.first==id || edge.first.second==id;});
        nodes_.at(id)=std::move(next);
        try
        {
            rewire();
            std::unique_ptr<Plan> plan(compile());
            removedHosts_.push_back(old.host);
            collect();delete pending_.exchange(plan.release(),std::memory_order_acq_rel);
        }
        catch(...){nodes_.at(id)=old;audioRoutes_=routes;noteEdges_=notes;noteZones_=zones;throw;}
    }

    void Graph::remove( NodeId node )
    {
        if(auto it=nodes_.find(node);it!=nodes_.end())removedHosts_.push_back(it->second.host);
        nodes_.erase( node );
        std::erase_if(audioRoutes_,[&](const auto& e){return e.from.node==node || e.to.node==node;});
        for( auto it = paramTargets_.begin(); it != paramTargets_.end(); )
            it = ( it->first.first == node || it->second.first == node ) ? paramTargets_.erase( it ) : std::next( it );
        auto touches = [&]( const std::pair<NodeId, NodeId>& e ) { return e.first == node || e.second == node; };
        std::erase_if(noteEdges_,[&](const auto& edge){return touches(edge.first);});
        std::erase_if(noteZones_,[&](const auto& edge){return touches(edge.first);});
    }

    void Graph::checkSource( NodeId from, bool audio ) const
    {
        if( from == graphInput && (!audio || inputChannels_) )
            return;
        const auto it = nodes_.find( from );
        if( it == nodes_.end() )
            throw std::invalid_argument( "no such source node" );
        if( audio ? it->second.audioOutputs == 0 : it->second.host->noteOutputPorts() == 0 )
            throw std::invalid_argument( audio ? "the source has no audio outputs" : "the source has no note outputs" );
    }

    void Graph::checkDestination( NodeId to, bool audio ) const
    {
        if( to == graphOutput && audio && outputChannels_ )
            return;
        const auto it = nodes_.find( to );
        if( it == nodes_.end() )
            throw std::invalid_argument( "no such destination node" );
        if( audio ? it->second.audioInputs == 0 : it->second.host->noteInputPorts() == 0 )
            throw std::invalid_argument( audio ? "the destination has no audio inputs" : "the destination has no note inputs" );
    }

    void Graph::connectAudio( NodeId from, NodeId to )
    {
        checkSource( from, true );
        checkDestination( to, true );

        auto mainChannels=[&](NodeId node,bool input) {
            const bool external=node==graphInput || node==graphOutput;
            const auto& ports=external?(input?audioLayout_.outputs:audioLayout_.inputs):
                (input?host(node).inputPorts():host(node).outputPorts());
            const auto ids=external?std::vector<uint32_t>{ports.front().id}:host(node).mainAudioPorts(input);
            std::vector<AudioEndpoint> result;
            for(auto id:ids)for(const auto& p:ports)if(p.id==id)
                for(uint32_t c=0;c<p.channels;++c)result.push_back({node,id,c});
            return result;
        };
        const auto source=mainChannels(from,false),destination=mainChannels(to,true);
        std::vector<AudioConnection> connections;
        const auto count=source.size()==1 && destination.size()==2?2:std::min(source.size(),destination.size());
        for(size_t i=0;i<count;++i)connections.push_back({source[std::min(i,source.size()-1)],destination[i]});
        connectAudioBatch(connections);
    }

    uint32_t Graph::resolve(const AudioEndpoint& e,bool source,uint32_t count) const
    {
        const std::vector<kick::clap::Port>* ports=nullptr;
        if(source && e.node==graphInput)ports=&audioLayout_.inputs;
        else if(!source && e.node==graphOutput)ports=&audioLayout_.outputs;
        else
        {
            const auto it=nodes_.find(e.node);
            if(it==nodes_.end())throw std::invalid_argument("Unknown audio endpoint node");
            ports=source?&it->second.host->outputPorts():&it->second.host->inputPorts();
        }
        uint32_t offset=0;
        for(const auto& p:*ports)
        {
            if(p.id==e.port)
            {
                if(!count || e.channel>=p.channels || count>p.channels-e.channel)throw std::invalid_argument("Audio channel range exceeds selected port");
                return offset+e.channel;
            }
            offset+=p.channels;
        }
        throw std::invalid_argument("Unknown audio port ID");
    }
    void Graph::connectAudioBatch(const std::vector<AudioConnection>& connections)
    {
        // Stage the whole operation locally: invalid ranges or limits cannot leave
        // half of a mono-to-stereo connection in the editable graph.
        auto next=audioRoutes_;
        for(const auto& c:connections)
        {
            resolve(c.from,true,c.channels);resolve(c.to,false,c.channels);
            if(!std::isfinite(c.gain) || std::abs(c.gain)>16)
                throw std::invalid_argument("Audio route gain must be finite and within -16..16");
            for(uint32_t i=0;i<c.channels;++i)
            {
                AudioConnection channel=c;channel.channels=1;channel.from.channel+=i;channel.to.channel+=i;
                auto existing=std::find_if(next.begin(),next.end(),[&](const auto& r){return r.from==channel.from && r.to==channel.to;});
                if(existing!=next.end())existing->gain=c.gain;
                else
                {
                    if(next.size()==maxAudioConnections)throw std::invalid_argument("Audio connection limit exceeded");
                    next.push_back(channel);
                }
            }
        }
        audioRoutes_.swap(next);
    }
    void Graph::connectAudio(const AudioConnection& c) { connectAudioBatch({c}); }
    void Graph::disconnectAudio(const AudioConnection& c)
    {
        resolve(c.from,true,c.channels);resolve(c.to,false,c.channels);
        std::erase_if(audioRoutes_,[&](const AudioConnection& r) {
            return r.from.node==c.from.node && r.to.node==c.to.node &&
                r.from.port==c.from.port && r.to.port==c.to.port &&
                r.from.channel>=c.from.channel && r.from.channel-c.from.channel<c.channels &&
                r.to.channel>=c.to.channel && r.to.channel-c.to.channel==r.from.channel-c.from.channel;
        });
    }

    void Graph::connectNotes( NodeId from, NodeId to, const std::vector<NoteZone>& zones )
    {
        checkSource( from, false );
        checkDestination( to, false );
        if(zones.size()>NoteZones::capacity || !std::all_of(zones.begin(),zones.end(),[](const NoteZone& z){return z.valid();}))
            throw std::invalid_argument("invalid or too many note zones");
        const auto key=std::make_pair(from,to);
        if(!noteEdges_.contains(key))noteEdges_.emplace(key,std::make_shared<NoteRoute>(host(to)));
        if(zones.empty())noteZones_.erase(key);else noteZones_[key]=zones;
    }

    void Graph::disconnectAudio( NodeId from, NodeId to )
    {
        std::erase_if( audioRoutes_, [&]( const AudioConnection& c ) {
            return c.from.node == from && c.to.node == to;
        } );
    }

    void Graph::disconnectNotes( NodeId from, NodeId to )
    {
        noteEdges_.erase({from,to});noteZones_.erase({from,to});
    }

    void Graph::mapParamTarget( NodeId source, uint32_t lane, NodeId target, clap_id param )
    {
        if( !nodes_.count( source ) || !nodes_.count( target ) || source == target )
            throw std::invalid_argument( "a parameter mapping needs two different nodes" );
        const auto& params = nodes_.at( target ).host->parameters();
        const auto  found  = std::find_if( params.begin(), params.end(), [&]( const kick::clap::Parameter& p ) { return p.id == param; } );
        if( found == params.end() || ( found->flags & CLAP_PARAM_IS_READONLY ) )
            throw std::invalid_argument( "the target has no such parameter, or it is read-only" );
        paramTargets_[{ source, lane }] = { target, param };
    }

    void Graph::unmapParamTarget( NodeId source, uint32_t lane )
    {
        paramTargets_.erase( { source, lane } );
    }

    const PluginHost& Graph::host( NodeId node ) const
    {
        return *nodes_.at( node ).host;
    }

    PluginHost& Graph::host( NodeId node )
    {
        return *nodes_.at( node ).host;
    }

    Graph::Description Graph::describe() const
    {
        Description result;
        for( const auto& [id,node] : nodes_ )
            result.nodes.push_back({id,node.host->descriptor(),node.host->inputPorts(),node.host->outputPorts(),
                                    node.host->noteInputPorts(),node.host->noteOutputPorts()});
        result.audioRoutes=audioRoutes_;result.audioLayout=audioLayout_;
        for(const auto& [edge,state]:noteEdges_)result.notes.push_back(edge);
        result.noteZones=noteZones_;
        for( const auto& [from,to] : paramTargets_ )
            result.mappings.push_back({from.first,from.second,to.first,to.second});
        return result;
    }

    Graph::Plan* Graph::compile() const
    {
        // Dependency order (Kahn's algorithm), lowest node ID first among ready nodes so
        // the order is stable.
        std::map<NodeId, int> indegree;
        for( const auto& [id, node] : nodes_ )
            indegree[id] = 0;
        auto internal = [&]( const std::pair<NodeId, NodeId>& e ) { return nodes_.count( e.first ) && nodes_.count( e.second ); };
        std::vector<std::pair<NodeId, NodeId>> noteDependencies;
        for(const auto& [edge,state]:noteEdges_)noteDependencies.push_back(edge);
        std::vector<std::pair<NodeId, NodeId>> audioDependencies;
        for(const auto& e:audioRoutes_)audioDependencies.emplace_back(e.from.node,e.to.node);
        std::vector<std::pair<NodeId, NodeId>> mappingEdges;
        for( const auto& [from, to] : paramTargets_ )
            mappingEdges.emplace_back( from.first, to.first );
        for( const auto* edges : { &std::as_const(audioDependencies), &std::as_const(noteDependencies), &std::as_const( mappingEdges ) } )
            for( const auto& e : *edges )
                if( internal( e ) )
                    ++indegree[e.second];

        std::vector<NodeId> order;
        while( order.size() < nodes_.size() )
        {
            auto ready = std::find_if( indegree.begin(), indegree.end(), []( const auto& d ) { return d.second == 0; } );
            if( ready == indegree.end() )
                throw std::runtime_error( "the graph's connections form a cycle" );
            const NodeId id = ready->first;
            ready->second   = -1; // done
            order.push_back( id );
            for( const auto* edges : { &std::as_const(audioDependencies), &std::as_const(noteDependencies), &std::as_const( mappingEdges ) } )
                for( const auto& e : *edges )
                    if( e.first == id && internal( e ) )
                        --indegree[e.second];
        }

        auto plan = std::make_unique<Plan>();
        plan->replacementSerial=replacementSerial_;plan->noteCutoff=replacementNoteCutoff_;
        std::map<NodeId, int> stepOf;
        for( const NodeId id : order )
        {
            const auto& node = nodes_.at( id );
            Plan::Step  step;
            step.host         = node.host;
            step.audioInputs  = node.audioInputs;
            step.audioOutputs = node.audioOutputs;
            stepOf[id]        = (int)plan->steps.size();
            plan->steps.push_back( std::move( step ) );
        }
        auto route=[&](NodeId from,uint32_t source,NodeId to,uint32_t dest,float gain)
        {
            Plan::AudioRoute r{from==graphInput?-1:stepOf.at(from),source,dest,gain};
            if(to==graphOutput)plan->outputRoutes.push_back(r);
            else plan->steps[stepOf.at(to)].audioSources.push_back(r);
        };
        for(const auto& c:audioRoutes_)
        {
            const auto from=resolve(c.from,true,c.channels),to=resolve(c.to,false,c.channels);
            for(uint32_t i=0;i<c.channels;++i)route(c.from.node,from+i,c.to.node,to+i,static_cast<float>(c.gain));
        }
        for( const auto& [edge,state] : noteEdges_ )
        {
            const auto [from,to]=edge;
            if( !nodes_.count( to ) || ( from != graphInput && !nodes_.count( from ) ) )
                continue;
            NoteZones zones;
            if(const auto z=noteZones_.find(edge);z!=noteZones_.end()){zones.count=z->second.size();std::copy(z->second.begin(),z->second.end(),zones.zones.begin());}
            plan->steps[(size_t)stepOf[to]].noteSources.push_back({from == graphInput ? -1 : stepOf[from],state,zones});
        }

        for( const auto& [from, to] : paramTargets_ )
        {
            const auto& params = nodes_.at( to.first ).host->parameters();
            const auto  p      = std::find_if( params.begin(), params.end(), [&]( const kick::clap::Parameter& q ) { return q.id == to.second; } );
            if( p == params.end() )
                continue; // the plugin no longer has it
            auto& source = plan->steps[(size_t)stepOf[from.first]];
            auto& target = plan->steps[(size_t)stepOf[to.first]];
            source.routes.push_back( { from.second, stepOf[to.first], to.second, p->minimum, p->maximum, ( p->flags & CLAP_PARAM_IS_STEPPED ) != 0 } );
            if(const auto notes=noteEdges_.find({from.first,to.first});notes!=noteEdges_.end())source.routes.back().notes=notes->second.get();
            target.paramIn.resize( kick::clap::Host::maxBlockEvents );
            target.paramNotes.resize( kick::clap::Host::maxBlockEvents );
        }

        auto prepare=[](auto& routes,uint32_t channels,auto& silent) {
            std::vector<bool> written(channels);
            for(auto& r:routes){r.first=!written[r.to];written[r.to]=true;}
            for(uint32_t c=0;c<channels;++c)if(!written[c])silent.push_back(c);
        };
        prepare(plan->outputRoutes,outputChannels_,plan->silentOutputs);
        plan->externalInputs.resize(size_t(inputChannels_)*maxFrames_);
        for(auto& step:plan->steps)
        {
            prepare(step.audioSources,step.audioInputs,step.silentInputs);
            step.inputBuffer.resize(size_t(step.audioInputs)*maxFrames_);
            step.outputBuffer.resize(size_t(step.audioOutputs)*maxFrames_);
            for(uint32_t c=0;c<step.audioInputs;++c)step.in.push_back(step.inputBuffer.data()+size_t(c)*maxFrames_);
            for(uint32_t c=0;c<step.audioOutputs;++c)step.out.push_back(step.outputBuffer.data()+size_t(c)*maxFrames_);
        }
        plan->keys.reserve( kick::clap::Host::maxBlockEvents );
        plan->merged.resize( kick::clap::Host::maxBlockEvents );
        return plan.release();
    }

    void Graph::commit()
    {
        Plan* plan = compile(); // throws on a cycle, before anything changes
        collect();
        delete pending_.exchange( plan, std::memory_order_acq_rel ); // a plan the audio thread never took
    }

    void Graph::collect()
    {
        while( Plan* p = retired_.pop() )
            delete p;
        std::erase_if(removedHosts_,[](const auto& host){return host.use_count()==1;});
    }

    void Graph::service()
    {
        collect();
        for(const auto& [id,node]:nodes_)node.host->service();
        for(const auto& host:removedHosts_)host->service();
    }

    std::optional<size_t> Graph::beginBlock() noexcept {
        // Adopt the newest plan, retiring the current one, if there is room to retire it.
        if( !current_ || !retired_.full() )
            if( Plan* plan = pending_.exchange( nullptr, std::memory_order_acq_rel ) )
            {
                const bool replacement=!current_?plan->replacementSerial!=0:current_->replacementSerial!=plan->replacementSerial;
                if( current_ ) {
                    for(auto& old:current_->steps)for(auto& next:plan->steps)if(old.host==next.host)
                        for(auto& source:old.noteSources)
                            if(std::none_of(next.noteSources.begin(),next.noteSources.end(),[&](const auto& s){return s.route==source.route;}))
                                next.releaseOverflow|=!source.route->retire([&](const auto& e){
                                    if(next.releaseCount==next.releases.size())return false;
                                    next.releases[next.releaseCount++]=e;return true;},[&](size_t port,int channel){
                                    uint8_t value=0;
                                    for(const auto& remaining:next.noteSources)value=std::max(value,remaining.route->pedal[port][channel]);
                                    return value;
                                });
                    retired_.push( current_ );
                }
                current_ = plan;
                if(replacement)return plan->noteCutoff;
            }
        return {};
    }

    bool Graph::process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                         const kick::clap::Event* events, uint32_t eventCount ) noexcept
    {
        return process(nullptr,0,outputs,channels,frames,transport,events,eventCount);
    }

    bool Graph::process(const float* const* inputs,uint32_t ni,float* const* outputs,uint32_t no,uint32_t frames,
                        const clap_event_transport_t* transport,const kick::clap::Event* events,uint32_t eventCount,bool adopt,const uint8_t* sources) noexcept
    {
        auto clear=[&]{if(outputs)for(uint32_t c=0;c<no;++c)if(outputs[c])std::fill_n(outputs[c],frames,0.f);};
        if(!frames || frames>maxFrames_ || ni>maxAudioChannels || no>maxAudioChannels || (ni && !inputs) ||
           (no && !outputs) || no<outputChannels_ || (eventCount && !events) || eventCount>kick::clap::Host::maxBlockEvents)
        {clear();return false;}
        for(uint32_t c=0;c<outputChannels_;++c)if(!outputs[c]){clear();return false;}
        if(adopt)beginBlock();

        const bool panicInput=std::exchange(panicInput_,false);
        Plan* plan = current_;
        if(!plan){clear();return true;}
        for(uint32_t c=0;c<inputChannels_;++c)
        {
            auto* dest=plan->externalInputs.data()+size_t(c)*maxFrames_;
            if(c<ni && inputs[c])std::copy_n(inputs[c],frames,dest);else std::fill_n(dest,frames,0.f);
        }
        for(auto c:plan->silentOutputs)std::fill_n(outputs[c],frames,0.f);
        for(uint32_t c=outputChannels_;c<no;++c)if(outputs[c])std::fill_n(outputs[c],frames,0.f);
        auto mix=[&](const Plan::AudioRoute& r,float* dest)
        {
            const float* from=r.source<0?plan->externalInputs.data()+size_t(r.from)*maxFrames_:plan->steps[r.source].out[r.from];
            if(r.first)
            {
                if(r.gain==1.f)std::copy_n(from,frames,dest);
                else for(uint32_t i=0;i<frames;++i)dest[i]=from[i]*r.gain;
            }
            else if(r.gain==1.f)for(uint32_t i=0;i<frames;++i)dest[i]+=from[i];
            else for(uint32_t i=0;i<frames;++i)dest[i]+=from[i]*r.gain;
        };
        bool ok = true;
        for( auto& step : plan->steps )
        {
            const bool inputRecovery=panicInput && std::any_of(step.noteSources.begin(),step.noteSources.end(),[](const auto& s){return s.step<0;});
            for(auto c:step.silentInputs)std::fill_n(step.inputBuffer.data()+size_t(c)*maxFrames_,frames,0.f);
            for(const auto& r:step.audioSources)mix(r,step.inputBuffer.data()+size_t(r.to)*maxFrames_);

            // Notes in: every source's events this block, merged in time order.
            plan->keys.clear();
            uint32_t order = 0;
            uint64_t totalEvents=step.paramCount+step.paramDrops+step.releaseCount;
            bool overflow=step.paramDrops!=0 || step.releaseOverflow || inputRecovery;
            for(uint32_t i=0;i<step.releaseCount;++i)plan->keys.push_back({0,order++,&step.releases[i]});
            for( uint32_t i = 0; i < step.paramCount; ++i ) {
                if(plan->keys.size()<kick::clap::Host::maxBlockEvents)
                    plan->keys.push_back( { step.paramIn[i].header()->time, order++, &step.paramIn[i],step.paramNotes[i],step.paramNotes[i]!=nullptr } );
                else overflow=true;
            }
            for( const auto& connection : step.noteSources )
            {
                const int source=connection.step;
                const kick::clap::Event* list  = events;
                uint32_t                 count = eventCount;
                if( source >= 0 )
                    list = plan->steps[(size_t)source].host->outputEvents( count );
                for( uint32_t i = 0; i < count; ++i )
                    if( list[i].header()->space_id == CLAP_CORE_EVENT_SPACE_ID && list[i].header()->type != CLAP_EVENT_NOTE_END ) // note ends belong to their destination
                    {
                        ++totalEvents;
                        if(plan->keys.size()<kick::clap::Host::maxBlockEvents)
                            plan->keys.push_back( { list[i].header()->time, order++, &list[i],connection.route.get(),false,&connection.zones,
                                                    source<0 && sources?sources[i]:untaggedSource } );
                        else overflow=true;
                    }
            }
            std::sort( plan->keys.begin(), plan->keys.end(), []( const Plan::Key& a, const Plan::Key& b ) {
                if(a.time!=b.time)return a.time<b.time;
                if(a.afterNotes!=b.afterNotes)return !a.afterNotes;
                return a.order<b.order;
            } );
            uint32_t merged=0;
            auto emit=[&](const auto& e){if(merged==plan->merged.size())return false;plan->merged[merged++]=e;return true;};
            if(!overflow)for(const auto& key:plan->keys)
                if(!(key.route && key.time<frames?key.route->route(*key.event,nextNoteId_,emit,key.zones,key.source):emit(*key.event))){overflow=true;break;}
            if(overflow)
            {
                // A lost release must reset the destination in its own dialect.
                eventDrops_.fetch_add(totalEvents,std::memory_order_relaxed);
                merged=0;step.host->panic();
                for(auto& source:step.noteSources)source.route->clear();
            }

            const bool processed=step.host->process(step.in.data(),step.audioInputs,step.out.data(),step.audioOutputs,frames,plan->merged.data(),merged,transport);
            if(!processed) {
                // A busy control gate can discard a release; reset when rendering resumes.
                step.host->panic();
                for(auto& source:step.noteSources)source.route->clear();
            }
            ok &= processed;
            step.paramCount = 0;step.paramDrops=0;step.releaseCount=0;step.releaseOverflow=false;
            uint32_t endedCount=0;const auto* ended=step.host->outputEvents(endedCount);
            for(uint32_t i=0;i<endedCount;++i)for(auto& source:step.noteSources)source.route->ended(ended[i]);

            // This step's param-target lane events become parameter events for their
            // targets, which come later in this block.
            if( !step.routes.empty() )
            {
                uint32_t    count  = 0;
                const auto* output = step.host->outputEvents( count );
                for( uint32_t i = 0; i < count; ++i )
                {
                    const auto* h = output[i].header();
                    if( h->space_id != kick::clap::Host::paramTargetSpace || h->type != OSCAIRN_EVENT_PARAM_TARGET )
                        continue;
                    const auto& e     = *reinterpret_cast<const oscairn_event_param_target_t*>( h );
                    const auto  route = std::find_if( step.routes.begin(), step.routes.end(), [&]( const Plan::Step::Route& r ) { return r.lane == e.lane; } );
                    if( route == step.routes.end() )
                        continue; // lane not mapped
                    auto& target = plan->steps[(size_t)route->target];
                    if( target.paramCount == target.paramIn.size() )
                    {++target.paramDrops;continue;}
                    // Per-note lane addresses belong to the source's corresponding note route.
                    if(e.note_id>=0 && !route->notes){eventDrops_.fetch_add(1,std::memory_order_relaxed);continue;}
                    target.paramNotes[target.paramCount]=e.note_id>=0?route->notes:nullptr;
                    const double range = route->maximum - route->minimum;
                    if( e.mode == OSCAIRN_PARAM_TARGET_SET )
                    {
                        clap_event_param_value_t v{};
                        v.header     = { sizeof( v ), h->time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                        v.param_id   = route->param;
                        v.note_id    = e.note_id;
                        v.port_index = e.port_index;
                        v.channel    = e.channel;
                        v.key        = e.key;
                        v.value      = route->minimum + std::clamp( e.value, 0.0, 1.0 ) * range;
                        if( route->stepped )
                            v.value = std::round( v.value );
                        target.paramIn[target.paramCount++] = kick::clap::Event::from( v );
                    }
                    else
                    {
                        clap_event_param_mod_t m{};
                        m.header     = { sizeof( m ), h->time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0 };
                        m.param_id   = route->param;
                        m.note_id    = e.note_id;
                        m.port_index = e.port_index;
                        m.channel    = e.channel;
                        m.key        = e.key;
                        m.amount     = std::clamp( e.value, -1.0, 1.0 ) * range;
                        target.paramIn[target.paramCount++] = kick::clap::Event::from( m );
                    }
                }
            }
        }

        for(const auto& r:plan->outputRoutes)mix(r,outputs[r.to]);
        return ok;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "NoteRoute.h"
#include "PreparedInstrument.h"

#include "oscairn/harmony.h"
#include <oscairn-clap/beat.h>
#include <oscairn-clap/param-target.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace oscairn
{
    // A harmony's input (setHarmonies): the notes through its zones, and the last harmony sent.
    struct Graph::HeldKeys { NoteRoute route{16u}; oscairn_harmony_t last{}; };

    /// A compiled graph: nodes in dependency order with everything the audio thread needs
    /// preallocated, so processing never allocates or locks.
    struct Graph::Plan
    {
        uint64_t replacementSerial=0;
        bool rampGains=false; // see commit()
        struct Harmony // see setHarmonies; events: this block's, at their times
        {
            std::shared_ptr<Graph::HeldKeys> held; NoteZones zones; bool input=false; uint8_t root=0; uint16_t scale=1;
            std::array<kick::clap::Event,32> events; uint32_t count=0;
        };
        std::vector<Harmony> harmonies;
        // -1: external input. now, delta and left (audio thread only) ramp a gain that changed with a
        // new plan from the old one to the new one over a few milliseconds, so a moved fader does not step.
        struct AudioRoute { int source; uint32_t from, to; float gain; bool first=false; mutable float now=0, delta=0; mutable uint32_t left=0; std::array<uint8_t,4> live{}; }; // live: see setLiveGain
        std::vector<AudioRoute> outputRoutes;
        std::vector<float> externalInputs;
        std::vector<uint32_t> silentOutputs;
        struct Step
        {
            std::shared_ptr<PluginHost> host; // keeps the plugin alive while any plan uses it
            NodeId id = 0;
            std::shared_ptr<std::array<std::atomic<float>,Graph::meterChannels>> peaks; // the node's, shared with every plan
            uint32_t                          audioInputs = 0, audioOutputs = 0;
            bool ringOut = false; // the last two of audioOutputs are the plugin's ring-out buffer (Graph::ringOutPort)
            std::vector<AudioRoute>           audioSources;
            std::vector<uint32_t> silentInputs;
            struct NoteSource {int step;std::shared_ptr<NoteRoute> route;NoteZones zones;bool draining=false;};
            bool retiring=false;std::shared_ptr<Graph::Activity> activity; // see Graph::setRetiring
            int harmony=-1; // the part's harmony it gets, in Plan::harmonies
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
            bool draining=false;
        };

        struct Tap { int step; std::shared_ptr<NoteRoute> route; NoteZones zones; uint64_t ports; uint8_t channel; }; // see setMidiTaps; step -1: the input
        std::vector<Tap>               taps;
        std::vector<Step>              steps;
        std::vector<Key>               keys;
        std::vector<kick::clap::Event> merged;
    };

    // A routed event as a MIDI 1.0 channel message on `channel` (1..16; 0: its own): notes,
    // pressure, and MIDI events' own bytes. False for anything else.
    static bool toMidi(const kick::clap::Event& e,uint8_t channel,Graph::MidiMessage& m) noexcept
    {
        const auto* h=e.header();
        if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID)return false;
        auto on=[&](int own){return uint8_t(channel?channel-1:std::clamp(own,0,15));};
        auto seven=[](double v){return uint8_t(std::lround(std::clamp(v,0.0,1.0)*127));};
        if((h->type==CLAP_EVENT_NOTE_ON || h->type==CLAP_EVENT_NOTE_OFF || h->type==CLAP_EVENT_NOTE_CHOKE) && h->size==sizeof(clap_event_note_t))
        {
            const auto& n=*reinterpret_cast<const clap_event_note_t*>(h);
            if(n.key<0 || n.key>127)return false;
            const bool down=h->type==CLAP_EVENT_NOTE_ON;
            m.bytes={uint8_t((down?0x90:0x80)|on(n.channel)),uint8_t(n.key),down?std::max<uint8_t>(seven(n.velocity),1):seven(n.velocity)};m.size=3;
            return true;
        }
        if(h->type==CLAP_EVENT_NOTE_EXPRESSION && h->size==sizeof(clap_event_note_expression_t))
        {
            const auto& x=*reinterpret_cast<const clap_event_note_expression_t*>(h);
            if(x.expression_id!=CLAP_NOTE_EXPRESSION_PRESSURE || x.key>127)return false;
            if(x.key<0){m.bytes={uint8_t(0xd0|on(x.channel)),seven(x.value),0};m.size=2;}
            else{m.bytes={uint8_t(0xa0|on(x.channel)),uint8_t(x.key),seven(x.value)};m.size=3;}
            return true;
        }
        if(h->type==CLAP_EVENT_MIDI && h->size==sizeof(clap_event_midi_t))
        {
            const auto& x=*reinterpret_cast<const clap_event_midi_t*>(h);
            const auto kind=x.data[0]&0xf0;
            if(kind<0x80 || kind>=0xf0 || x.data[1]>127 || x.data[2]>127)return false;
            m.bytes={uint8_t(kind|on(x.data[0]&15)),x.data[1],x.data[2]};m.size=kind==0xc0 || kind==0xd0?2:3;
            return true;
        }
        return false;
    }

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
        for(auto& g:liveTarget_)g.store(1.f,std::memory_order_relaxed);
        liveFrom_.fill(1.f);liveTo_.fill(1.f);
    }

    Graph::~Graph()
    {
        // The audio thread must have stopped calling process().
        delete pending_.exchange( nullptr );
        delete current_;
        collect();
    }

    NodeId Graph::add( const std::string& path, const std::string& pluginId, uint64_t memoryRoom )
    {
        if(nextId_>=graphInput)throw std::overflow_error("Graph node IDs exhausted");
        Node node;
        auto loaded=loadPlugin(path,pluginId,sampleRate_,maxFrames_,mode_,lv2Buffers_,memoryRoom);
        node.host = preparedInstrument(std::move(loaded),sampleRate_,maxFrames_,mode_,lv2Buffers_);node.host->setAssets(assets_); // a pool passes it on to the instances it loads later
        node.audioInputs=countChannels(node.host->inputPorts());
        node.audioOutputs=countChannels(node.host->outputPorts());

        const NodeId id = nextId_++;
        nodes_.emplace( id, std::move( node ) );
        return id;
    }

    NodeId Graph::add( std::shared_ptr<PluginHost> host )
    {
        if(nextId_>=graphInput)throw std::overflow_error("Graph node IDs exhausted");
        if(!host)throw std::invalid_argument("no plugin host");
        Node node;node.host=std::move(host);
        node.audioInputs=countChannels(node.host->inputPorts());node.audioOutputs=countChannels(node.host->outputPorts());
        const NodeId id=nextId_++;nodes_.emplace(id,std::move(node));return id;
    }

    NodeId Graph::addEffectContext(NodeId source) { return addEffectContext(*this, source); }
    NodeId Graph::addEffectContext(const Graph& graph,NodeId source)
    {
        if(nextId_>=graphInput)throw std::overflow_error("Graph node IDs exhausted");
        const auto& from=graph.nodes_.at(source);
        Node node;node.host=from.host->createEffectContext();node.effectGroup=from.effectGroup;
        node.audioInputs=countChannels(node.host->inputPorts());node.audioOutputs=countChannels(node.host->outputPorts());
        const NodeId id=nextId_++;nodes_.emplace(id,std::move(node));return id;
    }

    NodeId Graph::effectSource(NodeId id,const std::set<NodeId>& except) const {
        const auto& group=nodes_.at(id).effectGroup;
        for(const auto& [other,node]:nodes_)if(node.effectGroup==group && !except.count(other))return other;
        return id;
    }
    std::unique_ptr<Graph> Graph::prepareReplacement() const {
        auto next=std::make_unique<Graph>(sampleRate_,maxFrames_,mode_,lv2Buffers_,audioLayout_);
        next->nextId_=nextId_;next->assets_=assets_;return next;
    }
    void Graph::carryOver(const Graph& current,const std::set<NodeId>& ids) {
        for(const auto id:ids){if(nodes_.count(id))throw std::logic_error("a carried-over node ID is taken");auto node=current.nodes_.at(id);node.retiring=false;nodes_[id]=std::move(node);setRetiring(id,true);}
        nextId_=std::max(nextId_,current.nextId_); // a replacement prepared ahead: the current graph may have added since
        // Their live gains are frozen at what they are now: the slots are the next Performance's.
        for(auto r:current.audioRoutes_)if(ids.count(r.from.node) && (ids.count(r.to.node) || r.to.node==graphOutput)){
            for(auto& slot:r.live){r.gain*=current.liveGain(slot);slot=0;}
            audioRoutes_.push_back(r);}
        for(const auto& [edge,route]:current.noteEdges_)if(ids.count(edge.second) && (ids.count(edge.first) || edge.first==graphInput)){
            noteEdges_.emplace(edge,route);draining_.insert(edge);
            if(const auto z=current.noteZones_.find(edge);z!=current.noteZones_.end())noteZones_[edge]=z->second;}
    }
    void Graph::takeNotes(const Graph& current,NodeId from,NodeId node) {
        for(const auto& [edge,route]:current.noteEdges_){
            if(edge.second!=from)continue;
            const std::pair key{edge.first,node};
            if(edge.first==graphInput){if(auto it=noteEdges_.find(key);it!=noteEdges_.end())it->second=route;continue;} // with none, they are released
            const auto source=nodes_.find(edge.first);
            if(source==nodes_.end() || !source->second.retiring || noteEdges_.contains(key))continue;
            noteEdges_.emplace(key,route);draining_.insert(key);
            if(const auto z=current.noteZones_.find(edge);z!=current.noteZones_.end())noteZones_[key]=z->second;
        }
    }
    void Graph::ringOut(const Graph& current,NodeId from,NodeId node) {
        const auto it=nodes_.find(node);if(it==nodes_.end() || !it->second.host->ringsOut())return;
        const auto mains=it->second.host->mainAudioPorts(false);if(mains.empty())return;
        std::vector<AudioConnection> routes;
        for(auto r:current.audioRoutes_){
            if(r.from.node!=from || r.from.port!=mains.front() || r.from.channel>=2)continue;
            const auto to=nodes_.find(r.to.node);
            if(r.to.node!=graphOutput && (to==nodes_.end() || !to->second.retiring))return; // not all the outgoing chain: none
            for(auto& slot:r.live){r.gain*=current.liveGain(slot);slot=0;}
            r.from={node,ringOutPort,r.from.channel};routes.push_back(r);
        }
        connectAudioBatch(routes);
    }
    void Graph::dropRingOut(NodeId node) { std::erase_if(audioRoutes_,[&](const auto& r){return r.from.node==node && r.from.port==ringOutPort;}); }
    size_t Graph::ringOutRoutes(NodeId node) const { return size_t(std::count_if(audioRoutes_.begin(),audioRoutes_.end(),[&](const auto& r){return r.from.node==node && r.from.port==ringOutPort;})); }
    void Graph::publishReplacement(Graph& next,const std::function<void()>& onPublish) {
        if(next.sampleRate_!=sampleRate_ || next.maxFrames_!=maxFrames_ || next.inputChannels_!=inputChannels_ || next.outputChannels_!=outputChannels_)
            throw std::invalid_argument("Replacement device layout mismatch");
        next.replacementSerial_=replacementSerial_+1;
        std::unique_ptr<Plan> plan(next.compile());
        ++plansCompiled_;
        removedHosts_.reserve(removedHosts_.size()+nodes_.size());
        // A host carried over stays in use; listing it here too would keep it alive forever (collect() frees at one owner).
        // So is one taken over under another ID (RFC 0021).
        std::set<const PluginHost*> staying;for(const auto& [id,node]:next.nodes_)staying.insert(node.host.get());
        onPublish(); // caller supplies a nonthrowing metadata/transport commit
        for(const auto& [id,node]:nodes_)if(!staying.count(node.host.get()))removedHosts_.push_back(node.host);
        nodes_.swap(next.nodes_);audioRoutes_.swap(next.audioRoutes_);noteEdges_.swap(next.noteEdges_);noteZones_.swap(next.noteZones_);draining_.swap(next.draining_);paramTargets_.swap(next.paramTargets_);taps_.swap(next.taps_);harmonies_.swap(next.harmonies_);
        nextId_=next.nextId_;replacementSerial_=next.replacementSerial_;
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
        next.host=preparedInstrument(std::move(loaded),sampleRate_,maxFrames_,mode_,lv2Buffers_);next.host->setAssets(assets_);
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

    void Graph::restore( NodeId id, const Node& node )
    {
        if(!id || id >= graphInput || !node.host) throw std::invalid_argument("invalid node to restore");
        if(nodes_.count(id)) remove(id);
        nodes_.emplace(id, node);
        nextId_ = std::max(nextId_, id + 1);
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

    std::shared_ptr<PluginHost> Graph::release( NodeId node )
    {
        auto host=nodes_.at(node).host;
        remove(node);removedHosts_.pop_back();
        return host;
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
            if(source && e.port==ringOutPort && it->second.host->ringsOut())
            {
                if(!count || e.channel>=2 || count>2-e.channel)throw std::invalid_argument("Audio channel range exceeds selected port");
                return it->second.audioOutputs+e.channel;
            }
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
                if(existing!=next.end()){existing->gain=c.gain;existing->live=c.live;}
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
        if(!noteEdges_.contains(key))noteEdges_.emplace(key,std::make_shared<NoteRoute>(host(to),uint32_t(sampleRate_*.005))); // RFC 0018's 5 ms steal fade
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
        noteEdges_.erase({from,to});noteZones_.erase({from,to});draining_.erase({from,to});
    }

    void Graph::setMidiTaps( std::vector<MidiTap> taps )
    {
        for(const auto& t:taps)
        {
            checkSource(t.from,false);
            if(t.zones.size()>NoteZones::capacity || !std::all_of(t.zones.begin(),t.zones.end(),[](const NoteZone& z){return z.valid();}))
                throw std::invalid_argument("invalid or too many note zones");
            if(t.channel>16)throw std::invalid_argument("a MIDI channel is 1 to 16, or 0 for each message's own");
        }
        decltype(taps_) next;
        for(auto& t:taps)
        {
            // A tap whose chosen destination changed is a different tap: the old route is gone, and
            // beginBlock sends its held notes' offs with the old ports and channel.
            const auto kept=std::find_if(taps_.begin(),taps_.end(),[&](const auto& o){return o.first.key==t.key && o.first.from==t.from && o.first.destination==t.destination;});
            auto route=kept!=taps_.end() && std::none_of(next.begin(),next.end(),[&](const auto& n){return n.second==kept->second;})?kept->second:std::make_shared<NoteRoute>(16u);
            next.emplace_back(std::move(t),std::move(route));
        }
        taps_.swap(next);
    }

    void Graph::setHarmonies( std::vector<Harmony> harmonies )
    {
        for(const auto& h:harmonies)
            if(h.zones.size()>NoteZones::capacity || !std::all_of(h.zones.begin(),h.zones.end(),[](const NoteZone& z){return z.valid();}) ||
               h.root>11 || h.scale>=4096 || !(h.scale&1))
                throw std::invalid_argument("invalid harmony: zones, root or scale");
        decltype(harmonies_) next;
        for(auto& h:harmonies)
        {
            const auto kept=std::find_if(harmonies_.begin(),harmonies_.end(),[&](const auto& o){return o.first.key==h.key;});
            // Held keys are routed through the old input and zones: if either changed, later
            // releases may never reach them, so start fresh (a lost release is a lost chord).
            const bool same=kept!=harmonies_.end() && kept->first.input==h.input && kept->first.zones==h.zones;
            next.emplace_back(std::move(h),same?kept->second:std::make_shared<HeldKeys>());
        }
        harmonies_.swap(next);
    }

    std::vector<Graph::MidiTap> Graph::midiTaps() const
    {
        std::vector<MidiTap> out;for(const auto& [t,route]:taps_)out.push_back(t);return out;
    }

    void Graph::setDraining( NodeId from, NodeId to, bool draining )
    {
        if(draining)draining_.insert({from,to});else draining_.erase({from,to});
    }

    void Graph::setRetiring( NodeId node, bool retiring )
    {
        auto& n=nodes_.at(node);
        if(retiring==n.retiring)return;
        n.retiring=retiring;
        // Counting starts afresh; the audio thread takes the flag with the next plan.
        n.activity=std::make_shared<Activity>();
        n.activity->sounding.store(UINT32_MAX,std::memory_order_relaxed); // not idle until it has been counted
    }

    uint32_t Graph::sounding( NodeId node ) const
    {
        const auto it=nodes_.find(node);
        return it==nodes_.end() || !it->second.activity ? UINT32_MAX : it->second.activity->sounding.load(std::memory_order_relaxed);
    }
    bool Graph::silent( NodeId node, uint64_t quietFrames ) const
    {
        const auto& a=*nodes_.at(node).activity;
        return !a.sounding.load(std::memory_order_relaxed) && (a.asleep.load(std::memory_order_relaxed) || a.quiet.load(std::memory_order_relaxed)>=quietFrames);
    }
    void Graph::reload( NodeId id, const std::string& path, const std::string& pluginId, uint64_t memoryRoom )
    {
        auto& node=nodes_.at(id);
        auto loaded=loadPlugin(path,pluginId,sampleRate_,maxFrames_,mode_,lv2Buffers_,memoryRoom);
        node.host=preparedInstrument(std::move(loaded),sampleRate_,maxFrames_,mode_,lv2Buffers_);node.host->setAssets(assets_);
        node.audioInputs=countChannels(node.host->inputPorts());node.audioOutputs=countChannels(node.host->outputPorts());
    }
    bool Graph::idle( NodeId node, uint64_t quietFrames, uint64_t limitFrames ) const
    {
        const auto& a=*nodes_.at(node).activity;
        if(a.sounding.load(std::memory_order_relaxed))return false;
        return a.asleep.load(std::memory_order_relaxed) || a.quiet.load(std::memory_order_relaxed)>=quietFrames || a.free.load(std::memory_order_relaxed)>=limitFrames;
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
        for(const auto& r:audioRoutes_)if(r.from.port!=ringOutPort)result.audioRoutes.push_back(r); // internal: see ringOutPort
        result.audioLayout=audioLayout_;
        for(const auto& [edge,state]:noteEdges_)result.notes.push_back(edge);
        result.noteZones=noteZones_;result.draining=draining_;
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
        plan->replacementSerial=replacementSerial_;
        std::map<NodeId, int> stepOf;
        for( const NodeId id : order )
        {
            const auto& node = nodes_.at( id );
            Plan::Step  step;
            step.host         = node.host;
            step.id           = id;
            step.peaks        = node.peaks;
            step.retiring     = node.retiring;
            step.activity     = node.activity;
            step.audioInputs  = node.audioInputs;
            step.audioOutputs = node.audioOutputs;
            // The ring-out buffer's two channels, after the node's own, while it is routed.
            step.ringOut      = std::any_of(audioRoutes_.begin(),audioRoutes_.end(),[&](const auto& r){return r.from.node==id && r.from.port==ringOutPort;});
            if(step.ringOut) step.audioOutputs += 2;
            stepOf[id]        = (int)plan->steps.size();
            plan->steps.push_back( std::move( step ) );
        }
        auto route=[&](NodeId from,uint32_t source,NodeId to,uint32_t dest,float gain,const std::array<uint8_t,4>& live)
        {
            Plan::AudioRoute r{from==graphInput?-1:stepOf.at(from),source,dest,gain};r.now=gain;r.live=live;
            if(to==graphOutput)plan->outputRoutes.push_back(r);
            else plan->steps[stepOf.at(to)].audioSources.push_back(r);
        };
        for(const auto& c:audioRoutes_)
        {
            const auto from=resolve(c.from,true,c.channels),to=resolve(c.to,false,c.channels);
            for(uint32_t i=0;i<c.channels;++i)route(c.from.node,from+i,c.to.node,to+i,static_cast<float>(c.gain),c.live);
        }
        for( const auto& [edge,state] : noteEdges_ )
        {
            const auto [from,to]=edge;
            if( !nodes_.count( to ) || ( from != graphInput && !nodes_.count( from ) ) )
                continue;
            NoteZones zones;
            if(const auto z=noteZones_.find(edge);z!=noteZones_.end()){zones.count=z->second.size();std::copy(z->second.begin(),z->second.end(),zones.zones.begin());}
            plan->steps[(size_t)stepOf[to]].noteSources.push_back({from == graphInput ? -1 : stepOf[from],state,zones,draining_.count(edge)!=0});
        }

        for(const auto& [tap,route]:taps_)
        {
            if(tap.from!=graphInput && !nodes_.count(tap.from))continue; // removed since
            Plan::Tap t{tap.from==graphInput?-1:stepOf.at(tap.from),route,{},tap.ports,tap.channel};
            t.zones.count=tap.zones.size();std::copy(tap.zones.begin(),tap.zones.end(),t.zones.zones.begin());
            plan->taps.push_back(std::move(t));
        }

        for(const auto& [harmony,held]:harmonies_)
        {
            Plan::Harmony h{held,{},harmony.input,harmony.root,harmony.scale};
            h.zones.count=harmony.zones.size();std::copy(harmony.zones.begin(),harmony.zones.end(),h.zones.zones.begin());
            for(const auto node:harmony.nodes)if(const auto s=stepOf.find(node);s!=stepOf.end())plan->steps[(size_t)s->second].harmony=int(plan->harmonies.size());
            plan->harmonies.push_back(std::move(h));
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
        plan->keys.reserve( 2 * kick::clap::Host::maxBlockEvents ); // releases, then the beat and harmony, never reallocating
        plan->merged.resize( kick::clap::Host::maxProcessEvents ); // maxBlockEvents notes, plus the beat and up to 32 harmony changes
        return plan.release();
    }

    void Graph::commit(bool rampGains)
    {
        Plan* plan = compile(); // throws on a cycle, before anything changes
        ++plansCompiled_;
        plan->rampGains = rampGains;
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

    bool Graph::beginBlock() noexcept {
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
                    // A tap gone: note offs for the notes it holds, and its pedals released.
                    for(auto& old:current_->taps)
                        if(std::none_of(plan->taps.begin(),plan->taps.end(),[&](const auto& t){return t.route==old.route;}))
                        {
                            auto send=[&](const kick::clap::Event& e){
                                MidiMessage m;if(!toMidi(e,old.channel,m))return;
                                if(midiOutCount_==maxMidiOut){eventDrops_.fetch_add(1,std::memory_order_relaxed);return;}
                                m.ports=old.ports;midiOut_[midiOutCount_++]=m;};
                            for(const auto& n:old.route->notes)if(n.used)send(old.route->release(n,0,false));
                            for(int p=0;p<16;++p)for(int c=0;c<16;++c)if(old.route->pedal[p][c]>=64)send(kick::clap::Event::midi(uint8_t(0xb0|c),64,0));
                            old.route->clear();
                        }
                    // A played change (commit(true)): a route in both plans ramps from the gain it has now to its new one.
                    const auto ramp=std::max<uint32_t>(1,uint32_t(sampleRate_*.005));
                    auto carry=[&](const std::vector<Plan::AudioRoute>& was,const std::vector<Plan::AudioRoute>& next){
                        auto host=[](const Plan& p,int i){return i<0?nullptr:p.steps[size_t(i)].host.get();};
                        for(auto& r:next)for(const auto& o:was)
                            if(o.from==r.from && o.to==r.to && host(*current_,o.source)==host(*plan,r.source) && (o.source<0)==(r.source<0))
                            {
                                r.now=o.left?o.now:o.gain;
                                if(r.now!=r.gain){r.left=ramp;r.delta=(r.gain-r.now)/float(ramp);}
                                break;
                            }
                    };
                    if(plan->rampGains)
                    {
                        carry(current_->outputRoutes,plan->outputRoutes);
                        for(auto& next:plan->steps)for(const auto& old:current_->steps)if(old.host==next.host){carry(old.audioSources,next.audioSources);break;}
                    }
                    retired_.push( current_ );
                }
                current_ = plan;
                return replacement;
            }
        return {};
    }

    Graph::Peaks Graph::takePeaks( NodeId node ) noexcept
    {
        Peaks out{};
        if( const auto it = nodes_.find( node ); it != nodes_.end() )
            for( uint32_t c = 0; c < meterChannels; ++c ) out[c] = ( *it->second.peaks )[c].exchange( 0.f, std::memory_order_relaxed );
        return out;
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
        auto* const listener=listener_.load(std::memory_order_acquire);
        // Live gains for this block: each slot ramps from where the last block left it.
        for(uint32_t i=1;i<liveGains;++i)
        {
            const bool jump=liveJump_[i].load(std::memory_order_relaxed) && liveJump_[i].exchange(false,std::memory_order_acquire);
            const float to=liveTarget_[i].load(std::memory_order_relaxed);
            liveFrom_[i]=jump?to:liveTo_[i];liveTo_[i]=to;
        }
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
            if(r.live[0])
            {
                // Scaled by its live gains, ramped across the block from the last block's product.
                float start=1,end=1;
                for(const auto slot:r.live)if(slot){start*=liveFrom_[slot];end*=liveTo_[slot];}
                if(start!=end || r.left)
                {
                    const float step=(end-start)/float(frames);
                    for(uint32_t i=0;i<frames;++i)
                    {
                        if(r.left){r.now+=r.delta;if(--r.left==0)r.now=r.gain;}
                        const float g=r.now*(start+step*float(i+1));
                        dest[i]=r.first?from[i]*g:dest[i]+from[i]*g;
                    }
                    return;
                }
                const float g=r.gain*end;
                if(r.first)for(uint32_t i=0;i<frames;++i)dest[i]=from[i]*g;
                else for(uint32_t i=0;i<frames;++i)dest[i]+=from[i]*g;
                return;
            }
            if(r.left)
            {
                for(uint32_t i=0;i<frames;++i)
                {
                    if(r.left){r.now+=r.delta;if(--r.left==0)r.now=r.gain;}
                    dest[i]=r.first?from[i]*r.now:dest[i]+from[i]*r.now;
                }
                return;
            }
            if(r.first)
            {
                if(r.gain==1.f)std::copy_n(from,frames,dest);
                else for(uint32_t i=0;i<frames;++i)dest[i]=from[i]*r.gain;
            }
            else if(r.gain==1.f)for(uint32_t i=0;i<frames;++i)dest[i]+=from[i];
            else for(uint32_t i=0;i<frames;++i)dest[i]+=from[i]*r.gain;
        };
        // RFC 0032: the free-running beat (oscairn-clap/beat.h), the same for every node this block.
        const double tempo=transport && (transport->flags&CLAP_TRANSPORT_HAS_TEMPO) && std::isfinite(transport->tempo) && transport->tempo>0?transport->tempo:120.0;
        oscairn_event_beat_t beat{};
        beat.header={sizeof(beat),0,kick::clap::Host::beatSpace,OSCAIRN_EVENT_BEAT,0};
        beat.beats=clap_beattime(std::llround(freeBeats_*double(CLAP_BEATTIME_FACTOR)));
        beat.tempo=tempo;
        const auto beatEvent=kick::clap::Event::from(beat);
        freeBeats_+=frames/sampleRate_*tempo/60.0;
        // RFC 0033: each part's harmony at sample 0, then each change its input's keys make.
        for(auto& h:plan->harmonies)
        {
            auto& held=*h.held;
            auto add=[&](uint32_t time){
                oscairn_harmony_t x{};x.key_root=h.root;x.scale=h.scale;
                for(const auto& n:held.route.notes)if(n.used && n.held && n.key>=0 && n.key<128)x.held[n.key/64]|=uint64_t(1)<<(n.key%64);
                recognizeChord(x);
                if(h.count && !std::memcmp(&x,&held.last,sizeof x))return;
                held.last=x;
                oscairn_event_harmony_t e{};
                e.header={sizeof(e),time,kick::clap::Host::harmonySpace,OSCAIRN_EVENT_HARMONY,0};
                e.harmony=x;
                // shortcut: past 32 changes in one block the last slot takes each later one, at its time; raise the capacity if played input ever needs more.
                if(h.count==h.events.size())--h.count;
                h.events[h.count++]=kick::clap::Event::from(e);};
            h.count=0;add(0);
            if(h.input)for(uint32_t i=0;i<eventCount;++i)
                if(events[i].header()->space_id==CLAP_CORE_EVENT_SPACE_ID)
                {
                    held.route.route(events[i],nextNoteId_,[](const kick::clap::Event&){return true;},&h.zones,sources?sources[i]:untaggedSource);
                    add(std::min(events[i].header()->time,frames-1));
                }
        }
        bool ok = true;
        for( auto& step : plan->steps )
        {
            const bool inputRecovery=panicInput && std::any_of(step.noteSources.begin(),step.noteSources.end(),[](const auto& s){return s.step<0;});
            for(auto c:step.silentInputs)std::fill_n(step.inputBuffer.data()+size_t(c)*maxFrames_,frames,0.f);
            for(const auto& r:step.audioSources)mix(r,step.inputBuffer.data()+size_t(r.to)*maxFrames_);

            // Notes in: every source's events this block, merged in time order.
            plan->keys.clear();
            uint32_t order = 0;
            plan->keys.push_back({0,order++,&beatEvent}); // oscairn.beat, first at sample 0
            if(step.harmony>=0) // the part's harmony, each before the notes at its sample
            {
                const auto& h=plan->harmonies[(size_t)step.harmony];
                for(uint32_t i=0;i<h.count;++i)plan->keys.push_back({h.events[i].header()->time,order++,&h.events[i]});
            }
            uint64_t totalEvents=step.paramCount+step.paramDrops+step.releaseCount;
            bool overflow=step.paramDrops!=0 || step.releaseOverflow || inputRecovery;
            // The beat and the part's harmony ride with the notes in their own room (see
            // maxProcessEvents): a block that fills all maxBlockEvents note slots still fits.
            for(uint32_t i=0;i<step.releaseCount;++i)plan->keys.push_back({0,order++,&step.releases[i]});
            for( uint32_t i = 0; i < step.paramCount; ++i ) {
                if(plan->keys.size()<plan->merged.size())
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
                        if(plan->keys.size()<plan->merged.size())
                            plan->keys.push_back( { list[i].header()->time, order++, &list[i],connection.route.get(),false,&connection.zones,
                                                    source<0 && sources?sources[i]:untaggedSource,connection.draining } );
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
            // Stolen notes' fade steps go in among the events, at their times.
            auto fades=[&](uint32_t until){bool ok=true;for(auto& source:step.noteSources)ok=source.route->fade(until,emit) && ok;return ok;};
            if(!overflow)for(const auto& key:plan->keys)
                if(!fades(std::min(key.time,frames)) ||
                   !(key.route && key.time<frames?key.route->route(*key.event,nextNoteId_,emit,key.zones,key.source,key.draining):emit(*key.event))){overflow=true;break;}
            if(!overflow && !fades(frames))overflow=true;
            for(auto& source:step.noteSources)source.route->advance(frames);
            if(overflow)
            {
                // A lost release must reset the destination in its own dialect.
                eventDrops_.fetch_add(totalEvents,std::memory_order_relaxed);
                merged=0;step.host->panic();
                for(auto& source:step.noteSources)source.route->clear();
            }

            if(listener)listener->events(step.id,plan->merged.data(),merged,false);
            const bool processed=step.host->process(step.in.data(),step.audioInputs,step.out.data(),step.audioOutputs,frames,plan->merged.data(),merged,transport);
            if(!processed) {
                // A busy control gate can discard a release; reset when rendering resumes.
                step.host->panic();
                for(auto& source:step.noteSources)source.route->clear();
            }
            ok &= processed;
            if(listener && processed){uint32_t sent=0;const auto* list=step.host->outputEvents(sent);listener->events(step.id,list,sent,true);}
            if(processed && step.peaks)
                for(uint32_t c=0;c<std::min(step.audioOutputs-(step.ringOut?2u:0u),meterChannels);++c)
                {
                    float peak=0;
                    for(uint32_t i=0;i<frames;++i)peak=std::max(peak,std::abs(step.out[c][i]));
                    auto& held=(*step.peaks)[c];
                    for(float seen=held.load(std::memory_order_relaxed);peak>seen && !held.compare_exchange_weak(seen,peak,std::memory_order_relaxed););
                }
            // Every node: the notes sounding through it (for its cost per voice, RFC 0025), and how
            // long it has been silent, so a retiring node may go and a free one be taken over (RFC 0021).
            if(step.activity)
            {
                uint32_t sounding=0;for(const auto& source:step.noteSources)sounding+=source.route->sounding();
                auto& a=*step.activity;
                a.sounding.store(sounding,std::memory_order_relaxed);
                if(processed)
                {
                    float loudest=0;for(uint32_t c=0;c<step.audioOutputs;++c)for(uint32_t i=0;i<frames;++i)loudest=std::max(loudest,std::abs(step.out[c][i]));
                    a.quiet.store(loudest<1e-5f?a.quiet.load(std::memory_order_relaxed)+frames:0,std::memory_order_relaxed);
                    a.free.store(sounding?0:a.free.load(std::memory_order_relaxed)+frames,std::memory_order_relaxed);
                    a.asleep.store(step.host->asleep(),std::memory_order_relaxed);
                }
                // A block the plugin never rendered (a worker that died and did not restart) is
                // as good as silence: it counts toward the limit, or such a node is never idle.
                else a.free.store(sounding?0:a.free.load(std::memory_order_relaxed)+frames,std::memory_order_relaxed);
            }
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

        // MIDI for external outputs (setMidiTaps), from the input or a node's note outputs.
        for(auto& tap:plan->taps)
        {
            const kick::clap::Event* list=events;uint32_t count=eventCount;
            if(tap.step>=0)list=plan->steps[(size_t)tap.step].host->outputEvents(count);
            auto emit=[&](const kick::clap::Event& e){
                MidiMessage m;if(!toMidi(e,tap.channel,m))return true;
                if(midiOutCount_==maxMidiOut){eventDrops_.fetch_add(1,std::memory_order_relaxed);return true;}
                m.time=std::min(e.header()->time,frames-1);m.ports=tap.ports;midiOut_[midiOutCount_++]=m;return true;};
            for(uint32_t i=0;i<count;++i)
                if(list[i].header()->space_id==CLAP_CORE_EVENT_SPACE_ID && list[i].header()->type!=CLAP_EVENT_NOTE_END)
                    tap.route->route(list[i],nextNoteId_,emit,&tap.zones,tap.step<0 && sources?sources[i]:untaggedSource);
        }
        for(const auto& r:plan->outputRoutes)mix(r,outputs[r.to]);
        return ok;
    }

    const float* Graph::audioOutput(NodeId node,uint32_t channel) const noexcept
    {
        const Plan* plan=current_;
        if(!plan)return nullptr;
        if(node==graphInput)return channel<inputChannels_?plan->externalInputs.data()+size_t(channel)*maxFrames_:nullptr;
        for(const auto& step:plan->steps)if(step.id==node)return channel<step.audioOutputs?step.out[channel]:nullptr;
        return nullptr;
    }
}

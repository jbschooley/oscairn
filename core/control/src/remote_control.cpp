// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/remote_control.h"
#include "PropertyWire.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace oscairn
{
    namespace
    {
        using V=const json_value_s*;
        using Doc=std::unique_ptr<json_value_s,decltype(&std::free)>;
        V field(V v,std::string_view key)
        {
            if(V value=detail::member(v,key))return value;
            throw std::runtime_error("Missing RPC field: "+std::string(key));
        }
        std::string string(V v)
        {
            if(!v || v->type!=json_type_string)throw std::runtime_error("Expected RPC string");
            const auto* s=static_cast<const json_string_s*>(v->payload);return {s->string,s->string_size};
        }
        double number(V v)
        {
            if(!v || v->type!=json_type_number)throw std::runtime_error("Expected RPC number");
            const auto* n=static_cast<const json_number_s*>(v->payload);
            std::istringstream stream(std::string(n->number,n->number_size));stream.imbue(std::locale::classic());double result=0;stream>>result;
            if(stream.fail() || !stream.eof() || !std::isfinite(result))throw std::runtime_error("Invalid RPC number");return result;
        }
        uint64_t integer(V v)
        {
            if(!v || v->type!=json_type_number)throw std::runtime_error("Expected RPC integer");
            const auto* n=static_cast<const json_number_s*>(v->payload);uint64_t result=0;
            const auto parsed=std::from_chars(n->number,n->number+n->number_size,result);
            if(parsed.ec!=std::errc{} || parsed.ptr!=n->number+n->number_size)throw std::runtime_error("Invalid RPC integer");
            return result;
        }
        bool boolean(V v)
        {
            if(!v || (v->type!=json_type_true && v->type!=json_type_false))throw std::runtime_error("Expected RPC boolean");return v->type==json_type_true;
        }
        std::vector<V> array(V v)
        {
            if(!v || v->type!=json_type_array)throw std::runtime_error("Expected RPC array");
            std::vector<V> result;for(auto* e=static_cast<const json_array_s*>(v->payload)->start;e;e=e->next)result.push_back(e->value);return result;
        }
        using detail::quote;
        std::string encode(double value)
        {
            if(!std::isfinite(value))throw std::invalid_argument("Nonfinite control value");
            char buffer[64];const auto end=std::to_chars(buffer,buffer+sizeof(buffer),value);
            if(end.ec!=std::errc{})throw std::runtime_error("Cannot encode control value");return {buffer,end.ptr};
        }
        std::string address(NodeId node,uint32_t param){return "\"node\":"+std::to_string(node)+",\"param\":"+std::to_string(param);}
        struct RpcError:std::runtime_error{int code;RpcError(int c,const std::string& message):std::runtime_error(message),code(c){}};
        Doc parse(const std::string& message)
        {
            // Responses were depth/size checked by LocalClient. Notification frames
            // receive the same check here before entering the recursive parser.
            size_t depth=0;bool quoted=false,escaped=false;
            for(char c:message)if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
                else if(c=='"')quoted=true;else if(c=='['||c=='{'){if(++depth>32)throw std::runtime_error("RPC nesting limit");}
                else if((c==']'||c=='}')&&depth)--depth;
            Doc result(json_parse(message.data(),message.size()),&std::free);if(!result)throw std::runtime_error("Invalid RPC JSON");return result;
        }
    }
    struct RemoteControl::Impl
    {
        LocalClient& client;
        GraphState graph;SessionStatus status;TransportState transport;
        Transport::ClockSource clock=Transport::ClockSource::Internal;
        std::map<NodeId,std::vector<ParamInfo>> info;
        std::map<std::pair<NodeId,uint32_t>,double> values;
        std::deque<Change> history;uint64_t sequence=0;
        explicit Impl(LocalClient& c):client(c){}
        Doc call(std::string_view method,std::string_view params="{}",unsigned timeout=1000)
        {
            auto doc=parse(client.call(method,params,timeout));
            V error=nullptr;try{error=field(doc.get(),"error");}catch(const std::runtime_error&){}
            if(error)
            {
                const auto code=static_cast<int>(number(field(error,"code")));
                const auto message=string(field(error,"message"));
                if(code==-32602 || code==-32001 || code==-32002)throw std::invalid_argument(message);
                throw RpcError(code,message);
            }
            field(doc.get(),"result");return doc;
        }
        void publish(Change change)
        {
            if(history.size()==4096)history.pop_front();change.sequence=++sequence;history.push_back(change);
        }
        void readTransport(V v)
        {
            transport={boolean(field(v,"playing")),number(field(v,"tempo")),number(field(v,"beat"))};
            clock=string(field(v,"clockSource"))=="internal"?Transport::ClockSource::Internal:Transport::ClockSource::MidiClock;
        }
        void readStatus(V v)
        {
            status.audioDevice=string(field(v,"audioDevice"));status.sampleRate=number(field(v,"sampleRate"));
            status.bufferSize=static_cast<uint32_t>(integer(field(v,"bufferSize")));status.cpuLoad=number(field(v,"cpuLoad"));
            auto channels=[](V v){std::vector<DeviceChannel> result;for(auto c:array(v))result.push_back({static_cast<uint32_t>(integer(field(c,"deviceChannel"))),string(field(c,"name")),boolean(field(c,"available"))});return result;};
            status.audioInputs=channels(field(v,"audioInputs"));status.audioOutputs=channels(field(v,"audioOutputs"));
            status.audioReady=boolean(field(v,"audioReady"));
            status.graphEventDrops=integer(field(v,"graphEventDrops"));status.midiEventDrops=integer(field(v,"midiEventDrops"));
            status.deadlineMisses=integer(field(v,"deadlineMisses"));status.midiInputs.clear();
            for(auto name:array(field(v,"midiInputs")))status.midiInputs.push_back(string(name));
        }
        void refresh()
        {
            auto doc=call("graph.get");auto root=field(doc.get(),"result");GraphState next;
            next.revision=integer(field(root,"revision"));next.staged=boolean(field(root,"staged"));
            auto ports=[](V v){std::vector<kick::clap::Port> result;for(auto p:array(v))result.push_back({static_cast<uint32_t>(integer(field(p,"id"))),string(field(p,"name")),static_cast<uint32_t>(integer(field(p,"channels"))),static_cast<uint32_t>(integer(field(p,"flags")))});return result;};
            next.topology.audioLayout.inputs=ports(field(root,"audioInputs"));
            next.topology.audioLayout.outputs=ports(field(root,"audioOutputs"));
            for(auto n:array(field(root,"nodes")))
            {
                Graph::NodeInfo node{};node.id=static_cast<NodeId>(integer(field(n,"id")));next.names[node.id]=string(field(n,"name"));
                auto plugin=field(n,"plugin");node.plugin.id=string(field(plugin,"id"));node.plugin.name=string(field(plugin,"name"));node.plugin.path=string(field(plugin,"path"));
                node.inputs=ports(field(n,"audioInputs"));node.outputs=ports(field(n,"audioOutputs"));
                node.noteInputs=static_cast<uint32_t>(integer(field(n,"noteInputs")));node.noteOutputs=static_cast<uint32_t>(integer(field(n,"noteOutputs")));
                next.topology.nodes.push_back(std::move(node));
            }
            for(auto c:array(field(root,"connections")))
            {
                const auto from=static_cast<NodeId>(integer(field(c,"from"))),to=static_cast<NodeId>(integer(field(c,"to")));
                if(string(field(c,"kind"))=="notes")next.topology.notes.emplace_back(from,to);
                else next.topology.audioRoutes.push_back({
                    {from,static_cast<uint32_t>(integer(field(c,"fromPort"))),static_cast<uint32_t>(integer(field(c,"fromChannel")))},
                    {to,static_cast<uint32_t>(integer(field(c,"toPort"))),static_cast<uint32_t>(integer(field(c,"toChannel")))},
                    static_cast<uint32_t>(integer(field(c,"channels"))),number(field(c,"gain"))});
            }
            for(auto m:array(field(root,"mappings")))next.topology.mappings.push_back({static_cast<NodeId>(integer(field(m,"from"))),static_cast<uint32_t>(integer(field(m,"lane"))),static_cast<NodeId>(integer(field(m,"to"))),static_cast<uint32_t>(integer(field(m,"param")))});
            std::map<NodeId,std::vector<ParamInfo>> metadata;std::map<std::pair<NodeId,uint32_t>,double> snapshot;
            std::string batch;size_t count=0;
            auto flush=[&]{if(!count)return;auto reply=call("params.get","{\"values\":["+batch+"]}");for(auto value:array(field(field(reply.get(),"result"),"values")))snapshot[{static_cast<NodeId>(integer(field(value,"node"))),static_cast<uint32_t>(integer(field(value,"param")))}]=number(field(value,"value"));batch.clear();count=0;};
            for(const auto& n:next.topology.nodes)
            {
                auto reply=call("params.list","{\"node\":"+std::to_string(n.id)+'}');
                for(auto p:array(field(field(reply.get(),"result"),"parameters")))
                {
                    ParamInfo param{static_cast<uint32_t>(integer(field(p,"id"))),string(field(p,"name")),number(field(p,"minimum")),number(field(p,"maximum")),number(field(p,"defaultValue")),boolean(field(p,"stepped")),static_cast<uint32_t>(integer(field(p,"flags")))};
                    metadata[n.id].push_back(param);if(count)batch+=',';batch+='{'+address(n.id,param.id)+'}';if(++count==128)flush();
                }
                metadata.try_emplace(n.id);
            }
            flush();graph=std::move(next);info=std::move(metadata);values=std::move(snapshot);
        }
        void audioEdge(std::string_view method,const AudioConnection& r)
        {
            call(method,"{\"kind\":\"audio\",\"from\":"+std::to_string(r.from.node)+",\"to\":"+std::to_string(r.to.node)+
                ",\"fromPort\":"+std::to_string(r.from.port)+",\"fromChannel\":"+std::to_string(r.from.channel)+
                ",\"toPort\":"+std::to_string(r.to.port)+",\"toChannel\":"+std::to_string(r.to.channel)+
                ",\"channels\":"+std::to_string(r.channels)+(method=="graph.connect"?",\"gain\":"+encode(r.gain):std::string{})+'}');refresh();
        }
        void edge(std::string_view method,const char* kind,NodeId from,NodeId to)
        {call(method,"{\"kind\":"+quote(kind)+",\"from\":"+std::to_string(from)+",\"to\":"+std::to_string(to)+'}');refresh();}
    };
    RemoteControl::RemoteControl(LocalClient& client):impl_(std::make_unique<Impl>(client))
    {
        auto hello=impl_->call("session.hello");if(integer(field(field(hello.get(),"result"),"protocolVersion"))!=1)throw std::runtime_error("Unsupported control protocol version");
        impl_->call("subscribe",R"({"topics":["params.changed","transport","graph.changed","status"],"allChanges":true})");
        impl_->refresh();auto state=impl_->call("transport.get");impl_->readTransport(field(state.get(),"result"));
        auto status=impl_->call("session.status");impl_->readStatus(field(status.get(),"result"));
        // Keep interleaved notifications: another client may have changed a value
        // after its snapshot was read. service() drains them in server order.
    }
    RemoteControl::~RemoteControl()=default;
    Control::GraphState RemoteControl::graphState()const{return impl_->graph;}
    Control::SessionStatus RemoteControl::sessionStatus()const{return impl_->status;}
    void RemoteControl::savePerformance(const std::string& directory) {
        impl_->call("performance.save","{\"directory\":"+quote(directory)+'}',30000);
    }
    void RemoteControl::loadPerformance(const std::string& directory) {
        impl_->call("performance.load","{\"directory\":"+quote(directory)+'}',30000);impl_->refresh();
    }
    bool RemoteControl::supportsState(NodeId node) const {
        auto d=impl_->call("state.info","{\"node\":"+std::to_string(node)+'}');return boolean(field(field(d.get(),"result"),"supported"));
    }
    void RemoteControl::saveState(NodeId node,const std::string& directory) {
        impl_->call("state.save","{\"node\":"+std::to_string(node)+",\"directory\":"+quote(directory)+'}',30000);
    }
    std::vector<PresetInfo> RemoteControl::presets(NodeId node) {
        auto d=impl_->call("presets.list","{\"node\":"+std::to_string(node)+'}');std::vector<PresetInfo> result;
        for(auto v:array(field(field(d.get(),"result"),"presets")))result.push_back({string(field(v,"uri")),string(field(v,"name"))});
        return result;
    }
    std::vector<PropertyInfo> RemoteControl::properties(NodeId node) const {
        auto d=impl_->call("properties.list","{\"node\":"+std::to_string(node)+'}');std::vector<PropertyInfo> result;
        for(auto v:array(field(field(d.get(),"result"),"properties")))result.push_back({string(field(v,"uri")),string(field(v,"name")),property_wire::type(string(field(v,"type"))),boolean(field(v,"readable")),boolean(field(v,"writable"))});
        return result;
    }
    bool RemoteControl::setProperty(NodeId node,const std::string& uri,const PropertyValue& value) {
        try{impl_->call("properties.set","{\"node\":"+std::to_string(node)+",\"uri\":"+quote(uri)+",\"value\":"+property_wire::encode(value,quote,encode)+'}');return true;}
        catch(const RpcError& e){if(e.code==-32005)return false;throw;}
    }
    bool RemoteControl::requestProperty(NodeId node,const std::string& uri) {
        try{impl_->call("properties.request","{\"node\":"+std::to_string(node)+",\"uri\":"+quote(uri)+'}');return true;}
        catch(const RpcError& e){if(e.code==-32005)return false;throw;}
    }
    std::optional<PropertyValue> RemoteControl::propertyValue(NodeId node,const std::string& uri) const {
        auto d=impl_->call("properties.get","{\"node\":"+std::to_string(node)+",\"uri\":"+quote(uri)+'}');auto v=field(field(d.get(),"result"),"value");
        if(v && v->type==json_type_null)return {};
        return property_wire::decode(v);
    }
    Control::Sounds RemoteControl::sounds(NodeId node)const
    {
        auto doc=impl_->call("sounds.get","{\"node\":"+std::to_string(node)+'}');auto value=field(doc.get(),"result");
        Sounds result{static_cast<uint32_t>(integer(field(value,"capacity"))),{},string(field(value,"mode"))};
        for(auto c:array(field(value,"contexts")))result.contexts.push_back({static_cast<uint32_t>(integer(field(c,"id"))),string(field(c,"name")),static_cast<uint32_t>(integer(field(c,"selectedChannels"))),static_cast<uint32_t>(integer(field(c,"activeVoices"))),boolean(field(c,"idle"))});
        return result;
    }
    uint32_t RemoteControl::prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& state,const std::string& preset,const std::string& sfz,const std::string& directory)
    {
        std::string values;for(const auto& p:parameters){if(!values.empty())values+=',';values+="{\"id\":"+std::to_string(p.id)+",\"value\":"+encode(p.value)+'}';}
        auto doc=impl_->call("sounds.prepare","{\"node\":"+std::to_string(node)+",\"name\":"+quote(name)+",\"parameters\":["+values+"],\"state\":"+quote(state)+",\"preset\":"+quote(preset)+",\"sfz\":"+quote(sfz)+",\"directory\":"+quote(directory)+"}",30000);
        return static_cast<uint32_t>(integer(field(field(doc.get(),"result"),"context")));
    }
    bool RemoteControl::selectSound(NodeId node,uint32_t id,int16_t channel)
    {try{impl_->call("sounds.select","{\"node\":"+std::to_string(node)+",\"context\":"+std::to_string(id)+",\"channel\":"+std::to_string(channel)+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    bool RemoteControl::discardSound(NodeId node,uint32_t id,bool cutTail)
    {try{impl_->call("sounds.discard","{\"node\":"+std::to_string(node)+",\"context\":"+std::to_string(id)+",\"cutTail\":"+(cutTail?"true":"false")+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    NodeId RemoteControl::nodeId(const std::string& name)const
    {for(const auto& [id,n]:impl_->graph.names)if(!name.empty()&&n==name)return id;throw std::invalid_argument("Unknown node name");}
    bool RemoteControl::hasNode(NodeId n)const{return impl_->graph.names.contains(n);}
    bool RemoteControl::hasParameter(NodeId n,uint32_t p)const{return impl_->values.contains({n,p});}
    std::vector<Control::ParamInfo> RemoteControl::parameters(NodeId n)const
    {const auto it=impl_->info.find(n);if(it==impl_->info.end())throw std::invalid_argument("Unknown node");return it->second;}
    Control::ParamInfo RemoteControl::parameterInfo(NodeId n,uint32_t p)const
    {for(const auto& v:parameters(n))if(v.id==p)return v;throw std::invalid_argument("Unknown parameter");}
    double RemoteControl::parameter(NodeId n,uint32_t p)const
    {const auto it=impl_->values.find({n,p});if(it==impl_->values.end())throw std::invalid_argument("Unknown parameter");return it->second;}
    std::string RemoteControl::parameterText(NodeId n,uint32_t p,double value)const
    {auto doc=impl_->call("params.text",'{'+address(n,p)+",\"value\":"+encode(value)+'}');return string(field(field(doc.get(),"result"),"text"));}
    bool RemoteControl::setParameter(NodeId n,uint32_t p,double value)
    {
        try{auto doc=impl_->call("params.set","{\"values\":[{"+address(n,p)+",\"value\":"+encode(value)+"}]}");impl_->values[{n,p}]=number(field(array(field(field(doc.get(),"result"),"values")).at(0),"value"));return true;}
        catch(const RpcError& e){if(e.code==-32005)return false;throw;}
    }
    bool RemoteControl::parameterGesture(NodeId n,uint32_t p,bool begin)
    {try{impl_->call("params.gesture",'{'+address(n,p)+",\"begin\":"+(begin?"true":"false")+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    bool RemoteControl::note(bool on,int key,double velocity,int channel)
    {try{impl_->call("notes.send",std::string("{\"on\":")+(on?"true":"false")+",\"key\":"+std::to_string(key)+",\"velocity\":"+encode(velocity)+",\"channel\":"+std::to_string(channel)+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    void RemoteControl::setPlaying(bool playing){impl_->call("transport.set",std::string("{\"playing\":")+(playing?"true":"false")+'}');}
    void RemoteControl::setTempo(double bpm){impl_->call("transport.set","{\"tempo\":"+encode(bpm)+'}');}
    void RemoteControl::setClockSource(Transport::ClockSource source)
    {impl_->call("transport.set","{\"clockSource\":"+quote(source==Transport::ClockSource::Internal?"internal":"midi")+'}');impl_->clock=source;}
    Transport::ClockSource RemoteControl::clockSource()const{return impl_->clock;}
    Control::TransportState RemoteControl::transportState()const{return impl_->transport;}
    NodeId RemoteControl::add(const std::string& name,const std::string& path,const std::string& plugin)
    {auto doc=impl_->call("graph.add","{\"name\":"+quote(name)+",\"path\":"+quote(path)+",\"pluginId\":"+quote(plugin)+'}');const auto id=static_cast<NodeId>(integer(field(field(doc.get(),"result"),"node")));impl_->refresh();return id;}
    NodeId RemoteControl::addEffectContext(const std::string& name,NodeId source)
    {auto doc=impl_->call("effects.create","{\"name\":"+quote(name)+",\"source\":"+std::to_string(source)+'}');const auto id=static_cast<NodeId>(integer(field(field(doc.get(),"result"),"node")));impl_->refresh();return id;}
    std::optional<oscairn_effect_resources_t> RemoteControl::effectResources(NodeId n) const
    {
        auto doc=impl_->call("effects.resources","{\"node\":"+std::to_string(n)+'}');const auto* r=field(doc.get(),"result");
        if(r->type==json_type_null)return {};
        return oscairn_effect_resources_t{integer(field(r,"sharedBytes")),integer(field(r,"contextBytes")),static_cast<uint32_t>(integer(field(r,"capacity"))),static_cast<uint32_t>(integer(field(r,"prepared"))),boolean(field(r,"tailActive"))};
    }
    void RemoteControl::replace(NodeId n,const std::string& path,const std::string& plugin,const std::string& preset)
    {impl_->call("graph.replace","{\"node\":"+std::to_string(n)+",\"path\":"+quote(path)+",\"pluginId\":"+quote(plugin)+",\"preset\":"+quote(preset)+'}',30000);impl_->refresh();}
    void RemoteControl::remove(NodeId n){impl_->call("graph.remove","{\"node\":"+std::to_string(n)+'}');impl_->refresh();}
    void RemoteControl::connectAudio(NodeId a,NodeId b){impl_->edge("graph.connect","audio",a,b);}
    void RemoteControl::connectAudio(const AudioConnection& c){impl_->audioEdge("graph.connect",c);}
    void RemoteControl::disconnectAudio(const AudioConnection& c){impl_->audioEdge("graph.disconnect",c);}
    void RemoteControl::connectNotes(NodeId a,NodeId b){impl_->edge("graph.connect","notes",a,b);}
    void RemoteControl::disconnectAudio(NodeId a,NodeId b){impl_->edge("graph.disconnect","audio",a,b);}
    void RemoteControl::disconnectNotes(NodeId a,NodeId b){impl_->edge("graph.disconnect","notes",a,b);}
    void RemoteControl::mapParameter(NodeId a,uint32_t lane,NodeId b,uint32_t param)
    {impl_->call("graph.map","{\"from\":"+std::to_string(a)+",\"lane\":"+std::to_string(lane)+",\"to\":"+std::to_string(b)+",\"param\":"+std::to_string(param)+'}');impl_->refresh();}
    void RemoteControl::unmapParameter(NodeId a,uint32_t lane){impl_->call("graph.unmap","{\"from\":"+std::to_string(a)+",\"lane\":"+std::to_string(lane)+'}');impl_->refresh();}
    void RemoteControl::commit(){impl_->call("graph.commit");impl_->refresh();}
    uint64_t RemoteControl::changeSequence()const{return impl_->sequence;}
    Control::Changes RemoteControl::changesAfter(uint64_t seq)const
    {Changes result{impl_->sequence,seq>impl_->sequence||(!impl_->history.empty()&&seq<impl_->history.front().sequence-1),{}};for(const auto& event:impl_->history)if(event.sequence>seq)result.events.push_back(event);return result;}
    void RemoteControl::service()
    {
        impl_->client.poll();bool refresh=false,resync=false;
        for(const auto& message:impl_->client.takeNotifications())
        {
            auto doc=parse(message);const auto method=string(field(doc.get(),"method"));auto params=field(doc.get(),"params");
            if(method=="transport")impl_->readTransport(params);
            else if(method=="status")impl_->readStatus(params);
            else if(method=="graph.changed"){refresh=true;impl_->publish({0,ChangeKind::Graph});}
            else if(method=="session.resync"){refresh=resync=true;impl_->publish({0,ChangeKind::Resync});}
            else if(method=="params.changed")for(auto c:array(field(params,"changes")))
            {
                Change change;change.node=static_cast<NodeId>(integer(field(c,"node")));change.param=static_cast<uint32_t>(integer(field(c,"param")));
                const auto kind=string(field(c,"kind"));change.kind=kind=="value"?ChangeKind::Value:kind=="begin"?ChangeKind::GestureBegin:ChangeKind::GestureEnd;
                change.plugin=string(field(c,"source"))=="plugin";
                if(change.kind==ChangeKind::Value){change.value=number(field(c,"value"));if(impl_->values.contains({change.node,change.param}))impl_->values[{change.node,change.param}]=change.value;}
                impl_->publish(change);
            }
        }
        if(refresh)impl_->refresh();
        if(resync)
        {
            // Transport and status notifications are sent only on change; reread any that were lost.
            auto state=impl_->call("transport.get");impl_->readTransport(field(state.get(),"result"));
            auto status=impl_->call("session.status");impl_->readStatus(field(status.get(),"result"));
        }
    }
}

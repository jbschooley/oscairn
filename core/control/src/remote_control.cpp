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
        // An enum from its name in one of parts.h's tables.
        template<class Names> static size_t named(const Names& names,const std::string& name)
        {
            const auto at=std::find_if(names.begin(),names.end(),[&](const char* n){return name==n;});
            if(at==names.end())throw std::runtime_error("Unknown name: "+name);
            return size_t(at-names.begin());
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
            for(auto e:array(field(v,"midiInputs")))
                status.midiInputs.push_back({MidiEndpoint::Kind(named(endpointKinds,string(field(e,"kind")))),uint8_t(integer(field(e,"source"))),string(field(e,"name")),static_cast<uint32_t>(integer(field(e,"port")))});
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
            auto zones=[](V list){
                std::vector<NoteZone> result;
                for(auto z:array(list)){
                    NoteZone zone;const auto keys=array(field(z,"keys")),velocities=array(field(z,"velocities"));
                    if(keys.size()!=2 || velocities.size()!=2)throw std::runtime_error("Invalid zone range");
                    zone.lowKey=uint8_t(integer(keys[0]));zone.highKey=uint8_t(integer(keys[1]));zone.lowVelocity=number(velocities[0]);zone.highVelocity=number(velocities[1]);
                    zone.channels=uint16_t(integer(field(z,"channels")));zone.transpose=int8_t(number(field(z,"transpose")));
                    zone.curve=number(field(z,"curve"));zone.fixedVelocity=number(field(z,"fixedVelocity"));result.push_back(zone);
                }
                return result;
            };
            for(auto p:array(field(root,"parts")))
            {
                Part part;part.id=static_cast<uint32_t>(integer(field(p,"id")));part.name=string(field(p,"name"));
                auto input=field(p,"input");const auto kind=string(field(input,"kind"));
                part.input.kind=kind=="notes"?PartInput::Kind::Notes:kind=="audio"?PartInput::Kind::Audio:kind=="bus"?PartInput::Kind::Bus:PartInput::Kind::None;
                part.input.port=static_cast<uint32_t>(integer(field(input,"port")));part.input.zones=zones(field(input,"zones"));
                for(auto r:array(field(input,"devices")))part.input.devices.push_back({DeviceRule::Kind(named(deviceRuleKinds,string(field(r,"kind")))),
                    DeviceRule::Match(named(deviceMatches,string(field(r,"match")))),static_cast<uint32_t>(integer(field(r,"port"))),string(field(r,"pattern"))});
                part.output=static_cast<uint32_t>(integer(field(p,"output")));
                auto chain=[](V list){std::vector<NodeId> result;for(auto n:array(list))result.push_back(n->type==json_type_object?splitMark|static_cast<NodeId>(integer(field(n,"split"))):static_cast<NodeId>(integer(n)));return result;};
                part.chain=chain(field(p,"chain"));
                for(auto s:array(field(p,"splits"))){
                    Split split;split.id=static_cast<uint32_t>(integer(field(s,"id")));split.kind=Split::Kind(named(splitKinds,string(field(s,"kind"))));
                    split.splitter=static_cast<NodeId>(integer(field(s,"splitter")));split.joiner=static_cast<NodeId>(integer(field(s,"joiner")));
                    for(auto l:array(field(s,"lanes")))split.lanes.push_back({chain(field(l,"chain")),zones(field(l,"zones")),number(field(l,"level")),number(field(l,"pan")),boolean(field(l,"mute")),boolean(field(l,"solo"))});
                    part.splits.push_back(std::move(split));
                }
                part.bus=static_cast<uint32_t>(integer(field(p,"bus")));
                part.level=number(field(p,"level"));part.mute=boolean(field(p,"mute"));part.solo=boolean(field(p,"solo"));
                for(auto s:array(field(p,"sends")))part.sends.push_back({static_cast<uint32_t>(integer(field(s,"bus"))),number(field(s,"level"))});
                next.parts.push_back(std::move(part));
            }
            for(auto w:array(field(root,"wires")))
            {
                Wire wire;wire.id=static_cast<uint32_t>(integer(field(w,"id")));
                wire.kind=string(field(w,"kind"))=="audio"?WireKind::Audio:WireKind::Notes;
                wire.owner=string(field(w,"owner"))=="part"?WireOwner::Part:WireOwner::User;
                if(wire.owner==WireOwner::Part)wire.part=static_cast<uint32_t>(integer(field(w,"part")));
                wire.from=static_cast<NodeId>(integer(field(w,"from")));wire.to=static_cast<NodeId>(integer(field(w,"to")));
                wire.fromPort=static_cast<uint32_t>(integer(field(w,"fromPort")));wire.toPort=static_cast<uint32_t>(integer(field(w,"toPort")));
                for(auto pair:array(field(w,"channels"))){const auto ends=array(pair);if(ends.size()!=2)throw std::runtime_error("Invalid wire channel pair");wire.channels.push_back({static_cast<uint32_t>(integer(ends[0])),static_cast<uint32_t>(integer(ends[1]))});}
                wire.level=number(field(w,"level"));wire.pan=number(field(w,"pan"));
                wire.mute=boolean(field(w,"mute"));wire.solo=boolean(field(w,"solo"));wire.zones=zones(field(w,"zones"));wire.send=static_cast<uint32_t>(integer(field(w,"send")));
                wire.split=static_cast<uint32_t>(integer(field(w,"split")));wire.lane=static_cast<uint32_t>(integer(field(w,"lane")));wire.fader=boolean(field(w,"fader"));wire.connected=boolean(field(w,"connected"));
                next.wires.push_back(std::move(wire));
            }
            for(auto s:array(field(root,"scenes")))
            {
                Scene scene;scene.id=static_cast<uint32_t>(integer(field(s,"id")));scene.name=string(field(s,"name"));
                for(auto p:array(field(s,"parts")))scene.parts.push_back({static_cast<uint32_t>(integer(field(p,"part"))),number(field(p,"level")),boolean(field(p,"mute"))});
                for(auto v:array(field(s,"values")))scene.values.push_back({{static_cast<NodeId>(integer(field(v,"node"))),static_cast<uint32_t>(integer(field(v,"param")))},number(field(v,"value"))});
                for(auto m:array(field(s,"controls")))scene.macros.push_back({static_cast<uint32_t>(integer(field(m,"control"))),number(field(m,"value"))});
                next.scenes.push_back(std::move(scene));
            }
            for(auto at:array(field(root,"sceneParameters")))next.sceneParameters.push_back({static_cast<NodeId>(integer(field(at,"node"))),static_cast<uint32_t>(integer(field(at,"param")))});
            next.scene=static_cast<uint32_t>(integer(field(root,"scene")));
            for(auto m:array(field(root,"controls")))
            {
                Macro macro;macro.id=static_cast<uint32_t>(integer(field(m,"id")));macro.name=string(field(m,"name"));macro.value=number(field(m,"value"));
                for(auto t:array(field(m,"targets")))
                {
                    MacroTarget target;const auto kind=string(field(t,"kind")),curve=string(field(t,"curve"));
                    target.kind=kind=="parameter"?MacroTarget::Kind::Parameter:kind=="partLevel"?MacroTarget::Kind::PartLevel:kind=="partMute"?MacroTarget::Kind::PartMute:MacroTarget::Kind::Macro;
                    target.curve=curve=="exponential"?MacroTarget::Curve::Exponential:curve=="logarithmic"?MacroTarget::Curve::Logarithmic:curve=="s-curve"?MacroTarget::Curve::SCurve:MacroTarget::Curve::Linear;
                    target.node=static_cast<NodeId>(integer(field(t,"node")));target.param=static_cast<uint32_t>(integer(field(t,"param")));
                    target.part=static_cast<uint32_t>(integer(field(t,"part")));target.macro=static_cast<uint32_t>(integer(field(t,"control")));
                    target.start=number(field(t,"start"));target.end=number(field(t,"end"));target.relative=boolean(field(t,"relative"));target.base=number(field(t,"base"));
                    macro.targets.push_back(target);
                }
                next.macros.push_back(std::move(macro));
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
    uint32_t RemoteControl::prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& state,const std::string& preset,const std::string& sfz,const std::string& library,const std::string& directory)
    {
        std::string values;for(const auto& p:parameters){if(!values.empty())values+=',';values+="{\"id\":"+std::to_string(p.id)+",\"value\":"+encode(p.value)+'}';}
        auto doc=impl_->call("sounds.prepare","{\"node\":"+std::to_string(node)+",\"name\":"+quote(name)+",\"parameters\":["+values+"],\"state\":"+quote(state)+",\"preset\":"+quote(preset)+",\"sfz\":"+quote(sfz)+",\"library\":"+quote(library)+",\"directory\":"+quote(directory)+"}",30000);
        return static_cast<uint32_t>(integer(field(field(doc.get(),"result"),"context")));
    }
    // Registration hashes every file in a folder, so allow time for large libraries.
    Control::AssetFolder RemoteControl::registerAssetFolder(const std::string& folder)
    {auto doc=impl_->call("assets.register","{\"folder\":"+quote(folder)+"}",600000);auto r=field(doc.get(),"result");return {string(field(r,"library")),string(field(r,"directory"))};}
    kick::clap::AssetReference RemoteControl::importAsset(const std::string& file)
    {auto doc=impl_->call("assets.import","{\"file\":"+quote(file)+"}",60000);auto r=field(doc.get(),"result");return {string(field(r,"library")),string(field(r,"path")),string(field(r,"sha256"))};}
    std::vector<Control::AssetLibrary> RemoteControl::assetLibraries() const
    {
        auto doc=impl_->call("assets.libraries");std::vector<AssetLibrary> result;
        for(auto l:array(field(field(doc.get(),"result"),"libraries")))result.push_back({string(field(l,"id")),string(field(l,"name")),string(field(l,"path")),static_cast<size_t>(integer(field(l,"files")))});
        return result;
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
    PadMap RemoteControl::padMap() const
    {
        auto doc=impl_->call("pads.get");auto root=field(doc.get(),"result");PadMap map;
        map.bank=uint8_t(integer(field(root,"bank")));
        const auto banks=array(field(root,"banks"));if(banks.size()!=PadMap::banks)throw std::runtime_error("Invalid pad map");
        for(size_t b=0;b<PadMap::banks;++b){
            const auto pads=array(banks[b]);if(pads.size()!=PadMap::pads)throw std::runtime_error("Invalid pad bank");
            for(size_t i=0;i<PadMap::pads;++i){
                auto& a=map.assignments[b][i];a.note=uint8_t(integer(field(pads[i],"note")));a.channel=int8_t(number(field(pads[i],"channel")));
                a.velocity=number(field(pads[i],"velocity"));const auto p=string(field(pads[i],"pressure"));
                a.pressure=p=="poly"?PadAssignment::Pressure::Poly:p=="channel"?PadAssignment::Pressure::Channel:PadAssignment::Pressure::None;
            }
        }
        return map;
    }
    void RemoteControl::setPadMap(const PadMap& map)
    {
        std::string banks="[";
        for(size_t b=0;b<PadMap::banks;++b){
            banks+=b?",[":"[";
            for(size_t i=0;i<PadMap::pads;++i){const auto& a=map.assignments[b][i];
                banks+=std::string(i?",":"")+"{\"note\":"+std::to_string(a.note)+",\"channel\":"+std::to_string(a.channel)+",\"velocity\":"+encode(a.velocity)+",\"pressure\":\""+(a.pressure==PadAssignment::Pressure::Poly?"poly":a.pressure==PadAssignment::Pressure::Channel?"channel":"none")+"\"}";}
            banks+="]";
        }
        impl_->call("pads.set","{\"map\":{\"bank\":"+std::to_string(map.bank)+",\"banks\":"+banks+"]}}");
    }
    bool RemoteControl::pad(uint32_t pad,bool on,double velocity,int channel)
    {try{impl_->call("pads.play","{\"pad\":"+std::to_string(pad)+",\"on\":"+(on?"true":"false")+",\"velocity\":"+encode(velocity)+",\"channel\":"+std::to_string(channel)+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    bool RemoteControl::padPressure(uint32_t pad,double pressure)
    {try{impl_->call("pads.pressure","{\"pad\":"+std::to_string(pad)+",\"pressure\":"+encode(pressure)+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
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
    void RemoteControl::remove(NodeId n){remove(n,UserWires::Remove);}
    void RemoteControl::remove(NodeId n,UserWires w){impl_->call("graph.remove","{\"node\":"+std::to_string(n)+",\"userWires\":"+(w==UserWires::Reconnect?"\"reconnect\"":"\"remove\"")+'}');impl_->refresh();}
    namespace
    {
        std::string zonesText(const std::vector<NoteZone>& zones)
        {
            std::string out="[";
            for(const auto& z:zones){if(out.size()>1)out+=',';out+="{\"keys\":["+std::to_string(z.lowKey)+","+std::to_string(z.highKey)+"],\"velocities\":["+encode(z.lowVelocity)+","+encode(z.highVelocity)+"],\"channels\":"+std::to_string(z.channels)+",\"transpose\":"+std::to_string(z.transpose)+",\"curve\":"+encode(z.curve)+",\"fixedVelocity\":"+encode(z.fixedVelocity)+'}';}
            return out+']';
        }
        std::string inputText(const PartInput& input)
        {
            const char* kind=input.kind==PartInput::Kind::Notes?"notes":input.kind==PartInput::Kind::Audio?"audio":input.kind==PartInput::Kind::Bus?"bus":"none";
            std::string devices="[";
            for(const auto& r:input.devices){if(devices.size()>1)devices+=',';devices+=std::string("{\"kind\":\"")+deviceRuleKinds[size_t(r.kind)]+"\",\"port\":"+std::to_string(r.port)+",\"pattern\":"+quote(r.pattern)+",\"match\":\""+deviceMatches[size_t(r.match)]+"\"}";}
            return "{\"kind\":\""+std::string(kind)+"\",\"port\":"+std::to_string(input.port)+",\"zones\":"+zonesText(input.zones)+",\"devices\":"+devices+"]}";
        }
    }
    uint32_t RemoteControl::addPart(const std::string& name,PartInput input,std::optional<uint32_t> output)
    {
        auto reply=impl_->call("parts.add","{\"name\":"+quote(name)+",\"input\":"+inputText(input)+(output?",\"output\":"+std::to_string(*output):std::string{})+'}');
        const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"part")));impl_->refresh();return id;
    }
    void RemoteControl::setPartInput(uint32_t part,const PartInput& input){impl_->call("parts.input","{\"part\":"+std::to_string(part)+",\"input\":"+inputText(input)+'}');impl_->refresh();}
    void RemoteControl::setPartOutput(uint32_t part,uint32_t bus,std::optional<uint32_t> port)
    {impl_->call("parts.output","{\"part\":"+std::to_string(part)+(bus?",\"bus\":"+std::to_string(bus):std::string{})+(port?",\"port\":"+std::to_string(*port):std::string{})+'}');impl_->refresh();}
    void RemoteControl::setSend(uint32_t part,uint32_t bus,std::optional<double> level)
    {impl_->call("parts.send","{\"part\":"+std::to_string(part)+",\"bus\":"+std::to_string(bus)+(level?",\"level\":"+encode(*level):std::string{})+'}');impl_->refresh();}
    void RemoteControl::setPartLevel(uint32_t part,double level,bool mute)
    {impl_->call("parts.level","{\"part\":"+std::to_string(part)+",\"level\":"+encode(level)+",\"mute\":"+(mute?"true":"false")+'}');impl_->refresh();}
    void RemoteControl::setPartName(uint32_t part,const std::string& name)
    {impl_->call("parts.name","{\"part\":"+std::to_string(part)+",\"name\":"+quote(name)+'}');impl_->refresh();}
    uint32_t RemoteControl::addScene(const std::string& name)
    {auto reply=impl_->call("scenes.add","{\"name\":"+quote(name)+'}');const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"scene")));impl_->refresh();return id;}
    void RemoteControl::storeScene(uint32_t scene){impl_->call("scenes.store","{\"scene\":"+std::to_string(scene)+'}');impl_->refresh();}
    void RemoteControl::selectScene(uint32_t scene){impl_->call("scenes.select","{\"scene\":"+std::to_string(scene)+'}');impl_->refresh();}
    void RemoteControl::removeScene(uint32_t scene){impl_->call("scenes.remove","{\"scene\":"+std::to_string(scene)+'}');impl_->refresh();}
    void RemoteControl::setSceneName(uint32_t scene,const std::string& name){impl_->call("scenes.name","{\"scene\":"+std::to_string(scene)+",\"name\":"+quote(name)+'}');impl_->refresh();}
    void RemoteControl::setSceneParameters(const std::vector<SceneParameter>& parameters)
    {
        std::string list="[";
        for(const auto& at:parameters){if(list.size()>1)list+=',';list+="{\"node\":"+std::to_string(at.node)+",\"param\":"+std::to_string(at.param)+'}';}
        impl_->call("scenes.parameters","{\"parameters\":"+list+"]}");impl_->refresh();
    }
    uint32_t RemoteControl::addMacro(const std::string& name)
    {auto reply=impl_->call("controls.add","{\"name\":"+quote(name)+'}');const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"control")));impl_->refresh();return id;}
    void RemoteControl::removeMacro(uint32_t macro){impl_->call("controls.remove","{\"control\":"+std::to_string(macro)+'}');impl_->refresh();}
    void RemoteControl::setMacroName(uint32_t macro,const std::string& name){impl_->call("controls.name","{\"control\":"+std::to_string(macro)+",\"name\":"+quote(name)+'}');impl_->refresh();}
    void RemoteControl::setMacroTargets(uint32_t macro,const std::vector<MacroTarget>& targets)
    {
        std::string list="[";
        for(const auto& t:targets){
            if(list.size()>1)list+=',';
            const char* kind=t.kind==MacroTarget::Kind::Parameter?"parameter":t.kind==MacroTarget::Kind::PartLevel?"partLevel":t.kind==MacroTarget::Kind::PartMute?"partMute":"control";
            const char* curve=t.curve==MacroTarget::Curve::Linear?"linear":t.curve==MacroTarget::Curve::Exponential?"exponential":t.curve==MacroTarget::Curve::Logarithmic?"logarithmic":"s-curve";
            list+=std::string("{\"kind\":\"")+kind+"\",\"node\":"+std::to_string(t.node)+",\"param\":"+std::to_string(t.param)+",\"part\":"+std::to_string(t.part)+",\"control\":"+std::to_string(t.macro)+
                ",\"start\":"+encode(t.start)+",\"end\":"+encode(t.end)+",\"curve\":\""+curve+"\",\"relative\":"+(t.relative?"true":"false")+'}';
        }
        impl_->call("controls.targets","{\"control\":"+std::to_string(macro)+",\"targets\":"+list+"]}");impl_->refresh();
    }
    void RemoteControl::setMacro(uint32_t macro,double value){impl_->call("controls.set","{\"control\":"+std::to_string(macro)+",\"value\":"+encode(value)+'}');impl_->refresh();}
    void RemoteControl::setZones(uint32_t wire,const std::vector<NoteZone>& zones){impl_->call("wires.zones","{\"wire\":"+std::to_string(wire)+",\"zones\":"+zonesText(zones)+'}');impl_->refresh();}
    void RemoteControl::removePart(uint32_t part){impl_->call("parts.remove","{\"part\":"+std::to_string(part)+'}');impl_->refresh();}
    namespace { std::string placeText(ChainPlace p){return p.split?",\"split\":"+std::to_string(p.split)+",\"lane\":"+std::to_string(p.lane):std::string{};} }
    void RemoteControl::insertIntoPart(uint32_t part,size_t index,NodeId node,ChainPlace place){impl_->call("parts.insert","{\"part\":"+std::to_string(part)+",\"index\":"+std::to_string(index)+",\"node\":"+std::to_string(node)+placeText(place)+'}');impl_->refresh();}
    void RemoteControl::removeFromPart(uint32_t part,NodeId node){impl_->call("parts.removeNode","{\"part\":"+std::to_string(part)+",\"node\":"+std::to_string(node)+'}');impl_->refresh();}
    void RemoteControl::movePartNode(uint32_t part,size_t from,size_t to,ChainPlace place){impl_->call("parts.move","{\"part\":"+std::to_string(part)+",\"from\":"+std::to_string(from)+",\"to\":"+std::to_string(to)+placeText(place)+'}');impl_->refresh();}
    uint32_t RemoteControl::addSplit(uint32_t part,size_t index,ChainPlace place,Split::Kind kind,NodeId splitter,NodeId joiner)
    {
        auto reply=impl_->call("parts.addSplit","{\"part\":"+std::to_string(part)+",\"index\":"+std::to_string(index)+placeText(place)+",\"kind\":\""+splitKinds[size_t(kind)]+'"'+
            (splitter?",\"splitter\":"+std::to_string(splitter):std::string{})+(joiner?",\"joiner\":"+std::to_string(joiner):std::string{})+'}');
        const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"split")));impl_->refresh();return id;
    }
    void RemoteControl::removeSplit(uint32_t part,uint32_t split){impl_->call("parts.removeSplit","{\"part\":"+std::to_string(part)+",\"split\":"+std::to_string(split)+'}');impl_->refresh();}
    uint32_t RemoteControl::addLane(uint32_t part,uint32_t split)
    {
        auto reply=impl_->call("parts.addLane","{\"part\":"+std::to_string(part)+",\"split\":"+std::to_string(split)+'}');
        const auto lane=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"lane")));impl_->refresh();return lane;
    }
    Control::History RemoteControl::history() const
    {
        auto reply=impl_->call("edit.history");auto r=field(reply.get(),"result");
        return {string(field(r,"undo")),string(field(r,"redo"))};
    }
    bool RemoteControl::undo(){const bool any=!history().undo.empty();impl_->call("edit.undo");impl_->refresh();return any;}
    bool RemoteControl::redo(){const bool any=!history().redo.empty();impl_->call("edit.redo");impl_->refresh();return any;}
    void RemoteControl::setPartSolo(uint32_t part,bool solo){impl_->call("parts.solo","{\"part\":"+std::to_string(part)+",\"solo\":"+(solo?"true":"false")+'}');impl_->refresh();}
    std::vector<PartMeter> RemoteControl::partMeters() const
    {
        auto reply=impl_->call("parts.meters");std::vector<PartMeter> out;
        for(auto m:array(field(field(reply.get(),"result"),"meters")))out.push_back({static_cast<uint32_t>(integer(field(m,"part"))),number(field(m,"peak"))});
        return out;
    }
    void RemoteControl::removeLane(uint32_t part,uint32_t split,uint32_t lane){impl_->call("parts.removeLane","{\"part\":"+std::to_string(part)+",\"split\":"+std::to_string(split)+",\"lane\":"+std::to_string(lane)+'}');impl_->refresh();}
    void RemoteControl::setLane(uint32_t part,uint32_t split,uint32_t lane,const Lane& l)
    {
        impl_->call("parts.lane","{\"part\":"+std::to_string(part)+",\"split\":"+std::to_string(split)+",\"lane\":"+std::to_string(lane)+",\"zones\":"+zonesText(l.zones)+
            ",\"level\":"+encode(l.level)+",\"pan\":"+encode(l.pan)+",\"mute\":"+(l.mute?"true":"false")+",\"solo\":"+(l.solo?"true":"false")+'}');
        impl_->refresh();
    }
    uint32_t RemoteControl::connectPorts(const Wire& w)
    {
        std::string params="{\"kind\":"+std::string(w.kind==WireKind::Audio?"\"audio\"":"\"notes\"")+",\"from\":"+std::to_string(w.from)+",\"to\":"+std::to_string(w.to);
        if(w.kind==WireKind::Audio)
        {
            params+=",\"fromPort\":"+std::to_string(w.fromPort)+",\"toPort\":"+std::to_string(w.toPort);
            if(!w.channels.empty()){params+=",\"channels\":[";for(size_t i=0;i<w.channels.size();++i)params+=(i?",[":"[")+std::to_string(w.channels[i].first)+","+std::to_string(w.channels[i].second)+"]";params+="]";}
        }
        if(!w.zones.empty())params+=",\"zones\":"+zonesText(w.zones);
        params+=",\"level\":"+encode(w.level)+",\"pan\":"+encode(w.pan)+'}';
        auto reply=impl_->call("wires.connect",params);
        const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"wire")));impl_->refresh();return id;
    }
    void RemoteControl::disconnectWire(uint32_t wire){impl_->call("wires.disconnect","{\"wire\":"+std::to_string(wire)+'}');impl_->refresh();}
    void RemoteControl::setMix(uint32_t wire,double level,double pan,bool mute,bool solo)
    {impl_->call("wires.mix","{\"wire\":"+std::to_string(wire)+",\"level\":"+encode(level)+",\"pan\":"+encode(pan)+",\"mute\":"+(mute?"true":"false")+",\"solo\":"+(solo?"true":"false")+'}');impl_->refresh();}
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

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
            transport={boolean(field(v,"playing")),number(field(v,"tempo")),number(field(v,"beat")),{}};
            for(auto r:array(field(v,"clockOutputs")))transport.clockOutputs.push_back({DeviceRule::Kind(named(deviceRuleKinds,string(field(r,"kind")))),
                DeviceRule::Match(named(deviceMatches,string(field(r,"match")))),static_cast<uint32_t>(integer(field(r,"port"))),string(field(r,"pattern"))});
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
            status.deadlineMisses=integer(field(v,"deadlineMisses"));status.midiInputs.clear();status.midiOutputs.clear();status.failures.clear();status.workers.clear();
            for(auto f:array(field(v,"failures")))status.failures.push_back({static_cast<NodeId>(integer(field(f,"node"))),string(field(f,"name")),string(field(f,"reason"))});
            for(auto w:array(field(v,"workers")))status.workers.push_back({static_cast<NodeId>(integer(field(w,"node"))),string(field(w,"name")),number(field(w,"cpu")),uint64_t(integer(field(w,"memory")))});
            for(auto [list,key]:{std::pair{&status.midiInputs,"midiInputs"},{&status.midiOutputs,"midiOutputs"}})
                for(auto e:array(field(v,key)))
                    list->push_back({MidiEndpoint::Kind(named(endpointKinds,string(field(e,"kind")))),uint8_t(integer(field(e,"source"))),string(field(e,"name")),static_cast<uint32_t>(integer(field(e,"port")))});
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
                if(!boolean(field(n,"enabled")))next.disabled.insert(node.id);
                if(boolean(field(n,"ringing")))next.ringing.insert(node.id);
                next.topology.nodes.push_back(std::move(node));
            }
            for(auto c:array(field(root,"connections")))
            {
                const auto from=static_cast<NodeId>(integer(field(c,"from"))),to=static_cast<NodeId>(integer(field(c,"to")));
                if(string(field(c,"kind"))=="notes"){
                    next.topology.notes.emplace_back(from,to);
                    if(const auto d=detail::member(c,"draining");d && d->type==json_type_true)next.topology.draining.insert({from,to});}
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
                part.input.port=static_cast<uint32_t>(integer(field(input,"port")));part.input.zones=zones(field(input,"zones"));part.input.maxNotes=uint16_t(integer(field(input,"maxNotes")));
                for(auto r:array(field(input,"devices")))part.input.devices.push_back({DeviceRule::Kind(named(deviceRuleKinds,string(field(r,"kind")))),
                    DeviceRule::Match(named(deviceMatches,string(field(r,"match")))),static_cast<uint32_t>(integer(field(r,"port"))),string(field(r,"pattern"))});
                part.output=static_cast<uint32_t>(integer(field(p,"output")));
                const auto midiOut=field(p,"midiOut");part.midiOut.channel=uint8_t(integer(field(midiOut,"channel")));
                for(auto r:array(field(midiOut,"devices")))part.midiOut.devices.push_back({DeviceRule::Kind(named(deviceRuleKinds,string(field(r,"kind")))),
                    DeviceRule::Match(named(deviceMatches,string(field(r,"match")))),static_cast<uint32_t>(integer(field(r,"port"))),string(field(r,"pattern"))});
                auto chain=[](V list){std::vector<NodeId> result;for(auto n:array(list))result.push_back(n->type==json_type_object?splitMark|static_cast<NodeId>(integer(field(n,"split"))):static_cast<NodeId>(integer(n)));return result;};
                part.chain=chain(field(p,"chain"));
                for(auto s:array(field(p,"splits"))){
                    Split split;split.id=static_cast<uint32_t>(integer(field(s,"id")));split.kind=Split::Kind(named(splitKinds,string(field(s,"kind"))));
                    split.splitter=static_cast<NodeId>(integer(field(s,"splitter")));split.joiner=static_cast<NodeId>(integer(field(s,"joiner")));
                    for(auto l:array(field(s,"lanes")))split.lanes.push_back({chain(field(l,"chain")),zones(field(l,"zones")),number(field(l,"level")),number(field(l,"pan")),boolean(field(l,"mute")),boolean(field(l,"solo"))});
                    part.splits.push_back(std::move(split));
                }
                part.bus=static_cast<uint32_t>(integer(field(p,"bus")));
                part.level=number(field(p,"level"));part.mute=boolean(field(p,"mute"));part.solo=boolean(field(p,"solo"));part.pan=number(field(p,"pan"));
                for(auto s:array(field(p,"sends")))part.sends.push_back({static_cast<uint32_t>(integer(field(s,"bus"))),number(field(s,"level"))});
                if(const auto from=field(p,"from");from->type!=json_type_null)part.from={string(field(from,"sound")),string(field(from,"revision")),string(field(from,"name")),boolean(field(from,"modified"))};
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
                wire.split=static_cast<uint32_t>(integer(field(w,"split")));wire.lane=static_cast<uint32_t>(integer(field(w,"lane")));wire.fader=boolean(field(w,"fader"));wire.connected=boolean(field(w,"connected"));wire.enabled=boolean(field(w,"enabled"));
                next.wires.push_back(std::move(wire));
            }
            auto ref=[&](V r){const auto kind=string(field(r,"kind"));
                return WireRef{kind=="audio"?WireKind::Audio:WireKind::Notes,static_cast<uint32_t>(integer(field(r,"part"))),static_cast<NodeId>(integer(field(r,"from"))),static_cast<NodeId>(integer(field(r,"to"))),
                    static_cast<uint32_t>(integer(field(r,"fromPort"))),static_cast<uint32_t>(integer(field(r,"toPort"))),static_cast<uint32_t>(integer(field(r,"send"))),
                    static_cast<uint32_t>(integer(field(r,"split"))),static_cast<uint32_t>(integer(field(r,"lane"))),boolean(field(r,"fader"))};};
            for(auto s:array(field(root,"scenes")))
            {
                Scene scene;scene.id=static_cast<uint32_t>(integer(field(s,"id")));scene.name=string(field(s,"name"));scene.scope=0;
                scene.glide=static_cast<uint32_t>(integer(field(s,"glide")));
                auto index=[](const auto& names,const std::string& name){const auto at=std::find(names.begin(),names.end(),name);return at==names.end()?0:int(at-names.begin());};
                scene.curve=Scene::Curve(index(sceneCurves,std::string(string(field(s,"curve")))));
                scene.launch=Scene::Launch(index(sceneLaunches,std::string(string(field(s,"launch")))));
                for(auto k:array(field(s,"scope"))){const auto name=std::string(string(k));const auto at=std::find(sceneKinds.begin(),sceneKinds.end(),name);if(at!=sceneKinds.end())scene.scope|=1u<<(at-sceneKinds.begin());}
                for(auto n:array(field(s,"off")))scene.off.push_back(static_cast<NodeId>(integer(n)));
                for(auto r:array(field(s,"offWires")))scene.offWires.push_back(ref(r));
                for(auto p:array(field(s,"parts")))
                {
                    Scene::PartState st{static_cast<uint32_t>(integer(field(p,"part"))),number(field(p,"level")),number(field(p,"pan")),boolean(field(p,"mute")),boolean(field(p,"solo")),{},{}};
                    for(auto x:array(field(p,"sends")))st.sends.push_back({static_cast<uint32_t>(integer(field(x,"bus"))),number(field(x,"level"))});
                    for(auto l:array(field(p,"lanes")))st.lanes.push_back({static_cast<uint32_t>(integer(field(l,"split"))),static_cast<uint32_t>(integer(field(l,"lane"))),number(field(l,"level")),number(field(l,"pan")),boolean(field(l,"mute")),boolean(field(l,"solo"))});
                    scene.parts.push_back(std::move(st));
                }
                for(auto w:array(field(s,"wires")))scene.wires.push_back({ref(w),number(field(w,"level")),number(field(w,"pan")),boolean(field(w,"mute")),boolean(field(w,"solo"))});
                for(auto v:array(field(s,"values")))scene.values.push_back({{static_cast<NodeId>(integer(field(v,"node"))),static_cast<uint32_t>(integer(field(v,"param")))},number(field(v,"value"))});
                for(auto m:array(field(s,"controls")))scene.macros.push_back({static_cast<uint32_t>(integer(field(m,"control"))),number(field(m,"value"))});
                next.scenes.push_back(std::move(scene));
            }
            for(auto at:array(field(root,"sceneUntouched")))next.untouched.push_back({static_cast<NodeId>(integer(field(at,"node"))),static_cast<uint32_t>(integer(field(at,"param")))});
            next.scene=static_cast<uint32_t>(integer(field(root,"scene")));
            for(auto m:array(field(root,"controls")))
            {
                Macro macro;macro.id=static_cast<uint32_t>(integer(field(m,"id")));macro.part=static_cast<uint32_t>(integer(field(m,"part")));macro.name=string(field(m,"name"));macro.value=number(field(m,"value"));
                for(auto t:array(field(m,"targets")))
                {
                    MacroTarget target;const auto kind=string(field(t,"kind")),curve=string(field(t,"curve"));
                    target.kind=kind=="parameter"?MacroTarget::Kind::Parameter:kind=="partLevel"?MacroTarget::Kind::PartLevel:kind=="partMute"?MacroTarget::Kind::PartMute:kind=="morph"?MacroTarget::Kind::Morph:MacroTarget::Kind::Macro;
                    target.curve=curve=="exponential"?MacroTarget::Curve::Exponential:curve=="logarithmic"?MacroTarget::Curve::Logarithmic:curve=="s-curve"?MacroTarget::Curve::SCurve:MacroTarget::Curve::Linear;
                    target.node=static_cast<NodeId>(integer(field(t,"node")));target.param=static_cast<uint32_t>(integer(field(t,"param")));
                    target.part=static_cast<uint32_t>(integer(field(t,"part")));target.macro=static_cast<uint32_t>(integer(field(t,"control")));
                    target.start=number(field(t,"start"));target.end=number(field(t,"end"));target.relative=boolean(field(t,"relative"));target.base=number(field(t,"base"));
                    for(auto id:array(field(t,"offIn")))target.offIn.push_back(static_cast<uint32_t>(integer(id)));
                    target.sceneFrom=static_cast<uint32_t>(integer(field(t,"from")));target.sceneTo=static_cast<uint32_t>(integer(field(t,"to")));
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
                    ParamInfo param{static_cast<uint32_t>(integer(field(p,"id"))),string(field(p,"name")),number(field(p,"minimum")),number(field(p,"maximum")),number(field(p,"defaultValue")),boolean(field(p,"stepped")),static_cast<uint32_t>(integer(field(p,"flags"))),string(field(p,"module")),
                                    widgetNamed(std::string(string(field(p,"widget")))),static_cast<uint32_t>(integer(field(p,"priority")))};
                    if(param.widget==OSCAIRN_WIDGET_RANGE)param.rangeHigh=static_cast<uint32_t>(integer(field(p,"rangeHigh")));
                    param.group=static_cast<uint32_t>(integer(field(p,"group")));
                    if(V role=detail::member(p,"role"))param.role=roleNamed(string(role));
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
    void RemoteControl::setBudget(const Budget& b) {
        impl_->call("budget.set","{\"cpu\":"+encode(b.cpu)+",\"memory\":"+std::to_string(b.memory)+",\"device\":"+std::to_string(b.device)+",\"system\":"+std::to_string(b.system)+
                    ",\"floor\":"+std::to_string(b.floor)+",\"margin\":"+std::to_string(b.margin)+'}');
    }
    void RemoteControl::setAlwaysAvailable(const std::vector<AppMemory>& apps) {
        std::string list;for(const auto& a:apps)list+=(list.empty()?"":",")+std::string("{\"app\":")+quote(a.app)+",\"reserve\":"+std::to_string(a.reserve)+'}';
        impl_->call("memory.alwaysAvailable","{\"apps\":["+list+"]}");
    }
    Control::Launch RemoteControl::launchApp(const AppMemory& app, bool dropNext) {
        auto d=impl_->call("memory.launch","{\"app\":"+quote(app.app)+",\"reserve\":"+std::to_string(app.reserve)+",\"dropNext\":"+(dropNext?"true":"false")+'}');auto r=field(d.get(),"result");
        return {boolean(field(r,"admitted")),boolean(field(r,"offerNext")),string(field(r,"reason"))};
    }
    void RemoteControl::appClosed(const std::string& app) { impl_->call("memory.closed","{\"app\":"+quote(app)+'}'); }
    Control::MemoryState RemoteControl::memoryState() {
        auto d=impl_->call("memory.state","{}");auto r=field(d.get(),"result");MemoryState m;
        for(auto [key,to]:{std::pair{"device",&m.device},{"reserved",&m.reserved},{"share",&m.share},{"floor",&m.floor},{"margin",&m.margin},{"committed",&m.committed},{"opportunistic",&m.opportunistic}})
            *to=uint64_t(integer(field(r,key)));
        auto apps=[](V v){std::vector<AppMemory> out;for(auto a:array(v))out.push_back({string(field(a,"app")),uint64_t(integer(field(a,"reserve")))});return out;};
        m.running=apps(field(r,"running"));m.alwaysAvailable=apps(field(r,"alwaysAvailable"));
        m.nextPrepared=boolean(field(r,"nextPrepared"));m.nextCommitted=boolean(field(r,"nextCommitted"));
        return m;
    }
    Control::Estimate RemoteControl::estimate(const std::string& directory) {
        auto d=impl_->call("budget.estimate",directory.empty()?std::string("{}"):"{\"directory\":"+quote(directory)+'}',30000);auto r=field(d.get(),"result");
        Estimate e;e.cpu=number(field(r,"cpu"));e.memory=uint64_t(integer(field(r,"memory")));e.fits=boolean(field(r,"fits"));e.seamless=boolean(field(r,"seamless"));
        e.budget.cpu=number(field(field(r,"budget"),"cpu"));e.budget.memory=uint64_t(integer(field(field(r,"budget"),"memory")));
        for(auto x:array(field(r,"plugins")))e.plugins.push_back({string(field(x,"name")),string(field(x,"plugin")),number(field(x,"cpu")),uint64_t(integer(field(x,"memory"))),boolean(field(x,"measured"))});
        return e;
    }
    void RemoteControl::loadPerformance(const std::string& directory) {
        impl_->call("performance.load","{\"directory\":"+quote(directory)+'}',30000);impl_->refresh();
    }
    bool RemoteControl::preparePerformance(const std::string& directory) {
        auto d=impl_->call("performance.prepare","{\"directory\":"+quote(directory)+'}',30000);return boolean(field(field(d.get(),"result"),"prepared"));
    }
    namespace {
        std::vector<std::string> strings(V v){std::vector<std::string> out;for(auto e:array(v))out.push_back(string(e));return out;}
        std::string quoted(const std::vector<std::string>& values){std::string out="[";for(const auto& v:values){if(out.size()>1)out+=',';out+=quote(v);}return out+']';}
        // An export's sign member, when it signs anything.
        Control::Signer signer(V s){return {string(field(s,"kind")),string(field(s,"name")),string(field(s,"version")),string(field(s,"platform")),string(field(s,"status")),string(field(s,"key")),string(field(s,"keyName"))};}
        std::vector<Control::Signer> signers(V list){std::vector<Control::Signer> out;for(auto s:array(list))out.push_back(signer(s));return out;}
        std::string signing(const Control::ExportSigning& s){return s.items || s.archive?",\"sign\":{\"key\":"+quote(s.key)+",\"items\":"+(s.items?"true":"false")+",\"archive\":"+(s.archive?"true":"false")+'}':std::string();}
    }
    std::vector<std::string> RemoteControl::exportPerformance(const std::string& file,bool editedSounds,const ExportSigning& sign) {
        auto d=impl_->call("performance.export","{\"file\":"+quote(file)+",\"editedSounds\":"+(editedSounds?"true":"false")+signing(sign)+'}',120000);
        return strings(field(field(d.get(),"result"),"notes"));
    }
    Control::PerformanceInfo RemoteControl::performanceInfo() const {
        auto d=impl_->call("performance.info");auto r=field(d.get(),"result");
        return {string(field(r,"id")),string(field(r,"name")),string(field(r,"description")),strings(field(r,"tags")),string(field(r,"color"))};
    }
    void RemoteControl::setPerformanceInfo(const PerformanceInfo& info) {
        impl_->call("performance.setInfo","{\"name\":"+quote(info.name)+",\"description\":"+quote(info.description)+",\"tags\":"+quoted(info.tags)+",\"color\":"+quote(info.color)+'}');
    }
    Control::LibraryResult RemoteControl::libraryQuery(const LibraryQuery& q) {
        auto d=impl_->call("library.query","{\"text\":"+quote(q.text)+",\"tags\":"+quoted(q.tags)+",\"favorites\":"+(q.favorites?"true":"false")+",\"recent\":"+(q.recent?"true":"false")+",\"limit\":"+std::to_string(q.limit)+(q.kind.empty()?std::string():",\"kind\":"+quote(q.kind))+(q.source.empty()?std::string():",\"source\":"+quote(q.source))+(q.id.empty()?std::string():",\"id\":"+quote(q.id))+'}');
        auto r=field(d.get(),"result");LibraryResult result;
        for(auto i:array(field(r,"items")))result.items.push_back({string(field(i,"id")),string(field(i,"kind")),string(field(i,"name")),string(field(i,"description")),string(field(i,"path")),strings(field(i,"tags")),boolean(field(i,"favorite")),static_cast<uint32_t>(integer(field(i,"recent"))),string(field(i,"color")),string(field(i,"revision")),string(field(i,"hash")),string(field(i,"library")),{}});
        for(auto t:array(field(r,"tags")))result.tags.emplace_back(string(field(t,"tag")),static_cast<uint32_t>(integer(field(t,"count"))));
        for(auto t:array(field(r,"tagColors")))result.tagColors.emplace_back(string(field(t,"tag")),string(field(t,"color")));
        result.hiddenTags=strings(field(r,"hiddenTags"));
        result.total=static_cast<uint32_t>(integer(field(r,"total")));
        return result;
    }
    void RemoteControl::libraryTag(const std::string& tag,bool hidden,const std::string& color) {
        impl_->call("library.tag","{\"tag\":"+quote(tag)+",\"hidden\":"+(hidden?"true":"false")+",\"color\":"+quote(color)+'}');
    }
    void RemoteControl::libraryFavorite(const std::string& id,bool favorite) {
        impl_->call("library.favorite","{\"id\":"+quote(id)+",\"favorite\":"+(favorite?"true":"false")+'}');
    }
    // A rescan, save or load reads or writes every Performance file, so allow time.
    Control::Archive RemoteControl::archiveContents(const std::string& file) {
        auto d=impl_->call("library.contents","{\"file\":"+quote(file)+'}',30000);auto r=field(d.get(),"result");
        Archive a{string(field(r,"id")),string(field(r,"name")),string(field(r,"kind")),{},signers(field(r,"code")),signer(field(r,"signer"))};
        for(auto i:array(field(r,"items"))){LibraryItem item;item.kind=string(field(i,"kind"));item.id=string(field(i,"id"));item.name=string(field(i,"name"));a.items.push_back(item);}
        return a;
    }
    Control::Installed RemoteControl::installArchive(const std::string& file,bool asLibrary,const std::vector<std::string>& items,bool acceptUnsignedCode) {
        auto d=impl_->call("library.install","{\"file\":"+quote(file)+",\"asLibrary\":"+(asLibrary?"true":"false")+",\"items\":"+quoted(items)+",\"acceptUnsignedCode\":"+(acceptUnsignedCode?"true":"false")+'}',120000);auto r=field(d.get(),"result");
        return {static_cast<uint32_t>(integer(field(r,"added"))),static_cast<uint32_t>(integer(field(r,"skipped"))),strings(field(r,"notes")),signers(field(r,"code"))};
    }
    std::vector<Control::InstalledLibrary> RemoteControl::libraries() {
        auto d=impl_->call("library.libraries","{}");std::vector<InstalledLibrary> out;
        for(auto l:array(field(field(d.get(),"result"),"libraries")))out.push_back({string(field(l,"id")),string(field(l,"name")),static_cast<uint32_t>(integer(field(l,"items")))});
        return out;
    }
    std::vector<Control::InstalledPlugin> RemoteControl::installedPlugins() {
        auto d=impl_->call("plugins.list","{}",30000);std::vector<InstalledPlugin> out;
        for(auto x:array(field(field(d.get(),"result"),"plugins")))
            out.push_back({string(field(x,"format")),string(field(x,"id")),string(field(x,"version")),string(field(x,"name")),strings(field(x,"platforms")),uint64_t(integer(field(x,"bytes"))),strings(field(x,"usedBy")),boolean(field(x,"open"))});
        return out;
    }
    void RemoteControl::removePlugin(const std::string& format,const std::string& id,const std::string& version,const std::string& platform) {
        impl_->call("plugins.remove","{\"format\":"+quote(format)+",\"id\":"+quote(id)+",\"version\":"+quote(version)+",\"platform\":"+quote(platform)+'}',30000);
    }
    void RemoteControl::addPluginBuild(const std::string& format,const std::string& id,const std::string& version,const std::string& platform,const std::string& path) {
        impl_->call("plugins.addBuild","{\"format\":"+quote(format)+",\"id\":"+quote(id)+",\"version\":"+quote(version)+",\"platform\":"+quote(platform)+",\"path\":"+quote(path)+'}',120000);
    }
    std::vector<Control::TrustedKey> RemoteControl::trustedKeys() {
        auto d=impl_->call("trust.list","{}");std::vector<TrustedKey> out;
        for(auto k:array(field(field(d.get(),"result"),"keys")))out.push_back({string(field(k,"id")),string(field(k,"key")),string(field(k,"name")),strings(field(k,"scope")),boolean(field(k,"revoked"))});
        return out;
    }
    void RemoteControl::setTrustedKey(const TrustedKey& k) {
        impl_->call("trust.set","{\"key\":"+quote(k.key)+",\"name\":"+quote(k.name)+",\"scope\":"+quoted(k.scope)+",\"revoked\":"+(k.revoked?"true":"false")+'}');
    }
    void RemoteControl::removeTrustedKey(const std::string& id) { impl_->call("trust.remove","{\"id\":"+quote(id)+'}'); }
    void RemoteControl::uninstallLibrary(const std::string& id) { impl_->call("library.uninstall","{\"library\":"+quote(id)+'}',30000); }
    void RemoteControl::libraryEdit(const std::string& id,const PerformanceInfo& info) {
        impl_->call("library.edit","{\"id\":"+quote(id)+",\"name\":"+quote(info.name)+",\"description\":"+quote(info.description)+",\"tags\":"+quoted(info.tags)+",\"color\":"+quote(info.color)+'}',30000);
    }
    void RemoteControl::auditionStart(const std::string& id,uint32_t part) {
        impl_->call("audition.start","{\"id\":"+quote(id)+",\"part\":"+std::to_string(part)+'}',120000);
    }
    void RemoteControl::auditionStop(bool keep) { impl_->call("audition.stop",std::string("{\"keep\":")+(keep?"true":"false")+'}',30000); }
    Control::Audition RemoteControl::audition() const {
        auto d=impl_->call("audition.get");auto r=field(d.get(),"result");
        return {string(field(r,"id")),string(field(r,"kind")),static_cast<uint32_t>(integer(field(r,"part")))};
    }
    std::vector<std::string> RemoteControl::exportLibraryItem(const std::string& id,const std::string& file,bool editedSounds,const ExportSigning& sign) {
        auto d=impl_->call("library.export","{\"id\":"+quote(id)+",\"file\":"+quote(file)+",\"editedSounds\":"+(editedSounds?"true":"false")+signing(sign)+'}',120000);
        return strings(field(field(d.get(),"result"),"notes"));
    }
    std::vector<Control::SigningKey> RemoteControl::signingKeys() {
        auto d=impl_->call("keys.list","{}");std::vector<SigningKey> out;
        for(auto k:array(field(field(d.get(),"result"),"keys")))out.push_back({string(field(k,"name")),string(field(k,"id")),string(field(k,"key"))});
        return out;
    }
    Control::SigningKey RemoteControl::makeSigningKey(const std::string& name) {
        auto d=impl_->call("keys.make","{\"name\":"+quote(name)+'}');auto k=field(d.get(),"result");
        return {string(field(k,"name")),string(field(k,"id")),string(field(k,"key"))};
    }
    std::string RemoteControl::libraryCopy(const std::string& id) {
        auto d=impl_->call("library.copy","{\"id\":"+quote(id)+'}',30000);return string(field(field(d.get(),"result"),"id"));
    }
    void RemoteControl::deleteLibraryItem(const std::string& id) { impl_->call("library.delete","{\"id\":"+quote(id)+'}',30000); }
    uint32_t RemoteControl::libraryRescan() {
        auto d=impl_->call("library.rescan","{}",30000);return static_cast<uint32_t>(integer(field(field(d.get(),"result"),"items")));
    }
    std::string RemoteControl::savePerformanceToLibrary(const std::string& name,bool asNew) {
        auto d=impl_->call("library.save","{\"name\":"+quote(name)+",\"new\":"+(asNew?"true":"false")+'}',30000);return string(field(field(d.get(),"result"),"id"));
    }
    Control::SoundSaved RemoteControl::savePartAsSound(uint32_t part,const std::string& name,bool asNew) {
        auto d=impl_->call("parts.saveSound","{\"part\":"+std::to_string(part)+",\"name\":"+quote(name)+",\"new\":"+(asNew?"true":"false")+'}',30000);auto r=field(d.get(),"result");impl_->refresh();
        return {string(field(r,"id")),string(field(r,"revision")),strings(field(r,"left"))};
    }
    std::vector<std::string> RemoteControl::loadSound(uint32_t part,const std::string& id) {
        auto d=impl_->call("parts.loadSound","{\"part\":"+std::to_string(part)+",\"id\":"+quote(id)+'}',30000);impl_->refresh();
        return strings(field(field(d.get(),"result"),"flagged"));
    }
    Control::PerformanceContents RemoteControl::performanceContents(const std::string& directory) {
        auto d=impl_->call("performance.contents","{\"directory\":"+quote(directory)+'}',30000);auto r=field(d.get(),"result");PerformanceContents out;
        for(auto x:array(field(r,"parts")))out.parts.push_back({static_cast<uint32_t>(integer(field(x,"id"))),string(field(x,"name")),boolean(field(x,"bus"))});
        for(auto x:array(field(r,"scenes")))out.scenes.push_back({static_cast<uint32_t>(integer(field(x,"id"))),string(field(x,"name"))});
        return out;
    }
    Control::Imported RemoteControl::importPerformance(const std::string& directory,const std::vector<uint32_t>& parts,const std::vector<uint32_t>& scenes) {
        auto list=[](const std::vector<uint32_t>& ids){std::string out;for(const auto id:ids){if(!out.empty())out+=',';out+=std::to_string(id);}return out;};
        auto d=impl_->call("performance.import","{\"directory\":"+quote(directory)+",\"parts\":["+list(parts)+"],\"scenes\":["+list(scenes)+"]}",30000);impl_->refresh();
        auto r=field(d.get(),"result");Imported out;
        for(auto x:array(field(r,"parts")))out.parts.push_back(static_cast<uint32_t>(integer(x)));
        out.notes=strings(field(r,"notes"));
        return out;
    }
    uint32_t RemoteControl::partFromSound(const std::string& id) {
        auto d=impl_->call("parts.fromSound","{\"id\":"+quote(id)+'}',30000);impl_->refresh();
        return static_cast<uint32_t>(integer(field(field(d.get(),"result"),"part")));
    }
    void RemoteControl::loadPerformanceFromLibrary(const std::string& id) {
        impl_->call("library.load","{\"id\":"+quote(id)+'}',30000);impl_->refresh();
    }
    bool RemoteControl::supportsState(NodeId node) const {
        auto d=impl_->call("state.info","{\"node\":"+std::to_string(node)+'}');return boolean(field(field(d.get(),"result"),"supported"));
    }
    bool RemoteControl::hasPluginWindow(NodeId node) const {
        auto d=impl_->call("window.info","{\"node\":"+std::to_string(node)+'}');return boolean(field(field(d.get(),"result"),"available"));
    }
    void RemoteControl::showPluginWindow(NodeId node,bool show,const std::string& title) {
        impl_->call("window.show","{\"node\":"+std::to_string(node)+",\"show\":"+(show?"true":"false")+",\"title\":"+quote(title)+'}',10000);
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
    void RemoteControl::allNotesOff() { impl_->call("notes.panic"); }
    void RemoteControl::ignoreMidiInputs(const std::vector<std::string>& names)
    {
        std::string inputs;
        for(const auto& n : names) inputs += (inputs.empty() ? "" : ",") + quote(n);
        impl_->call("midi.ignore", "{\"inputs\":[" + inputs + "]}");
    }
    bool RemoteControl::note(bool on,int key,double velocity,int channel)
    {try{impl_->call("notes.send",std::string("{\"on\":")+(on?"true":"false")+",\"key\":"+std::to_string(key)+",\"velocity\":"+encode(velocity)+",\"channel\":"+std::to_string(channel)+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    std::vector<AssignmentOverride> RemoteControl::assignments() const
    {
        auto doc=impl_->call("assignments.get");std::vector<AssignmentOverride> out;
        for(auto a:array(field(field(doc.get(),"result"),"assignments"))){
            AssignmentOverride o;o.control=string(field(a,"control"));const auto kind=string(field(a,"kind"));
            for(size_t k=0;k<assignmentKinds.size();++k)if(kind==assignmentKinds[k])o.target.kind=Assignment::Kind(k);
            o.target.node=static_cast<NodeId>(integer(field(a,"node")));o.target.param=static_cast<uint32_t>(integer(field(a,"param")));o.target.id=static_cast<uint32_t>(integer(field(a,"id")));o.part=static_cast<uint32_t>(integer(field(a,"part")));
            out.push_back(std::move(o));}
        return out;
    }
    void RemoteControl::assign(const std::string& control,const Assignment& t,uint32_t part)
    {
        impl_->call("assignments.set","{\"control\":"+quote(control)+",\"kind\":\""+assignmentKinds[size_t(t.kind)]+"\",\"node\":"+std::to_string(t.node)+",\"param\":"+std::to_string(t.param)+",\"id\":"+std::to_string(t.id)+",\"part\":"+std::to_string(part)+'}');impl_->refresh();
    }
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
        std::string devicesText(const std::vector<DeviceRule>& rules)
        {
            std::string devices="[";
            for(const auto& r:rules){if(devices.size()>1)devices+=',';devices+=std::string("{\"kind\":\"")+deviceRuleKinds[size_t(r.kind)]+"\",\"port\":"+std::to_string(r.port)+",\"pattern\":"+quote(r.pattern)+",\"match\":\""+deviceMatches[size_t(r.match)]+"\"}";}
            return devices+']';
        }
        std::string inputText(const PartInput& input)
        {
            const char* kind=input.kind==PartInput::Kind::Notes?"notes":input.kind==PartInput::Kind::Audio?"audio":input.kind==PartInput::Kind::Bus?"bus":"none";
            return "{\"kind\":\""+std::string(kind)+"\",\"port\":"+std::to_string(input.port)+",\"zones\":"+zonesText(input.zones)+",\"devices\":"+devicesText(input.devices)+",\"maxNotes\":"+std::to_string(input.maxNotes)+"}";
        }
    }
    uint32_t RemoteControl::addPart(const std::string& name,PartInput input,std::optional<uint32_t> output)
    {
        auto reply=impl_->call("parts.add","{\"name\":"+quote(name)+",\"input\":"+inputText(input)+(output?",\"output\":"+std::to_string(*output):std::string{})+'}');
        const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"part")));impl_->refresh();return id;
    }
    void RemoteControl::setPartInput(uint32_t part,const PartInput& input){impl_->call("parts.input","{\"part\":"+std::to_string(part)+",\"input\":"+inputText(input)+'}');impl_->refresh();}
    void RemoteControl::setClockOutputs(const std::vector<DeviceRule>& devices)
    {impl_->call("transport.set","{\"clockOutputs\":"+devicesText(devices)+'}');auto state=impl_->call("transport.get");impl_->readTransport(field(state.get(),"result"));}
    void RemoteControl::setPartMidiOut(uint32_t part,const PartMidiOut& midiOut)
    {impl_->call("parts.midiOut","{\"part\":"+std::to_string(part)+",\"midiOut\":{\"devices\":"+devicesText(midiOut.devices)+",\"channel\":"+std::to_string(midiOut.channel)+"}}");impl_->refresh();}
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
    void RemoteControl::setSceneUntouched(const std::vector<SceneParameter>& parameters)
    {
        std::string list="[";
        for(const auto& at:parameters){if(list.size()>1)list+=',';list+="{\"node\":"+std::to_string(at.node)+",\"param\":"+std::to_string(at.param)+'}';}
        impl_->call("scenes.untouched","{\"parameters\":"+list+"]}");impl_->refresh();
    }
    void RemoteControl::setSceneTransition(uint32_t scene,uint32_t glide,Scene::Curve curve,Scene::Launch launch)
    {
        impl_->call("scenes.transition","{\"scene\":"+std::to_string(scene)+",\"glide\":"+std::to_string(glide)+",\"curve\":"+quote(sceneCurves[size_t(curve)])+",\"launch\":"+quote(sceneLaunches[size_t(launch)])+'}');impl_->refresh();
    }
    void RemoteControl::setSceneScope(uint32_t scene,uint32_t scope)
    {
        std::string list="[";
        for(size_t k=0;k<sceneKinds.size();++k)if(scope&(1u<<k)){if(list.size()>1)list+=',';list+=quote(sceneKinds[k]);}
        impl_->call("scenes.scope","{\"scene\":"+std::to_string(scene)+",\"scope\":"+list+"]}");impl_->refresh();
    }
    uint32_t RemoteControl::addMacro(const std::string& name, uint32_t part)
    {auto reply=impl_->call("controls.add","{\"name\":"+quote(name)+",\"part\":"+std::to_string(part)+'}');const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"control")));impl_->refresh();return id;}
    void RemoteControl::removeMacro(uint32_t macro){impl_->call("controls.remove","{\"control\":"+std::to_string(macro)+'}');impl_->refresh();}
    void RemoteControl::setMacroName(uint32_t macro,const std::string& name){impl_->call("controls.name","{\"control\":"+std::to_string(macro)+",\"name\":"+quote(name)+'}');impl_->refresh();}
    void RemoteControl::setMacroTargets(uint32_t macro,const std::vector<MacroTarget>& targets)
    {
        std::string list="[";
        for(const auto& t:targets){
            if(list.size()>1)list+=',';
            const char* kind=t.kind==MacroTarget::Kind::Parameter?"parameter":t.kind==MacroTarget::Kind::PartLevel?"partLevel":t.kind==MacroTarget::Kind::PartMute?"partMute":t.kind==MacroTarget::Kind::Morph?"morph":"control";
            const char* curve=t.curve==MacroTarget::Curve::Linear?"linear":t.curve==MacroTarget::Curve::Exponential?"exponential":t.curve==MacroTarget::Curve::Logarithmic?"logarithmic":"s-curve";
            list+=std::string("{\"kind\":\"")+kind+"\",\"node\":"+std::to_string(t.node)+",\"param\":"+std::to_string(t.param)+",\"part\":"+std::to_string(t.part)+",\"control\":"+std::to_string(t.macro)+",\"from\":"+std::to_string(t.sceneFrom)+",\"to\":"+std::to_string(t.sceneTo)+
                ",\"start\":"+encode(t.start)+",\"end\":"+encode(t.end)+",\"curve\":\""+curve+"\",\"relative\":"+(t.relative?"true":"false")+",\"offIn\":[";
            for(size_t i=0;i<t.offIn.size();++i)list+=(i?",":"")+std::to_string(t.offIn[i]);
            list+="]}";
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
    uint32_t RemoteControl::recordStart(const std::vector<RecordTap>& taps,const std::string& name)
    {
        static const char* const kinds[]{"output","input","part","node","notes"};
        std::string list;
        for(const auto& t:taps)
        {
            list+=std::string(list.empty()?"":",")+"{\"kind\":\""+kinds[size_t(t.kind)]+"\"";
            if(t.port)list+=",\"port\":"+std::to_string(t.port);
            if(t.part)list+=",\"part\":"+std::to_string(t.part);
            if(t.node)list+=",\"node\":"+std::to_string(t.node);
            if(t.preFader)list+=",\"fader\":\"pre\"";
            list+='}';
        }
        auto d=impl_->call("record.start","{\"taps\":["+list+"],\"name\":"+quote(name)+'}',30000);
        return static_cast<uint32_t>(integer(field(field(d.get(),"result"),"take")));
    }
    void RemoteControl::recordStop(uint32_t take){impl_->call("record.stop","{\"take\":"+std::to_string(take)+'}');}
    std::vector<Control::Take> RemoteControl::recordTakes() const
    {
        auto d=impl_->call("record.takes");std::vector<Take> out;
        for(auto v:array(field(field(d.get(),"result"),"takes")))
        {
            Take t{static_cast<uint32_t>(integer(field(v,"take"))),string(field(v,"state")),string(field(v,"folder")),string(field(v,"error")),{},
                   integer(field(v,"start")),integer(field(v,"frames")),{},integer(field(v,"droppedNotes"))};
            for(auto f:array(field(v,"files")))t.files.push_back(string(f));
            for(auto o:array(field(v,"overruns")))t.overruns.push_back({integer(field(o,"start")),integer(field(o,"frames"))});
            out.push_back(std::move(t));
        }
        return out;
    }
    Control::History RemoteControl::history() const
    {
        auto reply=impl_->call("edit.history");auto r=field(reply.get(),"result");
        return {string(field(r,"undo")),string(field(r,"redo"))};
    }
    bool RemoteControl::undo(){const bool any=!history().undo.empty();impl_->call("edit.undo");impl_->refresh();return any;}
    bool RemoteControl::redo(){const bool any=!history().redo.empty();impl_->call("edit.redo");impl_->refresh();return any;}
    void RemoteControl::setPartPan(uint32_t part,double pan){impl_->call("parts.pan","{\"part\":"+std::to_string(part)+",\"pan\":"+encode(pan)+'}');impl_->refresh();}
    void RemoteControl::setPartSolo(uint32_t part,bool solo){impl_->call("parts.solo","{\"part\":"+std::to_string(part)+",\"solo\":"+(solo?"true":"false")+'}');impl_->refresh();}
    std::vector<PartMeter> RemoteControl::partMeters() const
    {
        auto reply=impl_->call("parts.meters");std::vector<PartMeter> out;
        for(auto m:array(field(field(reply.get(),"result"),"meters")))
        {
            PartMeter meter{static_cast<uint32_t>(integer(field(m,"part"))),number(field(m,"peak")),{}};
            for(auto v:array(field(m,"channels")))meter.channels.push_back(number(v));
            out.push_back(std::move(meter));
        }
        return out;
    }

    std::vector<WireMeter> RemoteControl::wireMeters() const
    {
        auto reply=impl_->call("wires.meters");std::vector<WireMeter> out;
        for(auto m:array(field(field(reply.get(),"result"),"meters")))
        {
            WireMeter meter{static_cast<uint32_t>(integer(field(m,"wire"))),number(field(m,"peak")),{}};
            for(auto v:array(field(m,"channels")))meter.channels.push_back(number(v));
            out.push_back(std::move(meter));
        }
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
    void RemoteControl::setWireEnabled(uint32_t wire,bool enabled){impl_->call("wires.enable","{\"wire\":"+std::to_string(wire)+",\"enabled\":"+(enabled?"true":"false")+'}');impl_->refresh();}
    void RemoteControl::setNodeEnabled(NodeId node,bool enabled){impl_->call("graph.enable","{\"node\":"+std::to_string(node)+",\"enabled\":"+(enabled?"true":"false")+'}');impl_->refresh();}
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

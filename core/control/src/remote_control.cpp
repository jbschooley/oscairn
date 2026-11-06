// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/remote_control.h"
#include "PropertyWire.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "CurveJson.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <locale>
#include <map>
#include <optional>
#include <set>
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
        // RFC 0033: a key as harmony.get and graph.get give it, and as harmony.set takes it.
        MusicalKey keyFrom(V v)
        {
            MusicalKey k{uint8_t(integer(field(v,"root"))),0};
            for(auto s:array(field(v,"scale")))k.scale|=uint16_t(1u<<(integer(s)&15));
            if(!k.valid())throw std::runtime_error("Invalid RPC key");
            return k;
        }
        std::string keyText(const MusicalKey& k)
        {
            std::string steps;for(int i=0;i<12;++i)if(k.scale>>i&1)steps+=(steps.empty()?"":",")+std::to_string(i);
            return "{\"root\":"+std::to_string(k.root)+",\"scale\":["+steps+"]}";
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
        // Live edits (RFC 0011's instant controls) go without waiting for their replies: the
        // cache shows them at once, and what they set stays over it until the engine has
        // answered, so a push sent before it had them does not show the old value. key: what an
        // edit sets ("level:3" for a part's level and mute, "pan:3", "solo:3", "master", "scene").
        struct Sent { uint64_t id; std::string key; std::function<void(GraphState&)> apply; };
        std::vector<Sent> sent;
        bool live=false;              // a live edit sent since the last commit
        std::string failure;          // a live edit the engine refused, for takeError()
        // Those answered while a graph read is out: its graph may be older than them, so they go
        // over it when it comes in, unless the engine pushed the value since or refused them.
        std::vector<Sent> confirmed;
        // Plugin parameters set without waiting (a knob dragged): the cache shows the value at once,
        // and the newest sent for an address stays over the engine's pushes until its reply, which
        // gives the value the engine kept (clamped, stepped).
        std::map<std::pair<NodeId,uint32_t>,std::pair<uint64_t,double>> sentValues; // address: newest request, its value
        std::map<uint64_t,std::pair<NodeId,uint32_t>> valueReplies;                 // request: its address
        void answered(uint64_t id)    // every request up to id is answered
        {
            for(auto s=sent.begin();s!=sent.end();)
                if(s->id<=id){if(reading)confirmed.push_back(std::move(*s));s=sent.erase(s);}else ++s;
        }
        void refused(uint64_t id){std::erase_if(confirmed,[&](const Sent& s){return s.id==id;});}
        void pushed(const std::string& key){std::erase_if(confirmed,[&](const Sent& s){return s.key==key;});}
        static void overlay(GraphState& g,const std::vector<Sent>& list){for(const auto& s:list)s.apply(g);}
        // The wires faders move live follow the parts' levels, mutes and pans, as the engine's own do.
        static void follow(GraphState& g)
        {
            for(auto& w:g.wires)
            {
                if(w.faders.empty())continue;
                w.level=w.fixed;w.mute=w.fixedMute;
                for(const auto f:w.faders)for(const auto& p:g.parts)if(p.id==f){w.level*=p.level;w.mute=w.mute || p.mute;if(w.fader && f==w.part)w.pan=std::clamp(p.pan+w.share,-1.0,1.0);}
            }
        }
        void overlay(){overlay(graph,sent);}
        // Sends a live edit and shows it; one sent for the same key before it is over it now.
        void liveEdit(std::string_view method,const std::string& params,std::string key,std::function<void(GraphState&)> apply,bool commits=true)
        {
            const auto id=client.send(method,params);
            std::erase_if(sent,[&](const Sent& s){return s.key==key;});
            apply(graph);sent.push_back({id,std::move(key),std::move(apply)});live=live || commits;
        }
        static void check(const Doc& doc)
        {
            V error=nullptr;try{error=field(doc.get(),"error");}catch(const std::runtime_error&){}
            if(error)
            {
                const auto code=static_cast<int>(number(field(error,"code")));
                const auto message=string(field(error,"message"));
                if(code==-32602 || code==-32001 || code==-32002)throw std::invalid_argument(message);
                throw RpcError(code,message);
            }
            field(doc.get(),"result");
        }
        Doc call(std::string_view method,std::string_view params="{}",unsigned timeout=1000){return reply(client.send(method,params,timeout),timeout);}
        Doc reply(uint64_t id,unsigned timeout=1000){auto doc=parse(client.wait(id,timeout));answered(id);check(doc);return doc;}
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
        Control::ControllerState controllers;
        void readControllers(V v)
        {
            controllers.positions.clear();
            for(auto p:array(field(v,"positions")))controllers.positions.push_back(static_cast<uint32_t>(integer(p)));
            controllers.touched=string(field(v,"touched"));controllers.touches=integer(field(v,"touches"));
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
            status.notices.clear();for(auto n:array(field(v,"notices")))status.notices.push_back(string(n));
            for(auto w:array(field(v,"workers")))status.workers.push_back({static_cast<NodeId>(integer(field(w,"node"))),string(field(w,"name")),number(field(w,"cpu")),uint64_t(integer(field(w,"memory"))),number(field(w,"block")),number(field(w,"blockMax"))});
            for(auto [list,key]:{std::pair{&status.midiInputs,"midiInputs"},{&status.midiOutputs,"midiOutputs"}})
                for(auto e:array(field(v,key)))
                    list->push_back({MidiEndpoint::Kind(named(endpointKinds,string(field(e,"kind")))),uint8_t(integer(field(e,"source"))),string(field(e,"name")),static_cast<uint32_t>(integer(field(e,"port")))});
        }
        static GraphState readGraph(V root)
        {
            GraphState next;
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
                if(const auto role=field(n,"role");role->type!=json_type_null)next.roles[node.id]=string(role);
                if(const auto s=field(n,"shared");s->type!=json_type_null)next.shared[node.id]={static_cast<NodeId>(integer(field(s,"engine"))),static_cast<uint16_t>(integer(field(s,"channel"))),static_cast<uint32_t>(integer(field(s,"bus")))};
                next.parameterRevisions[node.id]=integer(field(n,"parameterRevision"));
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
                part.input.port=static_cast<uint32_t>(integer(field(input,"port")));part.input.zones=zones(field(input,"zones"));part.input.maxNotes=uint16_t(integer(field(input,"maxNotes")));part.input.reserve=uint16_t(integer(field(input,"reserve")));part.input.priority=uint8_t(integer(field(input,"priority")));
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
                if(const auto key=field(p,"key");key->type!=json_type_null)part.key=keyFrom(key);
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
                for(auto f:array(field(w,"faders")))wire.faders.push_back(static_cast<uint32_t>(integer(f)));
                wire.fixed=number(field(w,"fixed"));wire.fixedMute=boolean(field(w,"fixedMute"));wire.share=number(field(w,"share"));
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
                scene.masterLevel=number(field(field(s,"master"),"level"));scene.masterPan=number(field(field(s,"master"),"pan"));
                next.scenes.push_back(std::move(scene));
            }
            for(auto at:array(field(root,"sceneUntouched")))next.untouched.push_back({static_cast<NodeId>(integer(field(at,"node"))),static_cast<uint32_t>(integer(field(at,"param")))});
            next.scene=static_cast<uint32_t>(integer(field(root,"scene")));
            next.masterLevel=number(field(field(root,"master"),"level"));next.masterPan=number(field(field(root,"master"),"pan"));
            for(auto m:array(field(root,"controls")))
            {
                Macro macro;macro.id=static_cast<uint32_t>(integer(field(m,"id")));macro.part=static_cast<uint32_t>(integer(field(m,"part")));macro.name=string(field(m,"name"));macro.value=number(field(m,"value"));
                for(auto t:array(field(m,"targets")))
                {
                    MacroTarget target;const auto kind=string(field(t,"kind"));
                    target.kind=kind=="parameter"?MacroTarget::Kind::Parameter:kind=="partLevel"?MacroTarget::Kind::PartLevel:kind=="partMute"?MacroTarget::Kind::PartMute:kind=="morph"?MacroTarget::Kind::Morph:MacroTarget::Kind::Macro;
                    if(!detail::curveFrom(field(t,"curve"),target))throw std::runtime_error("Invalid RPC curve");
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
            return next;
        }
        // A read of the graph and every node's parameters: graph.get, then the params.list of each
        // node whose parameterRevision is not the cache's, then every value, several requests out at once (the engine answers up to 16 a client each
        // tick of its timer), so a read takes a few ticks rather than one per node.
        struct GraphRead
        {
            enum class Step { Graph, List, Values };
            std::deque<std::tuple<Step,std::string,std::string>> queued{{Step::Graph,"graph.get","{}"}}; // not sent yet
            std::deque<std::pair<uint64_t,Step>> sent;                                                     // in the order they are answered
            GraphState next;std::map<NodeId,std::vector<ParamInfo>> metadata;std::map<std::pair<NodeId,uint32_t>,double> snapshot;
            std::vector<NodeId> nodes;size_t listed=0;std::string batch;size_t count=0;
            bool graphRead=false; // its graph.get answered: values pushed after it are newer than it
            bool done()const{return queued.empty() && sent.empty();}
            void values(){if(count)queued.push_back({Step::Values,"params.get","{\"values\":["+batch+"]}"});batch.clear();count=0;}
            void value(NodeId node,uint32_t param){if(count)batch+=',';batch+='{'+address(node,param)+'}';if(++count==128)values();}
        };
        size_t largest=0;                 // the longest params.list reply seen: how many fit in the engine's 1 MB queue at once
        std::optional<GraphRead> reading; // the read service() started, taken as its replies come
        bool again=false;                 // the graph changed again while it was out
        std::set<uint64_t> dropped;       // replies to a read given up, which nobody takes
        void send(GraphRead& r)
        {
            const size_t window=largest?std::clamp<size_t>(LocalServer::maxQueuedBytes/2/largest,1,LocalServer::maxRequestsPerTick):4;
            while(!r.queued.empty() && r.sent.size()<window)
            {
                const auto& [step,method,params]=r.queued.front();
                r.sent.emplace_back(client.send(method,params),step);r.queued.pop_front();
            }
        }
        void take(GraphRead& r,GraphRead::Step step,V result,size_t bytes)
        {
            using Step=GraphRead::Step;
            if(step==Step::Graph)
            {
                r.next=readGraph(result);r.graphRead=true;
                for(const auto& n:r.next.topology.nodes)
                {
                    // Its info as the cache has it, unless its plugin or that info changed since.
                    if(const auto known=graph.parameterRevisions.find(n.id);known!=graph.parameterRevisions.end() && known->second==r.next.parameterRevisions.at(n.id) && info.contains(n.id))
                    {const auto& list=r.metadata[n.id]=info.at(n.id);for(const auto& p:list)r.value(n.id,p.id);continue;}
                    r.nodes.push_back(n.id);r.queued.push_back({Step::List,"params.list","{\"node\":"+std::to_string(n.id)+'}'});
                }
                if(r.nodes.empty())r.values();
            }
            else if(step==Step::List)
            {
                largest=std::max(largest,bytes);
                const auto node=r.nodes.at(r.listed++);auto& list=r.metadata[node];
                for(auto p:array(field(result,"parameters")))
                {
                    ParamInfo param{static_cast<uint32_t>(integer(field(p,"id"))),string(field(p,"name")),number(field(p,"minimum")),number(field(p,"maximum")),number(field(p,"defaultValue")),boolean(field(p,"stepped")),static_cast<uint32_t>(integer(field(p,"flags"))),string(field(p,"module")),
                                    widgetNamed(std::string(string(field(p,"widget")))),static_cast<uint32_t>(integer(field(p,"priority")))};
                    if(param.widget==OSCAIRN_WIDGET_RANGE)param.rangeHigh=static_cast<uint32_t>(integer(field(p,"rangeHigh")));
                    param.group=static_cast<uint32_t>(integer(field(p,"group")));
                    if(V role=detail::member(p,"role"))param.role=roleNamed(string(role));
                    list.push_back(param);r.value(node,param.id);
                }
                if(r.listed==r.nodes.size())r.values();
            }
            else for(auto value:array(field(result,"values")))r.snapshot[{static_cast<NodeId>(integer(field(value,"node"))),static_cast<uint32_t>(integer(field(value,"param")))}]=number(field(value,"value"));
        }
        void apply(GraphRead& r)
        {
            overlay(r.next,confirmed);confirmed.clear();
            graph=std::move(r.next);info=std::move(r.metadata);values=std::move(r.snapshot);overlay();
            for(const auto& [key,sent]:sentValues)if(values.contains(key))values[key]=sent.second;
        }
        void drop(){if(reading)for(const auto& s:reading->sent)dropped.insert(s.first);reading.reset();again=false;confirmed.clear();}
        // Waits for the whole read: an edit's caller reads the state it made on return.
        void refresh()
        {
            drop(); // older than this one
            GraphRead r;
            try
            {
                send(r);
                while(!r.sent.empty())
                {
                    const auto [id,step]=r.sent.front();r.sent.pop_front();
                    const auto reply=client.wait(id,1000);auto doc=parse(reply);answered(id);check(doc);
                    take(r,step,field(doc.get(),"result"),reply.size());send(r);
                }
            }
            catch(...){for(const auto& s:r.sent)dropped.insert(s.first);throw;}
            apply(r);
        }
        // Sends the read and returns: service() takes its replies as they come.
        void refreshLater(){if(reading){again=true;return;}reading.emplace();send(*reading);}
        // Reads sent without waiting (the views' polls, a gap's snapshots): service() hands each
        // reply's result to its reader. One read of a kind is out at a time.
        struct Fetch { std::string key; std::function<void(V)> read; };
        std::map<uint64_t,Fetch> fetches;std::set<std::string> fetching;
        bool fetch(std::string key,std::string_view method,const std::string& params,std::function<void(V)> read)
        {
            if(fetching.contains(key))return false;
            const auto id=client.send(method,params);fetching.insert(key);fetches[id]={std::move(key),std::move(read)};return true;
        }
        // A reply to the read out: true once the read is done and the cache holds it.
        bool reply(const Doc& doc,size_t bytes)
        {
            const auto step=reading->sent.front().second;reading->sent.pop_front();
            try{check(doc);take(*reading,step,field(doc.get(),"result"),bytes);send(*reading);}
            catch(const std::exception&)
            {
                // A node removed between the graph and its parameters: the change that removed
                // it is on its way, and read once it comes. Anything else is said, as before.
                const bool newer=again;drop();if(!newer)throw;refreshLater();return false;
            }
            if(!reading->done())return false;
            apply(*reading);reading.reset();
            if(std::exchange(again,false))refreshLater();
            return true;
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
    RemoteControl::RemoteControl(LocalClient& client,const KnownParameters& known):impl_(std::make_unique<Impl>(client))
    {
        for(const auto& [node,k]:known){impl_->graph.parameterRevisions[node]=k.first;impl_->info[node]=k.second;}
        auto hello=impl_->call("session.hello");if(integer(field(field(hello.get(),"result"),"protocolVersion"))!=1)throw std::runtime_error("Unsupported control protocol version");
        // The rest at once, the graph read behind them: the server answers in order, so the
        // snapshots are read after subscribing, and the reads take a few ticks, not one each.
        auto& c=impl_->client;
        const auto subscribed=c.send("subscribe",R"({"topics":["params.changed","transport","graph.changed","status","controllers","controls.changed"],"allChanges":true})");
        const auto controllers=c.send("controllers.get"),transport=c.send("transport.get"),status=c.send("session.status");
        impl_->refresh();
        impl_->reply(subscribed);
        {auto state=impl_->reply(controllers);impl_->readControllers(field(state.get(),"result"));}
        {auto state=impl_->reply(transport);impl_->readTransport(field(state.get(),"result"));}
        {auto state=impl_->reply(status);impl_->readStatus(field(state.get(),"result"));}
        // Keep interleaved notifications: another client may have changed a value
        // after its snapshot was read. service() drains them in server order.
    }
    RemoteControl::~RemoteControl()=default;
    RemoteControl::KnownParameters RemoteControl::knownParameters()const
    {KnownParameters known;for(const auto& [node,list]:impl_->info)if(const auto r=impl_->graph.parameterRevisions.find(node);r!=impl_->graph.parameterRevisions.end())known[node]={r->second,list};return known;}
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
    namespace { Control::PerformanceInfo performanceFrom(V r){return {string(field(r,"id")),string(field(r,"name")),string(field(r,"description")),strings(field(r,"tags")),string(field(r,"color"))};} }
    Control::PerformanceInfo RemoteControl::performanceInfo() const {
        auto d=impl_->call("performance.info");return performanceFrom(field(d.get(),"result"));
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
    namespace { Control::Audition auditionFrom(V r){return {string(field(r,"id")),string(field(r,"kind")),static_cast<uint32_t>(integer(field(r,"part")))};} }
    Control::Audition RemoteControl::audition() const {
        auto d=impl_->call("audition.get");return auditionFrom(field(d.get(),"result"));
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
        auto d=impl_->call("presets.list","{\"node\":"+std::to_string(node)+'}');std::vector<PresetInfo> result; // the engine never waits for the scan helper: "ask again" until it is done
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
    // Live (RFC 0011's instant controls): shown in the cache at once, sent without waiting. A
    // refusal (the pending store full, a read-only parameter) arrives with service(), as the
    // edit's error, and the engine's values are read again.
    bool RemoteControl::setParameter(NodeId n,uint32_t p,double value)
    {
        const std::pair key{n,p};
        if(!impl_->values.contains(key))throw std::invalid_argument("Unknown parameter"); // as the engine would say, at once
        const auto info=parameterInfo(n,p);
        if(info.flags&CLAP_PARAM_IS_READONLY)throw std::invalid_argument("Parameter is read-only");
        if(!std::isfinite(value))throw std::invalid_argument("Invalid parameter value");
        const auto id=impl_->client.send("params.set","{\"values\":[{"+address(n,p)+",\"value\":"+encode(value)+"}]}");
        // Shown as the engine keeps it: clamped to the range, a stepped value truncated.
        auto kept=std::clamp(value,info.minimum,info.maximum);if(info.stepped)kept=std::trunc(kept);
        impl_->sentValues[key]={id,kept};impl_->valueReplies[id]=key;impl_->values[key]=kept;
        return true;
    }
    bool RemoteControl::fetchParameterText(NodeId n,uint32_t p,double value,std::function<void(std::string)> done)
    {return impl_->fetch("params.text"+address(n,p),"params.text",'{'+address(n,p)+",\"value\":"+encode(value)+'}',[done=std::move(done)](V r){done(string(field(r,"text")));});}
    bool RemoteControl::parameterGesture(NodeId n,uint32_t p,bool begin)
    {try{impl_->call("params.gesture",'{'+address(n,p)+",\"begin\":"+(begin?"true":"false")+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    void RemoteControl::allNotesOff() { impl_->call("notes.panic"); }
    void RemoteControl::ignoreMidiInputs(const std::vector<std::string>& names)
    {
        std::string inputs;
        for(const auto& n : names) inputs += (inputs.empty() ? "" : ",") + quote(n);
        impl_->call("midi.ignore", "{\"inputs\":[" + inputs + "]}");
    }
    void RemoteControl::setControllers(const std::vector<ControlGroup>& groups,const std::vector<ControllerPort>& ports)
    {
        static constexpr std::array<const char*,4> roles{"notes","screen","assign","fixed"},messages{"cc","note","pitchBend","pressure"};
        auto list=[](const auto& items,auto text){std::string out;for(const auto& i:items)out+=(out.empty()?"":",")+text(i);return '['+out+']';};
        auto number=[](auto n){return std::to_string(n);};
        std::string text;
        for(const auto& g:groups)
        {
            const auto& m=g.midi;
            text+=(text.empty()?"":",")+std::string("{\"role\":")+quote(roles[size_t(g.role)])+",\"kind\":"+quote(g.kind)+",\"count\":"+std::to_string(g.count)
                +",\"rows\":"+list(g.rows,[](const ControlGroup::Row& r){return "{\"name\":"+quote(r.name)+",\"bank\":"+quote(bankKinds[size_t(r.bank)])+'}';})
                +",\"modes\":"+list(g.modes,quote)+",\"functions\":"+list(g.functions,quote)
                +",\"scenes\":"+(g.scenes?"true":"false")+",\"paged\":"+(g.paged?"true":"false")+",\"switches\":"+(g.switches?"true":"false")+",\"panic\":"+(g.panic?"true":"false");
            if(!m.device.empty())
                text+=",\"midi\":{\"device\":"+quote(m.device)+",\"port\":"+std::to_string(m.port)+",\"channel\":"+std::to_string(m.channel)+",\"channels\":"+list(m.channels,number)
                    +",\"message\":"+quote(messages[size_t(m.message)])+",\"numbers\":"+list(m.numbers,[](uint8_t n){return std::to_string(n);})
                    +",\"encodings\":"+list(m.encodings,[](Encoding e){return quote(e==Encoding::Steps?"steps":encodings[size_t(e)]);})
                    +",\"steps\":"+list(m.steps,[](const std::array<uint8_t,2>& s){return '['+std::to_string(s[0])+','+std::to_string(s[1])+']';})
                    +",\"latch\":"+(m.latch?"true":"false")+",\"feedback\":"+(m.feedback?"true":"false")+'}';
            text+='}';
        }
        impl_->call("controllers.set","{\"groups\":["+text+"],\"ports\":"+list(ports,[](const ControllerPort& p){return "{\"input\":"+quote(p.input)+",\"device\":"+quote(p.device)+",\"port\":"+std::to_string(p.port)+'}';})+'}');
    }
    void RemoteControl::setControlView(const ControlView& view)
    {
        std::string text;
        if(view.part)text+="\"part\":"+std::to_string(*view.part);
        if(view.screen)
        {
            std::string params;
            for(const auto& t:view.screen->params)params+=(params.empty()?"":",")+std::string("{\"kind\":\"")+assignmentKinds[size_t(t.kind)]+"\",\"node\":"+std::to_string(t.node)+",\"param\":"+std::to_string(t.param)+",\"id\":"+std::to_string(t.id)+'}';
            text+=(text.empty()?"":",")+std::string("\"screen\":{\"name\":")+quote(view.screen->name)+",\"params\":["+params+"]}";
        }
        if(!view.positions.empty())
        {
            std::string positions;
            for(const auto& [g,p]:view.positions)positions+=(positions.empty()?"":",")+std::string("{\"group\":")+std::to_string(g)+",\"position\":"+std::to_string(p)+'}';
            text+=(text.empty()?"":",")+std::string("\"positions\":[")+positions+']';
        }
        impl_->call("controllers.view",'{'+text+'}');
    }
    Control::ControllerState RemoteControl::controllerState() const { return impl_->controllers; }
    Control::LiveValues RemoteControl::liveValues() const
    {
        LiveValues v;const auto& g=impl_->graph;
        for(const auto& m:g.macros)v.controls.push_back({m.id,m.value});
        for(const auto& p:g.parts)v.parts.push_back({p.id,p.level,p.mute,p.pan});
        v.masterLevel=g.masterLevel;v.masterPan=g.masterPan;
        return v;
    }
    bool RemoteControl::note(bool on,int key,double velocity,int channel)
    {try{impl_->call("notes.send",std::string("{\"on\":")+(on?"true":"false")+",\"key\":"+std::to_string(key)+",\"velocity\":"+encode(velocity)+",\"channel\":"+std::to_string(channel)+'}');return true;}catch(const RpcError& e){if(e.code==-32005)return false;throw;}}
    namespace { std::vector<AssignmentOverride> assignmentsFrom(V r)
    {
        std::vector<AssignmentOverride> out;
        for(auto a:array(field(r,"assignments"))){
            AssignmentOverride o;o.control=string(field(a,"control"));const auto kind=string(field(a,"kind"));
            for(size_t k=0;k<assignmentKinds.size();++k)if(kind==assignmentKinds[k])o.target.kind=Assignment::Kind(k);
            o.target.node=static_cast<NodeId>(integer(field(a,"node")));o.target.param=static_cast<uint32_t>(integer(field(a,"param")));o.target.id=static_cast<uint32_t>(integer(field(a,"id")));o.part=static_cast<uint32_t>(integer(field(a,"part")));
            out.push_back(std::move(o));}
        return out;
    } }
    std::vector<AssignmentOverride> RemoteControl::assignments() const {auto doc=impl_->call("assignments.get");return assignmentsFrom(field(doc.get(),"result"));}
    void RemoteControl::assign(const std::string& control,const Assignment& t,uint32_t part)
    {
        impl_->call("assignments.set","{\"control\":"+quote(control)+",\"kind\":\""+assignmentKinds[size_t(t.kind)]+"\",\"node\":"+std::to_string(t.node)+",\"param\":"+std::to_string(t.param)+",\"id\":"+std::to_string(t.id)+",\"part\":"+std::to_string(part)+'}');impl_->refresh();
    }
    namespace { PadMap padMapFrom(V root)
    {
        PadMap map;
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
    } }
    PadMap RemoteControl::padMap() const {auto doc=impl_->call("pads.get");return padMapFrom(field(doc.get(),"result"));}
    bool RemoteControl::fetchPadMap(std::function<void(PadMap)> done){return impl_->fetch("pads.get","pads.get","{}",[done=std::move(done)](V r){done(padMapFrom(r));});}
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
    NodeId RemoteControl::shareInstrument(const std::string& name,NodeId source)
    {auto doc=impl_->call("instruments.share","{\"name\":"+quote(name)+",\"source\":"+std::to_string(source)+'}',30000);const auto id=static_cast<NodeId>(integer(field(field(doc.get(),"result"),"node")));impl_->refresh();return id;}
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
            return "{\"kind\":\""+std::string(kind)+"\",\"port\":"+std::to_string(input.port)+",\"zones\":"+zonesText(input.zones)+",\"devices\":"+devicesText(input.devices)+",\"maxNotes\":"+std::to_string(input.maxNotes)+",\"reserve\":"+std::to_string(input.reserve)+",\"priority\":"+std::to_string(input.priority)+"}";
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
    MusicalKey RemoteControl::key() const {auto doc=impl_->call("harmony.get");return keyFrom(field(field(doc.get(),"result"),"key"));}
    void RemoteControl::setKey(const MusicalKey& key){impl_->call("harmony.set","{\"key\":"+keyText(key)+'}');impl_->refresh();}
    void RemoteControl::setPartKey(uint32_t part,const std::optional<MusicalKey>& key)
    {impl_->call("harmony.set","{\"part\":"+std::to_string(part)+",\"key\":"+(key?keyText(*key):std::string("null"))+'}');impl_->refresh();}
    void RemoteControl::setPartOutput(uint32_t part,uint32_t bus,std::optional<uint32_t> port)
    {impl_->call("parts.output","{\"part\":"+std::to_string(part)+(bus?",\"bus\":"+std::to_string(bus):std::string{})+(port?",\"port\":"+std::to_string(*port):std::string{})+'}');impl_->refresh();}
    void RemoteControl::setSend(uint32_t part,uint32_t bus,std::optional<double> level)
    {impl_->call("parts.send","{\"part\":"+std::to_string(part)+",\"bus\":"+std::to_string(bus)+(level?",\"level\":"+encode(*level):std::string{})+'}');impl_->refresh();}
    // Live (RFC 0011's instant controls): shown in the cache at once, sent without waiting.
    void RemoteControl::setPartLevel(uint32_t part,double level,bool mute)
    {
        impl_->liveEdit("parts.level","{\"part\":"+std::to_string(part)+",\"level\":"+encode(level)+",\"mute\":"+(mute?"true":"false")+'}',"level:"+std::to_string(part),
            [=](GraphState& g){for(auto& p:g.parts)if(p.id==part){p.level=level;p.mute=mute;}});
    }
    void RemoteControl::setPartName(uint32_t part,const std::string& name)
    {impl_->call("parts.name","{\"part\":"+std::to_string(part)+",\"name\":"+quote(name)+'}');impl_->refresh();}
    uint32_t RemoteControl::addScene(const std::string& name)
    {auto reply=impl_->call("scenes.add","{\"name\":"+quote(name)+'}');const auto id=static_cast<uint32_t>(integer(field(field(reply.get(),"result"),"scene")));impl_->refresh();return id;}
    void RemoteControl::storeScene(uint32_t scene){impl_->call("scenes.store","{\"scene\":"+std::to_string(scene)+'}');impl_->refresh();}
    // Live: the engine commits a scene itself, so no commit follows it; its push confirms it.
    void RemoteControl::selectScene(uint32_t scene)
    {impl_->liveEdit("scenes.select","{\"scene\":"+std::to_string(scene)+'}',"scene",[=](GraphState& g){g.scene=scene;},false);}
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
            list+=std::string("{\"kind\":\"")+kind+"\",\"node\":"+std::to_string(t.node)+",\"param\":"+std::to_string(t.param)+",\"part\":"+std::to_string(t.part)+",\"control\":"+std::to_string(t.macro)+",\"from\":"+std::to_string(t.sceneFrom)+",\"to\":"+std::to_string(t.sceneTo)+
                ",\"start\":"+encode(t.start)+",\"end\":"+encode(t.end)+",\"curve\":"+detail::curveJson(t,encode)+",\"relative\":"+(t.relative?"true":"false")+",\"offIn\":[";
            for(size_t i=0;i<t.offIn.size();++i)list+=(i?",":"")+std::to_string(t.offIn[i]);
            list+="]}";
        }
        impl_->call("controls.targets","{\"control\":"+std::to_string(macro)+",\"targets\":"+list+"]}");impl_->refresh();
    }
    void RemoteControl::setMacro(uint32_t macro,double value)
    {
        impl_->call("controls.set","{\"control\":"+std::to_string(macro)+",\"value\":"+encode(value)+'}');
        // A control that only moves plugin parameters changes nothing else in the graph state: keep
        // its value here rather than read the whole graph again on every move. One that moves part
        // levels, mutes, other controls or a morph is read again, so the state is current on return.
        bool shown=false;
        for(auto& m:impl_->graph.macros)if(m.id==macro){m.value=value;for(const auto& t:m.targets)shown=shown||t.kind!=MacroTarget::Kind::Parameter;}
        if(shown)impl_->refresh();
    }
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
    namespace
    {
        std::string tapList(const std::vector<Control::RecordTap>& taps)
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
            return "["+list+"]";
        }
    }
    uint32_t RemoteControl::recordStart(const std::vector<RecordTap>& taps,const std::string& name,const RecordWhen& when)
    {
        using When=RecordWhen;
        std::string extra;
        if(when.from==When::From::SecondsAgo)extra+=",\"from\":{\"secondsAgo\":"+encode(when.seconds)+'}';
        else if(when.from!=When::From::Now)extra+=std::string(",\"from\":{\"bar\":\"")+(when.from==When::From::NextBar?"next":"last")+"\"}";
        if(when.until!=When::Until::Stop)extra+=std::string(",\"until\":{\"")+(when.until==When::Until::Bars?"bars":"seconds")+"\":"+encode(when.length)+'}';
        if(when.keepGoing)extra+=",\"keepGoing\":true";
        auto d=impl_->call("record.start","{\"taps\":"+tapList(taps)+",\"name\":"+quote(name)+extra+'}',30000);
        return static_cast<uint32_t>(integer(field(field(d.get(),"result"),"take")));
    }
    void RemoteControl::recordRetro(double seconds,const std::vector<RecordTap>& taps){impl_->call("record.retro","{\"seconds\":"+encode(seconds)+",\"taps\":"+tapList(taps)+'}',30000);}
    double RemoteControl::recordRetroSeconds() const{auto d=impl_->call("record.get");return number(field(field(field(d.get(),"result"),"retro"),"seconds"));}
    void RemoteControl::recordDelete(uint32_t take){impl_->call("record.delete","{\"take\":"+std::to_string(take)+'}',30000);}
    void RemoteControl::recordRename(uint32_t take,const std::string& name){impl_->call("record.rename","{\"take\":"+std::to_string(take)+",\"name\":"+quote(name)+'}',30000);}
    void RemoteControl::recordPlay(uint32_t take){impl_->call("record.play","{\"take\":"+std::to_string(take)+'}',30000);}
    void RemoteControl::recordStop(uint32_t take){impl_->call("record.stop","{\"take\":"+std::to_string(take)+'}');}
    namespace { std::vector<Control::Take> takesFrom(V r)
    {
        std::vector<Control::Take> out;
        for(auto v:array(field(r,"takes")))
        {
            Control::Take t{static_cast<uint32_t>(integer(field(v,"take"))),string(field(v,"state")),string(field(v,"folder")),string(field(v,"error")),{},
                   integer(field(v,"start")),integer(field(v,"frames")),{},integer(field(v,"droppedNotes")),number(field(v,"sampleRate")),boolean(field(v,"behind")),boolean(field(v,"playing"))};
            for(auto f:array(field(v,"files")))t.files.push_back(string(f));
            for(auto o:array(field(v,"overruns")))t.overruns.push_back({integer(field(o,"start")),integer(field(o,"frames"))});
            out.push_back(std::move(t));
        }
        return out;
    }
    Control::History historyFrom(V r){return {string(field(r,"undo")),string(field(r,"redo"))};} }
    std::vector<Control::Take> RemoteControl::recordTakes() const {auto d=impl_->call("record.takes");return takesFrom(field(d.get(),"result"));}
    Control::History RemoteControl::history() const {auto reply=impl_->call("edit.history");return historyFrom(field(reply.get(),"result"));}
    bool RemoteControl::undo(){const bool any=!history().undo.empty();impl_->call("edit.undo");impl_->refresh();return any;}
    bool RemoteControl::redo(){const bool any=!history().redo.empty();impl_->call("edit.redo");impl_->refresh();return any;}
    void RemoteControl::setNodeRole(NodeId node,const std::string& role){impl_->call("graph.role","{\"node\":"+std::to_string(node)+",\"role\":"+(role.empty()?std::string("null"):quote(role))+'}');impl_->refresh();}
    void RemoteControl::setMaster(double level,double pan)
    {
        impl_->liveEdit("master.set","{\"level\":"+encode(level)+",\"pan\":"+encode(pan)+'}',"master",[=](GraphState& g){g.masterLevel=level;g.masterPan=pan;});
    }
    // Live: shown at once, sent without waiting; the engine's push (a graph.changed it reads, or
    // controls.changed) confirms them.
    void RemoteControl::setPartPan(uint32_t part,double pan)
    {
        if(!std::isfinite(pan) || pan<-1 || pan>1)throw std::invalid_argument("pan is -1..1");
        impl_->liveEdit("parts.pan","{\"part\":"+std::to_string(part)+",\"pan\":"+encode(pan)+'}',"pan:"+std::to_string(part),[=](GraphState& g){for(auto& p:g.parts)if(p.id==part)p.pan=pan;});
    }
    void RemoteControl::setPartSolo(uint32_t part,bool solo)
    {impl_->liveEdit("parts.solo","{\"part\":"+std::to_string(part)+",\"solo\":"+(solo?"true":"false")+'}',"solo:"+std::to_string(part),[=](GraphState& g){for(auto& p:g.parts)if(p.id==part)p.solo=solo;});}
    namespace { std::vector<PartMeter> partMetersFrom(V r)
    {
        std::vector<PartMeter> out;
        for(auto m:array(field(r,"meters")))
        {
            PartMeter meter{static_cast<uint32_t>(integer(field(m,"part"))),number(field(m,"peak")),{},uint64_t(integer(field(m,"stolen"))),uint64_t(integer(field(m,"hardChokes")))};
            for(auto v:array(field(m,"channels")))meter.channels.push_back(number(v));
            out.push_back(std::move(meter));
        }
        return out;
    }
    std::vector<WireMeter> wireMetersFrom(V r)
    {
        std::vector<WireMeter> out;
        for(auto m:array(field(r,"meters")))
        {
            WireMeter meter{static_cast<uint32_t>(integer(field(m,"wire"))),number(field(m,"peak")),{}};
            for(auto v:array(field(m,"channels")))meter.channels.push_back(number(v));
            out.push_back(std::move(meter));
        }
        return out;
    } }
    std::vector<PartMeter> RemoteControl::partMeters() const {auto reply=impl_->call("parts.meters");return partMetersFrom(field(reply.get(),"result"));}
    std::vector<WireMeter> RemoteControl::wireMeters() const {auto reply=impl_->call("wires.meters");return wireMetersFrom(field(reply.get(),"result"));}
    bool RemoteControl::fetchPartMeters(std::function<void(std::vector<PartMeter>)> done){return impl_->fetch("parts.meters","parts.meters","{}",[done=std::move(done)](V r){done(partMetersFrom(r));});}
    bool RemoteControl::fetchWireMeters(std::function<void(std::vector<WireMeter>)> done){return impl_->fetch("wires.meters","wires.meters","{}",[done=std::move(done)](V r){done(wireMetersFrom(r));});}
    bool RemoteControl::fetchHistory(std::function<void(History)> done){return impl_->fetch("edit.history","edit.history","{}",[done=std::move(done)](V r){done(historyFrom(r));});}
    bool RemoteControl::fetchAssignments(std::function<void(std::vector<AssignmentOverride>)> done){return impl_->fetch("assignments.get","assignments.get","{}",[done=std::move(done)](V r){done(assignmentsFrom(r));});}
    bool RemoteControl::fetchTakes(std::function<void(std::vector<Take>)> done){return impl_->fetch("record.takes","record.takes","{}",[done=std::move(done)](V r){done(takesFrom(r));});}
    bool RemoteControl::fetchPerformanceInfo(std::function<void(PerformanceInfo)> done){return impl_->fetch("performance.info","performance.info","{}",[done=std::move(done)](V r){done(performanceFrom(r));});}
    bool RemoteControl::fetchAudition(std::function<void(Audition)> done){return impl_->fetch("audition.get","audition.get","{}",[done=std::move(done)](V r){done(auditionFrom(r));});}
    bool RemoteControl::fetchPluginWindow(NodeId node,std::function<void(bool)> done)
    {const auto params="{\"node\":"+std::to_string(node)+'}';return impl_->fetch("window.info"+params,"window.info",params,[done=std::move(done)](V r){done(boolean(field(r,"available")));});}
    bool RemoteControl::reading()const{return impl_->reading.has_value();}
    void RemoteControl::forgetFetches(){auto& i=*impl_;for(const auto& [id,f]:i.fetches)i.dropped.insert(id);i.fetches.clear();i.fetching.clear();}
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
    void RemoteControl::commit()
    {
        // After live edits alone the engine compiles nothing; what changes reaches the cache as
        // controls.changed, or graph.changed (a mute), so it is sent without waiting or reading.
        // Other edits read the graph back, so staged says whether any wait for this commit.
        const bool live=std::exchange(impl_->live,false) && !impl_->graph.staged;
        if(live){impl_->client.send("graph.commit");return;}
        impl_->call("graph.commit");impl_->refresh();
    }
    std::string RemoteControl::takeError(){return std::exchange(impl_->failure,{});}
    uint64_t RemoteControl::changeSequence()const{return impl_->sequence;}
    Control::Changes RemoteControl::changesAfter(uint64_t seq)const
    {Changes result{impl_->sequence,seq>impl_->sequence||(!impl_->history.empty()&&seq<impl_->history.front().sequence-1),{}};for(const auto& event:impl_->history)if(event.sequence>seq)result.events.push_back(event);return result;}
    bool RemoteControl::service()
    {
        impl_->client.poll();bool refresh=false,resync=false,read=false;
        for(const auto& message:impl_->client.takeNotifications())
        {
            auto doc=parse(message);
            if(const auto id=detail::member(doc.get(),"id"))
            {
                const auto n=integer(id);impl_->answered(n);
                if(impl_->dropped.erase(n))continue;
                if(impl_->reading && !impl_->reading->sent.empty() && impl_->reading->sent.front().first==n){read=impl_->reply(doc,message.size()) || read;continue;}
                if(const auto f=impl_->fetches.find(n);f!=impl_->fetches.end())
                {
                    auto fetched=std::move(f->second);impl_->fetches.erase(f);impl_->fetching.erase(fetched.key);
                    // A read refused, or a reply not understood, gives nothing: the view keeps what it showed, as when a call failed.
                    try{Impl::check(doc);fetched.read(field(doc.get(),"result"));}catch(const std::exception&){}
                    continue;
                }
                if(const auto v=impl_->valueReplies.find(n);v!=impl_->valueReplies.end())
                {
                    const auto key=v->second;impl_->valueReplies.erase(v);
                    const auto sent=impl_->sentValues.find(key);const bool newest=sent!=impl_->sentValues.end() && sent->second.first==n;
                    if(newest)impl_->sentValues.erase(sent);
                    try
                    {
                        Impl::check(doc);
                        if(newest && impl_->values.contains(key))impl_->values[key]=number(field(array(field(field(doc.get(),"result"),"values")).at(0),"value"));
                    }
                    catch(const std::exception& e){impl_->failure=e.what();refresh=true;}
                    continue;
                }
                // The reply to a live edit sent without waiting: a refusal is said, and the
                // engine's state read again in place of what the cache showed.
                try{Impl::check(doc);}catch(const std::exception& e){impl_->failure=e.what();impl_->refused(n);refresh=true;}
                continue;
            }
            const auto method=string(field(doc.get(),"method"));auto params=field(doc.get(),"params");
            if(method=="transport")impl_->readTransport(params);
            else if(method=="status")impl_->readStatus(params);
            else if(method=="controllers")impl_->readControllers(params);
            else if(method=="controls.changed")
            {
                // Values pushed as they change: the cached graph takes them, with no graph read, and
                // so does a read out whose graph came before them, or it would show older ones.
                auto take=[&](GraphState& g){
                    for(auto c:array(field(params,"controls"))){const auto id=static_cast<uint32_t>(integer(field(c,"control")));for(auto& m:g.macros)if(m.id==id)m.value=number(field(c,"value"));}
                    for(auto f:array(field(params,"parts"))){const auto id=static_cast<uint32_t>(integer(field(f,"part")));for(auto& p:g.parts)if(p.id==id){p.level=number(field(f,"level"));p.mute=boolean(field(f,"mute"));p.pan=number(field(f,"pan"));}}
                    V master=nullptr;try{master=field(params,"master");}catch(const std::runtime_error&){}
                    if(master){g.masterLevel=number(field(master,"level"));g.masterPan=number(field(master,"pan"));}
                };
                for(auto f:array(field(params,"parts"))){const auto part=std::to_string(integer(field(f,"part")));impl_->pushed("level:"+part);impl_->pushed("pan:"+part);}
                if(detail::member(params,"master"))impl_->pushed("master");
                take(impl_->graph);if(impl_->reading && impl_->reading->graphRead){take(impl_->reading->next);Impl::follow(impl_->reading->next);}
                impl_->overlay();Impl::follow(impl_->graph);
            }
            else if(method=="graph.changed"){refresh=true;impl_->publish({0,ChangeKind::Graph});}
            else if(method=="session.resync"){refresh=resync=true;impl_->publish({0,ChangeKind::Resync});}
            else if(method=="params.changed")for(auto c:array(field(params,"changes")))
            {
                Change change;change.node=static_cast<NodeId>(integer(field(c,"node")));change.param=static_cast<uint32_t>(integer(field(c,"param")));
                const auto kind=string(field(c,"kind"));change.kind=kind=="value"?ChangeKind::Value:kind=="begin"?ChangeKind::GestureBegin:ChangeKind::GestureEnd;
                change.plugin=string(field(c,"source"))=="plugin";
                if(change.kind==ChangeKind::Value)
                {
                    change.value=number(field(c,"value"));const std::pair key{change.node,change.param};
                    if(impl_->values.contains(key) && !impl_->sentValues.contains(key))impl_->values[key]=change.value; // a newer one sent stays
                    // A read out that has this value already read it before the change.
                    if(impl_->reading)if(const auto at=impl_->reading->snapshot.find(key);at!=impl_->reading->snapshot.end())at->second=change.value;
                }
                impl_->publish(change);
            }
        }
        // Read without waiting: the cache shows the graph read once all its replies are in.
        if(refresh)impl_->refreshLater();
        if(resync)
        {
            // Transport and status notifications are sent only on change; reread any that were lost.
            auto& i=*impl_;
            i.fetch("transport.get","transport.get","{}",[&i](V r){i.readTransport(r);});
            i.fetch("session.status","session.status","{}",[&i](V r){i.readStatus(r);});
            i.fetch("controllers.get","controllers.get","{}",[&i](V r){i.readControllers(r);});
        }
        return read;
    }
}

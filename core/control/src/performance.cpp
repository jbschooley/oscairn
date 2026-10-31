// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <locale>
#include <set>
#include <sstream>
#include <tuple>

namespace oscairn {
namespace {
using V=const json_value_s*;
namespace fs=std::filesystem;
[[noreturn]] void bad(const std::string& why){throw std::runtime_error("Performance: "+why);}
std::string text(V v){if(!v || v->type!=json_type_string)bad("expected string");const auto* s=static_cast<const json_string_s*>(v->payload);std::string out(s->string,s->string_size);if(out.size()>4096 || out.find('\0')!=std::string::npos)bad("invalid string");return out;}
V member(V v,std::string_view key){return detail::member(v,key);}
V field(V v,std::string_view key){if(!v || v->type!=json_type_object)bad("expected object");if(V value=detail::member(v,key))return value;bad("missing field "+std::string(key));}
void fields(V v,std::initializer_list<std::string_view> allowed){if(!v || v->type!=json_type_object)bad("expected object");std::set<std::string_view> seen;for(auto* e=static_cast<const json_object_s*>(v->payload)->start;e;e=e->next){std::string_view key(e->name->string,e->name->string_size);if(!seen.insert(key).second || std::find(allowed.begin(),allowed.end(),key)==allowed.end())bad("unknown or duplicate field");}}
std::vector<V> array(V v,size_t limit){if(!v || v->type!=json_type_array)bad("expected array");const auto* a=static_cast<const json_array_s*>(v->payload);if(a->length>limit)bad("too many entries");std::vector<V> out;for(auto* e=a->start;e;e=e->next)out.push_back(e->value);return out;}
double number(V v){if(!v || v->type!=json_type_number)bad("expected number");const auto* n=static_cast<const json_number_s*>(v->payload);std::istringstream in(std::string(n->number,n->number_size));in.imbue(std::locale::classic());double value;if(!(in>>value) || !in.eof() || !std::isfinite(value))bad("invalid number");return value;}
bool boolean(V v){if(!v || (v->type!=json_type_true && v->type!=json_type_false))bad("expected boolean");return v->type==json_type_true;}
uint32_t integer(V v,uint32_t max=UINT32_MAX){const auto n=number(v);if(n<0 || n>max || n!=std::floor(n))bad("invalid integer");return static_cast<uint32_t>(n);}
std::string quote(std::string_view s){if(s.size()>4096 || s.find('\0')!=std::string_view::npos)bad("invalid string");return detail::quote(s);}
std::string numeric(double value){if(!std::isfinite(value))bad("nonfinite value");char buffer[64];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);if(result.ec!=std::errc{})bad("number encoding failed");return {buffer,result.ptr};}
std::string join(const std::vector<std::string>& values){std::string out="[";for(const auto& v:values){if(out.size()>1)out+=',';out+=v;}return out+']';}
// RFC 0011 note zones, as the control protocol writes them.
std::string zonesText(const std::vector<NoteZone>& zones){
    std::vector<std::string> out;
    for(const auto& z:zones)out.push_back("{\"keys\":["+std::to_string(z.lowKey)+","+std::to_string(z.highKey)+"],\"velocities\":["+numeric(z.lowVelocity)+","+numeric(z.highVelocity)+"],\"channels\":"+std::to_string(z.channels)+",\"transpose\":"+std::to_string(z.transpose)+",\"curve\":"+numeric(z.curve)+",\"fixedVelocity\":"+numeric(z.fixedVelocity)+'}');
    return join(out);
}
std::vector<NoteZone> zonesFrom(V v){
    std::vector<NoteZone> zones;
    for(auto z:array(v,NoteZones::capacity)){
        fields(z,{"keys","velocities","channels","transpose","curve","fixedVelocity"});NoteZone zone;
        const auto keys=array(field(z,"keys"),2),velocities=array(field(z,"velocities"),2);if(keys.size()!=2 || velocities.size()!=2)bad("invalid zone range");
        zone.lowKey=uint8_t(integer(keys[0],127));zone.highKey=uint8_t(integer(keys[1],127));zone.lowVelocity=number(velocities[0]);zone.highVelocity=number(velocities[1]);
        zone.channels=uint16_t(integer(field(z,"channels"),0xFFFF));const auto t=number(field(z,"transpose"));if(t<-127 || t>127 || t!=std::floor(t))bad("invalid zone transpose");zone.transpose=int8_t(t);
        zone.curve=number(field(z,"curve"));zone.fixedVelocity=number(field(z,"fixedVelocity"));if(!zone.valid())bad("invalid zone");zones.push_back(zone);
    }
    return zones;
}
bool lv2Path(const std::string& path){auto p=fs::path(path).lexically_normal();if(p.filename().empty())p=p.parent_path();return p.extension()==".lv2";}
std::string ports(const std::vector<kick::clap::Port>& list){std::vector<std::string> out;for(const auto& p:list)out.push_back("{\"id\":"+std::to_string(p.id)+",\"channels\":"+std::to_string(p.channels)+",\"flags\":"+std::to_string(p.flags)+'}');return join(out);}
void checkPorts(V list,const std::vector<kick::clap::Port>& expected){const auto entries=array(list,Graph::maxAudioChannels);if(entries.size()!=expected.size())bad("audio port layout changed");for(size_t i=0;i<entries.size();++i){fields(entries[i],{"id","channels","flags"});if(integer(field(entries[i],"id"))!=expected[i].id || integer(field(entries[i],"channels"))!=expected[i].channels || integer(field(entries[i],"flags"))!=expected[i].flags)bad("audio port layout changed");}}
void validateTree(const fs::path& root){size_t files=0;uint64_t bytes=0;for(const auto& e:fs::recursive_directory_iterator(root)){if(++files>65536 || e.is_symlink() || (!e.is_directory() && !e.is_regular_file()))bad("invalid or oversized asset tree");if(e.is_regular_file()){const auto n=e.file_size();if(n>16ull*1024*1024*1024-bytes)bad("assets exceed 16 GiB");bytes+=n;}}}
std::unique_ptr<json_value_s,decltype(&std::free)> document(const fs::path& root){const auto bytes=state_files::read(root/"performance.json",4*1024*1024);const auto error=detail::jsonPreflight({reinterpret_cast<const char*>(bytes.data()),bytes.size()},32);if(!error.empty())bad(std::string(error));auto* doc=json_parse(bytes.data(),bytes.size());if(!doc)bad("invalid JSON");return {doc,&std::free};}
}

void Session::savePerformance(const std::string& directory) {
    if(staged_)bad("commit staged graph edits before saving");
    service();if(!pending_.empty())bad("parameter edits are still pending; retry after processing");
    const auto graph=graph_.describe();if(graph.nodes.size()>256 || graph.notes.size()>4096 || graph.mappings.size()>4096)bad("graph exceeds Performance limits");
    state_files::Bundle bundle(directory);fs::create_directory(bundle.temporary.path/"nodes");
    std::map<NodeId,std::string> keys{{graphInput,"input"},{graphOutput,"output"}};
    for(size_t i=0;i<graph.nodes.size();++i)keys[graph.nodes[i].id]="node-"+std::to_string(i+1);
    std::vector<std::string> nodes,wires,parts,mappings;std::set<std::string> libraries;
    for(const auto& n:graph.nodes){const auto& key=keys.at(n.id);auto& h=graph_.host(n.id);
        // Sound assets are dependencies (RFC 0009): record references, never the files.
        std::vector<std::string> assets;
        for(const auto& a:h.assetDependencies()){libraries.insert(a.library);assets.push_back("{\"library\":"+quote(a.library)+",\"path\":"+quote(a.path)+",\"sha256\":"+quote(a.sha256)+'}');}
        try{h.saveSnapshot((bundle.temporary.path/"nodes"/key).string());}catch(const std::exception& e){bad("cannot save "+nodes_.at(n.id)+" ("+n.plugin.id+"): "+e.what());}
        const bool lv2=lv2Path(n.plugin.path);
        nodes.push_back("{\"key\":"+quote(key)+",\"name\":"+quote(nodes_.at(n.id))+",\"plugin\":{\"format\":"+quote(lv2?"lv2":"clap")+",\"id\":"+quote(n.plugin.id)+",\"version\":"+quote(n.plugin.version)+",\"pathHint\":"+quote(n.plugin.path)+"},\"engine\":"+quote(keys.at(graph_.effectSource(n.id)))+",\"inputs\":"+ports(n.inputs)+",\"outputs\":"+ports(n.outputs)+",\"noteInputs\":"+std::to_string(n.noteInputs)+",\"noteOutputs\":"+std::to_string(n.noteOutputs)+",\"assets\":"+join(assets)+'}');
    }
    // RFC 0011: wires with their owners and Mix settings, and the parts; graph routes follow from them.
    if(wires_.size()>Graph::maxAudioConnections || parts_.size()>256)bad("wires or parts exceed Performance limits");
    for(const auto& w:wires_){
        std::vector<std::string> pairs;for(const auto& [a,b]:w.channels)pairs.push_back("["+std::to_string(a)+","+std::to_string(b)+"]");
        wires.push_back(std::string("{\"kind\":")+(w.kind==WireKind::Audio?"\"audio\"":"\"notes\"")+",\"owner\":"+(w.owner==WireOwner::Part?"\"part\",\"part\":"+std::to_string(w.part):"\"user\"")
            +",\"from\":"+quote(keys.at(w.from))+",\"to\":"+quote(keys.at(w.to))+",\"fromPort\":"+std::to_string(w.fromPort)+",\"toPort\":"+std::to_string(w.toPort)
            +",\"channels\":"+join(pairs)+",\"level\":"+numeric(w.level)+",\"pan\":"+numeric(w.pan)+",\"mute\":"+(w.mute?"true":"false")+",\"solo\":"+(w.solo?"true":"false")+",\"zones\":"+zonesText(w.zones)+",\"send\":"+std::to_string(w.send)+'}');
    }
    for(const auto& p:parts_){
        std::vector<std::string> chain;for(const auto n:p.chain)chain.push_back(quote(keys.at(n)));
        const char* kind=p.input.kind==PartInput::Kind::Notes?"notes":p.input.kind==PartInput::Kind::Audio?"audio":p.input.kind==PartInput::Kind::Bus?"bus":"none";
        std::vector<std::string> sends;for(const auto& s:p.sends)sends.push_back("{\"bus\":"+std::to_string(s.bus)+",\"level\":"+numeric(s.level)+'}');
        parts.push_back("{\"id\":"+std::to_string(p.id)+",\"name\":"+quote(p.name)+",\"input\":{\"kind\":\""+kind+"\",\"port\":"+std::to_string(p.input.port)+",\"zones\":"+zonesText(p.input.zones)+"},\"chain\":"+join(chain)+",\"output\":"+std::to_string(p.output)+",\"bus\":"+std::to_string(p.bus)+",\"sends\":"+join(sends)+'}');
    }
    for(const auto& m:graph.mappings)mappings.push_back("{\"from\":"+quote(keys.at(m.source))+",\"lane\":"+std::to_string(m.lane)+",\"to\":"+quote(keys.at(m.target))+",\"param\":"+std::to_string(m.param)+'}');
    std::vector<std::string> named;
    for(const auto& l:assets_.libraries())if(libraries.count(l.id))named.push_back("{\"id\":"+quote(l.id)+",\"name\":"+quote(l.name)+",\"fingerprint\":"+quote(l.fingerprint)+'}');
    const auto out="{\"format\":\"oscairn.performance\",\"version\":2,\"libraries\":"+join(named)+",\"transport\":{\"tempo\":"+numeric(tempo_.load())+",\"clockSource\":"+quote(clockSource_.load()==Transport::ClockSource::Internal?"internal":"midi")+"},\"audioInputs\":"+ports(graph.audioLayout.inputs)+",\"audioOutputs\":"+ports(graph.audioLayout.outputs)+",\"nodes\":"+join(nodes)+",\"wires\":"+join(wires)+",\"parts\":"+join(parts)+",\"mappings\":"+join(mappings)+"}\n";
    if(out.size()>4*1024*1024)bad("manifest exceeds size limit");
    const auto encoding=detail::jsonPreflight(out,32);if(!encoding.empty())bad(std::string(encoding));
    state_files::write(bundle.temporary.path/"performance.json",out.data(),out.size());validateTree(bundle.temporary.path);bundle.publish();
}

void Session::loadPerformance(const std::string& directory) {
    if(directory.empty() || directory.find('\0')!=std::string::npos)bad("invalid directory");
    const auto root=fs::canonical(directory);validateTree(root);const auto doc=document(root);V d=doc.get();
    fields(d,{"format","version","libraries","transport","audioInputs","audioOutputs","nodes","wires","parts","mappings"});
    if(text(field(d,"format"))!="oscairn.performance" || integer(field(d,"version"))!=2)bad("unsupported format version");
    const auto transport=field(d,"transport");fields(transport,{"tempo","clockSource"});const double tempo=number(field(transport,"tempo"));
    const auto clock=text(field(transport,"clockSource"));if(tempo<1 || tempo>1000 || (clock!="internal" && clock!="midi"))bad("invalid transport");
    const auto layout=graph_.describe().audioLayout;checkPorts(field(d,"audioInputs"),layout.inputs);checkPorts(field(d,"audioOutputs"),layout.outputs);
    // Every sound asset must resolve before any plugin loads; report all that do not.
    {
        std::map<std::string,std::string> names;
        if(member(d,"libraries"))for(auto l:array(field(d,"libraries"),4096)){fields(l,{"id","name","fingerprint"});names[text(field(l,"id"))]=text(field(l,"name"));}
        std::vector<std::string> missing;
        for(auto node:array(field(d,"nodes"),256)){
            if(!member(node,"assets"))continue;
            std::map<std::string,std::pair<size_t,std::string>> unavailable;size_t total=0;
            for(auto a:array(field(node,"assets"),65536)){fields(a,{"library","path","sha256"});++total;
                assets::Ref ref{text(field(a,"library")),text(field(a,"path")),text(field(a,"sha256"))};const auto library=ref.library;
                try{assets_.resolve(ref);}catch(const std::exception& e){auto& u=unavailable[library];if(!u.first++)u.second=e.what();}}
            for(const auto& [library,u]:unavailable)
                missing.push_back(text(field(node,"name"))+": "+std::to_string(u.first)+" of "+std::to_string(total)+" files from library '"+(names.count(library)?names[library]:library)+"' unavailable, e.g. "+u.second);
        }
        if(!missing.empty()){std::string all;for(const auto& m:missing)all+=(all.empty()?"":"; ")+m;bad("missing sound assets: "+all);}
    }
    auto next=graph_.prepareReplacement();std::map<std::string,NodeId> ids{{"input",graphInput},{"output",graphOutput}};std::map<NodeId,std::string> names;std::set<std::string> usedNames;
    std::map<std::string,V> definitions;const auto nodes=array(field(d,"nodes"),256);
    for(auto node:nodes){fields(node,{"key","name","plugin","engine","inputs","outputs","noteInputs","noteOutputs","assets"});const auto key=text(field(node,"key"));
        if(key.size()<6 || key.size()>12 || key.substr(0,5)!="node-" || !std::all_of(key.begin()+5,key.end(),[](char c){return c>='0' && c<='9';}) || !definitions.emplace(key,node).second)bad("invalid or duplicate node key");
        const auto name=text(field(node,"name"));if(!name.empty() && !usedNames.insert(name).second)bad("duplicate node name");}
    // Roots first; siblings can refer forward to a root, never to an arbitrary chain.
    for(bool siblings:{false,true})for(auto node:nodes){const auto key=text(field(node,"key")),engine=text(field(node,"engine"));if((key!=engine)!=siblings)continue;
        const auto plugin=field(node,"plugin");fields(plugin,{"format","id","version","pathHint"});const auto format=text(field(plugin,"format")),id=text(field(plugin,"id")),path=text(field(plugin,"pathHint"));
        if((format!="clap" && format!="lv2") || id.empty() || path.empty() || (lv2Path(path)!=(format=="lv2")))bad("invalid plugin identity or format");
        const auto name=text(field(node,"name"));
        try {
            NodeId runtime;
            if(siblings){if(!definitions.contains(engine) || text(field(definitions.at(engine),"engine"))!=engine || !ids.contains(engine))bad("missing shared engine root");
                const auto& source=next->host(ids.at(engine)).descriptor();if(source.id!=id || source.path!=path)bad("incompatible shared engine identity");runtime=next->addEffectContext(ids.at(engine));}
            else runtime=next->add(path,id);
            auto& h=next->host(runtime);if(h.descriptor().version!=text(field(plugin,"version")))bad("saved plugin version does not match the loaded plugin");
            h.loadSnapshot((root/"nodes"/key).string());
            checkPorts(field(node,"inputs"),h.inputPorts());checkPorts(field(node,"outputs"),h.outputPorts());
            if(integer(field(node,"noteInputs"),16)!=h.noteInputPorts() || integer(field(node,"noteOutputs"),16)!=h.noteOutputPorts())bad("note port layout changed");
            ids.emplace(key,runtime);names.emplace(runtime,name);
        }catch(const std::exception& e){bad("cannot restore "+name+" ("+id+"): "+e.what());}
    }
    auto node=[&](V v){const auto key=text(v);const auto it=ids.find(key);if(it==ids.end())bad("unknown node reference "+key);return it->second;};
    std::vector<Wire> wires;std::vector<Part> parts;uint32_t nextPart=1;
    {
        std::set<uint32_t> partIds;
        for(auto p:array(field(d,"parts"),256)){
            fields(p,{"id","name","input","chain","output","bus","sends"});Part part;part.id=integer(field(p,"id"));part.name=text(field(p,"name"));
            if(!part.id || !partIds.insert(part.id).second)bad("invalid or duplicate part");
            const auto input=field(p,"input");fields(input,{"kind","port","zones"});const auto kind=text(field(input,"kind"));
            part.input.kind=kind=="notes"?PartInput::Kind::Notes:kind=="audio"?PartInput::Kind::Audio:kind=="none"?PartInput::Kind::None:kind=="bus"?PartInput::Kind::Bus:(bad("invalid part input"),PartInput::Kind::None);
            part.input.port=integer(field(input,"port"));part.input.zones=zonesFrom(field(input,"zones"));part.output=integer(field(p,"output"));
            for(auto n:array(field(p,"chain"),256)){const auto id=node(n);if(id==graphInput || id==graphOutput || std::find(part.chain.begin(),part.chain.end(),id)!=part.chain.end())bad("invalid part chain");part.chain.push_back(id);}
            part.bus=integer(field(p,"bus"));
            for(auto s:array(field(p,"sends"),256)){fields(s,{"bus","level"});Send send{integer(field(s,"bus")),number(field(s,"level"))};if(send.level<0)bad("invalid send level");part.sends.push_back(send);}
            nextPart=std::max(nextPart,part.id+1);parts.push_back(std::move(part));
        }
        // Buses named by outputs and sends must be buses of this Performance, without loops.
        auto busOf=[&](uint32_t id)->const Part&{for(const auto& p:parts)if(p.id==id && p.input.kind==PartInput::Kind::Bus)return p;bad("unknown bus");};
        for(const auto& p:parts){if(p.bus)busOf(p.bus);for(const auto& s:p.sends)busOf(s.bus);}
        std::function<bool(uint32_t,uint32_t,size_t)> reaches=[&](uint32_t from,uint32_t to,size_t depth){
            if(depth>parts.size())return true;
            for(const auto& p:parts)if(p.id==from){if(p.bus==to || (p.bus && reaches(p.bus,to,depth+1)))return true;for(const auto& s:p.sends)if(s.bus==to || reaches(s.bus,to,depth+1))return true;}
            return false;};
        for(const auto& p:parts)if(reaches(p.id,p.id,0))bad("a bus loops into itself");
        for(auto c:array(field(d,"wires"),Graph::maxAudioConnections)){
            Wire w;const auto kind=text(field(c,"kind")),owner=text(field(c,"owner"));
            if((kind!="audio" && kind!="notes") || (owner!="part" && owner!="user"))bad("invalid wire");
            if(owner=="part")fields(c,{"kind","owner","part","from","to","fromPort","toPort","channels","level","pan","mute","solo","zones","send"});
            else fields(c,{"kind","owner","from","to","fromPort","toPort","channels","level","pan","mute","solo","zones","send"});
            w.kind=kind=="audio"?WireKind::Audio:WireKind::Notes;w.owner=owner=="part"?WireOwner::Part:WireOwner::User;
            if(owner=="part"){w.part=integer(field(c,"part"));if(!partIds.count(w.part))bad("wire of an unknown part");}
            w.from=node(field(c,"from"));w.to=node(field(c,"to"));w.fromPort=integer(field(c,"fromPort"));w.toPort=integer(field(c,"toPort"));
            for(auto pair:array(field(c,"channels"),Graph::maxAudioChannels)){const auto ends=array(pair,2);if(ends.size()!=2)bad("invalid channel pair");w.channels.push_back({integer(ends[0],Graph::maxAudioChannels-1),integer(ends[1],Graph::maxAudioChannels-1)});}
            w.level=number(field(c,"level"));w.pan=number(field(c,"pan"));w.mute=boolean(field(c,"mute"));w.solo=boolean(field(c,"solo"));
            if(w.level<0 || w.pan<-1 || w.pan>1)bad("invalid wire level or pan");
            w.zones=zonesFrom(field(c,"zones"));if(w.kind==WireKind::Audio && !w.zones.empty())bad("audio wires have no zones");
            w.send=integer(field(c,"send"));if(w.send && (w.owner!=WireOwner::Part || !std::any_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==w.send;})))bad("a send wire of an unknown bus");
            wires.push_back(std::move(w));
        }
    }
    uint32_t nextWire=1;for(auto& w:wires)w.id=nextWire++;
    compileWires(*next,wires);
    std::set<std::pair<NodeId,uint32_t>> mappings;
    for(auto m:array(field(d,"mappings"),4096)){fields(m,{"from","lane","to","param"});const auto from=node(field(m,"from")),lane=integer(field(m,"lane"));if(!mappings.emplace(from,lane).second)bad("duplicate parameter mapping");next->mapParamTarget(from,lane,node(field(m,"to")),integer(field(m,"param")));}
    std::map<Address,double> observed;for(const auto& [id,name]:names)for(const auto& p:next->host(id).parameters())observed[{id,p.id}]=next->host(id).parameterValue(p.id);
    // Compilation and all allocating preparation precede publication. The audio thread
    // adopts at one boundary; old plans and their hosts are reclaimed by service().
    auto history=history_;
    if(history.size()==historyCapacity)history.pop_front();
    history.push_back({sequence_+1,ChangeKind::Resync,0,0,0,false});
    graph_.publishReplacement(*next,noteHead_.load(std::memory_order_acquire),[&]() noexcept {
        nodes_.swap(names);observed_.swap(observed);parts_.swap(parts);wires_.swap(wires);nextPart_=nextPart;nextWire_=nextWire;pending_.clear();commandCount_=0;outputDrops_.clear();staged_=false;
        tempo_.store(tempo);clockSource_.store(clock=="internal"?Transport::ClockSource::Internal:Transport::ClockSource::MidiClock);playing_.store(false);
        ++revision_;++sequence_;history_.swap(history);
    });
}
}

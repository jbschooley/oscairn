// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "CurveJson.h"
#include "oscairn/package.h"
#include "oscairn/unavailable_plugin.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <functional>
#include <locale>
#include <optional>
#include <random>
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
// A scene's scope: the names of the kinds it applies (sceneKinds).
uint32_t sceneScope(V v){
    uint32_t scope=0;
    for(auto k:array(v,sceneKinds.size())){const auto name=text(k);const auto at=std::find(sceneKinds.begin(),sceneKinds.end(),name);if(at==sceneKinds.end())bad("unknown scene kind "+name);scope|=1u<<(at-sceneKinds.begin());}
    return scope;
}
// How a scene moves there: its glide (at most a minute), curve and launch.
void sceneTransition(V s,Scene& scene){
    scene.glide=integer(field(s,"glide"));if(scene.glide>60000)bad("a scene's glide is at most a minute");
    auto pick=[&](const char* key,const auto& names){const auto name=text(field(s,key));const auto at=std::find(names.begin(),names.end(),name);if(at==names.end())bad("invalid scene "+std::string(key));return int(at-names.begin());};
    scene.curve=Scene::Curve(pick("curve",sceneCurves));scene.launch=Scene::Launch(pick("launch",sceneLaunches));
}
// A wire as a scene names it, its ends through `node` (a key to a node).
template<class Node> WireRef wireRef(V r,Node&& node){
    const auto kind=text(field(r,"kind"));if(kind!="audio" && kind!="notes")bad("invalid scene wire kind");
    return {kind=="audio"?WireKind::Audio:WireKind::Notes,integer(field(r,"part")),node(field(r,"from")),node(field(r,"to")),integer(field(r,"fromPort")),integer(field(r,"toPort")),
            integer(field(r,"send")),integer(field(r,"split")),integer(field(r,"lane")),boolean(field(r,"fader"))};
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
// RFC 0012 device selectors on a part's input.
std::string devicesText(const std::vector<DeviceRule>& rules){
    std::vector<std::string> out;
    for(const auto& r:rules)out.push_back(std::string("{\"kind\":\"")+deviceRuleKinds[size_t(r.kind)]+"\",\"port\":"+std::to_string(r.port)+",\"pattern\":"+quote(r.pattern)+",\"match\":\""+deviceMatches[size_t(r.match)]+"\"}");
    return join(out);
}
template<class Names> size_t named(const Names& names,const std::string& name,const char* error){
    const auto at=std::find_if(names.begin(),names.end(),[&](const char* n){return name==n;});
    if(at==names.end())bad(error);
    return size_t(at-names.begin());
}
std::vector<DeviceRule> devicesFrom(V v){
    std::vector<DeviceRule> rules;
    for(auto r:array(v,16)){
        fields(r,{"kind","port","pattern","match"});DeviceRule rule;
        rule.kind=DeviceRule::Kind(named(deviceRuleKinds,text(field(r,"kind")),"invalid device rule kind"));rule.port=integer(field(r,"port"));
        rule.pattern=text(field(r,"pattern"));rule.match=DeviceRule::Match(named(deviceMatches,text(field(r,"match")),"invalid device rule match"));
        if(!rule.valid())bad("invalid device rule");rules.push_back(std::move(rule));
    }
    return rules;
}
// RFC 0012 step 1: a part's MIDI out, its output selector and channel.
std::string midiOutText(const PartMidiOut& m){return "{\"devices\":"+devicesText(m.devices)+",\"channel\":"+std::to_string(m.channel)+'}';}
PartMidiOut midiOutFrom(V v){fields(v,{"devices","channel"});return {devicesFrom(field(v,"devices")),uint8_t(integer(field(v,"channel"),16))};}
// RFC 0033: a key, its root and its scale's pitch classes above the root, ascending from 0.
// The Performance's level and pan on its main output, top-level and in each scene.
std::string masterText(double level,double pan){return "{\"level\":"+numeric(level)+",\"pan\":"+numeric(pan)+'}';}
std::pair<double,double> masterFrom(V v){
    fields(v,{"level","pan"});const double level=number(field(v,"level")),pan=number(field(v,"pan"));
    if(level<0 || level>2 || pan<-1 || pan>1)bad("invalid master level or pan");return {level,pan};}
// RFC 0016: a node's role, or empty when it has none.
std::string roleFrom(V node){const V r=member(node,"role");if(!r)return {};const auto role=text(r);if(!validNodeRole(role))bad("invalid node role");return role;}
std::string keyText(const MusicalKey& k){
    std::vector<std::string> steps;for(int i=0;i<12;++i)if(k.scale>>i&1)steps.push_back(std::to_string(i));
    return "{\"root\":"+std::to_string(k.root)+",\"scale\":"+join(steps)+'}';
}
MusicalKey keyFrom(V v){
    fields(v,{"root","scale"});MusicalKey k{uint8_t(integer(field(v,"root"),11)),0};
    int last=-1;for(auto s:array(field(v,"scale"),12)){const int i=int(integer(s,11));if(i<=last)bad("a scale's pitch classes ascend");last=i;k.scale|=uint16_t(1u<<i);}
    if(!k.valid())bad("a scale starts at its root, 0");
    return k;
}
// The pad map (pads.h): the selected bank and every bank's 16 pads.
const char* pressureName(PadAssignment::Pressure p){return p==PadAssignment::Pressure::Poly?"poly":p==PadAssignment::Pressure::Channel?"channel":"none";}
std::string padsText(const PadMap& map){
    std::vector<std::string> banks;
    for(const auto& bank:map.assignments){std::vector<std::string> pads;for(const auto& a:bank)pads.push_back("{\"note\":"+std::to_string(a.note)+",\"channel\":"+std::to_string(a.channel)+",\"velocity\":"+numeric(a.velocity)+",\"pressure\":\""+pressureName(a.pressure)+"\"}");banks.push_back(join(pads));}
    return "{\"bank\":"+std::to_string(map.bank)+",\"banks\":"+join(banks)+'}';
}
PadMap padsFrom(V v){
    fields(v,{"bank","banks"});PadMap map;map.bank=uint8_t(integer(field(v,"bank"),PadMap::banks-1));
    const auto banks=array(field(v,"banks"),PadMap::banks);if(banks.size()!=PadMap::banks)bad("the pad map has eight banks");
    for(size_t b=0;b<PadMap::banks;++b){
        const auto pads=array(banks[b],PadMap::pads);if(pads.size()!=PadMap::pads)bad("a pad bank has 16 pads");
        for(size_t i=0;i<PadMap::pads;++i){
            fields(pads[i],{"note","channel","velocity","pressure"});auto& a=map.assignments[b][i];
            a.note=uint8_t(integer(field(pads[i],"note"),127));const auto c=number(field(pads[i],"channel"));if(c<-1 || c>15 || c!=std::floor(c))bad("invalid pad channel");a.channel=int8_t(c);
            a.velocity=number(field(pads[i],"velocity"));const auto p=text(field(pads[i],"pressure"));
            a.pressure=p=="poly"?PadAssignment::Pressure::Poly:p=="channel"?PadAssignment::Pressure::Channel:p=="none"?PadAssignment::Pressure::None:(bad("invalid pad pressure"),PadAssignment::Pressure::None);
        }
    }
    if(!map.valid())bad("invalid pad map");
    return map;
}
bool lv2Path(const std::string& path){auto p=fs::path(path).lexically_normal();if(p.filename().empty())p=p.parent_path();return p.extension()==".lv2";}
// Each node's parameters the document names (mappings, controls, scenes, assignments), by node key: what a
// stand-in for a plugin that is not here must answer to.
std::map<std::string,std::set<uint32_t>> parameterRefs(V d){
    std::map<std::string,std::set<uint32_t>> refs;
    auto each=[&](V list,const char* node,const char* param){if(list && list->type==json_type_array)for(auto e=static_cast<const json_array_s*>(list->payload)->start;e;e=e->next)
        if(const V n=member(e->value,node),p=member(e->value,param);n && p && n->type==json_type_string && p->type==json_type_number)refs[text(n)].insert(uint32_t(std::strtoul(static_cast<const json_number_s*>(p->payload)->number,nullptr,10)));};
    each(member(d,"mappings"),"to","param");
    each(member(d,"sceneUntouched"),"node","param");
    if(const V scenes=member(d,"scenes");scenes && scenes->type==json_type_array)for(auto s=static_cast<const json_array_s*>(scenes->payload)->start;s;s=s->next)each(member(s->value,"values"),"node","param");
    if(const V controls=member(d,"controls");controls && controls->type==json_type_array)for(auto c=static_cast<const json_array_s*>(controls->payload)->start;c;c=c->next)each(member(c->value,"targets"),"node","param");
    each(member(d,"assignments"),"node","param");
    return refs;
}
// A document's lock (RFC 0006): each plugin build it was saved with, per platform.
std::vector<Session::LockEntry> readLock(V d){
    std::vector<Session::LockEntry> out;
    const V lock=field(d,"lock");fields(lock,{"plugins"});
    for(auto e:array(field(lock,"plugins"),4096)){fields(e,{"format","id","version","platform","sha256"});
        Session::LockEntry l{text(field(e,"format")),text(field(e,"id")),text(field(e,"version")),text(field(e,"platform")),text(field(e,"sha256"))};
        if((l.format!="clap" && l.format!="lv2") || l.id.empty() || l.platform.empty() || l.sha256.size()!=64)bad("invalid lock entry");
        out.push_back(std::move(l));}
    return out;
}
// The plugin store (RFC 0006): installed plugin builds by format, ID and version, apart from
// any one Performance; empty when that build is not there.
std::string storeName(const std::string& id,const std::string& version){
    std::string safe;for(const char c:id+"@"+version)safe+=(std::isalnum(static_cast<unsigned char>(c)) || c=='.' || c=='-' || c=='@' || c=='_')?c:'_';
    return safe;
}
fs::path storeFolder(const std::string& format,const std::string& id,const std::string& version){return dataHome()/"Plugins"/format/storeName(id,version);}
// A plugin's builds in the store by platform: <folder>/<platform>/<bundle>, beside plugin.json.
std::map<std::string,fs::path> storedBuilds(const std::string& format,const std::string& id,const std::string& version){
    std::map<std::string,fs::path> out;std::error_code e;
    const auto folder=storeFolder(format,id,version);
    if(fs::is_directory(folder,e))for(const auto& p:fs::directory_iterator(folder,e)){
        if(!p.is_directory() || p.path().filename().string().front()=='.')continue;
        for(const auto& b:fs::directory_iterator(p.path(),e))if(b.path().extension()==(format=="lv2"?".lv2":".clap")){out[p.path().filename().string()]=b.path();break;}
    }
    return out;
}
fs::path storedPlugin(const std::string& format,const std::string& id,const std::string& version){
    const auto builds=storedBuilds(format,id,version);const auto here=builds.find(platform());
    return here==builds.end()?fs::path():here->second;
}
// A store entry's plugin.json: the plugin's format, ID, version and name, as the folder's name
// cannot say them exactly. Empty ID when it cannot be read.
Control::InstalledPlugin storedIdentity(const fs::path& folder){
    Control::InstalledPlugin p;
    try{
        const auto bytes=state_files::read(folder/"plugin.json",64*1024);
        const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
        if(!doc)return p;
        p.format=text(field(doc.get(),"format"));p.id=text(field(doc.get(),"id"));p.version=text(field(doc.get(),"version"));p.name=text(field(doc.get(),"name"));
    }catch(const std::exception&){p.id.clear();}
    return p;
}
std::string storedName(const std::string& format,const std::string& id,const std::string& version){
    const auto p=storedIdentity(storeFolder(format,id,version));
    return p.id==id && !p.name.empty()?p.name:id;
}
// Puts one platform's build into the store, whole or not at all, replacing that platform's, with
// its publishers' signatures beside it as <bundle>.sig (RFC 0006).
void storeBuild(const std::string& format,const std::string& id,const std::string& version,const std::string& name,const std::string& forPlatform,const fs::path& build,const std::vector<signing::Signature>& signatures={}){
    const auto folder=storeFolder(format,id,version);
    fs::create_directories(folder);
    const auto identity="{\"format\":"+quote(format)+",\"id\":"+quote(id)+",\"version\":"+quote(version)+",\"name\":"+quote(name)+"}\n";
    state_files::write(folder/"plugin.json",identity.data(),identity.size());
    const auto partial=folder/("."+forPlatform+".partial");
    std::error_code e;fs::remove_all(partial,e);fs::create_directories(partial);
    fs::copy(build,partial/build.filename(),fs::copy_options::recursive);
    if(!signatures.empty())signing::writeSignatures(partial/(build.filename().string()+".sig"),signatures);
    fs::remove_all(folder/forPlatform,e);fs::rename(partial,folder/forPlatform);
}
// The signatures in <build>.sig, beside a build where its author or the store put it, that are
// over this build; others are dropped, said in notes when given.
std::vector<signing::Signature> buildSignatures(const fs::path& build,const std::string& format,const std::string& id,const std::string& version,const std::string& forPlatform,const std::string& digest,std::vector<std::string>* notes=nullptr){
    std::vector<signing::Signature> out;
    const auto message=signing::buildMessage(format,id,version,forPlatform,digest);
    try{for(const auto& s:signing::readSignatures(build.string()+".sig"))if(signing::verify(s,message))out.push_back(s);else if(notes)notes->push_back("a signature beside "+build.filename().string()+" is not over this build; it was left out");}
    catch(const std::runtime_error& x){if(notes)notes->push_back(x.what());}
    return out;
}
// A plugin that is not here stands in its place (RFC 0006), with the saved identity, ports and
// the parameters its snapshot and the document name, keeping its saved state.
std::shared_ptr<PluginHost> standIn(V node,const fs::path& snapshot,const std::set<uint32_t>& refs,const std::string& reason){
    const V plugin=field(node,"plugin");
    UnavailablePlugin p;
    p.descriptor.path=text(field(plugin,"pathHint"));p.descriptor.id=text(field(plugin,"id"));p.descriptor.version=text(field(plugin,"version"));
    p.descriptor.name=text(field(node,"name"));
    auto ports=[](V list){std::vector<kick::clap::Port> out;for(auto e:array(list,Graph::maxAudioChannels))out.push_back({uint32_t(integer(field(e,"id"))),{},uint32_t(integer(field(e,"channels"))),uint32_t(integer(field(e,"flags")))});return out;};
    p.inputs=ports(field(node,"inputs"));p.outputs=ports(field(node,"outputs"));
    p.noteInputs=uint32_t(integer(field(node,"noteInputs"),16));p.noteOutputs=uint32_t(integer(field(node,"noteOutputs"),16));
    std::set<uint32_t> ids=refs;
    try{const auto bytes=state_files::read(snapshot/"snapshot",2*1024*1024);std::istringstream in(std::string(bytes.begin(),bytes.end()));in.imbue(std::locale::classic());
        std::string kind;unsigned v;uint32_t id;double value;if(in>>kind>>v && kind=="parameters")while(in>>id>>value)ids.insert(id);}catch(const std::exception&){}
    for(const auto id:ids){kick::clap::Parameter q;q.id=id;q.name="Parameter "+std::to_string(id);q.minimum=-1e9;q.maximum=1e9;p.parameters.push_back(q);}
    p.reason=p.descriptor.id+" "+p.descriptor.version+" is not here ("+reason+"); its place and saved state are kept";
    return unavailablePlugin(std::move(p));
}
// A plugin refused before activating because it declared more memory than the budget left
// (kick::clap::Host's reason, which reaches here as text, also from a worker): not a stand-in.
bool memoryRefusal(const std::exception& e){return std::string(e.what()).find(" MiB of memory where ")!=std::string::npos;}
// Two saved snapshots alike, file for file and byte for byte.
bool sameTree(const fs::path& a,const fs::path& b){
    std::error_code e;std::map<fs::path,uintmax_t> files;size_t count=0;
    for(const auto& f:fs::recursive_directory_iterator(a,e))if(f.is_regular_file())files[fs::relative(f.path(),a)]=f.file_size();
    if(e)return false;
    for(const auto& f:fs::recursive_directory_iterator(b,e)){if(!f.is_regular_file())continue;++count;
        const auto it=files.find(fs::relative(f.path(),b));if(it==files.end() || it->second!=f.file_size())return false;}
    if(e || count!=files.size())return false;
    for(const auto& [name,size]:files)if(state_files::read(a/name,size)!=state_files::read(b/name,size))return false;
    return true;
}
// A saved node's plugin: the build at its saved path, else the store's, of the saved version;
// else its stand-in. load adds a build, version reads it, unload removes a mismatched one.
template<class Load,class Unload,class Version,class Adopt>
NodeId resolvePlugin(V node,const fs::path& snapshot,const std::set<uint32_t>& refs,Load load,Unload unload,Version version,Adopt adopt){
    const V plugin=field(node,"plugin");
    const auto format=text(field(plugin,"format")),id=text(field(plugin,"id")),saved=text(field(plugin,"version"));
    std::string reason="not installed";
    for(const auto& candidate:{fs::path(text(field(plugin,"pathHint"))),storedPlugin(format,id,saved)}){
        std::error_code e;if(candidate.empty() || !fs::exists(candidate,e))continue;
        try{const auto n=load(candidate.string(),id);const auto have=version(n);if(have==saved)return n;unload(n);reason="version "+have+" installed";}
        catch(const std::runtime_error& x){if(memoryRefusal(x))throw std::invalid_argument(x.what());reason=x.what();} // a load that failed; a refusal (the budget's) is not one
    }
    return adopt(standIn(node,snapshot,refs,reason));
}
std::string ports(const std::vector<kick::clap::Port>& list){std::vector<std::string> out;for(const auto& p:list)out.push_back("{\"id\":"+std::to_string(p.id)+",\"channels\":"+std::to_string(p.channels)+",\"flags\":"+std::to_string(p.flags)+'}');return join(out);}
void checkPorts(V list,const std::vector<kick::clap::Port>& expected){const auto entries=array(list,Graph::maxAudioChannels);if(entries.size()!=expected.size())bad("audio port layout changed");for(size_t i=0;i<entries.size();++i){fields(entries[i],{"id","channels","flags"});if(integer(field(entries[i],"id"))!=expected[i].id || integer(field(entries[i],"channels"))!=expected[i].channels || integer(field(entries[i],"flags"))!=expected[i].flags)bad("audio port layout changed");}}
void validateTree(const fs::path& root){size_t files=0;uint64_t bytes=0;for(const auto& e:fs::recursive_directory_iterator(root)){if(++files>65536 || e.is_symlink() || (!e.is_directory() && !e.is_regular_file()))bad("invalid or oversized asset tree");if(e.is_regular_file()){const auto n=e.file_size();if(n>16ull*1024*1024*1024-bytes)bad("assets exceed 16 GiB");bytes+=n;}}}
// The next free name in a name's pattern: "Verse" gives "Verse 2", "Scene 3" gives "Scene 4".
std::string nextFree(const std::string& name,const std::function<bool(const std::string&)>& taken){
    if(!taken(name))return name;
    std::string base=name;int n=1;
    const auto space=name.find_last_of(' ');
    if(space!=std::string::npos && space+1<name.size() && std::all_of(name.begin()+std::ptrdiff_t(space)+1,name.end(),[](char c){return c>='0' && c<='9';}) && name.size()-space<=9)
        {base=name.substr(0,space);n=std::stoi(name.substr(space+1));}
    std::string next;
    do next=base+" "+std::to_string(++n);while(taken(next));
    return next;
}
std::unique_ptr<json_value_s,decltype(&std::free)> document(const fs::path& root){const auto bytes=state_files::read(root/"performance.json",4*1024*1024);const auto error=detail::jsonPreflight({reinterpret_cast<const char*>(bytes.data()),bytes.size()},32);if(!error.empty())bad(std::string(error));auto* doc=json_parse(bytes.data(),bytes.size());if(!doc)bad("invalid JSON");return {doc,&std::free};}
}

void Session::savePerformance(const std::string& directory) {info_=writeDocument(directory,info_,nullptr,nullptr);}

// The samples a saved Performance's or Sound's plugins use, as its document lists them.
std::vector<assets::Ref> Session::assetsOf(const fs::path& manifest) {
    const auto bytes=state_files::read(manifest,4*1024*1024);
    const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
    if(!doc)bad("invalid JSON in "+manifest.string());
    std::vector<assets::Ref> out;
    for(auto node:array(field(doc.get(),"nodes"),256))if(member(node,"assets"))
        for(auto a:array(field(node,"assets"),65536))out.push_back({text(field(a,"library")),text(field(a,"path")),text(field(a,"sha256")),member(a,"optional") && boolean(field(a,"optional"))});
    return out;
}

std::vector<std::string> Session::exportPerformance(const std::string& file,bool editedSounds,const ExportSigning& sign) {
    return exportArchive(file,"performance",[&](const fs::path& stage,Package& package){
        const auto name=info_.name.empty()?fs::path(file).stem().string():info_.name;
        const auto folder=fs::path("Performances")/(library().newFolder(name,"performance").filename());
        fs::create_directories(stage/"Performances");
        const auto saved=writeDocument((stage/folder).string(),info_,nullptr,nullptr); // this Performance's info is not changed by exporting
        package.id=saved.id;package.name=saved.name;
        package.items.push_back({"performance",saved.id,saved.name,folder.generic_string()});
        std::vector<PartSource> from;for(const auto& p:parts_)from.push_back(p.from);
        return from;
    },editedSounds,sign);
}

std::vector<std::string> Session::exportLibraryItem(const std::string& id,const std::string& file,bool editedSounds,const ExportSigning& sign) {
    const auto* found=library().find(id);
    if(!found)bad("no such library item");
    const auto item=*found;
    return exportArchive(file,item.kind,[&](const fs::path& stage,Package& package){
        const auto folder=fs::path(item.kind=="sound"?"Sounds":"Performances")/fs::path(item.path).filename();
        fs::create_directories((stage/folder).parent_path());fs::copy(item.path,stage/folder,fs::copy_options::recursive);
        package.id=item.id;package.name=item.name;
        package.items.push_back({item.kind,item.id,item.name,folder.generic_string()});
        // A Performance's parts, as saved: where each came from.
        std::vector<PartSource> from;
        if(item.kind=="performance"){
            const auto bytes=state_files::read(stage/folder/"performance.json",4*1024*1024);
            const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
            if(!doc)bad("invalid JSON in "+item.path);
            for(auto part:array(field(doc.get(),"parts"),1024))
                if(const auto* f=member(part,"from");f && f->type==json_type_object)from.push_back({text(field(f,"sound")),text(field(f,"revision")),text(field(f,"name")),boolean(field(f,"modified"))});
        }
        return from;
    },editedSounds,sign);
}

// An archive of what write puts in the stage, with the Sounds of the parts it reports (each only
// while the part is still that Sound, that revision the library's; an edited part's when asked)
// and every sample they all use.
std::vector<std::string> Session::exportArchive(const std::string& file,const std::string& kind,const std::function<std::vector<PartSource>(const fs::path&,Package&)>& write,bool editedSounds,const ExportSigning& sign) {
    std::vector<std::string> notes;
    const fs::path archive(file);
    if(archive.filename().empty() || fs::exists(archive))bad("the export file has no name or exists already");
    std::optional<signing::Key> key;
    if(sign.items || sign.archive)key=signingKey(sign.key);
    std::random_device random;
    const auto stage=dataHome()/(".export-"+std::to_string(random())+std::to_string(random())); // like installs: in the data folder, not the OS temp folder, which may be RAM (tmpfs)
    try{
        Package package;package.kind=kind;
        const auto from=write(stage,package);
        std::vector<fs::path> manifests{stage/package.items.front().path/(kind=="sound"?"sound.json":"performance.json")};
        std::set<std::string> sounds;
        for(const auto& f:from){
            if(f.sound.empty() || (f.modified && !editedSounds) || sounds.count(f.sound))continue;
            const auto* item=library().find(f.sound);
            if(!item || item->kind!="sound" || item->revision!=f.revision)continue;
            sounds.insert(item->id);
            const auto to=fs::path("Sounds")/fs::path(item->path).filename();
            fs::create_directories(stage/to);fs::copy(item->path,stage/to,fs::copy_options::recursive);
            package.items.push_back({"sound",item->id,item->name,to.generic_string()});
            manifests.push_back(stage/to/"sound.json");
        }
        // Every sample they use, each once, by its hash.
        std::set<std::string> copied;
        for(const auto& m:manifests)for(auto ref:assetsOf(m)){
            const auto source=assets_.resolve(ref);
            if(!copied.insert(ref.sha256).second)continue;
            const auto to=stage/"Assets"/ref.sha256.substr(0,16)/source.filename();
            fs::create_directories(to.parent_path());fs::copy_file(source,to);
        }
        // Every plugin they use (RFC 0006), each build there is: this device's from its saved path,
        // else the store, and the store's for other platforms; one not here, or not CLAP yet, is
        // said and left out.
        std::set<std::string> bundled,said;
        for(const auto& m:manifests){
            const auto bytes=state_files::read(m,4*1024*1024);
            const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
            for(auto node:array(field(doc.get(),"nodes"),256)){
                const V plugin=field(node,"plugin");
                const auto format=text(field(plugin,"format")),id=text(field(plugin,"id")),version=text(field(plugin,"version"));
                const auto name=version.empty()?id:id+" "+version; // LV2 plugins have no version yet
                if(!bundled.insert(format+" "+name).second)continue;
                if(format!="clap"){if(said.insert(name).second)notes.push_back(name+" was left out: LV2 plugins are installed on each device, not carried in archives");continue;}
                // The saved path only while it still holds the build the document locked, else the store's.
                std::string locked;
                for(const auto& l:readLock(doc.get()))if(l.format==format && l.id==id && l.version==version && l.platform==platform())locked=l.sha256;
                // Other platforms' builds come from the store.
                auto builds=storedBuilds(format,id,version);
                std::error_code e;fs::path build=text(field(plugin,"pathHint"));
                if(!locked.empty() && fs::exists(build,e) && bundleDigest(build)==locked)builds[platform()]=build;
                std::string pluginName=storedName(format,id,version);
                for(const auto& n:nodes_)if(const auto& d=graph_.host(n.first).descriptor();d.id==id && d.version==version && !d.name.empty())pluginName=d.name;
                if(builds.empty()){if(said.insert(name).second)notes.push_back(name+" was left out: it is not here");continue;}
                Package::Plugin packed{format,id,version,pluginName,{}};
                for(const auto& [forPlatform,from]:builds){
                    const auto to=fs::path("Plugins")/format/storeName(id,version)/forPlatform/from.filename();
                    fs::create_directories((stage/to).parent_path());
                    fs::copy(from,stage/to,fs::copy_options::recursive);
                    const auto digest=bundleDigest(stage/to);
                    packed.builds.push_back({forPlatform,to.generic_string(),digest,buildSignatures(from,format,id,version,forPlatform,digest,&notes)});
                }
                package.plugins.push_back(std::move(packed));
            }
        }
        // The exporter's signatures, when chosen: each item's over its content hash, and the whole archive's.
        if(sign.items)for(auto& i:package.items)
            i.signatures.push_back({key->publicKey,signing::sign(*key,signing::itemMessage(contentHash(stage/i.path/(i.kind=="sound"?"sound.json":"performance.json"))))});
        writePackage(archive,package,stage,sign.archive?&*key:nullptr);
    }catch(...){std::error_code ignored;fs::remove_all(stage,ignored);throw;}
    std::error_code ignored;fs::remove_all(stage,ignored);
    return notes;
}

// The nodes a part holds: its chains' and lanes' boxes, and its splits' Split and Join nodes.
std::set<NodeId> Session::nodesOf(const Part& p) const {
    std::set<NodeId> out;
    auto add=[&](const std::vector<NodeId>& c){for(const auto n:c)if(!isSplit(n))out.insert(n);};
    add(p.chain);
    for(const auto& sp:p.splits){add(std::vector<NodeId>{sp.splitter,sp.joiner});for(const auto& l:sp.lanes)add(l.chain);}
    out.erase(0);
    return out;
}

// performance.json, or with `sound` set, that part's sound.json (RFC 0021): its nodes, the wires
// with both ends among them, its chain and splits without its seat, and the controls it owns.
// Wires that cross the part's edge are not saved; their descriptions go to `left`.
Control::PerformanceInfo Session::writeDocument(const std::string& directory,PerformanceInfo info,const Part* sound,std::vector<std::string>* left) {
    if(staged_)bad("commit staged graph edits before saving");
    service();if(!pending_.empty())bad("parameter edits are still pending; retry after processing");
    const auto graph=graph_.describe();if(graph.nodes.size()>256 || graph.notes.size()>4096 || graph.mappings.size()>4096)bad("graph exceeds Performance limits");
    const auto inside=sound?nodesOf(*sound):std::set<NodeId>{};
    auto kept=[&](NodeId n){return !leaving_.count(n) && (!sound || inside.count(n));};
    state_files::Bundle bundle(directory);fs::create_directory(bundle.temporary.path/"nodes");
    std::map<NodeId,std::string> keys{{graphInput,"input"},{graphOutput,"output"}};
    {size_t i=0;for(const auto& n:graph.nodes)if(kept(n.id))keys[n.id]="node-"+std::to_string(++i);}
    std::vector<std::string> nodes,wires,parts,mappings;std::set<std::string> libraries;
    std::vector<LockEntry> lock; // RFC 0006: each plugin build it uses, by its digest, per platform
    for(const auto& n:graph.nodes){if(!kept(n.id))continue;const auto& key=keys.at(n.id);auto& h=graph_.host(n.id);
        if(!keys.count(graph_.effectSource(n.id,leaving_)))bad(nodes_.at(n.id)+" shares its engine with a plugin outside the part");
        // Sound assets are dependencies (RFC 0009): record references, never the files.
        std::vector<std::string> assets;
        for(const auto& a:h.assetDependencies()){libraries.insert(a.library);assets.push_back("{\"library\":"+quote(a.library)+",\"path\":"+quote(a.path)+",\"sha256\":"+quote(a.sha256)+(a.optional?",\"optional\":true":"")+'}');}
        try{h.saveSnapshot((bundle.temporary.path/"nodes"/key).string());}catch(const std::exception& e){bad("cannot save "+nodes_.at(n.id)+" ("+n.plugin.id+"): "+e.what());}
        const bool lv2=lv2Path(n.plugin.path);
        lockPlugin(lock,lv2?"lv2":"clap",n.plugin.id,n.plugin.version,n.plugin.path,!h.failure().empty());
        nodes.push_back("{\"key\":"+quote(key)+",\"name\":"+quote(nodes_.at(n.id))+",\"plugin\":{\"format\":"+quote(lv2?"lv2":"clap")+",\"id\":"+quote(n.plugin.id)+",\"version\":"+quote(n.plugin.version)+",\"pathHint\":"+quote(n.plugin.path)+"},\"engine\":"+quote(keys.at(graph_.effectSource(n.id,leaving_)))+",\"inputs\":"+ports(n.inputs)+",\"outputs\":"+ports(n.outputs)+",\"noteInputs\":"+std::to_string(n.noteInputs)+",\"noteOutputs\":"+std::to_string(n.noteOutputs)+",\"assets\":"+join(assets)+",\"enabled\":"+(disabled_.count(n.id)?"false":"true")+(roles_.count(n.id)?",\"role\":"+quote(roles_.at(n.id)):std::string())+'}');
    }
    // RFC 0011: wires with their owners and Mix settings, and the parts; graph routes follow from them.
    if(wires_.size()>Graph::maxAudioConnections || parts_.size()>256)bad("wires or parts exceed Performance limits");
    for(const auto& w:wires_){
        if(sound){
            const bool from=inside.count(w.from),to=inside.count(w.to);
            if(!from && !to)continue;
            if(from!=to){if(w.owner==WireOwner::User && left)left->push_back((from?nodes_.at(w.from):"the "+std::string(w.from==graphInput?"input":"output"))+" to "+(to?nodes_.at(w.to):w.to==graphOutput?"the output":nodes_.count(w.to)?nodes_.at(w.to):"elsewhere"));continue;}
        }
        std::vector<std::string> pairs;for(const auto& [a,b]:w.channels)pairs.push_back("["+std::to_string(a)+","+std::to_string(b)+"]");
        wires.push_back(std::string("{\"kind\":")+(w.kind==WireKind::Audio?"\"audio\"":"\"notes\"")+",\"owner\":"+(w.owner==WireOwner::Part?"\"part\",\"part\":"+std::to_string(w.part):"\"user\"")
            +",\"from\":"+quote(keys.at(w.from))+",\"to\":"+quote(keys.at(w.to))+",\"fromPort\":"+std::to_string(w.fromPort)+",\"toPort\":"+std::to_string(w.toPort)
            +",\"channels\":"+join(pairs)+",\"level\":"+numeric(w.level)+",\"pan\":"+numeric(w.pan)+",\"mute\":"+(w.mute?"true":"false")+",\"solo\":"+(w.solo?"true":"false")+",\"zones\":"+zonesText(w.zones)+",\"send\":"+std::to_string(w.send)+",\"split\":"+std::to_string(w.split)+",\"lane\":"+std::to_string(w.lane)+",\"fader\":"+(w.fader?"true":"false")+",\"enabled\":"+(w.enabled?"true":"false")+'}');
    }
    // A chain's entries: node keys, and {"split":id} for a split.
    auto chainText=[&](const std::vector<NodeId>& entries){std::vector<std::string> out;for(const auto n:entries)out.push_back(isSplit(n)?"{\"split\":"+std::to_string(n&~splitMark)+'}':quote(keys.at(n)));return join(out);};
    auto splitsText=[&](const Part& p){
        std::vector<std::string> splits;
        for(const auto& sp:p.splits){
            std::vector<std::string> lanes;
            for(const auto& l:sp.lanes)lanes.push_back("{\"chain\":"+chainText(l.chain)+",\"zones\":"+zonesText(l.zones)+",\"level\":"+numeric(l.level)+",\"pan\":"+numeric(l.pan)+",\"mute\":"+(l.mute?"true":"false")+",\"solo\":"+(l.solo?"true":"false")+'}');
            auto key=[&](NodeId n){return n?quote(keys.at(n)):std::string("null");};
            splits.push_back("{\"id\":"+std::to_string(sp.id)+",\"kind\":\""+splitKinds[size_t(sp.kind)]+"\",\"splitter\":"+key(sp.splitter)+",\"joiner\":"+key(sp.joiner)+",\"lanes\":"+join(lanes)+'}');
        }
        return join(splits);
    };
    if(!sound)for(const auto& p:parts_){
        const char* kind=p.input.kind==PartInput::Kind::Notes?"notes":p.input.kind==PartInput::Kind::Audio?"audio":p.input.kind==PartInput::Kind::Bus?"bus":"none";
        std::vector<std::string> sends;for(const auto& s:p.sends)sends.push_back("{\"bus\":"+std::to_string(s.bus)+",\"level\":"+numeric(s.level)+'}');
        parts.push_back("{\"id\":"+std::to_string(p.id)+",\"name\":"+quote(p.name)+",\"input\":{\"kind\":\""+kind+"\",\"port\":"+std::to_string(p.input.port)+",\"zones\":"+zonesText(p.input.zones)+",\"devices\":"+devicesText(p.input.devices)+",\"maxNotes\":"+std::to_string(p.input.maxNotes)+"},\"chain\":"+chainText(p.chain)+",\"splits\":"+splitsText(p)+",\"output\":"+std::to_string(p.output)+",\"level\":"+numeric(p.level)+",\"pan\":"+numeric(p.pan)+",\"mute\":"+(p.mute?"true":"false")+",\"solo\":"+(p.solo?"true":"false")+",\"bus\":"+std::to_string(p.bus)+",\"sends\":"+join(sends)+",\"midiOut\":"+midiOutText(p.midiOut)+",\"key\":"+(p.key?keyText(*p.key):std::string("null"))
            +",\"from\":"+(p.from.sound.empty()?std::string("null"):"{\"sound\":"+quote(p.from.sound)+",\"revision\":"+quote(p.from.revision)+",\"name\":"+quote(p.from.name)+",\"modified\":"+(p.from.modified?"true":"false")+'}')+'}');
    }
    for(const auto& m:graph.mappings)if(kept(m.source) && kept(m.target))mappings.push_back("{\"from\":"+quote(keys.at(m.source))+",\"lane\":"+std::to_string(m.lane)+",\"to\":"+quote(keys.at(m.target))+",\"param\":"+std::to_string(m.param)+'}');
    // The control tree: each control's value and targets, parameters by node key. A Sound has no scenes to be off in.
    std::vector<std::string> controls;
    auto offIn=[&](const MacroTarget& t){std::vector<std::string> ids;if(!sound)for(const auto id:t.offIn)ids.push_back(std::to_string(id));return join(ids);};
    for(const auto& m:macros_){
        if(sound && m.part!=sound->id)continue;
        std::vector<std::string> targets;
        for(const auto& t:m.targets){
            if(t.kind==MacroTarget::Kind::Parameter && !keys.count(t.node))continue; // a plugin moved out of the part since
            const char* kind=t.kind==MacroTarget::Kind::Parameter?"parameter":t.kind==MacroTarget::Kind::PartLevel?"partLevel":t.kind==MacroTarget::Kind::PartMute?"partMute":t.kind==MacroTarget::Kind::Morph?"morph":"control";
            const auto curve=detail::curveJson(t,numeric);
            std::string at=t.kind==MacroTarget::Kind::Parameter?",\"node\":"+quote(keys.at(t.node))+",\"param\":"+std::to_string(t.param):t.kind==MacroTarget::Kind::Macro?",\"control\":"+std::to_string(t.macro):
                t.kind==MacroTarget::Kind::Morph?",\"from\":"+std::to_string(t.sceneFrom)+",\"to\":"+std::to_string(t.sceneTo):",\"part\":"+std::to_string(t.part);
            targets.push_back(std::string("{\"kind\":\"")+kind+"\""+at+",\"start\":"+numeric(t.start)+",\"end\":"+numeric(t.end)+",\"curve\":"+curve+",\"relative\":"+(t.relative?"true":"false")+",\"base\":"+numeric(t.base)+",\"offIn\":"+offIn(t)+'}');
        }
        controls.push_back("{\"id\":"+std::to_string(m.id)+",\"part\":"+std::to_string(m.part)+",\"name\":"+quote(m.name)+",\"value\":"+numeric(m.value)+",\"targets\":"+join(targets)+'}');
    }
    std::vector<std::string> named;
    for(const auto& l:assets_.libraries())if(libraries.count(l.id))named.push_back("{\"id\":"+quote(l.id)+",\"name\":"+quote(l.name)+",\"fingerprint\":"+quote(l.fingerprint)+'}');
    // RFC 0021: the library's info; the ID is made at the first save and kept by every later one.
    auto fresh=[]{std::random_device random;constexpr char hex[]="0123456789abcdef";std::string id;for(int i=0;i<16;++i)id+=hex[random()&15];return id;};
    if(info.id.empty())info.id=fresh();
    // A new revision at every save, its parent the one it was saved over.
    info.parent=info.revision;info.revision=fresh();
    if(info.name.empty())info.name=fs::path(directory).stem().string();
    std::vector<std::string> tags;for(const auto& t:info.tags)tags.push_back(quote(t));
    auto infoText=[&]{return infoJson(info);};
    // The document around its info block; the content hash covers it and the plugins' state.
    std::string head,tail;
    std::vector<std::string> locked;
    for(const auto& l:lock)locked.push_back("{\"format\":"+quote(l.format)+",\"id\":"+quote(l.id)+",\"version\":"+quote(l.version)+",\"platform\":"+quote(l.platform)+",\"sha256\":"+quote(l.sha256)+'}');
    const auto lockText="{\"plugins\":"+join(locked)+'}';
    // A Sound's own physical-control assignments (RFC 0011): its plugins' parameters and its controls.
    std::vector<std::string> own;
    if(sound)for(const auto& o:assignments_)if(o.part==sound->id){const auto& t=o.target;
        if(t.kind==Assignment::Kind::Parameter && keys.count(t.node))own.push_back("{\"control\":"+quote(o.control)+",\"kind\":\"parameter\",\"node\":"+quote(keys.at(t.node))+",\"param\":"+std::to_string(t.param)+'}');
        else if(t.kind==Assignment::Kind::Control && std::any_of(macros_.begin(),macros_.end(),[&](const Macro& m){return m.id==t.id && m.part==sound->id;}))own.push_back("{\"control\":"+quote(o.control)+",\"kind\":\"control\",\"id\":"+std::to_string(t.id)+'}');}
    if(sound){head="{\"format\":\"oscairn.sound\",\"version\":7,\"info\":";tail=",\"libraries\":"+join(named)+",\"nodes\":"+join(nodes)+",\"wires\":"+join(wires)+",\"part\":{\"chain\":"+chainText(sound->chain)+",\"splits\":"+splitsText(*sound)+"},\"mappings\":"+join(mappings)+",\"controls\":"+join(controls)+",\"assignments\":"+join(own)+",\"lock\":"+lockText+"}\n";}
    else{
        // Scenes: all each holds (parts.h), its scope; the parameters scenes leave alone; and the scene last selected.
        auto ref=[&](const WireRef& r){return "\"kind\":"+std::string(r.kind==WireKind::Audio?"\"audio\"":"\"notes\"")+",\"part\":"+std::to_string(r.part)+",\"from\":"+quote(keys.at(r.from))+",\"to\":"+quote(keys.at(r.to))
            +",\"fromPort\":"+std::to_string(r.fromPort)+",\"toPort\":"+std::to_string(r.toPort)+",\"send\":"+std::to_string(r.send)+",\"split\":"+std::to_string(r.split)+",\"lane\":"+std::to_string(r.lane)+",\"fader\":"+(r.fader?"true":"false");};
        auto here=[&](const WireRef& r){return keys.count(r.from) && keys.count(r.to);}; // a plugin removed since leaves its entries behind
        std::vector<std::string> scenes,parameters;
        for(const auto& s:scenes_){
            std::vector<std::string> scope,off,offWires,sp,mixes,values,sc;
            for(size_t k=0;k<sceneKinds.size();++k)if(s.scope&(1u<<k))scope.push_back(quote(sceneKinds[k]));
            for(const auto n:s.off)if(keys.count(n))off.push_back(quote(keys.at(n)));
            for(const auto& r:s.offWires)if(here(r))offWires.push_back("{"+ref(r)+'}');
            for(const auto& p:s.parts){
                std::vector<std::string> sends,lanes;
                for(const auto& x:p.sends)sends.push_back("{\"bus\":"+std::to_string(x.bus)+",\"level\":"+numeric(x.level)+'}');
                for(const auto& l:p.lanes)lanes.push_back("{\"split\":"+std::to_string(l.split)+",\"lane\":"+std::to_string(l.lane)+",\"level\":"+numeric(l.level)+",\"pan\":"+numeric(l.pan)+",\"mute\":"+(l.mute?"true":"false")+",\"solo\":"+(l.solo?"true":"false")+'}');
                sp.push_back("{\"part\":"+std::to_string(p.part)+",\"level\":"+numeric(p.level)+",\"pan\":"+numeric(p.pan)+",\"mute\":"+(p.mute?"true":"false")+",\"solo\":"+(p.solo?"true":"false")+",\"sends\":"+join(sends)+",\"lanes\":"+join(lanes)+'}');}
            for(const auto& m:s.wires)if(here(m.wire))mixes.push_back("{"+ref(m.wire)+",\"level\":"+numeric(m.level)+",\"pan\":"+numeric(m.pan)+",\"mute\":"+(m.mute?"true":"false")+",\"solo\":"+(m.solo?"true":"false")+'}');
            for(const auto& v:s.values)if(keys.count(v.at.node))values.push_back("{\"node\":"+quote(keys.at(v.at.node))+",\"param\":"+std::to_string(v.at.param)+",\"value\":"+numeric(v.value)+'}');
            for(const auto& [m,v]:s.macros)sc.push_back("{\"control\":"+std::to_string(m)+",\"value\":"+numeric(v)+'}');
            scenes.push_back("{\"id\":"+std::to_string(s.id)+",\"name\":"+quote(s.name)+",\"scope\":"+join(scope)+",\"glide\":"+std::to_string(s.glide)+",\"curve\":"+quote(sceneCurves[size_t(s.curve)])+",\"launch\":"+quote(sceneLaunches[size_t(s.launch)])+",\"off\":"+join(off)+",\"offWires\":"+join(offWires)+",\"parts\":"+join(sp)+",\"wires\":"+join(mixes)+",\"values\":"+join(values)+",\"controls\":"+join(sc)+",\"master\":"+masterText(s.masterLevel,s.masterPan)+'}');
        }
        // Physical controls' overrides, while what they name is here.
        std::vector<std::string> assigned;
        for(const auto& o:assignments_){const auto& t=o.target;
            if(o.part && std::none_of(parts_.begin(),parts_.end(),[&](const Part& p){return p.id==o.part;}))continue;
            const auto part=",\"part\":"+std::to_string(o.part);
            if(t.kind==Assignment::Kind::Parameter){if(keys.count(t.node))assigned.push_back("{\"control\":"+quote(o.control)+",\"kind\":\"parameter\",\"node\":"+quote(keys.at(t.node))+",\"param\":"+std::to_string(t.param)+part+'}');continue;}
            const bool here=t.kind==Assignment::Kind::Control?std::any_of(macros_.begin(),macros_.end(),[&](const Macro& m){return m.id==t.id;})
                           :t.kind==Assignment::Kind::Scene?std::any_of(scenes_.begin(),scenes_.end(),[&](const Scene& x){return x.id==t.id;})
                           :std::any_of(parts_.begin(),parts_.end(),[&](const Part& p){return p.id==t.id;});
            if(here)assigned.push_back("{\"control\":"+quote(o.control)+",\"kind\":"+quote(assignmentKinds[size_t(t.kind)])+",\"id\":"+std::to_string(t.id)+part+'}');}
        for(const auto& at:untouched_)if(keys.count(at.node))parameters.push_back("{\"node\":"+quote(keys.at(at.node))+",\"param\":"+std::to_string(at.param)+'}');
        head="{\"format\":\"oscairn.performance\",\"version\":11,\"info\":";tail=",\"libraries\":"+join(named)+",\"transport\":{\"tempo\":"+numeric(tempo_.load())+",\"clockSource\":"+quote(clockSource_.load()==Transport::ClockSource::Internal?"internal":"midi")+"},\"audioInputs\":"+ports(graph.audioLayout.inputs)+",\"audioOutputs\":"+ports(graph.audioLayout.outputs)+",\"nodes\":"+join(nodes)+",\"wires\":"+join(wires)+",\"parts\":"+join(parts)+",\"mappings\":"+join(mappings)+",\"pads\":"+padsText(padMap_)+",\"key\":"+keyText(key_)+",\"master\":"+masterText(masterLevel_,masterPan_)+",\"scenes\":"+join(scenes)+",\"sceneUntouched\":"+join(parameters)+",\"scene\":"+std::to_string(scene_)+",\"controls\":"+join(controls)+",\"assignments\":"+join(assigned)+",\"lock\":"+lockText+"}\n";
    }
    info.hash=contentHash(head+tail,bundle.temporary.path/"nodes");
    const auto out=head+infoText()+tail;
    if(out.size()>4*1024*1024)bad("manifest exceeds size limit");
    const auto encoding=detail::jsonPreflight(out,32);if(!encoding.empty())bad(std::string(encoding));
    state_files::write(bundle.temporary.path/(sound?"sound.json":"performance.json"),out.data(),out.size());validateTree(bundle.temporary.path);bundle.publish();
    return info;
}

namespace {
    fs::path performanceRoot(const std::string& directory) {
        if(directory.empty() || directory.find('\0')!=std::string::npos)bad("invalid directory");
        return fs::canonical(directory);
    }
    // What a prepared Performance was read from; saving over it changes this.
    fs::file_time_type stamp(const fs::path& root) { return fs::last_write_time(root/"performance.json"); }
}

void Session::loadPerformance(const std::string& directory) {
    audition_.reset(); // opening a Performance keeps what an audition tried; auditionStart sets its own after
    const auto root=performanceRoot(directory);
    // Over the budget it is refused, saying what does not fit (RFC 0025), keeping what is prepared.
    bool seamless=true;uint64_t incoming=0;
    if(budget_.cpu>0){const auto e=estimate(root.string());if(!e.fits)bad("this Performance "+tooMuch(e));seamless=e.seamless;incoming=e.memory;
        if(!seamless)std::fprintf(stderr,"oscairn: opened at once: with the last Performance ringing out it would not fit\n");}
    // Prepared ahead (RFC 0021), the cue only publishes it; another one prepared is let go first.
    auto prepared=std::move(prepared_);preparedOpportunistic_=false;
    if(prepared && (prepared->root!=root || prepared->stamp!=stamp(root)))prepared.reset();
    // Read at the cue, it may take over the playing Performance's free instances where keeping them warm would not fit (RFC 0021); a cut, any of them.
    if(!prepared)prepared=readTaking(root,!keepInstances_,!seamless,incoming);
    open(*prepared,seamless);
    if(!seamless)warm_.clear(); // it cut for want of room: none kept warm (RFC 0025)
    evictOpportunistic();
}

std::unique_ptr<Session::Prepared> Session::readTaking(const fs::path& root,bool playing,bool all,uint64_t incoming) {
    auto free=warm_;
    if(playing){auto more=freeInstances(all);
        // The middle rule (RFC 0021): unless it cuts, a playing instance is kept warm instead of taken
        // over while keeping it fits the warm limit and, with memory classes, what admission leaves
        // opportunistic data once this one plays (at its estimate) beside what still rings out. Older
        // instances kept warm are let go first, as least recently used.
        if(!all){
            uint64_t room=UINT64_MAX;
            if(memoryClasses()){
                std::set<NodeId> outgoing(leaving_),offered;for(const auto& [n,name]:nodes_)outgoing.insert(n);for(const auto& f:more)offered.insert(f.node);
                uint64_t used=reservedMemory(false)+budget_.margin+recorder_.committedBytes()+incoming;
                for(const auto n:outgoing)if(!offered.count(n) && graph_.contains(n))used+=nodeCost(graph_,n).memory; // rings out, or no instance to keep
                room=used<budget_.device?budget_.device-used:0;}
            size_t count=0;uint64_t bytes=0;
            for(auto& f:more){const auto b=f.host->residentBytes(); // as warmMemory counts it
                if(count<warmLimit && b<=room-bytes){f.keep=true;++count;bytes+=b;}}}
        free.insert(free.end(),more.begin(),more.end());}
    std::unique_ptr<Prepared> p;
    // Instances kept warm it gave a state to before it failed hold that state now, not the one remembered.
    try{p=read(root,std::move(free));}catch(...){for(auto& w:warm_)w.loaded.source.clear();throw;}
    std::erase_if(warm_,[&](const Free& w){return std::find(p->warm.begin(),p->warm.end(),w.host.get())!=p->warm.end();});
    return p;
}

Session::Beside::~Beside() {
    // Prepared and never selected: nothing pins them, so they go at once.
    for(const auto id:fresh)try{host->discardSound(id);}catch(const std::exception&){}
}

// The outgoing sounds of instances taken over with sounds beside: each goes once the plugin lets it
// (idle: not selected, queued, sounding or ringing); one it no longer lists is forgotten.
// Then their ring-out routes go, once no unselected sound sounds (the Default may hold a tail too);
// for a change's sounds a later change choked, its own routes once they and the Default are silent.
void Session::retireSounds() {
    if(staged_)return; // committing would publish edits not committed yet
    bool dropped=false;
    std::erase_if(retiringSounds_,[&](Retiring& r){
        const auto host=r.host.lock();if(!host)return true;
        try{
            std::erase_if(r.ids,[&](uint32_t id){return host->discardSound(id);});
            const auto listed=host->soundContexts();
            if(!r.ids.empty()){std::set<uint32_t> ids;for(const auto& c:listed)ids.insert(c.id);std::erase_if(r.ids,[&](uint32_t id){return !ids.count(id);});}
            if(!r.ids.empty() || std::any_of(listed.begin(),listed.end(),[&](const auto& c){return !c.selected_channels && !c.idle && (!r.older || !c.id);}))return false;
        }catch(const std::exception&){}
        if(graph_.contains(r.node) && graph_.node(r.node).host==host && graph_.ringOutRoutes(r.node)){graph_.dropRingOut(r.node,r.older);dropped=true;}
        return true;});
    if(dropped)graph_.commit();
}
size_t Session::ringOutRoutes() const { size_t n=0;for(const auto& node:graph_.describe().nodes)n+=graph_.ringOutRoutes(node.id);return n; }

// A node of the last Performance rung out: kept loaded, out of the graph, for a later change to
// take over (RFC 0021), when it could be; else removed. Not while an audition holds what it set aside.
void Session::keepWarm(NodeId n) {
    const auto loaded=loaded_.find(n);
    if(audition_ || loaded==loaded_.end() || graph_.host(n).effectResources() || !graph_.host(n).failure().empty()){graph_.remove(n);return;}
    warm_.push_back({0,graph_.release(n),loaded->second});
    if(warm_.size()>warmLimit)warm_.erase(warm_.begin());
}

uint64_t Session::warmMemory() const {
    // shortcut: a plugin in the engine's process reports no memory of its own, so only workers' count; measure in-process plugins by key if they matter.
    uint64_t n=0;for(const auto& w:warm_)n+=w.host->residentBytes();return n;
}

std::vector<Session::Free> Session::freeInstances(bool all) const {
    std::set<NodeId> outgoing(leaving_);for(const auto& [n,name]:nodes_)outgoing.insert(n);
    std::set<NodeId> busy;
    if(!all){
        // Free: nothing sounding through it, and asleep or three seconds silent (ringOut's rule
        // without its limit); and nothing busy sending to it, so a held note's chain stays whole.
        const auto quiet=uint64_t(sampleRate_*3);
        for(const auto n:outgoing)if(!graph_.silent(n,quiet))busy.insert(n);
        const auto t=graph_.describe();
        for(bool more=true;more;){more=false;
            for(const auto& r:t.audioRoutes)if(busy.count(r.from.node) && r.to.node!=graphOutput && busy.insert(r.to.node).second)more=true;
            for(const auto& e:t.notes)if(busy.count(e.first) && e.second!=graphOutput && busy.insert(e.second).second)more=true;}
    }
    std::vector<Free> out;
    for(const auto n:outgoing){
        const auto loaded=loaded_.find(n);if(busy.count(n) || loaded==loaded_.end())continue;
        auto host=graph_.node(n).host;
        // Engines offering effect contexts go by slice 1; a failed plugin or a stand-in never.
        if(host->effectResources() || !host->failure().empty())continue;
        out.push_back({n,std::move(host),loaded->second});
    }
    return out;
}

// Plays a Performance prepared ahead at the next loadPerformance of the same folder. One that
// would not fit beside what plays now is not prepared (false): it loads at the cue, as a cut.
bool Session::preparePerformance(const std::string& directory) {
    // The one prepared already, unchanged since, is kept: preparing it again would only read it again.
    if(prepared_ && !directory.empty() && prepared_->root==performanceRoot(directory) && prepared_->stamp==stamp(prepared_->root))return !preparedOpportunistic_;
    prepared_.reset();preparedOpportunistic_=false;
    if(directory.empty())return false;
    const auto root=performanceRoot(directory);
    bool opportunistic=false;
    if(budget_.cpu>0){const auto e=estimate(root.string());if(!e.fits)bad("this Performance "+tooMuch(e));
        if(!e.seamless){
            // RFC 0025: with memory classes it is prepared anyway, as opportunistic data, when what
            // neither running apps' reservations nor committed memory hold allows, so the cut is fast.
            const auto used=memoryClasses()?reservedMemory(false)+budget_.margin+committedMemory():0;
            if(!memoryClasses() || used+e.memory>budget_.device){std::fprintf(stderr,"oscairn: not prepared: beside the open Performance it would not fit; it opens at the cue\n");return false;}
            opportunistic=true;}}
    prepared_=readTaking(root,false,false);preparedOpportunistic_=opportunistic;
    graph_.reserveIds(*prepared_->next); // so plugins added meanwhile cannot take its IDs
    return !opportunistic;
}

// Reads a Performance and loads its plugins with their state into a replacement graph, without
// touching what plays. Throws, leaving nothing behind, on anything it cannot restore.
std::unique_ptr<Session::Prepared> Session::read(const fs::path& root,std::vector<Free> free) {
    validateTree(root);const auto doc=document(root);V d=doc.get();
    fields(d,{"format","version","info","libraries","transport","audioInputs","audioOutputs","nodes","wires","parts","mappings","pads","key","master","scenes","sceneUntouched","scene","controls","assignments","lock"});
    const auto pads=padsFrom(field(d,"pads"));const auto key=keyFrom(field(d,"key"));const auto master=masterFrom(field(d,"master"));
    if(text(field(d,"format"))!="oscairn.performance" || integer(field(d,"version"))!=11)bad("unsupported format version");
    fields(field(d,"info"),{"id","name","description","tags","color","revision","parent","hash","origin"});const auto info=readInfo(root/"performance.json","oscairn.performance");
    auto lock=readLock(d);
    const auto transport=field(d,"transport");fields(transport,{"tempo","clockSource"});const double tempo=number(field(transport,"tempo"));
    const auto clock=text(field(transport,"clockSource"));if(tempo<1 || tempo>1000 || (clock!="internal" && clock!="midi"))bad("invalid transport");
    const auto layout=graph_.describe().audioLayout;checkPorts(field(d,"audioInputs"),layout.inputs);checkPorts(field(d,"audioOutputs"),layout.outputs);
    // Every required sound asset must resolve before any plugin loads; report all that do not. An
    // optional one missing is a notice: the plugin loads, its resolve failing.
    std::vector<std::string> notices;
    {
        std::map<std::string,std::string> names;
        if(member(d,"libraries"))for(auto l:array(field(d,"libraries"),4096)){fields(l,{"id","name","fingerprint"});names[text(field(l,"id"))]=text(field(l,"name"));}
        std::vector<std::string> missing;
        for(auto node:array(field(d,"nodes"),256)){
            if(!member(node,"assets"))continue;
            std::map<std::string,std::pair<size_t,std::string>> unavailable;size_t total=0;
            for(auto a:array(field(node,"assets"),65536)){fields(a,{"library","path","sha256","optional"});++total;
                assets::Ref ref{text(field(a,"library")),text(field(a,"path")),text(field(a,"sha256"))};const auto library=ref.library;const bool optional=member(a,"optional") && boolean(field(a,"optional"));
                try{assets_.resolve(ref);}catch(const std::exception& e){
                    if(optional){notices.push_back(text(field(node,"name"))+": optional "+ref.path+" unavailable, so it plays without it: "+e.what());continue;}
                    auto& u=unavailable[library];if(!u.first++)u.second=e.what();}}
            for(const auto& [library,u]:unavailable)
                missing.push_back(text(field(node,"name"))+": "+std::to_string(u.first)+" of "+std::to_string(total)+" files from library '"+(names.count(library)?names[library]:library)+"' unavailable, e.g. "+u.second);
        }
        if(!missing.empty()){std::string all;for(const auto& m:missing)all+=(all.empty()?"":"; ")+m;bad("missing sound assets: "+all);}
    }
    std::set<NodeId> disabled; // off plugins: they never sounded here, so they have rung out
    std::map<NodeId,std::string> roles; // RFC 0016
    auto next=graph_.prepareReplacement();std::map<std::string,NodeId> ids{{"input",graphInput},{"output",graphOutput}};std::map<NodeId,std::string> names;std::set<std::string> usedNames;
    std::map<std::string,V> definitions;const auto nodes=array(field(d,"nodes"),256);
    for(auto node:nodes){fields(node,{"key","name","plugin","engine","inputs","outputs","noteInputs","noteOutputs","assets","enabled","role"});const auto key=text(field(node,"key"));
        if(key.size()<6 || key.size()>12 || key.substr(0,5)!="node-" || !std::all_of(key.begin()+5,key.end(),[](char c){return c>='0' && c<='9';}) || !definitions.emplace(key,node).second)bad("invalid or duplicate node key");
        const auto name=text(field(node,"name"));if(!name.empty() && !usedNames.insert(name).second)bad("duplicate node name");}
    // RFC 0021: the nodes sharing one engine become contexts of an engine of that plugin playing
    // now, when it has room for all of them, instead of loading another instance.
    std::map<std::string,uint32_t> group;for(auto node:nodes)++group[text(field(node,"engine"))];
    std::map<NodeId,uint32_t> claimed; // contexts taken per playing engine
    auto playing=[&](const std::string& id,const std::string& path,const std::string& version,uint32_t count)->std::optional<NodeId>{
        for(const auto& [n,name]:nodes_){const auto& h=graph_.host(n);const auto& p=h.descriptor();if(p.id!=id || p.path!=path || p.version!=version)continue;
            const auto r=h.effectResources();auto& taken=claimed[graph_.effectSource(n)];
            if(r && r->capacity>=r->prepared+taken+count){taken+=count;return n;}}
        return std::nullopt;};
    auto refs=parameterRefs(d);
    // RFC 0021: a node alone on its engine takes over a free instance of the same plugin, build and
    // placement instead of loading one.
    std::vector<NodeId> taken;std::vector<const PluginHost*> warm;std::map<NodeId,Loaded> origins;uint64_t reused=0,kept=0;
    struct Later{NodeId runtime;std::string key;size_t free;};std::vector<Later> later; // playing instances: their state last
    const auto stamped="@"+std::to_string(static_cast<long long>(stamp(root).time_since_epoch().count()));
    auto source=[&](const std::string& key){return (root/"nodes"/key).string()+stamped;};
    // The plugin, build and placement a node would load, which an instance it takes over must be.
    struct Want{std::string id,version,build;fs::path path;bool worker=false;};
    auto want=[&](V plugin)->std::optional<Want>{
        const auto format=text(field(plugin,"format")),id=text(field(plugin,"id")),saved=text(field(plugin,"version"));
        fs::path wanted;std::error_code e;
        for(const auto& candidate:{fs::path(text(field(plugin,"pathHint"))),storedPlugin(format,id,saved)})if(!candidate.empty() && fs::exists(candidate,e)){wanted=candidate;break;}
        if(wanted.empty())return std::nullopt;
        const auto build=buildDigest(wanted.string());if(build.empty())return std::nullopt;
        const auto& policy=placementPolicy();const auto placed=policy.plugins.find(id);
        const bool worker=format=="clap" && !policy.worker.empty() && (placed==policy.plugins.end()?policy.standard:placed->second)==Placement::Worker;
        return Want{id,saved,build,wanted,worker};};
    auto same=[&](const Want& w,const PluginHost& h,const Loaded& l){const auto& p=h.descriptor();std::error_code e;
        return p.id==w.id && p.version==w.version && l.build==w.build && h.inWorker()==w.worker && (p.path==w.path.string() || fs::equivalent(p.path,w.path,e));};
    auto take=[&](V plugin,const std::string& key)->std::optional<size_t>{
        if(free.empty())return std::nullopt;
        const auto w=want(plugin);if(!w)return std::nullopt;
        auto fits=[&](size_t i){return free[i].host && same(*w,*free[i].host,free[i].loaded);};
        // One that holds this node's state already first (switching back), then kept warm, least recently
        // used first, then a playing one only where keeping it warm would not fit (the middle rule).
        for(size_t i=0;i<free.size();++i)if(free[i].loaded.source==source(key) && fits(i))return i;
        for(size_t i=0;i<free.size();++i)if(!free[i].keep && fits(i))return i;
        return std::nullopt;};
    // RFC 0021: a native instrument of the open Performance with the same plugin, build, placement and
    // ports has the node's sounds prepared beside its own; the node takes it over at the cue. Not in an
    // audition, which takes over nothing set aside.
    std::vector<std::unique_ptr<Beside>> beside;
    auto claim=[&](V node,V plugin,const std::string& key)->std::optional<NodeId>{
        if(keepInstances_)return std::nullopt;
        std::optional<Want> w;
        for(const auto& [n,name]:nodes_){
            const auto loaded=loaded_.find(n);if(loaded==loaded_.end() || std::find(taken.begin(),taken.end(),n)!=taken.end())continue;
            const auto host=graph_.node(n).host;const auto& h=*host;
            if(!h.preparesSnapshots() || h.effectResources() || !h.failure().empty())continue;
            if(!w && !(w=want(plugin)))return std::nullopt;
            if(!same(*w,h,loaded->second) || integer(field(node,"noteInputs"),16)!=h.noteInputPorts() || integer(field(node,"noteOutputs"),16)!=h.noteOutputPorts())continue;
            try{checkPorts(field(node,"inputs"),h.inputPorts());checkPorts(field(node,"outputs"),h.outputPorts());}catch(const std::exception&){continue;}
            auto b=std::make_unique<Beside>();b->was=n;b->host=host;
            for(const auto& c:h.soundContexts())if(c.id)b->old.push_back(c.id);
            try{b->selected=b->host->prepareSnapshot((root/"nodes"/key).string());}
            catch(const std::exception& e){std::fprintf(stderr,"oscairn: %s: its sounds not prepared beside %s's: %s\n",text(field(node,"name")).c_str(),name.c_str(),e.what());continue;}
            for(const auto& c:h.soundContexts())if(c.id && std::find(b->old.begin(),b->old.end(),c.id)==b->old.end())b->fresh.push_back(c.id);
            b->now=next->add(host);origins[b->now]={loaded->second.build,{}}; // not its snapshot byte for byte: never skips a load
            for(auto& f:free)if(f.host==host)f.host.reset(); // neither kept warm nor taken over twice
            taken.push_back(n);beside.push_back(std::move(b));
            return beside.back()->now;}
        return std::nullopt;};
    auto restore=[&](NodeId runtime,V node,uint64_t room){
        auto& h=next->host(runtime);const auto key=text(field(node,"key"));
        if(room)h.setMemoryRoom(room);
        h.loadSnapshot((root/"nodes"/key).string());
        if(room)h.setMemoryRoom(0);
        checkPorts(field(node,"inputs"),h.inputPorts());checkPorts(field(node,"outputs"),h.outputPorts());
        if(integer(field(node,"noteInputs"),16)!=h.noteInputPorts() || integer(field(node,"noteOutputs"),16)!=h.noteOutputPorts())bad("note port layout changed");};
    // A playing instance's own state, saved before it is given another: put back if the load fails,
    // or if it is let go for a fresh one, so it plays on as before (RFC 0021).
    struct Saved{NodeId node;std::shared_ptr<PluginHost> host;std::unique_ptr<state_files::TemporaryDirectory> folder;};std::vector<Saved> saved;
    auto putBack=[&](const Saved& s){try{s.host->loadSnapshot((s.folder->path/"was").string());}
        catch(const std::exception& e){std::fprintf(stderr,"oscairn: %s not given its own state back: %s\n",s.host->descriptor().id.c_str(),e.what());}};
    // A taken-over instance whose state does not load, or whose ports then differ, or (playing) whose
    // own state cannot be saved, is let go for a fresh one.
    auto restoreTaken=[&](NodeId runtime,V node,uint64_t room,size_t f){
        auto& h=next->host(runtime);const auto key=text(field(node,"key"));
        // Its state already this node's, byte for byte: reset instead of loading it again.
        if(free[f].loaded.source==source(key)){
            state_files::TemporaryDirectory now;h.saveSnapshot((now.path/"now").string());
            if(sameTree(now.path/"now",root/"nodes"/key) && h.reset()){
                checkPorts(field(node,"inputs"),h.inputPorts());checkPorts(field(node,"outputs"),h.outputPorts());++kept;return;}}
        bool savable=true;
        if(free[f].node)try{saved.push_back({free[f].node,graph_.node(free[f].node).host,std::make_unique<state_files::TemporaryDirectory>()});h.saveSnapshot((saved.back().folder->path/"was").string());}
            catch(const std::exception&){saved.pop_back();savable=false;}
        if(savable)try{restore(runtime,node,room);return;}
        catch(const std::exception& e){if(memoryRefusal(e))throw;}
        const auto path=next->host(runtime).descriptor().path;
        next->reload(runtime,path,text(field(field(node,"plugin"),"id")),room);origins[runtime]={buildDigest(path),source(key)};--reused;
        if(free[f].node)std::erase(taken,free[f].node);
        restore(runtime,node,room);};
    // Roots first; siblings can refer forward to a root, never to an arbitrary chain.
    for(bool siblings:{false,true})for(auto node:nodes){const auto key=text(field(node,"key")),engine=text(field(node,"engine"));if((key!=engine)!=siblings)continue;
        const auto plugin=field(node,"plugin");fields(plugin,{"format","id","version","pathHint"});const auto format=text(field(plugin,"format")),id=text(field(plugin,"id")),path=text(field(plugin,"pathHint"));
        if((format!="clap" && format!="lv2") || id.empty() || path.empty() || (lv2Path(path)!=(format=="lv2")))bad("invalid plugin identity or format");
        const auto name=text(field(node,"name"));
        try {
            NodeId runtime;bool restored=false; // a taken-over instance gets its state its own way
            // RFC 0025: each plugin may declare what the memory budget leaves beside those loaded
            // before it (the Performance alone, as its estimate was checked); more is refused before it activates.
            std::vector<NodeId> loaded;for(const auto& [n,x]:names)loaded.push_back(n);
            const auto room=memoryRoom(*next,loaded);
            auto context=[&](PluginHost& source,const std::function<NodeId()>& make){
                if(room)source.setMemoryRoom(room);
                try{const auto n=make();if(room)source.setMemoryRoom(0);return n;}catch(...){if(room)source.setMemoryRoom(0);throw;}};
            if(siblings){if(!definitions.contains(engine) || text(field(definitions.at(engine),"engine"))!=engine || !ids.contains(engine))bad("missing shared engine root");
                const auto& source=next->host(ids.at(engine)).descriptor();if(source.id!=id || source.path!=path)bad("incompatible shared engine identity");
                try{runtime=context(next->host(ids.at(engine)),[&]{return next->addEffectContext(ids.at(engine));});}
                catch(const std::exception& e){
                    if(memoryRefusal(e))throw;
                    runtime=next->add(standIn(node,root/"nodes"/key,refs[key],std::string("its shared engine is not here: ")+e.what()));}}
            else if(const auto b=group[key]==1?claim(node,plugin,key):std::nullopt){runtime=*b;++reused;restored=true;}
            else if(const auto engine=playing(id,path,text(field(plugin,"version")),group[key])){runtime=context(graph_.host(*engine),[&]{return next->addEffectContext(graph_,*engine);});++reused;}
            else if(const auto f=group[key]==1?take(plugin,key):std::nullopt){
                runtime=next->add(free[*f].host);origins[runtime]={free[*f].loaded.build,source(key)};++reused;restored=true;
                if(free[*f].node){taken.push_back(free[*f].node);later.push_back({runtime,key,*f});}else{warm.push_back(free[*f].host.get());restoreTaken(runtime,node,room,*f);}
                free[*f].host.reset();}
            else{runtime=resolvePlugin(node,root/"nodes"/key,refs[key],[&](const std::string& p,const std::string& i){return next->add(p,i,room);},[&](NodeId n){next->remove(n);},
                                       [&](NodeId n){return next->host(n).descriptor().version;},[&](std::shared_ptr<PluginHost> h){return next->add(std::move(h));});
                // What another Performance may take over later: a plugin of its own, loaded (not a stand-in).
                if(group[key]==1 && next->host(runtime).failure().empty())origins[runtime]={buildDigest(next->host(runtime).descriptor().path),source(key)};}
            if(!restored)restore(runtime,node,room);
            ids.emplace(key,runtime);names.emplace(runtime,name);
            if(!boolean(field(node,"enabled")))disabled.insert(runtime);
            if(const auto role=roleFrom(node);!role.empty())roles[runtime]=role;
        }catch(const std::exception& e){bad("cannot restore "+name+" ("+id+"): "+e.what());}
    }
    auto node=[&](V v){const auto key=text(v);const auto it=ids.find(key);if(it==ids.end())bad("unknown node reference "+key);return it->second;};
    std::vector<Wire> wires;std::vector<Part> parts;uint32_t nextPart=1;
    {
        std::set<uint32_t> partIds;
        std::set<NodeId> partNodes; // a node belongs to one part
        for(auto p:array(field(d,"parts"),256)){
            fields(p,{"id","name","input","chain","splits","output","bus","sends","level","pan","mute","solo","midiOut","key","from"});Part part;part.id=integer(field(p,"id"));part.name=text(field(p,"name"));
            if(const auto from=field(p,"from");from->type!=json_type_null){
                fields(from,{"sound","revision","name","modified"});
                part.from={text(field(from,"sound")),text(field(from,"revision")),text(field(from,"name")),boolean(field(from,"modified"))};
                if(part.from.sound.empty())bad("a part's source names no Sound");}
            if(!part.id || !partIds.insert(part.id).second)bad("invalid or duplicate part");
            const auto input=field(p,"input");fields(input,{"kind","port","zones","devices","maxNotes"});const auto kind=text(field(input,"kind"));
            part.input.kind=kind=="notes"?PartInput::Kind::Notes:kind=="audio"?PartInput::Kind::Audio:kind=="none"?PartInput::Kind::None:kind=="bus"?PartInput::Kind::Bus:(bad("invalid part input"),PartInput::Kind::None);
            part.input.port=integer(field(input,"port"));part.input.zones=zonesFrom(field(input,"zones"));part.input.devices=devicesFrom(field(input,"devices"));part.input.maxNotes=uint16_t(integer(field(input,"maxNotes"),128));part.output=integer(field(p,"output"));part.midiOut=midiOutFrom(field(p,"midiOut"));if(const V k=field(p,"key");k->type!=json_type_null)part.key=keyFrom(k);
            // Chains hold node keys and split entries; every node and split appears once in the part, and each split is reached from the part's own chain.
            std::set<NodeId> seen;
            auto chain=[&](V list){std::vector<NodeId> out;for(auto n:array(list,256)){
                NodeId id;
                if(n->type==json_type_object){fields(n,{"split"});const auto s=integer(field(n,"split"));if(!s || s>=splitMark)bad("invalid split entry");id=splitMark|s;}
                else{id=node(n);if(id==graphInput || id==graphOutput)bad("invalid part chain");if(!partNodes.insert(id).second)bad("a node appears in two parts");}
                if(!seen.insert(id).second)bad("a node or split appears twice in a part");out.push_back(id);}return out;};
            part.chain=chain(field(p,"chain"));
            for(auto sp:array(field(p,"splits"),256)){
                fields(sp,{"id","kind","splitter","joiner","lanes"});Split split;split.id=integer(field(sp,"id"));if(!split.id || split.id>=splitMark)bad("invalid split");
                split.kind=Split::Kind(named(splitKinds,text(field(sp,"kind")),"invalid split kind"));
                // Its own Split and Join nodes, by key, for the kinds that have them.
                auto own=[&](const char* name,const char* plugin,bool needed){
                    const auto v=field(sp,name);if(v->type==json_type_null){if(needed)bad("a split is missing its nodes");return NodeId(0);}
                    const auto id=node(v);if(!needed || id==graphInput || id==graphOutput || !seen.insert(id).second || !partNodes.insert(id).second || next->describe().nodes.end()==std::find_if(next->describe().nodes.begin(),next->describe().nodes.end(),[&](const auto& n){return n.id==id && n.plugin.id==plugin;}))bad("invalid split node");
                    return id;};
                const bool joins=split.kind==Split::Kind::LeftRight || split.kind==Split::Kind::MidSide;
                split.splitter=own("splitter",splitPlugin,split.kind!=Split::Kind::Copy);split.joiner=own("joiner",joinPlugin,joins);
                for(auto l:array(field(sp,"lanes"),16)){
                    fields(l,{"chain","zones","level","pan","mute","solo"});Lane lane;lane.chain=chain(field(l,"chain"));lane.zones=zonesFrom(field(l,"zones"));
                    lane.level=number(field(l,"level"));lane.pan=number(field(l,"pan"));lane.mute=boolean(field(l,"mute"));lane.solo=boolean(field(l,"solo"));
                    if(lane.level<0 || lane.pan<-1 || lane.pan>1)bad("invalid lane level or pan");split.lanes.push_back(std::move(lane));}
                if(split.lanes.empty() || std::any_of(part.splits.begin(),part.splits.end(),[&](const Split& x){return x.id==split.id;}))bad("invalid or duplicate split");
                if((joins && split.lanes.size()!=2) || (split.kind==Split::Kind::Bands && (split.lanes.size()<2 || split.lanes.size()>4)))bad("wrong number of lanes for the split's kind");
                part.splits.push_back(std::move(split));
            }
            std::set<uint32_t> reached;
            std::function<void(const std::vector<NodeId>&)> reach=[&](const std::vector<NodeId>& c){for(const auto e:c)if(isSplit(e)){
                const auto it=std::find_if(part.splits.begin(),part.splits.end(),[&](const Split& x){return x.id==(e&~splitMark);});
                if(it==part.splits.end() || !reached.insert(it->id).second)bad("unknown or repeated split entry");for(const auto& l:it->lanes)reach(l.chain);}};
            reach(part.chain);
            if(reached.size()!=part.splits.size())bad("a split is not in the part's chain");
            part.bus=integer(field(p,"bus"));part.level=number(field(p,"level"));part.mute=boolean(field(p,"mute"));part.solo=boolean(field(p,"solo"));part.pan=number(field(p,"pan"));if(part.level<0 || part.pan<-1 || part.pan>1)bad("invalid part level or pan");
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
            if(owner=="part")fields(c,{"kind","owner","part","from","to","fromPort","toPort","channels","level","pan","mute","solo","zones","send","split","lane","fader","enabled"});
            else fields(c,{"kind","owner","from","to","fromPort","toPort","channels","level","pan","mute","solo","zones","send","split","lane","fader","enabled"});
            w.kind=kind=="audio"?WireKind::Audio:WireKind::Notes;w.owner=owner=="part"?WireOwner::Part:WireOwner::User;
            if(owner=="part"){w.part=integer(field(c,"part"));if(!partIds.count(w.part))bad("wire of an unknown part");}
            w.from=node(field(c,"from"));w.to=node(field(c,"to"));w.fromPort=integer(field(c,"fromPort"));w.toPort=integer(field(c,"toPort"));
            for(auto pair:array(field(c,"channels"),Graph::maxAudioChannels)){const auto ends=array(pair,2);if(ends.size()!=2)bad("invalid channel pair");w.channels.push_back({integer(ends[0],Graph::maxAudioChannels-1),integer(ends[1],Graph::maxAudioChannels-1)});}
            w.level=number(field(c,"level"));w.pan=number(field(c,"pan"));w.mute=boolean(field(c,"mute"));w.solo=boolean(field(c,"solo"));
            if(w.level<0 || w.pan<-1 || w.pan>1)bad("invalid wire level or pan");
            w.zones=zonesFrom(field(c,"zones"));if(w.kind==WireKind::Audio && !w.zones.empty())bad("audio wires have no zones");
            w.enabled=boolean(field(c,"enabled"));w.send=integer(field(c,"send"));w.split=integer(field(c,"split"));w.lane=integer(field(c,"lane"));if(w.split && w.owner!=WireOwner::Part)bad("only a part's wire joins a lane");w.fader=boolean(field(c,"fader"));if(w.fader && w.owner!=WireOwner::Part)bad("only a part's wire carries its fader");if(w.send && (w.owner!=WireOwner::Part || !std::any_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==w.send;})))bad("a send wire of an unknown bus");
            wires.push_back(std::move(w));
        }
    }
    uint32_t nextWire=1;for(auto& w:wires)w.id=nextWire++;
    std::vector<Scene> scenes;std::vector<SceneParameter> untouched;uint32_t nextScene=1;std::set<uint32_t> sceneIds;
    {
        auto parameterAt=[&](V v){SceneParameter at{node(field(v,"node")),integer(field(v,"param"))};
            if(at.node==graphInput || at.node==graphOutput)bad("invalid scene parameter");
            const auto& all=next->host(at.node).parameters();if(std::none_of(all.begin(),all.end(),[&](const auto& p){return p.id==at.param;}))bad("unknown scene parameter");return at;};
        for(auto at:array(field(d,"sceneUntouched"),65536)){fields(at,{"node","param"});const auto p=parameterAt(at);if(std::find(untouched.begin(),untouched.end(),p)!=untouched.end())bad("duplicate untouched parameter");untouched.push_back(p);}
        for(auto s:array(field(d,"scenes"),256)){
            fields(s,{"id","name","scope","glide","curve","launch","off","offWires","parts","wires","values","controls","master"});Scene scene;scene.id=integer(field(s,"id"));scene.name=text(field(s,"name"));
            sceneTransition(s,scene);
            if(!scene.id || !sceneIds.insert(scene.id).second)bad("invalid or duplicate scene");
            scene.scope=sceneScope(field(s,"scope"));
            for(auto n:array(field(s,"off"),256))scene.off.push_back(node(n));
            for(auto r:array(field(s,"offWires"),Graph::maxAudioConnections)){fields(r,{"kind","part","from","to","fromPort","toPort","send","split","lane","fader"});scene.offWires.push_back(wireRef(r,node));}
            for(auto p:array(field(s,"parts"),256)){
                fields(p,{"part","level","pan","mute","solo","sends","lanes"});
                Scene::PartState st{integer(field(p,"part")),number(field(p,"level")),number(field(p,"pan")),boolean(field(p,"mute")),boolean(field(p,"solo")),{},{}};
                if(st.level<0 || st.pan<-1 || st.pan>1 || std::none_of(parts.begin(),parts.end(),[&](const Part& x){return x.id==st.part;}))bad("invalid scene part");
                for(auto x:array(field(p,"sends"),256)){fields(x,{"bus","level"});st.sends.push_back({integer(field(x,"bus")),number(field(x,"level"))});if(st.sends.back().level<0)bad("invalid scene send");}
                for(auto l:array(field(p,"lanes"),4096)){fields(l,{"split","lane","level","pan","mute","solo"});
                    Scene::LaneState lane{integer(field(l,"split")),integer(field(l,"lane")),number(field(l,"level")),number(field(l,"pan")),boolean(field(l,"mute")),boolean(field(l,"solo"))};
                    if(lane.level<0 || lane.pan<-1 || lane.pan>1)bad("invalid scene lane");st.lanes.push_back(lane);}
                scene.parts.push_back(std::move(st));}
            for(auto w:array(field(s,"wires"),Graph::maxAudioConnections)){
                fields(w,{"kind","part","from","to","fromPort","toPort","send","split","lane","fader","level","pan","mute","solo"});
                Scene::WireMix m{wireRef(w,node),number(field(w,"level")),number(field(w,"pan")),boolean(field(w,"mute")),boolean(field(w,"solo"))};
                if(m.level<0 || m.pan<-1 || m.pan>1)bad("invalid scene wire");scene.wires.push_back(m);}
            for(auto v:array(field(s,"values"),65536)){fields(v,{"node","param","value"});scene.values.push_back({parameterAt(v),number(field(v,"value"))});}
            for(auto c:array(field(s,"controls"),4096)){fields(c,{"control","value"});const auto v=number(field(c,"value"));if(v<0 || v>1)bad("invalid scene control value");scene.macros.push_back({integer(field(c,"control")),v});}
            std::tie(scene.masterLevel,scene.masterPan)=masterFrom(field(s,"master"));
            nextScene=std::max(nextScene,scene.id+1);scenes.push_back(std::move(scene));
        }
        const auto selected=integer(field(d,"scene"));if(selected && !sceneIds.count(selected))bad("unknown selected scene");
    }
    const auto selectedScene=integer(field(d,"scene"));
    uint32_t mappingScene=0; // the mappings off in the selected scene are off, when it sets them
    for(const auto& s:scenes)if(s.id==selectedScene && (s.scope&Scene::Mappings))mappingScene=s.id;
    std::vector<Macro> macros;uint32_t nextMacro=1;
    {
        std::set<uint32_t> macroIds;
        const auto list=array(field(d,"controls"),4096);
        for(auto m:list){const auto id=integer(field(m,"id"));if(!id || !macroIds.insert(id).second)bad("invalid or duplicate control");}
        for(auto m:list){
            fields(m,{"id","part","name","value","targets"});Macro macro;macro.id=integer(field(m,"id"));macro.part=integer(field(m,"part"));macro.name=text(field(m,"name"));macro.value=number(field(m,"value"));
            if(macro.part && std::none_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==macro.part;}))bad("a control of an unknown part");
            if(macro.value<0 || macro.value>1)bad("invalid control value");
            for(auto t:array(field(m,"targets"),256)){
                MacroTarget target;const auto kind=text(field(t,"kind"));
                if(kind=="parameter"){fields(t,{"kind","node","param","start","end","curve","relative","base","offIn"});target.kind=MacroTarget::Kind::Parameter;target.node=node(field(t,"node"));target.param=integer(field(t,"param"));
                    if(target.node==graphInput || target.node==graphOutput)bad("invalid control target");const auto& all=next->host(target.node).parameters();
                    if(std::none_of(all.begin(),all.end(),[&](const auto& p){return p.id==target.param;}))bad("unknown control target parameter");}
                else if(kind=="partLevel" || kind=="partMute"){fields(t,{"kind","part","start","end","curve","relative","base","offIn"});target.kind=kind=="partLevel"?MacroTarget::Kind::PartLevel:MacroTarget::Kind::PartMute;target.part=integer(field(t,"part"));
                    if(std::none_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==target.part;}))bad("control target of an unknown part");}
                else if(kind=="control"){fields(t,{"kind","control","start","end","curve","relative","base","offIn"});target.kind=MacroTarget::Kind::Macro;target.macro=integer(field(t,"control"));
                    if(!macroIds.count(target.macro))bad("control target of an unknown control");}
                else if(kind=="morph"){fields(t,{"kind","from","to","start","end","curve","relative","base","offIn"});target.kind=MacroTarget::Kind::Morph;target.sceneFrom=integer(field(t,"from"));target.sceneTo=integer(field(t,"to"));
                    if(macro.part || !sceneIds.count(target.sceneFrom) || !sceneIds.count(target.sceneTo) || target.sceneFrom==target.sceneTo)bad("a morph between unknown scenes");}
                else bad("invalid control target kind");
                if(!detail::curveFrom(field(t,"curve"),target))bad("invalid curve");
                target.start=number(field(t,"start"));target.end=number(field(t,"end"));target.relative=boolean(field(t,"relative"));target.base=number(field(t,"base"));
                for(auto id:array(field(t,"offIn"),256)){target.offIn.push_back(integer(id));if(!sceneIds.count(target.offIn.back()))bad("a mapping off in an unknown scene");}
                if(target.start<0 || target.start>1 || target.end<0 || target.end>1)bad("invalid control target range");
                macro.targets.push_back(target);
            }
            nextMacro=std::max(nextMacro,macro.id+1);macros.push_back(std::move(macro));
        }
        // The controls must not loop.
        std::function<bool(uint32_t,uint32_t,size_t)> reaches=[&](uint32_t from,uint32_t to,size_t depth){
            if(depth>macros.size())return true;
            for(const auto& m:macros)if(m.id==from)for(const auto& t:m.targets)if(t.kind==MacroTarget::Kind::Macro && (t.macro==to || reaches(t.macro,to,depth+1)))return true;
            return false;};
        for(const auto& m:macros)if(reaches(m.id,m.id,0))bad("controls loop");
        for(const auto& s:scenes)for(const auto& [m,v]:s.macros)if(!macroIds.count(m))bad("a scene value for an unknown control");
    }
    // Physical controls' overrides (RFC 0011), by the control's logical identity.
    std::vector<AssignmentOverride> assignments;
    for(auto a:array(field(d,"assignments"),4096)){
        AssignmentOverride o;o.control=text(field(a,"control"));const auto kind=text(field(a,"kind"));o.part=integer(field(a,"part"));
        if(!validPhysicalControl(o.control) || std::any_of(assignments.begin(),assignments.end(),[&](const AssignmentOverride& x){return x.control==o.control && x.part==o.part;}))bad("invalid or duplicate physical control");
        if(o.part && (std::none_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==o.part;}) || (kind!="parameter" && kind!="control")))bad("a part's assignment plays its own plugins and controls");
        if(kind=="parameter"){fields(a,{"control","kind","node","param","part"});o.target.kind=Assignment::Kind::Parameter;o.target.node=node(field(a,"node"));o.target.param=integer(field(a,"param"));
            if(o.target.node==graphInput || o.target.node==graphOutput)bad("invalid assignment");const auto& all=next->host(o.target.node).parameters();
            if(std::none_of(all.begin(),all.end(),[&](const auto& p){return p.id==o.target.param;}))bad("unknown assigned parameter");}
        else{fields(a,{"control","kind","id","part"});o.target.id=integer(field(a,"id"));
            if(kind=="control"){o.target.kind=Assignment::Kind::Control;if(std::none_of(macros.begin(),macros.end(),[&](const Macro& m){return m.id==o.target.id;}))bad("an assignment to an unknown control");}
            else if(kind=="partLevel" || kind=="partMute"){o.target.kind=kind=="partLevel"?Assignment::Kind::PartLevel:Assignment::Kind::PartMute;if(std::none_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==o.target.id;}))bad("an assignment to an unknown part");}
            else if(kind=="scene"){o.target.kind=Assignment::Kind::Scene;if(!sceneIds.count(o.target.id))bad("an assignment to an unknown scene");}
            else bad("invalid assignment kind");}
        assignments.push_back(std::move(o));
    }
    std::set<std::pair<NodeId,uint32_t>> mappings;
    for(auto m:array(field(d,"mappings"),4096)){fields(m,{"from","lane","to","param"});const auto from=node(field(m,"from")),lane=integer(field(m,"lane"));if(!mappings.emplace(from,lane).second)bad("duplicate parameter mapping");next->mapParamTarget(from,lane,node(field(m,"to")),integer(field(m,"param")));}
    // Last, once the whole document has been read: the playing instances taken over get their state.
    try{
        for(const auto& l:later){
            const V n=definitions.at(l.key);
            std::vector<NodeId> loaded;for(const auto& [id,x]:names)if(id!=l.runtime)loaded.push_back(id);
            try{restoreTaken(l.runtime,n,memoryRoom(*next,loaded),l.free);}
            catch(const std::exception& e){bad("cannot restore "+text(field(n,"name"))+" ("+text(field(field(n,"plugin"),"id"))+"): "+e.what());}
        }
    }catch(...){for(const auto& s:saved)putBack(s);throw;}
    for(const auto& s:saved)if(std::find(taken.begin(),taken.end(),s.node)==taken.end())putBack(s); // let go for a fresh one: it plays on
    auto prepared=std::make_unique<Prepared>(Prepared{root,stamp(root),std::move(next),pads,info,std::move(lock),tempo,clock=="internal"?Transport::ClockSource::Internal:Transport::ClockSource::MidiClock,
        std::move(disabled),std::move(names),std::move(wires),std::move(parts),nextPart,nextWire,std::move(scenes),std::move(untouched),nextScene,selectedScene,mappingScene,std::move(macros),nextMacro,std::move(assignments),key,master.first,master.second,std::move(roles),std::move(notices),std::move(taken),std::move(warm),std::move(origins),reused,kept,{}});
    prepared->beside=std::move(beside);
    return prepared;
}

// Publishes a read Performance in place of the open one.
void Session::open(Prepared& p,bool seamless) {
    auto& next=p.next;auto& wires=p.wires;auto& disabled=p.disabled;
    // A part's input wires hear the devices its selector chooses here (RFC 0012); files keep selectors, not sources.
    for(auto& w:wires)if(w.owner==WireOwner::Part && w.kind==WireKind::Notes && w.from==graphInput)
        for(const auto& part:p.parts)if(part.id==w.part && (!part.input.devices.empty() || part.input.maxNotes)){
            // The file's zones (a lane's included) with this machine's sources and the part's note limit.
            if(w.zones.empty())w.zones.emplace_back();
            const auto mask=resolveDevices(part.input.devices,hostStatus_.midiInputs);for(auto& z:w.zones){z.sources=mask;z.maxNotes=part.input.maxNotes;}}
    // Seamless (RFC 0021): this Performance's plugins, and any still ringing out from the one
    // before, come along retiring: held notes play on until released and tails ring out, while
    // new notes go to the new Performance.
    // Unless the two together would not fit the budget (RFC 0025): then the change is a cut.
    std::set<NodeId> leaving;
    if(seamless){leaving=leaving_;for(const auto& [id,name]:nodes_)leaving.insert(id);for(const auto n:p.taken)leaving.erase(n);} // taken over: nothing rings on them
    auto loaded=loaded_;for(const auto& [n,l]:p.loaded)loaded[n]=l;
    next->carryOver(graph_,leaving);
    compileWires(*next,wires,disabled,disabled,leaving,p.masterLevel,p.masterPan);next->setMidiTaps(midiTaps(*next,p.parts,wires));next->setHarmonies(harmonies(p.parts,p.key));auto rungOut=disabled;
    // An instance whose next sounds were prepared beside (RFC 0021): the notes it holds go on through
    // the incoming node, keeping their sound; a cut releases them.
    // Its outgoing sounds ring out through the outgoing chain, by the plugin's ring-out buffer.
    // Only one change's tail carries over: one still ringing out of it is cut at the cue (chokes).
    if(seamless)for(const auto& b:p.beside){next->takeNotes(graph_,b->was,b->now);next->ringOut(graph_,b->was,b->now);b->chokes=graph_.ringOutRoutes(b->was)!=0;}
    std::map<Address,double> observed;for(const auto& [id,name]:p.names)for(const auto& q:next->host(id).parameters())observed[{id,q.id}]=next->host(id).parameterValue(q.id);
    // Compilation and all allocating preparation precede publication. The audio thread
    // adopts at one boundary; old plans and their hosts are reclaimed by service().
    auto history=history_;
    if(history.size()==historyCapacity)history.pop_front();
    history.push_back({sequence_+1,ChangeKind::Resync,0,0,0,false});
    graph_.publishReplacement(*next,[&]() noexcept {
        // Their channels select the incoming sounds, queued for the plugin's next block: the plan's first,
        // or the last before it if one runs between here and the hand-over.
        for(auto& b:p.beside){
            const auto& sel=b->selected;
            if(b->chokes)try{b->host->chokeRetiring();}catch(const std::exception&){} // before the selections: the sounds retiring now fade out
            try{if(std::all_of(sel.begin(),sel.end(),[&](uint32_t id){return id==sel[0];}))b->host->selectSound(sel[0],-1);
                else for(int16_t ch=0;ch<16;++ch)b->host->selectSound(sel[size_t(ch)],ch);}
            catch(const std::exception& e){std::fprintf(stderr,"oscairn: incoming sounds not selected: %s\n",e.what());}
            b->fresh.clear();}
        nodes_.swap(p.names);observed_.swap(observed);parts_.swap(p.parts);wires_.swap(wires);disabled_.swap(disabled);rungOut_.swap(rungOut);leaving_.swap(leaving);nextPart_=p.nextPart;nextWire_=p.nextWire;padMap_=p.pads;key_=p.key;masterLevel_=p.masterLevel;masterPan_=p.masterPan;roles_.swap(p.roles);notices_.swap(p.notices);heldPads_={};scenes_.swap(p.scenes);untouched_.swap(p.untouched);scene_=p.selectedScene;mappingScene_=p.mappingScene;transition_.reset();pendingScene_.reset();nextScene_=p.nextScene;macros_.swap(p.macros);assignments_.swap(p.assignments);nextMacro_=p.nextMacro;pending_.clear();commandCount_=0;outputDrops_.clear();parameterRevisions_.clear();staged_=false;
        tempo_.store(p.tempo);clockSource_.store(p.clock);playing_.store(false);loaded_.swap(loaded);reused_+=p.reused;keptStates_+=p.kept;
        ++revision_;++sequence_;history_.swap(history);
    });
    std::erase_if(loaded_,[&](const auto& l){return !graph_.contains(l.first);});
    // The outgoing sounds ring out, then go (retireSounds).
    // Those an earlier change left keep their routes, now the incoming node's from channels 2 and 3.
    for(auto& b:p.beside)if(b->chokes)for(auto& r:retiringSounds_)if(r.node==b->was){if(r.older)r.node=0;else{r.node=b->now;r.older=true;}}
    for(auto& b:p.beside)if(!b->old.empty() || graph_.ringOutRoutes(b->now))retiringSounds_.push_back({b->host,std::move(b->old),b->now});
    beside_+=p.beside.size();
    // Its parts' faders as live gains (RFC 0011's instant controls): the wires are built again
    // aside to work out which faders scale each, the wires read from the file keep everything
    // else, and they are compiled again with the slots.
    try{
        auto read=wires_;const auto next=nextWire_;buildParts();
        for(auto& w:read)for(const auto& b:wires_)
            if(w.owner==WireOwner::Part && b.owner==WireOwner::Part && w.part==b.part && w.kind==b.kind && w.from==b.from && w.to==b.to && w.fromPort==b.fromPort && w.toPort==b.toPort &&
               w.send==b.send && w.split==b.split && w.lane==b.lane && w.fader==b.fader){w.faders=b.faders;w.fixed=b.fixed;w.unsent=b.unsent;w.fixedMute=b.fixedMute;w.live=b.live;break;}
        wires_.swap(read);nextWire_=next;syncWires();graph_.commit();staged_=false;
    }catch(const std::exception&){}
    info_=p.info;
    lock_=std::move(p.lock);
    clearHistory(); // undo does not reach back into another Performance
    noteBases();
}

// A plugin into a lock being written: this platform's build by its digest (once per build) where
// it loaded, else what the documents loaded said; other platforms' builds as they said.
void Session::lockPlugin(std::vector<LockEntry>& lock,const std::string& format,const std::string& id,const std::string& version,const std::string& path,bool standIn) {
    if(std::any_of(lock.begin(),lock.end(),[&](const LockEntry& l){return l.format==format && l.id==id && l.version==version;}))return;
    const auto here=platform();
    const auto digest=standIn?std::string():buildDigest(path,true);
    for(const auto& l:lock_)if(l.format==format && l.id==id && l.version==version && (l.platform!=here || digest.empty()))lock.push_back(l);
    if(!digest.empty())lock.push_back({format,id,version,here,digest});
}

std::string Session::buildDigest(const std::string& path,bool fresh) const {
    auto& known=digests_[path];
    const auto now=std::chrono::steady_clock::now();
    if(!fresh && !known.digest.empty() && now-known.checked<std::chrono::seconds(1))return known.digest;
    known.checked=now;
    std::error_code e;
    if(!fs::exists(path,e))return known.digest={};
    // Again once it is rebuilt: the newest write to it, or to anything in its bundle.
    auto newest=fs::last_write_time(path,e);
    if(fs::is_directory(path,e))for(const auto& f:fs::recursive_directory_iterator(path,e))newest=std::max(newest,f.last_write_time(e));
    if(known.digest.empty() || known.newest!=newest){known.newest=newest;known.digest.clear();try{known.digest=bundleDigest(path);}catch(const std::exception&){}}
    return known.digest;
}

void Session::mergeLock(const std::vector<LockEntry>& more) {
    for(const auto& l:more)
        if(std::none_of(lock_.begin(),lock_.end(),[&](const LockEntry& x){return x.format==l.format && x.id==l.id && x.version==l.version && x.platform==l.platform;}))lock_.push_back(l);
}

NodeId Session::adopt(const std::string& name,std::shared_ptr<PluginHost> host) {
    const auto id=graph_.add(std::move(host));
    nodes_.emplace(id,name);
    for(const auto& p:graph_.host(id).parameters())observed_[{id,p.id}]=graph_.host(id).parameterValue(p.id);
    staged_=true;
    return id;
}

// Adds a document's plugins (all, or those keyed in `keep` and the engines they share), with
// their saved state, roots before the effect contexts sharing their engines; names that are
// taken get the next number. Every asset must resolve first. On failure nothing stays added.
std::map<std::string,NodeId> Session::addNodes(const json_value_s* document,const fs::path& root,std::vector<NodeId>& added,const std::set<std::string>* only) {
    const auto nodes=array(field(document,"nodes"),256);
    notices_.clear(); // a Sound's or an import's notices take the place of the last ones
    auto refs=parameterRefs(document);
    std::set<std::string> keep;
    if(only){keep=*only;for(auto node:nodes){const auto key=text(field(node,"key"));if(keep.count(key))keep.insert(text(field(node,"engine")));}}
    auto wanted=[&](V node){return !only || keep.count(text(field(node,"key")))!=0;};
    for(auto node:nodes)if(wanted(node) && member(node,"assets"))for(auto a:array(field(node,"assets"),65536)){
        fields(a,{"library","path","sha256","optional"});
        assets::Ref ref{text(field(a,"library")),text(field(a,"path")),text(field(a,"sha256"))};
        try{assets_.resolve(ref);}catch(const std::exception& e){
            if(member(a,"optional") && boolean(field(a,"optional"))){notices_.push_back(text(field(node,"name"))+": optional "+ref.path+" unavailable, so it plays without it: "+e.what());continue;}
            bad("missing sound asset: "+std::string(e.what()));}}
    std::map<std::string,NodeId> ids;
    auto unique=[&](const std::string& name){return name.empty()?name:nextFree(name,[&](const std::string& n){return std::any_of(nodes_.begin(),nodes_.end(),[&](const auto& x){return x.second==n;});});};
    try{
        for(bool siblings:{false,true})for(auto node:nodes){
            if(!wanted(node))continue;
            fields(node,{"key","name","plugin","engine","inputs","outputs","noteInputs","noteOutputs","assets","enabled","role"});
            const auto key=text(field(node,"key")),engine=text(field(node,"engine"));if((key!=engine)!=siblings)continue;
            if(key.size()<6 || key.size()>12 || key.substr(0,5)!="node-" || ids.count(key))bad("invalid or duplicate node key");
            const auto plugin=field(node,"plugin");fields(plugin,{"format","id","version","pathHint"});
            const auto id=text(field(plugin,"id")),name=text(field(node,"name"));
            NodeId runtime;
            const auto snapshot=root/"nodes"/key;
            if(siblings){if(!ids.count(engine))bad("missing shared engine root");
                try{runtime=addEffectContext(unique(name),ids.at(engine));}
                catch(const std::exception& e){if(memoryRefusal(e))throw;runtime=adopt(unique(name),standIn(node,snapshot,refs[key],std::string("its shared engine is not here: ")+e.what()));}}
            else runtime=resolvePlugin(node,snapshot,refs[key],[&](const std::string& p,const std::string& i){return add(unique(name),p,i);},[&](NodeId n){dropNode(n);},
                                       [&](NodeId n){return graph_.host(n).descriptor().version;},[&](std::shared_ptr<PluginHost> h){return adopt(unique(name),std::move(h));});
            added.push_back(runtime);
            auto& h=graph_.host(runtime);
            if(h.descriptor().id!=id)bad("saved plugin does not match the loaded plugin: "+name);
            std::vector<NodeId> others;for(const auto& [n,x]:nodes_)if(n!=runtime)others.push_back(n);
            const auto room=memoryRoom(graph_,others); // RFC 0025: its saved state may declare what the budget leaves
            if(room)h.setMemoryRoom(room);
            h.loadSnapshot((root/"nodes"/key).string());
            if(room)h.setMemoryRoom(0);
            checkPorts(field(node,"inputs"),h.inputPorts());checkPorts(field(node,"outputs"),h.outputPorts());
            if(integer(field(node,"noteInputs"),16)!=h.noteInputPorts() || integer(field(node,"noteOutputs"),16)!=h.noteOutputPorts())bad("note port layout changed: "+name);
            for(const auto& p:h.parameters())observed_[{runtime,p.id}]=h.parameterValue(p.id);
            if(!boolean(field(node,"enabled"))){disabled_.insert(runtime);rungOut_.insert(runtime);} // off, and never sounded here
            if(const auto role=roleFrom(node);!role.empty())roles_[runtime]=role;
            ids.emplace(key,runtime);
        }
    }catch(...){for(const auto n:added)dropNode(n);added.clear();graph_.commit();staged_=false;throw;}
    return ids;
}

// A Sound into a part (RFC 0021): its nodes are added with their saved state, the part's old
// nodes go with every wire, mapping, scene value and control target on them, and the part keeps
// its seat (input, output, sends, fader). The part's own controls are replaced by the Sound's; a
// Performance control that targeted one moves to the Sound's control of the same name. One undoable step; notes
// playing in the part are cut. Returns what could not be kept.
std::vector<std::string> Session::loadSoundInto(uint32_t partId,const fs::path& folder) {
    if(staged_)bad("commit staged graph edits before loading a Sound");
    record(); // an edit waiting for its step comes first
    findPart(partId);
    const auto root=fs::canonical(folder);validateTree(root);
    const auto bytes=state_files::read(root/"sound.json",4*1024*1024);
    if(const auto error=detail::jsonPreflight({reinterpret_cast<const char*>(bytes.data()),bytes.size()},32);!error.empty())bad(std::string(error));
    const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
    V d=doc.get();if(!d)bad("invalid JSON");
    fields(d,{"format","version","info","libraries","nodes","wires","part","mappings","controls","assignments","lock"});
    mergeLock(readLock(d)); // its plugins' builds, for saving the Performance it joins
    if(text(field(d,"format"))!="oscairn.sound" || integer(field(d,"version"))!=7)bad("not a version 7 Sound");
    fields(field(d,"info"),{"id","name","description","tags","color","revision","parent","hash","origin"});
    const auto info=readInfo(root/"sound.json","oscairn.sound");
    std::vector<NodeId> added;
    auto ids=addNodes(d,root,added,nullptr);
    std::vector<std::string> flagged;
    try{
        auto node=[&](V v){const auto key=text(v);const auto it=ids.find(key);if(it==ids.end())bad("unknown node reference "+key);return it->second;};
        // The chain and its splits, by the Sound's keys.
        const auto sp=field(d,"part");fields(sp,{"chain","splits"});
        std::function<std::vector<NodeId>(V)> chain=[&](V list){std::vector<NodeId> out;for(auto n:array(list,256)){
            if(n->type==json_type_object){fields(n,{"split"});const auto x=integer(field(n,"split"));if(!x || x>=splitMark)bad("invalid split entry");out.push_back(splitMark|x);}
            else out.push_back(node(n));}return out;};
        Part sound;sound.chain=chain(field(sp,"chain"));
        for(auto x:array(field(sp,"splits"),256)){
            fields(x,{"id","kind","splitter","joiner","lanes"});Split split;split.id=integer(field(x,"id"));
            split.kind=Split::Kind(named(splitKinds,text(field(x,"kind")),"invalid split kind"));
            for(auto [name,at]:{std::pair{"splitter",&split.splitter},std::pair{"joiner",&split.joiner}}){const auto v=field(x,name);*at=v->type==json_type_null?0:node(v);}
            for(auto l:array(field(x,"lanes"),16)){
                fields(l,{"chain","zones","level","pan","mute","solo"});Lane lane;lane.chain=chain(field(l,"chain"));lane.zones=zonesFrom(field(l,"zones"));
                lane.level=number(field(l,"level"));lane.pan=number(field(l,"pan"));lane.mute=boolean(field(l,"mute"));lane.solo=boolean(field(l,"solo"));
                if(lane.level<0 || lane.pan<-1 || lane.pan>1)bad("invalid lane level or pan");split.lanes.push_back(std::move(lane));}
            sound.splits.push_back(std::move(split));
        }
        if(nodesOf(sound).size()!=added.size())bad("every node of a Sound is in its part, once");
        // Its wires: user wires as they were; its part wires' Mix, for the rebuilt ones to take.
        std::vector<Wire> users,mixes;
        for(auto c:array(field(d,"wires"),Graph::maxAudioConnections)){
            const auto owner=text(field(c,"owner"));Wire w;
            w.kind=text(field(c,"kind"))=="audio"?WireKind::Audio:WireKind::Notes;w.owner=owner=="part"?WireOwner::Part:WireOwner::User;
            w.from=node(field(c,"from"));w.to=node(field(c,"to"));w.fromPort=integer(field(c,"fromPort"));w.toPort=integer(field(c,"toPort"));
            for(auto pair:array(field(c,"channels"),Graph::maxAudioChannels)){const auto ends=array(pair,2);if(ends.size()!=2)bad("invalid channel pair");w.channels.push_back({integer(ends[0],Graph::maxAudioChannels-1),integer(ends[1],Graph::maxAudioChannels-1)});}
            w.level=number(field(c,"level"));w.pan=number(field(c,"pan"));w.mute=boolean(field(c,"mute"));w.solo=boolean(field(c,"solo"));
            if(w.level<0 || w.pan<-1 || w.pan>1)bad("invalid wire level or pan");
            w.zones=zonesFrom(field(c,"zones"));w.enabled=boolean(field(c,"enabled"));
            (w.owner==WireOwner::User?users:mixes).push_back(std::move(w));
        }
        // The Sound's controls, numbered after this Performance's; parsed before anything changes.
        std::map<uint32_t,uint32_t> controlIds;std::vector<Macro> arriving;
        const auto list=array(field(d,"controls"),4096);
        for(auto m:list)controlIds[integer(field(m,"id"))]=nextMacro_++;
        for(auto m:list){
            fields(m,{"id","part","name","value","targets"});Macro macro;macro.id=controlIds.at(integer(field(m,"id")));macro.part=partId;macro.name=text(field(m,"name"));macro.value=number(field(m,"value"));
            if(macro.value<0 || macro.value>1)bad("invalid control value");
            for(auto t:array(field(m,"targets"),256)){
                MacroTarget target;const auto kind=text(field(t,"kind"));
                if(kind=="parameter"){target.kind=MacroTarget::Kind::Parameter;target.node=node(field(t,"node"));target.param=integer(field(t,"param"));}
                else if(kind=="control"){target.kind=MacroTarget::Kind::Macro;const auto it=controlIds.find(integer(field(t,"control")));if(it==controlIds.end())bad("control target of an unknown control");target.macro=it->second;}
                else bad("a Sound's control targets only its own plugins and controls");
                if(!detail::curveFrom(field(t,"curve"),target))bad("invalid curve");
                target.start=number(field(t,"start"));target.end=number(field(t,"end"));target.relative=boolean(field(t,"relative"));target.base=number(field(t,"base"));
                macro.targets.push_back(target);
            }
            arriving.push_back(std::move(macro));
        }
        std::vector<AssignmentOverride> owned; // its own physical-control assignments, for this part
        for(auto a:array(field(d,"assignments"),4096)){
            AssignmentOverride o;o.part=partId;o.control=text(field(a,"control"));const auto kind=text(field(a,"kind"));
            if(!validPhysicalControl(o.control) || std::any_of(owned.begin(),owned.end(),[&](const AssignmentOverride& x){return x.control==o.control;}))bad("invalid or duplicate physical control");
            if(kind=="parameter"){fields(a,{"control","kind","node","param"});o.target.kind=Assignment::Kind::Parameter;o.target.node=node(field(a,"node"));o.target.param=integer(field(a,"param"));
                const auto& all=graph_.host(o.target.node).parameters();if(std::none_of(all.begin(),all.end(),[&](const auto& q){return q.id==o.target.param;}))bad("unknown assigned parameter");}
            else if(kind=="control"){fields(a,{"control","kind","id"});o.target.kind=Assignment::Kind::Control;const auto it=controlIds.find(integer(field(a,"id")));if(it==controlIds.end())bad("an assignment to an unknown control");o.target.id=it->second;}
            else bad("a Sound's assignment plays its own plugins and controls");
            owned.push_back(std::move(o));}
        std::vector<Graph::Mapping> mappings;
        for(auto m:array(field(d,"mappings"),4096)){
            fields(m,{"from","lane","to","param"});Graph::Mapping x;x.source=node(field(m,"from"));x.lane=integer(field(m,"lane"));x.target=node(field(m,"to"));x.param=integer(field(m,"param"));
            const auto& all=graph_.host(x.target).parameters();if(std::none_of(all.begin(),all.end(),[&](const auto& q){return q.id==x.param;}))bad("unknown mapped parameter");
            mappings.push_back(x);}
        for(const auto& m:arriving)for(const auto& t:m.targets)if(t.kind==MacroTarget::Kind::Parameter){
            const auto& all=graph_.host(t.node).parameters();if(std::none_of(all.begin(),all.end(),[&](const auto& q){return q.id==t.param;}))bad("unknown control target parameter");}
        auto& part=findPart(partId);
        const auto old=nodesOf(part);
        std::set<uint32_t> oldControls;for(const auto& m:macros_)if(m.part==partId)oldControls.insert(m.id);
        // As editWires: if the new wiring does not compile, the parts and wires return to how they were.
        auto partsBefore=parts_;auto wiresBefore=wires_;
        try{
            for(const auto& w:wires_)if(w.owner==WireOwner::User && (old.count(w.from)!=old.count(w.to)))
                flagged.push_back("the wire from "+(nodes_.count(w.from)?nodes_.at(w.from):std::string("the input"))+" to "+(nodes_.count(w.to)?nodes_.at(w.to):std::string("the output"))+" was removed");
            std::erase_if(wires_,[&](const Wire& w){return old.count(w.from) || old.count(w.to);});
            auto& p=findPart(partId);p.chain=sound.chain;p.splits=sound.splits;
            for(auto w:users){w.id=nextWire_++;wires_.push_back(std::move(w));}
            buildParts(); // a bus's senders now feed the new chain
            for(auto& w:wires_)if(w.owner==WireOwner::Part && w.part==partId)
                for(const auto& m:mixes)if(m.kind==w.kind && m.from==w.from && m.to==w.to && m.fromPort==w.fromPort && m.toPort==w.toPort){w.level=m.level;w.pan=m.pan;w.mute=m.mute;w.solo=m.solo;w.enabled=m.enabled;break;}
            syncWires();
        }catch(...){parts_.swap(partsBefore);wires_.swap(wiresBefore);syncWires();flagged.clear();throw;}
        for(const auto n:old)dropNode(n);
        for(const auto& m:mappings)graph_.mapParamTarget(m.source,m.lane,m.target,m.param);
        // What scenes hold of the old nodes goes.
        for(auto& scene:scenes_){
            auto gone=[&](const WireRef& r){return old.count(r.from) || old.count(r.to);};
            std::erase_if(scene.values,[&](const Scene::Value& v){return old.count(v.at.node)!=0;});
            std::erase_if(scene.off,[&](NodeId n){return old.count(n)!=0;});
            std::erase_if(scene.offWires,gone);std::erase_if(scene.wires,[&](const Scene::WireMix& m){return gone(m.wire);});}
        std::erase_if(untouched_,[&](const SceneParameter& at){return old.count(at.node)!=0;});
        std::map<uint32_t,std::string> leaving;for(const auto& m:macros_)if(oldControls.count(m.id))leaving[m.id]=m.name;
        std::erase_if(macros_,[&](const Macro& m){return oldControls.count(m.id)!=0;});
        for(auto& m:macros_){
            std::erase_if(m.targets,[&](MacroTarget& t){
                if(t.kind==MacroTarget::Kind::Parameter && old.count(t.node)){flagged.push_back(m.name+": a target on the old plugins was removed");return true;}
                if(t.kind!=MacroTarget::Kind::Macro || !leaving.count(t.macro))return false;
                const auto same=std::find_if(arriving.begin(),arriving.end(),[&](const Macro& a){return a.name==leaving.at(t.macro);});
                if(same==arriving.end()){flagged.push_back(m.name+": its target "+leaving.at(t.macro)+" is not in the Sound and was removed");return true;}
                t.macro=same->id;return false;});
        }
        for(auto& scene:scenes_)for(auto& [id,value]:scene.macros)if(leaving.count(id)){
            const auto same=std::find_if(arriving.begin(),arriving.end(),[&](const Macro& a){return a.name==leaving.at(id);});
            id=same==arriving.end()?0:same->id;}
        for(auto& scene:scenes_)std::erase_if(scene.macros,[](const auto& v){return v.first==0;});
        macros_.insert(macros_.end(),arriving.begin(),arriving.end());
        noteBases();
        std::erase_if(assignments_,[&](const AssignmentOverride& o){return o.part==partId;});
        assignments_.insert(assignments_.end(),owned.begin(),owned.end());
        auto& loaded=findPart(partId);loaded.name=info.name;
        loaded.from={info.id,info.revision,info.name,false};
    }catch(...){
        // Everything that can fail comes before the old part changes: take the additions back.
        for(const auto n:added)if(graph_.contains(n)){std::erase_if(wires_,[&](const Wire& w){return w.from==n || w.to==n;});dropNode(n);}
        throw;
    }
    loading_.insert(partId);
    try{commit();}catch(...){loading_.erase(partId);throw;} // one step to undo
    loading_.erase(partId);
    return flagged;
}

Control::SoundSaved Session::savePartAsSound(uint32_t partId,const std::string& name,bool asNew) {
    auto& l=library();
    const auto& part=findPart(partId);
    if(name.size()>256 || name.find('\0')!=std::string::npos)bad("invalid Sound name");
    // Save writes over the Sound the part came from, as its next revision; Save As New, or a
    // part from no Sound (or one no longer in the library), makes a new one.
    const auto* item=asNew || part.from.sound.empty()?nullptr:l.find(part.from.sound);
    if(item && (item->kind!="sound" || !item->library.empty()))item=nullptr; // nothing saves over a locked library's Sound: Save is Save As New
    PerformanceInfo info;
    if(item){info=readInfo(fs::path(item->path)/"sound.json","oscairn.sound");info.revision=part.from.revision;}
    if(!name.empty())info.name=name;
    if(info.name.empty())info.name=part.name;
    SoundSaved saved;
    PerformanceInfo written;
    if(!item){
        const auto folder=l.newFolder(info.name,"sound");
        fs::create_directories(folder.parent_path());
        written=writeDocument(folder.string(),info,&part,&saved.left);
    }else{
        const fs::path folder=item->path;
        const auto fresh=folder.parent_path()/("."+folder.filename().string()+".new"),old=folder.parent_path()/("."+folder.filename().string()+".old");
        fs::remove_all(fresh);fs::remove_all(old);
        written=writeDocument(fresh.string(),info,&part,&saved.left);
        fs::rename(folder,old);fs::rename(fresh,folder);fs::remove_all(old);
    }
    l.rescan();
    saved.id=written.id;saved.revision=written.revision;
    {
        Quiet quiet(*this); // where the part came from changes, but saving is not an edit
        findPart(partId).from={written.id,written.revision,written.name,false};
    }
    ++revision_;publish(ChangeKind::Graph); // clients see where the part comes from now
    return saved;
}

std::vector<std::string> Session::loadSound(uint32_t partId,const std::string& id) {
    auto& l=library();
    const auto* item=l.find(id);
    if(!item || item->kind!="sound")bad("no such Sound in the library");
    const auto path=item->path;
    auto flagged=loadSoundInto(partId,path);
    l.used(id);
    return flagged;
}

uint32_t Session::partFromSound(const std::string& id) {
    auto& l=library();
    const auto* item=l.find(id);
    if(!item || item->kind!="sound")bad("no such Sound in the library");
    const auto name=item->name;
    const auto part=addPart(name,{},std::nullopt);commit();
    try{loadSound(part,id);}catch(...){removePart(part);commit();throw;}
    return part;
}

Control::PerformanceContents Session::performanceContents(const std::string& directory) {
    if(directory.empty() || directory.find('\0')!=std::string::npos)bad("invalid directory");
    const auto doc=document(fs::canonical(directory));V d=doc.get();
    if(text(field(d,"format"))!="oscairn.performance" || integer(field(d,"version"))!=11)bad("unsupported format version");
    PerformanceContents out;
    for(auto p:array(field(d,"parts"),256))out.parts.push_back({integer(field(p,"id")),text(field(p,"name")),text(field(field(p,"input"),"kind"))=="bus"});
    for(auto s:array(field(d,"scenes"),256))out.scenes.push_back({integer(field(s,"id")),text(field(s,"name"))});
    return out;
}

// Another Performance, or chosen parts and scenes of it, into this one (RFC 0021). A chosen part
// brings its plugins, the buses it plays through or sends to (and theirs), the wires and
// mappings among them, the controls it owns, and the Performance's controls whose every
// target comes too; plugins no chosen part uses stay behind. A partial import takes at least
// one part and, when there are any, one scene; a chosen scene comes under a free name, with
// what it holds for what comes. All of it brings every scene. This Performance's scenes keep the arriving parts as
// they arrive. Pads, tempo and its info stay behind. One
// step to undo; nothing playing is cut but what the graph's rebuild cuts.
Control::Imported Session::importPerformance(const std::string& directory,const std::vector<uint32_t>& chosen,const std::vector<uint32_t>& chosenScenes) {
    if(staged_)bad("commit staged graph edits before importing");
    record(); // an edit waiting for its step comes first: the import is a step of its own
    if(directory.empty() || directory.find('\0')!=std::string::npos)bad("invalid directory");
    const auto root=fs::canonical(directory);validateTree(root);const auto doc=document(root);V d=doc.get();
    fields(d,{"format","version","info","libraries","transport","audioInputs","audioOutputs","nodes","wires","parts","mappings","pads","key","master","scenes","sceneUntouched","scene","controls","assignments","lock"});
    mergeLock(readLock(d));
    if(text(field(d,"format"))!="oscairn.performance" || integer(field(d,"version"))!=11)bad("unsupported format version");
    fields(field(d,"info"),{"id","name","description","tags","color","revision","parent","hash","origin"});
    // The parts that come: those chosen, and the buses they play through or send to, and theirs.
    const auto partList=array(field(d,"parts"),256);
    std::map<uint32_t,V> byId;
    for(auto p:partList){const auto id=integer(field(p,"id"));if(!id || !byId.emplace(id,p).second)bad("invalid or duplicate part");}
    const bool whole=chosen.empty() && chosenScenes.empty();
    // A partial import takes at least one part, and one scene when it has any: the scene says
    // how what comes is set (RFC 0021).
    if(!whole && chosen.empty())bad("choose at least one part to import");
    if(!whole && chosenScenes.empty() && !array(field(d,"scenes"),256).empty())bad("choose at least one scene to import");
    std::set<uint32_t> taking;
    std::function<void(uint32_t)> take=[&](uint32_t id){
        if(!byId.count(id))bad("no part "+std::to_string(id)+" in that Performance");
        if(!taking.insert(id).second)return;
        const auto p=byId.at(id);
        if(const auto bus=integer(field(p,"bus")))take(bus);
        for(auto s:array(field(p,"sends"),256))take(integer(field(s,"bus")));};
    if(whole)for(const auto& [id,p]:byId)taking.insert(id);else for(const auto id:chosen)take(id);
    std::set<std::string> keys;
    auto keysIn=[&](V list){for(auto n:array(list,256))if(n->type==json_type_string)keys.insert(text(n));};
    for(const auto id:taking){
        const auto p=byId.at(id);keysIn(field(p,"chain"));
        for(auto sp:array(field(p,"splits"),256)){
            for(const char* k:{"splitter","joiner"})if(const auto v=field(sp,k);v->type==json_type_string)keys.insert(text(v));
            for(auto l:array(field(sp,"lanes"),16))keysIn(field(l,"chain"));}}
    std::vector<NodeId> added;
    auto ids=addNodes(d,root,added,whole?nullptr:&keys);
    ids.emplace("input",graphInput);ids.emplace("output",graphOutput);
    Imported result;auto& notes=result.notes;
    std::vector<Part> parts;std::vector<Wire> users,mixes;std::vector<Graph::Mapping> mappings;std::vector<Macro> macros;
    std::vector<SceneParameter> untouched;std::vector<Scene> scenes;
    std::map<uint32_t,uint32_t> partIds,controlIds,sceneIds; // the file's IDs to this Performance's
    try{
        auto has=[&](V v){return ids.count(text(v))!=0;};
        auto node=[&](V v){const auto key=text(v);const auto it=ids.find(key);if(it==ids.end())bad("unknown node reference "+key);return it->second;};
        auto parameterOf=[&](NodeId n,uint32_t param){const auto& all=graph_.host(n).parameters();if(std::none_of(all.begin(),all.end(),[&](const auto& q){return q.id==param;}))bad("unknown parameter "+std::to_string(param));};
        const auto layout=graph_.describe().audioLayout;
        auto onPort=[](const auto& list,uint32_t id){return std::any_of(list.begin(),list.end(),[&](const auto& x){return x.id==id;});};
        uint32_t nextId=nextPart_;
        for(auto p:partList)if(taking.count(integer(field(p,"id"))))partIds[integer(field(p,"id"))]=nextId++;
        const auto sceneList=array(field(d,"scenes"),256);
        {
            std::set<uint32_t> inFile;for(auto s:sceneList)inFile.insert(integer(field(s,"id")));
            for(const auto id:chosenScenes)if(!inFile.count(id))bad("no scene "+std::to_string(id)+" in that Performance");
            uint32_t nextScene=nextScene_;
            for(auto s:sceneList){const auto id=integer(field(s,"id"));if(whole || std::find(chosenScenes.begin(),chosenScenes.end(),id)!=chosenScenes.end())sceneIds[id]=nextScene++;}
        }
        std::function<std::vector<NodeId>(V)> chain=[&](V list){std::vector<NodeId> out;for(auto n:array(list,256)){
            if(n->type==json_type_object){fields(n,{"split"});const auto x=integer(field(n,"split"));if(!x || x>=splitMark)bad("invalid split entry");out.push_back(splitMark|x);}
            else out.push_back(node(n));}return out;};
        for(auto p:partList){
            const auto fileId=integer(field(p,"id"));if(!taking.count(fileId))continue;
            fields(p,{"id","name","input","chain","splits","output","bus","sends","level","pan","mute","solo","midiOut","key","from"});
            Part part;part.id=partIds.at(fileId);part.name=text(field(p,"name"));
            const auto input=field(p,"input");fields(input,{"kind","port","zones","devices","maxNotes"});const auto kind=text(field(input,"kind"));
            part.input.kind=kind=="notes"?PartInput::Kind::Notes:kind=="audio"?PartInput::Kind::Audio:kind=="none"?PartInput::Kind::None:kind=="bus"?PartInput::Kind::Bus:(bad("invalid part input"),PartInput::Kind::None);
            part.input.port=integer(field(input,"port"));part.input.zones=zonesFrom(field(input,"zones"));part.input.devices=devicesFrom(field(input,"devices"));part.input.maxNotes=uint16_t(integer(field(input,"maxNotes"),128));part.midiOut=midiOutFrom(field(p,"midiOut"));if(const V k=field(p,"key");k->type!=json_type_null)part.key=keyFrom(k);
            if(part.input.kind==PartInput::Kind::Audio && !onPort(layout.inputs,part.input.port)){
                if(layout.inputs.empty())bad(part.name+" listens to an audio input this device does not have");
                part.input.port=layout.inputs.front().id;notes.push_back(part.name+" listens to the first audio input: this device has no input "+std::to_string(integer(field(input,"port"))));}
            part.output=integer(field(p,"output"));
            if(!onPort(layout.outputs,part.output)){notes.push_back(part.name+" plays to the first output: this device has no output "+std::to_string(part.output));part.output=layout.outputs.front().id;}
            part.chain=chain(field(p,"chain"));
            for(auto x:array(field(p,"splits"),256)){
                fields(x,{"id","kind","splitter","joiner","lanes"});Split split;split.id=integer(field(x,"id"));
                split.kind=Split::Kind(named(splitKinds,text(field(x,"kind")),"invalid split kind"));
                for(auto [name,at]:{std::pair{"splitter",&split.splitter},std::pair{"joiner",&split.joiner}}){const auto v=field(x,name);*at=v->type==json_type_null?0:node(v);}
                for(auto l:array(field(x,"lanes"),16)){
                    fields(l,{"chain","zones","level","pan","mute","solo"});Lane lane;lane.chain=chain(field(l,"chain"));lane.zones=zonesFrom(field(l,"zones"));
                    lane.level=number(field(l,"level"));lane.pan=number(field(l,"pan"));lane.mute=boolean(field(l,"mute"));lane.solo=boolean(field(l,"solo"));
                    if(lane.level<0 || lane.pan<-1 || lane.pan>1)bad("invalid lane level or pan");split.lanes.push_back(std::move(lane));}
                part.splits.push_back(std::move(split));
            }
            if(const auto bus=integer(field(p,"bus")))part.bus=partIds.at(bus);
            for(auto s:array(field(p,"sends"),256)){fields(s,{"bus","level"});Send send{partIds.at(integer(field(s,"bus"))),number(field(s,"level"))};if(send.level<0)bad("invalid send level");part.sends.push_back(send);}
            part.level=number(field(p,"level"));part.pan=number(field(p,"pan"));part.mute=boolean(field(p,"mute"));part.solo=boolean(field(p,"solo"));
            if(part.level<0 || part.pan<-1 || part.pan>1)bad("invalid part level or pan");
            if(const auto from=field(p,"from");from->type!=json_type_null){
                fields(from,{"sound","revision","name","modified"});
                part.from={text(field(from,"sound")),text(field(from,"revision")),text(field(from,"name")),boolean(field(from,"modified"))};}
            parts.push_back(std::move(part));
        }
        // Wires among what comes: user wires as they were, part wires for their Mix.
        for(auto c:array(field(d,"wires"),Graph::maxAudioConnections)){
            const auto owner=text(field(c,"owner"));
            if(owner=="part" && !taking.count(integer(field(c,"part"))))continue;
            if(!has(field(c,"from")) || !has(field(c,"to")))continue;
            Wire w;w.kind=text(field(c,"kind"))=="audio"?WireKind::Audio:WireKind::Notes;w.owner=owner=="part"?WireOwner::Part:WireOwner::User;
            w.from=node(field(c,"from"));w.to=node(field(c,"to"));w.fromPort=integer(field(c,"fromPort"));w.toPort=integer(field(c,"toPort"));
            if(w.owner==WireOwner::User && (w.from==graphInput || w.from==graphOutput) && (w.to==graphInput || w.to==graphOutput))continue;
            for(auto pair:array(field(c,"channels"),Graph::maxAudioChannels)){const auto ends=array(pair,2);if(ends.size()!=2)bad("invalid channel pair");w.channels.push_back({integer(ends[0],Graph::maxAudioChannels-1),integer(ends[1],Graph::maxAudioChannels-1)});}
            w.level=number(field(c,"level"));w.pan=number(field(c,"pan"));w.mute=boolean(field(c,"mute"));w.solo=boolean(field(c,"solo"));
            if(w.level<0 || w.pan<-1 || w.pan>1)bad("invalid wire level or pan");
            w.zones=zonesFrom(field(c,"zones"));w.enabled=boolean(field(c,"enabled"));
            (w.owner==WireOwner::User?users:mixes).push_back(std::move(w));
        }
        for(auto m:array(field(d,"mappings"),4096)){
            fields(m,{"from","lane","to","param"});if(!has(field(m,"from")) || !has(field(m,"to")))continue;
            Graph::Mapping x;x.source=node(field(m,"from"));x.lane=integer(field(m,"lane"));x.target=node(field(m,"to"));x.param=integer(field(m,"param"));parameterOf(x.target,x.param);mappings.push_back(x);}
        // Controls: a part's with its part; the Performance's when every target comes too.
        const auto controlList=array(field(d,"controls"),4096);
        std::set<uint32_t> coming;
        for(bool grew=true;grew;){
            grew=false;
            for(auto m:controlList){
                const auto id=integer(field(m,"id"));if(coming.count(id))continue;
                const auto owner=integer(field(m,"part"));
                bool comes=whole || (owner && taking.count(owner));
                if(!comes && !owner){
                    comes=true;
                    for(auto t:array(field(m,"targets"),256)){
                        const auto kind=text(field(t,"kind"));
                        if(kind=="parameter")comes=comes && has(field(t,"node"));
                        else if(kind=="partLevel" || kind=="partMute")comes=comes && taking.count(integer(field(t,"part")));
                        else comes=comes && coming.count(integer(field(t,"control")));}
                }
                if(comes){coming.insert(id);grew=true;}
            }
        }
        uint32_t nextControl=nextMacro_;
        for(auto m:controlList)if(coming.count(integer(field(m,"id"))))controlIds[integer(field(m,"id"))]=nextControl++;
        auto takenName=[&](const std::string& n){return std::any_of(macros_.begin(),macros_.end(),[&](const Macro& x){return x.name==n;}) || std::any_of(macros.begin(),macros.end(),[&](const Macro& x){return x.name==n;});};
        for(auto m:controlList){
            fields(m,{"id","part","name","value","targets"});const auto fileId=integer(field(m,"id"));if(!coming.count(fileId))continue;
            Macro macro;macro.id=controlIds.at(fileId);macro.value=number(field(m,"value"));if(macro.value<0 || macro.value>1)bad("invalid control value");
            if(const auto owner=integer(field(m,"part")))macro.part=partIds.at(owner);
            const auto name=text(field(m,"name"));macro.name=nextFree(name,takenName);
            if(macro.name!=name)notes.push_back("the control "+name+" is "+macro.name+" here");
            for(auto t:array(field(m,"targets"),256)){
                MacroTarget target;const auto kind=text(field(t,"kind"));
                if(kind=="parameter"){target.kind=MacroTarget::Kind::Parameter;target.node=node(field(t,"node"));target.param=integer(field(t,"param"));parameterOf(target.node,target.param);}
                else if(kind=="partLevel" || kind=="partMute"){target.kind=kind=="partLevel"?MacroTarget::Kind::PartLevel:MacroTarget::Kind::PartMute;target.part=partIds.at(integer(field(t,"part")));}
                else if(kind=="control"){target.kind=MacroTarget::Kind::Macro;target.macro=controlIds.at(integer(field(t,"control")));}
                else if(kind=="morph"){
                    // Between scenes that come too; one that does not loses the target.
                    const auto f=sceneIds.find(integer(field(t,"from"))),g=sceneIds.find(integer(field(t,"to")));
                    if(f==sceneIds.end() || g==sceneIds.end())continue;
                    target.kind=MacroTarget::Kind::Morph;target.sceneFrom=f->second;target.sceneTo=g->second;}
                else bad("invalid control target kind");
                if(!detail::curveFrom(field(t,"curve"),target))bad("invalid curve");
                target.start=number(field(t,"start"));target.end=number(field(t,"end"));target.relative=boolean(field(t,"relative"));target.base=number(field(t,"base"));
                for(auto id:array(field(t,"offIn"),256))if(const auto it=sceneIds.find(integer(id));it!=sceneIds.end())target.offIn.push_back(it->second);
                macro.targets.push_back(target);
            }
            macros.push_back(std::move(macro));
        }
        // Scenes: all with all of it, else those chosen, each under a name free here, holding
        // only what comes.
        {
            if(!sceneIds.empty())for(auto at:array(field(d,"sceneUntouched"),65536)){fields(at,{"node","param"});if(!has(field(at,"node")))continue;SceneParameter x{node(field(at,"node")),integer(field(at,"param"))};parameterOf(x.node,x.param);untouched.push_back(x);}
            auto takenScene=[&](const std::string& n){return std::any_of(scenes_.begin(),scenes_.end(),[&](const Scene& x){return x.name==n;}) || std::any_of(scenes.begin(),scenes.end(),[&](const Scene& x){return x.name==n;});};
            // A wire whose ends and part come; its part numbered here.
            auto ref=[&](V r)->std::optional<WireRef>{
                if(!has(field(r,"from")) || !has(field(r,"to")))return std::nullopt;
                auto x=wireRef(r,node);
                if(x.part){const auto it=partIds.find(x.part);if(it==partIds.end())return std::nullopt;x.part=it->second;}
                if(x.send){const auto it=partIds.find(x.send);if(it==partIds.end())return std::nullopt;x.send=it->second;}
                return x;};
            for(auto s:sceneList){
                const auto fileId=integer(field(s,"id"));if(!sceneIds.count(fileId))continue;
                fields(s,{"id","name","scope","glide","curve","launch","off","offWires","parts","wires","values","controls","master"});Scene scene;scene.id=sceneIds.at(fileId);
                sceneTransition(s,scene);
                const auto name=text(field(s,"name"));scene.name=nextFree(name,takenScene);
                if(scene.name!=name)notes.push_back("the scene "+name+" is "+scene.name+" here");
                scene.scope=sceneScope(field(s,"scope"));
                for(auto n:array(field(s,"off"),256))if(has(n))scene.off.push_back(node(n));
                for(auto r:array(field(s,"offWires"),Graph::maxAudioConnections))if(const auto x=ref(r))scene.offWires.push_back(*x);
                for(auto p:array(field(s,"parts"),256)){
                    const auto it=partIds.find(integer(field(p,"part")));if(it==partIds.end())continue;
                    Scene::PartState st{it->second,number(field(p,"level")),number(field(p,"pan")),boolean(field(p,"mute")),boolean(field(p,"solo")),{},{}};
                    for(auto x:array(field(p,"sends"),256))if(const auto bus=partIds.find(integer(field(x,"bus")));bus!=partIds.end())st.sends.push_back({bus->second,number(field(x,"level"))});
                    for(auto l:array(field(p,"lanes"),4096))st.lanes.push_back({integer(field(l,"split")),integer(field(l,"lane")),number(field(l,"level")),number(field(l,"pan")),boolean(field(l,"mute")),boolean(field(l,"solo"))});
                    scene.parts.push_back(std::move(st));}
                for(auto w:array(field(s,"wires"),Graph::maxAudioConnections))if(const auto x=ref(w))scene.wires.push_back({*x,number(field(w,"level")),number(field(w,"pan")),boolean(field(w,"mute")),boolean(field(w,"solo"))});
                for(auto v:array(field(s,"values"),65536)){fields(v,{"node","param","value"});if(has(field(v,"node")))scene.values.push_back({{node(field(v,"node")),integer(field(v,"param"))},number(field(v,"value"))});}
                for(auto c:array(field(s,"controls"),4096)){fields(c,{"control","value"});const auto it=controlIds.find(integer(field(c,"control")));if(it!=controlIds.end())scene.macros.push_back({it->second,number(field(c,"value"))});}
                std::tie(scene.masterLevel,scene.masterPan)=masterFrom(field(s,"master"));
                scenes.push_back(std::move(scene));
            }
        }
    }catch(const std::out_of_range&){
        for(const auto n:added)dropNode(n);graph_.commit();staged_=false;bad("a reference to something not in that Performance");
    }catch(...){for(const auto n:added)dropNode(n);graph_.commit();staged_=false;throw;}
    // Nothing has changed yet; now the parts and their wiring, rebuilt with every part.
    {
        auto partsBefore=parts_;auto wiresBefore=wires_;
        try{
            parts_.insert(parts_.end(),parts.begin(),parts.end());
            for(auto w:users){w.id=nextWire_++;wires_.push_back(std::move(w));}
            buildParts();
            for(auto& w:wires_)if(w.owner==WireOwner::Part && std::any_of(parts.begin(),parts.end(),[&](const Part& p){return p.id==w.part;}))
                for(const auto& m:mixes)if(m.kind==w.kind && m.from==w.from && m.to==w.to && m.fromPort==w.fromPort && m.toPort==w.toPort){w.level=m.level;w.pan=m.pan;w.mute=m.mute;w.solo=m.solo;w.enabled=m.enabled;break;}
            syncWires();
        }catch(...){
            parts_.swap(partsBefore);wires_.swap(wiresBefore);syncWires();
            for(const auto n:added){std::erase_if(wires_,[&](const Wire& w){return w.from==n || w.to==n;});dropNode(n);}
            throw;
        }
    }
    for(const auto& p:parts){nextPart_=std::max(nextPart_,p.id+1);result.parts.push_back(p.id);}
    for(const auto& m:mappings)graph_.mapParamTarget(m.source,m.lane,m.target,m.param);
    for(const auto& m:macros)nextMacro_=std::max(nextMacro_,m.id+1);
    macros_.insert(macros_.end(),macros.begin(),macros.end());
    noteBases();
    untouched_.insert(untouched_.end(),untouched.begin(),untouched.end());
    // This Performance's scenes keep what arrives as it arrives; the arriving scenes keep what was
    // here as it is now.
    {
        Scene now;capture(now);
        const std::set<NodeId> arriving(added.begin(),added.end());
        std::set<uint32_t> newParts,newControls;for(const auto& p:parts)newParts.insert(p.id);for(const auto& m:macros)newControls.insert(m.id);
        auto comes=[&](const WireRef& r){return arriving.count(r.from) || arriving.count(r.to) || newParts.count(r.part);};
        auto fill=[&](Scene& scene,bool takeArriving){
            for(const auto n:now.off)if(arriving.count(n)==takeArriving)scene.off.push_back(n);
            for(const auto& r:now.offWires)if(comes(r)==takeArriving)scene.offWires.push_back(r);
            for(const auto& st:now.parts)if(newParts.count(st.part)==takeArriving && std::none_of(scene.parts.begin(),scene.parts.end(),[&](const auto& x){return x.part==st.part;}))scene.parts.push_back(st);
            for(const auto& m:now.wires)if(comes(m.wire)==takeArriving && std::none_of(scene.wires.begin(),scene.wires.end(),[&](const auto& x){return x.wire==m.wire;}))scene.wires.push_back(m);
            for(const auto& v:now.values)if(arriving.count(v.at.node)==takeArriving && std::none_of(scene.values.begin(),scene.values.end(),[&](const auto& x){return x.at==v.at;}))scene.values.push_back(v);
            for(const auto& v:now.macros)if(newControls.count(v.first)==takeArriving && std::none_of(scene.macros.begin(),scene.macros.end(),[&](const auto& x){return x.first==v.first;}))scene.macros.push_back(v);
        };
        for(auto& scene:scenes_)fill(scene,true);
        for(auto& scene:scenes){fill(scene,false);scene.masterLevel=now.masterLevel;scene.masterPan=now.masterPan;} // the master stays this Performance's
    }
    for(const auto& s:scenes)nextScene_=std::max(nextScene_,s.id+1);
    scenes_.insert(scenes_.end(),scenes.begin(),scenes.end());
    commit(); // one step to undo
    return result;
}

namespace {
// A name, description, tags and color the library can hold.
void checkInfo(const Control::PerformanceInfo& info) {
    if(info.name.size()>256 || info.description.size()>4096 || info.tags.size()>32 ||
       std::any_of(info.tags.begin(),info.tags.end(),[](const std::string& t){return t.empty() || t.size()>64 || t.find('\0')!=std::string::npos;}) ||
       info.name.find('\0')!=std::string::npos || info.description.find('\0')!=std::string::npos || !validColor(info.color))bad("invalid info");
}
}

void Session::setPerformanceInfo(const PerformanceInfo& info) {
    checkInfo(info);
    const auto id=info_.id,revision=info_.revision,parent=info_.parent;const auto origin=info_.origin;info_=info;info_.id=id;info_.revision=revision;info_.parent=parent;info_.origin=origin;
}

std::string Session::savePerformanceToLibrary(const std::string& name,bool asNew) {
    auto& l=library();
    // Save writes over the open library Performance; Save As New, or one not from the library or
    // from a locked library, makes a new one.
    if(const auto* open=info_.id.empty()?nullptr:l.find(info_.id);open && !open->library.empty())asNew=true;
    if(asNew){info_.id.clear();info_.revision.clear();info_.origin={};}
    if(!name.empty())info_.name=name;
    const auto* item=info_.id.empty()?nullptr:l.find(info_.id);
    const auto folder=item?fs::path(item->path):l.newFolder(info_.name.empty()?"Performance":info_.name,"performance");
    fs::create_directories(folder.parent_path());
    if(!item)info_=writeDocument(folder.string(),info_,nullptr,nullptr);
    else{
        // Saving refuses an existing folder, so save beside it and swap; the old one goes only once the new is in place.
        const auto fresh=folder.parent_path()/("."+folder.filename().string()+".new"),old=folder.parent_path()/("."+folder.filename().string()+".old");
        fs::remove_all(fresh);fs::remove_all(old);
        info_=writeDocument(fresh.string(),info_,nullptr,nullptr);
        fs::rename(folder,old);fs::rename(fresh,folder);fs::remove_all(old);
    }
    l.rescan();l.used(info_.id);
    return info_.id;
}

// Libraries (RFC 0021). An archive is unpacked into a folder of its own and checked before
// anything is installed; it goes again afterwards.
namespace {
// Unpacked inside the data folder, not the OS temp folder: on devices that mount /tmp as tmpfs,
// a big archive would fill RAM on the way in, and copies across file systems cannot be renames.
fs::path unpackFolder(){std::random_device random;return dataHome()/(".install-"+std::to_string(random())+std::to_string(random()));}
std::string freshId(){std::random_device random;constexpr char hex[]="0123456789abcdef";std::string id;for(int i=0;i<16;++i)id+=hex[random()&15];return id;}
}

Control::Archive Session::archiveContents(const std::string& file) {
    const auto p=readPackage(file,{}); // the manifest only
    Archive out{p.id,p.name,p.kind,{},{},judge("archive",p.signature.key.empty()?std::vector<signing::Signature>{}:std::vector<signing::Signature>{p.signature})};
    for(const auto& i:p.items){LibraryItem item;item.kind=i.kind;item.id=i.id;item.name=i.name;out.items.push_back(item);}
    // The code it would add: each plugin build not already in the store. Apps join this when archives carry them (RFC 0022 step 2).
    for(const auto& plugin:p.plugins){
        const auto there=storedBuilds(plugin.format,plugin.id,plugin.version);
        for(const auto& b:plugin.builds)if(!there.count(b.platform)){auto s=judge("plugin",b.signatures);s.name=plugin.name;s.version=plugin.version;s.platform=b.platform;out.code.push_back(s);}
    }
    return out;
}

Control::Installed Session::installArchive(const std::string& file,bool asLibrary,const std::vector<std::string>& chosen,bool acceptUnsignedCode) {
    auto& l=library();
    const auto into=unpackFolder();
    const auto p=readPackage(file,into);
    Installed out;
    std::vector<fs::path> created; // item folders this call copies, undone if it fails
    bool freshRoot=false;
    try{
        std::vector<Package::Item> items;
        for(const auto& id:chosen)if(std::none_of(p.items.begin(),p.items.end(),[&](const auto& i){return i.id==id;}))bad("no item "+id+" in that archive");
        for(const auto& i:p.items)if(chosen.empty() || std::find(chosen.begin(),chosen.end(),i.id)!=chosen.end())items.push_back(i);
        // On a case-insensitive file system an ID differing only in case names another library's
        // folder: never two libraries in one, which rescan would list as neither.
        if(asLibrary){std::error_code e;for(const auto& entry:fs::directory_iterator(l.libraries(),e))
            if(entry.path().filename()!=p.id && fs::equivalent(entry.path(),l.libraries()/p.id,e))bad("another library is installed in that folder: "+entry.path().filename().string());}
        auto manifest=[&](const Package::Item& i){return into/i.path/(i.kind=="sound"?"sound.json":"performance.json");};
        // Each item is what it says it is.
        for(const auto& i:items){const auto info=readInfo(manifest(i),"oscairn."+i.kind);if(info.id!=i.id)bad("the item "+i.name+" is not the one its archive lists");}
        // The plugins they use, and who signed each build that would go into the store (RFC 0006),
        // before anything is installed: code unsigned or by an unknown key only when the user said
        // yes, a revoked key's never. Apps join this when archives carry them (RFC 0022 step 2).
        std::set<std::tuple<std::string,std::string,std::string>> used;
        for(const auto& i:items){
            const auto bytes=state_files::read(manifest(i),4*1024*1024);
            const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
            for(auto node:array(field(doc.get(),"nodes"),256)){const V plugin=field(node,"plugin");used.insert({text(field(plugin,"format")),text(field(plugin,"id")),text(field(plugin,"version"))});}
        }
        std::set<std::string> revoked; // build paths not installed
        std::string unknown;
        for(const auto& plugin:p.plugins){
            if(!used.count({plugin.format,plugin.id,plugin.version}))continue;
            const auto there=storedBuilds(plugin.format,plugin.id,plugin.version);
            for(const auto& build:plugin.builds){
                if(there.count(build.platform))continue;
                auto s=judge("plugin",build.signatures);s.name=plugin.name;s.version=plugin.version;s.platform=build.platform;
                const auto what=plugin.name+" "+plugin.version+" ("+build.platform+")";
                if(s.status=="revoked"){revoked.insert(build.path);out.notes.push_back(what+" is signed by a revoked key ("+s.key+"); it was not installed");}
                else if(s.status!="trusted")unknown+=(unknown.empty()?"":", ")+what+(s.status=="unsigned"?" unsigned":" by an unknown key "+s.key);
                out.code.push_back(s);
            }
        }
        if(!unknown.empty() && !acceptUnsignedCode)
            bad("it runs code from an unknown source: "+unknown+"; install it accepting unsigned code to go ahead");
        // The samples they use, into the user's samples by their hash, so they stay when a library goes.
        for(const auto& i:items)for(const auto& ref:assetsOf(manifest(i))){
            const auto folder=into/"Assets"/ref.sha256.substr(0,16);
            bool found=false;std::error_code error;
            if(fs::is_directory(folder,error))for(const auto& f:fs::directory_iterator(folder,error))if(f.is_regular_file() && assets_.import(f.path()).sha256==ref.sha256)found=true;
            if(!found){auto copy=ref;try{assets_.resolve(copy);}catch(const std::exception&){out.notes.push_back(i.name+" uses a sample that is not in the archive or on this device: "+ref.path);}}
        }
        // The plugin builds they use, every platform's, into the store (RFC 0006), each whole or not
        // at all; a different build already there is kept, a revoked key's left out.
        for(const auto& plugin:p.plugins){
            if(!used.count({plugin.format,plugin.id,plugin.version}))continue;
            if(std::none_of(plugin.builds.begin(),plugin.builds.end(),[](const auto& b){return b.platform==platform();}))
                out.notes.push_back(plugin.name+" "+plugin.version+" has no build for this device in the archive");
            const auto there=storedBuilds(plugin.format,plugin.id,plugin.version);
            for(const auto& build:plugin.builds){
                if(const auto at=there.find(build.platform);at!=there.end()){
                    if(bundleDigest(at->second)!=build.sha256)out.notes.push_back("a different "+build.platform+" build of "+plugin.name+" "+plugin.version+" is already installed; it was kept");
                    continue;
                }
                if(!revoked.count(build.path))storeBuild(plugin.format,plugin.id,plugin.version,plugin.name,build.platform,into/build.path,build.signatures);
            }
        }
        if(asLibrary){
            // Locked, in its own folder: what is there stays, what is missing comes back.
            const auto root=l.libraries()/p.id; // readPackage: the ID is a plain name
            freshRoot=!fs::exists(root);
            fs::create_directories(root);
            for(const auto& i:items){
                const auto to=root/i.path;
                if(fs::exists(fs::symlink_status(to))){++out.skipped;continue;} // a link there too: never followed or removed
                // Recorded before the copy, so one that fails partway comes back out.
                fs::create_directories(to.parent_path());created.push_back(to);fs::copy(into/i.path,to,fs::copy_options::recursive);++out.added;
            }
            // Listed only once its items are all there.
            writeAtomically(root/"library.json",manifestJson(p));
        }else{
            // The user's own, under new IDs: Sounds first, so the Performances that came with them point at their copies.
            std::map<std::string,std::string> ids; // the archive's item ID -> the copy's
            std::stable_sort(items.begin(),items.end(),[](const auto& a,const auto& b){return a.kind=="sound" && b.kind!="sound";});
            l.rescan();
            for(const auto& i:items){
                const auto copy=std::find_if(l.items().begin(),l.items().end(),[&](const LibraryItem& x){return x.library.empty() && x.origin.library==p.id && x.origin.item==i.id;});
                if(copy!=l.items().end() && copy->hash==copy->origin.hash){ids[i.id]=copy->id;++out.skipped;continue;} // still as imported
                const auto to=l.newFolder(i.name,i.kind);
                fs::create_directories(to.parent_path());created.push_back(to);fs::copy(into/i.path,to,fs::copy_options::recursive);
                const auto doc=to/(i.kind=="sound"?"sound.json":"performance.json");
                if(i.kind=="performance"){
                    // Parts point at the Sounds' copies. The span comes from the needle itself: an
                    // ID with a quote escapes longer than it reads, and a fixed length breaks the JSON.
                    // The search goes on after the replacement, which may hold the needle itself.
                    const auto bytes=state_files::read(doc,4*1024*1024);std::string text(bytes.begin(),bytes.end());
                    for(const auto& [from,id]:ids){const std::string needle="\"sound\":"+quote(from),with="\"sound\":"+quote(id);for(auto at=text.find(needle);at!=std::string::npos;at=text.find(needle,at+with.size()))text.replace(at,needle.size(),with);}
                    state_files::write(doc,text.data(),text.size());
                }
                auto info=readInfo(doc,"oscairn."+i.kind);
                info.id=freshId();info.parent.clear();info.revision=freshId();
                info.hash=contentHash(doc);info.origin={p.id,i.id,info.hash};
                writeInfo(doc,info);
                ids[i.id]=info.id;++out.added;
            }
        }
    }catch(...){
        // The unpacked folder, this call's item copies, and a library folder it made itself: nothing half stays.
        std::error_code ignored;
        for(const auto& made:created)fs::remove_all(made,ignored);
        if(freshRoot)fs::remove_all(l.libraries()/p.id,ignored);
        fs::remove_all(into,ignored);l.rescan();throw;
    }
    std::error_code ignored;fs::remove_all(into,ignored);
    l.rescan();
    return out;
}

std::vector<Control::InstalledLibrary> Session::libraries() { return library().installed(); }

void Session::uninstallLibrary(const std::string& id) {
    auto& l=library();
    const auto all=l.installed();
    if(std::none_of(all.begin(),all.end(),[&](const auto& x){return x.id==id;}))bad("no such library installed");
    fs::remove_all(l.libraries()/id); // readPackage: the ID is a plain name
    l.rescan();
}

std::vector<Control::InstalledPlugin> Session::installedPlugins() {
    // What each library item uses. shortcut: reads every item's document on each call; keep it in
    // the library's index when libraries reach thousands of items.
    std::vector<std::pair<std::string,std::set<std::tuple<std::string,std::string,std::string>>>> uses;
    for(const auto& item:library().items())try{
        const auto bytes=state_files::read(fs::path(item.path)/(item.kind=="sound"?"sound.json":"performance.json"),4*1024*1024);
        const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
        if(!doc)continue;
        std::set<std::tuple<std::string,std::string,std::string>> plugins;
        for(auto node:array(field(doc.get(),"nodes"),256)){const V plugin=field(node,"plugin");plugins.insert({text(field(plugin,"format")),text(field(plugin,"id")),text(field(plugin,"version"))});}
        uses.push_back({item.name.empty()?"Untitled":item.name,std::move(plugins)});
    }catch(const std::exception&){}
    std::vector<InstalledPlugin> out;
    std::error_code e;
    for(const auto* format:{"clap","lv2"})
        if(fs::is_directory(dataHome()/"Plugins"/format,e))for(const auto& entry:fs::directory_iterator(dataHome()/"Plugins"/format,e)){
            const auto leaf=entry.path().filename().string();
            if(entry.is_directory() && leaf.front()=='.' && leaf.size()>9 && leaf.compare(leaf.size()-9,9,".removing")==0){std::error_code ignored;fs::remove_all(entry.path(),ignored);} // a removal cut short
            if(!entry.is_directory() || leaf.front()=='.')continue;
            auto p=storedIdentity(entry.path());
            if(p.id.empty() || p.format!=format || storeFolder(p.format,p.id,p.version)!=entry.path())continue;
            for(const auto& [forPlatform,build]:storedBuilds(p.format,p.id,p.version)){
                p.platforms.push_back(forPlatform);
                if(fs::is_regular_file(build,e))p.bytes+=fs::file_size(build,e);
                else for(const auto& f:fs::recursive_directory_iterator(build,e))if(f.is_regular_file())p.bytes+=f.file_size();
            }
            for(const auto& [name,plugins]:uses)if(plugins.count({p.format,p.id,p.version}))p.usedBy.push_back(name);
            const auto here=(entry.path()/platform()).string()+"/";
            for(const auto& n:nodes_)if(graph_.host(n.first).descriptor().path.rfind(here,0)==0 && graph_.host(n.first).failure().empty())p.open=true; // a stand-in plays nothing
            out.push_back(std::move(p));
        }
    std::sort(out.begin(),out.end(),[](const auto& a,const auto& b){return std::tie(a.name,a.version)<std::tie(b.name,b.version);});
    return out;
}

void Session::removePlugin(const std::string& format,const std::string& id,const std::string& version,const std::string& forPlatform) {
    if(!forPlatform.empty() && !knownPlatform(forPlatform))bad("unknown platform "+forPlatform);
    const auto folder=storeFolder(format,id,version);
    const auto p=storedIdentity(folder);
    if(p.id!=id || p.version!=version || p.format!=format)bad("that plugin is not installed");
    if(forPlatform.empty() || forPlatform==platform()){
        const auto here=(folder/platform()).string()+"/";
        for(const auto& n:nodes_)if(graph_.host(n.first).descriptor().path.rfind(here,0)==0 && graph_.host(n.first).failure().empty())
            bad("the open Performance plays "+(p.name.empty()?id:p.name)+"; remove it from the Performance or open another first");
    }
    std::error_code e;
    if(!forPlatform.empty()){
        if(!fs::is_directory(folder/forPlatform,e))bad("that plugin has no "+forPlatform+" build installed");
        fs::remove_all(folder/forPlatform);
        if(!storedBuilds(format,id,version).empty())return;
    }
    // Out of the store's sight first, so a removal cut short leaves nothing half there.
    const auto gone=folder.parent_path()/("."+folder.filename().string()+".removing");
    fs::remove_all(gone,e);fs::rename(folder,gone);fs::remove_all(gone,e);
}

void Session::addPluginBuild(const std::string& format,const std::string& id,const std::string& version,const std::string& forPlatform,const std::string& path) {
    if(format!="clap")bad("only CLAP plugin builds can be added; LV2 plugins are installed on each device");
    if(id.empty() || id.size()>256 || version.size()>128)bad("invalid plugin ID or version");
    if(!knownPlatform(forPlatform))bad("unknown platform "+forPlatform);
    if(forPlatform==platform())bad("this device's build is the one it loads; add builds for other platforms");
    const fs::path build=path;std::error_code e;
    if(build.extension()!=".clap" || !fs::exists(build,e) || fs::is_symlink(build,e))bad("not a CLAP build: "+path);
    if(fs::is_directory(build,e))for(const auto& f:fs::recursive_directory_iterator(build))if(f.is_symlink())bad("a link in the build: "+f.path().string());
    std::string name=storedName(format,id,version);
    for(const auto& n:nodes_)if(const auto& d=graph_.host(n.first).descriptor();d.id==id && d.version==version && !d.name.empty())name=d.name;
    storeBuild(format,id,version,name,forPlatform,build,buildSignatures(build,format,id,version,forPlatform,bundleDigest(build)));
}

void Session::deleteLibraryItem(const std::string& id) {
    auto& l=library();
    const auto* item=l.find(id);
    if(!item)bad("no such library item");
    fs::remove_all(item->path);
    l.rescan();
}

void Session::libraryEdit(const std::string& id,const PerformanceInfo& edit) {
    auto& l=library();
    const auto* item=l.find(id);
    if(!item)bad("no such library item");
    if(!item->library.empty())bad("a locked library's items cannot be edited; copy it into your own first");
    const auto doc=fs::path(item->path)/(item->kind=="sound"?"sound.json":"performance.json");
    auto info=readInfo(doc,"oscairn."+item->kind);
    checkInfo(edit);
    info.name=edit.name;info.description=edit.description;info.tags=edit.tags;info.color=edit.color;
    writeInfo(doc,info);
    // The open Performance, if it is this one, so saving it keeps the edit.
    if(info_.id==id){info_.name=info.name;info_.description=info.description;info_.tags=info.tags;info_.color=info.color;}
    l.rescan();
}

std::string Session::libraryCopy(const std::string& id) {
    auto& l=library();
    const auto* item=l.find(id);
    if(!item)bad("no such library item");
    const auto from=*item;
    const bool locked=!from.library.empty();
    const auto name=locked?from.name:(from.name.empty()?"Untitled":from.name)+" copy";
    const auto to=l.newFolder(name,from.kind);
    fs::create_directories(to.parent_path());
    fs::copy(from.path,to,fs::copy_options::recursive);
    try{
        const auto doc=to/(from.kind=="sound"?"sound.json":"performance.json");
        auto info=readInfo(doc,"oscairn."+from.kind);
        info.id=freshId();info.name=name;info.parent.clear();info.revision=freshId();
        info.hash=contentHash(doc);
        if(locked)info.origin={from.library,from.id,info.hash}; // as installing it as the user's own would
        writeInfo(doc,info);
        l.rescan();
        return info.id;
    }catch(...){std::error_code ignored;fs::remove_all(to,ignored);throw;}
}

void Session::loadPerformanceFromLibrary(const std::string& id) {
    auto& l=library();
    const auto* item=l.find(id);
    if(!item || item->kind!="performance")bad("no such Performance in the library");
    loadPerformance(item->path);
    l.used(id);
}
}

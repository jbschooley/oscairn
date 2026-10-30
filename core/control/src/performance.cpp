// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include <algorithm>
#include <charconv>
#include <cmath>
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
V field(V v,std::string_view key){if(!v || v->type!=json_type_object)bad("expected object");for(auto* e=static_cast<const json_object_s*>(v->payload)->start;e;e=e->next)if(std::string_view(e->name->string,e->name->string_size)==key)return e->value;bad("missing field "+std::string(key));}
void fields(V v,std::initializer_list<std::string_view> allowed){if(!v || v->type!=json_type_object)bad("expected object");std::set<std::string_view> seen;for(auto* e=static_cast<const json_object_s*>(v->payload)->start;e;e=e->next){std::string_view key(e->name->string,e->name->string_size);if(!seen.insert(key).second || std::find(allowed.begin(),allowed.end(),key)==allowed.end())bad("unknown or duplicate field");}}
std::vector<V> array(V v,size_t limit){if(!v || v->type!=json_type_array)bad("expected array");const auto* a=static_cast<const json_array_s*>(v->payload);if(a->length>limit)bad("too many entries");std::vector<V> out;for(auto* e=a->start;e;e=e->next)out.push_back(e->value);return out;}
double number(V v){if(!v || v->type!=json_type_number)bad("expected number");const auto* n=static_cast<const json_number_s*>(v->payload);std::istringstream in(std::string(n->number,n->number_size));in.imbue(std::locale::classic());double value;if(!(in>>value) || !in.eof() || !std::isfinite(value))bad("invalid number");return value;}
uint32_t integer(V v,uint32_t max=UINT32_MAX){const auto n=number(v);if(n<0 || n>max || n!=std::floor(n))bad("invalid integer");return static_cast<uint32_t>(n);}
std::string quote(std::string_view s){if(s.size()>4096 || s.find('\0')!=std::string_view::npos)bad("invalid string");std::string out="\"";const char* hex="0123456789abcdef";for(unsigned char c:s){if(c=='"' || c=='\\'){out+='\\';out+=char(c);}else if(c<32){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}return out+'"';}
std::string numeric(double value){if(!std::isfinite(value))bad("nonfinite value");char buffer[64];const auto result=std::to_chars(buffer,buffer+sizeof(buffer),value);if(result.ec!=std::errc{})bad("number encoding failed");return {buffer,result.ptr};}
std::string join(const std::vector<std::string>& values){std::string out="[";for(const auto& v:values){if(out.size()>1)out+=',';out+=v;}return out+']';}
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
    std::vector<std::string> nodes,audio,notes,mappings;
    for(const auto& n:graph.nodes){const auto& key=keys.at(n.id);auto& h=graph_.host(n.id);
        try{h.saveSnapshot((bundle.temporary.path/"nodes"/key).string());}catch(const std::exception& e){bad("cannot save "+nodes_.at(n.id)+" ("+n.plugin.id+"): "+e.what());}
        const bool lv2=lv2Path(n.plugin.path);
        nodes.push_back("{\"key\":"+quote(key)+",\"name\":"+quote(nodes_.at(n.id))+",\"plugin\":{\"format\":"+quote(lv2?"lv2":"clap")+",\"id\":"+quote(n.plugin.id)+",\"version\":"+quote(n.plugin.version)+",\"pathHint\":"+quote(n.plugin.path)+"},\"engine\":"+quote(keys.at(graph_.effectSource(n.id)))+",\"inputs\":"+ports(n.inputs)+",\"outputs\":"+ports(n.outputs)+",\"noteInputs\":"+std::to_string(n.noteInputs)+",\"noteOutputs\":"+std::to_string(n.noteOutputs)+'}');
    }
    auto endpoint=[&](const AudioEndpoint& e){return "{\"node\":"+quote(keys.at(e.node))+",\"port\":"+std::to_string(e.port)+",\"channel\":"+std::to_string(e.channel)+'}';};
    for(const auto& c:graph.audioRoutes)audio.push_back("{\"from\":"+endpoint(c.from)+",\"to\":"+endpoint(c.to)+",\"channels\":"+std::to_string(c.channels)+",\"gain\":"+numeric(c.gain)+'}');
    for(const auto& [from,to]:graph.notes)notes.push_back("{\"from\":"+quote(keys.at(from))+",\"to\":"+quote(keys.at(to))+'}');
    for(const auto& m:graph.mappings)mappings.push_back("{\"from\":"+quote(keys.at(m.source))+",\"lane\":"+std::to_string(m.lane)+",\"to\":"+quote(keys.at(m.target))+",\"param\":"+std::to_string(m.param)+'}');
    const auto out="{\"format\":\"oscairn.performance\",\"version\":1,\"transport\":{\"tempo\":"+numeric(tempo_.load())+",\"clockSource\":"+quote(clockSource_.load()==Transport::ClockSource::Internal?"internal":"midi")+"},\"audioInputs\":"+ports(graph.audioLayout.inputs)+",\"audioOutputs\":"+ports(graph.audioLayout.outputs)+",\"nodes\":"+join(nodes)+",\"audio\":"+join(audio)+",\"notes\":"+join(notes)+",\"mappings\":"+join(mappings)+"}\n";
    if(out.size()>4*1024*1024)bad("manifest exceeds size limit");
    const auto encoding=detail::jsonPreflight(out,32);if(!encoding.empty())bad(std::string(encoding));
    state_files::write(bundle.temporary.path/"performance.json",out.data(),out.size());validateTree(bundle.temporary.path);bundle.publish();
}

void Session::loadPerformance(const std::string& directory) {
    if(directory.empty() || directory.find('\0')!=std::string::npos)bad("invalid directory");
    const auto root=fs::canonical(directory);validateTree(root);const auto doc=document(root);V d=doc.get();
    fields(d,{"format","version","transport","audioInputs","audioOutputs","nodes","audio","notes","mappings"});
    if(text(field(d,"format"))!="oscairn.performance" || integer(field(d,"version"))!=1)bad("unsupported format version");
    const auto transport=field(d,"transport");fields(transport,{"tempo","clockSource"});const double tempo=number(field(transport,"tempo"));
    const auto clock=text(field(transport,"clockSource"));if(tempo<1 || tempo>1000 || (clock!="internal" && clock!="midi"))bad("invalid transport");
    const auto layout=graph_.describe().audioLayout;checkPorts(field(d,"audioInputs"),layout.inputs);checkPorts(field(d,"audioOutputs"),layout.outputs);
    auto next=graph_.prepareReplacement();std::map<std::string,NodeId> ids{{"input",graphInput},{"output",graphOutput}};std::map<NodeId,std::string> names;std::set<std::string> usedNames;
    std::map<std::string,V> definitions;const auto nodes=array(field(d,"nodes"),256);
    for(auto node:nodes){fields(node,{"key","name","plugin","engine","inputs","outputs","noteInputs","noteOutputs"});const auto key=text(field(node,"key"));
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
    auto endpoint=[&](V v){fields(v,{"node","port","channel"});return AudioEndpoint{node(field(v,"node")),integer(field(v,"port")),integer(field(v,"channel"),Graph::maxAudioChannels-1)};};
    std::set<std::tuple<NodeId,uint32_t,uint32_t,NodeId,uint32_t,uint32_t>> audioRoutes;
    for(auto c:array(field(d,"audio"),Graph::maxAudioConnections)){
        fields(c,{"from","to","channels","gain"});const auto from=endpoint(field(c,"from")),to=endpoint(field(c,"to"));const auto count=integer(field(c,"channels"),Graph::maxAudioChannels);
        for(uint32_t i=0;i<count;++i)if(!audioRoutes.emplace(from.node,from.port,from.channel+i,to.node,to.port,to.channel+i).second)bad("duplicate audio route");
        next->connectAudio({from,to,count,number(field(c,"gain"))});
    }
    std::set<std::pair<NodeId,NodeId>> noteRoutes;
    for(auto c:array(field(d,"notes"),4096)){fields(c,{"from","to"});const auto from=node(field(c,"from")),to=node(field(c,"to"));if(!noteRoutes.emplace(from,to).second)bad("duplicate note route");next->connectNotes(from,to);}
    std::set<std::pair<NodeId,uint32_t>> mappings;
    for(auto m:array(field(d,"mappings"),4096)){fields(m,{"from","lane","to","param"});const auto from=node(field(m,"from")),lane=integer(field(m,"lane"));if(!mappings.emplace(from,lane).second)bad("duplicate parameter mapping");next->mapParamTarget(from,lane,node(field(m,"to")),integer(field(m,"param")));}
    std::map<Address,double> observed;for(const auto& [id,name]:names)for(const auto& p:next->host(id).parameters())observed[{id,p.id}]=next->host(id).parameterValue(p.id);
    // Compilation and all allocating preparation precede publication. The audio thread
    // adopts at one boundary; old plans and their hosts are reclaimed by service().
    auto history=history_;
    if(history.size()==historyCapacity)history.pop_front();
    history.push_back({sequence_+1,ChangeKind::Resync,0,0,0,false});
    graph_.publishReplacement(*next,noteHead_.load(std::memory_order_acquire),[&]() noexcept {
        nodes_.swap(names);observed_.swap(observed);pending_.clear();commandCount_=0;outputDrops_.clear();staged_=false;
        tempo_.store(tempo);clockSource_.store(clock=="internal"?Transport::ClockSource::Internal:Transport::ClockSource::MidiClock);playing_.store(false);
        ++revision_;++sequence_;history_.swap(history);
    });
}
}

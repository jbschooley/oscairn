// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include "oscairn/wav_file.h"
#include "../src/PropertyWire.h"
#include "../../plugin-host/src/StateFiles.h"
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <thread>
using namespace oscairn;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
std::string quote(const std::string& s){std::ostringstream out;out<<std::quoted(s);return out.str();}
void wire(){
    const std::array<PropertyValue,9> values{true,int32_t(-2147483647),int64_t(INT64_MIN),int64_t(INT64_MAX),.5f,.125,std::string("a\\b\"c"),FilePath{"/tmp/sample.wav"},false};
    for(const auto& v:values){const auto encoded=property_wire::encode(v,quote,[](double n){std::ostringstream out;out<<std::setprecision(17)<<n;return out.str();});
        std::unique_ptr<json_value_s,decltype(&std::free)> doc(json_parse(encoded.data(),encoded.size()),std::free);check(doc && property_wire::decode(doc.get())==v,"typed property round trip");}
    for(const std::string s:{R"({"type":"long","value":9223372036854775807})",R"({"type":"long","value":"9223372036854775808"})",R"({"type":"int","value":2147483648})",R"({"type":"int","value":1.5})",R"({"type":"bool","value":1})",R"({"type":"float","value":1e40})",R"({"type":"double","value":1e400})",R"({"type":"path","value":"a\u0000b"})",R"({"type":"unknown","value":1})",R"({"type":"int","value":1,"extra":2})",R"({"type":"int","type":"int","value":1})",R"({"type":"int"})"}){
        std::unique_ptr<json_value_s,decltype(&std::free)> doc(json_parse(s.data(),s.size()),std::free);bool rejected=false;try{property_wire::decode(doc.get());}catch(const std::invalid_argument&){rejected=true;}check(rejected,"invalid typed property accepted");}
}
struct Fixture {
    Session session{48000,512};JsonRpc rpc{session};std::array<float,512> left{},right{};
    std::string call(const std::string& method,const std::string& params){return rpc.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":"+quote(method)+",\"params\":"+params+'}').value_or("");}
    void render(){float* out[]{left.data(),right.data()};session.service();check(session.process(out,512),"session render");session.service();}
};
void lv2(const char* sampler,const char* fixture){
    state_files::TemporaryDirectory temp;const auto sample=(temp.path/"test.wav").string();writeWavFile(sample,std::vector<float>(48000,.25f),1,48000);
    Fixture f;const auto n=f.session.add("sampler",sampler,"http://lv2plug.in/plugins/eg-sampler");f.session.connectNotes(graphInput,n);f.session.connectAudio(n,graphOutput);f.session.commit();
    const std::string address=R"("node":"sampler","uri":"http://lv2plug.in/plugins/eg-sampler#sample")";
    check(f.call("properties.list",R"({"node":"sampler"})").find("\"type\":\"path\"")!=std::string::npos,"LV2 property metadata");
    check(f.call("properties.get","{"+address+'}').find("\"value\":null")!=std::string::npos,"unknown until confirmation");
    check(f.call("properties.set","{"+address+",\"value\":{\"type\":\"string\",\"value\":"+quote(sample)+"}}").find("-32005")!=std::string::npos,"declared property type enforced");
    check(f.call("properties.set","{"+address+",\"value\":{\"type\":\"path\",\"value\":"+quote(sample)+"}}").find("\"accepted\":true")!=std::string::npos,"queue patch sample load");
    bool confirmed=false;for(int i=0;i<2000 && !confirmed;++i){f.render();confirmed=f.call("properties.get","{"+address+'}').find(quote(sample))!=std::string::npos;if(!confirmed)std::this_thread::sleep_for(std::chrono::milliseconds(1));}check(confirmed,"plugin-confirmed file readback");
    check(f.call("properties.request","{"+address+'}').find("-32602")!=std::string::npos,"refresh requires advertised readable property");f.render();
    f.session.note(true,60,1);f.render();check(f.left[0]==.25f,"property change affects rendered sample");
    const auto state=(temp.path/"saved").string();check(f.call("state.save","{\"node\":\"sampler\",\"directory\":"+quote(state)+'}').find("\"saved\":true")!=std::string::npos,"wire portable state export");
    check(std::filesystem::exists(std::filesystem::path(state)/"state.ttl"),"state saved");
    const auto performance=temp.path/"performance",moved=temp.path/"moved";
    f.session.savePerformance(performance.string());std::filesystem::rename(performance,moved);std::filesystem::remove(sample);
    Fixture imported;imported.session.loadPerformance(moved.string());imported.render();
    imported.session.note(true,60,1);imported.render();check(imported.left[0]==.25f,"Performance carries sampler assets after move and original removal");
    const auto revision=imported.session.graphState().revision;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(moved))if(entry.path().extension()==".wav")std::filesystem::remove(entry.path());
    bool missing=false;try{imported.session.loadPerformance(moved.string());}catch(const std::exception&){missing=true;}
    check(missing && imported.session.graphState().revision==revision,"missing Performance asset preserves live graph");
    imported.render();check(imported.left[0]==.25f,"missing asset does not interrupt current sampler");
    f.session.add("presets",fixture,"urn:oscairn:test:state");check(f.call("presets.list",R"({"node":"presets"})").find("urn:oscairn:test:preset")!=std::string::npos,"LV2 preset enumeration");
    check(f.call("properties.get",R"({"node":"sampler","uri":"missing"})").find("-32602")!=std::string::npos,"unknown property rejected");
}
int main(int argc,char** argv){try{wire();if(argc==3)lv2(argv[1],argv[2]);std::cout<<"ok: typed property validation and sound-management controls\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

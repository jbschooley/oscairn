// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "AllocationGuard.h"
#include <array>
#include <atomic>
#include <iostream>
#include <thread>
using namespace oscairn;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f,const char* message){bool bad=false;try{f();}catch(const std::exception&){bad=true;}check(bad,message);}
AudioLayout layout(){return {{{7,"Deck A",2,0},{9,"Sidechain",1,0},{11,"Deck B",2,0}},{{100,"Master",2,0},{200,"Cue",2,0},{300,"Record",1,0}}};}
struct Audio {
    std::array<std::array<float,64>,5> input{},output{};
    std::array<const float*,5> in;std::array<float*,5> out;
    Audio(){const float values[]{1,2,3,11,13};for(size_t c=0;c<5;++c){input[c].fill(values[c]);in[c]=input[c].data();out[c]=output[c].data();}}
    bool run(Graph& g,uint32_t n=64){forbidAllocation=true;auto ok=g.process(in.data(),5,out.data(),5,n,nullptr);forbidAllocation=false;return ok;}
    void expect(std::array<float,5> values,uint32_t n=64){for(size_t c=0;c<5;++c)for(size_t i=0;i<n;++i)check(output[c][i]==values[c],"independent routed samples");}
};
int main(int argc,char** argv){try{
    if(argc<2)return 2;
    Graph g(48000,64,ProcessingMode::Realtime,{},layout());auto n=g.add(argv[1]);
    g.connectAudio({{graphInput,7,0},{n,10,0},2});g.connectAudio({{graphInput,9,0},{n,20,0}});
    g.connectAudio({{n,30,0},{graphOutput,100,0},2});
    const AudioConnection cue{{graphInput,11,0},{graphOutput,200,0},2};
    g.connectAudio(cue);g.connectAudio(cue);g.connectAudio({{n,40,0},{graphOutput,300,0}});g.commit();
    Audio a;check(a.run(g),"multichannel render");a.expect({31,62,11,13,3});
    check(g.describe().audioRoutes.size()==5,"duplicate route idempotent");
    // Buffer sizes, sidechains and routing remain correct with uneven render blocks.
    check(a.run(g,37),"uneven blocks");a.expect({31,62,11,13,3},37);
    rejects([&]{g.connectAudio({{n,0,0},{graphOutput,100,0}});},"port IDs are not array indices");
    rejects([&]{g.connectAudio({{n,30,1},{graphOutput,100,0},2});},"range cannot cross port boundary");
    rejects([&]{g.connectAudio({{graphInput,7,0},{graphOutput,999,0}});},"missing output bus rejected");
    rejects([&]{g.connectAudio({{n,30,0},{n,10,0},0});},"zero count rejected");
    const AudioConnection cycle{{n,30,0},{n,10,0},2};g.connectAudio(cycle);
    rejects([&]{g.commit();},"explicit-route cycle rejected");check(a.run(g),"previous plan survives failed commit");a.expect({31,62,11,13,3});g.disconnectAudio(cycle);
    g.disconnectAudio(cue);check(a.run(g),"staged disconnect leaves live routing");a.expect({31,62,11,13,3});g.commit();check(a.run(g),"cue disconnect");a.expect({31,62,0,0,3});
    // Deliberate stereo-to-mono fold: each channel gets an explicit half-gain route.
    g.connectAudio({{graphInput,11,0},{graphOutput,200,0},1,.5});g.connectAudio({{graphInput,11,1},{graphOutput,200,0},1,.5});g.commit();
    check(a.run(g),"explicit downmix");a.expect({31,62,12,0,3});
    check(!g.process(a.in.data(),5,a.out.data(),2,64,nullptr),"missing cue outputs fail rather than reroute to master");
    for(auto v:a.output[0])check(v==0,"failed output layout silences master");
    // Same buffers for capture and playback: input is preserved before clearing outputs.
    for(size_t c=0;c<5;++c)a.out[c]=a.input[c].data();check(a.run(g),"aliased capture/playback");
    check(a.input[0][0]==31 && a.input[1][0]==62 && a.input[2][0]==12,"input copy precedes output clearing");
    Audio missing;check(g.process(nullptr,0,missing.out.data(),5,64,nullptr),"missing capture is silence");missing.expect({0,0,0,0,0});
    std::atomic<bool> stop=false,valid=true;std::thread render([&]{Audio live;while(!stop){if(!live.run(g) || live.output[0][0]!=31 || live.output[1][0]!=62 || !((live.output[2][0]==12 && live.output[3][0]==0) || (live.output[2][0]==23 && live.output[3][0]==13)))valid=false;}});
    for(int i=0;i<50;++i){g.connectAudio(cue);g.commit();g.disconnectAudio(cue);g.commit();g.collect();}
    stop=true;render.join();check(valid,"live route editing remains allocation-free");

    Session session(48000,64,{},layout());JsonRpc rpc(session);auto id=session.add("effect",argv[1]);
    const auto route="{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"graph.connect\",\"params\":{\"kind\":\"audio\",\"from\":4294967294,\"to\":4294967295,\"fromPort\":11,\"fromChannel\":0,\"toPort\":200,\"toChannel\":0,\"channels\":2}}";
    auto response=rpc.dispatch(route);check(response->find("error")==std::string::npos,"explicit JSON route accepted");session.commit();
    Audio b;forbidAllocation=true;auto ok=session.process(b.in.data(),5,b.out.data(),5,64);forbidAllocation=false;check(ok,"session external audio render");b.expect({0,0,11,13,0});
    auto snapshot=rpc.dispatch(R"({"jsonrpc":"2.0","id":2,"method":"graph.get","params":{}})");
    check(snapshot->find("Deck A")!=std::string::npos && snapshot->find("fromPort")!=std::string::npos,"snapshot preserves ports and explicit routes");
    auto invalid=rpc.dispatch(R"({"jsonrpc":"2.0","id":3,"method":"graph.connect","params":{"kind":"notes","from":"input","to":"output","fromPort":0}})");check(invalid->find("error")!=std::string::npos,"note routes reject audio fields");
    session.remove(id);session.commit();
    if(argc==3){
        Graph mixed(48000,64,ProcessingMode::Realtime,{},layout());auto lv=mixed.add(argv[2],"urn:oscairn:test:effect");auto cl=mixed.add(argv[1]);
        mixed.connectAudio({{graphInput,9,0},{lv,1,0}});mixed.connectAudio({{lv,2,0},{cl,20,0}});mixed.connectAudio({{cl,30,0},{graphOutput,200,0},2});mixed.commit();
        Audio c;check(c.run(mixed),"mixed LV2/CLAP explicit ports");c.expect({0,0,15,30,0});
    }
    Graph changed(48000,64,ProcessingMode::Realtime,{},layout());auto changedNode=changed.add(argv[1]);
    changed.connectAudio({{graphInput,9,0},{changedNode,20,0}});changed.connectAudio({{changedNode,30,0},{graphOutput,200,0},2});changed.commit();
    Audio rescan;rescan.input[2].fill(99);check(rescan.run(changed),"initial layout before rescan");changed.host(changedNode).service();
    check(changed.describe().nodes[0].outputs[0].id==30,"rejected rescan preserves original port identities");
    check(!rescan.run(changed),"changed CLAP port layout fails instead of redirecting cue");rescan.expect({0,0,0,0,0});
    std::cout<<"ok: independent buses, sidechains, ranges, mixes, aliasing, live edits, JSON and mixed-format audio\n";return 0;
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}

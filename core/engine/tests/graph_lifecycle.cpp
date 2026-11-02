// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "AllocationGuard.h"
#include <array>
#include <iostream>
using namespace oscairn;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct Audio {
    std::array<float,64> left{},right{};float* out[2]{left.data(),right.data()};
    bool run(Graph& g){forbidAllocation=true;auto ok=g.process(out,64,nullptr);forbidAllocation=false;return ok;}
    bool run(Session& s){forbidAllocation=true;auto ok=s.process(out,64);forbidAllocation=false;return ok;}
};
int main(int argc,char** argv){try{
    if(argc!=2)return 2;
    for(bool parameters:{false,true})
    {
        Graph g(48000,64);auto early=g.add(argv[1]),early2=g.add(argv[1]),late=g.add(argv[1]),sink=g.add(argv[1]);
        if(parameters)g.mapParamTarget(early,0,sink,2),g.mapParamTarget(early2,0,sink,2);else g.connectNotes(early,sink),g.connectNotes(early2,sink);
        g.connectNotes(late,sink);g.connectAudio(sink,graphOutput);g.commit();Audio a;
        g.host(late).setParameter(1,1);check(a.run(g) && a.left[0]==1,"held note before overflow");
        g.host(early).setParameter(1,parameters?-512:512);g.host(early2).setParameter(1,parameters?-512:512);g.host(late).setParameter(1,-1);
        check(a.run(g) && a.left[0]==0,"late note-off survives saturated earlier sources via emergency release");
        check(g.eventDrops()==1025,"discarded burst is counted");
        g.host(late).setParameter(1,1);check(a.run(g) && a.left[0]==1,"new notes play after overflow");
    }
    {
        // A retiring node whose plugin fails every block (a worker that died and did not restart)
        // is never rendered. Only rendered blocks once counted toward its limit, so it was never
        // idle and the engine carried the dead plugin for its whole life.
        Graph g(48000,64);auto s=g.add(argv[1]);
        g.connectNotes(graphInput,s);g.connectAudio(s,graphOutput);g.commit();Audio a;
        g.setRetiring(s,true);g.host(s).setParameter(1,4);g.commit();
        for(int i=0;i<10;++i)check(!a.run(g),"a plugin that errs refuses every block");
        check(!g.idle(s,UINT64_MAX,uint64_t(64)*11),"not idle before its limit has passed");
        check(g.idle(s,UINT64_MAX,64*8),"an unrendered block counts toward the limit as silence does");
    }
    Session s(48000,64);auto removed=s.add("removed",argv[1]);s.connectAudio(removed,graphOutput);s.commit();Audio a;
    check(a.run(s) && a.right[0]==0,"initial callback request");
    s.remove(removed);s.service();check(a.run(s) && a.right[0]==1,"staged removal still services active plugin");
    auto x=s.add("x",argv[1]),y=s.add("y",argv[1]);s.connectAudio(x,y);s.connectAudio(y,x);
    bool rejected=false;try{s.commit();}catch(const std::runtime_error&){rejected=true;}check(rejected,"cycle rejected");
    s.service();check(a.run(s) && a.right[0]==2,"failed commit keeps old plugin serviced");
    s.disconnectAudio(y,x);s.commit();check(a.run(s) && a.right[0]==0,"successful removal replaces active plan");s.service();
    std::cout<<"ok: graph overflow release, drop accounting and servicing through staged/failed removal\n";return 0;
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}

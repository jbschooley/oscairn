// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "../../plugin-host/src/StateFiles.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
using namespace oscairn;
namespace fs=std::filesystem;
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Audio {
    std::array<float,127> l{},r{};float* out[2]{l.data(),r.data()};
    void run(Session& s){forbidAllocation=true;const bool ok=s.process(out,127);forbidAllocation=false;check(ok,"render");}
};
static void configure(Session& s,const char* synth,const char* delay,const char* seq) {
    auto n=s.add("synth",synth),d=s.add("delay",delay),e=s.addEffectContext("long-echo",d),q=s.add("sequencer",seq);
    s.connectNotes(graphInput,n);s.connectNotes(q,n);
    s.connectAudio({{n,1,0},{d,0,0},2});s.connectAudio({{n,2,0},{e,0,0},2});
    s.connectAudio(d,graphOutput);s.connectAudio(e,graphOutput);
    s.mapParameter(q,0,n,3);s.commit();
    const auto a=s.prepareSound(n,"Detuned saws",{{4,1},{5,7},{8,1}});
    const auto b=s.prepareSound(n,"Square vibrato",{{4,2},{6,18},{7,70},{8,2}});
    check(s.selectSound(n,a,0) && s.selectSound(n,b,1),"select patches");
    check(s.setParameter(d,1,.125) && s.setParameter(d,2,.2) && s.setParameter(e,1,.75) && s.setParameter(e,2,.7),"effect parameters");
    check(s.setParameter(q,200,48) && s.setParameter(q,101,0),"sequence edits");s.setTempo(137);
    Audio audio;audio.run(s);s.service();
}
static void compare(Session& a,Session& b) {
    Audio x,y;double energy=0;a.setPlaying(true);b.setPlaying(true);
    for(int block=0;block<1100;++block) {
        if(block==3 || block==35){a.note(true,60,.7,block==3?0:1);b.note(true,60,.7,block==3?0:1);}
        if(block==200 || block==240){a.note(false,60,0,block==200?0:1);b.note(false,60,0,block==200?0:1);}
        x.run(a);y.run(b);if(x.l!=y.l || x.r!=y.r){std::cerr<<"different at block "<<block<<"\n";throw std::runtime_error("restored output differs from fresh reference");}
        for(auto v:x.l)energy+=v*v;
    }
    check(energy>1,"audible restored output");a.setPlaying(false);b.setPlaying(false);
}
static void replaceText(const fs::path& file,const std::string& from,const std::string& to) {
    auto bytes=state_files::read(file,4*1024*1024);std::string value(bytes.begin(),bytes.end());const auto at=value.find(from);
    check(at!=std::string::npos,"mutation fixture");value.replace(at,from.size(),to);state_files::write(file,value.data(),value.size());
}
static void rewindClock() {
    Transport t(48000);t.setPlaying(true);t.advance(48000);t.rewind();
    check(t.position()==0 && t.current().song_pos_beats==0,"rewind play position");
    t.setClockSource(Transport::ClockSource::MidiClock);t.midiClock(0xfa,48000);t.midiClock(0xf8,48000);t.advance(1000);
    check(t.current().song_pos_beats>CLAP_BEATTIME_FACTOR/25,"rewind preserves absolute MIDI timestamp domain");
}
static void native(const char* synth,const char* delay,const char* seq,const fs::path& root) {
    Session original(48000,127);configure(original,synth,delay,seq);const auto file=root/"original";original.savePerformance(file.string());
    try{original.savePerformance(file.string());throw std::logic_error("overwrote existing save");}catch(const std::runtime_error&){}
    Session a(48000,127),b(48000,127);configure(a,synth,delay,seq);configure(b,synth,delay,seq);const auto old=a.nodeId("synth");
    a.note(true,100,1);a.loadPerformance(file.string());
    check(a.nodeId("synth")!=old && !a.hasNode(old),"old node IDs must become invalid");
    Audio reset;reset.run(a);reset.run(b); // adopt and flush parameter-only sequencer state
    check(a.transportState().beat==0 && !a.transportState().playing,"load stopped at beat zero");
    const auto sounds=a.sounds(a.nodeId("synth"));
    check(sounds.contexts.size()==3 && sounds.contexts[1].selectedChannels==1 && sounds.contexts[2].selectedChannels==2,"all contexts and channel assignments");
    check(a.parameter(a.nodeId("sequencer"),200)==48 && a.parameter(a.nodeId("sequencer"),101)==0,"sequencer state");
    check(a.effectResources(a.nodeId("delay"))->prepared==2 && a.parameter(a.nodeId("long-echo"),2)==.7,"shared effect engine and independent state");
    // The live-configured reference must finish its parameter glides before comparison.
    for(int i=0;i<1000;++i){reset.run(a);reset.run(b);}
    compare(a,b);
    // A save made with voices/tails running restores patches, not that transient audio.
    original.note(true,55,1);reset.run(original);original.savePerformance((root/"held").string());
    Session held(48000,127);held.loadPerformance((root/"held").string());reset.run(held);
    check(std::all_of(reset.l.begin(),reset.l.end(),[](float x){return x==0;}),"held notes/tails are not resurrected");
    auto rejects=[&](const fs::path& path){const auto revision=a.graphState().revision;bool failed=false;try{a.loadPerformance(path.string());}catch(const std::exception&){failed=true;}
        check(failed && a.graphState().revision==revision,"invalid restore changed current graph");};
    for(int which=0;which<5;++which){const auto bad=root/("bad"+std::to_string(which));fs::copy(file,bad,fs::copy_options::recursive);
        if(which==0)replaceText(bad/"performance.json","\"version\":1","\"version\":999");
        if(which==1)replaceText(bad/"performance.json",synth,"/missing/plugin.clap");
        if(which==2)fs::remove(bad/"nodes/node-1/state/state.clap");
        if(which==3)replaceText(bad/"performance.json","\"engine\":\"node-1\"","\"engine\":\"node-4\"");
        if(which==4)fs::create_symlink(file/"performance.json",bad/"link");
        rejects(bad);
    }
    // Byte-identical continuing playback proves rejection didn't reset the live graph.
    compare(a,b);
    // Select+edit+save before the next callback must capture the accepted selection.
    const auto n=original.nodeId("synth");const auto c=original.prepareSound(n,"Pending",{{1,.1},{4,2}});
    check(original.selectSound(n,c) && original.setParameter(n,1,.37),"pending edit");original.savePerformance((root/"pending").string());
    Session pending(48000,127);pending.loadPerformance((root/"pending").string());reset.run(pending);
    check(pending.parameter(pending.nodeId("synth"),1)==.37 && pending.sounds(pending.nodeId("synth")).contexts.back().selectedChannels==65535,"queued selection and edit persisted");
    // Saving after the original effect interface was removed still preserves sharing.
    original.disconnectAudio(original.nodeId("delay"),graphOutput);original.remove(original.nodeId("delay"));original.commit();reset.run(original);original.service();
    original.savePerformance((root/"orphan").string());pending.loadPerformance((root/"orphan").string());reset.run(pending);
    check(pending.effectResources(pending.nodeId("long-echo"))->prepared==1,"surviving effect can become saved engine root");
    // Concurrent preparation/publication and saves never allocate in Session::process.
    std::atomic<bool> stop{false};std::atomic<unsigned> rendered{0};std::thread audio([&]{Audio out;while(!stop.load()){forbidAllocation=true;const bool ok=a.process(out.out,127);forbidAllocation=false;if(ok)++rendered;}});
    try {for(int i=0;i<12;++i){a.loadPerformance(file.string());a.service();a.savePerformance((root/("live"+std::to_string(i))).string());}}
    catch(...){stop=true;audio.join();throw;}stop=true;audio.join();check(rendered>0,"concurrent render");reset.run(a);a.service();
}
static void hosted(const char* path,const fs::path& root) {
    Session s(48000,127);const auto n=s.add("synth",path,"org.kick.test.midi");s.connectNotes(graphInput,n);s.connectAudio(n,graphOutput);s.commit();
    const auto a=s.prepareSound(n,"Quiet",{{7,.25}}),b=s.prepareSound(n,"Loud",{{7,.75}});s.selectSound(n,a,0);s.selectSound(n,b,1);
    s.savePerformance((root/"hosted").string());Session restored(48000,127);restored.loadPerformance((root/"hosted").string());
    Audio out;out.run(restored);check(restored.sounds(restored.nodeId("synth")).contexts.size()==3,"all hosted instances restored");
    restored.note(true,60,1,0);restored.note(true,64,1,1);out.run(restored);check(out.l.back()==1,"hosted channels retain independent patches");
    Session failing(48000,127);failing.add("good",path,"org.kick.test.midi");const auto broken=failing.add("broken",path,"org.kick.test.save-failure");failing.commit();failing.setParameter(broken,7,.8);
    bool rejected=false;try{failing.savePerformance((root/"incomplete").string());}catch(const std::exception&){rejected=true;}
    check(rejected && !fs::exists(root/"incomplete"),"mid-save plugin failure must not publish a partial Performance");
    for(const auto& entry:fs::directory_iterator(root))check(!entry.path().filename().string().starts_with(".oscairn-state-"),"failed save staging cleaned up");
}
int main(int argc,char** argv){try{check(argc==5,"synth/delay/sequencer/fixture arguments");state_files::TemporaryDirectory temp;rewindClock();native(argv[1],argv[2],argv[3],temp.path);hosted(argv[4],temp.path);std::cout<<"ok: complete Performance roundtrip, failed-load isolation, native/pool state and concurrent publication\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}

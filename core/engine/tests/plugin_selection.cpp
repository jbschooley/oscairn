// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "oscairn/discovery.h"
#include "AllocationGuard.h"
#include <array>
#include <algorithm>
#include <iostream>
#include <cmath>
using namespace oscairn;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv){try {
    check(argc==6 || argc==7,"need synth, effect, sequencer, scanner, crash fixture and optional LV2 bundle");
    std::vector<std::filesystem::path> paths{argv[1],argv[2],argv[5]};if(argc==7)paths.emplace_back(argv[6]);
    auto scan=scanPlugins(paths,argv[4]);
    check(scan.failures.size()==1,"crashed scan is reported without killing discovery");
    check(scan.plugins.size()>=(argc==7?3u:2u),"CLAP and optional LV2 plugins discovered");
    Session session(48000,512),reference(48000,512);
    const auto synth=session.add("synth",argv[1]),baseline=reference.add("synth",argv[1]);
    session.connectNotes(graphInput,synth);session.connectAudio(synth,graphOutput);session.commit();
    reference.connectNotes(graphInput,baseline);reference.connectAudio(baseline,graphOutput);reference.commit();
    std::array<float,512> left{},right{},expected{},scratch{};float* out[]{left.data(),right.data()},*ref[]{expected.data(),scratch.data()};
    auto render=[&]{session.service();reference.service();forbidAllocation=true;const auto ok=session.process(out,512)&&reference.process(ref,512);forbidAllocation=false;check(ok,"render after replacement");};
    session.note(true,60,.7);reference.note(true,60,.7);render();
    const auto revision=session.graphState().revision;
    {
        bool refused=false;try{session.replace(synth,"/missing/plugin.clap","org.oscairn.basic-synth");}catch(const std::exception&){refused=true;}
        check(refused,"a plugin that cannot load is refused");render();check(left==expected,"failed replacement preserves held audio byte for byte");
        check(session.graphState().revision==revision,"failed replacement leaves revision unchanged");
    }
    // RFC 0011: a plugin whose ports do not fit is accepted; the wires that no longer fit stay, unconnected.
    auto connected=[&](WireKind kind){for(const auto& w:session.wires())if(w.kind==kind)return w.connected;throw std::runtime_error("missing wire");};
    session.replace(synth,argv[2],"org.oscairn.delay");
    check(session.wires().size()==2 && !connected(WireKind::Notes) && connected(WireKind::Audio),"an effect in the synth's place keeps its note wire, unconnected");
    session.replace(synth,argv[3],"org.oscairn.step-sequencer");
    check(session.wires().size()==2 && !connected(WireKind::Audio),"a sequencer in its place keeps the audio wire, unconnected");
    session.replace(synth,argv[1],"org.oscairn.basic-synth");
    check(connected(WireKind::Notes) && connected(WireKind::Audio),"both wires connect again when a synth returns");
    session.setParameter(synth,1,.01);
    session.replace(synth,argv[1],"org.oscairn.basic-synth");render();
    check(session.sounds(synth).contexts.size()==1 && session.parameter(synth,1)==1,"replacement clears old sound pool and pending parameter edits");
    check(std::all_of(left.begin(),left.end(),[](float v){return v==0;}),"old voices stop on explicit replacement");
    for(int i=0;i<8;++i){session.replace(synth,argv[1],"org.oscairn.basic-synth");session.note(true,60,.7);render();check(std::any_of(left.begin(),left.end(),[](float v){return v!=0;}),"repeated replacement accepts new notes");}
    auto seq=session.add("sequencer",argv[3]);
    bool staged=false;try{session.replace(synth,argv[1],"org.oscairn.basic-synth");}catch(const std::exception&){staged=true;}check(staged,"replacement refuses pending topology edits");
    session.mapParameter(seq,0,synth,1);session.commit();render();
    bool mapped=false;try{session.replace(synth,argv[1],"org.oscairn.basic-synth");}catch(const std::exception&){mapped=true;}check(mapped,"numeric parameter IDs are never silently mapped to another plugin");
    session.unmapParameter(seq,0);session.commit();render();
    if(argc==7){
        session.replace(synth,argv[6],"urn:oscairn:test:instrument","urn:oscairn:test:instrument-preset");session.note(true,60,.7);render();
        check(session.parameter(synth,0)==.75,"LV2 preset prepared before atomic replacement");
        check(left==right && left[400]>.1,"mono LV2 output remaps to both stereo channels");
        const auto before=left;bool refused=false;try{session.replace(synth,argv[6],"urn:oscairn:test:instrument","urn:missing-preset");}catch(const std::exception&){refused=true;}
        check(refused,"invalid replacement preset rejected");render();check(left==before,"failed preset leaves live audio unchanged");
        session.replace(synth,argv[1],"org.oscairn.basic-synth");render();
    }
    std::cout<<"ok: isolated discovery, atomic replacement, held audio preservation, routing and preset preparation\n";
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}

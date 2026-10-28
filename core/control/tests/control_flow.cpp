// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "AllocationGuard.h"
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace
{
    void check(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
    struct Audio
    {
        std::array<float,64> l{},r{};
        void render(oscairn::Session& s, std::span<const kick::clap::Event> events = {})
        {
            float* out[]{l.data(),r.data()};
            forbidAllocation=true;
            const auto ok=s.process(out,64,events);
            forbidAllocation=false;
            check(ok,"render");
        }
    };
    bool contains(const std::vector<std::string>& messages,const std::string& value)
    {
        for(const auto& m: messages) if(m.find(value)!=std::string::npos) return true;
        return false;
    }
    void run(const char* plugin)
    {
        using Kind=oscairn::Control::ChangeKind;
        oscairn::Session s(48000,64);
        auto a=s.add("a",plugin);
        s.connectNotes(oscairn::graphInput,a); s.connectAudio(a,oscairn::graphOutput); s.commit();
        Audio audio;
        s.note(true,60,1); audio.render(s);
        check(audio.l[0]==.5f,"held note before edits");
        oscairn::JsonRpc one(s),two(s);
        one.subscription("params.changed",true);
        two.subscription("params.changed",true,true);
        s.parameterGesture(a,1,true);
        for(int i=0;i<10000;++i) check(s.setParameter(a,1,(i%100)/100.),"coalescing accepts a long drag");
        check(s.setParameter(a,1,.75) && s.parameterGesture(a,1,false),"final value and end accepted");
        s.service(); audio.render(s);
        check(audio.l[0]==.75f,"coalesced final value rendered");
        const auto beforeEcho=s.changeSequence();
        s.service();
        const auto received=s.changesAfter(beforeEcho);
        check(received.events.size()==3,"plugin received only begin, final value, end");
        check(received.events[0].kind==Kind::GestureBegin && received.events[1].kind==Kind::Value &&
              received.events[1].value==.75 && received.events[2].kind==Kind::GestureEnd,"gesture delivery ordering");
        check(received.events[0].plugin,"plugin-originated gestures collected");
        check(contains(two.notifications(0),"session.resync"),"slow all-change subscriber is told history was lost");
        one.notifications(0);
        s.setParameter(a,1,.2); s.setParameter(a,1,.3);
        check(one.notifications(1).empty(),"default notification rate limited");
        const auto all=two.notifications(1);
        check(contains(all,"\"value\":0.2") && contains(all,"\"value\":0.3"),"all changes retained per subscriber");
        auto coalesced=one.notifications(16666667);
        check(!contains(coalesced,"\"value\":0.2") && contains(coalesced,"\"value\":0.3"),"default notification uses latest value");
        one.subscription("params.changed",false);
        check(one.notifications(33333334).empty(),"unsubscribe stops delivery");
        s.service(); audio.render(s); s.service();

        // Two complete gestures before a tick must retain both final values.
        for(double value : {.45,.55})
        {
            s.parameterGesture(a,1,true); s.setParameter(a,1,value); s.parameterGesture(a,1,false);
        }
        s.service(); audio.render(s); check(audio.l[0]==.45f,"first gesture final value retained");
        s.service(); audio.render(s); check(audio.l[0]==.55f,"second gesture final value retained");
        s.service();

        // Fill the host's queue by ticking without rendering. The refused last value
        // and end must survive; earlier accepted gestures must not be reset away.
        s.parameterGesture(a,1,true); s.service();
        for(size_t i=0;i<kick::clap::Host::maxEvents+20;++i) { s.setParameter(a,1,.4); s.service(); }
        check(s.setParameter(a,1,.81) && s.parameterGesture(a,1,false),"pending final drag is accepted under backpressure");
        s.service();
        for(int i=0;i<12;++i) { audio.render(s); check(audio.l[0]>0,"queue refusal never resets held note"); s.service(); }
        check(std::abs(audio.l[0]-.81f)<1e-6,"retained final value eventually arrives without resending");
        const auto changes=s.changesAfter(0);
        bool foundEnd=false;
        for(const auto& e: changes.events) if(e.plugin && e.kind==Kind::GestureEnd) foundEnd=true;
        check(foundEnd,"gesture end eventually reaches plugin");

        // A plugin can emit faster than control service drains its output. Detect
        // that loss separately from the session history wrapping.
        const auto beforeOverflow=s.changeSequence();
        clap_event_param_value_t event{};
        event.header={sizeof(event),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
        event.param_id=1; event.note_id=-1; event.port_index=event.channel=event.key=-1; event.value=.81;
        std::array<kick::clap::Event,256> burst;
        burst.fill(kick::clap::Event::from(event));
        for(int i=0;i<9;++i)audio.render(s,burst);
        s.service();
        bool outputGap=false;
        for(const auto& change:s.changesAfter(beforeOverflow).events) if(change.kind==Kind::Resync)outputGap=true;
        check(outputGap,"plugin output overflow explicitly requests resynchronization");

        // Staged topology and a rejected cycle must leave the active plan playing.
        one.subscription("graph.changed",true);
        const auto revision=s.graphState().revision;
        const auto b=s.add("",plugin);
        s.disconnectAudio(a,oscairn::graphOutput); s.connectAudio(a,b); s.connectAudio(b,oscairn::graphOutput);
        check(s.graphState().staged && s.graphState().names.at(b).empty(),"unnamed staged node supported");
        audio.render(s); check(std::abs(audio.l[0]-.81f)<1e-6,"staged edits not audible");
        s.connectAudio(b,a);
        bool rejected=false;
        try{s.commit();}catch(const std::runtime_error&){rejected=true;}
        check(rejected && s.graphState().revision==revision,"cycle preserves revision");
        check(one.notifications(0).empty(),"failed commit does not notify");
        audio.render(s); check(std::abs(audio.l[0]-.81f)<1e-6,"failed commit preserves audio");
        s.disconnectAudio(b,a); s.commit(); audio.render(s);
        check(std::abs(audio.l[0]-.81f)<1e-6 && contains(one.notifications(0),"graph.changed"),"repaired graph commits and notifies");
        s.remove(b); s.connectAudio(a,oscairn::graphOutput); s.commit(); audio.render(s);
        check(!s.hasNode(b) && std::abs(audio.l[0]-.81f)<1e-6,"remove prunes connections without losing survivor state");

        // Removal cancels pending commands and forgets the name; IDs are not reused.
        s.setParameter(a,1,.1); s.remove(a); s.commit(); audio.render(s);
        check(audio.l[0]==0,"removing instrument silences graph");
        const auto next=s.add("a",plugin); check(next>b,"node IDs not reused");
        s.service();
    }
}
int main(int argc,char** argv)
{
    if(argc!=2)return 2;
    try{run(argv[1]); std::cout<<"ok: coalescing, backpressure, gestures, subscribers and staged live graph edits\n";}
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}

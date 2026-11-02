// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "../src/NoteRoute.h"
#include "../../../hosts/audio/MidiBlock.h"
#include "AllocationGuard.h"
#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <vector>
using namespace oscairn;
using kick::clap::Event;
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
    if(argc!=4)return 2;
    Graph graph(48000,512);
    const auto a=graph.add(argv[2]),b=graph.add(argv[2]),synth=graph.add(argv[1]);
    {
        NoteRoute route(graph.host(synth));uint32_t nextId=40;
        std::array<Event,10> delivered;size_t count=0;
        auto emit=[&](const Event& e){if(count==delivered.size())return false;delivered[count++]=e;return true;};
        check(route.route(Event::note(true,60,1,0,0,0,7),nextId,emit),"assign destination note ID");
        check(route.route(Event::midi(0xa0,60,100),nextId,emit),"scope raw poly pressure to owned notes");
        check(reinterpret_cast<const clap_event_note_expression_t*>(delivered[1].header())->note_id==40,"pressure uses remapped ID");
        clap_event_param_value_t parameter{};parameter.header={sizeof(parameter),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
        parameter.note_id=7;parameter.param_id=1;parameter.key=60;parameter.value=.5;
        check(route.route(Event::from(parameter),nextId,emit),"per-note parameter delivered");
        check(reinterpret_cast<const clap_event_param_value_t*>(delivered[2].header())->note_id==40,"per-note parameter uses remapped ID");
        parameter.note_id=parameter.key=-1;
        check(route.route(Event::from(parameter),nextId,emit),"channel parameter delivered");
        check(reinterpret_cast<const clap_event_param_value_t*>(delivered[3].header())->note_id==-1,"channel-wide parameter remains channel-wide");
        check(route.route(Event::note(false,60,.37,0,0,0,7),nextId,emit),"release velocity delivered");
        check(reinterpret_cast<const clap_event_note_t*>(delivered[4].header())->velocity==.37,"release velocity preserved while remapping ID");
        check(route.route(Event::midi(0xb0,64,37),nextId,emit),"continuous pedal delivered");
        check(delivered[5].header()->type==CLAP_EVENT_MIDI && reinterpret_cast<const clap_event_midi_t*>(delivered[5].header())->data[2]==37,"half-pedal value reaches plugin unchanged");
        check(route.retire(emit,[](size_t,int){return uint8_t{91};}),"route retirement emits releases and remaining pedal");
        check(reinterpret_cast<const clap_event_midi_t*>(delivered[count-1].header())->data[2]==91,"retirement restores remaining route pedal value");
    }
    {
        // A note limit of two (RFC 0018 step 2): a third note chokes the oldest released note
        // before any held one, then the oldest held; a stolen note's release goes nowhere.
        NoteRoute route(graph.host(synth));uint32_t nextId=1;
        NoteZones zones;zones.count=1;zones.zones[0].maxNotes=2;
        std::array<Event,16> delivered;size_t count=0;
        auto emit=[&](const Event& e){if(count==delivered.size())return false;delivered[count++]=e;return true;};
        auto ignoreEnd=[](const NoteRoute::Note&,uint32_t){};
        auto chokes=[&](size_t from){std::vector<int> keys;for(size_t i=from;i<count;++i)if(delivered[i].header()->type==CLAP_EVENT_NOTE_CHOKE)keys.push_back(reinterpret_cast<const clap_event_note_t*>(delivered[i].header())->key);return keys;};
        route.route(Event::note(true,60,1),nextId,emit,ignoreEnd,&zones);route.route(Event::note(true,62,1),nextId,emit,ignoreEnd,&zones);
        route.route(Event::note(false,62,0),nextId,emit,ignoreEnd,&zones); // the newer note released, still sounding until the synth ends it
        check(chokes(0).empty(),"two notes fit");
        size_t mark=count;route.route(Event::note(true,64,1),nextId,emit,ignoreEnd,&zones);
        check(chokes(mark)==std::vector<int>{62},"the third note chokes the released note, though the held one is older");
        mark=count;route.route(Event::note(true,65,1),nextId,emit,ignoreEnd,&zones);
        check(chokes(mark)==std::vector<int>{60},"then the oldest held note");
        mark=count;route.route(Event::note(false,60,0),nextId,emit,ignoreEnd,&zones);
        check(count==mark,"a stolen note's release goes nowhere");
        zones.zones[0].maxNotes=0;mark=count;route.route(Event::note(true,67,1),nextId,emit,ignoreEnd,&zones);
        check(chokes(mark).empty(),"no limit, no chokes");
        // Melody and bass: with three held, a fourth spares the highest and lowest keys.
        NoteRoute spare(graph.host(synth));NoteZones three;three.count=1;three.zones[0].maxNotes=3;
        spare.route(Event::note(true,48,1),nextId,emit,ignoreEnd,&three);spare.route(Event::note(true,60,1),nextId,emit,ignoreEnd,&three);
        spare.route(Event::note(true,72,1),nextId,emit,ignoreEnd,&three);
        mark=count;spare.route(Event::note(true,64,1),nextId,emit,ignoreEnd,&three);
        check(chokes(mark)==std::vector<int>{60},"the oldest held note goes unless it is the bass or the melody");
    }
    {
        // The steal fade (RFC 0018): the stolen note's volume falls in steps from where it was
        // set, across blocks, then it is choked; its own volume expressions no longer reach it.
        NoteRoute route(graph.host(synth),240);uint32_t nextId=1;
        NoteZones one;one.count=1;one.zones[0].maxNotes=1;
        std::vector<Event> delivered;
        auto emit=[&](const Event& e){delivered.push_back(e);return true;};
        auto ignoreEnd=[](const NoteRoute::Note&,uint32_t){};
        auto expression=[](double value,uint32_t time){clap_event_note_expression_t v{};v.header={sizeof(v),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_EXPRESSION,0};
            v.expression_id=CLAP_NOTE_EXPRESSION_VOLUME;v.note_id=v.port_index=v.channel=-1;v.key=60;v.value=value;return Event::from(v);};
        route.route(Event::note(true,60,1),nextId,emit,ignoreEnd,&one);route.route(expression(.5,0),nextId,emit,ignoreEnd,&one);
        delivered.clear();
        route.route(Event::note(true,62,1,100),nextId,emit,ignoreEnd,&one);
        check(delivered.size()==1 && delivered[0].header()->type==CLAP_EVENT_NOTE_ON,"the new note plays at once; nothing is cut");
        route.route(expression(1,110),nextId,emit,ignoreEnd,&one);
        check(delivered.size()==1,"the stolen note's own volume no longer reaches it");
        // Blocks of 128 frames: the fade starts at 100 and ends at 340, in the third block.
        std::vector<std::pair<uint64_t,double>> steps;uint64_t chokedAt=0;
        for(uint64_t block=0;block<4;++block) {
            delivered.clear();check(route.fade(128,emit),"fade steps sent");route.advance(128);
            for(const auto& e:delivered) {
                const auto at=block*128+e.header()->time;
                check(e.header()->time<128,"steps fall inside the block");
                if(e.header()->type==CLAP_EVENT_NOTE_CHOKE){check(reinterpret_cast<const clap_event_note_t*>(e.header())->key==60,"the old note is choked");chokedAt=at;}
                else {const auto& v=*reinterpret_cast<const clap_event_note_expression_t*>(e.header());check(v.key==60 && v.note_id==1,"steps address the old note");steps.push_back({at,v.value});}
            }
        }
        check(steps.size()==NoteRoute::fadeSteps && steps.front().first==100 && std::abs(steps.front().second-.5*7/8)<1e-9 && steps.back().second==0,"eight steps down from its volume to silence");
        for(size_t i=1;i<steps.size();++i)check(steps[i].first==steps[i-1].first+30 && steps[i].second<steps[i-1].second,"evenly spaced, falling");
        check(chokedAt==340,"then choked when the fade ends");
    }
    graph.connectNotes(a,synth);graph.connectNotes(b,synth);graph.connectNotes(graphInput,synth);
    graph.connectAudio(synth,graphOutput);graph.commit();
    std::array<float,512> left{},right{};float* out[]{left.data(),right.data()};
    auto run=[&](std::span<const Event> events={}){
        forbidAllocation=true;const bool ok=graph.process(out,512,nullptr,events.data(),events.size());forbidAllocation=false;
        check(ok,"allocation-free graph rendering");
    };
    auto voices=[&]{return graph.host(synth).soundContexts().front().active_voices;};
    auto tail=[&]{for(int i=0;i<120;++i)run();};
    // Both sources emit the same channel, key and wildcard ID.
    graph.host(a).setParameter(1,1);graph.host(b).setParameter(1,1);run();check(voices()==2,"two routes own matching notes independently");
    graph.disconnectNotes(a,synth);graph.commit();run();check(voices()==1,"disconnect chokes only the departing route");
    graph.host(b).setParameter(1,-1);run();tail();check(voices()==0,"remaining route still releases its note");
    graph.connectNotes(a,synth);graph.commit();graph.host(a).setParameter(1,1);run();check(voices()==1,"reconnect starts fresh ownership");
    graph.remove(a);graph.commit();run();check(voices()==0,"removing source releases notes in surviving destination");

    const auto on=Event::midi(0x90,60,127),off=Event::midi(0x80,60,0),down=Event::midi(0xb0,64,127);
    run({&on,1});
    const auto lateOff=Event::midi(0x80,60,0,999);
    run({&lateOff,1});run({&off,1});tail();check(voices()==0,"late release cannot corrupt FIFO ownership");
    run({&on,1});const auto malformed=Event::note(false,60,0,0,0,0,-2);
    run({&malformed,1});tail();check(voices()==1,"malformed ID cannot become a wildcard release");
    run({&off,1});tail();check(voices()==0,"valid release still owns its note after malformed input");
    run({&on,1});run({&down,1});run({&off,1});tail();check(voices()==1,"pedal holds route-owned note");
    graph.disconnectNotes(graphInput,synth);graph.commit();graph.commit();run();check(voices()==0,"skipped plans still release pedal-held route notes");
    graph.connectNotes(graphInput,synth);graph.commit();
    // More than a ledger's lifetime capacity must be reusable after NOTE_END.
    for(int i=0;i<600;++i)run({&on,1});
    const auto choke=Event::midi(0xb0,120,0);run({&choke,1});
    check(graph.eventDrops()==0,"ended notes reclaim bounded ownership slots");
    run({&on,1});check(voices()==1,"held note before physical input overflow");
    host::MidiBlock block;
    const uint8_t pressure[]{0xd0,64};
    for(size_t i=0;i<block.events.size();++i)block.append(pressure,sizeof(pressure));
    const uint8_t release[]{0x80,60,0};block.append(release,sizeof(release));
    check(block.dropped==1,"physical block reports dropped release");
    graph.panicInput();run();check(voices()==0,"overflow recovery resets held notes and sustain");
    block.reset();block.append(release,sizeof(release));check(block.count==1 && !block.dropped,"next physical block recovers");
    const uint8_t bend[]{0xe0,0,96},wheel[]{0xb0,1,64},volume[]{0xb0,7,64};block.append(bend,3);block.append(wheel,3);block.append(volume,3);
    check(block.count==3,"physical input takes pitch bend and the mod wheel, not other controllers");
    block.reset();
    run({&on,1});run({&off,1});tail();check(voices()==0,"overflow does not leave sustain engaged");
    {
        Graph lanes(48000,512);const auto source=lanes.add(argv[2]),target=lanes.add(argv[2]);
        lanes.connectNotes(source,target);lanes.connectAudio(target,graphOutput);lanes.mapParamTarget(source,0,target,2);lanes.commit();
        lanes.host(source).setParameter(1,-2);
        forbidAllocation=true;const bool ok=lanes.process(out,512,nullptr);forbidAllocation=false;
        check(ok && left[0]==1,"per-note lane follows the remapped ID after a same-sample note-on");
    }
    {
        Graph midi(48000,512);const auto source=midi.add(argv[2]),target=midi.add(argv[3],"org.kick.test.midi");
        midi.connectNotes(source,target);midi.connectNotes(graphInput,target);midi.connectAudio(target,graphOutput);midi.commit();
        midi.host(source).setParameter(1,1);check(midi.process(out,512,nullptr) && left[0]==.5f,"MIDI-only route sounds");
        midi.process(out,512,nullptr,&down,1);
        midi.disconnectNotes(source,target);midi.commit();
        check(midi.process(out,512,nullptr) && left[0]==0,"MIDI-only retirement uses channel all-sound-off even under another route's sustain");
    }
    {
        // Through the graph, in 64-frame blocks: a note limit of one keeps the old note through its
        // 5 ms fade (240 frames), then chokes it.
        Graph limited(48000,64);const auto s=limited.add(argv[1]);
        NoteZone one;one.maxNotes=1;limited.connectNotes(graphInput,s,{one});limited.connectAudio(s,graphOutput);limited.commit();
        auto block=[&](std::span<const Event> events={}){forbidAllocation=true;const bool ok=limited.process(out,64,nullptr,events.data(),events.size());forbidAllocation=false;check(ok,"allocation-free fading");};
        auto playing=[&]{return limited.host(s).soundContexts().front().active_voices;};
        const auto first=Event::note(true,60,1),second=Event::note(true,62,1);
        block({&first,1});block({&second,1});block();block();
        check(playing()==2,"the stolen note sounds through its fade");
        for(int i=0;i<4;++i)block(); // the choke, then the synth's own 3 ms choke fade
        check(playing()==1,"and is choked after it");
    }
    // Draining and retiring (for seamless scene and Performance changes): a draining connection
    // takes no new note while the notes it holds still release, sustain included; a retiring
    // node is idle once nothing sounds through it and it has been silent long enough.
    {
        Graph d(48000,512);
        const auto s=d.add(argv[1]);d.connectNotes(graphInput,s);d.connectAudio(s,graphOutput);d.commit();
        auto play=[&](std::span<const Event> events={}){
            forbidAllocation=true;const bool ok=d.process(out,512,nullptr,events.data(),events.size());forbidAllocation=false;
            check(ok,"allocation-free rendering while draining");
        };
        auto held=[&]{return d.host(s).soundContexts().front().active_voices;};
        const auto other=Event::midi(0x90,64,127),pedalUp=Event::midi(0xb0,64,0);
        play({&on,1});check(held()==1,"a note held before draining");
        d.setDraining(graphInput,s,true);d.setRetiring(s,true);d.commit();play();
        play({&other,1});check(held()==1,"a draining connection takes no new note");
        check(!d.idle(s,0,UINT64_MAX),"a held note keeps a retiring node busy");
        play({&down,1});play({&off,1});play();check(!d.idle(s,0,UINT64_MAX),"so does a note the pedal sustains");
        play({&pedalUp,1});for(int i=0;i<120;++i)play();
        check(held()==0,"the held note still released through the draining connection");
        check(d.idle(s,512*4,UINT64_MAX),"then idle once silent");
        check(!d.idle(s,uint64_t(48000)*60,UINT64_MAX),"but not before the silence asked for");
        d.setDraining(graphInput,s,false);d.commit();play({&other,1});check(held()==1,"undrained, it takes notes again");
    }
    {
        // A retiring instrument that says it is asleep (CLAP_PROCESS_SLEEP) is idle as soon as its
        // notes end, without the silence window: through the prepared-instrument wrapper too.
        Graph g(48000,512);
        const auto s=g.add(argv[3],"org.kick.test.sleep");g.connectNotes(graphInput,s);g.connectAudio(s,graphOutput);g.commit();
        auto play=[&](std::span<const Event> events={}){check(g.process(out,512,nullptr,events.data(),events.size()),"render the sleeping fixture");};
        const auto down=Event::note(true,60,1),up=Event::note(false,60,0);
        play({&down,1});
        g.setDraining(graphInput,s,true);g.setRetiring(s,true);g.commit();play();
        check(!g.idle(s,UINT64_MAX,UINT64_MAX),"busy while its note is held");
        play({&up,1});play();play();
        check(g.idle(s,UINT64_MAX,UINT64_MAX),"asleep once released: idle without waiting for silence");
    }
    std::cout<<"ok: route ownership, retirement, sustain, physical MIDI overflow, the steal fade, draining and retiring\n";
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}

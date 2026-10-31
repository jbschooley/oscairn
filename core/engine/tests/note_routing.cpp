// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "../src/NoteRoute.h"
#include "../../../hosts/audio/MidiBlock.h"
#include "AllocationGuard.h"
#include <array>
#include <iostream>
#include <span>
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
    std::cout<<"ok: route ownership, retirement, sustain and physical MIDI overflow\n";
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}

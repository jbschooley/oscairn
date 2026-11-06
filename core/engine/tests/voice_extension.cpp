// SPDX-License-Identifier: AGPL-3.0-or-later
// RFC 0018 step 4: oscairn.voice-budget, rendered offline. Usage: basic-synth voice-fixture
// [hostile-worker]; with voice-extension-workers, in workers (the limits and the report cross the
// worker's shared block).
//
// One pool, several sounds (the SDK helper, in the Basic Synth): today's case is a Performance
// change, the outgoing sound ringing out beside the incoming one in one instance (RFC 0021). With
// the instance limited to six voices, a held mono key of the outgoing sound and the incoming
// sound's held notes survive, while the outgoing sound's released voices are given up first,
// though they are the loudest; with fewer voices still, the incoming sound gives up its own,
// though the mono key is the quietest.
//
// Elements before notes (the voice fixture, four voices a note, each half as loud as the one
// before, as an AWM2-class sound's elements, through the SDK's voice-budget-plugin.hpp): under the
// shared budget the host limits its voices, and it drops its quietest elements, so every note
// still sounds, and the host gives up none of its notes.
//
// With a hostile worker (one writing out-of-range counts, costs and levels after each block) only:
// the engine bounds what it reads, and the budget goes on with it.
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
using namespace oscairn;
using kick::clap::Event;
static void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}

int main(int argc,char** argv){try{
    if(argc!=3 && argc!=4)return 2;
    constexpr uint32_t frames=64;
    std::array<float,frames> left{},right{};float* out[]{left.data(),right.data()};
    if(argc==4){
        PlacementPolicy policy;policy.worker=argv[3];setPlacementPolicy(policy);
        Graph g(48000,frames);const auto s=g.add(argv[1]);
        g.connectNotes(graphInput,s);g.connectAudio(s,graphOutput);g.commit();
        g.setVoiceBudget(600);
        auto& synth=g.host(s);
        check(synth.inWorker() && synth.voiceBudget(),"the Basic Synth in the hostile worker, with the extension");
        std::array<oscairn_voice_budget_context_t,OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS+4> many{};
        synth.setVoiceLimits(4,many.data(),uint32_t(many.size())); // more contexts than the block holds
        bool clamped=false;
        for(int i=0;i<40;++i){
            const auto e=Event::note(true,48+i,.8);
            forbidAllocation=true;g.process(out,frames,nullptr,&e,1);forbidAllocation=false;
            double cost;const oscairn_voice_budget_note_t* notes;uint32_t count;
            const uint32_t voices=synth.voiceReport(cost,notes,count);
            check(voices<=(1u<<16) && count<=kick::clap::Host::maxBlockEvents,"counts bounded: "+std::to_string(voices)+" voices, "+std::to_string(count)+" notes");
            check(std::isfinite(cost) && cost>=0 && cost<=1e6,"a cost in range: "+std::to_string(cost));
            for(uint32_t n=0;n<count;++n)check(std::isfinite(notes[n].loudness) && notes[n].loudness>=0,"a level in range: "+std::to_string(notes[n].loudness));
            clamped=clamped || (voices==(1u<<16) && count==kick::clap::Host::maxBlockEvents);
        }
        check(clamped,"the worker's out-of-range report arrived, bounded");
        check(synth.failure().empty(),"the worker kept running: "+synth.failure());
        std::puts("ok: a hostile worker's voice report bounded");
        return 0;
    }
    {
        Graph g(48000,frames);const auto s=g.add(argv[1]);
        g.connectNotes(graphInput,s);g.connectAudio(s,graphOutput);g.commit();
        auto& synth=g.host(s);
        check(synth.voiceBudget() && synth.honorsVolume(),"the Basic Synth offers oscairn.voice-budget and honors volume expressions");
        auto block=[&](std::span<const Event> events={}){forbidAllocation=true;const bool ok=g.process(out,frames,nullptr,events.data(),uint32_t(events.size()));forbidAllocation=false;check(ok,"rendered without allocating");};
        auto voices=[&](uint32_t id){for(const auto& c:synth.soundContexts())if(c.id==id)return c.active_voices;return 0u;};
        auto play=[&](bool on,int key,double velocity=1){const auto e=Event::note(on,key,velocity);block({&e,1});};
        // The outgoing sound (the default, 0): a soft held mono key and three loud released notes ringing.
        play(true,36,.1);for(const int k:{60,62,64}){play(true,k);play(false,k);}
        // The incoming sound, selected on every channel: the default one retires.
        const auto next=synth.prepareSound("Next",{});check(synth.selectSound(next,-1),"the incoming sound selected");block();
        // Six voices for the instance; the outgoing sound plays mono, the incoming one is the part's.
        const std::array<oscairn_voice_budget_context_t,2> limits{{{0,0,0,0,OSCAIRN_VOICE_BUDGET_MONO},{OSCAIRN_VOICE_BUDGET_SELECTED,0,0,0,0}}};
        synth.setVoiceLimits(6,limits.data(),uint32_t(limits.size()));block();
        check(voices(0)==4,"the outgoing sound holds its mono key and rings three released notes: "+std::to_string(voices(0)));
        play(true,72,.2);play(true,74,.2);
        check(voices(0)==4 && voices(next)==2,"six voices fit");
        for(const int k:{76,77,79})play(true,k,.2);
        for(int i=0;i<4;++i)block(); // the 3 ms fades end
        check(voices(0)==1,"the outgoing sound's released voices gave way, its held mono key did not: "+std::to_string(voices(0)));
        check(voices(next)==5,"the incoming sound's notes all sound: "+std::to_string(voices(next)));
        // Past that, the incoming sound gives up its own voice, never the held mono key.
        play(true,81,.2);for(int i=0;i<4;++i)block();
        check(voices(0)==1 && voices(next)==5,"a seventh note takes the incoming sound's own voice: "+std::to_string(voices(0))+" and "+std::to_string(voices(next)));
        // A lower limit, at the next block: the incoming sound's voices go, not the quieter mono key.
        synth.setVoiceLimits(4,limits.data(),uint32_t(limits.size()));for(int i=0;i<4;++i)block();
        check(voices(0)==1 && voices(next)==3,"within four, the held mono key stays: "+std::to_string(voices(0))+" and "+std::to_string(voices(next)));
        synth.setVoiceLimits(6,limits.data(),uint32_t(limits.size()));block();
        // The report: every note's loudness and voices.
        double cost;const oscairn_voice_budget_note_t* notes;uint32_t count;
        check(synth.voiceReport(cost,notes,count)==4 && count==4,"four notes reported, a voice each");
        for(uint32_t i=0;i<count;++i)check(notes[i].voices==1 && notes[i].loudness>0 && notes[i].loudness<=1,"each with its level now");
    }
    {
        ::setenv("OSCAIRN_FIXTURE_VOICE_US","60",1);::setenv("OSCAIRN_FIXTURE_LAYERS","4",1);::setenv("OSCAIRN_FIXTURE_VOICE_BUDGET","1",1);
        Graph g(48000,frames);const auto s=g.add(argv[2]);
        g.connectNotes(graphInput,s);g.connectAudio(s,graphOutput);g.commit();
        // 30 us a voice: the soft limit, 85% of 600 us, holds about 17 of the 32 voices eight notes ask for.
        g.setVoiceBudget(600);
        auto block=[&](std::span<const Event> events={}){forbidAllocation=true;const bool ok=g.process(out,frames,nullptr,events.data(),uint32_t(events.size()));forbidAllocation=false;check(ok,"rendered without allocating");};
        for(int i=0;i<20;++i)block(); // at rest, measured
        for(int k=0;k<8;++k){const auto e=Event::note(true,60+k,1);block({&e,1});block();}
        for(int i=0;i<40;++i)block();
        double cost;const oscairn_voice_budget_note_t* notes;uint32_t count;
        const uint32_t voices=g.host(s).voiceReport(cost,notes,count);
        const auto given=g.noteCost(s);
        std::printf("layers: %u voices for %u notes, %.1f us a voice measured, %llu notes faded and %llu choked by the host\n",voices,count,given.perNote,(unsigned long long)given.stolen,(unsigned long long)given.hardChokes);
        check(count==8,"every note still sounds: "+std::to_string(count));
        check(voices<32 && voices<=20,"its quietest elements gave way: "+std::to_string(voices)+" voices");
        uint32_t whole=0;for(uint32_t i=0;i<count;++i)whole+=notes[i].voices==4;
        check(whole<8,"elements dropped from notes, not notes from the sound");
        for(uint32_t i=0;i<count;++i)check(notes[i].loudness>.9f,"each note keeps its loudest element");
        check(given.stolen==0 && given.hardChokes==0,"the host gave up none of its notes");
    }
    std::puts("ok: one pool's policy across sounds, and elements given up before notes");
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}

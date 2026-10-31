// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/plugin_host.h"
#include "oscairn/midi_input.h"
#include "oscairn/note_zone.h"
#include <array>
#include <climits>

namespace oscairn
{
// One audio-thread ledger per connection; shared by successive compiled plans.
// Destination IDs separate otherwise identical notes from different graph sources.
struct NoteRoute
{
    using Event=kick::clap::Event;
    // address: the note as its source sent it, which releases and expressions match; key: the
    // key it plays here, after its zone's transpose; zone: which zone let it through, or -1;
    // source: where it entered the graph, so another device's release of the same key leaves it.
    // stolen: choked to keep within the connection's note limit, waiting for the plugin to end it.
    struct Note { clap_event_note_t address{}; int32_t id=-1; bool used=false,held=false,midi=false,stolen=false; int16_t key=-1; int8_t zone=-1; uint8_t source=untaggedSource; };
    std::array<Note,kick::clap::Host::maxBlockEvents> notes{};
    std::array<std::array<uint8_t,16>,16> pedal{};
    std::array<bool,16> clap{};
    uint32_t ports=0;
    explicit NoteRoute(const PluginHost& host):ports(host.noteInputPorts()) {for(uint32_t p=0;p<clap.size();++p)clap[p]=host.usesClapNotes(p);}
    void clear() noexcept {for(auto& n:notes)n.used=false;for(auto& p:pedal)p.fill(0);}
    // Whether an event from `source` may release or shape a note: untagged events reach every note.
    static bool from(const Note& n,uint8_t source) noexcept {return source==untaggedSource || n.source==source;}
    static bool matches(const Note& n,int32_t id,int16_t port,int16_t channel,int16_t key) noexcept
    {return n.used && (id<0 || id==n.address.note_id) && (port<0 || port==n.address.port_index) &&
        (channel<0 || channel==n.address.channel) && (key<0 || key==n.address.key);}
    Event release(const Note& n,uint32_t time,bool choke) const noexcept
    {
        auto e=n.address;e.header.time=time;e.header.type=choke?CLAP_EVENT_NOTE_CHOKE:CLAP_EVENT_NOTE_OFF;
        e.key=n.key;e.note_id=clap[e.port_index]?n.id:-1;e.velocity=0;return Event::from(e);
    }
    void ended(const Event& e) noexcept
    {
        if(e.header()->space_id!=CLAP_CORE_EVENT_SPACE_ID || e.header()->type!=CLAP_EVENT_NOTE_END || e.header()->size!=sizeof(clap_event_note_t))return;
        const auto& end=*reinterpret_cast<const clap_event_note_t*>(e.header());
        for(auto& n:notes)if(n.used && (end.note_id<0 || n.id==end.note_id) &&
            (end.port_index<0 || n.address.port_index==end.port_index) &&
            (end.channel<0 || n.address.channel==end.channel) && (end.key<0 || n.key==end.key))n.used=false;
    }
    template<class Emit,class RemainingPedal> bool retire(Emit&& emit,RemainingPedal&& remainingPedal) noexcept
    {
        bool ok=true;
        std::array<std::array<bool,16>,16> panicChannels{};
        for(auto& n:notes)if(n.used) {
            if(clap[n.address.port_index])ok=emit(release(n,0,true)) && ok;
            else panicChannels[n.address.port_index][n.address.channel]=true;
        }
        // MIDI cannot choke one pedal-held voice; silence its channel rather than strand it.
        for(size_t p=0;p<pedal.size();++p)for(int c=0;c<16;++c) {
            if(panicChannels[p][c])ok=emit(Event::midi(0xb0|c,120,0,0,p)) && ok;
            if(pedal[p][c])ok=emit(Event::midi(0xb0|c,64,remainingPedal(p,c),0,p)) && ok;
        }
        clear();return ok;
    }
    template<class Emit> bool route(Event e,uint32_t& nextId,Emit&& emit,const NoteZones* zones=nullptr,uint8_t source=untaggedSource) noexcept
    {return route(e,nextId,emit,[](const Note&,uint32_t){},zones,source);}
    // zones, when given, decide which notes pass and how they change (note_zone.h); each zone
    // that accepts a note plays its own copy. source: where the event entered the graph.
    template<class Emit,class End> bool route(Event e,uint32_t& nextId,Emit&& emit,End&& end,const NoteZones* zones=nullptr,uint8_t source=untaggedSource) noexcept
    {
        if(!e.valid())return emit(e);
        const auto* h=e.header();
        if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID)return emit(e);
        bool midi=false;
        if(h->type==CLAP_EVENT_MIDI && h->size==sizeof(clap_event_midi_t))
        {
            const auto m=*reinterpret_cast<const clap_event_midi_t*>(h);
            if(m.port_index>=ports || m.port_index>=16)return emit(e);
            const auto kind=m.data[0]&0xf0,channel=m.data[0]&15;
            if(zones && zones->count && kind>=0x80 && kind<0xf0 && !zones->passes(channel,source))return true; // another zone's channel or device
            if(kind==0x90 || kind==0x80) {
                e=Event::note(kind==0x90 && m.data[2],m.data[1],m.data[2]/127.0,h->time,channel,m.port_index);midi=true;h=e.header();
            } else if(kind==0xb0 && m.data[1]==64) {
                pedal[m.port_index][channel]=m.data[2];
                if(!clap[m.port_index] && m.data[2]<64)for(auto& n:notes)
                    if(n.used && !n.held && n.address.port_index==m.port_index && n.address.channel==channel){end(n,h->time);n.used=false;}
                return emit(e);
            } else if(kind==0xb0 && (m.data[1]==120 || m.data[1]==123)) {
                auto n=Event::note(false,-1,0,h->time,channel,m.port_index);
                if(m.data[1]==120)reinterpret_cast<clap_event_header_t*>(n.bytes.data())->type=CLAP_EVENT_NOTE_CHOKE;
                return route(n,nextId,emit,end,zones,source);
            } else if(kind==0xa0 || kind==0xd0) {
                Event pressure;
                if(!midiToClap(m.data,kind==0xd0?2:3,h->time,pressure))return emit(e);
                reinterpret_cast<clap_event_note_expression_t*>(pressure.bytes.data())->port_index=m.port_index;
                return route(pressure,nextId,emit,end,zones,source);
            } else return emit(e);
        }
        if(h->type==CLAP_EVENT_NOTE_ON && h->size==sizeof(clap_event_note_t))
        {
            const auto n=*reinterpret_cast<const clap_event_note_t*>(h);
            if(n.port_index>=16 || static_cast<uint32_t>(n.port_index)>=ports)return emit(e);
            // A note limit: before a note past it, the oldest released note goes, else the oldest
            // held one. Only where the plugin takes CLAP notes: MIDI cannot choke one note.
            const uint16_t limit=zones && zones->count && clap[n.port_index]?zones->zones[0].maxNotes:0;
            auto makeRoom=[&]()->bool {
                if(!limit)return true;
                for(;;) {
                    Note* victim=nullptr;size_t playing=0;
                    for(auto& slot:notes)if(slot.used && !slot.stolen) {
                        ++playing;
                        if(!victim || (victim->held && !slot.held) || (victim->held==slot.held && slot.id<victim->id))victim=&slot;
                    }
                    if(playing<limit || !victim)return true;
                    victim->stolen=true;victim->held=false;
                    if(!emit(release(*victim,h->time,true)))return false;
                }
            };
            auto play=[&](int8_t zone,int16_t key,double velocity) {
                if(!makeRoom())return false;
                for(auto& slot:notes)if(!slot.used) {
                    if(nextId>INT32_MAX)return false;
                    slot={n,static_cast<int32_t>(nextId++),true,true,midi,false,key,zone,source};
                    auto copy=n;copy.key=key;copy.velocity=velocity;copy.note_id=clap[n.port_index]?slot.id:-1;return emit(Event::from(copy));
                }
                return false;
            };
            if(!zones || !zones->count)return play(-1,n.key,n.velocity);
            bool ok=true;
            for(size_t z=0;z<zones->count;++z)if(const auto& zone=zones->zones[z];zone.accepts(n.key,n.velocity,n.channel,source))
                if(const auto key=n.key+zone.transpose;key>=0 && key<=127)ok=play(int8_t(z),int16_t(key),zone.shape(n.velocity)) && ok;
            return ok; // no zone took it: dropped, and so is its release

        }
        if((h->type==CLAP_EVENT_NOTE_OFF || h->type==CLAP_EVENT_NOTE_CHOKE) && h->size==sizeof(clap_event_note_t))
        {
            const auto n=*reinterpret_cast<const clap_event_note_t*>(h);const bool choke=h->type==CLAP_EVENT_NOTE_CHOKE;
            // A repeated MIDI key releases its oldest note, in each zone that played it.
            std::array<Note*,NoteZones::capacity+1> oldest{};
            if(midi)for(auto& slot:notes)if(slot.held && slot.midi && from(slot,source) && matches(slot,-1,n.port_index,n.channel,n.key))
                if(auto& o=oldest[size_t(slot.zone+1)];!o || slot.id<o->id)o=&slot;
            bool ok=true;
            for(auto& slot:notes)if(!slot.stolen && from(slot,source) && matches(slot,n.note_id,n.port_index,n.channel,n.key) && (!midi || &slot==oldest[size_t(slot.zone+1)])) {
                slot.held=false;
                const bool sustained=pedal[slot.address.port_index][slot.address.channel]>=64;
                auto output=release(slot,h->time,choke);
                auto* off=reinterpret_cast<clap_event_note_t*>(output.bytes.data());off->velocity=n.velocity;off->header.flags=h->flags;
                ok=emit(output) && ok;
                if(choke || (!clap[slot.address.port_index] && !sustained)){end(slot,h->time);slot.used=false;}
            }
            return ok;
        }
        if(h->type==CLAP_EVENT_NOTE_EXPRESSION && h->size==sizeof(clap_event_note_expression_t))
        {
            const auto expression=*reinterpret_cast<const clap_event_note_expression_t*>(h);bool ok=true;
            for(const auto& slot:notes)if(from(slot,source) && matches(slot,expression.note_id,expression.port_index,expression.channel,expression.key)) {
                auto copy=expression;copy.note_id=clap[slot.address.port_index]?slot.id:-1;
                copy.port_index=slot.address.port_index;copy.channel=slot.address.channel;copy.key=slot.key;
                ok=emit(Event::from(copy)) && ok;
            }
            return ok;
        }
        if((h->type==CLAP_EVENT_PARAM_VALUE && h->size==sizeof(clap_event_param_value_t)) ||
           (h->type==CLAP_EVENT_PARAM_MOD && h->size==sizeof(clap_event_param_mod_t))) {
            // Both CLAP parameter event structs have the same note-address fields.
            auto remap=[&]<class T>(const T& parameter) {
                if(parameter.note_id<0 && parameter.key<0)return emit(e);
                bool ok=true;
                for(const auto& slot:notes)if(matches(slot,parameter.note_id,parameter.port_index,parameter.channel,parameter.key)) {
                    auto copy=parameter;copy.note_id=clap[slot.address.port_index]?slot.id:-1;
                    copy.port_index=slot.address.port_index;copy.channel=slot.address.channel;copy.key=slot.key;
                    ok=emit(Event::from(copy)) && ok;
                }
                return ok;
            };
            if(h->type==CLAP_EVENT_PARAM_VALUE)return remap(*reinterpret_cast<const clap_event_param_value_t*>(h));
            return remap(*reinterpret_cast<const clap_event_param_mod_t*>(h));
        }
        if(h->type==CLAP_EVENT_NOTE_END)return true; // consumed by the owning destination, never forwarded
        return emit(e);
    }
};
}

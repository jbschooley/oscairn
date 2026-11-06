// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/graph.h"
#include "oscairn/plugin_host.h"
#include "oscairn/midi_input.h"
#include "oscairn/note_zone.h"
#include <array>
#include <algorithm>
#include <climits>
#include <span>

namespace oscairn
{
struct NoteRoute;
// RFC 0018's shared budget (option B), one per plan: every note connection into a plugin that
// takes CLAP notes, with the node it plays and that node's cost (Graph::Activity). Audio thread
// only: notes are routed, and each step's cost read, on the graph's own thread in plan order,
// while only the plugins' rendering moves to helper threads (RFC 0034); the counters it keeps
// are atomics, read elsewhere. Fixed in size: choosing what to steal scans the ledgers.
struct VoiceBudget
{
    // group: the first entry of its part (its zones' part; draining connections apart, being
    // another Performance's), else itself; rest: the first entry into its node, which counts the
    // node's cost at rest.
    struct Entry { NoteRoute* route; const NoteZones* zones; Graph::Activity* cost; uint32_t group; bool rest,draining; };
    struct Group { uint32_t held=0; int16_t low=INT16_MAX,high=-1; float cost=0; }; // cost: of its held notes
    std::vector<Entry> entries;
    std::vector<Group> groups; // scratch, one per entry
    // limit: this block's hard limit in microseconds, 0 for none; others: the last block's cost of
    // the plugins taking no CLAP notes (effects), which give way to nothing but count.
    float limit=0,others=0;
    bool over=false; // the last block measured past the limit
    static constexpr float soft=.85f; // stealing starts here, so fades have time
    // Before a note into `self` at `time` passes on: under the soft limit, notes fade to make room
    // for it; past the hard limit, they are choked.
    void admit(NoteRoute& self,uint32_t time) noexcept;
    // After a block that took `measured` microseconds in all: past the hard limit in this block and
    // the one before, notes are choked from the next block's start until the estimate, scaled to
    // the measurement, fits.
    void measured(float measured) noexcept;
private:
    struct Victim { const Entry* entry=nullptr; size_t slot=0; };
    enum Counting { Kept, Fading, Ending }; // the notes counted: not given up; and those fading; and those choked, until they end
    float estimate(Counting counting) const noexcept;
    Victim choose(bool reserved) noexcept;
    Victim fadingNote() const noexcept;
    float reserves() noexcept;
    void choke(float scale,float extra,const NoteRoute* self,uint32_t time,bool reserved) noexcept;
};

// One audio-thread ledger per connection; shared by successive compiled plans.
// Destination IDs separate otherwise identical notes from different graph sources.
struct NoteRoute
{
    using Event=kick::clap::Event;
    // address: the note as its source sent it, which releases and expressions match; key: the
    // key it plays here, after its zone's transpose; zone: which zone let it through, or -1;
    // source: where it entered the graph, so another device's release of the same key leaves it.
    // stolen: given up to keep within the connection's note limit, fading then choked, waiting for
    // the plugin to end it. volume: its last volume expression; fadeAt: when its next fade step
    // is due in this block, or -1; faded: the steps sent.
    struct Note { clap_event_note_t address{}; int32_t id=-1; bool used=false,held=false,midi=false,stolen=false; int16_t key=-1; int8_t zone=-1; uint8_t source=untaggedSource;
        double volume=1; int32_t fadeAt=-1; uint8_t faded=0;
        bool voiceEnded=false; // its voice ended (NOTE_END) while the key was held
        float velocity=0; }; // as it was sent on, for the budget's quietest first
    static constexpr uint8_t fadeSteps=8;
    std::array<Note,kick::clap::Host::maxBlockEvents> notes{};
    std::array<std::array<uint8_t,16>,16> pedal{};
    std::array<bool,16> clap{};
    uint32_t ports=0;
    uint32_t fadeFrames=0,fading=0; // a stolen note's fade, 0 to choke at once; notes fading
    explicit NoteRoute(uint32_t ports):ports(ports) {} // a MIDI destination: MIDI's notes on every port
    explicit NoteRoute(const PluginHost& host,uint32_t fadeFrames=0):ports(host.noteInputPorts()),fadeFrames(fadeFrames) {for(uint32_t p=0;p<clap.size();++p)clap[p]=host.usesClapNotes(p);}
    void clear() noexcept {for(auto& n:notes)n.used=false;for(auto& p:pedal)p.fill(0);fading=0;}
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
    // Notes still sounding by the player's hand: keys down, or released under the sustain pedal.
    uint32_t sounding() const noexcept
    {uint32_t count=0;for(const auto& n:notes)if(n.used && (n.held || pedal[n.address.port_index&15][n.address.channel&15]>=64))++count;return count;}
    void ended(const Event& e) noexcept
    {
        if(e.header()->space_id!=CLAP_CORE_EVENT_SPACE_ID || e.header()->type!=CLAP_EVENT_NOTE_END || e.header()->size!=sizeof(clap_event_note_t))return;
        const auto& end=*reinterpret_cast<const clap_event_note_t*>(e.header());
        for(auto& n:notes)if(n.used && (end.note_id<0 || n.id==end.note_id) &&
            (end.port_index<0 || n.address.port_index==end.port_index) &&
            (end.channel<0 || n.address.channel==end.channel) && (end.key<0 || n.key==end.key))
            {
                // NOTE_END says a voice ended, not that the key went up: a mono synth gliding one voice
                // ends a new note's ID at once. A held key keeps its slot, so its release still goes out.
                if(n.held)n.voiceEnded=true;else n.used=false;
            }
    }
    // A stolen note fades (RFC 0018): its volume expression falls in steps over fadeFrames, for
    // plugins that do not fade a choke themselves, then it is choked. Sends the steps due before
    // `until`, in time order across notes.
    template<class Emit> bool fade(uint32_t until,Emit&& emit) noexcept
    {
        bool ok=true;
        while(fading) {
            Note* next=nullptr;fading=0;
            for(auto& n:notes)if(n.used && n.fadeAt>=0){++fading;if(!next || n.fadeAt<next->fadeAt)next=&n;}
            if(!next || uint32_t(next->fadeAt)>=until)break;
            const auto at=uint32_t(next->fadeAt);
            if(next->faded==fadeSteps){next->fadeAt=-1;ok=emit(release(*next,at,true)) && ok;continue;}
            clap_event_note_expression_t v{};v.header={sizeof(v),at,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_EXPRESSION,0};
            v.expression_id=CLAP_NOTE_EXPRESSION_VOLUME;v.note_id=next->id;v.port_index=next->address.port_index;
            v.channel=next->address.channel;v.key=next->key;v.value=next->volume*(fadeSteps-1-next->faded)/fadeSteps;
            ++next->faded;next->fadeAt+=int32_t(fadeFrames/fadeSteps);
            ok=emit(Event::from(v)) && ok;
        }
        return ok;
    }
    // The notes it holds until their plugin ends them, released and fading ones included.
    uint32_t live() const noexcept {uint32_t count=0;for(const auto& n:notes)count+=n.used;return count;}
    // Chokes a note at `at` without (more of) a fade, at the budget's hard limit.
    void choke(Note& n,uint32_t at) noexcept
    {
        if(n.stolen && n.fadeAt>=0)n.fadeAt=std::min(n.fadeAt,int32_t(at));
        else {n.stolen=true;n.held=false;n.fadeAt=int32_t(at);++fading;}
        n.faded=fadeSteps;
    }
    // Gives a note up from elsewhere (a peer's note limit): it fades from `at`, or is choked there.
    void steal(Note& n,uint32_t at) noexcept
    {n.stolen=true;n.held=false;n.fadeAt=int32_t(at);n.faded=fadeFrames?0:fadeSteps;++fading;}
    // A block of `frames` done: fades carry on from the next block's start.
    void advance(uint32_t frames) noexcept
    {if(fading)for(auto& n:notes)if(n.used && n.fadeAt>=0)n.fadeAt=std::max(0,n.fadeAt-int32_t(frames));}
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
    template<class Emit> bool route(Event e,uint32_t& nextId,Emit&& emit,const NoteZones* zones=nullptr,uint8_t source=untaggedSource,bool draining=false,std::span<NoteRoute* const> peers={},VoiceBudget* budget=nullptr) noexcept
    {return route(e,nextId,emit,[](const Note&,uint32_t){},zones,source,draining,peers,budget);}
    // zones, when given, decide which notes pass and how they change (note_zone.h); each zone
    // that accepts a note plays its own copy. source: where the event entered the graph.
    // draining: a connection being retired (RFC 0021's seamless switching) takes no new notes;
    // releases, expressions and the pedal still reach the notes it holds. peers: the routes sharing
    // the note limit (a part's lanes), this one included; none: this one alone. budget: the plan's
    // shared budget, which makes room for each new note across every part.
    template<class Emit,class End> bool route(Event e,uint32_t& nextId,Emit&& emit,End&& end,const NoteZones* zones=nullptr,uint8_t source=untaggedSource,bool draining=false,std::span<NoteRoute* const> peers={},VoiceBudget* budget=nullptr) noexcept
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
            // Releases and chokes pass any zone: they end only notes this route played, which zones
            // since changed may no longer pass; one for a note never played here matches nothing.
            const bool release=kind==0x80 || (kind==0x90 && !m.data[2]) || (kind==0xb0 && (m.data[1]==120 || m.data[1]==123));
            if(zones && zones->count && kind>=0x80 && kind<0xf0 && !release && !zones->passes(channel,source))return true; // another zone's channel or device
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
                return route(n,nextId,emit,end,zones,source,draining,peers,budget);
            } else if(kind==0xa0 || kind==0xd0) {
                Event pressure;
                if(!midiToClap(m.data,kind==0xd0?2:3,h->time,pressure))return emit(e);
                reinterpret_cast<clap_event_note_expression_t*>(pressure.bytes.data())->port_index=m.port_index;
                return route(pressure,nextId,emit,end,zones,source,draining,peers,budget);
            } else return emit(e);
        }
        if(h->type==CLAP_EVENT_NOTE_ON && h->size==sizeof(clap_event_note_t))
        {
            if(draining)return true;
            const auto n=*reinterpret_cast<const clap_event_note_t*>(h);
            if(n.port_index>=16 || static_cast<uint32_t>(n.port_index)>=ports)return emit(e);
            // A note limit: before a note past it, the oldest released note goes, else the oldest
            // held one, sparing the highest and lowest held keys (melody and bass) while another
            // can go; it fades, then is choked. Counted across the peers, the notes they can choke:
            // only where the plugin takes CLAP notes, as MIDI cannot choke one note.
            const uint16_t limit=zones && zones->count && clap[n.port_index]?zones->zones[0].maxNotes:0;
            NoteRoute* const self[]{this};
            const auto routes=peers.empty()?std::span<NoteRoute* const>(self):peers;
            auto makeRoom=[&]()->bool {
                if(!limit)return true;
                for(;;) {
                    Note* victim=nullptr;NoteRoute* owner=nullptr;size_t playing=0,held=0;int16_t low=INT16_MAX,high=-1;
                    for(const auto* r:routes)for(const auto& slot:r->notes)if(slot.used && !slot.stolen && r->clap[slot.address.port_index] && slot.held){++held;low=std::min(low,slot.key);high=std::max(high,slot.key);}
                    auto spared=[&](const Note& n){return n.held && held>2 && (n.key==low || n.key==high);};
                    for(auto* r:routes)for(auto& slot:r->notes)if(slot.used && !slot.stolen && r->clap[slot.address.port_index]) {
                        ++playing;
                        if(!victim || (spared(*victim) && !spared(slot)) || (spared(*victim)==spared(slot) &&
                           ((victim->held && !slot.held) || (victim->held==slot.held && slot.id<victim->id)))){victim=&slot;owner=r;}
                    }
                    if(playing<limit || !victim)return true;
                    if(owner!=this){owner->steal(*victim,0);continue;} // its step sends the fade, from its block's start
                    victim->stolen=true;victim->held=false;
                    if(fadeFrames){victim->fadeAt=int32_t(h->time);++fading;}
                    else if(!emit(release(*victim,h->time,true)))return false;
                }
            };
            auto play=[&](int8_t zone,int16_t key,double velocity) {
                if(!makeRoom())return false;
                if(budget)budget->admit(*this,h->time);
                // Full: a held note whose voice already ended gives its slot up, as before NOTE_END kept it.
                if(std::none_of(notes.begin(),notes.end(),[](const Note& x){return !x.used;}))
                    for(auto& slot:notes)if(slot.used && slot.voiceEnded){slot.used=false;break;}
                for(auto& slot:notes)if(!slot.used) {
                    if(nextId>INT32_MAX)return false;
                    slot={n,static_cast<int32_t>(nextId++),true,true,midi,false,key,zone,source};slot.velocity=float(velocity);
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
            bool ok=true,sent=false;
            for(auto& slot:notes)if(!slot.stolen && from(slot,source) && matches(slot,n.note_id,n.port_index,n.channel,n.key) && (!midi || &slot==oldest[size_t(slot.zone+1)])) {
                // A note whose voice already ended needs only the key's release, once: not one per slot.
                if(slot.voiceEnded && sent){slot.used=false;continue;}
                sent=true;
                slot.held=false;
                const bool sustained=pedal[slot.address.port_index][slot.address.channel]>=64;
                auto output=release(slot,h->time,choke);
                auto* off=reinterpret_cast<clap_event_note_t*>(output.bytes.data());off->velocity=n.velocity;off->header.flags=h->flags;
                ok=emit(output) && ok;
                if(slot.voiceEnded)slot.used=false; // its end was the plugin's own, already reported
                else if(choke || (!clap[slot.address.port_index] && !sustained)){end(slot,h->time);slot.used=false;}
            }
            return ok;
        }
        if(h->type==CLAP_EVENT_NOTE_EXPRESSION && h->size==sizeof(clap_event_note_expression_t))
        {
            const auto expression=*reinterpret_cast<const clap_event_note_expression_t*>(h);bool ok=true;
            const bool volume=expression.expression_id==CLAP_NOTE_EXPRESSION_VOLUME;
            for(auto& slot:notes)if(from(slot,source) && matches(slot,expression.note_id,expression.port_index,expression.channel,expression.key)) {
                if(volume && slot.stolen)continue; // its fade owns its volume
                if(volume)slot.volume=expression.value;
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
// What the plan's plugins are estimated to cost now: at rest, and their notes at each node's cost
// per note, counting the notes kept, or those and the ones fading out, or every note until it ends.
inline float VoiceBudget::estimate(Counting counting) const noexcept
{
    float total=others;
    for(const auto& e:entries)
    {
        if(e.rest)total+=e.cost->restNow();
        uint32_t n=0;
        for(const auto& slot:e.route->notes)
            if(slot.used && (!slot.stolen || counting==Ending || (counting==Fading && slot.fadeAt>=0 && slot.faded<NoteRoute::fadeSteps)))++n;
        total+=float(n)*e.cost->noteNow();
    }
    return total;
}
// The next note to give up, in RFC 0018's order: released notes, then held notes beyond their
// part's reserve, its highest and lowest held keys last; within each, the part of lower priority
// first, then the quietest, then the oldest. reserved: held notes within a reserve may go too.
// Only notes the plugin takes as CLAP notes, which a choke can address, of nodes whose notes cost.
inline VoiceBudget::Victim VoiceBudget::choose(bool reserved) noexcept
{
    for(auto& g:groups)g={};
    for(const auto& e:entries)for(const auto& slot:e.route->notes)if(slot.used && !slot.stolen && slot.held)
    {auto& g=groups[e.group];++g.held;g.low=std::min(g.low,slot.key);g.high=std::max(g.high,slot.key);}
    Victim best;int bestRank=0;uint8_t bestPriority=0;const NoteRoute::Note* chosen=nullptr;
    for(const auto& e:entries)
    {
        if(e.cost->noteNow()<=0)continue; // giving its notes up frees nothing measured
        const auto& g=groups[e.group];
        const NoteZone* z=e.zones && e.zones->count?&e.zones->zones[0]:nullptr;
        const uint16_t reserve=z?z->reserve:0;const uint8_t priority=z?z->priority:0;
        for(size_t i=0;i<e.route->notes.size();++i)
        {
            const auto& slot=e.route->notes[i];
            if(!slot.used || slot.stolen || !e.route->clap[slot.address.port_index&15])continue;
            int rank=0;
            if(slot.held)
            {
                if(!reserved && g.held<=reserve)continue;
                rank=g.held>2 && (slot.key==g.low || slot.key==g.high)?2:1;
            }
            const bool better=!chosen || (rank!=bestRank?rank<bestRank:priority!=bestPriority?priority<bestPriority:
                slot.velocity!=chosen->velocity?slot.velocity<chosen->velocity:slot.id<chosen->id);
            if(better){best={&e,i};chosen=&slot;bestRank=rank;bestPriority=priority;}
        }
    }
    return best;
}
// The plugins at rest and each part's reserved held notes alone (a part's held notes' cost, in
// proportion to the part's reserve).
inline float VoiceBudget::reserves() noexcept
{
    for(auto& g:groups)g={};
    float total=others;
    for(const auto& e:entries)
    {
        if(e.rest)total+=e.cost->restNow();
        for(const auto& slot:e.route->notes)if(slot.used && !slot.stolen && slot.held){auto& g=groups[e.group];++g.held;g.cost+=e.cost->noteNow();}
    }
    for(size_t i=0;i<entries.size();++i)if(const auto& g=groups[i];g.held)
    {
        const auto* z=entries[i].zones;const uint16_t reserve=z && z->count?z->zones[0].reserve:0;
        total+=g.cost*float(std::min<uint32_t>(g.held,reserve))/float(g.held);
    }
    return total;
}
// The oldest note still fading out, which a choke finishes.
inline VoiceBudget::Victim VoiceBudget::fadingNote() const noexcept
{
    Victim best;const NoteRoute::Note* chosen=nullptr;
    for(const auto& e:entries)if(e.cost->noteNow()>0)for(size_t i=0;i<e.route->notes.size();++i)
    {
        const auto& slot=e.route->notes[i];
        if(slot.used && slot.stolen && slot.fadeAt>=0 && slot.faded<NoteRoute::fadeSteps && (!chosen || slot.id<chosen->id)){best={&e,i};chosen=&slot;}
    }
    return best;
}
// Past the hard limit: notes choked without a fade, fading ones first; a note not yet fading counted.
inline void VoiceBudget::choke(float scale,float extra,const NoteRoute* self,uint32_t time,bool reserved) noexcept
{
    while(estimate(Fading)*scale+extra>limit)
    {
        auto v=fadingNote();
        if(!v.entry)v=choose(false);
        if(!v.entry && reserved && reserves()*scale+extra>limit)v=choose(true); // reserves alone pass the hard limit
        if(!v.entry)return;
        auto& note=v.entry->route->notes[v.slot];
        if(!note.stolen)v.entry->cost->hardChokes.fetch_add(1,std::memory_order_relaxed); // a fading note was counted when it was stolen
        v.entry->route->choke(note,v.entry->route==self?time:0);
    }
}
inline void VoiceBudget::admit(NoteRoute& self,uint32_t time) noexcept
{
    if(limit<=0)return;
    const Entry* mine=nullptr;
    for(const auto& e:entries)if(e.route==&self){mine=&e;break;}
    if(!mine)return;
    const float cost=mine->cost->noteNow();
    while(estimate(Kept)+cost>soft*limit)
    {
        const auto v=choose(false);
        if(!v.entry)break;
        v.entry->route->steal(v.entry->route->notes[v.slot],v.entry->route==&self?time:0);
        v.entry->cost->stolen.fetch_add(1,std::memory_order_relaxed);
    }
    choke(1,cost,&self,time,true);
}
inline void VoiceBudget::measured(float total) noexcept
{
    // Twice running: one block over may be the thread held up from outside, or a burst admitted.
    const bool again=std::exchange(over,total>limit);
    if(limit<=0 || total<=limit || !again)return;
    // The measurement covers every note still ending; what is left once they end must fit. At most
    // a quarter over the estimate a block, and never a reserve: a block's time also has the
    // machine's in it, and the estimate follows the measurement within blocks anyway.
    if(const float e=estimate(Ending);e>0)choke(std::min(total/e,1.25f),0,nullptr,0,false);
}
}

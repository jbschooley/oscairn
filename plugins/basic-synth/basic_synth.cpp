// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Basic Synth: a small polyphonic CLAP instrument for tests and demos.
// Each voice uses a band-limited saw, detuned saw pair or square, with a pitch LFO,
// through a two-pole low-pass filter whose
// cutoff follows the amplitude envelope, which gives a plucked bass sound.
// Brightness, Level, Pressure, oscillator mode, detune and pitch LFO settings belong
// to each sound context. Continuous controls take values and modulation on their
// exact sample. Pressure (CLAP's pressure note expression: polyphonic or, with a wildcard
// key, channel aftertouch) applies per voice.
// Events are applied on their exact sample, and the output does not depend on the
// host's block size.

#include <clap/clap.h>
#include <oscairn-clap/sound-contexts.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    constexpr double pi = 3.14159265358979323846;

    const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.basic-synth",
        "Oscairn Basic Synth",
        "Oscairn",
        "https://github.com/jbschooley/oscairn",
        "",
        "",
        "0.1.0",
        "A small polyphonic subtractive synth for tests and demos",
        features,
    };

    // Stable parameter IDs are the table index + 1.
    struct Parameter { const char* name; double maximum, initial; const char* unit; bool stepped=false; };
    constexpr std::array<Parameter,7> parameters{{
        {"Brightness",1,1,"%"}, {"Level",1,1,"%"}, {"Pressure",1,.5,"%"},
        {"Oscillator",2,0,"",true}, {"Detune",50,7,"cents"},
        {"Pitch LFO rate",40,0,"Hz"}, {"Pitch LFO depth",200,0,"cents"}
    }};
    constexpr auto defaults=[] {std::array<double,parameters.size()> v{};for(size_t i=0;i<v.size();++i)v[i]=parameters[i].initial;return v;}();
    constexpr std::array<const char*,3> oscillators{"Saw","Detuned saws","Square"};
    constexpr double pressureCutoff = 6000.0; // Hz the filter opens at full pressure and amount

    enum class Stage
    {
        Attack,
        Decay,
        Release
    };

    struct Voice
    {
        bool     active   = false, held = false;
        uint32_t context = 0;
        int32_t noteId = -1;
        Stage    stage    = Stage::Attack;
        int16_t  key      = 0;
        int16_t  channel  = 0;
        double   phase    = 0.0, phase2 = 0.0, lfoPhase = 0.0;
        double   increment = 0.0;
        double   level    = 0.0; // envelope
        double   velocity = 0.0;
        double   pressure = 0.0; // 0..1, from note expressions
        double   low1 = 0.0, low2 = 0.0;
        uint64_t started = 0;
    };

    struct Context
    {
        // Main-thread metadata; audio uses only pinned slot indices.
        bool prepared=false;
        uint32_t id=0;
        char name[64]{};
        std::array<double,parameters.size()> values=defaults,modulation{};
        // users includes selected channels, queued selections and active voices.
        std::atomic<uint32_t> users{0},selected{0},voices{0};
    };

    struct Synth
    {
        clap_plugin_t          plugin{};
        const clap_output_events_t* output=nullptr;
        uint32_t eventTime=0;
        double                 sampleRate = 48000.0;
        std::array<Voice, 16>  voices{};
        uint64_t               notesStarted = 0;
        double                 attackStep = 0.0, decayCoefficient = 0.0, releaseCoefficient = 0.0;
        static constexpr double sustain = 0.35;

        // Values are atomic: the main thread reads them (get_value) while audio runs.
        std::array<std::atomic<double>,parameters.size()> values{};
        static constexpr uint32_t contextCapacity=8,selectionCapacity=32;
        std::array<Context,contextCapacity> contexts;
        std::array<uint32_t,16> selected{}; // audio thread
        std::array<bool,16> sustainDown{};
        struct Selection { uint32_t slot;int16_t channel; };
        std::array<Selection,selectionCapacity> selections{};
        std::atomic<uint32_t> selectionHead{0},selectionTail{0};
        uint32_t nextContext=1; // main thread, never reuse IDs
        Synth(){for(size_t i=0;i<values.size();++i)values[i]=defaults[i];contexts[0].prepared=true;contexts[0].users=16;contexts[0].selected=65535;std::strcpy(contexts[0].name,"Default");}
    };

    Synth& self( const clap_plugin_t* p )
    {
        return *static_cast<Synth*>( p->plugin_data );
    }

    double polyBlep( double t, double dt )
    {
        if( t < dt )
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }
        if( t > 1.0 - dt )
        {
            t = ( t - 1.0 ) / dt;
            return t * t + t + t + 1.0;
        }
        return 0.0;
    }

    void finishVoice(Synth& s,Voice& v)
    {
        if(!v.active)return;
        if(s.output) {
            clap_event_note_t end{};end.header={sizeof(end),s.eventTime,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_END,0};
            end.note_id=v.noteId;end.port_index=0;end.channel=v.channel;end.key=v.key;
            s.output->try_push(s.output,&end.header);
        }
        v.active=false;
        auto& c=s.contexts[v.context];
        c.voices.fetch_sub(1,std::memory_order_relaxed);
        // Last access to this context before releasing its lifetime pin.
        c.users.fetch_sub(1,std::memory_order_release);
    }
    void resetVoices(Synth& s)
    {
        for(auto& v:s.voices)finishVoice(s,v);
        s.sustainDown.fill(false);
    }
    void noteOn(Synth& s,const clap_event_note_t& note)
    {
        if(note.port_index!=0 || note.channel<0 || note.channel>15 || note.key<0 || note.key>127)return;
        Voice* voice=nullptr;
        for(auto& v:s.voices)if(!v.active){voice=&v;break;}
        if(!voice){voice=&s.voices[0];for(auto& v:s.voices)if(v.started<voice->started)voice=&v;finishVoice(s,*voice);}
        *voice=Voice{};
        voice->active=voice->held=true;voice->key=note.key;voice->channel=note.channel;voice->noteId=note.note_id;
        voice->context=s.selected[note.channel];voice->velocity=note.velocity;
        voice->increment=440.0*std::pow(2.0,(note.key-69)/12.0)/s.sampleRate;voice->started=++s.notesStarted;
        auto& c=s.contexts[voice->context];c.users.fetch_add(1,std::memory_order_relaxed);c.voices.fetch_add(1,std::memory_order_relaxed);
    }
    bool matches(const Voice& v,int32_t id,int16_t key,int16_t channel)
    {return v.active && (id<0 || v.noteId==id) && (key<0 || v.key==key) && (channel<0 || v.channel==channel);}
    void noteOff(Synth& s,const clap_event_note_t& note,bool choke=false,bool midi=false)
    {
        if(note.port_index>0)return;
        // Raw MIDI has no note IDs; only that dialect uses oldest-held-first release.
        Voice* oldest=nullptr;
        const bool fifo=midi && !choke && note.note_id<0 && note.key>=0 && note.channel>=0;
        for(auto& v:s.voices)if(matches(v,note.note_id,note.key,note.channel) && (choke || v.held))
        {
            if(fifo){if(!oldest || v.started<oldest->started)oldest=&v;continue;}
            v.held=false;
            if(choke)finishVoice(s,v);
            else if(note.key<0 || !s.sustainDown[v.channel])v.stage=Stage::Release;
        }
        if(oldest){oldest->held=false;if(!s.sustainDown[oldest->channel])oldest->stage=Stage::Release;}
        // An emergency wildcard release must also release already pedal-held voices.
        if(note.key<0)for(auto& v:s.voices)if(matches(v,note.note_id,note.key,note.channel))v.stage=Stage::Release;
    }
    void applyExpression(Synth& s,const clap_event_note_expression_t& e)
    {
        if(e.expression_id!=CLAP_NOTE_EXPRESSION_PRESSURE || e.port_index>0)return;
        for(auto& v:s.voices)if(matches(v,e.note_id,e.key,e.channel))v.pressure=std::clamp(e.value,0.0,1.0);
    }
    void applyMidi(Synth& s,const clap_event_midi_t& e)
    {
        if(e.port_index)return;
        const auto channel=e.data[0]&15,kind=e.data[0]&0xf0;
        if(kind==0x90 || kind==0x80)
        {
            clap_event_note_t n{};n.note_id=-1;n.channel=channel;n.key=e.data[1];n.velocity=e.data[2]/127.0;
            if(kind==0x90 && e.data[2])noteOn(s,n);else noteOff(s,n,false,true);
            return;
        }
        if(kind==0xa0 || kind==0xd0)
        {
            clap_event_note_expression_t pressure{};pressure.expression_id=CLAP_NOTE_EXPRESSION_PRESSURE;
            pressure.note_id=-1;pressure.channel=channel;pressure.key=kind==0xa0?e.data[1]:-1;
            pressure.value=(kind==0xa0?e.data[2]:e.data[1])/127.0;applyExpression(s,pressure);return;
        }
        if(kind!=0xb0)return;
        if(e.data[1]==64)
        {
            s.sustainDown[channel]=e.data[2]>=64;
            if(!s.sustainDown[channel])for(auto& v:s.voices)if(v.active && v.channel==channel && !v.held)v.stage=Stage::Release;
        }
        else if(e.data[1]==120 || e.data[1]==123)
        {
            clap_event_note_t n{};n.note_id=-1;n.key=-1;n.channel=channel;
            noteOff(s,n,e.data[1]==120);
        }
    }
    void applyParam(Synth& s,const clap_event_header_t* header)
    {
        if(header->space_id!=CLAP_CORE_EVENT_SPACE_ID)return;
        clap_id id;double value;int16_t channel;bool modulation=false;
        if(header->type==CLAP_EVENT_PARAM_VALUE){const auto& e=*reinterpret_cast<const clap_event_param_value_t*>(header);id=e.param_id;value=e.value;channel=e.channel;if(e.note_id>=0 || e.key>=0 || e.port_index>0)return;}
        else if(header->type==CLAP_EVENT_PARAM_MOD){const auto& e=*reinterpret_cast<const clap_event_param_mod_t*>(header);id=e.param_id;value=e.amount;channel=e.channel;modulation=true;if(e.note_id>=0 || e.key>=0 || e.port_index>0)return;}
        else return;
        if(id<1 || id>parameters.size() || channel < -1 || channel>15 || !std::isfinite(value))return;
        if(!modulation){value=std::clamp(value,0.0,parameters[id-1].maximum);if(parameters[id-1].stepped)value=std::round(value);s.values[id-1]=value;}
        else if(parameters[id-1].stepped)return;
        std::array<bool,Synth::contextCapacity> changed{};
        for(int ch=0;ch<16;++ch)if(channel<0 || channel==ch)
        {
            const auto slot=s.selected[ch];if(changed[slot])continue;changed[slot]=true;
            auto& c=s.contexts[slot];(modulation?c.modulation:c.values)[id-1]=value;
        }
    }
    void applySelections(Synth& s,const clap_output_events_t* output)
    {
        auto tail=s.selectionTail.load(std::memory_order_relaxed);
        const auto head=s.selectionHead.load(std::memory_order_acquire);
        while(tail!=head)
        {
            const auto selection=s.selections[tail%Synth::selectionCapacity];auto& next=s.contexts[selection.slot];
            for(int ch=0;ch<16;++ch)if(selection.channel<0 || selection.channel==ch)
            {
                const auto old=s.selected[ch];if(old==selection.slot)continue;
                next.users.fetch_add(1,std::memory_order_relaxed);next.selected.fetch_or(1u<<ch,std::memory_order_relaxed);
                s.selected[ch]=selection.slot;s.contexts[old].selected.fetch_and(~(1u<<ch),std::memory_order_relaxed);
                s.contexts[old].users.fetch_sub(1,std::memory_order_release);
            }
            if(selection.channel<0)
            {
                for(uint32_t i=0;i<parameters.size();++i){s.values[i]=next.values[i];clap_event_param_value_t e{};e.header={sizeof(e),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};e.param_id=i+1;e.note_id=e.port_index=e.channel=e.key=-1;e.value=next.values[i];output->try_push(output,&e.header);}
            }
            next.users.fetch_sub(1,std::memory_order_release); // queued-selection pin
            ++tail;
        }
        s.selectionTail.store(tail,std::memory_order_release);
    }

    void render( Synth& s, float* left, float* right, uint32_t from, uint32_t to )
    {
        // Snapshot only contexts pinned by active voices. After a voice finishes,
        // the control thread may reclaim its slot; the rest of this span uses copies.
        std::array<std::array<double,parameters.size()>,Synth::contextCapacity> settings{};
        std::array<double,Synth::contextCapacity> detune{};
        std::array<bool,Synth::contextCapacity> copied{};
        for(const auto& v:s.voices)if(v.active && !copied[v.context])
        {
            copied[v.context]=true;const auto& c=s.contexts[v.context];
            for(size_t p=0;p<parameters.size();++p)settings[v.context][p]=std::clamp(c.values[p]+c.modulation[p],0.0,parameters[p].maximum);
            detune[v.context]=settings[v.context][3]==1?std::pow(2.0,settings[v.context][4]/1200.0):1;
        }
        for( uint32_t i = from; i < to; ++i )
        {
            s.eventTime=i;
            std::array<double,Synth::contextCapacity> mix{},levels{};
            for( auto& v : s.voices )
            {
                if( !v.active )
                    continue;

                switch( v.stage )
                {
                    case Stage::Attack:
                        v.level += s.attackStep;
                        if( v.level >= 1.0 )
                        {
                            v.level = 1.0;
                            v.stage = Stage::Decay;
                        }
                        break;
                    case Stage::Decay:
                        v.level = Synth::sustain + ( v.level - Synth::sustain ) * s.decayCoefficient;
                        break;
                    case Stage::Release:
                        v.level *= s.releaseCoefficient;
                        if( v.level < 1e-4 )
                        {
                            finishVoice(s,v);
                            continue;
                        }
                        break;
                }

                const auto& patch=settings[v.context];
                double increment=v.increment;
                if(patch[6]>0 && patch[5]>0)
                    increment*=std::pow(2.0,std::sin(2*pi*v.lfoPhase)*patch[6]/1200.0);
                v.lfoPhase+=patch[5]/s.sampleRate;v.lfoPhase-=std::floor(v.lfoPhase);
                const auto mode=static_cast<int>(patch[3]);
                const double dt=std::min(.49,mode==1?increment/detune[v.context]:increment);
                double oscillator;
                if(mode==2)
                {
                    const double shifted=v.phase<.5?v.phase+.5:v.phase-.5;
                    oscillator=(v.phase<.5?1.0:-1.0)+polyBlep(v.phase,dt)-polyBlep(shifted,dt);
                }
                else
                {
                    oscillator=2.0*v.phase-1.0-polyBlep(v.phase,dt);
                    if(mode==1)
                    {
                        const double dt2=std::min(.49,increment*detune[v.context]);
                        oscillator=.5*(oscillator+2.0*v.phase2-1.0-polyBlep(v.phase2,dt2));
                        v.phase2+=dt2;if(v.phase2>=1)v.phase2-=1;
                    }
                }
                v.phase+=dt;if(v.phase>=1)v.phase-=1;

                const double brightness=settings[v.context][0],pressure=settings[v.context][2];
                levels[v.context]=settings[v.context][1];
                const double cutoff = 120.0 + v.level * v.level * 5000.0 * v.velocity * brightness + v.pressure * pressure * pressureCutoff;
                const double g      = 1.0 - std::exp( -2.0 * pi * cutoff / s.sampleRate );
                v.low1 += g * ( oscillator - v.low1 );
                v.low2 += g * ( v.low1 - v.low2 );

                mix[v.context] += v.low2 * v.level * v.velocity * 0.35;
            }
            double sum=0;for(size_t c=0;c<mix.size();++c)if(mix[c]!=0)sum+=std::tanh(mix[c]*levels[c]);
            const auto sample=static_cast<float>(sum);
            left[i]           = sample;
            right[i]          = sample;
        }
    }

    clap_process_status process( const clap_plugin_t* p, const clap_process_t* process )
    {
        auto& s = self( p );
        if( process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2 )
            return CLAP_PROCESS_ERROR;
        float* left  = process->audio_outputs[0].data32[0];
        float* right = process->audio_outputs[0].data32[1];

        s.output=process->out_events;s.eventTime=0;
        struct ClearOutput {Synth& s;~ClearOutput(){s.output=nullptr;}} clearOutput{s};
        applySelections(s,process->out_events);
        const uint32_t frames = process->frames_count;
        const uint32_t count  = process->in_events->size( process->in_events );
        uint32_t       done   = 0;
        for( uint32_t e = 0; e < count; ++e )
        {
            const auto* header = process->in_events->get( process->in_events, e );
            if( header->space_id != CLAP_CORE_EVENT_SPACE_ID )
                continue;
            const uint32_t at = header->time < frames ? header->time : frames;
            if( at > done )
            {
                render( s, left, right, done, at );
                done = at;
            }
            s.eventTime=at;
            if( header->type == CLAP_EVENT_PARAM_VALUE || header->type == CLAP_EVENT_PARAM_MOD )
                applyParam( s, header );
            else if( header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF || header->type == CLAP_EVENT_NOTE_CHOKE )
            {
                const auto* note = reinterpret_cast<const clap_event_note_t*>( header );
                if( header->type == CLAP_EVENT_NOTE_ON )
                    noteOn(s,*note);
                else
                    noteOff(s,*note,header->type==CLAP_EVENT_NOTE_CHOKE);
            }
            else if(header->type==CLAP_EVENT_MIDI)applyMidi(s,*reinterpret_cast<const clap_event_midi_t*>(header));
            else if( header->type == CLAP_EVENT_NOTE_EXPRESSION )
                applyExpression( s, *reinterpret_cast<const clap_event_note_expression_t*>( header ) );
        }
        render( s, left, right, done, frames );
        return CLAP_PROCESS_CONTINUE;
    }

    uint32_t audioPortsCount( const clap_plugin_t*, bool isInput )
    {
        return isInput ? 0 : 1;
    }

    bool audioPortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info )
    {
        if( isInput || index != 0 )
            return false;
        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf( info->name, sizeof( info->name ), "%s", "Output" );
        return true;
    }

    uint32_t notePortsCount( const clap_plugin_t*, bool isInput )
    {
        return isInput ? 1 : 0;
    }

    bool notePortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info )
    {
        if( !isInput || index != 0 )
            return false;
        info->id                 = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf( info->name, sizeof( info->name ), "%s", "Notes" );
        return true;
    }

    uint32_t paramsCount( const clap_plugin_t* ) { return parameters.size(); }

    bool paramsInfo( const clap_plugin_t*, uint32_t index, clap_param_info_t* info )
    {
        if(index>=parameters.size())return false;
        const auto& parameter=parameters[index];
        *info={};info->id=index+1;
        info->flags=CLAP_PARAM_IS_AUTOMATABLE | (parameter.stepped?CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM:CLAP_PARAM_IS_MODULATABLE);
        info->max_value=parameter.maximum;info->default_value=parameter.initial;
        std::snprintf(info->name,sizeof(info->name),"%s",parameter.name);
        return true;
    }

    bool paramsValue( const clap_plugin_t* p, clap_id id, double* value )
    {
        if(id<1 || id>parameters.size())return false;
        *value=self(p).values[id-1];return true;
    }

    bool paramsToText( const clap_plugin_t*, clap_id id, double value, char* text, uint32_t size )
    {
        if(id<1 || id>parameters.size() || !std::isfinite(value))return false;
        value=std::clamp(value,0.0,parameters[id-1].maximum);
        if(id==4)std::snprintf(text,size,"%s",oscillators[static_cast<size_t>(std::round(value))]);
        else if(id<=3)std::snprintf(text,size,"%.0f%%",value*100);
        else std::snprintf(text,size,"%.1f %s",value,parameters[id-1].unit);
        return true;
    }

    bool paramsFromText( const clap_plugin_t*, clap_id id, const char* text, double* value )
    {
        if(id<1 || id>parameters.size())return false;
        if(id==4)for(size_t i=0;i<oscillators.size();++i)if(std::strcmp(text,oscillators[i])==0){*value=i;return true;}
        char* end=nullptr;double parsed=std::strtod(text,&end);
        if(end==text || !std::isfinite(parsed))return false;
        while(*end==' ')++end;
        if(*end && std::strcmp(end,parameters[id-1].unit)!=0)return false;
        if(id<=3)parsed/=100;
        if(parsed<0 || parsed>parameters[id-1].maximum || (parameters[id-1].stepped && parsed!=std::round(parsed)))return false;
        *value=parsed;return true;
    }

    void paramsFlush( const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* )
    {
        for( uint32_t i = 0, n = in->size( in ); i < n; ++i )
            applyParam( self( p ), in->get( in, i ) );
    }

    const clap_plugin_params_t      params     = { paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush };
    const clap_plugin_audio_ports_t audioPorts = { audioPortsCount, audioPortsGet };
    const clap_plugin_note_ports_t  notePorts  = { notePortsCount, notePortsGet };

    const oscairn_plugin_sound_contexts_t soundContexts{
        [](const clap_plugin_t*)->uint32_t{return Synth::contextCapacity;},
        [](const clap_plugin_t* p,uint32_t slot,oscairn_sound_context_info_t* info)->bool {
            if(slot>=Synth::contextCapacity || !info)return false;const auto& c=self(p).contexts[slot];
            if(!c.prepared)return false;*info={};info->id=c.id;std::memcpy(info->name,c.name,sizeof(c.name));
            info->selected_channels=c.selected.load();info->active_voices=c.voices.load();info->idle=c.users.load(std::memory_order_acquire)==0;return true;
        },
        [](const clap_plugin_t* p,const char* name,const oscairn_sound_parameter_t* supplied,uint32_t count)->uint32_t {
            if(!name || std::strlen(name)>63 || count>defaults.size() || (count && !supplied))return CLAP_INVALID_ID;
            auto patch=defaults;std::array<bool,defaults.size()> seen{};
            for(uint32_t i=0;i<count;++i)
            {
                const auto& v=supplied[i];
                if(v.id<1 || v.id>parameters.size() || seen[v.id-1] || !std::isfinite(v.value))return CLAP_INVALID_ID;
                const auto& definition=parameters[v.id-1];
                if(v.value<0 || v.value>definition.maximum || (definition.stepped && v.value!=std::round(v.value)))return CLAP_INVALID_ID;
                seen[v.id-1]=true;patch[v.id-1]=v.value;
            }
            auto& s=self(p);if(s.nextContext==CLAP_INVALID_ID)return CLAP_INVALID_ID;
            for(uint32_t i=1;i<Synth::contextCapacity;++i){auto& c=s.contexts[i];if(c.prepared)continue;
                c.values=patch;c.modulation={};c.id=s.nextContext++;std::strcpy(c.name,name);c.prepared=true;return c.id;}
            return CLAP_INVALID_ID;
        },
        [](const clap_plugin_t* p,uint32_t id,int16_t channel)->bool {
            if(channel < -1 || channel>15)return false;auto& s=self(p);
            const auto head=s.selectionHead.load(std::memory_order_relaxed);
            if(head-s.selectionTail.load(std::memory_order_acquire)==Synth::selectionCapacity)return false;
            for(uint32_t i=0;i<Synth::contextCapacity;++i){auto& c=s.contexts[i];if(!c.prepared || c.id!=id)continue;
                c.users.fetch_add(1,std::memory_order_relaxed);s.selections[head%Synth::selectionCapacity]={i,channel};s.selectionHead.store(head+1,std::memory_order_release);return true;}
            return false;
        },
        [](const clap_plugin_t* p,uint32_t id)->bool {
            if(!id)return false;for(auto& c:self(p).contexts)if(c.prepared && c.id==id){if(c.users.load(std::memory_order_acquire))return false;c.prepared=false;return true;}return false;
        }
    };

    const clap_plugin_t* create()
    {
        auto* s                    = new Synth;
        s->plugin.desc             = &descriptor;
        s->plugin.plugin_data      = s;
        s->plugin.init             = []( const clap_plugin_t* ) { return true; };
        s->plugin.destroy          = []( const clap_plugin_t* p ) { delete &self( p ); };
        s->plugin.activate         = []( const clap_plugin_t* p, double rate, uint32_t, uint32_t ) {
            auto& s              = self( p );
            s.sampleRate         = rate;
            s.attackStep         = 1.0 / ( 0.002 * rate );
            s.decayCoefficient   = std::exp( -1.0 / ( 0.25 * rate ) );
            s.releaseCoefficient = std::exp( -1.0 / ( 0.12 * rate ) );
            resetVoices(s);
            return true;
        };
        s->plugin.deactivate       = []( const clap_plugin_t* ) {};
        s->plugin.start_processing = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing  = []( const clap_plugin_t* ) {};
        s->plugin.reset            = []( const clap_plugin_t* p ) { resetVoices(self(p)); };
        s->plugin.process          = process;
        s->plugin.get_extension    = []( const clap_plugin_t*, const char* id ) -> const void* {
            if(std::strcmp(id,OSCAIRN_EXT_SOUND_CONTEXTS)==0)return &soundContexts;
            if( std::strcmp( id, CLAP_EXT_AUDIO_PORTS ) == 0 )
                return &audioPorts;
            if( std::strcmp( id, CLAP_EXT_NOTE_PORTS ) == 0 )
                return &notePorts;
            if( std::strcmp( id, CLAP_EXT_PARAMS ) == 0 )
                return &params;
            return nullptr;
        };
        s->plugin.on_main_thread   = []( const clap_plugin_t* ) {};
        return &s->plugin;
    }

    const clap_plugin_factory_t factory = {
        []( const clap_plugin_factory_t* ) -> uint32_t { return 1; },
        []( const clap_plugin_factory_t*, uint32_t index ) -> const clap_plugin_descriptor_t* { return index == 0 ? &descriptor : nullptr; },
        []( const clap_plugin_factory_t*, const clap_host_t*, const char* id ) -> const clap_plugin_t* {
            return std::strcmp( id, descriptor.id ) == 0 ? create() : nullptr;
        },
    };
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    []( const char* ) { return true; },
    []() {},
    []( const char* id ) -> const void* { return std::strcmp( id, CLAP_PLUGIN_FACTORY_ID ) == 0 ? &factory : nullptr; },
};

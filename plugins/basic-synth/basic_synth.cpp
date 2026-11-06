// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Basic Synth: a small polyphonic CLAP instrument for tests and demos.
// Each voice uses a band-limited saw, detuned saw pair or square, with a pitch LFO,
// through a two-pole low-pass filter whose
// cutoff follows the amplitude envelope, which gives a plucked bass sound.
// Brightness, Level, Pressure, oscillator mode, detune and pitch LFO settings belong
// to each sound context. Continuous controls take values and modulation on their
// exact sample. Pressure (CLAP's pressure note expression: polyphonic or, with a wildcard
// key, channel aftertouch) applies per voice. Pitch bend moves each voice by the sound's
// Pitch bend range; the mod wheel adds vibrato through the pitch LFO unless the sound turns it off.
// Events are applied on their exact sample, and the output does not depend on the
// host's block size. Its factory presets are listed through CLAP preset discovery and
// prepared as sound contexts (oscairn.sound-contexts draft 5). Sounds no channel selects any more
// ring out into the host's ring-out buffer when it gives one (oscairn.sound-snapshots draft 3).
// Within the voices the host gives it (oscairn.voice-budget draft 1) it gives voices up itself,
// released and quiet ones first, and reports each note's level from its envelope.

#include <clap/clap.h>
#include <oscairn-clap/preset-discovery-plugin.hpp>
#include <oscairn-clap/sound-contexts-plugin.hpp>
#include <oscairn-clap/ui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <locale>

namespace
{
    constexpr double pi = 3.14159265358979323846;

    const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.basic-synth",
        "Oscairn Basic Synth",
        "Oscairn",
        "https://github.com/oscairn/oscairn",
        "",
        "",
        "0.1.0",
        "A small polyphonic subtractive synth for tests and demos",
        features,
    };

    // Stable parameter IDs are the table index + 1. Each with the control it wants, its
    // priority (oscairn.ui): the most played first, so physical knobs take them in that order,
    // and its role: Brightness is the filter's cutoff, Level the output level.
    struct Parameter { const char* name; double maximum, initial; const char* unit; bool stepped=false; uint32_t widget=OSCAIRN_WIDGET_KNOB, priority=0, group=0, role=OSCAIRN_ROLE_NONE; };
    constexpr std::array<Parameter,10> parameters{{
        {"Brightness",1,1,"%",false,OSCAIRN_WIDGET_KNOB,1,0,OSCAIRN_ROLE_CUTOFF}, {"Level",1,1,"%",false,OSCAIRN_WIDGET_KNOB,2,0,OSCAIRN_ROLE_LEVEL}, {"Pressure",1,.5,"%",false,OSCAIRN_WIDGET_KNOB,5},
        {"Oscillator",2,0,"",true,OSCAIRN_WIDGET_BUTTONS,3}, {"Detune",50,7,"cents",false,OSCAIRN_WIDGET_KNOB,4},
        {"Pitch LFO rate",40,0,"Hz",false,OSCAIRN_WIDGET_KNOB,6,1}, {"Pitch LFO depth",200,0,"cents",false,OSCAIRN_WIDGET_KNOB,7,1}, // one LFO: drawn joined
        {"Output bus",8,0,"",true,OSCAIRN_WIDGET_LIST,0},
        {"Pitch bend range",24,2,"semitones"}, {"Mod wheel",1,0,"",true,OSCAIRN_WIDGET_BUTTONS,0}
    }};
    constexpr auto defaults=[] {std::array<double,parameters.size()> v{};for(size_t i=0;i<v.size();++i)v[i]=parameters[i].initial;return v;}();
    constexpr std::array<const char*,3> oscillators{"Saw","Detuned saws","Square"};
    constexpr std::array<const char*,2> wheelTargets{"Vibrato","Off"};
    // Factory presets, inside the plugin: each sets these parameters (up to an ID of 0) over the defaults.
    struct FactoryPreset { const char* name; const char* key; std::array<oscairn_sound_parameter_t,6> values; };
    constexpr std::array<FactoryPreset,5> factoryPresets{{
        {"Warm","warm",{{{1,.15},{2,.7},{3,.5}}}},
        {"Bright","bright",{{{1,.9},{2,.7},{3,.5}}}},
        {"Pressure swell","pressure-swell",{{{1,.05},{2,.7},{3,1.0}}}},
        {"Detuned saws","detuned-saws",{{{1,.65},{2,.5},{3,.5},{4,1},{5,7}}}},
        {"Square vibrato","square-vibrato",{{{1,.8},{2,.4},{3,.5},{4,2},{6,18},{7,70}}}}
    }};
    struct Presets
    {
        static constexpr clap_preset_discovery_provider_descriptor_t descriptor{CLAP_VERSION_INIT,"org.oscairn.basic-synth.presets","Basic Synth presets","Oscairn"};
        static constexpr const char* pluginId="org.oscairn.basic-synth";
        static void declare(oscairn::presets::Declarations& d){d.inside("Factory");}
        static void list(uint32_t kind,const char*,oscairn::presets::Listing& l)
        {if(kind==CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)for(const auto& p:factoryPresets)if(!l.add(p.name,p.key))return;}
    };
    // The mod wheel's vibrato at full wheel when the sound's pitch LFO has no rate or depth set.
    constexpr double wheelRate = 5.0, wheelDepth = 50.0; // Hz, cents
    constexpr double pressureCutoff = 6000.0; // Hz the filter opens at full pressure and amount

    enum class Stage
    {
        Attack,
        Decay,
        Release
    };

    struct Voice : oscairn::contexts::VoiceBase
    {
        uint32_t outputBus = 0;
        Stage    stage    = Stage::Attack;
        double   phase    = 0.0, phase2 = 0.0, lfoPhase = 0.0;
        double   increment = 0.0;
        double   level    = 0.0; // envelope
        double   low1 = 0.0, low2 = 0.0;
    };

    struct Patch
    {
        std::array<double,parameters.size()> values=defaults,modulation{};
    };

    struct Synth : oscairn::contexts::Engine<Synth,Voice,Patch>
    {
        double                 sampleRate = 48000.0;
        double                 attackStep = 0.0, decayCoefficient = 0.0, releaseCoefficient = 0.0;
        static constexpr double sustain = 0.35;

        // Values are atomic: the main thread reads them (get_value) while audio runs.
        std::array<std::atomic<double>,parameters.size()> values{};
        static constexpr const char* stateVersion="oscairn-synth-2";
        static constexpr size_t maxStateBytes=16384;
        Synth(){for(size_t i=0;i<values.size();++i)values[i]=defaults[i];}

        bool accepts(const Patch&,const clap_event_note_t&){return true;}
        void start(Voice& v,Context& c){v.increment=440.0*std::pow(2.0,(v.key-69)/12.0)/sampleRate;v.outputBus=static_cast<uint32_t>(c.patch.values[7]);}
        void release(Voice& v){v.stage=Stage::Release;}
        static constexpr bool volumeExpressions=true; // render multiplies by amplitude()
        double loudness(const Voice& v) const{return v.level*v.velocity*amplitude(v,0);}
        void selectedGlobally(Context& next,const clap_output_events_t* out);
        void applyParam(const clap_event_header_t* header);
        void render(const clap_process_t* process,uint32_t from,uint32_t to);
        static uint32_t prepare(const clap_plugin_t* p,const char* name,const oscairn_sound_parameter_t* supplied,uint32_t count,const char* patch,const oscairn_asset_ref_t* base,char* message,uint32_t capacity);
        bool writePatch(std::ostream& out,const Patch& patch){for(double value:patch.values)out<<' '<<value;return true;}
        bool readPatch(std::istream& in,Patch& patch)
        {
            // The rest of the slot's line: every value.
            std::string line;std::getline(in,line);std::istringstream values(line);values.imbue(std::locale::classic());
            size_t i=0;
            for(double v;i<parameters.size() && values>>v;++i){if(!std::isfinite(v) || v<0 || v>parameters[i].maximum || (parameters[i].stepped && v!=std::round(v)))return false;patch.values[i]=v;}
            values>>std::ws;
            return i==parameters.size() && values.eof();
        }
        bool readPreset(uint32_t kind,const char*,const char* key,Patch& patch,std::string& error)
        {
            for(const auto& preset:factoryPresets)if(kind==CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && key && std::strcmp(key,preset.key)==0)
                {for(const auto& v:preset.values)if(v.id)patch.values[v.id-1]=v.value;return true;}
            error="no such preset";return false;
        }
        void restored(){for(size_t i=0;i<parameters.size();++i)values[i]=contexts[selected[0]].patch.values[i];}
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

    void Synth::applyParam(const clap_event_header_t* header)
    {
        auto& s=*this;
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
            auto& c=s.contexts[slot].patch;(modulation?c.modulation:c.values)[id-1]=value;
        }
    }
    void Synth::selectedGlobally(Context& next,const clap_output_events_t* output)
    {
        for(uint32_t i=0;i<parameters.size();++i){values[i]=next.patch.values[i];clap_event_param_value_t e{};e.header={sizeof(e),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};e.param_id=i+1;e.note_id=e.port_index=e.channel=e.key=-1;e.value=next.patch.values[i];if(output)output->try_push(output,&e.header);}
    }

    void Synth::render( const clap_process_t* process, uint32_t from, uint32_t to )
    {
        auto& s=*this;
        // Snapshot only contexts pinned by active voices. After a voice finishes,
        // the control thread may reclaim its slot; the rest of this span uses copies.
        std::array<std::array<double,parameters.size()>,Synth::contextCapacity> settings{};
        std::array<double,Synth::contextCapacity> detune{};
        std::array<bool,Synth::contextCapacity> copied{};
        std::array<float* const*,Synth::contextCapacity> ring{}; // the ring-out channels its main output goes to, or null
        for(const auto& v:s.voices)if(v.active && !copied[v.context])
        {
            copied[v.context]=true;ring[v.context]=s.retiring(v.context);const auto& c=s.contexts[v.context].patch;
            for(size_t p=0;p<parameters.size();++p)settings[v.context][p]=std::clamp(c.values[p]+c.modulation[p],0.0,parameters[p].maximum);
            detune[v.context]=settings[v.context][3]==1?std::pow(2.0,settings[v.context][4]/1200.0):1;
        }
        for( uint32_t i = from; i < to; ++i )
        {
            s.eventTime=i;
            std::array<std::array<double,9>,Synth::contextCapacity> mix{};
            std::array<double,Synth::contextCapacity> levels{};
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
                            s.finishVoice(v);
                            continue;
                        }
                        break;
                }

                const auto& patch=settings[v.context];
                s.followControls(v);
                // The LFO's own depth, plus the mod wheel's: as much again, or 50 cents when the LFO has none.
                const double depth=(patch[5]>0?patch[6]:0)+(patch[9]==0?v.modWheel*(patch[6]>0?patch[6]:wheelDepth):0);
                const double octaves=v.bend*patch[8]/12+(depth>0?std::sin(2*pi*v.lfoPhase)*depth/1200:0);
                double increment=v.increment;
                if(octaves!=0)
                    increment*=std::pow(2.0,octaves);
                v.lfoPhase+=(patch[5]>0?patch[5]:wheelRate)/s.sampleRate;v.lfoPhase-=std::floor(v.lfoPhase);
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

                mix[v.context][v.outputBus] += v.low2 * v.level * v.velocity * 0.35 * amplitude(v, i - from); // volume expression and choke fade
            }
            for(uint32_t bus=0;bus<std::min(9u,process->audio_outputs_count);++bus) {
                double sum=0;
                for(size_t c=0;c<mix.size();++c)if(mix[c][bus]!=0){
                    const double y=std::tanh(mix[c][bus]*levels[c]);
                    if(bus==0 && ring[c]){ring[c][0][i]+=static_cast<float>(y);ring[c][1][i]+=static_cast<float>(y);}else sum+=y;}
                auto& out=process->audio_outputs[bus];
                if(out.channel_count>=2 && out.data32 && out.data32[0] && out.data32[1])out.data32[0][i]=out.data32[1][i]=static_cast<float>(sum);
            }
        }
    }

    uint32_t audioPortsCount( const clap_plugin_t*, bool isInput )
    {
        return isInput ? 0 : 9;
    }

    bool audioPortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info )
    {
        if( isInput || index > 8 )
            return false;
        info->id            = index;
        info->flags         = index==0?CLAP_AUDIO_PORT_IS_MAIN:0;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        if(index==0)std::snprintf(info->name,sizeof(info->name),"Main");
        else std::snprintf(info->name,sizeof(info->name),"Sound bus %u",index);
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
        else if(id==10)std::snprintf(text,size,"%s",wheelTargets[static_cast<size_t>(std::round(value))]);
        else if(id<=3)std::snprintf(text,size,"%.0f%%",value*100);
        else std::snprintf(text,size,"%.1f %s",value,parameters[id-1].unit);
        return true;
    }

    bool paramsFromText( const clap_plugin_t*, clap_id id, const char* text, double* value )
    {
        if(id<1 || id>parameters.size())return false;
        if(id==4)for(size_t i=0;i<oscillators.size();++i)if(std::strcmp(text,oscillators[i])==0){*value=i;return true;}
        if(id==10)for(size_t i=0;i<wheelTargets.size();++i)if(std::strcmp(text,wheelTargets[i])==0){*value=i;return true;}
        char* end=nullptr;double parsed=std::strtod(text,&end);
        if(end==text || !std::isfinite(parsed))return false;
        while(*end==' ')++end;
        if(*end && std::strcmp(end,parameters[id-1].unit)!=0)return false;
        if(id<=3)parsed/=100;
        if(parsed<0 || parsed>parameters[id-1].maximum || (parameters[id-1].stepped && parsed!=std::round(parsed)))return false;
        *value=parsed;return true;
    }

    void paramsFlush( const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out )
    {
        self(p).applySelections(out);
        for( uint32_t i = 0, n = in->size( in ); i < n; ++i )
            self( p ).applyParam( in->get( in, i ) );
    }

    const clap_plugin_params_t      params     = { paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush };
    const oscairn_plugin_ui_t       ui         = { []( const clap_plugin_t*, clap_id id, oscairn_param_hint_t* hint ) {
        if( id < 1 || id > parameters.size() ) return false;
        *hint = { parameters[id - 1].widget, parameters[id - 1].priority, CLAP_INVALID_ID, parameters[id - 1].group, parameters[id - 1].role };
        return true;
    } };
    const clap_plugin_audio_ports_t audioPorts = { audioPortsCount, audioPortsGet };
    const clap_plugin_note_ports_t  notePorts  = { notePortsCount, notePortsGet };

    uint32_t Synth::prepare(const clap_plugin_t* p,const char* name,const oscairn_sound_parameter_t* supplied,uint32_t count,const char* text,const oscairn_asset_ref_t* base,char*,uint32_t)
    {
        if(text || base || !name || std::strlen(name)>63 || count>defaults.size() || (count && !supplied))return CLAP_INVALID_ID;
        Patch patch;std::array<bool,defaults.size()> seen{};
        for(uint32_t i=0;i<count;++i)
        {
            const auto& v=supplied[i];
            if(v.id<1 || v.id>parameters.size() || seen[v.id-1] || !std::isfinite(v.value))return CLAP_INVALID_ID;
            const auto& definition=parameters[v.id-1];
            if(v.value<0 || v.value>definition.maximum || (definition.stepped && v.value!=std::round(v.value)))return CLAP_INVALID_ID;
            seen[v.id-1]=true;patch.values[v.id-1]=v.value;
        }
        auto& s=self(p);auto* c=s.freeSlot();if(!c)return CLAP_INVALID_ID;
        c->patch=patch;return s.publish(*c,name);
    }

    const clap_plugin_state_t snapshot{Synth::saveSnapshot,Synth::loadSnapshot};
    const oscairn_plugin_sound_contexts_t soundContexts{Synth::capacity,Synth::get,Synth::prepare,Synth::select,Synth::discard,&snapshot,Synth::preparePreset};
    const oscairn_plugin_sound_snapshots_t soundSnapshots{Synth::prepareSnapshot,Synth::setRingOut,Synth::chokeRetiring};
    const oscairn_plugin_voice_budget_t voiceBudget{Synth::honorsVolume,Synth::setLimits,Synth::activeVoices,Synth::voiceNotes};

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
            s.setSampleRate( rate );
            s.attackStep         = 1.0 / ( 0.002 * rate );
            s.decayCoefficient   = std::exp( -1.0 / ( 0.25 * rate ) );
            s.releaseCoefficient = std::exp( -1.0 / ( 0.12 * rate ) );
            s.resetVoices();
            return true;
        };
        s->plugin.deactivate       = []( const clap_plugin_t* ) {};
        s->plugin.start_processing = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing  = []( const clap_plugin_t* ) {};
        s->plugin.reset            = []( const clap_plugin_t* p ) { self(p).resetVoices(); };
        s->plugin.process          = Synth::process;
        s->plugin.get_extension    = []( const clap_plugin_t*, const char* id ) -> const void* {
            if(std::strcmp(id,OSCAIRN_EXT_SOUND_CONTEXTS)==0)return &soundContexts;
            if(std::strcmp(id,OSCAIRN_EXT_SOUND_SNAPSHOTS)==0)return &soundSnapshots;
            if(std::strcmp(id,OSCAIRN_EXT_VOICE_BUDGET)==0)return &voiceBudget;
            if( std::strcmp( id, CLAP_EXT_AUDIO_PORTS ) == 0 )
                return &audioPorts;
            if( std::strcmp( id, CLAP_EXT_NOTE_PORTS ) == 0 )
                return &notePorts;
            if( std::strcmp( id, CLAP_EXT_PARAMS ) == 0 )
                return &params;
            if( std::strcmp( id, OSCAIRN_EXT_UI ) == 0 )
                return &ui;
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
    []( const char* id ) -> const void* {
        if( std::strcmp( id, CLAP_PRESET_DISCOVERY_FACTORY_ID ) == 0 || std::strcmp( id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT ) == 0 )
            return &oscairn::presets::Factory<Presets>::factory;
        return std::strcmp( id, CLAP_PLUGIN_FACTORY_ID ) == 0 ? &factory : nullptr;
    },
};

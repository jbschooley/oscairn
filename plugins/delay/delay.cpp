// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Delay: a tempo-synced stereo ping-pong delay (CLAP audio effect).
// The default delay time is a dotted eighth note at the transport tempo (375 ms when
// the host gives no tempo); parameters choose beats, feedback and wet level. Each repeat crosses to the other side. The output does not depend
// on the host's block size.
//
// When the tempo changes, the delay time glides to the new value (a 50 ms time constant,
// at most half a sample per sample), like a tape delay, rather than jumping, which would
// click.

#include <clap/clap.h>
#include <oscairn-clap/effect-contexts.h>
#include <array>
#include <charconv>
#include <sstream>
#include <locale>
#include <atomic>
#include <memory>
#include <cstdlib>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    struct Parameter { const char* name; double minimum,maximum,initial; };
    constexpr std::array<Parameter,3> parameters{{
        {"Delay beats",.0625,4,.75},{"Feedback",0,.95,.45},{"Wet",0,1,.3}
    }};
    struct Engine {
        double sampleRate,glide;
        uint32_t contexts=0;
        explicit Engine(double rate):sampleRate(rate),glide(1-std::exp(-1/(.05*rate))){}
    };
    constexpr uint32_t capacity=16;
    constexpr double maxDelaySeconds = 2.0;

    const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.delay",
        "Oscairn Delay",
        "Oscairn",
        "https://github.com/jbschooley/oscairn",
        "",
        "",
        "0.1.0",
        "A tempo-synced stereo ping-pong delay",
        features,
    };

    struct Delay
    {
        clap_plugin_t      plugin{};
        std::shared_ptr<Engine> engine;
        std::array<std::atomic<double>,3> values{.75,.45,.3};
        std::atomic<bool> tail{false};
        size_t nonzero=0;
        ~Delay(){if(engine)--engine->contexts;}
        std::vector<float> left, right; // circular buffers
        size_t             write = 0;
        double             delay = -1.0; // current delay in samples, gliding; -1 until the first block
    };

    Delay& self( const clap_plugin_t* p )
    {
        return *static_cast<Delay*>( p->plugin_data );
    }

    void apply(Delay& s,const clap_event_header_t* h) {
        if(!h || h->space_id!=CLAP_CORE_EVENT_SPACE_ID || h->type!=CLAP_EVENT_PARAM_VALUE || h->size<sizeof(clap_event_param_value_t))return;
        const auto& e=*reinterpret_cast<const clap_event_param_value_t*>(h);
        if(e.param_id<1 || e.param_id>parameters.size() || e.note_id!=-1 || e.port_index!=-1 || e.channel!=-1 || e.key!=-1 || !std::isfinite(e.value))return;
        const auto& p=parameters[e.param_id-1];s.values[e.param_id-1].store(std::clamp(e.value,p.minimum,p.maximum),std::memory_order_relaxed);
    }

    clap_process_status process( const clap_plugin_t* p, const clap_process_t* process )
    {
        auto& s = self( p );
        if( process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2 || s.left.empty() )
            return CLAP_PROCESS_ERROR;

        const bool    hasInput = process->audio_inputs_count > 0 && process->audio_inputs[0].channel_count >= 2;
        const float*  inL      = hasInput ? process->audio_inputs[0].data32[0] : nullptr;
        const float*  inR      = hasInput ? process->audio_inputs[0].data32[1] : nullptr;
        float*        outL     = process->audio_outputs[0].data32[0];
        float*        outR     = process->audio_outputs[0].data32[1];

        const auto* t=process->transport;
        const double tempo=t && (t->flags&CLAP_TRANSPORT_HAS_TEMPO) && std::isfinite(t->tempo) && t->tempo>0?t->tempo:120;
        const size_t size=s.left.size();
        uint32_t event=0;
        const uint32_t count=process->in_events?process->in_events->size(process->in_events):0;
        double beats=s.values[0].load(std::memory_order_relaxed),feedback=s.values[1].load(std::memory_order_relaxed),wet=s.values[2].load(std::memory_order_relaxed);
        auto targetFor=[&]{return std::round(std::clamp(beats*60/tempo*s.engine->sampleRate,1.0,static_cast<double>(size-2)));};
        double target=targetFor();
        if(s.delay<0)s.delay=target;
        for(uint32_t i=0;i<process->frames_count;++i)
        {
            while(event<count) {
                const auto* h=process->in_events->get(process->in_events,event);
                if(h && h->time>i)break;
                apply(s,h);++event;
                beats=s.values[0].load(std::memory_order_relaxed);feedback=s.values[1].load(std::memory_order_relaxed);wet=s.values[2].load(std::memory_order_relaxed);target=targetFor();
            }
            if( s.delay != target )
            {
                // At most half a sample per sample: the echoes bend in pitch by up to
                // half (an octave down to a fifth up) while gliding, never backwards.
                s.delay += std::clamp( ( target - s.delay ) * s.engine->glide, -0.5, 0.5 );
                if( std::abs( target - s.delay ) < 1e-3 )
                    s.delay = target;
            }
            // Read between samples while gliding; at a whole number of samples this is
            // exactly the stored sample.
            const double whole = std::floor( s.delay );
            const auto   frac  = (float)( s.delay - whole );
            const size_t read  = ( s.write + size - (size_t)whole ) % size; // `whole` samples ago
            const size_t older = ( read + size - 1 ) % size;
            const float  dryL  = inL ? inL[i] : 0.0f;
            const float  dryR  = inR ? inR[i] : 0.0f;
            const float  echoL = frac == 0.0f ? s.left[read] : s.left[read] + frac * ( s.left[older] - s.left[read] );
            const float  echoR = frac == 0.0f ? s.right[read] : s.right[read] + frac * ( s.right[older] - s.right[read] );

            // Ping-pong: the left line is fed from the input and the right echoes,
            // and the other way round, so each repeat crosses sides.
            auto store=[&](float& slot,double value) {
                s.nonzero-=slot!=0;
                // Bound tails and avoid denormals below -240 dBFS.
                slot=std::abs(value)<1e-12?0:static_cast<float>(value);
                s.nonzero+=slot!=0;
            };
            store(s.left[s.write],0.5*(dryL+dryR)+feedback*echoR);
            store(s.right[s.write],feedback*echoL);
            s.write          = ( s.write + 1 ) % size;

            outL[i] = (float)( dryL + wet * echoL );
            outR[i] = (float)( dryR + wet * echoR );
        }
        s.tail.store(s.nonzero!=0,std::memory_order_relaxed);
        return CLAP_PROCESS_CONTINUE;
    }

    uint32_t audioPortsCount( const clap_plugin_t*, bool )
    {
        return 1;
    }

    bool audioPortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info )
    {
        if( index != 0 )
            return false;
        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf( info->name, sizeof( info->name ), "%s", isInput ? "Input" : "Output" );
        return true;
    }

    const clap_plugin_audio_ports_t audioPorts = { audioPortsCount, audioPortsGet };

    void clear( Delay& s )
    {
        std::fill( s.left.begin(), s.left.end(), 0.0f );
        std::fill( s.right.begin(), s.right.end(), 0.0f );
        s.write = 0;
        s.delay = -1.0;
        s.nonzero=0;s.tail.store(false);
    }

    const clap_plugin_params_t params{
        [](const clap_plugin_t*)->uint32_t{return parameters.size();},
        [](const clap_plugin_t*,uint32_t i,clap_param_info_t* out)->bool {
            if(i>=parameters.size())return false;*out={};out->id=i+1;out->flags=CLAP_PARAM_IS_AUTOMATABLE;
            out->min_value=parameters[i].minimum;out->max_value=parameters[i].maximum;out->default_value=parameters[i].initial;
            std::snprintf(out->name,sizeof(out->name),"%s",parameters[i].name);return true;
        },
        [](const clap_plugin_t* p,clap_id id,double* value)->bool {if(id<1 || id>3)return false;*value=self(p).values[id-1].load();return true;},
        [](const clap_plugin_t*,clap_id id,double value,char* text,uint32_t n)->bool {if(id<1 || id>3 || !std::isfinite(value))return false;std::snprintf(text,n,"%.3f",value);return true;},
        [](const clap_plugin_t*,clap_id id,const char* text,double* value)->bool {if(id<1 || id>3)return false;char* end=nullptr;const double v=std::strtod(text,&end);if(end==text || *end || !std::isfinite(v) || v<parameters[id-1].minimum || v>parameters[id-1].maximum)return false;*value=v;return true;},
        [](const clap_plugin_t* p,const clap_input_events_t* in,const clap_output_events_t*){for(uint32_t i=0;in && i<in->size(in);++i)apply(self(p),in->get(in,i));}
    };
    // Text state is independent of byte order and locale; history is not a patch.
    constexpr char stateHeader[]="oscairn-delay-1 ";
    const clap_plugin_state_t state{
        [](const clap_plugin_t* p,const clap_ostream_t* stream)->bool {
            char data[128];std::memcpy(data,stateHeader,sizeof(stateHeader)-1);
            char* end=data+sizeof(stateHeader)-1;
            for(const auto& value:self(p).values) {
                const auto result=std::to_chars(end,data+sizeof(data)-1,value.load(),std::chars_format::general,17);
                if(result.ec!=std::errc{})return false;end=result.ptr;*end++=' ';
            }
            end[-1]='\n';const int n=static_cast<int>(end-data);
            for(int at=0;at<n;){const auto written=stream->write(stream,data+at,n-at);if(written<=0 || written>n-at)return false;at+=static_cast<int>(written);}return true;
        },
        [](const clap_plugin_t* p,const clap_istream_t* stream)->bool {
            char data[128]{};size_t n=0;
            for(;;){char byte;const auto got=stream->read(stream,&byte,1);if(got<0 || got>1)return false;if(!got)break;if(n==sizeof(data)-1 || byte==0)return false;data[n++]=byte;}
            if(n<sizeof(stateHeader) || std::memcmp(data,stateHeader,sizeof(stateHeader)-1))return false;
            std::array<double,3> v{};
            std::istringstream input(std::string(data+sizeof(stateHeader)-1,n-(sizeof(stateHeader)-1)));
            input.imbue(std::locale::classic());
            for(auto& value:v)if(!(input>>value))return false;
            input>>std::ws;if(!input.eof())return false;
            for(size_t i=0;i<3;++i)if(!std::isfinite(v[i]) || v[i]<parameters[i].minimum || v[i]>parameters[i].maximum)return false;
            for(size_t i=0;i<3;++i)self(p).values[i].store(v[i]);return true;
        }
    };
    const clap_plugin_t* create(std::shared_ptr<Engine> engine={});
    const oscairn_plugin_effect_contexts_t contexts{
        [](const clap_plugin_t* p,const clap_host_t*)->const clap_plugin_t* {
            auto engine=self(p).engine;if(!engine || engine->contexts>=capacity)return nullptr;
            try{return create(std::move(engine));}catch(...){return nullptr;}
        },
        [](const clap_plugin_t* p,oscairn_effect_resources_t* out)->bool {
            const auto& s=self(p);if(!out || !s.engine)return false;
            *out={sizeof(Engine),sizeof(Delay)+(s.left.capacity()+s.right.capacity())*sizeof(float),capacity,s.engine->contexts,s.tail.load()};return true;
        }
    };

    const clap_plugin_t* create(std::shared_ptr<Engine> engine)
    {
        auto* s                    = new Delay;
        s->engine=std::move(engine);if(s->engine)++s->engine->contexts;
        s->plugin.desc             = &descriptor;
        s->plugin.plugin_data      = s;
        s->plugin.init             = []( const clap_plugin_t* ) { return true; };
        s->plugin.destroy          = []( const clap_plugin_t* p ) { delete &self( p ); };
        s->plugin.activate         = []( const clap_plugin_t* p, double rate, uint32_t, uint32_t ) {
            auto& s      = self( p );
            if(!std::isfinite(rate) || rate<1 || rate>768000)return false;
            if(!s.engine || s.engine->sampleRate!=rate) {
                if(s.engine && s.engine->contexts>1)return false;
                auto engine=std::make_shared<Engine>(rate);engine->contexts=1;
                if(s.engine)--s.engine->contexts;s.engine=std::move(engine);
            }
            s.left.assign(static_cast<size_t>(maxDelaySeconds*rate)+1,0);
            s.right.assign(s.left.size(),0);clear(s);
            return true;
        };
        s->plugin.deactivate       = []( const clap_plugin_t* ) {};
        s->plugin.start_processing = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing  = []( const clap_plugin_t* ) {};
        s->plugin.reset            = []( const clap_plugin_t* p ) { clear( self( p ) ); };
        s->plugin.process          = process;
        s->plugin.get_extension    = []( const clap_plugin_t*, const char* id ) -> const void* {
            if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &audioPorts;
            if(!std::strcmp(id,CLAP_EXT_PARAMS))return &params;
            if(!std::strcmp(id,CLAP_EXT_STATE))return &state;
            if(!std::strcmp(id,OSCAIRN_EXT_EFFECT_CONTEXTS))return &contexts;
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

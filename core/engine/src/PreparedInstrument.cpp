// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PreparedInstrument.h"
#include "NoteRoute.h"
#include "../../clap-host/src/Internal.h"
#include <algorithm>
#include <cmath>
#include "../../plugin-host/src/StateFiles.h"
#include <sstream>
#include <iomanip>
#include <locale>

namespace oscairn {
namespace {
using kick::clap::Event;
using kick::clap::Parameter;
using kick::clap::Port;
constexpr size_t capacity=8, eventCapacity=kick::clap::Host::maxBlockEvents;

// Only Graph constructs this host: its input ledger supplies unique note IDs,
// including FIFO ownership for repeated MIDI keys across instance changes.
class PreparedInstrument final:public PluginHost {
    struct Instance {
        std::shared_ptr<PluginHost> host;
        NoteRoute notes;
        uint32_t id;
        std::string name;
        std::vector<float> audio;
        std::vector<float*> outputs;
        std::array<Event,eventCapacity> events{};
        uint32_t count=0;
        bool running=false;
        uint64_t lastSelection=0; // main thread; pins pending selections
        std::atomic<uint32_t> selected{0},owned{0};
        std::atomic<bool> rendered{false};
        Instance(std::shared_ptr<PluginHost> h,uint32_t n,std::string label,uint32_t frames):
            host(std::move(h)),notes(*host),id(n),name(std::move(label)) {
            size_t channels=0;for(const auto& p:host->outputPorts())channels+=p.channels;
            audio.resize(channels*frames);for(size_t c=0;c<channels;++c)outputs.push_back(audio.data()+c*frames);
        }
    };
    struct Plan {
        std::array<std::shared_ptr<Instance>,capacity> instances{};
        std::array<uint8_t,16> selected{};
        uint64_t revision=0;
    };
    Plan desired_;
    Plan* current_=nullptr;
    std::atomic<Plan*> pending_{nullptr};
    kick::clap::detail::Queue<Plan*,32> retired_;
    std::atomic<unsigned> retiredCount_{0};
    std::atomic<uint64_t> applied_{0};
    std::atomic<bool> panic_{false};
    std::atomic<uint64_t> drops_{0},errors_{0};
    std::array<Event,eventCapacity> output_{};
    uint32_t outputCount_=0,nextNote_=0,nextContext_=1;
    std::array<std::array<uint8_t,16>,16> pedal_{};
    double rate_;
    uint32_t frames_,notePorts_;
    ProcessingMode mode_;
    Lv2BufferLimits limits_;

    PluginHost& base() const noexcept {return *desired_.instances[0]->host;}
    PluginHost& selected() const noexcept {return *desired_.instances[desired_.selected[0]]->host;}
    void collect() {Plan* p;while(retired_.pop(p)){retiredCount_.fetch_sub(1);delete p;}}
    void publish(std::unique_ptr<Plan> p) {
        p->revision=desired_.revision+1;
        desired_=*p;
        delete pending_.exchange(p.release(),std::memory_order_acq_rel);
        collect();
    }
    size_t index(uint32_t id) const {
        for(size_t i=0;i<capacity;++i)if(desired_.instances[i] && desired_.instances[i]->id==id)return i;
        return capacity;
    }
    static bool samePorts(const std::vector<Port>& a,const std::vector<Port>& b) {
        return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const auto& x,const auto& y){return x.id==y.id && x.channels==y.channels && x.flags==y.flags;});
    }
    void checkLayout(const PluginHost& h) const {
        if(!samePorts(base().inputPorts(),h.inputPorts()) || !samePorts(base().outputPorts(),h.outputPorts()) ||
           base().noteInputPorts()!=h.noteInputPorts() || base().noteOutputPorts()!=h.noteOutputPorts())
            throw std::runtime_error("Prepared sound changed the plugin port layout");
        for(uint32_t p=0;p<h.noteInputPorts();++p)if(base().usesClapNotes(p)!=h.usesClapNotes(p))throw std::runtime_error("Prepared sound changed note dialects");
        const auto& a=base().parameters();const auto& b=h.parameters();
        if(a.size()!=b.size() || !std::equal(a.begin(),a.end(),b.begin(),[](const auto& x,const auto& y){return x.id==y.id && x.minimum==y.minimum && x.maximum==y.maximum && x.flags==y.flags;}))
            throw std::runtime_error("Prepared sound changed parameter metadata");
    }
    bool append(Instance& s,const Event& e) noexcept {
        if(s.count==s.events.size())return false;
        s.events[s.count++]=e;return true;
    }
    void ended(const NoteRoute::Note& n,uint32_t time) noexcept {
        auto e=n.address;e.header.type=CLAP_EVENT_NOTE_END;e.header.time=time;
        if(outputCount_<output_.size())output_[outputCount_++]=Event::from(e);
        else {++drops_;panic_.store(true);}
    }
    void silence() noexcept {
        for(auto& s:current_->instances)if(s){s->host->panic();s->notes.clear();s->count=0;s->owned.store(0);}
        for(auto& p:pedal_)p.fill(0);
        auto e=Event::note(false,-1,0,0,-1,-1);reinterpret_cast<clap_event_header_t*>(e.bytes.data())->type=CLAP_EVENT_NOTE_END;
        outputCount_=1;output_[0]=e;
    }
    bool route(Instance& s,const Event& e) noexcept {
        if(e.valid() && e.header()->space_id==CLAP_CORE_EVENT_SPACE_ID && e.header()->type==CLAP_EVENT_NOTE_CHOKE){
            const auto& choke=*reinterpret_cast<const clap_event_note_t*>(e.header());
            std::array<std::array<bool,16>,16> channels{};
            for(const auto& n:s.notes.notes)if(n.used && !s.notes.clap[n.address.port_index] && NoteRoute::matches(n,choke.note_id,choke.port_index,choke.channel,choke.key))
                channels[n.address.port_index][n.address.channel]=true;
            for(size_t p=0;p<16;++p)for(int c=0;c<16;++c)if(channels[p][c])
                {
                    if(!append(s,Event::midi(0xb0|c,120,0,e.header()->time,p)))return false;
                    for(auto& n:s.notes.notes)if(NoteRoute::matches(n,-1,p,c,-1)){ended(n,e.header()->time);n.used=false;}
                }
        }
        return s.notes.route(e,nextNote_,[&](const Event& out){return append(s,out);},[&](const auto& n,uint32_t t){ended(n,t);});
    }
    template<class Call> bool selectedCall(Call&& call) noexcept {
        std::array<bool,capacity> seen{};bool ok=true;
        for(auto i:desired_.selected)if(!seen[i]){seen[i]=true;ok=call(*desired_.instances[i]->host) && ok;}
        return ok;
    }
public:
    PreparedInstrument(std::shared_ptr<PluginHost> h,double rate,uint32_t frames,ProcessingMode mode,Lv2BufferLimits limits):
        rate_(rate),frames_(frames),notePorts_(h->noteInputPorts()),mode_(mode),limits_(limits) {
        desired_.instances[0]=std::make_shared<Instance>(std::move(h),0,"Default",frames);
        desired_.instances[0]->selected.store(0xffff);current_=new Plan(desired_);
    }
    ~PreparedInstrument() override {delete pending_.load();delete current_;collect();}
    const char* soundContextMode() const noexcept override {return "instances";}
    uint32_t soundContextCapacity() const noexcept override {return capacity;}
    std::vector<oscairn_sound_context_info_t> soundContexts() const override {
        std::vector<oscairn_sound_context_info_t> result;
        for(const auto& s:desired_.instances)if(s){
            oscairn_sound_context_info_t info{};info.id=s->id;std::copy(s->name.begin(),s->name.end(),info.name);
            info.selected_channels=s->selected.load();info.active_voices=s->owned.load();
            info.idle=!info.selected_channels && !info.active_voices && !s->rendered.load() && applied_.load()>=s->lastSelection;
            result.push_back(info);
        }
        return result;
    }
    uint32_t prepareSound(const std::string& name,const std::vector<oscairn_sound_parameter_t>& patch,const std::string& state,const std::string& preset,const std::string& sfz,const std::string& library,const std::string& directory) override {
        if(!sfz.empty() || !library.empty() || !directory.empty())throw std::invalid_argument("Hosted instruments do not accept SFZ patches");
        if(name.empty() || name.size()>63 || name.find('\0')!=std::string::npos || patch.size()>128 ||
           (!state.empty() && !preset.empty()))throw std::invalid_argument("Invalid prepared sound name, patch, or state/preset combination");
        size_t slot=0;while(slot<capacity && desired_.instances[slot])++slot;
        if(slot==capacity || nextContext_==CLAP_INVALID_ID)throw std::runtime_error("Prepared instrument capacity exhausted");
        for(size_t i=0;i<patch.size();++i){
            const auto& v=patch[i];const auto& parameters=base().parameters();
            auto p=std::find_if(parameters.begin(),parameters.end(),[&](const auto& p){return p.id==v.id;});
            if(p==parameters.end() || (p->flags&CLAP_PARAM_IS_READONLY) || !std::isfinite(v.value) || v.value<p->minimum || v.value>p->maximum ||
               std::any_of(patch.begin(),patch.begin()+i,[&](const auto& x){return x.id==v.id;}))throw std::invalid_argument("Invalid prepared sound parameter");
        }
        auto h=loadPlugin(base().descriptor().path,base().descriptor().id,rate_,frames_,mode_,limits_);
        if(!state.empty())h->loadState(state);
        if(!preset.empty())h->loadPreset(preset);
        checkLayout(*h);
        for(const auto& p:patch)if(!h->setParameter(p.id,p.value))throw std::runtime_error("Prepared parameter queue full");
        // A new instance's first service (main-thread callback, parameter flush) holds its gate;
        // run it before the audio thread can see the instance, or its first block is refused.
        h->service();
        auto next=std::make_unique<Plan>(desired_);const auto id=nextContext_;
        next->instances[slot]=std::make_shared<Instance>(std::move(h),id,name,frames_);
        publish(std::move(next));++nextContext_;return id;
    }
    bool selectSound(uint32_t id,int16_t channel) override {
        if(channel < -1 || channel>15)throw std::invalid_argument("Invalid sound channel");
        const auto i=index(id);if(i==capacity)return false;
        auto next=std::make_unique<Plan>(desired_);
        if(channel<0)next->selected.fill(i);else next->selected[channel]=i;
        next->instances[i]->lastSelection=desired_.revision+1;publish(std::move(next));return true;
    }
    bool discardSound(uint32_t id,bool cutTail) override {
        const auto i=index(id);if(!i || i==capacity)return false;
        const auto& s=desired_.instances[i];
        if(std::find(desired_.selected.begin(),desired_.selected.end(),i)!=desired_.selected.end() ||
           applied_.load()<s->lastSelection || s->selected.load() || s->owned.load() || (!cutTail && s->rendered.load()))return false;
        auto next=std::make_unique<Plan>(desired_);next->instances[i].reset();publish(std::move(next));return true;
    }
    void saveSnapshot(const std::string& directory) override {
        state_files::Bundle bundle(directory);std::ostringstream out;out.imbue(std::locale::classic());out<<"pool 1\n";
        for(size_t i=0;i<capacity;++i){const auto& instance=desired_.instances[i];out<<bool(instance);
            if(instance){out<<' '<<std::quoted(instance->name);instance->host->saveSnapshot((bundle.temporary.path/std::to_string(i)).string());}out<<'\n';}
        for(auto slot:desired_.selected)out<<unsigned(slot)<<' ';out<<'\n';
        const auto text=out.str();state_files::write(bundle.temporary.path/"snapshot",text.data(),text.size());bundle.publish();
    }
    void loadSnapshot(const std::string& directory) override {
        const auto data=state_files::read(std::filesystem::path(directory)/"snapshot",4096);
        std::istringstream in(std::string(data.begin(),data.end()));in.imbue(std::locale::classic());std::string kind;unsigned version;
        if(!(in>>kind>>version) || kind!="pool" || version!=1)throw std::runtime_error("Invalid instrument pool snapshot");
        auto next=std::make_unique<Plan>();
        for(size_t i=0;i<capacity;++i){unsigned used;std::string name;if(!(in>>used) || used>1)throw std::runtime_error("Invalid pool slot");if(!used)continue;
            if(!(in>>std::quoted(name)) || name.size()>63 || name.find('\0')!=std::string::npos)throw std::runtime_error("Invalid pool sound name");
            auto h=loadPlugin(base().descriptor().path,base().descriptor().id,rate_,frames_,mode_,limits_);
            h->loadSnapshot((std::filesystem::path(directory)/std::to_string(i)).string());checkLayout(*h);h->service();
            if(nextContext_==CLAP_INVALID_ID)throw std::runtime_error("Sound IDs exhausted");
            next->instances[i]=std::make_shared<Instance>(std::move(h),i?nextContext_++:0,std::move(name),frames_);
        }
        if(!next->instances[0])throw std::runtime_error("Missing default pool sound");
        for(auto& slot:next->selected){unsigned n;if(!(in>>n) || n>=capacity || !next->instances[n])throw std::runtime_error("Invalid pool selection");slot=uint8_t(n);}
        in>>std::ws;if(!in.eof())throw std::runtime_error("Trailing pool data");publish(std::move(next));
    }
    void service() override {collect();for(const auto& s:desired_.instances)if(s)s->host->service();}
    const kick::clap::Descriptor& descriptor() const noexcept override {return base().descriptor();}
    const std::vector<Parameter>& parameters() const noexcept override {return base().parameters();}
    const std::vector<Port>& inputPorts() const noexcept override {return base().inputPorts();}
    const std::vector<Port>& outputPorts() const noexcept override {return base().outputPorts();}
    std::vector<uint32_t> mainAudioPorts(bool input) const override {return base().mainAudioPorts(input);}
    uint32_t noteInputPorts() const noexcept override {return base().noteInputPorts();}
    uint32_t noteOutputPorts() const noexcept override {return 0;}
    bool usesClapNotes(uint32_t port) const noexcept override {return port<noteInputPorts();}
    void panic() noexcept override {panic_.store(true);}
    std::string failure() const override {for(const auto& s:desired_.instances)if(s){auto f=s->host->failure();if(!f.empty())return f;}return {};}
    double parameterValue(clap_id id) const noexcept override {return selected().parameterValue(id);}
    std::string parameterText(clap_id id,double value) const override {return selected().parameterText(id,value);}
    bool setParameter(clap_id id,double value) noexcept override {return selectedCall([&](auto& h){return h.setParameter(id,value);});}
    bool parameterGesture(clap_id id,bool begin) noexcept override {return selectedCall([&](auto& h){return h.parameterGesture(id,begin);});}
    bool supportsState() const noexcept override {return selected().supportsState();}
    void saveState(const std::string& path) override {selected().saveState(path);}
    void loadState(const std::string& path) override {selectSound(prepareSound("Restored",{},path,{},{},{},{}),-1);}
    std::vector<PresetInfo> presets() override {return selected().presets();}
    void loadPreset(const std::string& uri) override {selectSound(prepareSound("Preset",{},{},uri,{},{},{}),-1);}
    std::vector<PropertyInfo> properties() const override {return selected().properties();}
    bool setProperty(const std::string& uri,const PropertyValue& value) override {return selected().setProperty(uri,value);}
    bool requestProperty(const std::string& uri) override {return selected().requestProperty(uri);}
    std::optional<PropertyValue> propertyValue(const std::string& uri) const override {return selected().propertyValue(uri);}
    bool popOutput(Event& event) noexcept override {
        bool found=false;
        for(const auto& s:desired_.instances)if(s){
            if(s->host.get()==&selected()) {if(!found)found=s->host->popOutput(event);}
            else {Event ignored;while(s->host->popOutput(ignored)){}}
        }
        return found;
    }
    uint64_t outputEventDrops() const noexcept override {uint64_t n=drops_.load();for(const auto& s:desired_.instances)if(s)n+=s->host->outputEventDrops();return n;}
    const Event* outputEvents(uint32_t& n) const noexcept override {n=outputCount_;return output_.data();}
    kick::clap::Statistics statistics() const noexcept override {
        kick::clap::Statistics result{};
        for(const auto& s:desired_.instances)if(s){const auto v=s->host->statistics();
            result.blocks=std::max(result.blocks,v.blocks);result.droppedEvents+=v.droppedEvents;result.processErrors+=v.processErrors;
            result.lastMicroseconds+=v.lastMicroseconds;result.maximumMicroseconds+=v.maximumMicroseconds;
            result.deadlineMisses+=v.deadlineMisses;result.controlSilences+=v.controlSilences;result.idleBlocks+=v.idleBlocks;result.restarts+=v.restarts;}
        result.droppedEvents+=drops_.load();result.processErrors+=errors_.load();return result;
    }
    bool process(const float* const*,uint32_t,float* const* outputs,uint32_t no,uint32_t frames,const Event* events,uint32_t count,const clap_event_transport_t* transport) noexcept override {
        outputCount_=0;
        for(uint32_t c=0;c<no;++c)if(outputs && outputs[c])std::fill_n(outputs[c],frames,0.f);
        if(frames>frames_ || (count && !events) || count>eventCapacity){++errors_;panic_.store(true);return false;}
        if(retiredCount_.load()<32)if(auto* next=pending_.exchange(nullptr,std::memory_order_acq_rel)){
            retiredCount_.fetch_add(1);retired_.push(current_);current_=next;
        }
        for(size_t i=0;i<capacity;++i)if(auto& s=current_->instances[i]){
            uint32_t mask=0;for(size_t c=0;c<16;++c)if(current_->selected[c]==i)mask|=1u<<c;
            s->selected.store(mask);s->count=0;
            if(mask && !s->running){
                s->running=true;
                for(uint32_t p=0;p<s->notes.ports;++p)for(int c=0;c<16;++c)if(pedal_[p][c])route(*s,Event::midi(0xb0|c,64,pedal_[p][c],0,p));
            }
        }
        if(panic_.exchange(false))silence();
        bool ok=true;
        for(uint32_t e=0;e<count && ok;++e){
            const auto& event=events[e];const auto* h=event.header();
            if(!event.valid() || h->time>=frames){++drops_;ok=false;break;}
            if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID){++drops_;continue;}
            if(h->type==CLAP_EVENT_NOTE_ON){
                const auto& n=*reinterpret_cast<const clap_event_note_t*>(h);
                if(n.channel<0 || n.port_index<0 || static_cast<uint32_t>(n.port_index)>=notePorts_){++drops_;ok=false;break;}
                auto& s=*current_->instances[current_->selected[n.channel]];ok=route(s,event);continue;
            }
            if(h->type==CLAP_EVENT_MIDI){
                const auto& m=*reinterpret_cast<const clap_event_midi_t*>(h);
                if(m.port_index<16 && (m.data[0]&0xf0)==0xb0 && m.data[1]==64)pedal_[m.port_index][m.data[0]&15]=m.data[2];
            }
            int channel=-1;bool globalParameter=false;
            if(h->type==CLAP_EVENT_PARAM_VALUE || h->type==CLAP_EVENT_PARAM_MOD){
                auto address=[&](const auto& p){globalParameter=p.note_id<0 && p.key<0;channel=p.channel;};
                if(h->type==CLAP_EVENT_PARAM_VALUE)address(*reinterpret_cast<const clap_event_param_value_t*>(h));
                else address(*reinterpret_cast<const clap_event_param_mod_t*>(h));
            }
            globalParameter|=h->type==CLAP_EVENT_PARAM_GESTURE_BEGIN || h->type==CLAP_EVENT_PARAM_GESTURE_END;
            for(size_t i=0;i<capacity;++i)if(auto& s=current_->instances[i];s && s->running){
                if(globalParameter && (channel<0?!s->selected.load():current_->selected[channel]!=i))continue;
                ok=route(*s,event) && ok;
            }
        }
        if(!ok){++drops_;silence();}
        for(auto& s:current_->instances)if(s && s->running){
            s->rendered.store(true);
            if(!s->host->process(nullptr,0,s->outputs.data(),s->outputs.size(),frames,s->events.data(),s->count,transport)){
                // This instance resets and ends its own notes; a failure reported upward would make
                // the graph reset the whole node, cutting the other sounds' notes too.
                s->host->panic();for(const auto& n:s->notes.notes)if(n.used)ended(n,0);s->notes.clear();
            }
            for(size_t c=0;c<std::min<size_t>(no,s->outputs.size());++c)if(outputs && outputs[c])
                for(uint32_t f=0;f<frames;++f)outputs[c][f]+=s->outputs[c][f];
            uint32_t n=0;const auto* emitted=s->host->outputEvents(n);
            for(uint32_t i=0;i<n;++i)if(emitted[i].valid() && emitted[i].header()->space_id==CLAP_CORE_EVENT_SPACE_ID && emitted[i].header()->type==CLAP_EVENT_NOTE_END){
                const auto& end=*reinterpret_cast<const clap_event_note_t*>(emitted[i].header());
                for(const auto& note:s->notes.notes)if(note.used && (end.note_id<0 || end.note_id==note.id) &&
                    (end.port_index<0 || end.port_index==note.address.port_index) && (end.channel<0 || end.channel==note.address.channel) && (end.key<0 || end.key==note.address.key))ended(note,end.header.time);
                s->notes.ended(emitted[i]);
            }
            s->owned.store(std::count_if(s->notes.notes.begin(),s->notes.notes.end(),[](const auto& n){return n.used;}));
        }
        applied_.store(current_->revision);return ok;
    }
};
}
std::shared_ptr<PluginHost> preparedInstrument(std::shared_ptr<PluginHost> h,double rate,uint32_t frames,ProcessingMode mode,Lv2BufferLimits limits) {
    // Effects and MIDI-output instruments need separate transition/routing policies.
    if(h->soundContextCapacity() || !h->inputPorts().empty() || h->outputPorts().empty() ||
       !h->noteInputPorts() || h->noteInputPorts()>16 || h->noteOutputPorts())return h;
    return std::make_shared<PreparedInstrument>(std::move(h),rate,frames,mode,limits);
}
}

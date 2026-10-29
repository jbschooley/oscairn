// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/plugin_host.h"
#include "Urids.h"
#include <lilv/lilv.h>
#include <lv2/atom/forge.h>
#include <lv2/patch/patch.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <array>
#include <cstring>
#include <filesystem>

namespace oscairn::lv2
{
class Patch
{
public:
    static constexpr uint32_t packetBytes=4096,capacity=32;
    struct Packet {uint32_t size=0;alignas(8) std::array<uint8_t,packetBytes> bytes{};};
private:
    struct Queue
    {
        std::array<Packet,capacity> slots;
        std::atomic<uint32_t> head{0},tail{0};
        bool push(const void* data,uint32_t size)
        {
            const auto h=head.load(std::memory_order_relaxed);
            if(size>packetBytes || h-tail.load(std::memory_order_acquire)==capacity)return false;
            auto& p=slots[h%capacity];p.size=size;std::memcpy(p.bytes.data(),data,size);head.store(h+1,std::memory_order_release);return true;
        }
        bool pop(Packet& p)
        {
            const auto t=tail.load(std::memory_order_relaxed);
            if(t==head.load(std::memory_order_acquire))return false;
            const auto& s=slots[t%capacity];p.size=s.size;std::memcpy(p.bytes.data(),s.bytes.data(),s.size);tail.store(t+1,std::memory_order_release);return true;
        }
        uint32_t available()const{return head.load(std::memory_order_acquire)-tail.load(std::memory_order_relaxed);}
    };
    struct Entry {PropertyInfo info;LV2_URID id;std::optional<PropertyValue> value;};
    std::vector<Entry> entries_;
    Queue input_,output_;
    LV2_Atom_Forge forge_{};
    LV2_URID set_,get_,put_,property_,value_,body_,object_,blank_,urid_;
    std::array<LV2_URID,7> types_;
    Entry* find(const std::string& uri){for(auto& e:entries_)if(e.info.uri==uri)return &e;return nullptr;}
    static bool fields(const LV2_Atom* atom,const auto& visit)
    {
        if(atom->size<sizeof(LV2_Atom_Object_Body))return false;
        const auto* p=reinterpret_cast<const uint8_t*>(atom+1)+sizeof(LV2_Atom_Object_Body);
        size_t remaining=atom->size-sizeof(LV2_Atom_Object_Body);
        while(remaining)
        {
            if(remaining<sizeof(LV2_Atom_Property_Body))return false;
            const auto* field=reinterpret_cast<const LV2_Atom_Property_Body*>(p);
            const uint64_t size=sizeof(LV2_Atom_Property_Body)+((uint64_t(field->value.size)+7)&~uint64_t(7));
            if(size>remaining)return false;
            visit(field->key,&field->value);p+=size;remaining-=size;
        }
        return true;
    }
    bool update(LV2_URID id,const LV2_Atom* value)
    {
        auto e=std::find_if(entries_.begin(),entries_.end(),[&](const auto& e){return e.id==id;});
        if(e==entries_.end())return true; // Unadvertised property, not a generic UI channel.
        const auto* data=value+1;
        if(value->type==types_[0] && value->size==4){int32_t v;std::memcpy(&v,data,4);e->value=v!=0;}
        else if(value->type==types_[1] && value->size==4){int32_t v;std::memcpy(&v,data,4);e->value=v;}
        else if(value->type==types_[2] && value->size==8){int64_t v;std::memcpy(&v,data,8);e->value=v;}
        else if(value->type==types_[3] && value->size==4){float v;std::memcpy(&v,data,4);if(!std::isfinite(v))return false;e->value=v;}
        else if(value->type==types_[4] && value->size==8){double v;std::memcpy(&v,data,8);if(!std::isfinite(v))return false;e->value=v;}
        else if((value->type==types_[5] || value->type==types_[6]) && value->size && static_cast<const char*>(static_cast<const void*>(data))[value->size-1]==0)
        {
            const auto* text=static_cast<const char*>(static_cast<const void*>(data));
            // Some production plugins include an extra terminator in forge's
            // length argument. Accept trailing zero padding, never hidden text.
            const auto length=std::strlen(text);
            for(size_t i=length+1;i<value->size;++i)if(text[i])return false;
            if(value->type==types_[6])e->value=FilePath{text};else e->value=std::string(text);
        }
        else return false;
        return true;
    }
public:
    Patch(LilvWorld* world,const LilvPlugin* plugin,Urids& urids,LV2_URID_Map* map)
    {
        lv2_atom_forge_init(&forge_,map);
        set_=urids(LV2_PATCH__Set);get_=urids(LV2_PATCH__Get);put_=urids(LV2_PATCH__Put);
        property_=urids(LV2_PATCH__property);value_=urids(LV2_PATCH__value);body_=urids(LV2_PATCH__body);
        object_=urids(LV2_ATOM__Object);blank_=urids(LV2_ATOM__Blank);urid_=urids(LV2_ATOM__URID);
        const char* types[]{LV2_ATOM__Bool,LV2_ATOM__Int,LV2_ATOM__Long,LV2_ATOM__Float,LV2_ATOM__Double,LV2_ATOM__String,LV2_ATOM__Path};
        for(size_t i=0;i<types_.size();++i)types_[i]=urids(types[i]);
        for(bool writable:{false,true})
        {
            auto* key=lilv_new_uri(world,writable?LV2_PATCH__writable:LV2_PATCH__readable);
            auto* nodes=lilv_plugin_get_value(plugin,key);lilv_node_free(key);
            LILV_FOREACH(nodes,i,nodes)
            {
                const auto* node=lilv_nodes_get(nodes,i);if(!lilv_node_is_uri(node))continue;
                const std::string uri=lilv_node_as_uri(node);auto* e=find(uri);
                if(!e)
                {
                    if(entries_.size()==256){lilv_nodes_free(nodes);throw std::runtime_error("LV2 property limit exceeded");}
                    entries_.push_back({{uri,uri},urids(uri.c_str()),{}});e=&entries_.back();
                    auto* label=lilv_new_uri(world,"http://www.w3.org/2000/01/rdf-schema#label");
                    auto* name=lilv_world_get(world,node,label,nullptr);if(name)e->info.name=lilv_node_as_string(name);lilv_node_free(name);lilv_node_free(label);
                    auto* range=lilv_new_uri(world,"http://www.w3.org/2000/01/rdf-schema#range");
                    auto* type=lilv_world_get(world,node,range,nullptr);
                    if(type)for(size_t t=0;t<types_.size();++t)if(!std::strcmp(lilv_node_as_string(type),types[t]))e->info.type=static_cast<PropertyType>(t+1);
                    lilv_node_free(type);lilv_node_free(range);
                }
                if(writable)e->info.writable=true;else e->info.readable=true;
            }
            lilv_nodes_free(nodes);
        }
    }
    std::vector<PropertyInfo> properties()const {std::vector<PropertyInfo> r;for(const auto& e:entries_)r.push_back(e.info);return r;}
    std::optional<PropertyValue> value(const std::string& uri)const{for(const auto& e:entries_)if(e.info.uri==uri)return e.value;return {};}
    bool request(const std::string& uri,const PropertyValue* value,uint32_t portBytes)
    {
        auto* e=find(uri);if(!e || (value && !e->info.writable))return false;
        if(value && e->info.type!=PropertyType::Unknown && static_cast<size_t>(e->info.type)!=value->index()+1)return false;
        Packet packet;auto forge=forge_;lv2_atom_forge_set_buffer(&forge,packet.bytes.data(),packet.bytes.size());
        LV2_Atom_Forge_Frame frame;lv2_atom_forge_object(&forge,&frame,0,value?set_:get_);
        lv2_atom_forge_key(&forge,property_);lv2_atom_forge_urid(&forge,e->id);
        if(value)
        {
            lv2_atom_forge_key(&forge,value_);
            bool valid=true;
            std::visit([&](const auto& v)
            {
                using T=std::decay_t<decltype(v)>;
                if constexpr(std::is_same_v<T,bool>)lv2_atom_forge_bool(&forge,v);
                else if constexpr(std::is_same_v<T,int32_t>)lv2_atom_forge_int(&forge,v);
                else if constexpr(std::is_same_v<T,int64_t>)lv2_atom_forge_long(&forge,v);
                else if constexpr(std::is_same_v<T,float>){valid=std::isfinite(v);lv2_atom_forge_float(&forge,v);}
                else if constexpr(std::is_same_v<T,double>){valid=std::isfinite(v);lv2_atom_forge_double(&forge,v);}
                else
                {
                    std::string text;if constexpr(std::is_same_v<T,FilePath>)text=v.value;else text=v;
                    valid=text.size()<=2048 && text.find('\0')==std::string::npos;
                    if constexpr(std::is_same_v<T,FilePath>)
                    {
                        std::error_code error;valid=valid && std::filesystem::is_regular_file(text,error) && !error;
                        if(valid){text=std::filesystem::canonical(text,error).string();valid=!error && text.size()<=2048;}
                        if(valid)lv2_atom_forge_path(&forge,text.c_str(),text.size());
                    }
                    else if(valid)lv2_atom_forge_string(&forge,text.c_str(),text.size());
                }
            },*value);
            if(!valid)return false;
        }
        lv2_atom_forge_pop(&forge,&frame);
        const auto* atom=reinterpret_cast<const LV2_Atom*>(packet.bytes.data());
        packet.size=sizeof(LV2_Atom)+atom->size;
        if(packet.size>packetBytes || packet.size+sizeof(LV2_Atom_Sequence)+sizeof(int64_t)>portBytes)return false;
        return input_.push(packet.bytes.data(),packet.size);
    }
    uint32_t pending()const{return input_.available();}
    bool pop(Packet& p){return input_.pop(p);}
    void clear(){Packet p;while(input_.pop(p)){}while(output_.pop(p)){}for(auto& e:entries_)e.value.reset();}
    bool collect(const LV2_Atom* atom){return output_.push(atom,sizeof(LV2_Atom)+atom->size);}
    uint32_t service()
    {
        uint32_t errors=0;Packet p;const auto count=output_.available();
        for(uint32_t i=0;i<count && output_.pop(p);++i)
        {
            const auto* atom=reinterpret_cast<const LV2_Atom*>(p.bytes.data());
            if(atom->size<sizeof(LV2_Atom_Object_Body)){++errors;continue;}
            const auto* object=reinterpret_cast<const LV2_Atom_Object*>(atom);
            const LV2_Atom *property=nullptr,*value=nullptr,*body=nullptr;
            if(!fields(atom,[&](uint32_t key,const LV2_Atom* v){if(key==property_)property=v;else if(key==value_)value=v;else if(key==body_)body=v;})){++errors;continue;}
            if(object->body.otype==set_)
            {
                if(!property || property->type!=urid_ || property->size!=4 || !value){++errors;continue;}
                LV2_URID id;std::memcpy(&id,property+1,4);if(!update(id,value))++errors;
            }
            else if(object->body.otype==put_ && body && (body->type==object_ || body->type==blank_))
            {if(!fields(body,[&](uint32_t key,const LV2_Atom* v){if(!update(key,v))++errors;}))++errors;}
        }
        return errors;
    }
};
}

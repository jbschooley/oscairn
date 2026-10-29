// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/plugin_host.h"
#include "sheredom/json.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>

namespace oscairn::property_wire {
inline const char* typeName(PropertyType type) {
    constexpr const char* names[]{"unknown","bool","int","long","float","double","string","path"};
    return names[static_cast<size_t>(type)];
}
inline PropertyType type(const std::string& name) {
    for(size_t i=0;i<8;++i)if(name==typeName(static_cast<PropertyType>(i)))return static_cast<PropertyType>(i);
    throw std::invalid_argument("Unknown property type");
}
template<class Quote,class Number> std::string encode(const PropertyValue& value,Quote quote,Number number) {
    const auto payload=std::visit([&](const auto& v)->std::string {
        using T=std::decay_t<decltype(v)>;
        if constexpr(std::is_same_v<T,bool>)return v?"true":"false";
        else if constexpr(std::is_same_v<T,int32_t>)return std::to_string(v);
        else if constexpr(std::is_same_v<T,int64_t>)return quote(std::to_string(v)); // exact through JavaScript clients
        else if constexpr(std::is_floating_point_v<T>)return number(v);
        else if constexpr(std::is_same_v<T,FilePath>)return quote(v.value);
        else return quote(v);
    },value);
    return "{\"type\":"+quote(typeName(static_cast<PropertyType>(value.index()+1)))+",\"value\":"+payload+'}';
}
inline PropertyValue decode(const json_value_s* value) {
    auto bad=[]()->void{throw std::invalid_argument("Invalid typed property value");};
    const json_value_s *kind=nullptr,*payload=nullptr;
    if(!value || value->type!=json_type_object)bad();
    for(auto* e=static_cast<const json_object_s*>(value->payload)->start;e;e=e->next){
        const std::string_view key(e->name->string,e->name->string_size);
        if(key=="type" && !kind)kind=e->value;else if(key=="value" && !payload)payload=e->value;else bad();
    }
    auto string=[&](const json_value_s* v){if(!v || v->type!=json_type_string)bad();auto* s=static_cast<const json_string_s*>(v->payload);return std::string(s->string,s->string_size);};
    if(!payload)bad();
    const auto t=type(string(kind));
    if(t==PropertyType::Bool){if(payload->type==json_type_true)return true;if(payload->type==json_type_false)return false;bad();}
    if(t==PropertyType::String || t==PropertyType::Path){auto s=string(payload);if(s.size()>4096 || s.find('\0')!=std::string::npos)bad();if(t==PropertyType::Path)return FilePath{s};return s;}
    if(t==PropertyType::Long){const auto s=string(payload);int64_t n;auto r=std::from_chars(s.data(),s.data()+s.size(),n);if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size())bad();return n;}
    if(payload->type!=json_type_number)bad();
    const auto* n=static_cast<const json_number_s*>(payload->payload);
    const std::string s(n->number,n->number_size);
    if(t==PropertyType::Int){int32_t v;auto r=std::from_chars(s.data(),s.data()+s.size(),v);if(r.ec!=std::errc{} || r.ptr!=s.data()+s.size())bad();return v;}
    double v=0;std::istringstream input(s);input.imbue(std::locale::classic());input>>v;
    if(input.fail() || !input.eof() || !std::isfinite(v))bad();
    if(t==PropertyType::Double)return v;
    if(t==PropertyType::Float){if(std::abs(v)>std::numeric_limits<float>::max())bad();return static_cast<float>(v);}
    bad();return false;
}
}

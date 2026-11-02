// SPDX-License-Identifier: AGPL-3.0-or-later
// Package signatures on this device (RFC 0006): the trust store, trust.json in the data folder.
#include "oscairn/session.h"
#include "oscairn/library.h"
#include "oscairn/signing.h"
#include "JsonInput.h"
#include <algorithm>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>

namespace oscairn {
namespace {
namespace fs=std::filesystem;
using V=const json_value_s*;
[[noreturn]] void bad(const std::string& why){throw std::runtime_error("Trust: "+why);}
const std::set<std::string> scopes{"plugins","apps","content"};
fs::path trustFile(){return dataHome()/"trust.json";}
std::string str(V v){if(!v || v->type!=json_type_string)bad("trust.json is damaged");const auto* s=static_cast<const json_string_s*>(v->payload);return {s->string,s->string_size};}

std::vector<Control::TrustedKey> readTrust(){
    std::error_code e;
    if(!fs::exists(trustFile(),e))return {};
    if(fs::file_size(trustFile())>4*1024*1024)bad("trust.json is too large");
    std::ifstream in(trustFile(),std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>());
    if(!detail::jsonPreflight(bytes,8).empty())bad("trust.json is damaged");
    const std::unique_ptr<json_value_s,decltype(&std::free)> doc{json_parse(bytes.data(),bytes.size()),&std::free};
    if(!doc || str(detail::member(doc.get(),"format"))!="oscairn.trust")bad("trust.json is damaged");
    const V keys=detail::member(doc.get(),"keys");
    if(!keys || keys->type!=json_type_array)bad("trust.json is damaged");
    std::vector<Control::TrustedKey> out;
    for(auto* k=static_cast<const json_array_s*>(keys->payload)->start;k;k=k->next){
        Control::TrustedKey t;t.key=str(detail::member(k->value,"key"));t.name=str(detail::member(k->value,"name"));t.id=signing::keyId(t.key);
        if(t.id.empty())bad("trust.json is damaged");
        if(V s=detail::member(k->value,"scope");s && s->type==json_type_array)for(auto* x=static_cast<const json_array_s*>(s->payload)->start;x;x=x->next)t.scope.push_back(str(x->value));
        const V revoked=detail::member(k->value,"revoked");t.revoked=revoked && revoked->type==json_type_true;
        out.push_back(std::move(t));
    }
    return out;
}
void writeTrust(const std::vector<Control::TrustedKey>& keys){
    std::string out="{\"format\":\"oscairn.trust\",\"version\":1,\"keys\":[";
    for(size_t i=0;i<keys.size();++i){
        std::string scope;for(const auto& s:keys[i].scope)scope+=(scope.empty()?"":",")+detail::quote(s);
        out+=(i?",":"")+std::string("\n  {\"key\":")+detail::quote(keys[i].key)+",\"name\":"+detail::quote(keys[i].name)+",\"scope\":["+scope+"],\"revoked\":"+(keys[i].revoked?"true":"false")+'}';
    }
    out+="\n]}\n";
    fs::create_directories(dataHome());
    const auto partial=dataHome()/".trust.json.partial";
    {std::ofstream o(partial,std::ios::binary|std::ios::trunc);o<<out;if(!o)bad("cannot write trust.json");}
    fs::rename(partial,trustFile());
}
}

std::vector<Control::TrustedKey> Session::trustedKeys(){return readTrust();}

void Session::setTrustedKey(const TrustedKey& key){
    TrustedKey t=key;t.id=signing::keyId(t.key);
    if(t.id.empty())throw std::invalid_argument("not a public key: 64 lowercase hex digits");
    if(t.name.empty() || t.name.size()>256)throw std::invalid_argument("a trusted key needs a name of at most 256 bytes");
    for(const auto& s:t.scope)if(!scopes.count(s))throw std::invalid_argument("unknown trust scope "+s+"; plugins, apps or content");
    std::sort(t.scope.begin(),t.scope.end());t.scope.erase(std::unique(t.scope.begin(),t.scope.end()),t.scope.end());
    auto keys=readTrust();
    const auto at=std::find_if(keys.begin(),keys.end(),[&](const auto& k){return k.key==t.key;});
    if(at!=keys.end())*at=t;else keys.push_back(t);
    writeTrust(keys);
}

void Session::removeTrustedKey(const std::string& id){
    auto keys=readTrust();
    const auto gone=std::remove_if(keys.begin(),keys.end(),[&](const auto& k){return k.id==id;});
    if(gone==keys.end())bad("no trusted key "+id);
    keys.erase(gone,keys.end());
    writeTrust(keys);
}
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/discovery.h"
#include <cstdlib>
#ifdef OSCAIRN_LV2
#include <lilv/lilv.h>
#include <memory>
#endif
namespace oscairn {
namespace fs=std::filesystem;
std::vector<fs::path> pluginSearchPaths() {
    auto result=kick::clap::defaultSearchPaths();
    if(const auto* home=std::getenv("HOME")) {
        result.emplace_back(fs::path(home)/".lv2");
#ifdef __APPLE__
        result.emplace_back(fs::path(home)/"Library/Audio/Plug-Ins/LV2");
#endif
    }
#ifdef __APPLE__
    result.emplace_back("/Library/Audio/Plug-Ins/LV2");
#else
    result.emplace_back("/usr/lib/lv2");result.emplace_back("/usr/local/lib/lv2");
#endif
    if(const auto* env=std::getenv("LV2_PATH")) {
        const std::string paths(env);size_t begin=0;
        while(begin<paths.size()){auto end=paths.find(':',begin);if(end==std::string::npos)end=paths.size();if(end>begin)result.emplace_back(paths.substr(begin,end-begin));begin=end+1;}
    }
    return result;
}
kick::clap::ScanResult scanPlugins(const std::vector<fs::path>& roots,const fs::path& helper,const std::atomic<bool>* cancelled) {
    auto result=kick::clap::scan(roots,helper,5000,cancelled);
    auto lv2=kick::clap::scan(roots,helper,5000,cancelled,".lv2");
    result.plugins.insert(result.plugins.end(),lv2.plugins.begin(),lv2.plugins.end());
    result.failures.insert(result.failures.end(),lv2.failures.begin(),lv2.failures.end());return result;
}
kick::clap::ScanResult inspectPlugin(const fs::path& source) {
    if(source.extension()!=".lv2")return kick::clap::inspect(source);
    kick::clap::ScanResult result;
    try {
#ifdef OSCAIRN_LV2
        std::unique_ptr<LilvWorld,decltype(&lilv_world_free)> world(lilv_world_new(),lilv_world_free);
        if(!world)throw std::runtime_error("Cannot create LV2 discovery world");
        auto node=[&](LilvNode* n){return std::unique_ptr<LilvNode,decltype(&lilv_node_free)>(n,lilv_node_free);};
        const auto path=fs::canonical(source).string();auto uri=node(lilv_new_file_uri(world.get(),nullptr,(path+"/").c_str()));
        lilv_world_load_bundle(world.get(),uri.get());const auto* plugins=lilv_world_get_all_plugins(world.get());
        auto audio=node(lilv_new_uri(world.get(),"http://lv2plug.in/ns/lv2core#AudioPort"));
        auto input=node(lilv_new_uri(world.get(),"http://lv2plug.in/ns/lv2core#InputPort"));
        auto output=node(lilv_new_uri(world.get(),"http://lv2plug.in/ns/lv2core#OutputPort"));
        LILV_FOREACH(plugins,it,plugins) {
            const auto* p=lilv_plugins_get(plugins,it);if(!lilv_plugin_verify(p))throw std::runtime_error("Invalid LV2 plugin metadata");
            kick::clap::Descriptor d;d.path=path;d.id=lilv_node_as_uri(lilv_plugin_get_uri(p));
            auto name=node(lilv_plugin_get_name(p)),author=node(lilv_plugin_get_author_name(p));
            d.name=name?lilv_node_as_string(name.get()):d.id;d.vendor=author?lilv_node_as_string(author.get()):"";
            const auto ins=lilv_plugin_get_num_ports_of_class(p,audio.get(),input.get(),nullptr),outs=lilv_plugin_get_num_ports_of_class(p,audio.get(),output.get(),nullptr);
            const std::string kind=lilv_node_as_uri(lilv_plugin_class_get_uri(lilv_plugin_get_class(p)));
            d.features.push_back(kind=="http://lv2plug.in/ns/lv2core#InstrumentPlugin"?"instrument":outs?(ins?"audio-effect":"instrument"):"note-effect");result.plugins.push_back(std::move(d));
        }
        if(result.plugins.empty())throw std::runtime_error("No LV2 plugins in bundle");
#else
        throw std::runtime_error("LV2 support is disabled in this build");
#endif
    }catch(const std::exception& e){result.plugins.clear();result.failures.push_back({source.string(),e.what()});}
    return result;
}
}

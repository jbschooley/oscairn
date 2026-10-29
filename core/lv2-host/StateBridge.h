// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <lilv/lilv.h>
#include <lv2/state/state.h>
#include <cstring>
#include <filesystem>
#include <vector>

namespace oscairn::lv2
{
// Lilv's restore API returns void, and its snapshot API can retain partial state
// after a plugin error. This state-only proxy preserves callback status while
// keeping Lilv responsible for serialization and path mapping. Never run it.
struct StateBridge
{
    const LV2_State_Interface* interface;
    LV2_Handle handle;
    LV2_State_Status status=LV2_STATE_SUCCESS;
    bool missingPath=false;
    LV2_Descriptor descriptor;
    LilvInstance proxy;
    const LV2_State_Map_Path* originalPaths=nullptr;
    LV2_State_Map_Path checkedPaths{this,abstractPath,absolutePath};
    explicit StateBridge(LilvInstance* instance,const LV2_State_Interface* state)
        :interface(state),handle(lilv_instance_get_handle(instance)),descriptor(*lilv_instance_get_descriptor(instance)),proxy(*instance)
    {
        descriptor.extension_data=[](const char* uri)->const void* {return !std::strcmp(uri,LV2_STATE__interface)?&callbacks:nullptr;};
        proxy.lv2_descriptor=&descriptor;proxy.lv2_handle=this;
    }
    static char* abstractPath(void* h,const char* path) noexcept
    {
        auto& s=*static_cast<StateBridge*>(h);
        try{return s.originalPaths->abstract_path(s.originalPaths->handle,path);}
        catch(...){s.status=LV2_STATE_ERR_UNKNOWN;return nullptr;}
    }
    static char* absolutePath(void* h,const char* path) noexcept
    {
        auto& s=*static_cast<StateBridge*>(h);char* result=nullptr;
        try
        {
            result=s.originalPaths->absolute_path(s.originalPaths->handle,path);
            std::error_code error;if(!result || !std::filesystem::exists(result,error) || error)s.missingPath=true;
        }
        catch(...){s.missingPath=true;}
        return result;
    }
    static LV2_State_Status save(LV2_Handle h,LV2_State_Store_Function store,LV2_State_Handle context,uint32_t flags,const LV2_Feature* const* features) noexcept
    {
        auto& s=*static_cast<StateBridge*>(h);
        try{return s.status=s.interface->save(s.handle,store,context,flags,features);}
        catch(...){return s.status=LV2_STATE_ERR_UNKNOWN;}
    }
    static LV2_State_Status restore(LV2_Handle h,LV2_State_Retrieve_Function retrieve,LV2_State_Handle context,uint32_t flags,const LV2_Feature* const* features) noexcept
    {
        auto& s=*static_cast<StateBridge*>(h);
        try
        {
            std::vector<const LV2_Feature*> checked;
            LV2_Feature paths{LV2_STATE__mapPath,&s.checkedPaths};
            for(auto f=features;f && *f;++f)
            {
                if(!std::strcmp((*f)->URI,LV2_STATE__mapPath)){s.originalPaths=static_cast<LV2_State_Map_Path*>((*f)->data);checked.push_back(&paths);}
                else checked.push_back(*f);
            }
            checked.push_back(nullptr);
            const auto result=s.interface->restore(s.handle,retrieve,context,flags,checked.data());
            if(result!=LV2_STATE_SUCCESS)s.status=result;
            return s.status;
        }
        catch(...){return s.status=LV2_STATE_ERR_UNKNOWN;}
    }
    inline static const LV2_State_Interface callbacks{save,restore};
};
}

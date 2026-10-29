// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <lv2/urid/urid.h>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace oscairn::lv2
{
// The URID contract permits allocation/locking: plugins cache IDs before run().
// Like Jalv/MOD's mapper, serialize creation instead of failing on contention.
// std::map owns immutable strings whose addresses survive subsequent insertions.
// See AUDIT.md for the reference implementations; no upstream code is copied.
class Urids
{
    std::mutex mutex_;
    std::map<std::string, LV2_URID, std::less<>> names_;
    std::vector<const std::string*> ids_;
public:
    static LV2_URID map(void* handle, const char* uri) noexcept
    {
        if(!uri)return 0;
        try
        {
            auto& self=*static_cast<Urids*>(handle);
            std::lock_guard lock(self.mutex_);
            const auto found=self.names_.find(uri);
            if(found!=self.names_.end())return found->second;
            if(self.ids_.size()==std::numeric_limits<LV2_URID>::max())return 0;
            const auto id=static_cast<LV2_URID>(self.ids_.size()+1);
            const auto entry=self.names_.emplace(uri,id).first;
            try { self.ids_.push_back(&entry->first); }
            catch(...) { self.names_.erase(entry); throw; }
            return id;
        }
        catch(...) { return 0; } // Never unwind through the plugin's C callback.
    }
    static const char* unmap(void* handle, LV2_URID id) noexcept
    {
        try
        {
            auto& self=*static_cast<Urids*>(handle);
            std::lock_guard lock(self.mutex_);
            return id && id<=self.ids_.size() ? self.ids_[id-1]->c_str() : nullptr;
        }
        catch(...) { return nullptr; }
    }
    LV2_URID operator()(const char* uri) { return map(this,uri); }
};
}

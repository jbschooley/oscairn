// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../Worker.h"
#include "AllocationGuard.h"
#include <array>
#include <cstring>
#include <iostream>
#include <semaphore>
#include <stdexcept>

namespace
{
using oscairn::lv2::Worker;
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Plugin
{
    Worker& worker;
    bool offline=false,blocked=false,followup=false,fail=false;
    const std::thread::id audio=std::this_thread::get_id();
    std::binary_semaphore entered{0},release{0};
    std::atomic<uint32_t> works{0},successfulReplies{0},rejectedReplies{0},active{0};
    std::atomic<bool> valid{true};
    uint32_t replies=0,ends=0;
    static LV2_Worker_Status work(void* handle,LV2_Worker_Respond_Function respond,void* context,uint32_t size,const void* data)
    {
        auto& p=*static_cast<Plugin*>(handle);
        if(p.active.fetch_add(1)!=0)p.valid=false;
        const auto ordinal=p.works.fetch_add(1);
        if(p.offline!=(std::this_thread::get_id()==p.audio))p.valid=false;
        if(!size && data)p.valid=false;
        if(size && !p.offline && reinterpret_cast<uintptr_t>(data)%alignof(std::max_align_t))p.valid=false;
        if(size)
        {
            const auto* bytes=static_cast<const uint8_t*>(data);
            for(uint32_t i=1;i<size;++i)if(bytes[i]!=bytes[0])p.valid=false;
        }
        if(p.blocked && !ordinal){p.entered.release();p.release.acquire();}
        if(respond(context,size,data)==LV2_WORKER_SUCCESS)++p.successfulReplies;else ++p.rejectedReplies;
        --p.active;
        return p.fail?LV2_WORKER_ERR_UNKNOWN:LV2_WORKER_SUCCESS;
    }
    static LV2_Worker_Status response(void* handle,uint32_t size,const void* data)
    {
        auto& p=*static_cast<Plugin*>(handle);++p.replies;
        if(std::this_thread::get_id()!=p.audio || (!size && data))p.valid=false;
        if(size && reinterpret_cast<uintptr_t>(data)%alignof(std::max_align_t))p.valid=false;
        if(p.followup)
        {
            p.followup=false;
            if(p.worker.feature()->schedule_work(&p.worker,0,nullptr)!=LV2_WORKER_SUCCESS)p.valid=false;
        }
        return p.fail?LV2_WORKER_ERR_UNKNOWN:LV2_WORKER_SUCCESS;
    }
    static LV2_Worker_Status end(void* handle)
    {auto& p=*static_cast<Plugin*>(handle);++p.ends;return p.fail?LV2_WORKER_ERR_UNKNOWN:LV2_WORKER_SUCCESS;}
    static constexpr LV2_Worker_Interface interface{work,response,end};
};
void offline()
{
    Worker worker(true);Plugin p{worker};p.offline=true;worker.attach(&p,&Plugin::interface);
    p.followup=true;
    check(worker.feature()->schedule_work(&worker,0,nullptr)==LV2_WORKER_SUCCESS,"zero-byte offline request");
    check(p.works==2 && p.replies==2 && p.valid,"synchronous response and non-overlapping follow-up work before schedule returns");
    worker.endRun();check(p.ends==1,"end-run delivered without pending replies");
    std::array<uint8_t,Worker::packetBytes> bytes{};bytes.fill(42);
    check(worker.feature()->schedule_work(&worker,bytes.size(),bytes.data())==LV2_WORKER_SUCCESS,"maximum packet accepted");
    check(worker.feature()->schedule_work(&worker,bytes.size()+1,bytes.data())==LV2_WORKER_ERR_NO_SPACE,"oversized request rejected before copying");
    check(worker.feature()->schedule_work(&worker,1,nullptr)==LV2_WORKER_ERR_UNKNOWN,"invalid request rejected");
    p.fail=true;worker.feature()->schedule_work(&worker,0,nullptr);worker.endRun();
    check(worker.errors()==4 && worker.drops()==1,"callback failures and rejected messages counted");
    worker.finish();worker.finish();
    check(worker.feature()->schedule_work(&worker,0,nullptr)==LV2_WORKER_ERR_UNKNOWN,"closed worker refuses work");
}
void saturationAndShutdown()
{
    Worker worker(false);Plugin p{worker};p.blocked=true;worker.attach(&p,&Plugin::interface);
    auto* schedule=worker.feature();
    forbidAllocation=true;const auto first=schedule->schedule_work(schedule->handle,0,nullptr);forbidAllocation=false;
    check(first==LV2_WORKER_SUCCESS,"realtime schedule does not allocate");p.entered.acquire();
    std::array<uint8_t,Worker::packetBytes> bytes{};
    for(uint32_t i=0;i<Worker::queueCapacity;++i)
    {
        bytes.fill(static_cast<uint8_t>(i));
        forbidAllocation=true;const auto status=schedule->schedule_work(schedule->handle,bytes.size(),bytes.data());forbidAllocation=false;
        check(status==LV2_WORKER_SUCCESS,"bounded queue accepts complete copied packets");
    }
    check(schedule->schedule_work(schedule->handle,0,nullptr)==LV2_WORKER_ERR_NO_SPACE,"full request queue rejects without partial publication");
    bytes.fill(255); // The worker must own copies, not pointers into caller storage.
    p.release.release();worker.finish();
    check(p.works==Worker::queueCapacity+1 && p.active==0 && p.valid,"shutdown joins after all accepted work completes");
    check(p.replies==p.successfulReplies && p.replies==Worker::queueCapacity && p.rejectedReplies==1,"shutdown delivers every accepted response; response saturation is explicit");
    check(worker.drops()==2,"request and response overflow counted");
}
void liveDelivery()
{
    Worker worker(false);Plugin p{worker};worker.attach(&p,&Plugin::interface);
    for(uint32_t i=0;i<2000;++i)
    {
        const uint8_t value=static_cast<uint8_t>(i);
        forbidAllocation=true;const auto status=worker.feature()->schedule_work(&worker,1,&value);worker.emitResponses();worker.endRun();forbidAllocation=false;
        check(status==LV2_WORKER_SUCCESS || status==LV2_WORKER_ERR_NO_SPACE,"defined scheduling result");
        std::this_thread::yield();
    }
    worker.finish();
    check(p.works>0 && p.replies==p.successfulReplies && p.valid && p.active==0,"live audio receives replies with serialized background work");
}
}
int main()
{
    try{offline();saturationAndShutdown();liveDelivery();std::cout<<"ok: Worker packets, threading, offline timing, saturation and shutdown\n";return 0;}
    catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}
}

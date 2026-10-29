// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC
// Adapted from Jalv src/worker.c, a0f01621908bfdda55f568d569853429dd2a3544.
// Replaces Zix with fixed packet queues and C++ synchronization; adds bounded
// delivery, synchronous offline replies, zero-length packets and shutdown drain.
// The original permission notice is in third_party/jalv/ISC.txt.
#include "Worker.h"
#include <cstring>
#include <stdexcept>

namespace oscairn::lv2
{
static_assert(std::atomic<uint32_t>::is_always_lock_free);
bool Worker::Queue::push(uint32_t size,const void* data) noexcept
{
    const auto head=head_.load(std::memory_order_relaxed);
    if(head-tail_.load(std::memory_order_acquire)==queueCapacity)return false;
    auto& packet=packets_[head%queueCapacity];packet.size=size;
    if(size)std::memcpy(packet.data.data(),data,size);
    head_.store(head+1,std::memory_order_release);
    return true;
}
bool Worker::Queue::pop(Packet& packet) noexcept
{
    const auto tail=tail_.load(std::memory_order_relaxed);
    if(tail==head_.load(std::memory_order_acquire))return false;
    const auto& source=packets_[tail%queueCapacity];packet.size=source.size;
    if(packet.size)std::memcpy(packet.data.data(),source.data.data(),packet.size);
    tail_.store(tail+1,std::memory_order_release);
    return true;
}
uint32_t Worker::Queue::available() const noexcept
{return head_.load(std::memory_order_acquire)-tail_.load(std::memory_order_relaxed);}

void Worker::attach(LV2_Handle instance,const LV2_Worker_Interface* interface)
{
    if(interface_ || !instance || !interface || !interface->work || !interface->work_response)
        throw std::runtime_error("Invalid LV2 Worker interface");
    auto responses=std::make_unique<Queue>();
    auto requests=offline_?nullptr:std::make_unique<Queue>();
    responses_=std::move(responses);requests_=std::move(requests);
    instance_=instance;interface_=interface;accepting_=true;
    if(!offline_)thread_=std::thread([this]{run();});
}
void Worker::work(uint32_t size,const void* data) noexcept
{
    if(interface_->work(instance_,respond,this,size,data)!=LV2_WORKER_SUCCESS)++errors_;
}
void Worker::run() noexcept
{
    Packet packet;
    for(;;)
    {
        wake_.acquire();
        if(requests_->pop(packet))work(packet.size,packet.size?packet.data.data():nullptr);
        else if(stopping_.load(std::memory_order_acquire))break;
    }
}
LV2_Worker_Status Worker::schedule(void* handle,uint32_t size,const void* data) noexcept
{
    auto& self=*static_cast<Worker*>(handle);
    if(!self.accepting_ || (size && !data) || self.working_){++self.errors_;return LV2_WORKER_ERR_UNKNOWN;}
    if(size>packetBytes){++self.drops_;return LV2_WORKER_ERR_NO_SPACE;}
    if(self.offline_)
    {
        self.working_=true;self.work(size,data);self.working_=false;
        // Deliver before schedule_work returns, but only after work has returned.
        // A response can schedule another work call (eg-sampler frees its old sample).
        self.emitResponses();
    }
    else
    {
        if(!self.requests_->push(size,data)){++self.drops_;return LV2_WORKER_ERR_NO_SPACE;}
        self.wake_.release();
    }
    return LV2_WORKER_SUCCESS;
}
LV2_Worker_Status Worker::respond(void* handle,uint32_t size,const void* data) noexcept
{
    auto& self=*static_cast<Worker*>(handle);
    if(size && !data){++self.errors_;return LV2_WORKER_ERR_UNKNOWN;}
    if(size>packetBytes || !self.responses_->push(size,data)){++self.drops_;return LV2_WORKER_ERR_NO_SPACE;}
    return LV2_WORKER_SUCCESS;
}
void Worker::emitResponses() noexcept
{
    if(!interface_ || delivering_)return;
    delivering_=true;
    // Snapshot bounds render work even while the worker produces more responses.
    const auto count=responses_->available();Packet packet;
    for(uint32_t i=0;(offline_ || i<count) && responses_->pop(packet);++i)
        if(interface_->work_response(instance_,packet.size,packet.size?packet.data.data():nullptr)!=LV2_WORKER_SUCCESS)++errors_;
    delivering_=false;
}
void Worker::endRun() noexcept
{
    if(interface_ && interface_->end_run && interface_->end_run(instance_)!=LV2_WORKER_SUCCESS)++errors_;
}
void Worker::finish() noexcept
{
    if(!accepting_)return;
    // Rendering is quiescent. Drain every accepted request before joining.
    if(thread_.joinable())
    {
        stopping_.store(true,std::memory_order_release);wake_.release();thread_.join();
    }
    // Deliver successful responses before cleanup. Follow-up cleanup work from
    // work_response is synchronous here, with no live audio or worker to race it.
    offline_=true;
    const bool pending=responses_->available()!=0;
    while(responses_->available())emitResponses();
    accepting_=false;
    if(pending)endRun();
}
}

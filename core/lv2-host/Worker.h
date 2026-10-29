// Copyright 2007-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC
// C++ adaptation of Jalv's Worker lifecycle; see third_party/jalv/upstream.json.
#pragma once
#include <lv2/worker/worker.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <thread>

namespace oscairn::lv2
{
class Worker
{
public:
    static constexpr uint32_t packetBytes=4096, queueCapacity=32;
    explicit Worker(bool offline):offline_(offline){}
    ~Worker(){finish();}
    Worker(const Worker&)=delete;
    Worker& operator=(const Worker&)=delete;
    // Attach before activation. Call finish after rendering stops, before cleanup.
    void attach(LV2_Handle instance,const LV2_Worker_Interface* interface);
    void finish() noexcept;
    void emitResponses() noexcept;
    void endRun() noexcept;
    LV2_Worker_Schedule* feature() noexcept {return &schedule_;}
    uint64_t drops() const noexcept {return drops_.load();}
    uint64_t errors() const noexcept {return errors_.load();}
private:
    struct Packet {uint32_t size=0;alignas(std::max_align_t) std::array<uint8_t,packetBytes> data;};
    // A whole packet is published with one release store, never header then body.
    class Queue
    {
        std::array<Packet,queueCapacity> packets_;
        std::atomic<uint32_t> head_{0},tail_{0};
    public:
        bool push(uint32_t size,const void* data) noexcept;
        bool pop(Packet& packet) noexcept;
        uint32_t available() const noexcept;
    };
    std::unique_ptr<Queue> requests_,responses_; // Allocated off-thread only on attach.
    std::counting_semaphore<queueCapacity+1> wake_{0};
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> drops_{0},errors_{0};
    bool offline_,accepting_=false,working_=false,delivering_=false;
    LV2_Handle instance_=nullptr;
    const LV2_Worker_Interface* interface_=nullptr;
    LV2_Worker_Schedule schedule_{this,schedule};
    static LV2_Worker_Status schedule(void*,uint32_t,const void*) noexcept;
    static LV2_Worker_Status respond(void*,uint32_t,const void*) noexcept;
    void run() noexcept;
    void work(uint32_t,const void*) noexcept;
};
}

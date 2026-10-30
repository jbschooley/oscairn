// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Worker.h"
#include "oscairn/plugin_host.h"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <thread>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/os_sync_wait_on_address.h>
#else
#include <climits>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#endif

namespace oscairn::worker
{
    bool waitWhileEqual(std::atomic<uint32_t>& word, uint32_t value, std::chrono::nanoseconds timeout) noexcept
    {
        const auto until = std::chrono::steady_clock::now() + timeout;
        while(word.load(std::memory_order_acquire) == value)
        {
            const auto left = until - std::chrono::steady_clock::now();
            if(left <= std::chrono::nanoseconds::zero()) return false;
            const auto ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(left).count());
#ifdef __APPLE__
            mach_timebase_info_data_t base; mach_timebase_info(&base);
            os_sync_wait_on_address_with_timeout(&word, value, sizeof(uint32_t), OS_SYNC_WAIT_ON_ADDRESS_SHARED, OS_CLOCK_MACH_ABSOLUTE_TIME, ns * base.denom / base.numer);
#else
            timespec t{time_t(ns / 1000000000u), long(ns % 1000000000u)};
            syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAIT, value, &t, nullptr, 0);
#endif
        }
        return true;
    }

    void wake(std::atomic<uint32_t>& word) noexcept
    {
#ifdef __APPLE__
        os_sync_wake_by_address_any(&word, sizeof(uint32_t), OS_SYNC_WAKE_BY_ADDRESS_SHARED);
#else
        syscall(SYS_futex, reinterpret_cast<uint32_t*>(&word), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0);
#endif
    }

    bool makeRealtime(double periodUs) noexcept
    {
#ifdef __APPLE__
        mach_timebase_info_data_t base; mach_timebase_info(&base);
        const auto ticks = [&](double us) { return uint32_t(us * 1000.0 * base.denom / base.numer); };
        thread_time_constraint_policy_data_t policy{ticks(periodUs), ticks(periodUs * .5), ticks(periodUs * .9), 1};
        return thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
#else
        (void)periodUs; sched_param p{}; p.sched_priority = 70;
        return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
#endif
    }

    namespace
    {
        void writeAll(int socket, const char* data, size_t size)
        {
            while(size)
            {
#ifdef MSG_NOSIGNAL
                const auto n = ::send(socket, data, size, MSG_NOSIGNAL);
#else
                const auto n = ::send(socket, data, size, 0); // SO_NOSIGPIPE is set on the socket
#endif
                if(n < 0 && errno == EINTR) continue;
                if(n <= 0) throw std::runtime_error("Plugin worker connection lost");
                data += n; size -= size_t(n);
            }
        }
        void readAll(int socket, char* data, size_t size)
        {
            while(size)
            {
                const auto n = ::recv(socket, data, size, 0);
                if(n < 0 && errno == EINTR) continue;
                if(n <= 0) throw std::runtime_error(n == 0 ? "Plugin worker connection closed" : "Plugin worker did not answer in time");
                data += n; size -= size_t(n);
            }
        }
    }

    void send(int socket, const std::string& message)
    {
        const auto n = uint32_t(message.size());
        writeAll(socket, reinterpret_cast<const char*>(&n), 4);
        writeAll(socket, message.data(), message.size());
    }

    std::string receive(int socket, size_t maxBytes)
    {
        uint32_t n = 0; readAll(socket, reinterpret_cast<char*>(&n), 4);
        if(n > maxBytes) throw std::runtime_error("Plugin worker message too large");
        std::string data(n, '\0'); readAll(socket, data.data(), n);
        return data;
    }

    void sendDescriptor(int socket, int fd)
    {
        char byte = 0; iovec io{&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr m{}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof(control);
        auto* c = CMSG_FIRSTHDR(&m); c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &fd, sizeof(int));
        if(sendmsg(socket, &m, 0) != 1) throw std::runtime_error("Cannot pass shared memory to the plugin worker");
    }

    int receiveDescriptor(int socket)
    {
        char byte = 0; iovec io{&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
        msghdr m{}; m.msg_iov = &io; m.msg_iovlen = 1; m.msg_control = control; m.msg_controllen = sizeof(control);
        if(recvmsg(socket, &m, 0) != 1) throw std::runtime_error("No shared memory from the engine");
        auto* c = CMSG_FIRSTHDR(&m);
        if(!c || c->cmsg_type != SCM_RIGHTS) throw std::runtime_error("No shared memory from the engine");
        int fd; std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
        return fd;
    }

    namespace
    {
        uint32_t channels(const std::vector<kick::clap::Port>& ports) { uint32_t n = 0; for(const auto& p : ports) n += p.channels; return n; }

        void reply(int socket, const Writer& w) { send(socket, std::string(1, '\1') + w.data); }
        void fail(int socket, const std::string& error) { send(socket, std::string(1, '\0') + error); }

        void putPorts(Writer& w, const std::vector<kick::clap::Port>& ports)
        {
            w.u32(uint32_t(ports.size()));
            for(const auto& p : ports) w.u32(p.id).str(p.name).u32(p.channels).u32(p.flags);
        }

        // The worker's audio thread: one plugin block per request.
        void audio(PluginHost& host, Shared& s, uint32_t blockFrames)
        {
            std::vector<const float*> in(s.inputChannels);
            std::vector<float*> out(s.outputChannels);
            for(uint32_t c = 0; c < s.inputChannels; ++c) in[c] = s.input(c);
            for(uint32_t c = 0; c < s.outputChannels; ++c) out[c] = s.output(c);
            makeRealtime(1e6 * blockFrames / s.sampleRate); // the block period, not the larger shared buffer
            uint32_t seen = s.done.load(std::memory_order_acquire); // a request may already be waiting
            for(;;)
            {
                waitWhileEqual(s.request, seen, std::chrono::seconds(1));
                if(s.stop.load(std::memory_order_acquire)) return;
                const auto next = s.request.load(std::memory_order_acquire);
                if(next == seen) continue;
                seen = next;
                if(s.panic.exchange(0, std::memory_order_acq_rel)) host.panic();
                const bool ok = host.process(in.data(), s.inputChannels, out.data(), s.outputChannels, s.frames,
                                             s.eventCount ? s.events : nullptr, s.eventCount, s.hasTransport ? &s.transport : nullptr);
                uint32_t count = 0; const auto* emitted = host.outputEvents(count);
                const auto kept = std::min(count, maxOutputEvents);
                std::copy_n(emitted, kept, s.outputs);
                s.outputCount = kept; s.ok = ok;
                s.outputDrops.store(host.outputEventDrops() + (count - kept), std::memory_order_relaxed);
                const auto st = host.statistics();
                s.blocks.store(st.blocks); s.droppedEvents.store(st.droppedEvents); s.processErrors.store(st.processErrors);
                s.deadlineMisses.store(st.deadlineMisses); s.controlSilences.store(st.controlSilences); s.idleBlocks.store(st.idleBlocks);
                s.lastMicroseconds.store(st.lastMicroseconds); s.maximumMicroseconds.store(st.maximumMicroseconds);
                s.done.store(seen, std::memory_order_release);
                wake(s.done);
            }
        }
    }

    int run(int socket)
    {
        unsetenv("OSCAIRN_PLUGIN_WORKER"); // the worker hosts the plugin itself
        std::shared_ptr<PluginHost> host;
        Shared* shared = nullptr; size_t sharedBytes = 0;
        std::thread audioThread;
        (void)sharedBytes;
        auto publishParameters = [&] {
            if(!shared) return;
            const auto& params = host->parameters();
            for(uint32_t i = 0; i < shared->parameterCount; ++i) shared->parameterValues[i].store(host->parameterValue(params[i].id), std::memory_order_relaxed);
        };
        auto stopAudio = [&] {
            if(!audioThread.joinable()) return;
            shared->stop.store(1, std::memory_order_release); shared->request.fetch_add(1, std::memory_order_release); wake(shared->request);
            audioThread.join();
        };
        for(;;)
        {
            pollfd p{socket, POLLIN, 0};
            const int ready = poll(&p, 1, 10);
            // The plugin is serviced only when the engine asks, as in the engine; values are refreshed meanwhile.
            if(ready == 0) { publishParameters(); continue; }
            if(ready < 0) { if(errno == EINTR) continue; break; }
            Reader r;
            try { r.data = receive(socket); } catch(const std::exception&) { break; }
            try
            {
                const auto op = Op(r.u8());
                Writer w;
                switch(op)
                {
                case Op::Init:
                {
                    if(host) throw std::runtime_error("Plugin already loaded");
                    const auto path = r.str(), id = r.str(); const double rate = r.f64(); const auto frames = r.u32();
                    host = loadPlugin(path, id, rate, frames);
                    const auto& d = host->descriptor();
                    w.str(d.path).str(d.id).str(d.name).str(d.vendor).str(d.version).str(d.description).u32(uint32_t(d.features.size()));
                    for(const auto& f : d.features) w.str(f);
                    const auto& params = host->parameters();
                    if(params.size() > maxParameters) throw std::runtime_error("Plugin has too many parameters for a worker");
                    w.u32(uint32_t(params.size()));
                    for(const auto& q : params) w.u32(q.id).str(q.name).str(q.module).f64(q.minimum).f64(q.maximum).f64(q.defaultValue).u32(q.flags);
                    putPorts(w, host->inputPorts()); putPorts(w, host->outputPorts());
                    w.u32(host->noteInputPorts()).u32(host->noteOutputPorts());
                    for(uint32_t i = 0; i < host->noteInputPorts(); ++i) w.u8(host->usesClapNotes(i));
                    w.u8(host->supportsState()).u32(host->soundContextCapacity()).u8(host->effectResources().has_value());
                    reply(socket, w);
                    // The engine answers with the shared block, sized from the ports above.
                    const int fd = receiveDescriptor(socket);
                    struct stat info{};
                    if(fstat(fd, &info) != 0 || size_t(info.st_size) < Shared::bytes(channels(host->inputPorts()), channels(host->outputPorts()), frames))
                        { close(fd); throw std::runtime_error("Shared memory is too small"); }
                    sharedBytes = size_t(info.st_size);
                    void* memory = mmap(nullptr, sharedBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    close(fd);
                    if(memory == MAP_FAILED) throw std::runtime_error("Cannot map shared memory");
                    shared = static_cast<Shared*>(memory);
                    publishParameters();
                    audioThread = std::thread(audio, std::ref(*host), std::ref(*shared), frames);
                    reply(socket, Writer{});
                    continue;
                }
                case Op::Service:
                {
                    host->service(); publishParameters();
                    kick::clap::Event e; std::vector<kick::clap::Event> events;
                    while(events.size() < 4096 && host->popOutput(e)) events.push_back(e);
                    w.u32(uint32_t(events.size()));
                    for(const auto& x : events) w.bytes(x.bytes.data(), x.bytes.size());
                    break;
                }
                case Op::SetParameter: { const auto id = r.u32(); const auto v = r.f64(); w.u8(host->setParameter(id, v)); break; }
                case Op::Gesture: { const auto id = r.u32(); const bool begin = r.u8(); w.u8(host->parameterGesture(id, begin)); break; }
                case Op::ParameterText: { const auto id = r.u32(); w.str(host->parameterText(id, r.f64())); break; }
                case Op::SaveState: host->saveState(r.str()); break;
                case Op::LoadState: host->loadState(r.str()); publishParameters(); break;
                case Op::Shutdown: stopAudio(); reply(socket, w); host.reset(); return 0;
                default: throw std::runtime_error("Unknown worker request");
                }
                reply(socket, w);
            }
            catch(const std::exception& e)
            {
                try { fail(socket, e.what()); } catch(const std::exception&) { break; }
            }
        }
        if(shared) stopAudio();
        return 0;
    }
}

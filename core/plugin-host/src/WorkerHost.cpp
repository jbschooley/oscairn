// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Engine side of a plugin worker (RFC 0010 step 2): a PluginHost whose plugin runs in a
// child process. Each block is handed over through shared memory; control calls travel
// over a socket. Plugins with native sound or effect contexts stay in the engine for now.
#include "Worker.h"
#include "oscairn/plugin_host.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

extern char** environ;

namespace oscairn
{
    namespace
    {
        using namespace worker;

        uint32_t channels(const std::vector<kick::clap::Port>& ports) { uint32_t n = 0; for(const auto& p : ports) n += p.channels; return n; }

        std::vector<kick::clap::Port> readPorts(Reader& r)
        {
            std::vector<kick::clap::Port> ports(r.u32());
            for(auto& p : ports) { p.id = r.u32(); p.name = r.str(); p.channels = r.u32(); p.flags = r.u32(); }
            return ports;
        }

        class WorkerHost final : public PluginHost
        {
        public:
            WorkerHost(const std::string& program, const std::string& path, const std::string& id, double rate, uint32_t frames, ProcessingMode mode)
            {
                try { start(program, path, id, rate, frames, mode); }
                catch(...) { stop(); throw; }
            }

            ~WorkerHost() override { stop(); }

            bool native = false; // the plugin has native contexts; it must stay in the engine

        private:
            void start(const std::string& program, const std::string& path, const std::string& id, double rate, uint32_t frames, ProcessingMode mode)
            {
                int pair[2];
                if(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) throw std::runtime_error("Cannot create a worker socket");
#ifdef SO_NOSIGPIPE
                const int one = 1; setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                fcntl(pair[0], F_SETFD, FD_CLOEXEC);
                socket_ = pair[0];
                posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
                posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
                std::string fd = "3"; char* argv[]{const_cast<char*>(program.c_str()), fd.data(), nullptr};
                const int spawned = posix_spawn(&pid_, program.c_str(), &actions, nullptr, argv, environ);
                posix_spawn_file_actions_destroy(&actions);
                close(pair[1]);
                if(spawned != 0) { close(socket_); socket_ = -1; pid_ = 0; throw std::runtime_error("Cannot start plugin worker " + program); }
                timeval t{30, 0}; setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);

                Reader r = request(Writer().u8(uint8_t(Op::Init)).str(path).str(id).f64(rate).u32(frames));
                descriptor_.path = r.str(); descriptor_.id = r.str(); descriptor_.name = r.str(); descriptor_.vendor = r.str();
                descriptor_.version = r.str(); descriptor_.description = r.str();
                for(auto n = r.u32(); n--;) descriptor_.features.push_back(r.str());
                parameters_.resize(r.u32());
                for(auto& p : parameters_) { p.id = r.u32(); p.name = r.str(); p.module = r.str(); p.minimum = r.f64(); p.maximum = r.f64(); p.defaultValue = r.f64(); p.flags = r.u32(); }
                for(uint32_t i = 0; i < parameters_.size(); ++i) index_[parameters_[i].id] = i;
                inputs_ = readPorts(r); outputs_ = readPorts(r);
                noteInputs_ = r.u32(); noteOutputs_ = r.u32();
                for(uint32_t i = 0; i < noteInputs_; ++i) clapNotes_.push_back(r.u8());
                rate_ = rate;
                state_ = r.u8(); const auto contexts = r.u32(); const bool effects = r.u8();
                if(contexts || effects) { native = true; return; }

                // Shared block, sized for the ports and maximum block, handed over as a descriptor.
                // Room for large blocks, a multiple of the plugin's maximum, so the worker's host
                // splits them exactly as an in-engine host would.
                capacity_ = frames * std::max<uint32_t>(1, (8192 + frames - 1) / frames);
                bytes_ = Shared::bytes(channels(inputs_), channels(outputs_), capacity_);
                const std::string name = "/osc-" + std::to_string(getpid()) + "-" + std::to_string(counter_.fetch_add(1));
                const int shm = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
                if(shm < 0) throw std::runtime_error("Cannot create worker shared memory");
                shm_unlink(name.c_str());
                if(ftruncate(shm, off_t(bytes_)) != 0) { close(shm); throw std::runtime_error("Cannot size worker shared memory"); }
                void* memory = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, shm, 0);
                if(memory == MAP_FAILED) { close(shm); throw std::runtime_error("Cannot map worker shared memory"); }
                shared_ = new(memory) Shared();
                shared_->inputChannels = channels(inputs_); shared_->outputChannels = channels(outputs_);
                shared_->maxFrames = capacity_; shared_->sampleRate = rate; shared_->parameterCount = uint32_t(parameters_.size());
                for(uint32_t i = 0; i < parameters_.size(); ++i) shared_->parameterValues[i].store(parameters_[i].defaultValue);
                try { sendDescriptor(socket_, shm); } catch(...) { close(shm); throw; }
                close(shm);
                expect(receive(socket_));
                output_.resize(maxOutputEvents);
                pending_.reserve(4096);
                // Each block may take half the block's time before it is silenced; a worker that
                // misses a quarter second of blocks in a row, or leaves one unanswered that long,
                // has failed. Offline rendering waits instead of silencing.
                const double period = double(frames) / rate;
                if(mode == ProcessingMode::Offline) { budget_ = std::chrono::seconds(10); hangLimit_ = std::chrono::seconds(10); missLimit_ = UINT32_MAX; }
                else
                {
                    budget_ = std::chrono::nanoseconds(int64_t(0.5e9 * period));
                    if(const char* us = std::getenv("OSCAIRN_WORKER_BUDGET_US")) budget_ = std::chrono::microseconds(std::max(1L, std::atol(us)));
                    hangLimit_ = std::chrono::milliseconds(250);
                    missLimit_ = std::max<uint32_t>(8, uint32_t(std::ceil(0.25 / period)));
                }
                supervisor_ = std::thread([this] { supervise(); });
            }

            // Watches the worker: a crash, a block left unanswered past the hang limit, or too
            // many missed blocks in a row stop it for good and silence the node.
            void supervise()
            {
                std::unique_lock lock(superviseMutex_);
                while(!stopping_)
                {
                    wakeSupervisor_.wait_for(lock, std::chrono::milliseconds(10));
                    if(stopping_ || failed_.load(std::memory_order_acquire)) continue;
                    int status = 0;
                    if(waitpid(pid_, &status, WNOHANG) == pid_)
                    {
                        reaped_ = true;
                        fail(WIFSIGNALED(status) ? "Plugin worker crashed (signal " + std::to_string(WTERMSIG(status)) + ")"
                                                 : "Plugin worker exited (status " + std::to_string(WEXITSTATUS(status)) + ")");
                        continue;
                    }
                    const auto since = requestedAt_.load(std::memory_order_acquire);
                    const bool waiting = shared_->done.load(std::memory_order_acquire) != requested_.load(std::memory_order_acquire);
                    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
                    if(waiting && since && std::chrono::nanoseconds(now - since) > hangLimit_)
                        fail("Plugin hung: a block was not finished within " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(hangLimit_).count()) + " ms");
                    else if(tooSlow_.load(std::memory_order_acquire))
                        fail("Plugin too slow: " + std::to_string(missLimit_) + " blocks in a row missed their deadline");
                }
            }

            void fail(const std::string& reason)
            {
                { std::lock_guard lock(failureMutex_); failure_ = reason; }
                failed_.store(true, std::memory_order_release);
                if(!reaped_) { kill(pid_, SIGKILL); int status = 0; waitpid(pid_, &status, 0); reaped_ = true; }
            }

            void stop() noexcept
            {
                if(supervisor_.joinable())
                {
                    { std::lock_guard lock(superviseMutex_); stopping_ = true; }
                    wakeSupervisor_.notify_all();
                    supervisor_.join();
                }
                if(socket_ >= 0)
                {
                    if(shared_ && !failed_.load())
                    {
                        timeval t{1, 0}; setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
                        try { std::lock_guard lock(control_); send(socket_, Writer().u8(uint8_t(Op::Shutdown)).data); receive(socket_); } catch(const std::exception&) {}
                    }
                    close(socket_); socket_ = -1;
                }
                if(pid_ > 0 && !reaped_)
                {
                    int status = 0;
                    for(int i = 0; shared_ && i < 100 && waitpid(pid_, &status, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if(waitpid(pid_, &status, WNOHANG) == 0) { kill(pid_, SIGKILL); waitpid(pid_, &status, 0); }
                    reaped_ = true;
                }
                if(shared_) { munmap(shared_, bytes_); shared_ = nullptr; }
            }

        public:
            std::string failure() const override { std::lock_guard lock(failureMutex_); return failure_; }

            void service() override
            {
                Reader r = request(Writer().u8(uint8_t(Op::Service)));
                pending_.clear(); next_ = 0;
                for(auto n = r.u32(); n--;) { kick::clap::Event e; r.bytes(e.bytes.data(), e.bytes.size()); pending_.push_back(e); }
            }
            bool supportsState() const noexcept override { return state_; }
            void saveState(const std::string& directory) override { request(Writer().u8(uint8_t(Op::SaveState)).str(directory)); }
            void loadState(const std::string& directory) override { request(Writer().u8(uint8_t(Op::LoadState)).str(directory)); }
            const kick::clap::Descriptor& descriptor() const noexcept override { return descriptor_; }
            const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return parameters_; }
            const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return inputs_; }
            const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return outputs_; }
            uint32_t noteInputPorts() const noexcept override { return noteInputs_; }
            uint32_t noteOutputPorts() const noexcept override { return noteOutputs_; }
            bool usesClapNotes(uint32_t port) const noexcept override { return port < clapNotes_.size() && clapNotes_[port]; }
            void panic() noexcept override { shared_->panic.store(1, std::memory_order_release); }
            double parameterValue(clap_id id) const noexcept override
            {
                const auto i = index_.find(id);
                return i == index_.end() ? 0.0 : shared_->parameterValues[i->second].load(std::memory_order_relaxed);
            }
            std::string parameterText(clap_id id, double value) const override
            {
                return const_cast<WorkerHost*>(this)->request(Writer().u8(uint8_t(Op::ParameterText)).u32(id).f64(value)).str();
            }
            bool setParameter(clap_id id, double value) noexcept override
            {
                try { return request(Writer().u8(uint8_t(Op::SetParameter)).u32(id).f64(value)).u8(); } catch(const std::exception&) { return false; }
            }
            bool parameterGesture(clap_id id, bool begin) noexcept override
            {
                try { return request(Writer().u8(uint8_t(Op::Gesture)).u32(id).u8(begin)).u8(); } catch(const std::exception&) { return false; }
            }
            bool popOutput(kick::clap::Event& event) noexcept override
            {
                if(next_ >= pending_.size()) return false;
                event = pending_[next_++]; return true;
            }
            uint64_t outputEventDrops() const noexcept override { return shared_->outputDrops.load(std::memory_order_relaxed); }
            const kick::clap::Event* outputEvents(uint32_t& count) const noexcept override { count = outputCount_; return output_.data(); }
            kick::clap::Statistics statistics() const noexcept override
            {
                kick::clap::Statistics s;
                s.blocks = shared_->blocks.load(); s.droppedEvents = shared_->droppedEvents.load(); s.processErrors = shared_->processErrors.load();
                s.lastMicroseconds = shared_->lastMicroseconds.load(); s.maximumMicroseconds = shared_->maximumMicroseconds.load();
                s.deadlineMisses = shared_->deadlineMisses.load() + workerMisses_.load(std::memory_order_relaxed);
                s.controlSilences = shared_->controlSilences.load(); s.idleBlocks = shared_->idleBlocks.load();
                return s;
            }

            bool process(const float* const* inputs, uint32_t ni, float* const* outputs, uint32_t no, uint32_t frames,
                         const kick::clap::Event* events, uint32_t count, const clap_event_transport_t* transport) noexcept override
            {
                outputCount_ = 0;
                if(frames <= capacity_) return block(inputs, ni, outputs, no, frames, events, count, transport, 0);
                // Larger than the shared buffer: split as the in-engine host does, rebasing events,
                // transport and output event times.
                if(count > maxEvents || (count && !events) || (no && !outputs) || ni > 64 || no > 64)
                {
                    if(outputs) for(uint32_t c = 0; c < no; ++c) if(outputs[c]) std::fill_n(outputs[c], frames, 0.f);
                    return false;
                }
                const float* in[64]; float* out[64]; bool ok = true;
                for(uint32_t done = 0; done < frames;)
                {
                    const auto n = std::min(capacity_, frames - done);
                    uint32_t chunkCount = 0;
                    for(uint32_t i = 0; i < count; ++i)
                    {
                        const auto t = events[i].header()->time;
                        if(t < done || t >= done + n) continue;
                        chunk_[chunkCount] = events[i];
                        reinterpret_cast<clap_event_header_t*>(chunk_[chunkCount++].bytes.data())->time -= done;
                    }
                    for(uint32_t c = 0; c < ni; ++c) in[c] = inputs && inputs[c] ? inputs[c] + done : nullptr;
                    for(uint32_t c = 0; c < no; ++c) out[c] = outputs[c] ? outputs[c] + done : nullptr;
                    clap_event_transport_t position{};
                    if(transport)
                    {
                        position = *transport;
                        if((position.flags & CLAP_TRANSPORT_IS_PLAYING) && (position.flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE))
                            position.song_pos_seconds += clap_sectime((double(done) / rate_) * CLAP_SECTIME_FACTOR);
                        if((position.flags & CLAP_TRANSPORT_IS_PLAYING) && (position.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (position.flags & CLAP_TRANSPORT_HAS_TEMPO))
                            position.song_pos_beats += clap_beattime((double(done) / rate_) * position.tempo / 60. * CLAP_BEATTIME_FACTOR);
                    }
                    ok = block(inputs ? in : nullptr, ni, out, no, n, chunk_.data(), chunkCount, transport ? &position : nullptr, done) && ok;
                    done += n;
                }
                return ok;
            }

        private:
            // One block that fits the shared buffer. Its output events are appended to this
            // call's, their times moved by `offset`, the frames already rendered in the call.
            bool block(const float* const* inputs, uint32_t ni, float* const* outputs, uint32_t no, uint32_t frames,
                       const kick::clap::Event* events, uint32_t count, const clap_event_transport_t* transport, uint32_t offset) noexcept
            {
                auto silence = [&] {
                    if(outputs) for(uint32_t c = 0; c < no; ++c) if(outputs[c]) std::fill_n(outputs[c], frames, 0.f);
                    return false;
                };
                if(!frames) return true;
                auto& s = *shared_;
                if(failed_.load(std::memory_order_acquire) || frames > s.maxFrames || count > maxEvents || (count && !events) || (no && !outputs)) return silence();
                // A block the worker has not finished yet: stay silent rather than queue another.
                if(s.done.load(std::memory_order_acquire) != sequence_) { workerMisses_.fetch_add(1, std::memory_order_relaxed); return silence(); }
                for(uint32_t c = 0; c < s.inputChannels; ++c)
                {
                    if(inputs && c < ni && inputs[c]) std::copy_n(inputs[c], frames, s.input(c));
                    else std::fill_n(s.input(c), frames, 0.f);
                }
                std::copy_n(events, count, s.events);
                s.frames = frames; s.eventCount = count; s.hasTransport = transport != nullptr;
                if(transport) s.transport = *transport;
                const auto sent = std::chrono::steady_clock::now();
                requestedAt_.store(sent.time_since_epoch().count(), std::memory_order_release);
                requested_.store(++sequence_, std::memory_order_release);
                s.request.store(sequence_, std::memory_order_release);
                wake(s.request);
                if(!waitWhileEqual(s.done, sequence_ - 1, budget_) || s.done.load(std::memory_order_acquire) != sequence_)
                {
                    missed();
                    return silence();
                }
                // Late by the clock counts toward "too slow" even when the wait itself woke late.
                if(std::chrono::steady_clock::now() - sent > budget_) missed();
                else consecutiveMisses_ = 0;
                for(uint32_t c = 0; c < no; ++c)
                {
                    if(!outputs[c]) continue;
                    if(c < s.outputChannels) std::copy_n(s.output(c), frames, outputs[c]);
                    else std::fill_n(outputs[c], frames, 0.f);
                }
                const auto first = outputCount_, kept = std::min(s.outputCount, maxOutputEvents - first);
                std::copy_n(s.outputs, kept, output_.data() + first);
                for(uint32_t i = first; i < first + kept; ++i) reinterpret_cast<clap_event_header_t*>(output_[i].bytes.data())->time += offset;
                outputCount_ = first + kept;
                return s.ok;
            }

            // A block that ran out of budget. Only these count toward "too slow"; a block the
            // worker never finishes is a hang, which the supervisor detects.
            void missed() noexcept
            {
                workerMisses_.fetch_add(1, std::memory_order_relaxed);
                if(++consecutiveMisses_ >= missLimit_) tooSlow_.store(true, std::memory_order_release);
            }

            Reader request(const Writer& w)
            {
                if(failed_.load(std::memory_order_acquire)) throw std::runtime_error(failure());
                std::lock_guard lock(control_);
                send(socket_, w.data);
                return expect(receive(socket_));
            }
            static Reader expect(std::string reply)
            {
                if(reply.empty()) throw std::runtime_error("Empty reply from plugin worker");
                if(reply[0] != '\1') throw std::runtime_error(reply.substr(1));
                Reader r; r.data = std::move(reply); r.at = 1; return r;
            }

            static inline std::atomic<uint32_t> counter_{0};
            int socket_ = -1; pid_t pid_ = 0;
            Shared* shared_ = nullptr; size_t bytes_ = 0;
            std::mutex control_;
            std::chrono::nanoseconds budget_{}, hangLimit_{};
            uint32_t missLimit_ = UINT32_MAX, consecutiveMisses_ = 0;
            std::atomic<uint32_t> requested_{0};
            std::atomic<int64_t> requestedAt_{0};
            std::atomic<bool> failed_{false}, tooSlow_{false};
            bool reaped_ = false, stopping_ = false;
            std::thread supervisor_;
            std::mutex superviseMutex_;
            std::condition_variable wakeSupervisor_;
            mutable std::mutex failureMutex_;
            std::string failure_;
            kick::clap::Descriptor descriptor_;
            std::vector<kick::clap::Parameter> parameters_;
            std::unordered_map<clap_id, uint32_t> index_;
            std::vector<kick::clap::Port> inputs_, outputs_;
            uint32_t noteInputs_ = 0, noteOutputs_ = 0;
            std::vector<uint8_t> clapNotes_;
            bool state_ = false;
            uint32_t sequence_ = 0, outputCount_ = 0, capacity_ = 0;
            double rate_ = 48000;
            std::array<kick::clap::Event, maxEvents> chunk_{};
            std::vector<kick::clap::Event> output_, pending_;
            size_t next_ = 0;
            std::atomic<uint64_t> workerMisses_{0};
        };
    }

    std::shared_ptr<PluginHost> loadPluginInWorker(const std::string& program, const std::string& path, const std::string& id,
                                                   double sampleRate, uint32_t maxFrames, ProcessingMode mode)
    {
        auto host = std::make_shared<WorkerHost>(program, path, id, sampleRate, maxFrames, mode);
        if(host->native) return nullptr;
        return host;
    }
}

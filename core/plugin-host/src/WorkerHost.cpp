// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Engine side of plugin workers (RFC 0010): PluginHosts whose plugins run in a child process.
// A Process is one worker. It hosts one plugin and the native effect contexts created from it,
// which share its engine; each of those is a WorkerHost, a graph node with its own shared
// block for audio and events. Control calls travel over the worker's socket. A failed worker
// is restarted, and each node gets back its state, prepared sounds and parameter values.
#include "Worker.h"
#include "oscairn/plugin_host.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fcntl.h>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <map>
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

        class WorkerHost;

        // One worker process and what its nodes share: the control socket, the supervisor, the
        // block budget and the restart policy. Requests and node changes hold `control`.
        struct Process
        {
            static constexpr size_t maxRestarts = 3;
            std::string program, path, id;
            double rate; uint32_t frames;
            std::chrono::nanoseconds budget{}, hangLimit{};
            uint32_t missLimit = UINT32_MAX;
            std::mutex control;
            int socket = -1; pid_t pid = 0; bool reaped = true;
            std::string root; // the root plugin's description, which a restarted worker must repeat
            std::mutex nodesMutex;
            std::vector<WorkerHost*> nodes; // changed under control and nodesMutex
            std::atomic<kick::clap::AssetResolver*> assets{nullptr};
            std::atomic<bool> failed{false};
            std::atomic<uint32_t> generation{0};
            std::atomic<uint64_t> restarted{0};
            mutable std::mutex failureMutex;
            std::string failureText, reason; // reason: supervisor thread
            bool gaveUp = false, stopping = false;
            std::vector<std::chrono::steady_clock::time_point> restarts;
            std::filesystem::path records; // recovery records of this worker's nodes
            uint32_t saves = 0;
            std::thread supervisor;
            std::mutex superviseMutex;
            std::condition_variable wakeSupervisor;
            static inline std::atomic<uint32_t> counter{0};

            Process(std::string program_, std::string path_, std::string id_, double rate_, uint32_t frames_, ProcessingMode mode)
                : program(std::move(program_)), path(std::move(path_)), id(std::move(id_)), rate(rate_), frames(frames_)
            {
                // Each block may take half the block's time before it is silenced; a worker that
                // misses a quarter second of blocks in a row, or leaves one unanswered that long,
                // has failed. Offline rendering waits instead of silencing.
                const double period = double(frames) / rate;
                if(mode == ProcessingMode::Offline) { budget = std::chrono::seconds(10); hangLimit = std::chrono::seconds(10); }
                else
                {
                    budget = std::chrono::nanoseconds(int64_t(0.5e9 * period));
                    if(const char* us = std::getenv("OSCAIRN_WORKER_BUDGET_US")) budget = std::chrono::microseconds(std::max(1L, std::atol(us)));
                    hangLimit = std::chrono::milliseconds(250);
                    missLimit = std::max<uint32_t>(8, uint32_t(std::ceil(0.25 / period)));
                }
                records = std::filesystem::temp_directory_path() / ("oscairn-worker-" + std::to_string(getpid()) + "-" + std::to_string(counter.fetch_add(1)));
            }

            ~Process()
            {
                if(supervisor.joinable())
                {
                    { std::lock_guard lock(superviseMutex); stopping = true; }
                    wakeSupervisor.notify_all();
                    supervisor.join();
                }
                const bool asked = socket >= 0 && !failed.load();
                if(asked)
                {
                    timeval t{1, 0}; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
                    try { std::lock_guard lock(control); exchange(Writer().u8(uint8_t(Op::Shutdown)).data); } catch(const std::exception&) {}
                }
                if(socket >= 0) { close(socket); socket = -1; }
                if(pid > 0 && !reaped)
                {
                    int status = 0;
                    for(int i = 0; asked && i < 100 && waitpid(pid, &status, WNOHANG) == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    if(waitpid(pid, &status, WNOHANG) == 0) { kill(pid, SIGKILL); waitpid(pid, &status, 0); }
                    reaped = true;
                }
                std::error_code ignored;
                std::filesystem::remove_all(records, ignored);
            }

            // Starts a worker process and loads the plugin in it. The reply is the root node's
            // index and description; the caller then attaches it. Under control.
            Reader start()
            {
                int pair[2];
                if(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) throw std::runtime_error("Cannot create a worker socket");
#ifdef SO_NOSIGPIPE
                const int one = 1; setsockopt(pair[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one); setsockopt(pair[1], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                fcntl(pair[0], F_SETFD, FD_CLOEXEC);
                socket = pair[0];
                posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
                posix_spawn_file_actions_adddup2(&actions, pair[1], 3);
                std::string fd = "3"; char* argv[]{program.data(), fd.data(), nullptr};
                const int spawned = posix_spawn(&pid, program.c_str(), &actions, nullptr, argv, environ);
                posix_spawn_file_actions_destroy(&actions);
                close(pair[1]);
                if(spawned != 0) { close(socket); socket = -1; pid = 0; throw std::runtime_error("Cannot start plugin worker " + program); }
                reaped = false;
                timeval t{30, 0}; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
                return exchange(Writer().u8(uint8_t(Op::Init)).str(path).str(id).f64(rate).u32(frames).data);
            }

            // One request and its reply, answering the worker's asset requests in between. Under control.
            Reader exchange(const std::string& message)
            {
                send(socket, message);
                for(;;)
                {
                    auto reply = receive(socket);
                    if(reply.empty() || reply[0] != ResolveAsset) return expect(std::move(reply));
                    Reader r; r.data = std::move(reply); r.at = 1;
                    const auto library = r.str(), file = r.str(); auto sha256 = r.str();
                    std::string answer;
                    try
                    {
                        // The store locks itself, so a restart may resolve from the supervisor thread.
                        auto* resolver = assets.load();
                        if(!resolver) throw std::runtime_error("No sound asset store");
                        const auto local = resolver->resolve(library, file, sha256);
                        answer = std::string(1, Done) + Writer().str(local).str(sha256).data;
                    }
                    catch(const std::exception& e) { answer = std::string(1, Failed) + e.what(); }
                    send(socket, answer);
                }
            }
            static Reader expect(std::string reply)
            {
                if(reply.empty()) throw std::runtime_error("Empty reply from plugin worker");
                if(reply[0] == Invalid) throw std::invalid_argument(reply.substr(1));
                if(reply[0] != Done) throw std::runtime_error(reply.substr(1));
                Reader r; r.data = std::move(reply); r.at = 1; return r;
            }

            std::string failure() const { std::lock_guard lock(failureMutex); return failureText; }
            void fail(const std::string& why)
            {
                reason = why;
                { std::lock_guard lock(failureMutex); failureText = why; }
                failed.store(true);
                if(!reaped) { kill(pid, SIGKILL); int status = 0; waitpid(pid, &status, 0); reaped = true; }
            }

            void supervise();
            void restart();
        };

        class WorkerHost final : public PluginHost
        {
        public:
            // Attaches a node the worker just described (index, description). Under process->control.
            WorkerHost(std::shared_ptr<Process> process, Reader& r, bool root) : process_(std::move(process)), root_(root)
            {
                node_ = r.u32();
                const auto at = r.at;
                descriptor_.path = r.str(); descriptor_.id = r.str(); descriptor_.name = r.str(); descriptor_.vendor = r.str();
                descriptor_.version = r.str(); descriptor_.description = r.str();
                for(auto n = r.u32(); n--;) descriptor_.features.push_back(r.str());
                parameters_.resize(r.u32());
                for(auto& p : parameters_) { p.id = r.u32(); p.name = r.str(); p.module = r.str(); p.minimum = r.f64(); p.maximum = r.f64(); p.defaultValue = r.f64(); p.flags = r.u32(); }
                for(uint32_t i = 0; i < parameters_.size(); ++i) index_[parameters_[i].id] = i;
                inputs_ = readPorts(r); outputs_ = readPorts(r);
                noteInputs_ = r.u32(); noteOutputs_ = r.u32();
                for(uint32_t i = 0; i < noteInputs_; ++i) clapNotes_.push_back(r.u8());
                state_ = r.u8(); contexts_ = r.u32(); effects_ = r.u8();
                description_ = r.data.substr(at, r.at - at);

                // Shared block, sized for the ports and maximum block, handed over as a descriptor.
                // Room for large blocks, a multiple of the plugin's maximum, so the worker's host
                // splits them exactly as an in-engine host would. The engine keeps it across restarts.
                const auto frames = process_->frames;
                capacity_ = frames * std::max<uint32_t>(1, (8192 + frames - 1) / frames);
                bytes_ = Shared::bytes(channels(inputs_), channels(outputs_), capacity_);
                const std::string name = "/osc-" + std::to_string(getpid()) + "-" + std::to_string(Process::counter.fetch_add(1));
                try
                {
                    shm_ = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
                    if(shm_ < 0) throw std::runtime_error("Cannot create worker shared memory");
                    shm_unlink(name.c_str());
                    fcntl(shm_, F_SETFD, FD_CLOEXEC); // other plugins' workers must not inherit it
                    if(ftruncate(shm_, off_t(bytes_)) != 0) throw std::runtime_error("Cannot size worker shared memory");
                    void* memory = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, shm_, 0);
                    if(memory == MAP_FAILED) throw std::runtime_error("Cannot map worker shared memory");
                    shared_ = new(memory) Shared();
                    shared_->inputChannels = channels(inputs_); shared_->outputChannels = channels(outputs_);
                    shared_->maxFrames = capacity_; shared_->sampleRate = process_->rate; shared_->parameterCount = uint32_t(parameters_.size());
                    for(uint32_t i = 0; i < parameters_.size(); ++i) shared_->parameterValues[i].store(parameters_[i].defaultValue);
                    connect();
                    output_.resize(maxOutputEvents);
                    pending_.reserve(4096);
                    fadeFrames_ = faded_ = uint32_t(process_->rate * .01);
                    // The sounds present from the start keep their IDs.
                    if(contexts_) for(const auto& c : contexts()) remember(c, c.id);
                }
                catch(...) { release(); throw; }
                std::lock_guard lock(process_->nodesMutex);
                process_->nodes.push_back(this);
            }

            ~WorkerHost() override
            {
                {
                    std::lock_guard control(process_->control);
                    { std::lock_guard lock(process_->nodesMutex); std::erase(process_->nodes, this); }
                    if(!process_->failed.load() && process_->socket >= 0)
                        try { process_->exchange(header(Op::DestroyNode).data); } catch(const std::exception&) {}
                }
                release();
            }

            // A restarted worker described this node again: attach it to the same shared block, the
            // new worker starting idle. Under control.
            void reconnect(Reader& reply)
            {
                node_ = reply.u32();
                if(reply.data.substr(reply.at) != description_) throw std::runtime_error("the plugin changed");
                auto& s = *shared_;
                s.stop.store(0); s.panic.store(0); s.crashed.store(0);
                s.request.store(requested_.load()); s.done.store(requested_.load());
                connect();
            }

            // After a restart: the recovery record (state or snapshot), then the prepared sounds made
            // since and their selections, then the parameter values the old worker last published.
            // Under control.
            void restore()
            {
                if(!record_.empty()) process_->exchange(header(snapshotRecord_ ? Op::LoadSnapshot : Op::LoadState).str(record_.string()).data);
                if(contexts_)
                {
                    const auto present = contexts();
                    if(present.size() != base_.size()) throw std::runtime_error("the restored sounds differ");
                    std::map<uint32_t, uint32_t> map;
                    for(size_t i = 0; i < base_.size(); ++i) map[base_[i]] = present[i].id;
                    for(const auto id : discarded_) process_->exchange(header(Op::DiscardSound).u32(map.at(id)).u8(1).data);
                    for(const auto& p : prepared_)
                    {
                        const auto worker = prepare(p);
                        if(worker == CLAP_INVALID_ID) throw std::runtime_error("a prepared sound was rejected");
                        map[p.id] = worker;
                    }
                    for(const auto id : discarded_) map.erase(id);
                    ids_ = map;
                    for(int16_t c = 0; c < 16; ++c) if(auto i = ids_.find(selected_[c]); i != ids_.end())
                        process_->exchange(header(Op::SelectSound).u32(i->second).u32(uint32_t(c)).data);
                    fresh_ = true;
                }
                // Only values that differ from what the new worker published, to stay within its event queue.
                for(size_t i = 0; i < values_.size(); ++i)
                    if(!(parameters_[i].flags & CLAP_PARAM_IS_READONLY) && shared_->parameterValues[i].load() != values_[i])
                        process_->exchange(header(Op::SetParameter).u32(parameters_[i].id).f64(values_[i]).data);
            }

            void capture() { values_.resize(parameters_.size()); for(size_t i = 0; i < values_.size(); ++i) values_[i] = shared_->parameterValues[i].load(); }
            bool busy() const { return busy_.load(); }
            bool isRoot() const { return root_; }
            // The supervisor's per-node checks: a worker marked as crashing, a block left unanswered
            // past the hang limit, or too many late ones in a row.
            std::string trouble(int64_t now) const
            {
                if(const auto signal = shared_->crashed.load()) return "Plugin worker crashed (signal " + std::to_string(signal) + ")";
                const auto since = requestedAt_.load(std::memory_order_acquire);
                const bool waiting = shared_->done.load(std::memory_order_acquire) != requested_.load(std::memory_order_acquire);
                if(waiting && since && std::chrono::nanoseconds(now - since) > process_->hangLimit)
                    return "Plugin hung: a block was not finished within " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(process_->hangLimit).count()) + " ms";
                if(tooSlow_.load(std::memory_order_acquire)) return "Plugin too slow: " + std::to_string(process_->missLimit) + " blocks in a row missed their deadline";
                return {};
            }
            void recovered() { tooSlow_.store(false); requestedAt_.store(0); }

            std::string failure() const override { return process_->failure(); }

            // Like the in-engine host's, never throws: a worker that fails here is the supervisor's to
            // handle, and throwing would stop the graph servicing its other nodes.
            void service() override
            {
                Reader r;
                try { r = request(Op::Service); } catch(const std::exception&) { return; }
                pending_.clear(); next_ = 0;
                for(auto n = r.u32(); n--;) { kick::clap::Event e; r.bytes(e.bytes.data(), e.bytes.size()); pending_.push_back(e); }
            }
            bool supportsState() const noexcept override { return state_; }
            void saveState(const std::string& directory) override { request(Op::SaveState, Writer().str(directory)); }
            void loadState(const std::string& directory) override
            {
                std::lock_guard lock(controlFor());
                process_->exchange(header(Op::LoadState).str(directory).data);
                record(Op::SaveState);
            }
            void saveSnapshot(const std::string& directory) override { request(Op::SaveSnapshot, Writer().str(directory)); }
            std::vector<PresetInfo> presets() override
            {
                Reader r = request(Op::Presets);
                std::vector<PresetInfo> result(r.u32());
                for(auto& p : result) { p.uri = r.str(); p.name = r.str(); }
                return result;
            }
            // A preset changes the plugin's state, so the recovery record is saved again.
            void loadPreset(const std::string& uri) override
            {
                std::lock_guard lock(controlFor());
                process_->exchange(header(Op::LoadPreset).str(uri).data);
                record(Op::SaveState);
            }
            // Only on a host not yet playing, so the record can be saved at once.
            void loadSnapshot(const std::string& directory) override
            {
                std::lock_guard lock(controlFor());
                process_->exchange(header(Op::LoadSnapshot).str(directory).data);
                if(contexts_)
                {
                    // The snapshot's sounds have fresh IDs.
                    ids_.clear(); base_.clear(); prepared_.clear(); discarded_.clear(); selected_.fill(UINT32_MAX);
                    for(const auto& c : contexts()) remember(c, allocate(c.id));
                }
                record(Op::SaveSnapshot);
            }
            std::shared_ptr<PluginHost> createEffectContext() override
            {
                if(!effects_) throw std::runtime_error("Plugin has no native effect contexts");
                std::lock_guard lock(controlFor());
                Reader r = process_->exchange(header(Op::CreateEffectContext).data);
                return std::make_shared<WorkerHost>(process_, r, false);
            }
            std::optional<oscairn_effect_resources_t> effectResources() const override
            {
                if(!effects_) return {};
                Reader r = const_cast<WorkerHost*>(this)->request(Op::EffectResources);
                if(!r.u8()) return {};
                oscairn_effect_resources_t res{}; r.bytes(&res, sizeof res); return res;
            }
            uint32_t soundContextCapacity() const noexcept override { return contexts_; }
            std::vector<oscairn_sound_context_info_t> soundContexts() const override
            {
                if(!contexts_) return {};
                auto& self = *const_cast<WorkerHost*>(this);
                std::lock_guard lock(self.controlFor());
                std::vector<oscairn_sound_context_info_t> result;
                for(auto c : self.contexts())
                    for(const auto& [engine, worker] : ids_) if(worker == c.id) { c.id = engine; result.push_back(c); break; }
                return result;
            }
            uint32_t prepareSound(const std::string& name, const std::vector<oscairn_sound_parameter_t>& values, const std::string& state, const std::string& preset,
                                  const std::string& sfz, const std::string& library, const std::string& directory) override
            {
                std::lock_guard lock(controlFor());
                Prepared p{0, name, values, state, preset, sfz, library, directory};
                const auto worker = prepare(p);
                if(!contexts_ || worker == CLAP_INVALID_ID) return worker;
                p.id = allocate(worker); ids_[p.id] = worker;
                prepared_.push_back(std::move(p));
                return prepared_.back().id;
            }
            bool selectSound(uint32_t id, int16_t channel) override
            {
                std::lock_guard lock(controlFor());
                const auto i = ids_.find(id);
                if(contexts_ && i == ids_.end()) { if(channel < -1 || channel > 15) throw std::invalid_argument("Invalid sound channel"); return false; }
                if(!process_->exchange(header(Op::SelectSound).u32(contexts_ ? i->second : id).u32(uint32_t(channel)).data).u8()) return false;
                if(channel < 0) selected_.fill(id); else selected_[size_t(channel)] = id;
                return true;
            }
            bool discardSound(uint32_t id, bool cutTail) override
            {
                std::lock_guard lock(controlFor());
                const auto i = ids_.find(id);
                if(contexts_ && i == ids_.end()) return false;
                if(!process_->exchange(header(Op::DiscardSound).u32(contexts_ ? i->second : id).u8(cutTail).data).u8()) return false;
                ids_.erase(id);
                if(std::erase_if(prepared_, [&](const auto& p) { return p.id == id; }) == 0) discarded_.push_back(id);
                return true;
            }
            void setAssets(kick::clap::AssetResolver* assets) noexcept override { process_->assets.store(assets); }
            std::vector<kick::clap::AssetReference> assetDependencies() const override
            {
                Reader r = const_cast<WorkerHost*>(this)->request(Op::AssetDependencies);
                std::vector<kick::clap::AssetReference> refs(r.u32());
                for(auto& a : refs) { a.library = r.str(); a.path = r.str(); a.sha256 = r.str(); }
                return refs;
            }
            const kick::clap::Descriptor& descriptor() const noexcept override { return descriptor_; }
            const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return parameters_; }
            const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return inputs_; }
            const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return outputs_; }
            uint32_t noteInputPorts() const noexcept override { return noteInputs_; }
            uint32_t noteOutputPorts() const noexcept override { return noteOutputs_; }
            bool usesClapNotes(uint32_t port) const noexcept override { return port < clapNotes_.size() && clapNotes_[port]; }
            // While down, a panic would reach the restarted worker and drop its first block's events.
            void panic() noexcept override { if(!process_->failed.load()) shared_->panic.store(1, std::memory_order_release); }
            double parameterValue(clap_id id) const noexcept override
            {
                const auto i = index_.find(id);
                return i == index_.end() ? 0.0 : shared_->parameterValues[i->second].load(std::memory_order_relaxed);
            }
            std::string parameterText(clap_id id, double value) const override
            {
                return const_cast<WorkerHost*>(this)->request(Op::ParameterText, Writer().u32(id).f64(value)).str();
            }
            bool setParameter(clap_id id, double value) noexcept override
            {
                try { return request(Op::SetParameter, Writer().u32(id).f64(value)).u8(); } catch(const std::exception&) { return false; }
            }
            bool parameterGesture(clap_id id, bool begin) noexcept override
            {
                try { return request(Op::Gesture, Writer().u32(id).u8(begin)).u8(); } catch(const std::exception&) { return false; }
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
                s.restarts = process_->restarted.load(std::memory_order_relaxed);
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
                const double rate = process_->rate;
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
                            position.song_pos_seconds += clap_sectime((double(done) / rate) * CLAP_SECTIME_FACTOR);
                        if((position.flags & CLAP_TRANSPORT_IS_PLAYING) && (position.flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (position.flags & CLAP_TRANSPORT_HAS_TEMPO))
                            position.song_pos_beats += clap_beattime((double(done) / rate) * position.tempo / 60. * CLAP_BEATTIME_FACTOR);
                    }
                    ok = block(inputs ? in : nullptr, ni, out, no, n, chunk_.data(), chunkCount, transport ? &position : nullptr, done) && ok;
                    done += n;
                }
                return ok;
            }

        private:
            struct Prepared
            {
                uint32_t id; // the engine's ID for it
                std::string name; std::vector<oscairn_sound_parameter_t> values;
                std::string state, preset, sfz, library, directory;
            };

            std::mutex& controlFor()
            {
                if(process_->failed.load()) throw std::runtime_error(process_->failure());
                return process_->control;
            }
            Writer header(Op op) const { return Writer().u8(uint8_t(op)).u32(node_); }
            Reader request(Op op, const Writer& args = {})
            {
                std::lock_guard lock(controlFor());
                return process_->exchange(header(op).data + args.data);
            }
            // The worker's sound contexts with its own IDs, in slot order. Under control.
            std::vector<oscairn_sound_context_info_t> contexts()
            {
                Reader r = process_->exchange(header(Op::SoundContexts).data);
                std::vector<oscairn_sound_context_info_t> result(r.u32());
                for(auto& c : result) r.bytes(&c, sizeof c);
                return result;
            }
            uint32_t prepare(const Prepared& p)
            {
                Writer w = header(Op::PrepareSound); w.str(p.name).u32(uint32_t(p.values.size()));
                for(const auto& v : p.values) w.u32(v.id).f64(v.value);
                w.str(p.state).str(p.preset).str(p.sfz).str(p.library).str(p.directory);
                return process_->exchange(w.data).u32();
            }
            // Engine sound IDs: the worker's own until a restart, then new ones; never reused.
            uint32_t allocate(uint32_t worker) { const auto id = fresh_ ? nextId_ : worker; nextId_ = std::max(nextId_, id + 1); return id; }
            void remember(const oscairn_sound_context_info_t& c, uint32_t id)
            {
                ids_[id] = c.id; base_.push_back(id); nextId_ = std::max(nextId_, id + 1);
                for(size_t ch = 0; ch < 16; ++ch) if(c.selected_channels & (1u << ch)) selected_[ch] = id;
            }
            // The recovery record, saved by the engine each time state or a snapshot is loaded. If a
            // save fails, the previous record stays. Under control.
            void record(Op save)
            {
                if(save == Op::SaveState && !state_) return;
                try
                {
                    const auto next = process_->records / std::to_string(++process_->saves);
                    process_->exchange(header(save).str(next.string()).data);
                    std::error_code ignored;
                    if(!record_.empty()) std::filesystem::remove_all(record_, ignored);
                    record_ = next; snapshotRecord_ = save == Op::SaveSnapshot;
                }
                catch(const std::exception&) {}
            }
            // Hands the worker this node's shared block, which it maps, then answers. Under control.
            void connect()
            {
                send(process_->socket, Writer().u8(1).data);
                sendDescriptor(process_->socket, shm_);
                Process::expect(receive(process_->socket));
            }
            void release() noexcept
            {
                if(shared_) { munmap(shared_, bytes_); shared_ = nullptr; }
                if(shm_ >= 0) { close(shm_); shm_ = -1; }
                std::error_code ignored;
                if(!record_.empty()) std::filesystem::remove_all(record_, ignored);
            }

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
                // While busy, a restart waits; it sets failed first, so a block that starts later sees it.
                busy_.store(true);
                struct Idle { std::atomic<bool>& busy; ~Idle() { busy.store(false); } } idle{busy_};
                auto& s = *shared_;
                if(process_->failed.load() || frames > s.maxFrames || count > maxEvents || (count && !events) || (no && !outputs)) return silence();
                if(const auto g = process_->generation.load(); g != generation_) { generation_ = g; faded_ = 0; consecutiveMisses_ = 0; } // restarted
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
                const auto budget = process_->budget;
                if(!waitWhileEqual(s.done, sequence_ - 1, budget) || s.done.load(std::memory_order_acquire) != sequence_)
                {
                    missed();
                    return silence();
                }
                // Late by the clock counts toward "too slow" even when the wait itself woke late.
                if(std::chrono::steady_clock::now() - sent > budget) missed();
                else consecutiveMisses_ = 0;
                for(uint32_t c = 0; c < no; ++c)
                {
                    if(!outputs[c]) continue;
                    if(c < s.outputChannels) std::copy_n(s.output(c), frames, outputs[c]);
                    else std::fill_n(outputs[c], frames, 0.f);
                    for(uint32_t f = 0; f < frames && faded_ + f < fadeFrames_; ++f) outputs[c][f] *= float(faded_ + f) / float(fadeFrames_);
                }
                faded_ = std::min(fadeFrames_, faded_ + frames);
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
                if(++consecutiveMisses_ >= process_->missLimit) tooSlow_.store(true, std::memory_order_release);
            }

            std::shared_ptr<Process> process_;
            bool root_;
            uint32_t node_ = 0; // the worker's index for this node; under control
            std::string description_;
            kick::clap::Descriptor descriptor_;
            std::vector<kick::clap::Parameter> parameters_;
            std::unordered_map<clap_id, uint32_t> index_;
            std::vector<kick::clap::Port> inputs_, outputs_;
            uint32_t noteInputs_ = 0, noteOutputs_ = 0, contexts_ = 0;
            std::vector<uint8_t> clapNotes_;
            bool state_ = false, effects_ = false;
            int shm_ = -1;
            Shared* shared_ = nullptr; size_t bytes_ = 0;
            uint32_t capacity_ = 0;
            // Recovery record, under control: state or snapshot, the sounds prepared and discarded
            // since, selections and parameter values.
            std::filesystem::path record_;
            bool snapshotRecord_ = false, fresh_ = false;
            std::map<uint32_t, uint32_t> ids_; // engine sound ID -> worker sound ID
            std::vector<uint32_t> base_, discarded_; // the record's sounds, in slot order; those discarded since
            std::vector<Prepared> prepared_;
            std::array<uint32_t, 16> selected_ = [] { std::array<uint32_t, 16> a; a.fill(UINT32_MAX); return a; }();
            uint32_t nextId_ = 1;
            std::vector<double> values_;
            // Audio thread, and what the supervisor reads of it.
            std::atomic<uint32_t> requested_{0};
            std::atomic<int64_t> requestedAt_{0};
            std::atomic<bool> tooSlow_{false}, busy_{false};
            std::atomic<uint64_t> workerMisses_{0};
            uint32_t sequence_ = 0, outputCount_ = 0, consecutiveMisses_ = 0, generation_ = 0, faded_ = 0, fadeFrames_ = 0;
            std::array<kick::clap::Event, maxEvents> chunk_{};
            std::vector<kick::clap::Event> output_, pending_;
            size_t next_ = 0;
        };

        // Watches the worker: a crash, a block left unanswered past the hang limit, or too many
        // missed blocks in a row stop it and silence its nodes until it is restarted.
        void Process::supervise()
        {
            std::unique_lock lock(superviseMutex);
            while(!stopping)
            {
                wakeSupervisor.wait_for(lock, std::chrono::milliseconds(10));
                if(stopping || gaveUp) continue;
                if(failed.load()) { restart(); continue; }
                int status = 0;
                if(waitpid(pid, &status, WNOHANG) == pid)
                {
                    reaped = true;
                    fail(WIFSIGNALED(status) ? "Plugin worker crashed (signal " + std::to_string(WTERMSIG(status)) + ")"
                                             : "Plugin worker exited (status " + std::to_string(WEXITSTATUS(status)) + ")");
                    continue;
                }
                const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
                std::string why;
                { std::lock_guard nodesLock(nodesMutex); for(auto* n : nodes) if(why = n->trouble(now); !why.empty()) break; }
                if(!why.empty()) fail(why);
            }
        }

        // Brings a failed worker back: a new process with the same plugin and its effect contexts,
        // each node restored and faded in. After maxRestarts within a minute the nodes stay silent.
        void Process::restart()
        {
            const auto now = std::chrono::steady_clock::now();
            std::erase_if(restarts, [&](auto t) { return now - t > std::chrono::minutes(1); });
            auto report = [&](const std::string& text) { std::lock_guard lock(failureMutex); failureText = reason + "; " + text; };
            if(restarts.size() >= maxRestarts)
            {
                gaveUp = true;
                report("not restarted: " + std::to_string(maxRestarts) + " restarts within a minute");
                return;
            }
            restarts.push_back(now);
            std::lock_guard controlLock(control); // the nodes stay as they are while restarting
            const auto list = [&] { std::lock_guard l(nodesMutex); return nodes; }();
            for(auto* n : list) while(n->busy()) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // audio leaves its last block
            for(auto* n : list) n->capture();
            try
            {
                if(socket >= 0) { close(socket); socket = -1; }
                Reader r = start();
                const auto rootNode = std::find_if(list.begin(), list.end(), [](auto* n) { return n->isRoot(); });
                if(rootNode != list.end()) (*rootNode)->reconnect(r);
                else
                {
                    // The root node is gone; a new one is needed only to create the contexts from.
                    r.u32();
                    if(r.data.substr(r.at) != root) throw std::runtime_error("the plugin changed");
                    send(socket, Writer().u8(0).data); expect(receive(socket));
                }
                for(auto* n : list) if(!n->isRoot()) { Reader c = exchange(Writer().u8(uint8_t(Op::CreateEffectContext)).u32(0).data); n->reconnect(c); }
                if(rootNode == list.end()) exchange(Writer().u8(uint8_t(Op::DestroyNode)).u32(0).data);
                for(auto* n : list) n->restore();
            }
            catch(const std::exception& e)
            {
                if(pid > 0 && !reaped) { kill(pid, SIGKILL); int status = 0; waitpid(pid, &status, 0); reaped = true; }
                report(std::string("restart failed: ") + e.what()); // tried again on the next check
                return;
            }
            for(auto* n : list) n->recovered();
            { std::lock_guard lock(failureMutex); failureText.clear(); }
            generation.fetch_add(1);
            restarted.fetch_add(1, std::memory_order_relaxed);
            failed.store(false);
        }
    }

    std::shared_ptr<PluginHost> loadPluginInWorker(const std::string& program, const std::string& path, const std::string& id,
                                                   double sampleRate, uint32_t maxFrames, ProcessingMode mode)
    {
        auto process = std::make_shared<Process>(program, path, id, sampleRate, maxFrames, mode);
        std::shared_ptr<PluginHost> host;
        {
            std::lock_guard lock(process->control);
            Reader r = process->start();
            process->root = r.data.substr(r.at + 4);
            host = std::make_shared<WorkerHost>(process, r, true);
        }
        process->supervisor = std::thread([p = process.get()] { p->supervise(); });
        return host;
    }
}

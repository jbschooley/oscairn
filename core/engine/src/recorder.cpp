// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/recorder.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace oscairn
{
    namespace fs = std::filesystem;
    static_assert(std::endian::native == std::endian::little, "WAV samples are written as the engine holds them");

    struct Recorder::Active
    {
        struct Ring
        {
            std::vector<float> data;  // interleaved, `capacity` frames
            std::vector<float> block; // planar, one block: where the audio thread sums a tap
            uint32_t channels = 0;
            uint64_t capacity = 0;
            std::atomic<uint64_t> written{0}; // frames, from the take's start
            ~Ring()
            {
#ifndef _WIN32
                if(!data.empty()) munlock(data.data(), data.size() * sizeof(float));
#endif
            }
        };
        struct Note { uint64_t frame = 0; uint16_t track = 0; uint8_t size = 0, bytes[3]{}; }; // size 0: a tempo, bytes its microseconds per quarter
        uint32_t id = 0;
        fs::path folder;
        std::string description;
        std::vector<std::string> names;   // the audio taps'
        std::vector<std::string> tracks;  // the note taps'
        std::vector<std::unique_ptr<Ring>> rings;
        std::vector<Note> notes = std::vector<Note>(noteCapacity);
        std::atomic<uint64_t> noteHead{0}, noteTail{0}, dropped{0};
        // Set by the audio thread before `started`.
        uint64_t start = 0;
        double tempo = 0, beat = 0;
        std::atomic<bool> started{false}, ended{false};
        std::atomic<uint64_t> endFrames{0};
        uint64_t frames = 0;   // audio thread: copied so far
        uint32_t quarter = 0;  // audio thread: the tempo last noted, in microseconds per quarter
        std::atomic<bool> abort{false}, done{false};
        std::thread writer;
        uint64_t bytes = 0;    // committed
        mutable std::mutex mutex; // the writer's and the control thread's
        Take status;
    };

    namespace
    {
        using Active = Recorder::Active;
        std::string quote(const std::string& s)
        {
            std::string r = "\"";
            for(const unsigned char c : s)
                if(c == '"' || c == '\\') { r += '\\'; r += char(c); }
                else if(c < 0x20) { char u[8]; std::snprintf(u, sizeof u, "\\u%04x", c); r += u; }
                else r += char(c);
            return r + '"';
        }
        // Characters FAT and exFAT refuse (RFC 0020: SD cards and USB drives use them).
        std::string safeName(std::string s)
        {
            for(auto& c : s) if(static_cast<unsigned char>(c) < 0x20 || std::strchr("<>:\"/\\|?*", c)) c = '_';
            while(!s.empty() && (s.back() == ' ' || s.back() == '.')) s.pop_back();
            return s.empty() ? "Tap" : s;
        }
        void addOverrun(Recorder::Take& t, uint64_t start, uint64_t frames)
        {
            t.overruns.push_back({start, frames});
            std::sort(t.overruns.begin(), t.overruns.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
            std::vector<Recorder::Overrun> merged;
            for(const auto& o : t.overruns)
                if(!merged.empty() && o.start <= merged.back().start + merged.back().frames)
                    merged.back().frames = std::max(merged.back().start + merged.back().frames, o.start + o.frames) - merged.back().start;
                else merged.push_back(o);
            t.overruns = std::move(merged);
        }

        // A 32-bit float WAV file written as it grows; the header's sizes are rewritten as it goes.
        struct Wav
        {
            FILE* file = nullptr;
            uint32_t channels = 0;
            uint64_t frames = 0;
            bool header(double sampleRate)
            {
                const uint64_t bytes = frames * channels * 4;
                // shortcut: sizes saturate past 4 GiB; RF64 (RFC 0020 step 1) is not written yet.
                const auto data = uint32_t(std::min<uint64_t>(bytes, 0xFFFFFFFFull - 50));
                uint8_t h[58];
                auto put = [&](size_t at, uint32_t v, int n) { for(int i = 0; i < n; ++i) h[at + i] = uint8_t(v >> (8 * i)); };
                std::memcpy(h, "RIFF", 4); put(4, 50 + data, 4); std::memcpy(h + 8, "WAVEfmt ", 8); put(16, 18, 4);
                put(20, 3, 2); put(22, channels, 2); put(24, uint32_t(sampleRate), 4); put(28, uint32_t(sampleRate) * channels * 4, 4);
                put(32, channels * 4, 2); put(34, 32, 2); put(36, 0, 2);
                std::memcpy(h + 38, "fact", 4); put(42, 4, 4); put(46, uint32_t(std::min<uint64_t>(frames, 0xFFFFFFFFull)), 4);
                std::memcpy(h + 50, "data", 4); put(54, data, 4);
                return std::fseek(file, 0, SEEK_SET) == 0 && std::fwrite(h, 1, sizeof h, file) == sizeof h && std::fseek(file, 0, SEEK_END) == 0;
            }
        };

        void writeVarLen(std::string& out, uint32_t v)
        {
            uint8_t b[5]; int n = 0;
            do { b[n++] = uint8_t(v & 0x7F); v >>= 7; } while(v);
            while(n--) out += char(b[n] | (n ? 0x80 : 0));
        }
        void put32be(std::string& out, uint32_t v) { for(int i = 3; i >= 0; --i) out += char(v >> (8 * i)); }

        // A Standard MIDI File, type 1, 960 ticks per quarter (RFC 0020): track 0 the tempo map,
        // then one track per note tap. Ticks follow the tempos noted during the take.
        bool writeMidi(const fs::path& file, const std::vector<std::string>& names, const std::vector<Active::Note>& notes, double sampleRate)
        {
            constexpr double ppq = 960;
            struct Segment { uint64_t frame; double tick; uint32_t quarter; };
            std::vector<Segment> tempo;
            for(const auto& n : notes)
                if(!n.size)
                {
                    const uint32_t q = uint32_t(n.bytes[0]) << 16 | uint32_t(n.bytes[1]) << 8 | n.bytes[2];
                    const double tick = tempo.empty() ? 0 : tempo.back().tick + double(n.frame - tempo.back().frame) * ppq * 1e6 / (double(tempo.back().quarter) * sampleRate);
                    tempo.push_back({n.frame, tick, q});
                }
            if(tempo.empty()) tempo.push_back({0, 0, 500000});
            auto tickOf = [&](uint64_t frame) {
                auto it = std::upper_bound(tempo.begin(), tempo.end(), frame, [](uint64_t f, const Segment& s) { return f < s.frame; });
                const auto& s = it == tempo.begin() ? tempo.front() : *std::prev(it);
                return uint32_t(std::llround(s.tick + (double(frame) - double(s.frame)) * ppq * 1e6 / (double(s.quarter) * sampleRate)));
            };
            auto track = [&](int index, const std::string& name) {
                std::string body;
                if(!name.empty()) { body += '\0'; body += "\xFF\x03"; writeVarLen(body, uint32_t(name.size())); body += name; }
                uint32_t last = 0;
                for(const auto& n : notes)
                {
                    if(index < 0 ? n.size != 0 : (n.size == 0 || n.track != index)) continue;
                    const auto tick = std::max(tickOf(n.frame), last);
                    writeVarLen(body, tick - last); last = tick;
                    if(!n.size) { body += "\xFF\x51\x03"; body.append(reinterpret_cast<const char*>(n.bytes), 3); }
                    else body.append(reinterpret_cast<const char*>(n.bytes), n.size);
                }
                body += std::string("\0\xFF\x2F\0", 4);
                std::string chunk = "MTrk"; put32be(chunk, uint32_t(body.size()));
                return chunk + body;
            };
            std::string out = "MThd"; put32be(out, 6);
            out += std::string("\0\x01", 2); out += char((names.size() + 1) >> 8); out += char((names.size() + 1) & 0xFF);
            out += char(int(ppq) >> 8); out += char(int(ppq) & 0xFF);
            out += track(-1, {});
            for(size_t i = 0; i < names.size(); ++i) out += track(int(i), names[i]);
            std::ofstream f(file, std::ios::binary);
            f.write(out.data(), std::streamsize(out.size()));
            return bool(f);
        }

        void writeSidecar(const Active& a, double sampleRate, const Recorder::Take& t, std::optional<uint64_t> firstNote)
        {
            std::string files;
            for(size_t i = 0; i < a.names.size(); ++i)
                files += std::string(files.empty() ? "" : ",") + "{\"file\":" + quote(t.files[i]) + ",\"name\":" + quote(a.names[i]) +
                    ",\"channels\":" + std::to_string(a.rings[i]->channels) + '}';
            if(!a.tracks.empty())
            {
                std::string tracks;
                for(const auto& n : a.tracks) tracks += (tracks.empty() ? "" : ",") + quote(n);
                files += std::string(files.empty() ? "" : ",") + "{\"file\":" + quote(t.files.back()) + ",\"tracks\":[" + tracks + "]}";
            }
            std::string overruns;
            for(const auto& o : t.overruns) overruns += std::string(overruns.empty() ? "" : ",") + "[" + std::to_string(o.start) + "," + std::to_string(o.frames) + "]";
            const bool started = a.started.load(std::memory_order_acquire); // the audio thread sets tempo and beat before it
            char numbers[160];
            std::snprintf(numbers, sizeof numbers, "\"sampleRate\":%.17g,\"start\":%llu,\"frames\":%llu,\"tempo\":%.17g,\"beat\":%.17g", sampleRate,
                          (unsigned long long)t.start, (unsigned long long)t.frames, started ? a.tempo : 0.0, started ? a.beat : 0.0);
            const auto text = "{\"format\":\"oscairn.take\",\"version\":1,\"take\":" + std::to_string(t.take) + ",\"description\":" + quote(a.description) + ',' + numbers +
                ",\"state\":" + quote(t.state) + ",\"error\":" + quote(t.error) + ",\"files\":[" + files + "],\"overruns\":[" + overruns + "],\"droppedNotes\":" +
                std::to_string(t.droppedNotes) + ",\"firstNote\":" + (firstNote ? std::to_string(*firstNote) : "null") + "}\n";
            const auto temporary = a.folder / "take.json.tmp";
            { std::ofstream f(temporary, std::ios::binary); f << text; }
            std::error_code ignored;
            fs::rename(temporary, a.folder / "take.json", ignored);
        }

        // The writer thread: rings to files until the take has ended and everything is written.
        void write(Active& a, double sampleRate, uint32_t maxFrames, uint64_t reserve, const std::atomic<bool>* hold)
        {
            Recorder::Take status;
            { std::lock_guard lock(a.mutex); status = a.status; }
            std::vector<Wav> files(a.rings.size());
            std::string error;
            for(size_t i = 0; i < files.size() && error.empty(); ++i)
            {
                files[i].channels = a.rings[i]->channels;
                files[i].file = std::fopen((a.folder / status.files[i]).string().c_str(), "wb");
                if(!files[i].file || !files[i].header(sampleRate)) error = "cannot write " + status.files[i] + ": " + std::strerror(errno);
            }
            writeSidecar(a, sampleRate, status, {});
            const auto second = std::max<uint64_t>(uint64_t(sampleRate), maxFrames);
            std::vector<float> chunk;
            std::vector<uint64_t> read(files.size(), 0);
            std::vector<Active::Note> notes; // shortcut: a take's events stay in memory until it ends; stream them for takes of many hours
            std::optional<uint64_t> firstNote;
            auto lastHeader = std::chrono::steady_clock::now();
            auto writeFrames = [&](Wav& w, const float* samples, uint64_t frames) {
                const auto bytes = frames * w.channels * sizeof(float);
                std::error_code ec;
                if(const auto space = fs::space(a.folder, ec); !ec && space.available < reserve + bytes) { error = "diskFull"; return false; }
                if(std::fwrite(samples, 1, bytes, w.file) != bytes) { error = std::string("write failed: ") + std::strerror(errno); return false; }
                w.frames += frames;
                return true;
            };
            while(error.empty())
            {
                const bool abort = a.abort.load();
                if(hold->load() && !abort) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
                const bool ended = a.ended.load(std::memory_order_acquire);
                const uint64_t end = ended ? a.endFrames.load(std::memory_order_relaxed) : UINT64_MAX;
                bool moved = false, all = true;
                for(size_t i = 0; i < files.size() && error.empty(); ++i)
                {
                    auto& ring = *a.rings[i];
                    auto& pos = read[i];
                    const auto ch = ring.channels;
                    const auto chunkFrames = std::min(second, ring.capacity / 2); // large writes, well inside the ring
                    // The audio thread may be writing up to a block past its count: a frame is
                    // intact while it is more than a ring, less that block, behind the count.
                    auto lapped = [&](uint64_t written) { return written + maxFrames > ring.capacity ? written + maxFrames - ring.capacity : 0; };
                    const auto written = ring.written.load(std::memory_order_acquire);
                    const auto until = std::min(written, end);
                    if(until <= pos || (until - pos < chunkFrames && !ended && !abort && pos >= lapped(written))) continue;
                    if(const auto lost = std::min(lapped(written), until); pos < lost)
                    {
                        chunk.assign(size_t(std::min(lost - pos, chunkFrames)) * ch, 0.f);
                        const auto frames = chunk.size() / ch;
                        if(!writeFrames(files[i], chunk.data(), frames)) break;
                        addOverrun(status, pos, frames);
                        pos += frames; moved = true;
                        continue;
                    }
                    const auto frames = std::min(until - pos, chunkFrames);
                    chunk.resize(size_t(frames) * ch);
                    const auto at = pos % ring.capacity, first = std::min(frames, ring.capacity - at);
                    std::memcpy(chunk.data(), ring.data.data() + at * ch, size_t(first) * ch * sizeof(float));
                    std::memcpy(chunk.data() + first * ch, ring.data.data(), size_t(frames - first) * ch * sizeof(float));
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if(const auto torn = std::min(lapped(ring.written.load(std::memory_order_relaxed)), pos + frames); pos < torn)
                    {
                        std::fill_n(chunk.data(), size_t(torn - pos) * ch, 0.f);
                        addOverrun(status, pos, torn - pos);
                    }
                    if(!writeFrames(files[i], chunk.data(), frames)) break;
                    pos += frames; moved = true;
                }
                for(auto tail = a.noteTail.load(std::memory_order_relaxed); tail != a.noteHead.load(std::memory_order_acquire); a.noteTail.store(++tail, std::memory_order_release))
                {
                    notes.push_back(a.notes[tail % Recorder::noteCapacity]);
                    if(notes.back().size && !firstNote) firstNote = notes.back().frame;
                    moved = true;
                }
                for(size_t i = 0; i < files.size(); ++i) all = all && read[i] >= end;
                {
                    std::lock_guard lock(a.mutex);
                    status.state = a.status.state;
                    if(a.started.load(std::memory_order_acquire)) a.status.start = status.start = a.start;
                    status.frames = files.empty() ? (ended ? end : 0) : *std::min_element(read.begin(), read.end());
                    status.droppedNotes = a.dropped.load();
                    a.status.frames = status.frames; a.status.overruns = status.overruns; a.status.droppedNotes = status.droppedNotes;
                }
                if(const auto now = std::chrono::steady_clock::now(); now - lastHeader > std::chrono::seconds(2))
                {
                    lastHeader = now;
                    for(auto& w : files) if(!w.header(sampleRate) || std::fflush(w.file)) error = "write failed";
                }
                if((ended && all) || (abort && !moved)) break;
                if(!moved) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            for(auto& w : files)
                if(w.file)
                {
                    if(!w.header(sampleRate) && error.empty()) error = "write failed";
                    if(std::fclose(w.file) && error.empty()) error = "write failed";
                }
            if(!a.tracks.empty() && !writeMidi(a.folder / status.files.back(), a.tracks, notes, sampleRate) && error.empty()) error = "cannot write " + status.files.back();
            status.state = error.empty() ? "done" : "failed";
            status.error = error;
            status.droppedNotes = a.dropped.load();
            writeSidecar(a, sampleRate, status, firstNote);
            std::lock_guard lock(a.mutex);
            a.status = status;
        }
        void writer(std::shared_ptr<Active> take, double sampleRate, uint32_t maxFrames, uint64_t reserve, const std::atomic<bool>* hold)
        {
            try { write(*take, sampleRate, maxFrames, reserve, hold); }
            catch(const std::exception& e) { std::lock_guard lock(take->mutex); take->status.state = "failed"; take->status.error = e.what(); }
            take->done.store(true);
        }
    }

    Recorder::Recorder(double sampleRate, uint32_t maxFrames) : sampleRate_(sampleRate), maxFrames_(maxFrames) {}

    Recorder::~Recorder()
    {
        for(auto& a : live_) { a->abort.store(true); if(a->writer.joinable()) a->writer.join(); }
    }

    uint32_t Recorder::start(const fs::path& folder, const Taps& taps, const std::string& description)
    {
        if(recording_) throw std::logic_error("a take is already recording");
        if(taps.audio.empty() && taps.notes.empty()) throw std::invalid_argument("nothing to record");
        for(const auto& t : taps.audio) if(!t.channels || t.channels > Graph::maxAudioChannels) throw std::invalid_argument("a tap needs 1 to 256 channels");
        auto a = std::make_shared<Active>();
        a->id = nextTake_;
        a->folder = folder;
        a->description = description;
        fs::create_directories(folder);
        std::vector<std::string> files;
        auto unique = [&](std::string name, const char* extension) {
            name = safeName(name);
            auto file = name + extension;
            for(int n = 2; std::find(files.begin(), files.end(), file) != files.end(); ++n) file = name + " " + std::to_string(n) + extension;
            files.push_back(file);
        };
        const auto capacity = std::max<uint64_t>(uint64_t(std::ceil(ringSeconds_ * sampleRate_)), 4ull * maxFrames_);
        for(const auto& t : taps.audio)
        {
            auto ring = std::make_unique<Active::Ring>();
            ring->channels = t.channels;
            ring->capacity = capacity;
            ring->data.assign(size_t(capacity) * t.channels, 0.f);
            ring->block.assign(size_t(maxFrames_) * t.channels, 0.f);
#ifndef _WIN32
            mlock(ring->data.data(), ring->data.size() * sizeof(float)); // best effort: audio never depends on swap
#endif
            a->bytes += (ring->data.size() + ring->block.size()) * sizeof(float);
            a->rings.push_back(std::move(ring));
            a->names.push_back(t.name);
            unique(t.name, ".wav");
        }
        for(const auto& t : taps.notes) a->tracks.push_back(t.name);
        if(!taps.notes.empty()) unique("Notes", ".mid");
        a->bytes += a->notes.size() * sizeof(Active::Note);
        a->status = {a->id, "recording", folder.string(), {}, files, 0, 0, {}, 0};
        a->writer = std::thread(writer, a, sampleRate_, maxFrames_, reserve_, &hold_);
        committed_ += a->bytes;
        live_.push_back(a);
        publish(std::make_unique<Table>(Table{a, taps}));
        recording_ = nextTake_++;
        return recording_;
    }

    void Recorder::update(const Taps& taps)
    {
        if(!recording_) return;
        publish(std::make_unique<Table>(Table{live_.back(), taps}));
    }

    void Recorder::stop()
    {
        if(!recording_) return;
        { std::lock_guard lock(live_.back()->mutex); if(live_.back()->status.state == "recording") live_.back()->status.state = "finishing"; }
        publish(nullptr);
        recording_ = 0;
    }

    std::vector<Recorder::Take> Recorder::takes() const
    {
        auto out = finished_;
        for(const auto& a : live_) { std::lock_guard lock(a->mutex); out.push_back(a->status); }
        return out;
    }

    void Recorder::service()
    {
        const auto* now = table_.load();
        const auto* used = inUse_.load();
        std::erase_if(tables_, [&](const auto& t) { return t.get() != now && t.get() != used; });
        // The audio thread's last touch of a take is `ended`; the writer's is `done`.
        std::erase_if(live_, [&](const std::shared_ptr<Active>& a) {
            if(!a->done.load() || !a->ended.load()) return false;
            a->writer.join();
            finished_.push_back(a->status);
            committed_ -= a->bytes;
            return true;
        });
    }

    void Recorder::publish(std::unique_ptr<Table> table)
    {
        table_.store(table.get());
        if(table) tables_.push_back(std::move(table));
        const auto* now = table_.load();
        const auto* used = inUse_.load();
        std::erase_if(tables_, [&](const auto& t) { return t.get() != now && t.get() != used; });
    }

    // Audio thread ------------------------------------------------------------------------------

    void Recorder::begin(uint64_t time, double tempo, double beat) noexcept
    {
        Table* t;
        do { t = table_.load(); inUse_.store(t); } while(table_.load() != t);
        block_ = t;
        auto* take = t ? t->take.get() : nullptr;
        if(take != current_)
        {
            if(current_) { current_->endFrames.store(current_->frames, std::memory_order_relaxed); current_->ended.store(true, std::memory_order_release); } // its last touch
            current_ = take;
            if(take) { take->start = time; take->tempo = tempo; take->beat = beat; take->started.store(true, std::memory_order_release); }
        }
        if(!current_) return;
        const auto quarter = uint32_t(std::clamp(std::lround(60e6 / std::max(tempo, 1.0)), 1l, 0xFFFFFFl));
        if(quarter == current_->quarter) return;
        current_->quarter = quarter;
        auto& a = *current_;
        const auto head = a.noteHead.load(std::memory_order_relaxed);
        if(head - a.noteTail.load(std::memory_order_acquire) >= noteCapacity) { a.dropped.fetch_add(1, std::memory_order_relaxed); return; }
        a.notes[head % noteCapacity] = {a.frames, 0, 0, {uint8_t(quarter >> 16), uint8_t(quarter >> 8), uint8_t(quarter)}};
        a.noteHead.store(head + 1, std::memory_order_release);
    }

    void Recorder::note(uint16_t track, const kick::clap::Event& event) noexcept
    {
        const auto* h = event.header();
        if(h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        auto& a = *current_;
        Active::Note n{a.frames + h->time, track, 0, {}};
        auto value = [](double v) { return uint8_t(std::clamp(std::lround(v * 127), 0l, 127l)); };
        switch(h->type)
        {
            case CLAP_EVENT_NOTE_ON:
            case CLAP_EVENT_NOTE_OFF:
            {
                const auto& e = *reinterpret_cast<const clap_event_note_t*>(h);
                if(e.key < 0 || e.key > 127) { a.dropped.fetch_add(1, std::memory_order_relaxed); return; } // a wildcard has no MIDI form
                const bool on = h->type == CLAP_EVENT_NOTE_ON;
                n.size = 3; n.bytes[0] = uint8_t((on ? 0x90 : 0x80) | (e.channel < 0 ? 0 : e.channel & 15)); n.bytes[1] = uint8_t(e.key);
                n.bytes[2] = on ? std::max<uint8_t>(1, value(e.velocity)) : value(e.velocity);
                break;
            }
            case CLAP_EVENT_NOTE_EXPRESSION:
            {
                const auto& e = *reinterpret_cast<const clap_event_note_expression_t*>(h);
                if(e.expression_id != CLAP_NOTE_EXPRESSION_PRESSURE || e.key > 127) { a.dropped.fetch_add(1, std::memory_order_relaxed); return; }
                const auto channel = uint8_t(e.channel < 0 ? 0 : e.channel & 15);
                if(e.key < 0) { n.size = 2; n.bytes[0] = 0xD0 | channel; n.bytes[1] = value(e.value); }
                else { n.size = 3; n.bytes[0] = 0xA0 | channel; n.bytes[1] = uint8_t(e.key); n.bytes[2] = value(e.value); }
                break;
            }
            case CLAP_EVENT_NOTE_CHOKE: a.dropped.fetch_add(1, std::memory_order_relaxed); return;
            case CLAP_EVENT_MIDI:
            {
                const auto& e = *reinterpret_cast<const clap_event_midi_t*>(h);
                if(e.data[0] < 0x80 || e.data[0] >= 0xF0) return;
                const auto kind = e.data[0] & 0xF0;
                n.size = kind == 0xC0 || kind == 0xD0 ? 2 : 3;
                std::copy_n(e.data, 3, n.bytes);
                break;
            }
            default: return; // parameters, note ends: not notes
        }
        const auto head = a.noteHead.load(std::memory_order_relaxed);
        if(head - a.noteTail.load(std::memory_order_acquire) >= noteCapacity) { a.dropped.fetch_add(1, std::memory_order_relaxed); return; }
        a.notes[head % noteCapacity] = n;
        a.noteHead.store(head + 1, std::memory_order_release);
    }

    void Recorder::graphEvents(const kick::clap::Event* list, uint32_t count) noexcept
    {
        events(graphInput, list, count, false);
    }

    void Recorder::events(NodeId node, const kick::clap::Event* list, uint32_t count, bool emitted) noexcept
    {
        if(!current_ || !block_ || !count) return;
        const auto& taps = block_->taps.notes;
        for(size_t t = 0; t < taps.size() && t < current_->tracks.size(); ++t)
            if(taps[t].emitted == emitted && std::find(taps[t].nodes.begin(), taps[t].nodes.end(), node) != taps[t].nodes.end())
                for(uint32_t i = 0; i < count; ++i) note(uint16_t(t), list[i]);
    }

    void Recorder::end(const Graph& graph, const float* const* outputs, uint32_t outputChannels, uint32_t frames) noexcept
    {
        if(!current_ || !block_) return;
        auto& a = *current_;
        const auto& taps = block_->taps.audio;
        for(size_t i = 0; i < a.rings.size(); ++i)
        {
            auto& ring = *a.rings[i];
            for(uint32_t c = 0; c < ring.channels; ++c) std::fill_n(ring.block.data() + size_t(c) * maxFrames_, frames, 0.f);
            if(i < taps.size())
                for(const auto& s : taps[i].sources)
                {
                    if(s.to >= ring.channels) continue;
                    const float* from = s.node == graphOutput ? (outputs && s.channel < outputChannels ? outputs[s.channel] : nullptr) : graph.audioOutput(s.node, s.channel);
                    if(!from) continue;
                    auto* to = ring.block.data() + size_t(s.to) * maxFrames_;
                    for(uint32_t k = 0; k < frames; ++k) to[k] += from[k] * s.gain;
                }
            const auto w = ring.written.load(std::memory_order_relaxed);
            for(uint32_t k = 0; k < frames; ++k)
            {
                auto* frame = ring.data.data() + size_t((w + k) % ring.capacity) * ring.channels;
                for(uint32_t c = 0; c < ring.channels; ++c) frame[c] = ring.block[size_t(c) * maxFrames_ + k];
            }
            ring.written.store(w + frames, std::memory_order_release);
        }
        a.frames += frames;
    }
}

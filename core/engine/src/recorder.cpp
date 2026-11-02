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
#include <set>
#include <stdexcept>
#include <thread>
#ifndef _WIN32
#include <sys/mman.h>
#endif

namespace oscairn
{
    namespace fs = std::filesystem;
    static_assert(std::endian::native == std::endian::little, "WAV samples are written as the engine holds them");
    constexpr uint64_t unset = UINT64_MAX;

    struct Recorder::Ring
    {
        std::vector<float> data;  // interleaved, `capacity` frames; frame t of the timeline at t % capacity
        std::vector<float> block; // planar, one block: where the audio thread sums a tap
        uint32_t channels = 0;
        uint64_t capacity = 0;
        std::atomic<uint64_t> written{0};     // the timeline after the last frame written
        std::atomic<uint64_t> first{unset};   // the timeline's first frame written
        uint64_t bytes() const { return (data.size() + block.size()) * sizeof(float); }
        ~Ring()
        {
#ifndef _WIN32
            if(!data.empty()) munlock(data.data(), data.size() * sizeof(float));
#endif
        }
    };

    // Events on the timeline, overwritten oldest first; a reader checks the head again after
    // copying, as for the audio rings.
    struct Recorder::Notes
    {
        // size 0: a tempo, bytes its microseconds per quarter; size 4: an event with no MIDI form (counted).
        struct Note { uint64_t frame = 0; uint16_t track = 0; uint8_t size = 0, bytes[3]{}; };
        std::vector<Note> events = std::vector<Note>(noteCapacity);
        std::atomic<uint64_t> head{0};
        uint32_t quarter = 0; // audio thread: the tempo last noted
        static constexpr uint64_t bytes() { return noteCapacity * sizeof(Note); }
    };
    static_assert(sizeof(Recorder::Notes::Note) == 16, "the notes ring is sized at 16 bytes an event");

    struct Recorder::Player
    {
        uint32_t take = 0, channels = 0;
        std::vector<float> samples; // interleaved
        uint64_t frames = 0;
        std::atomic<uint64_t> position{0};
        std::atomic<bool> ended{false};
    };

    struct Recorder::Active
    {
        uint32_t id = 0;
        fs::path folder;
        std::string description;
        std::vector<std::string> names;   // the audio taps'
        std::vector<std::string> tracks;  // the note taps'
        std::vector<std::shared_ptr<Ring>> rings; // one per audio tap, the retroactive ones or its own
        std::shared_ptr<Notes> notes;
        Set own;            // control thread: the rings of its own and their taps
        std::vector<size_t> ownOf; // control thread: each own ring's tap among the take's
        bool fed = false;   // control thread: its own rings are in the table
        bool fresh = false; // rings of its own: it starts no earlier than they do
        bool back = false;  // reads the past: it starts no earlier than every ring still holds
        uint64_t length = 0;               // frames; 0: until stopped
        std::atomic<uint64_t> start{0};    // on the timeline; the writer may move it later, as above
        std::atomic<uint64_t> end{unset};  // on the timeline
        std::atomic<bool> settled{false};  // the writer has fixed the start
        double tempo = 0, beat = 0;        // at the start
        std::atomic<bool> abort{false}, done{false};
        std::thread writer;
        mutable std::mutex mutex; // the writer's and the control thread's
        Take status;
    };

    namespace
    {
        using Active = Recorder::Active;
        using Note = Recorder::Notes::Note;
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
        uint64_t capacityFor(double seconds, double sampleRate, uint32_t maxFrames) { return std::max<uint64_t>(uint64_t(std::ceil(seconds * sampleRate)), 4ull * maxFrames); }
        uint32_t quarterOf(double tempo) { return uint32_t(std::clamp(std::lround(60e6 / std::max(tempo, 1.0)), 1l, 0xFFFFFFl)); }

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
        // then one track per note tap. Ticks follow the tempos noted during the take; frames are
        // from the take's start.
        bool writeMidi(const fs::path& file, const std::vector<std::string>& names, const std::vector<Note>& notes, double sampleRate)
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
                    if(index < 0 ? n.size != 0 : (n.size == 0 || n.size == 4 || n.track != index)) continue;
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
            char numbers[160];
            std::snprintf(numbers, sizeof numbers, "\"sampleRate\":%.17g,\"start\":%llu,\"frames\":%llu,\"tempo\":%.17g,\"beat\":%.17g", sampleRate,
                          (unsigned long long)t.start, (unsigned long long)t.frames, a.tempo, a.beat);
            const auto text = "{\"format\":\"oscairn.take\",\"version\":1,\"take\":" + std::to_string(t.take) + ",\"description\":" + quote(a.description) + ',' + numbers +
                ",\"state\":" + quote(t.state) + ",\"error\":" + quote(t.error) + ",\"files\":[" + files + "],\"overruns\":[" + overruns + "],\"droppedNotes\":" +
                std::to_string(t.droppedNotes) + ",\"firstNote\":" + (firstNote ? std::to_string(*firstNote) : "null") + "}\n";
            const auto temporary = a.folder / "take.json.tmp";
            { std::ofstream f(temporary, std::ios::binary); f << text; }
            std::error_code ignored;
            fs::rename(temporary, a.folder / "take.json", ignored);
        }

        struct Context { double sampleRate; uint32_t maxFrames; uint64_t reserve; const std::atomic<bool>* hold; const std::atomic<uint64_t>* now; };

        // The writer thread: rings to files until the take has ended and everything is written.
        void write(Active& a, const Context& x)
        {
            const double sampleRate = x.sampleRate;
            const uint32_t maxFrames = x.maxFrames;
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
            // A frame is intact while it is more than a ring, less a block, behind the count: the
            // audio thread may be writing up to a block past it.
            auto lapped = [&](const Recorder::Ring& ring, uint64_t written) { return written + maxFrames > ring.capacity ? written + maxFrames - ring.capacity : 0; };
            // The start: no earlier than a ring of its own began, nor than the past every ring
            // still holds, less what the writer needs to begin before the audio thread laps it.
            uint64_t start = a.start.load();
            for(const auto& ring : a.rings)
            {
                while(ring->first.load(std::memory_order_acquire) == unset && a.end.load() == unset && !a.abort.load()) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                const auto first = ring->first.load(std::memory_order_acquire);
                if(first == unset) { start = std::max(start, a.end.load() == unset ? start : a.end.load()); continue; } // ended before it began
                if(a.fresh || a.back) start = std::max(start, first);
                if(a.back)
                {
                    const auto margin = std::min<uint64_t>(ring->capacity / 4, uint64_t(2 * sampleRate));
                    start = std::max(start, lapped(*ring, ring->written.load(std::memory_order_acquire) + margin));
                }
            }
            a.start.store(start);
            if(a.length) a.end.store(start + a.length);
            a.settled.store(true, std::memory_order_release);
            status.start = start;
            writeSidecar(a, sampleRate, status, {});
            const auto second = std::max<uint64_t>(uint64_t(sampleRate), maxFrames);
            std::vector<float> chunk;
            std::vector<uint64_t> read(files.size(), start);
            std::vector<Note> notes; // shortcut: a take's events stay in memory until it ends; stream them for takes of many hours
            std::optional<uint64_t> firstNote;
            uint64_t dropped = 0;
            uint32_t quarter = 0; // the tempo at the start, from the last tempo noted before it
            uint64_t tail = 0;
            if(a.notes) { const auto head = a.notes->head.load(std::memory_order_acquire); tail = head >= Recorder::noteCapacity ? head - Recorder::noteCapacity + 1 : 0; }
            auto lastHeader = std::chrono::steady_clock::now();
            auto writeFrames = [&](Wav& w, const float* samples, uint64_t frames) {
                const auto bytes = frames * w.channels * sizeof(float);
                std::error_code ec;
                if(const auto space = fs::space(a.folder, ec); !ec && space.available < x.reserve + bytes) { error = "diskFull"; return false; }
                if(std::fwrite(samples, 1, bytes, w.file) != bytes) { error = std::string("write failed: ") + std::strerror(errno); return false; }
                w.frames += frames;
                return true;
            };
            while(error.empty())
            {
                const bool abort = a.abort.load();
                if(x.hold->load() && !abort) { std::this_thread::sleep_for(std::chrono::milliseconds(2)); continue; }
                const auto endAt = a.end.load(std::memory_order_acquire);
                const bool ended = endAt != unset;
                const uint64_t end = ended ? std::max(endAt, start) : unset;
                const auto now = x.now->load(std::memory_order_acquire); // the notes before it are in the ring
                bool moved = false, all = true, behind = false;
                for(size_t i = 0; i < files.size() && error.empty(); ++i)
                {
                    auto& ring = *a.rings[i];
                    auto& pos = read[i];
                    const auto ch = ring.channels;
                    const auto chunkFrames = std::min(second, ring.capacity / 2); // large writes, well inside the ring
                    const auto written = ring.written.load(std::memory_order_acquire);
                    const auto until = std::min(written, end);
                    behind = behind || (written > pos && written - pos > ring.capacity / 2);
                    if(until <= pos || (until - pos < chunkFrames && !ended && !abort && pos >= lapped(ring, written))) continue;
                    if(const auto lost = std::min(lapped(ring, written), until); pos < lost)
                    {
                        chunk.assign(size_t(std::min(lost - pos, chunkFrames)) * ch, 0.f);
                        const auto frames = chunk.size() / ch;
                        if(!writeFrames(files[i], chunk.data(), frames)) break;
                        addOverrun(status, pos - start, frames);
                        pos += frames; moved = true;
                        continue;
                    }
                    const auto frames = std::min(until - pos, chunkFrames);
                    chunk.resize(size_t(frames) * ch);
                    const auto at = pos % ring.capacity, first = std::min(frames, ring.capacity - at);
                    std::memcpy(chunk.data(), ring.data.data() + at * ch, size_t(first) * ch * sizeof(float));
                    std::memcpy(chunk.data() + first * ch, ring.data.data(), size_t(frames - first) * ch * sizeof(float));
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if(const auto torn = std::min(lapped(ring, ring.written.load(std::memory_order_relaxed)), pos + frames); pos < torn)
                    {
                        std::fill_n(chunk.data(), size_t(torn - pos) * ch, 0.f);
                        addOverrun(status, pos - start, torn - pos);
                    }
                    if(!writeFrames(files[i], chunk.data(), frames)) break;
                    pos += frames; moved = true;
                }
                if(a.notes)
                {
                    const auto head = a.notes->head.load(std::memory_order_acquire);
                    if(head > tail + Recorder::noteCapacity - 1) { dropped += head - (Recorder::noteCapacity - 1) - tail; tail = head - (Recorder::noteCapacity - 1); }
                    const size_t from = notes.size(); const uint64_t copiedFrom = tail;
                    for(; tail < head; ++tail) notes.push_back(a.notes->events[tail % Recorder::noteCapacity]);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    // Those the audio thread may have overwritten while they were copied are lost.
                    if(const auto again = a.notes->head.load(std::memory_order_relaxed); again + 1 > copiedFrom + Recorder::noteCapacity)
                    {
                        const auto torn = std::min<uint64_t>(again + 1 - Recorder::noteCapacity - copiedFrom, notes.size() - from);
                        notes.erase(notes.begin() + std::ptrdiff_t(from), notes.begin() + std::ptrdiff_t(from + torn));
                        dropped += torn;
                    }
                    // Kept from the start on, as frames from it; the tempo before it is the take's first.
                    for(size_t i = from; i < notes.size(); ++i)
                        if(notes[i].frame < start && !notes[i].size) quarter = uint32_t(notes[i].bytes[0]) << 16 | uint32_t(notes[i].bytes[1]) << 8 | notes[i].bytes[2];
                    notes.erase(std::remove_if(notes.begin() + std::ptrdiff_t(from), notes.end(), [&](const Note& n) { return n.frame < start; }), notes.end());
                    moved = moved || notes.size() > from;
                }
                for(size_t i = 0; i < files.size(); ++i) all = all && read[i] >= end;
                if(a.notes || files.empty()) all = all && now >= end;
                {
                    std::lock_guard lock(a.mutex);
                    status.state = a.status.state;
                    if(status.state == "waiting" && now > start) a.status.state = status.state = "recording";
                    a.status.start = status.start;
                    status.frames = files.empty() ? (std::min(now, end) > start ? std::min(now, end) - start : 0) : *std::min_element(read.begin(), read.end()) - start;
                    uint64_t marked = 0;
                    for(const auto& n : notes) marked += n.size == 4 && n.frame < end;
                    status.droppedNotes = dropped + marked;
                    status.behind = behind;
                    a.status.frames = status.frames; a.status.overruns = status.overruns; a.status.droppedNotes = status.droppedNotes; a.status.behind = behind;
                }
                if(const auto t = std::chrono::steady_clock::now(); t - lastHeader > std::chrono::seconds(2))
                {
                    lastHeader = t;
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
            const auto end = std::max(a.end.load() == unset ? start + status.frames : a.end.load(), start);
            std::erase_if(notes, [&](const Note& n) { return n.frame >= end; });
            uint64_t marked = 0;
            for(auto& n : notes) { n.frame -= start; marked += n.size == 4; if(n.size && n.size != 4 && !firstNote) firstNote = n.frame; }
            notes.insert(notes.begin(), Note{0, 0, 0, {uint8_t((quarter ? quarter : quarterOf(a.tempo)) >> 16), uint8_t((quarter ? quarter : quarterOf(a.tempo)) >> 8), uint8_t(quarter ? quarter : quarterOf(a.tempo))}});
            if(!a.tracks.empty() && !writeMidi(a.folder / status.files.back(), a.tracks, notes, sampleRate) && error.empty()) error = "cannot write " + status.files.back();
            status.state = error.empty() ? "done" : "failed";
            status.error = error;
            status.droppedNotes = dropped + marked;
            status.behind = false;
            writeSidecar(a, sampleRate, status, firstNote);
            std::lock_guard lock(a.mutex);
            a.status = status;
        }
        void writer(std::shared_ptr<Active> take, Context x)
        {
            try { write(*take, x); }
            catch(const std::exception& e) { std::lock_guard lock(take->mutex); take->status.state = "failed"; take->status.error = e.what(); }
            take->done.store(true);
        }

        // A 32-bit float WAV, as the writer makes them (any chunk order), whole.
        std::vector<float> readWav(const fs::path& file, uint32_t& channels, double& rate)
        {
            std::ifstream in(file, std::ios::binary);
            char riff[12];
            if(!in.read(riff, 12) || std::memcmp(riff, "RIFF", 4) || std::memcmp(riff + 8, "WAVE", 4)) throw std::invalid_argument("not a WAV file: " + file.filename().string());
            uint16_t format = 0, bits = 0;
            channels = 0; rate = 0;
            for(char head[8]; in.read(head, 8);)
            {
                uint32_t size; std::memcpy(&size, head + 4, 4);
                if(!std::memcmp(head, "fmt ", 4))
                {
                    char f[16]{};
                    if(size < 16 || !in.read(f, 16)) break;
                    uint16_t c; uint32_t r;
                    std::memcpy(&format, f, 2); std::memcpy(&c, f + 2, 2); std::memcpy(&r, f + 4, 4); std::memcpy(&bits, f + 14, 2);
                    channels = c; rate = r;
                    in.seekg(std::streamoff(size - 16 + (size & 1)), std::ios::cur);
                }
                else if(!std::memcmp(head, "data", 4))
                {
                    if(format != 3 || bits != 32 || !channels) throw std::invalid_argument("not a 32-bit float WAV: " + file.filename().string());
                    const auto at = in.tellg();
                    in.seekg(0, std::ios::end);
                    const auto bytes = std::min<uint64_t>(size, uint64_t(in.tellg() - at)); // sizes saturate past 4 GiB
                    // shortcut: the whole file in memory, as the Sampler loads one (RFC 0020): for captures of seconds to minutes.
                    if(bytes > (1ull << 30)) throw std::invalid_argument("too long to play back here: " + file.filename().string());
                    std::vector<float> samples(size_t(bytes / 4 / channels * channels));
                    in.seekg(at);
                    in.read(reinterpret_cast<char*>(samples.data()), std::streamsize(samples.size() * 4));
                    return samples;
                }
                else in.seekg(std::streamoff(size + (size & 1)), std::ios::cur);
            }
            throw std::invalid_argument("not a WAV file: " + file.filename().string());
        }
    }

    Recorder::Recorder(double sampleRate, uint32_t maxFrames) : sampleRate_(sampleRate), maxFrames_(maxFrames) {}

    Recorder::~Recorder()
    {
        for(auto& a : live_) { a->abort.store(true); if(a->writer.joinable()) a->writer.join(); }
    }

    std::shared_ptr<Recorder::Ring> Recorder::ring(uint32_t channels, uint64_t capacity) const
    {
        auto r = std::make_shared<Ring>();
        r->channels = channels;
        r->capacity = capacity;
        r->data.assign(size_t(capacity) * channels, 0.f);
        r->block.assign(size_t(maxFrames_) * channels, 0.f);
#ifndef _WIN32
        mlock(r->data.data(), r->data.size() * sizeof(float)); // best effort: audio never depends on swap
#endif
        return r;
    }

    Recorder::Clock Recorder::clock() const noexcept
    {
        Clock c;
        for(uint32_t s;;)
        {
            s = clockSequence_.load(std::memory_order_acquire);
            if(s & 1) continue;
            c = {clockTime_.load(std::memory_order_relaxed), clockEnd_.load(std::memory_order_relaxed), clockTempo_.load(std::memory_order_relaxed),
                 clockBeat_.load(std::memory_order_relaxed), clockBar_.load(std::memory_order_relaxed), clockPlaying_.load(std::memory_order_relaxed)};
            std::atomic_thread_fence(std::memory_order_acquire);
            if(clockSequence_.load(std::memory_order_relaxed) == s) return c;
        }
    }

    uint64_t Recorder::now() const noexcept { return clockEnd_.load(std::memory_order_acquire); }

    uint64_t Recorder::retroBytes(double seconds, const Taps& taps, double sampleRate, uint32_t maxFrames)
    {
        if(seconds <= 0 || (taps.audio.empty() && taps.notes.empty())) return 0;
        const auto capacity = capacityFor(seconds, sampleRate, maxFrames);
        uint64_t n = taps.notes.empty() ? 0 : Notes::bytes();
        for(const auto& t : taps.audio) n += uint64_t(t.channels) * (capacity + maxFrames) * sizeof(float);
        return n;
    }

    void Recorder::setRetro(double seconds, const Taps& taps)
    {
        for(const auto& a : live_)
            if(!a->done.load() && ((a->notes && a->notes == retro_.notes) || std::any_of(a->rings.begin(), a->rings.end(), [&](const auto& r) { return std::count(retro_.rings.begin(), retro_.rings.end(), r) != 0; })))
                throw std::logic_error("a take recording reads the retroactive rings");
        for(const auto& t : taps.audio) if(!t.channels || t.channels > Graph::maxAudioChannels) throw std::invalid_argument("a tap needs 1 to 256 channels");
        Set next;
        if(seconds > 0 && (!taps.audio.empty() || !taps.notes.empty()))
        {
            const auto capacity = capacityFor(seconds, sampleRate_, maxFrames_);
            for(const auto& t : taps.audio) next.rings.push_back(ring(t.channels, capacity));
            if(!taps.notes.empty()) next.notes = std::make_shared<Notes>();
            next.taps = taps;
        }
        else seconds = 0;
        retro_ = std::move(next);
        retroSeconds_ = seconds;
        publish();
        recount();
    }

    void Recorder::updateRetro(const Taps& taps)
    {
        if(retro_.rings.empty() && !retro_.notes) return;
        for(size_t i = 0; i < retro_.taps.audio.size() && i < taps.audio.size(); ++i) retro_.taps.audio[i].sources = taps.audio[i].sources;
        for(size_t i = 0; i < retro_.taps.notes.size() && i < taps.notes.size(); ++i) retro_.taps.notes[i].nodes = taps.notes[i].nodes;
        publish();
    }

    uint32_t Recorder::start(const fs::path& folder, const Taps& taps, const std::string& description, const Start& when)
    {
        if(taps.audio.empty() && taps.notes.empty()) throw std::invalid_argument("nothing to record");
        for(const auto& t : taps.audio) if(!t.channels || t.channels > Graph::maxAudioChannels) throw std::invalid_argument("a tap needs 1 to 256 channels");
        // Where it starts on the timeline (RFC 0020, "Start and stop").
        const auto c = clock();
        const auto now = c.end;
        const double perBeat = 60 * sampleRate_ / std::max(c.tempo, 1.0);
        uint64_t start = now;
        using From = Start::From;
        if(when.from == From::SecondsAgo) start = now - std::min<uint64_t>(now, uint64_t(std::llround(std::max(0.0, when.seconds) * sampleRate_)));
        else if((when.from == From::NextBar || when.from == From::LastBar) && c.playing)
        {
            const double bar = std::max(1u, c.bar), beat = c.beat + double(now - c.time) / perBeat;
            const double k = when.from == From::NextBar ? std::ceil(beat / bar - 1e-9) : std::floor(beat / bar + 1e-9);
            start = uint64_t(std::max<int64_t>(0, int64_t(c.time) + std::llround((k * bar - c.beat) * perBeat)));
        }
        const bool back = start < now, ends = (back && !when.keepGoing) || when.until != Start::Until::Stop;
        if(recording_ && !ends) throw std::logic_error("a take is already recording");
        auto a = std::make_shared<Active>();
        a->id = nextTake_;
        a->folder = folder;
        a->description = description;
        a->back = back;
        a->tempo = c.tempo;
        a->beat = c.playing ? c.beat + (double(start) - double(c.time)) / perBeat : c.beat;
        if(when.until == Start::Until::Seconds) a->length = uint64_t(std::llround(std::max(0.0, when.length) * sampleRate_));
        else if(when.until == Start::Until::Bars) a->length = uint64_t(std::llround(std::max(0.0, when.length) * std::max(1u, c.bar) * perBeat));
        // Each tap from the retroactive rings when they hold it; else a ring of its own, which has no past.
        const auto capacity = capacityFor(ringSeconds_, sampleRate_, maxFrames_);
        for(const auto& t : taps.audio)
        {
            const auto held = std::find(retro_.taps.audio.begin(), retro_.taps.audio.end(), t);
            if(held != retro_.taps.audio.end()) { a->rings.push_back(retro_.rings[size_t(held - retro_.taps.audio.begin())]); continue; }
            if(back) throw std::out_of_range(t.name + " is not kept in the retroactive buffer");
            a->rings.push_back(ring(t.channels, capacity));
            a->own.rings.push_back(a->rings.back());
            a->own.taps.audio.push_back(t);
            a->ownOf.push_back(a->rings.size() - 1);
            a->fresh = true;
        }
        if(!taps.notes.empty())
        {
            if(retro_.notes && taps.notes == retro_.taps.notes) a->notes = retro_.notes;
            else if(back) throw std::out_of_range("those notes are not kept in the retroactive buffer");
            else { a->notes = a->own.notes = std::make_shared<Notes>(); a->own.taps.notes = taps.notes; }
        }
        fs::create_directories(folder);
        std::vector<std::string> files;
        auto unique = [&](std::string name, const char* extension) {
            name = safeName(name);
            auto file = name + extension;
            for(int n = 2; std::find(files.begin(), files.end(), file) != files.end(); ++n) file = name + " " + std::to_string(n) + extension;
            files.push_back(file);
        };
        for(const auto& t : taps.audio) { a->names.push_back(t.name); unique(t.name, ".wav"); }
        for(const auto& t : taps.notes) a->tracks.push_back(t.name);
        if(!taps.notes.empty()) unique("Notes", ".mid");
        a->start.store(start);
        if(back && !when.keepGoing && when.until == Start::Until::Stop) a->end.store(now);
        a->status = {a->id, start > now ? "waiting" : a->end.load() != unset ? "finishing" : "recording", folder.string(), {}, files, start, 0, {}, 0, sampleRate_, false, false};
        a->fed = !a->own.rings.empty() || a->own.notes;
        a->writer = std::thread(writer, a, Context{sampleRate_, maxFrames_, reserve_, &hold_, &clockEnd_});
        live_.push_back(a);
        publish();
        recount();
        if(!ends) recording_ = a->id;
        return nextTake_++;
    }

    void Recorder::update(const Taps& taps)
    {
        if(!recording_) return;
        const auto it = std::find_if(live_.begin(), live_.end(), [&](const auto& a) { return a->id == recording_; });
        if(it == live_.end()) return;
        auto& a = **it;
        // Its own taps; the retroactive ones follow updateRetro.
        for(size_t j = 0; j < a.own.taps.audio.size(); ++j) if(a.ownOf[j] < taps.audio.size()) a.own.taps.audio[j].sources = taps.audio[a.ownOf[j]].sources;
        if(a.own.notes) for(size_t i = 0; i < a.own.taps.notes.size() && i < taps.notes.size(); ++i) a.own.taps.notes[i].nodes = taps.notes[i].nodes;
        publish();
    }

    void Recorder::stop()
    {
        if(!recording_) return;
        const auto it = std::find_if(live_.begin(), live_.end(), [&](const auto& a) { return a->id == recording_; });
        if(it == live_.end()) return;
        auto& a = **it;
        { std::lock_guard lock(a.mutex); if(a.status.state == "recording" || a.status.state == "waiting") a.status.state = "finishing"; }
        a.end.store(now()); // the rings hold everything before it: the clock is published after them
        a.fed = false;
        recording_ = 0;
        publish();
    }

    std::vector<Recorder::Take> Recorder::takes() const
    {
        auto out = finished_;
        for(const auto& a : live_) { std::lock_guard lock(a->mutex); out.push_back(a->status); }
        return out;
    }

    void Recorder::forget(uint32_t take)
    {
        std::erase_if(finished_, [&](const Take& t) { return t.take == take; });
    }

    void Recorder::service()
    {
        bool changed = false;
        // A take of a set length stops feeding its own rings once they hold its end.
        for(auto& a : live_)
        {
            if(!a->fed || !a->settled.load(std::memory_order_acquire) || a->end.load() == unset) continue;
            const auto end = a->end.load();
            bool full = clockEnd_.load(std::memory_order_acquire) >= end;
            for(const auto& r : a->own.rings) full = full && r->written.load(std::memory_order_acquire) >= end;
            if(!full) continue;
            { std::lock_guard lock(a->mutex); if(a->status.state == "recording") a->status.state = "finishing"; }
            a->fed = false; changed = true;
        }
        if(player_ && player_->ended.load()) { player_.reset(); changed = true; }
        if(changed) publish();
        const auto* now = table_.load();
        const auto* used = inUse_.load();
        std::erase_if(tables_, [&](const auto& t) { return t.get() != now && t.get() != used; });
        // A take's rings go once the writer is done and no table the audio thread may hold has them.
        const auto before = live_.size();
        std::erase_if(live_, [&](const std::shared_ptr<Active>& a) {
            if(!a->done.load() || a->fed) return false;
            a->writer.join();
            finished_.push_back(a->status);
            return true;
        });
        if(live_.size() != before || changed) recount();
    }

    void Recorder::recount()
    {
        // What is allocated: the retroactive rings, each live take's rings (some retroactive ones
        // it still reads may have been replaced) and a playback; each counted once. Tables still
        // held may keep rings a little longer.
        std::set<const void*> seen;
        uint64_t n = 0;
        auto ring = [&](const std::shared_ptr<Ring>& r) { if(r && seen.insert(r.get()).second) n += r->bytes(); };
        auto notes = [&](const std::shared_ptr<Notes>& r) { if(r && seen.insert(r.get()).second) n += Notes::bytes(); };
        for(const auto& r : retro_.rings) ring(r);
        notes(retro_.notes);
        for(const auto& a : live_) { for(const auto& r : a->rings) ring(r); notes(a->notes); }
        if(player_) n += player_->samples.size() * sizeof(float);
        committed_.store(n);
    }

    void Recorder::publish()
    {
        auto t = std::make_unique<Table>();
        auto add = [&](const Set& s) {
            for(size_t i = 0; i < s.rings.size() && i < s.taps.audio.size(); ++i) t->audio.push_back({s.rings[i], s.taps.audio[i].sources});
            if(s.notes) t->notes.push_back({s.notes, s.taps.notes});
        };
        add(retro_);
        for(const auto& a : live_) if(a->fed) add(a->own);
        t->player = player_;
        table_.store(t.get());
        tables_.push_back(std::move(t));
        const auto* now = table_.load();
        const auto* used = inUse_.load();
        std::erase_if(tables_, [&](const auto& x) { return x.get() != now && x.get() != used; });
    }

    void Recorder::play(uint32_t take, const fs::path& wav)
    {
        if(!take) { if(player_) { player_.reset(); publish(); recount(); } return; }
        auto p = std::make_shared<Player>();
        double rate = 0;
        p->samples = readWav(wav, p->channels, rate);
        if(std::llround(rate) != std::llround(sampleRate_)) throw std::invalid_argument("recorded at " + std::to_string(std::llround(rate)) + " Hz; the engine runs at " + std::to_string(std::llround(sampleRate_)));
        p->take = take;
        p->frames = p->samples.size() / p->channels;
        player_ = std::move(p);
        publish();
        recount();
    }

    uint32_t Recorder::playing() const noexcept { return player_ && !player_->ended.load() ? player_->take : 0; }

    // Audio thread ------------------------------------------------------------------------------

    void Recorder::begin(uint64_t time, const clap_event_transport_t& transport) noexcept
    {
        Table* t;
        do { t = table_.load(); inUse_.store(t); } while(table_.load() != t);
        block_ = t;
        time_ = time;
        tempo_ = transport.tempo;
        beat_ = double(transport.song_pos_beats) / double(CLAP_BEATTIME_FACTOR);
        bar_ = transport.tsig_num ? transport.tsig_num : 4;
        playing_ = (transport.flags & CLAP_TRANSPORT_IS_PLAYING) != 0;
        if(!t) return;
        const auto quarter = quarterOf(tempo_);
        for(auto& f : t->notes)
        {
            auto& ring = *f.ring;
            if(quarter == ring.quarter) continue;
            ring.quarter = quarter;
            const auto head = ring.head.load(std::memory_order_relaxed);
            ring.events[head % noteCapacity] = {time, 0, 0, {uint8_t(quarter >> 16), uint8_t(quarter >> 8), uint8_t(quarter)}};
            ring.head.store(head + 1, std::memory_order_release);
        }
    }

    void Recorder::note(Notes& ring, uint16_t track, const kick::clap::Event& event) noexcept
    {
        const auto* h = event.header();
        if(h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        Note n{time_ + h->time, track, 0, {}};
        auto value = [](double v) { return uint8_t(std::clamp(std::lround(v * 127), 0l, 127l)); };
        switch(h->type)
        {
            case CLAP_EVENT_NOTE_ON:
            case CLAP_EVENT_NOTE_OFF:
            {
                const auto& e = *reinterpret_cast<const clap_event_note_t*>(h);
                if(e.key < 0 || e.key > 127) { n.size = 4; break; } // a wildcard has no MIDI form
                const bool on = h->type == CLAP_EVENT_NOTE_ON;
                n.size = 3; n.bytes[0] = uint8_t((on ? 0x90 : 0x80) | (e.channel < 0 ? 0 : e.channel & 15)); n.bytes[1] = uint8_t(e.key);
                n.bytes[2] = on ? std::max<uint8_t>(1, value(e.velocity)) : value(e.velocity);
                break;
            }
            case CLAP_EVENT_NOTE_EXPRESSION:
            {
                const auto& e = *reinterpret_cast<const clap_event_note_expression_t*>(h);
                if(e.expression_id != CLAP_NOTE_EXPRESSION_PRESSURE || e.key > 127) { n.size = 4; break; }
                const auto channel = uint8_t(e.channel < 0 ? 0 : e.channel & 15);
                if(e.key < 0) { n.size = 2; n.bytes[0] = 0xD0 | channel; n.bytes[1] = value(e.value); }
                else { n.size = 3; n.bytes[0] = 0xA0 | channel; n.bytes[1] = uint8_t(e.key); n.bytes[2] = value(e.value); }
                break;
            }
            case CLAP_EVENT_NOTE_CHOKE: n.size = 4; break;
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
        // The ring never waits: the oldest event goes, and a reader that had not copied it counts it.
        const auto head = ring.head.load(std::memory_order_relaxed);
        ring.events[head % noteCapacity] = n;
        ring.head.store(head + 1, std::memory_order_release);
    }

    void Recorder::graphEvents(const kick::clap::Event* list, uint32_t count) noexcept
    {
        events(graphInput, list, count, false);
    }

    void Recorder::events(NodeId node, const kick::clap::Event* list, uint32_t count, bool emitted) noexcept
    {
        if(!block_ || !count) return;
        for(auto& f : block_->notes)
            for(size_t t = 0; t < f.taps.size(); ++t)
                if(f.taps[t].emitted == emitted && std::find(f.taps[t].nodes.begin(), f.taps[t].nodes.end(), node) != f.taps[t].nodes.end())
                    for(uint32_t i = 0; i < count; ++i) note(*f.ring, uint16_t(t), list[i]);
    }

    void Recorder::end(const Graph& graph, float* const* outputs, uint32_t outputChannels, uint32_t frames) noexcept
    {
        if(block_)
            for(auto& f : block_->audio)
            {
                auto& ring = *f.ring;
                for(uint32_t c = 0; c < ring.channels; ++c) std::fill_n(ring.block.data() + size_t(c) * maxFrames_, frames, 0.f);
                for(const auto& s : f.sources)
                {
                    if(s.to >= ring.channels) continue;
                    const float* from = s.node == graphOutput ? (outputs && s.channel < outputChannels ? outputs[s.channel] : nullptr) : graph.audioOutput(s.node, s.channel);
                    if(!from) continue;
                    auto* to = ring.block.data() + size_t(s.to) * maxFrames_;
                    for(uint32_t k = 0; k < frames; ++k) to[k] += from[k] * s.gain;
                }
                for(uint32_t k = 0; k < frames; ++k)
                {
                    auto* frame = ring.data.data() + size_t((time_ + k) % ring.capacity) * ring.channels;
                    for(uint32_t c = 0; c < ring.channels; ++c) frame[c] = ring.block[size_t(c) * maxFrames_ + k];
                }
                if(ring.first.load(std::memory_order_relaxed) == unset) ring.first.store(time_, std::memory_order_relaxed);
                ring.written.store(time_ + frames, std::memory_order_release);
            }
        // The timeline and transport for the control thread, after the rings: what they hold is before it.
        const auto s = clockSequence_.load(std::memory_order_relaxed);
        clockSequence_.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        clockTime_.store(time_, std::memory_order_relaxed); clockEnd_.store(time_ + frames, std::memory_order_relaxed);
        clockTempo_.store(tempo_, std::memory_order_relaxed); clockBeat_.store(beat_, std::memory_order_relaxed);
        clockBar_.store(bar_, std::memory_order_relaxed); clockPlaying_.store(playing_, std::memory_order_relaxed);
        clockSequence_.store(s + 2, std::memory_order_release);
        // A take played back, after the taps: heard, not recorded.
        if(!block_ || !block_->player || !outputs) return;
        auto& p = *block_->player;
        if(p.ended.load(std::memory_order_relaxed)) return;
        const auto at = p.position.load(std::memory_order_relaxed);
        const auto n = uint32_t(std::min<uint64_t>(frames, p.frames - at));
        for(uint32_t o = 0; o < outputChannels && outputs[o]; ++o)
        {
            if(p.channels > 1 && o >= p.channels) break;
            const uint32_t c = p.channels == 1 ? 0 : o;
            for(uint32_t k = 0; k < n; ++k) outputs[o][k] += p.samples[size_t(at + k) * p.channels + c];
        }
        p.position.store(at + n, std::memory_order_relaxed);
        if(at + n >= p.frames) p.ended.store(true, std::memory_order_release);
    }
}

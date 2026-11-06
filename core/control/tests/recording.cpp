// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0020 steps 1 to 3: recording taps to files, and the retroactive rings, checked against what the session rendered,
// sample for sample, with no audio device.
//   oscairn-recording-test BASIC_SYNTH.clap
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "AllocationGuard.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <unistd.h>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    std::string slurp(const fs::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }

    struct Wav { uint32_t channels = 0, rate = 0; std::vector<float> samples; bool valid = false; };
    Wav readWav(const fs::path& p)
    {
        Wav w;
        const auto b = slurp(p);
        auto u32 = [&](size_t at) { uint32_t v; std::memcpy(&v, b.data() + at, 4); return v; };
        auto u16 = [&](size_t at) { uint16_t v; std::memcpy(&v, b.data() + at, 2); return v; };
        if(b.size() < 58 || b.compare(0, 4, "RIFF") || b.compare(8, 4, "WAVE") || b.compare(50, 4, "data")) return w;
        w.channels = u16(22); w.rate = u32(24);
        const auto bytes = u32(54);
        w.valid = u16(20) == 3 && u16(34) == 32 && u32(4) == 50 + bytes && b.size() == 58 + size_t(bytes) && u32(46) * w.channels * 4 == bytes;
        w.samples.resize(bytes / 4);
        std::memcpy(w.samples.data(), b.data() + 58, std::min<size_t>(bytes, b.size() - 58));
        return w;
    }
    struct MidiEvent { uint32_t tick; std::vector<uint8_t> bytes; };
    std::vector<std::vector<MidiEvent>> readMidi(const fs::path& p) // per track; metas other than tempo left out
    {
        const auto b = slurp(p);
        std::vector<std::vector<MidiEvent>> tracks;
        if(b.size() < 14 || b.compare(0, 4, "MThd")) return tracks;
        size_t at = 14;
        auto vlq = [&] { uint32_t v = 0; uint8_t c; do { c = uint8_t(b[at++]); v = v << 7 | (c & 0x7F); } while(c & 0x80); return v; };
        while(at + 8 <= b.size() && !b.compare(at, 4, "MTrk"))
        {
            const size_t len = size_t(uint8_t(b[at + 4])) << 24 | size_t(uint8_t(b[at + 5])) << 16 | size_t(uint8_t(b[at + 6])) << 8 | uint8_t(b[at + 7]);
            const auto end = at + 8 + len; at += 8;
            tracks.emplace_back();
            uint32_t tick = 0;
            while(at < end)
            {
                tick += vlq();
                const auto status = uint8_t(b[at++]);
                if(status == 0xFF)
                {
                    const auto type = uint8_t(b[at++]); const auto n = vlq();
                    if(type == 0x51) tracks.back().push_back({tick, {0xFF, 0x51, uint8_t(b[at]), uint8_t(b[at + 1]), uint8_t(b[at + 2])}});
                    at += n;
                    continue;
                }
                const auto data = (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 1 : 2;
                MidiEvent e{tick, {status}};
                for(int i = 0; i < data; ++i) e.bytes.push_back(uint8_t(b[at++]));
                tracks.back().push_back(e);
            }
        }
        return tracks;
    }

    struct Render
    {
        Session& s;
        std::vector<float> main; // what Main played, interleaved, while recording
        std::array<float, 512> l{}, r{};
        bool keep = true;
        explicit Render(Session& session, bool keeping = true) : s(session), keep(keeping) {}
        void block(uint32_t frames, std::vector<kick::clap::Event> events = {})
        {
            s.service();
            float* out[2]{l.data(), r.data()};
            forbidAllocation = true;
            const bool ok = s.process(out, frames, events);
            forbidAllocation = false;
            check(ok, "render");
            if(keep) for(uint32_t i = 0; i < frames; ++i) { main.push_back(l[i]); main.push_back(r[i]); }
        }
    };
    Session::Take finish(Session& s, uint32_t take, bool stop = true, Render* through = nullptr)
    {
        if(stop) s.recordStop(take);
        if(through) through->block(16); else Render(s, false).block(16); // the audio thread ends it at the next block
        for(int i = 0; i < 2000; ++i)
        {
            s.service();
            for(const auto& t : s.recordTakes()) if(t.take == take && (t.state == "done" || t.state == "failed")) return t;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        check(false, "the take finishes");
        return {};
    }
    bool same(const std::vector<float>& a, const std::vector<float>& b, size_t from = 0)
    {
        if(a.size() != b.size()) return false;
        for(size_t i = from; i < a.size(); ++i) if(a[i] != b[i]) return false;
        return true;
    }

    // Step 1 gate: a take of Main matches the render sample for sample, whatever the block sizes.
    void mainMatchesRender(const char* synth, uint32_t maxFrames, std::vector<uint32_t> sizes)
    {
        Session s(48000, maxFrames);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        Render render(s);
        render.keep = false;
        render.block(std::min(maxFrames, 100u)); // before the take
        const auto take = s.recordStart();
        render.keep = true;
        const uint64_t expected = Recorder::noteCapacity * 16 + 2ull * 4 * (48000 * 10 + maxFrames);
        check(s.recorder().committedBytes() == expected, "the rings are what the RFC sizes: 10 s of stereo float, a block and the notes ring");
        uint32_t n = 0;
        for(size_t i = 0; n < 48000; ++i)
        {
            const auto frames = sizes[i % sizes.size()];
            std::vector<kick::clap::Event> events;
            if(i == 1) events.push_back(kick::clap::Event::note(true, 60, .8, 3));
            if(i == 7) events.push_back(kick::clap::Event::note(true, 67, .5, frames - 1));
            if(i == 40) events.push_back(kick::clap::Event::note(false, 60, 0, 0));
            render.block(frames, events);
            n += frames;
        }
        check(s.recorder().committedBytes() == expected, "the memory does not grow with the take");
        const auto t = finish(s, take);
        check(t.state == "done" && t.error.empty(), "the take is done: " + t.error);
        check(t.frames == n && t.overruns.empty(), "every frame is written, with no overrun");
        const auto wav = readWav(fs::path(t.folder) / "Main.wav");
        check(wav.valid && wav.channels == 2 && wav.rate == 48000, "Main.wav is a valid stereo float WAV");
        check(same(wav.samples, render.main), "Main.wav is the rendered output, sample for sample (block " + std::to_string(maxFrames) + ")");
        bool sound = false; for(auto v : render.main) sound = sound || v != 0;
        check(sound, "the synth played");
        check(fs::exists(fs::path(t.folder) / "take.json") && slurp(fs::path(t.folder) / "take.json").find("\"state\":\"done\"") != std::string::npos, "take.json records the take");
        // "done" is published a moment before the writer thread ends and service() may free its rings.
        for(int i = 0; i < 400 && s.recorder().committedBytes(); ++i) { s.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        check(s.recorder().committedBytes() == 0, "the rings are freed once the take is written");
    }

    // A writer stalled on purpose: one overrun with the exact lost range, written as silence, and
    // the rest of the file still aligned.
    void stalledWriter(const char* synth)
    {
        constexpr uint32_t block = 256;
        Session s(48000, block);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        s.recorder().setRingSeconds(0.01); // at least four blocks: 1024 frames
        s.recorder().holdWriter(true);
        const auto take = s.recordStart({{Control::RecordTap::Kind::Output, 0}});
        Render render(s);
        for(int i = 0; i < 16; ++i) render.block(block, i == 0 ? std::vector{kick::clap::Event::note(true, 60, 1, 0)} : std::vector<kick::clap::Event>{});
        s.recorder().holdWriter(false);
        const auto t = finish(s, take);
        const uint64_t written = 16 * block, lost = written + block - 1024; // a ring, less the block the audio thread may be writing
        check(t.state == "done", "a stalled writer still finishes the take");
        check(t.overruns.size() == 1 && t.overruns[0].start == 0 && t.overruns[0].frames == lost,
              "one overrun, the exact range lost: " + (t.overruns.empty() ? std::string("none") : std::to_string(t.overruns[0].start) + "+" + std::to_string(t.overruns[0].frames)));
        auto wav = readWav(fs::path(t.folder) / "Main.wav");
        check(wav.valid && wav.samples.size() == written * 2, "the file keeps every frame's place");
        bool silent = true; for(size_t i = 0; i < lost * 2 && i < wav.samples.size(); ++i) silent = silent && wav.samples[i] == 0;
        check(silent, "the lost span is silence");
        check(same(wav.samples, render.main, lost * 2), "after the overrun, the file is the render, aligned");
        check(slurp(fs::path(t.folder) / "take.json").find("\"overruns\":[[0," + std::to_string(lost) + "]]") != std::string::npos, "take.json lists the overrun");
    }

    // A drive with less free than the reserve: the take stops with valid files and says why.
    void diskFull(const char* synth)
    {
        Session s(48000, 128);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        s.recorder().setDiskReserve(uint64_t(1) << 62);
        const auto take = s.recordStart({{Control::RecordTap::Kind::Output, 0}});
        Render render(s);
        for(int i = 0; i < 20; ++i) render.block(128);
        const auto t = finish(s, take);
        check(t.state == "failed" && t.error == "diskFull", "a full drive fails the take with diskFull: " + t.state + " " + t.error);
        const auto wav = readWav(fs::path(t.folder) / "Main.wav");
        check(wav.valid && wav.samples.empty(), "its file is still a valid WAV");
    }

    // Step 2: part taps from the fader wires, and the notes each part receives.
    void partsAndNotes(const char* synth)
    {
        constexpr uint32_t block = 128;
        Session s(48000, block);
        const auto a = s.add("a", synth), b = s.add("b", synth);
        const auto lead = s.addPart("Lead"), pad = s.addPart("Pad");
        s.insertIntoPart(lead, 0, a);
        s.insertIntoPart(pad, 0, b);
        s.setPartLevel(lead, .5, false);
        s.setPartPan(pad, -.3);
        s.commit();
        using K = Control::RecordTap::Kind;
        bool refused = false;
        try { s.recordStart({{K::Part, 0, 99}}); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "an unknown part is refused");
        const auto take = s.recordStart({{K::Output, 0}, {K::Part, 0, lead}, {K::Part, 0, pad}, {K::Part, 0, lead, 0, true}, {K::Notes}, {K::Notes, 0, pad}});
        refused = false;
        try { s.recordStart(); } catch(const std::logic_error&) { refused = true; }
        check(refused, "one take at a time");
        Render render(s);
        // Notes at known samples from the take's start: 25 samples are one tick at 120 BPM.
        render.block(block, {kick::clap::Event::note(true, 60, 1, 100)});
        render.block(block, {kick::clap::Event::note(true, 64, .5, 0)});
        for(int i = 0; i < 20; ++i) render.block(block);
        s.setPartLevel(lead, .25, false); s.commit(); // the fader moves during the take
        for(int i = 0; i < 20; ++i) render.block(block);
        s.setPartLevel(pad, 1, true); s.commit(); // and a part is muted
        for(int i = 0; i < 9; ++i) render.block(block, i == 8 ? std::vector{kick::clap::Event::note(false, 60, 0, 25)} : std::vector<kick::clap::Event>{});
        const auto t = finish(s, take);
        check(t.state == "done", "the parts take is done: " + t.error);
        check(t.files == std::vector<std::string>{"Main.wav", "Lead.wav", "Pad.wav", "Lead pre-fader.wav", "Notes.mid"}, "a file per tap, named after it");
        const fs::path folder = t.folder;
        const auto main = readWav(folder / "Main.wav"), leadPost = readWav(folder / "Lead.wav"), padPost = readWav(folder / "Pad.wav"), leadPre = readWav(folder / "Lead pre-fader.wav");
        check(same(main.samples, render.main), "Main is the render");
        bool sums = leadPost.samples.size() == main.samples.size() && padPost.samples.size() == main.samples.size();
        // Within one rounding: the graph's mix may fuse its multiply-adds.
        for(size_t i = 0; sums && i < main.samples.size(); ++i) sums = std::abs(leadPost.samples[i] + padPost.samples[i] - main.samples[i]) <= std::abs(main.samples[i]) * 1.2e-7f + 1e-30f;
        check(sums, "the post-fader taps of the parts playing to Main sum to Main, through a fader move and a mute");
        bool muted = true; for(size_t i = 2 * 43 * block; i < padPost.samples.size(); ++i) muted = muted && padPost.samples[i] == 0; // the mute ramps across block 42
        check(muted, "a muted part records silence after its fader");
        bool pre = leadPre.samples.size() == main.samples.size(), sound = false;
        for(size_t i = 0; pre && i < 2 * 22 * block; ++i) { pre = leadPost.samples[i] == leadPre.samples[i] * .5f; sound = sound || leadPre.samples[i] != 0; }
        check(pre && sound, "pre-fader is the part before its level");
        const auto midi = readMidi(folder / "Notes.mid");
        check(midi.size() == 3, "a tempo track and a track per note tap");
        if(midi.size() == 3)
        {
            check(!midi[0].empty() && midi[0][0].tick == 0 && midi[0][0].bytes == std::vector<uint8_t>{0xFF, 0x51, 0x07, 0xA1, 0x20}, "the tempo map starts at 120 BPM");
            const std::vector<std::pair<uint32_t, std::vector<uint8_t>>> expected{{4, {0x90, 60, 127}}, {5, {0x90, 64, 64}}, {(50 * 128 + 25) / 25, {0x80, 60, 0}}};
            for(int track : {1, 2})
            {
                check(midi[track].size() == 3, "each note tap has the three events");
                for(size_t i = 0; i < 3 && i < midi[track].size(); ++i)
                    check(midi[track][i].tick == expected[i].first && midi[track][i].bytes == expected[i].second,
                          "event " + std::to_string(i) + " of track " + std::to_string(track) + " at tick " + std::to_string(midi[track][i].tick));
            }
        }
        check(slurp(folder / "take.json").find("\"firstNote\":100") != std::string::npos, "take.json keeps the first note's exact sample");
    }

    // A graph input port and a plugin's output port.
    void inputAndNode(const char* synth)
    {
        Session s(48000, 64, {}, {{{5, "Mic", 1, 0}}, {{0, "Main", 2, 0}}});
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        using K = Control::RecordTap::Kind;
        const auto take = s.recordStart({{K::Input, 5}, {K::Node, 0, 0, node}, {K::Part, 0, part, 0, true}});
        std::vector<float> fed;
        std::array<float, 64> mic{}, l{}, r{};
        for(int b = 0; b < 10; ++b)
        {
            for(size_t i = 0; i < mic.size(); ++i) fed.push_back(mic[i] = float(b * 64 + int(i)) / 1000.f);
            const float* in[1]{mic.data()};
            float* out[2]{l.data(), r.data()};
            s.service();
            std::vector<kick::clap::Event> events;
            if(b == 0) events.push_back(kick::clap::Event::note(true, 60, 1, 0));
            forbidAllocation = true;
            const bool ok = s.process(in, 1, out, 2, 64, events);
            forbidAllocation = false;
            check(ok, "render with an input"); // its message is a heap string past libstdc++'s 15-character SSO
        }
        const auto t = finish(s, take);
        std::string names; for(const auto& f : t.files) names += f + ";";
        check(t.files == std::vector<std::string>{"Mic.wav", "synth Main.wav", "Lead pre-fader.wav"}, "files named after the port and the plugin: " + names);
        const auto mic2 = readWav(fs::path(t.folder) / "Mic.wav"), out = readWav(fs::path(t.folder) / "synth Main.wav"), pre = readWav(fs::path(t.folder) / "Lead pre-fader.wav");
        check(mic2.valid && mic2.channels == 1 && same(mic2.samples, fed), "the input tap is what came in");
        bool sound = false; for(auto v : out.samples) sound = sound || v != 0;
        check(sound && same(out.samples, pre.samples), "the plugin's port is the part before its fader");
    }

    // The control protocol: start, status and stop over JSON-RPC.
    void protocol(const char* synth)
    {
        Session s(48000, 128);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        JsonRpc rpc(s);
        auto call = [&](const std::string& body) { return rpc.dispatch(R"({"jsonrpc":"2.0","id":1,)" + body + "}").value_or(""); };
        check(call(R"("method":"record.start","params":{"taps":[{"kind":"part","part":7}]})").find(R"("code":-32007)") != std::string::npos, "an unknown tap is -32007");
        check(call(R"("method":"record.start","params":{"taps":[{"kind":"output","port":0},{"kind":"part","part":)" + std::to_string(part) + R"(,"fader":"pre"},{"kind":"notes"}],"name":"Lick"})") == R"({"jsonrpc":"2.0","id":1,"result":{"take":1}})", "record.start");
        check(call(R"("method":"record.start","params":{})").find(R"("reason":"busy")") != std::string::npos, "a second take is refused as busy");
        check(call(R"("method":"subscribe","params":{"topics":["params.changed","transport","graph.changed","status","record"]})").find(R"("accepted":true)") != std::string::npos, "the record topic, with the other four");
        Render render(s);
        for(int i = 0; i < 4; ++i) render.block(128);
        auto notified = [&](uint64_t now, const std::string& what) { for(const auto& n : rpc.notifications(now)) if(n.find(R"("method":"record")") != std::string::npos && n.find(what) != std::string::npos) return true; return false; };
        check(notified(1, R"({"take":1,"state":"recording")"), "the record topic sends the take recording");
        check(!notified(2, R"("take":1)"), "at most 10 times a second");
        const auto got = call(R"("method":"record.get")");
        check(got.find(R"("recording":1,"retro":{"seconds":0},"takes":[)") != std::string::npos && got.find(R"({"take":1,"state":"recording")") != std::string::npos, "record.get reports the take recording, among the earlier tests' takes found on disk");
        check(call(R"("method":"record.stop","params":{"take":1})") == R"({"jsonrpc":"2.0","id":1,"result":{"accepted":true}})", "record.stop");
        const auto t = finish(s, 1, false);
        check(notified(1000000000, R"({"take":1,"state":"done")"), "and once more when it is done");
        check(!notified(2000000000, R"("take":1)"), "then no more");
        check(t.folder.find(" Lick") != std::string::npos && t.files.size() == 3, "the take's folder carries its name");
        const auto takes = call(R"("method":"record.takes")");
        check(takes.find(R"("state":"done")") != std::string::npos && takes.find(R"("frames":512)") != std::string::npos, "record.takes: " + takes);
    }

    // Step 3: the retroactive rings, armed outside any take. Capture saves the last seconds of
    // Main as rendered, sample for sample, and the notes played in them; the rings' memory is
    // fixed, counted as committed memory in the budget (RFC 0025), and a buffer past the budget
    // is refused.
    void retroactive(const char* synth)
    {
        constexpr uint32_t block = 128;
        Session s(48000, block);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        s.setBudget({0.9, 1ull << 30});
        const auto before = s.memoryState().committed;
        s.recordRetro(4); // Main and the notes entering the graph
        const uint64_t rings = 2ull * 4 * (4 * 48000 + block) + Recorder::noteCapacity * 16;
        check(s.recorder().committedBytes() == rings, "the retroactive rings are 4 s of stereo float, a block and the notes ring: " + std::to_string(s.recorder().committedBytes()));
        check(s.memoryState().committed == before + rings, "and they are committed memory in the budget: " + std::to_string(s.memoryState().committed - before));
        check(s.recordRetroSeconds() == 4, "the buffer's length reads back");
        bool refused = false;
        try { s.recordRetro(3000); } catch(const std::length_error&) { refused = true; }
        check(refused && s.recorder().committedBytes() == rings && s.recordRetroSeconds() == 4, "a buffer past the budget is refused, the one there kept");
        Render render(s);
        // Six seconds, so the rings lap; a note every 0.32 s.
        uint64_t total = 0;
        for(int i = 0; total < 6 * 48000; ++i)
        {
            std::vector<kick::clap::Event> events;
            if(i % 120 == 7) events.push_back(kick::clap::Event::note(true, 60 + i / 120, .8, 5));
            if(i % 120 == 60) events.push_back(kick::clap::Event::note(false, 60 + i / 120, 0, 9));
            render.block(block, events);
            total += block;
        }
        check(s.recorder().committedBytes() == rings, "the memory stays the rings' while they lap");
        Control::RecordWhen last2; last2.from = Control::RecordWhen::From::SecondsAgo; last2.seconds = 2;
        const auto take = s.recordStart({}, "Capture", last2);
        check(s.recorder().recording() == 0, "a capture ends by itself, so a take can record beside it");
        const auto t = finish(s, take, false);
        check(t.state == "done" && t.overruns.empty(), "the capture is done with no overrun: " + t.state + " " + t.error);
        check(t.start == total - 96000 && t.frames == 96000, "it is the last 2 seconds: " + std::to_string(t.start) + "+" + std::to_string(t.frames));
        const auto wav = readWav(fs::path(t.folder) / "Main.wav");
        const std::vector<float> lastTwo(render.main.end() - 2 * 96000, render.main.end());
        check(wav.valid && same(wav.samples, lastTwo), "Main.wav is the render's last 2 seconds, sample for sample");
        // The notes in those 2 seconds, at the ticks their samples give (25 samples a tick at 120 BPM).
        const auto midi = readMidi(fs::path(t.folder) / "Notes.mid");
        size_t expected = 0;
        std::vector<std::pair<uint32_t, uint8_t>> ticks; // the events the take should hold
        for(uint64_t i = 0, at = 0; at < total; ++i, at += block)
        {
            if(i % 120 == 7 && at + 5 >= total - 96000) ticks.push_back({uint32_t(std::llround((at + 5 - (total - 96000)) / 25.0)), 0x90});
            if(i % 120 == 60 && at + 9 >= total - 96000) ticks.push_back({uint32_t(std::llround((at + 9 - (total - 96000)) / 25.0)), 0x80});
        }
        expected = ticks.size();
        check(midi.size() == 2 && midi[1].size() == expected, "the notes played in the last 2 seconds: " + std::to_string(midi.size() == 2 ? midi[1].size() : 0) + " of " + std::to_string(expected));
        for(size_t i = 0; midi.size() == 2 && i < midi[1].size() && i < ticks.size(); ++i)
            check(midi[1][i].tick == ticks[i].first && midi[1][i].bytes[0] == ticks[i].second, "note event " + std::to_string(i) + " at tick " + std::to_string(midi[1][i].tick));
        for(int i = 0; i < 400 && s.recorder().committedBytes() > rings; ++i) { s.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); } // as above
        check(s.recorder().committedBytes() == rings, "after the capture, only the rings");
        // More than the buffer holds: as much as it holds, less what the writer needs to begin.
        Control::RecordWhen all; all.from = Control::RecordWhen::From::SecondsAgo; all.seconds = 60;
        const auto longest = finish(s, s.recordStart({}, {}, all), false);
        check(longest.state == "done" && longest.overruns.empty() && longest.frames >= 2 * 48000 && longest.frames <= 4 * 48000, "a capture longer than the buffer is what it holds: " + std::to_string(longest.frames));
        // A tap the buffer does not hold has no past.
        refused = false;
        try { s.recordStart({{Control::RecordTap::Kind::Part, 0, part}}, {}, last2); } catch(const std::out_of_range&) { refused = true; }
        check(refused, "a capture of a tap the buffer does not hold is refused");
        // Over the protocol: record.retro and its refusal, a capture with from, and record.get's retro.
        JsonRpc rpc(s);
        auto call = [&](const std::string& body) { return rpc.dispatch(R"({"jsonrpc":"2.0","id":1,)" + body + "}").value_or(""); };
        check(call(R"("method":"record.retro","params":{"seconds":3000})").find(R"("reason":"memory")") != std::string::npos, "record.retro past the budget is -32007 memory");
        check(call(R"("method":"record.get")").find(R"("retro":{"seconds":4})") != std::string::npos, "record.get reports the buffer");
        const auto reply = call(R"("method":"record.start","params":{"from":{"secondsAgo":1},"name":"Over the socket"})");
        check(reply.find(R"("take":)") != std::string::npos, "record.start from seconds ago: " + reply);
        for(int i = 0; i < 400 && s.recorder().committedBytes() > rings; ++i) { s.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        const auto on = s.memoryState().committed;
        for(int i = 0; i < 400; ++i) // refused while the capture over the socket still reads the rings
            try { s.recordRetro(0); break; } catch(const std::logic_error&) { s.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        for(int i = 0; i < 400 && s.recorder().committedBytes(); ++i) { s.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        check(s.recorder().committedBytes() == 0 && on - s.memoryState().committed == rings, "turned off, the rings go and the budget no longer counts them");
    }

    // A set-length capture taken while an open-ended take records: stop() ends the take
    // recording, not the capture started last, and the capture keeps its whole length.
    void stopDuringCapture(const char* synth)
    {
        constexpr uint32_t block = 128;
        Session s(48000, block);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        Render render(s);
        render.block(block);
        const auto before = render.main.size() / 2;
        const auto take = s.recordStart({}, "Song"); // until stopped
        Control::RecordWhen cap; cap.until = Control::RecordWhen::Until::Seconds; cap.length = 0.5;
        const auto capture = s.recordStart({{Control::RecordTap::Kind::Output, 0}}, "Mid take", cap);
        while(render.main.size() / 2 - before < 24000 + block) render.block(block);
        s.recordStop(take); // the take recording, not the capture
        const auto stopped = render.main.size() / 2;
        while(render.main.size() / 2 < stopped + 24000) render.block(block); // let the capture's files close
        const auto t = finish(s, take, false);
        check(t.state == "done", "stop() closes the take recording even with a capture in flight: " + t.state);
        check(t.frames == stopped - before, "the take holds every frame up to the stop: " + std::to_string(t.frames) + " for " + std::to_string(stopped - before));
        const auto c = finish(s, capture, false);
        check(c.state == "done" && c.frames == 24000 && c.overruns.empty(), "the capture keeps its half second beside it: " + c.state + " " + std::to_string(c.frames));
    }

    // A set-length capture follows edits as a take until stopped does: its part's fader moves
    // halfway through, and what it records after its fader moves with it.
    void captureFollowsEdits(const char* synth)
    {
        constexpr uint32_t block = 128;
        Session s(48000, block);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.setPartLevel(part, .5, false);
        s.commit();
        Render render(s, false);
        using K = Control::RecordTap::Kind;
        Control::RecordWhen cap; cap.until = Control::RecordWhen::Until::Seconds; cap.length = 0.25;
        const auto capture = s.recordStart({{K::Part, 0, part}, {K::Part, 0, part, 0, true}}, "Edited", cap);
        render.block(block, {kick::clap::Event::note(true, 60, 1, 0)});
        for(int i = 0; i < 20; ++i) render.block(block);
        s.setPartLevel(part, .25, false); s.commit(); // the fader moves during the capture
        for(int i = 0; i < 100; ++i) render.block(block);
        const auto c = finish(s, capture, false);
        check(c.state == "done", "the capture is done: " + c.error);
        const auto post = readWav(fs::path(c.folder) / "Lead.wav"), pre = readWav(fs::path(c.folder) / "Lead pre-fader.wav");
        bool before = post.samples.size() == pre.samples.size() && post.samples.size() == 2 * 12000, after = before, sound = false;
        for(size_t i = 0; before && i < 2 * 20 * block; ++i) before = post.samples[i] == pre.samples[i] * .5f;
        for(size_t i = 2 * 40 * block; after && i < post.samples.size(); ++i) { after = post.samples[i] == pre.samples[i] * .25f; sound = sound || pre.samples[i] != 0; }
        check(before && after && sound, "the capture's post-fader tap follows the fader move");
    }

    // A capture reading the retroactive rings must not be killed by a change to that buffer
    // while it still reads it: the change is refused until the capture finishes.
    void retroChangeDuringCapture(const char* synth)
    {
        constexpr uint32_t block = 128;
        Session s(48000, block);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        s.recordRetro(2);
        Render render(s);
        while(render.main.size() / 2 < 96000) render.block(block); // enough past for the capture's start
        Control::RecordWhen cap;
        cap.from = Control::RecordWhen::From::SecondsAgo; cap.seconds = 0.1;
        cap.until = Control::RecordWhen::Until::Seconds; cap.length = 2; // ends 1.9 s in the future
        const auto start = render.main.size() / 2;
        const auto capture = s.recordStart({}, "Spans the change", cap);
        bool settled = false; // the writer has fixed its start, so its end is set
        for(int i = 0; i < 400 && !settled; ++i)
        {
            render.block(block);
            for(const auto& t : s.recordTakes()) if(t.take == capture && t.frames > 0) settled = true;
        }
        check(settled, "the capture's writer fixed its start");
        bool refused = false;
        try { s.recordRetro(0); } catch(const std::logic_error&) { refused = true; }
        check(!refused || s.recordRetroSeconds() == 2, "the buffer's change waits for the capture, refused or kept feeding its rings");
        while(render.main.size() / 2 < start + 96000 - 4800 + block) render.block(block); // to the capture's end: it began 0.1 s ago
        const auto t = finish(s, capture, false);
        check(t.state == "done" && t.frames == 96000 && t.overruns.empty(), "the 2 s capture finishes: " + t.state + " " + std::to_string(t.frames));
        s.recordRetro(0); // once the take is done, the change goes through
        for(int i = 0; i < 400 && s.recorder().committedBytes(); ++i) { s.service(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        check(s.recorder().committedBytes() == 0, "and the rings go after");
    }

    // Step 3's gate, offline: a take from the next bar starts on the bar line's exact sample, and
    // one from the last bar reads the retroactive rings back to it; both for a length in bars or
    // seconds.
    void bars(const char* synth)
    {
        constexpr uint32_t block = 128;
        Session s(48000, block);
        const auto node = s.add("synth", synth);
        const auto part = s.addPart("Lead");
        s.insertIntoPart(part, 0, node);
        s.commit();
        s.setTempo(120); s.setPlaying(true); // a bar of 4 beats is 96000 samples
        s.recordRetro(4);
        Render render(s);
        render.block(block, {kick::clap::Event::note(true, 64, .9, 3)});
        while(render.main.size() / 2 < 50000) render.block(block);
        Control::RecordWhen next; next.from = Control::RecordWhen::From::NextBar; next.until = Control::RecordWhen::Until::Bars; next.length = 1;
        const auto a = s.recordStart({{Control::RecordTap::Kind::Output, 0}}, "Next bar", next);
        std::string state; for(const auto& x : s.recordTakes()) if(x.take == a) state = x.state;
        check(state == "waiting", "it waits for the bar line: " + state);
        while(render.main.size() / 2 < 2 * 96000 + 1000) render.block(block);
        auto t = finish(s, a, false, &render);
        check(t.state == "done" && t.start == 96000 && t.frames == 96000, "from the next bar for a bar: starts on its exact sample, " + std::to_string(t.start) + "+" + std::to_string(t.frames));
        auto wav = readWav(fs::path(t.folder) / "Main.wav");
        check(wav.valid && same(wav.samples, std::vector<float>(render.main.begin() + 2 * 96000, render.main.begin() + 4 * 96000)), "and holds that bar of the render");
        // Just after the downbeat at 192000: from the last bar, half a second, out of the rings.
        Control::RecordWhen last; last.from = Control::RecordWhen::From::LastBar; last.until = Control::RecordWhen::Until::Seconds; last.length = .5;
        const auto b = s.recordStart({}, "Last bar", last);
        while(render.main.size() / 2 < 2 * 96000 + 30000) render.block(block);
        t = finish(s, b, false, &render);
        check(t.state == "done" && t.start == 2 * 96000 && t.frames == 24000, "from the last bar: back to its exact sample, " + std::to_string(t.start) + "+" + std::to_string(t.frames));
        wav = readWav(fs::path(t.folder) / "Main.wav");
        check(wav.valid && same(wav.samples, std::vector<float>(render.main.begin() + 4 * 96000, render.main.begin() + 4 * 96000 + 2 * 24000)), "and holds the render from there");
        // With the transport stopped, a bar line is now.
        s.setPlaying(false);
        render.block(block);
        const auto now = render.main.size() / 2;
        const auto c = s.recordStart({{Control::RecordTap::Kind::Output, 0}}, {}, next);
        while(render.main.size() / 2 < now + 96000 + 512) render.block(block);
        t = finish(s, c, false, &render);
        check(t.start == now, "with the transport stopped, the next bar is now: " + std::to_string(t.start) + " for " + std::to_string(now));
    }

    // Takes of an earlier run are found by scanning recordings/; a take plays back into the
    // outputs only when asked, offline, and is renamed and deleted.
    void earlierTakes()
    {
        Session s(48000, 128); // no parts: what comes out is the playback alone
        const auto takes = s.recordTakes();
        check(takes.size() >= 5, "the takes of the sessions before are found: " + std::to_string(takes.size()));
        const Control::Take* lick = nullptr;
        for(const auto& t : takes) if(t.folder.ends_with(" Capture")) lick = &t;
        check(lick && lick->state == "done" && lick->frames == 96000 && lick->sampleRate == 48000 && lick->files.size() == 2, "the capture is found with its length and files");
        if(!lick) return;
        const auto take = lick->take;
        const auto wav = readWav(fs::path(lick->folder) / "Main.wav");
        Render render(s);
        render.block(128);
        bool silent = true; for(auto v : render.main) silent = silent && v == 0;
        check(silent && !lick->playing, "nothing plays until asked");
        s.recordPlay(take);
        render.main.clear();
        while(render.main.size() < wav.samples.size() + 512) render.block(128);
        s.service();
        check(same(std::vector<float>(render.main.begin(), render.main.begin() + std::ptrdiff_t(wav.samples.size())), wav.samples), "played back, the outputs are the take's samples");
        bool after = true; for(size_t i = wav.samples.size(); i < render.main.size(); ++i) after = after && render.main[i] == 0;
        check(after, "and silence once it ends");
        auto find = [&](uint32_t id) { for(const auto& t : s.recordTakes()) if(t.take == id) return t; return Control::Take{}; };
        check(!find(take).playing, "it is no longer playing once it ended");
        s.recordPlay(take); render.block(128);
        check(find(take).playing, "playing while it plays");
        s.recordPlay(0); s.service();
        check(!find(take).playing, "and stopped when asked");
        s.recordRename(take, "Best lick");
        const auto renamed = find(take);
        check(fs::path(renamed.folder).filename().string().ends_with(" Best lick") && fs::path(renamed.folder).filename().string().size() == 19 + 1 + 9 && fs::exists(renamed.folder),
              "renamed, its folder keeps its time and takes the name: " + renamed.folder);
        bool refused = false;
        try { s.recordRename(take, "  "); } catch(const std::invalid_argument&) { refused = true; }
        check(refused && fs::exists(renamed.folder), "an empty name is refused");
        s.recordDelete(take);
        check(find(take).take == 0 && !fs::exists(renamed.folder), "deleted, its folder goes");
        refused = false;
        try { s.recordDelete(take); } catch(const std::invalid_argument&) { refused = true; }
        check(refused, "a take deleted is no longer found");
    }
}

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    const auto home = fs::temp_directory_path() / ("oscairn-recording-test-" + std::to_string(getpid()));
    fs::remove_all(home);
    setenv("OSCAIRN_DATA_HOME", home.c_str(), 1);
    try
    {
        mainMatchesRender(argv[1], 256, {256, 37, 128, 1, 200});
        mainMatchesRender(argv[1], 32, {32, 5, 17});
        stalledWriter(argv[1]);
        diskFull(argv[1]);
        partsAndNotes(argv[1]);
        inputAndNode(argv[1]);
        protocol(argv[1]);
        stopDuringCapture(argv[1]);
        captureFollowsEdits(argv[1]);
        retroChangeDuringCapture(argv[1]);
        retroactive(argv[1]);
        bars(argv[1]);
        earlierTakes();
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    fs::remove_all(home);
    if(failures) return 1;
    std::puts("recording: ok");
    return 0;
}

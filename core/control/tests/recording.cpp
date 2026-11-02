// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0020 steps 1 and 2: recording taps to files, checked against what the session rendered,
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
    Session::Take finish(Session& s, uint32_t take, bool stop = true)
    {
        if(stop) s.recordStop(take);
        Render(s, false).block(16); // the audio thread ends it at the next block
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
        bool muted = true; for(size_t i = 2 * 42 * block; i < padPost.samples.size(); ++i) muted = muted && padPost.samples[i] == 0;
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
            check(s.process(in, 1, out, 2, 64, events), "render with an input");
            forbidAllocation = false;
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
        Render render(s);
        for(int i = 0; i < 4; ++i) render.block(128);
        check(call(R"("method":"record.get")").find(R"("recording":1,"takes":[{"take":1,"state":"recording")") != std::string::npos, "record.get reports the take recording");
        check(call(R"("method":"record.stop","params":{"take":1})") == R"({"jsonrpc":"2.0","id":1,"result":{"accepted":true}})", "record.stop");
        const auto t = finish(s, 1, false);
        check(t.folder.find(" Lick") != std::string::npos && t.files.size() == 3, "the take's folder carries its name");
        const auto takes = call(R"("method":"record.takes")");
        check(takes.find(R"("state":"done")") != std::string::npos && takes.find(R"("frames":512)") != std::string::npos, "record.takes: " + takes);
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
    }
    catch(const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); ++failures; }
    fs::remove_all(home);
    if(failures) return 1;
    std::puts("recording: ok");
    return 0;
}

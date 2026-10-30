// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/assets.h"
#include "oscairn/graph.h"
#include "oscairn/json_rpc.h"
#include "oscairn/session.h"
#include "oscairn/wav_file.h"
#include "AllocationGuard.h"
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define SAMPLER_TEST_SANITIZED 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define SAMPLER_TEST_SANITIZED 1
#endif
#include "../../plugin-host/src/StateFiles.h"
#include <array>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <span>
#include <thread>
using namespace oscairn;
using kick::clap::Event;
namespace fs = std::filesystem;
void check(bool ok, const std::string& why) { if(!ok) throw std::runtime_error(why); }
std::string quote(const std::string& s)
{ std::string out = "\""; for(char c : s) { if(c == '"' || c == '\\') out += '\\'; if(c == '\n') out += "\\n"; else out += c; } return out + '"'; }

assets::Store* store = nullptr; // the test's own asset store, under its temporary folder
Control::AssetFolder place(const fs::path& dir)
{ Control::AssetFolder f; if(!store->locate(dir, f.library, f.directory)) f.library = store->registerFolder(dir); return f; }

struct Sampler {
    Graph graph{48000, 512}; NodeId node{};
    std::array<float, 512> left{}, right{}; float* out[2]{left.data(), right.data()};
    explicit Sampler(const char* path) { graph.setAssets(store); node = graph.add(path); graph.connectNotes(graphInput, node); graph.connectAudio(node, graphOutput); graph.commit(); }
    PluginHost& host() { return graph.host(node); }
    void run(std::span<const Event> events = {}) { forbidAllocation = true; const bool ok = graph.process(out, 512, nullptr, events.data(), events.size()); forbidAllocation = false; check(ok, "sampler render"); }
    void one(const Event& e) { run({&e, 1}); }
    uint32_t prepare(const std::string& sfz, const fs::path& dir, const char* name = "Sound") { const auto f = place(dir); return host().prepareSound(name, {}, {}, {}, sfz, f.library, f.directory); }
    std::string rejection(const std::string& sfz, const fs::path& dir, std::vector<oscairn_sound_parameter_t> parameters = {})
    { try { const auto f = place(dir); host().prepareSound("Bad", parameters, {}, {}, sfz, f.library, f.directory); } catch(const std::exception& e) { return e.what(); } return "accepted"; }
    oscairn_sound_context_info_t info(uint32_t id) { for(const auto& s : host().soundContexts()) if(s.id == id) return s; throw std::runtime_error("missing sound"); }
    void use(const std::string& sfz, const fs::path& dir) { check(host().selectSound(prepare(sfz, dir)), "select"); }
};
void rejects(Sampler& s, const std::string& sfz, const fs::path& dir, const std::string& expected)
{ const auto why = s.rejection(sfz, dir); check(why.find(expected) != std::string::npos, "expected rejection '" + expected + "', got: " + why); }

constexpr double level = 100.0 * 100.0 / (127.0 * 127.0) * 0.70710678118654752; // liquidsfz default (CC7 100, centre pan)
std::vector<float> tone(size_t frames, double hz, double amplitude = .5)
{ std::vector<float> w(frames); for(size_t i = 0; i < frames; ++i) w[i] = float(amplitude * std::sin(2 * M_PI * hz * double(i) / 48000)); return w; }

int main(int argc, char** argv) { try {
    if(argc != 2) return 2;
    const char* plugin = argv[1];
    state_files::TemporaryDirectory temp;
    setenv("OSCAIRN_DATA_HOME", (temp.path / "data").c_str(), 1); // Sessions here never touch the user's asset store
    assets::Store local(temp.path / "test-assets"); store = &local;
    const auto dir = temp.path / "Sampler fixtures \xC3\xBC"; fs::create_directory(dir); // spaces and Unicode
    const auto wave = tone(24000, 220);
    writeWavFile(dir / "ton \xC3\xA9.wav", wave, 1, 48000);
    std::vector<float> stereo(2 * 4800); for(size_t i = 0; i < 4800; ++i) stereo[2 * i] = wave[i];
    writeWavFile(dir / "stereo.wav", stereo, 2, 48000);
    writeWavFile(dir / "short.wav", std::vector<float>(100, .25f), 1, 48000);
    const std::string a = "// comment\n<region> sample=ton \xC3\xA9.wav pitch_keycenter=c4\n", b = "<region> sample=ton \xC3\xA9.wav transpose=7 volume=-6\n";

    { // Exact sample timing, unity pitch and mono to both channels.
        Sampler s(plugin); s.use(a, dir); s.one(Event::note(true, 60, 1, 37));
        for(int i = 0; i < 37; ++i) check(s.left[i] == 0, "silent before the note");
        for(int i = 37; i < 512; ++i) check(s.left[i] == float(level * wave[i - 37]) && s.right[i] == s.left[i], "sample starts on its exact frame");
        Sampler octave(plugin); octave.use(a, dir); octave.one(Event::note(true, 72, 1));
        for(int i = 0; i < 200; ++i) check(std::abs(octave.left[i] - level * wave[2 * i]) < 1e-7, "one octave up reads every second frame");
        Sampler soft(plugin); soft.use(a, dir); soft.one(Event::note(true, 60, .5));
        for(int i = 0; i < 512; ++i) check(std::abs(soft.left[i] - .25 * level * wave[i]) < 1e-7, "default velocity curve is squared");
    }
    { // Held notes keep their sound; two sounds play at once; note IDs release independently.
        Sampler mixed(plugin), refA(plugin), refB(plugin);
        const auto x = mixed.prepare(a, dir), y = mixed.prepare(b, dir);
        refA.use(a, dir); refB.use(b, dir); mixed.host().selectSound(x);
        const auto first = Event::note(true, 60, 1, 0, 0, 0, 101), second = Event::note(true, 64, 1, 0, 0, 0, 102);
        mixed.one(first); refA.one(first); refB.run();
        check(mixed.left == refA.left, "first context matches a standalone sampler");
        check(mixed.host().selectSound(y), "switch while held"); mixed.run(); refA.run(); refB.run();
        check(mixed.left == refA.left, "held note survives the switch unchanged");
        mixed.one(second); refA.run(); refB.one(second);
        for(int i = 0; i < 512; ++i) check(std::abs(mixed.left[i] - (refA.left[i] + refB.left[i])) < 1e-6, "old and new sounds render together");
        const auto off = Event::note(false, 60, 0, 0, 0, 0, 101);
        for(int block = 0; block < 4; ++block) { mixed.one(off); refA.one(off); refB.run(); }
        for(int i = 0; i < 512; ++i) check(std::abs(mixed.left[i] - refB.left[i]) < 1e-6, "note-ID release ends only its own voice");
        check(mixed.info(x).active_voices == 0 && mixed.info(y).active_voices == 1, "voice ownership telemetry");
    }
    { // Stereo isolation, key/velocity ranges, sample end and release.
        Sampler s(plugin); s.use("<region> sample=stereo.wav", dir); s.one(Event::note(true, 60, 1));
        double l = 0; for(int i = 0; i < 512; ++i) { check(s.right[i] == 0, "right channel stays silent"); l += std::abs(s.left[i]); }
        check(l > 10, "left channel plays");
        Sampler range(plugin); range.use("<region> sample=ton \xC3\xA9.wav lokey=60 hikey=60 lovel=100", dir);
        range.one(Event::note(true, 61, 1)); range.one(Event::note(true, 60, .5));
        for(float v : range.left) check(v == 0, "notes outside key or velocity range are silent");
        Sampler end(plugin); const auto id = end.prepare("<region> sample=short.wav", dir); end.host().selectSound(id); end.one(Event::note(true, 60, 1));
        check(end.left[99] == float(level * .25) && end.left[100] == 0 && end.info(id).active_voices == 0, "sample end finishes the voice");
        end.host().selectSound(0); end.run(); check(end.host().discardSound(id), "finished sound can be discarded");
        Sampler rel(plugin); rel.use("<region> sample=ton \xC3\xA9.wav ampeg_release=0.005", dir);
        rel.one(Event::note(true, 60, 1)); rel.one(Event::note(false, 60, 0));
        for(int i = 240; i < 512; ++i) check(rel.left[i] == 0, "release reaches silence after ampeg_release");
        check(std::abs(rel.left[100]) > 0, "release fades rather than cuts");
    }
    { // Invalid input is reported with its location and never disturbs the playing sound.
        Sampler live(plugin), reference(plugin); live.use(a, dir); reference.use(a, dir);
        live.one(Event::note(true, 60, 1)); reference.one(Event::note(true, 60, 1));
        const auto outside = temp.path / "outside.wav"; writeWavFile(outside, wave, 1, 48000);
        fs::create_symlink(outside, dir / "link.wav");
        { std::ofstream(dir / "text.wav") << "not audio"; }
        writeWavFile(dir / "three.wav", std::vector<float>(300, .1f), 3, 48000);
        fs::copy_file(dir / "short.wav", dir / "cut.wav"); fs::resize_file(dir / "cut.wav", 20);
        check(live.prepare("\xEF\xBB\xBF<region> sample=short.wav", dir) != CLAP_INVALID_ID, "a UTF-8 byte-order mark is ignored");
        rejects(live, "<region> sample=short.wav\nfoo=1", dir, "line 2: unsupported opcode foo");
        rejects(live, "<group>\n<region> sample=short.wav", dir, "line 1: unsupported header <group>");
        rejects(live, "#include \"other.sfz\"", dir, "line 1: preprocessor");
        rejects(live, "<region> /* x */ sample=short.wav", dir, "block comments");
        rejects(live, "lokey=1 <region> sample=short.wav", dir, "outside <region>");
        rejects(live, "<region> sample=short.wav\n<region> sample=short.wav", dir, "only one <region>");
        rejects(live, "<region> key=200 sample=short.wav", dir, "key out of range");
        rejects(live, "<region> lokey=70 hikey=60 sample=short.wav", dir, "lokey is above hikey");
        rejects(live, "<region> tune=500 sample=short.wav", dir, "out of range");
        rejects(live, "<region> lokey=60", dir, "no sample");
        rejects(live, "<region> sample=missing.wav", dir, "missing.wav not found in library");
        rejects(live, "<region> sample=../outside.wav", dir, "outside its library");
        rejects(live, "<region> sample=link.wav", dir, "outside library");
        rejects(live, "<region> sample=" + outside.string(), dir, "must be relative");
        rejects(live, "<region> sample=text.wav", dir, "cannot read WAV");
        rejects(live, "<region> sample=cut.wav", dir, "cannot read WAV");
        rejects(live, "<region> sample=three.wav", dir, "mono or stereo");
        check(live.rejection("<region> sample=short.wav", dir, {{1, 0}}).find("Invalid sound parameter") != std::string::npos, "parameters are refused");
        writeWavFile(dir / "huge.wav", std::vector<float>((64u << 20) / 4 + 1), 1, 48000);
        rejects(live, "<region> sample=huge.wav", dir, "64 MiB");
        fs::remove(dir / "huge.wav");
        live.run(); reference.run(); check(live.left == reference.left, "rejections leave the held sound intact");
    }
    { // Identical audio is decoded once (7 x 40 MiB exceeds the 256 MiB budget unless shared);
      // rewriting a file is new content, not stale cached audio.
        const auto big = tone(10u << 20, 110); writeWavFile(dir / "big.wav", big, 1, 48000); fs::copy_file(dir / "big.wav", dir / "copy.wav");
        Sampler s(plugin);
        for(int i = 0; i < 7; ++i) s.prepare(std::string("<region> sample=") + (i % 2 ? "copy.wav" : "big.wav"), dir);
        rejects(s, "<region> sample=short.wav", dir, "no free sound slot");
        fs::remove(dir / "big.wav"); fs::remove(dir / "copy.wav");
#if !defined(SAMPLER_TEST_SANITIZED)
        // Single-threaded and memory-heavy: sanitizers add minutes here and no coverage.
        { // Distinct audio counts against the 256 MiB instance budget: four 60 MiB samples fit, a fifth does not,
          // but reusing audio already loaded costs nothing even when the budget is nearly spent.
            Sampler budget(plugin); std::vector<float> block((60u << 20) / 4, .1f);
            for(int i = 0; i < 5; ++i) { block[0] = float(i) / 8; writeWavFile(dir / ("distinct" + std::to_string(i) + ".wav"), block, 1, 48000); }
            for(int i = 0; i < 4; ++i) budget.prepare("<region> sample=distinct" + std::to_string(i) + ".wav", dir);
            budget.prepare("<region> sample=distinct0.wav transpose=12", dir);
            rejects(budget, "<region> sample=distinct4.wav", dir, "memory budget");
        }
        { // A reopened Performance keeps shared audio shared: three sounds on one 60 MiB sample restore as
          // one copy, leaving room for two more distinct 60 MiB samples (300 MiB if restored unshared).
            Session original(48000, 512); const auto n = original.add("sampler", plugin); original.connectAudio(n, graphOutput); original.commit();
            const auto folder = original.registerAssetFolder(dir.string());
            for(int i = 0; i < 3; ++i) original.prepareSound(n, "Shared " + std::to_string(i), {}, {}, {}, "<region> sample=distinct0.wav tune=" + std::to_string(i), folder.library, folder.directory);
            const auto saved = temp.path / "shared"; original.savePerformance(saved.string());
            Session reopened(48000, 512); reopened.loadPerformance(saved.string()); const auto m = reopened.nodeId("sampler");
            for(int i = 1; i <= 2; ++i) reopened.prepareSound(m, "More", {}, {}, {}, "<region> sample=distinct" + std::to_string(i) + ".wav", folder.library, folder.directory);
            check(reopened.sounds(m).contexts.size() == 6, "restored sounds share audio and leave budget");
        }
#endif
        for(int i = 0; i < 5; ++i) fs::remove(dir / ("distinct" + std::to_string(i) + ".wav"));
        Sampler fresh(plugin), changed(plugin);
        writeWavFile(dir / "edit.wav", wave, 1, 48000); fresh.use("<region> sample=edit.wav", dir);
        writeWavFile(dir / "edit.wav", tone(24000, 330), 1, 48000); changed.use("<region> sample=edit.wav", dir);
        fresh.one(Event::note(true, 60, 1)); changed.one(Event::note(true, 60, 1));
        check(fresh.left != changed.left, "edited file is decoded again");
    }
    { // Preparation and retirement on the control thread while audio renders without allocating.
        Sampler s(plugin); std::atomic<bool> done{false}, ok{true};
        std::thread audio([&] { forbidAllocation = true; const std::array events{Event::note(true, 60, 1), Event::note(false, 60, 0, 100)};
            while(!done.load()) if(!s.graph.process(s.out, 512, nullptr, events.data(), events.size())) ok = false; forbidAllocation = false; });
        unsigned prepared = 0;
        for(const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5); prepared < 60 && std::chrono::steady_clock::now() < deadline;)
        {
            s.host().selectSound(0);
            for(const auto& c : s.host().soundContexts()) if(c.id) s.host().discardSound(c.id);
            if(s.host().soundContexts().size() < 8) { s.host().selectSound(s.prepare(prepared % 2 ? a : b, dir)); ++prepared; }
            std::this_thread::yield();
        }
        done = true; audio.join(); check(ok && prepared >= 60, "concurrent sample preparation, selection and retirement");
    }
    { // JSON control; saved Performances reference samples as dependencies.
        struct Fixture {
            Session session{48000, 512}; JsonRpc rpc{session}; std::array<float, 512> left{}, right{};
            std::string call(const std::string& method, const std::string& params) { return rpc.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":" + quote(method) + ",\"params\":" + params + '}').value_or(""); }
            void render() { float* out[]{left.data(), right.data()}; session.service(); check(session.process(out, 512), "session render"); session.service(); }
        };
        Fixture f; const auto n = f.session.add("sampler", plugin); f.session.connectNotes(graphInput, n); f.session.connectAudio(n, graphOutput); f.session.commit();
        const auto registered = f.call("assets.register", "{\"folder\":" + quote(dir.string()) + '}');
        const auto library = registered.substr(registered.find("\"library\":\"") + 11, 16);
        check(registered.find("\"directory\":\"\"") != std::string::npos && library.find('"') == std::string::npos, "wire registers the folder as a library: " + registered);
        const auto bad = f.call("sounds.prepare", "{\"node\":\"sampler\",\"name\":\"Bad\",\"sfz\":\"<region>\\nfoo=1\",\"library\":" + quote(library) + '}');
        check(bad.find("-32005") != std::string::npos && bad.find("line 2") != std::string::npos, "wire reports the diagnostic: " + bad);
        const auto reply = f.call("sounds.prepare", "{\"node\":\"sampler\",\"name\":\"Pitched\",\"sfz\":" + quote(b) + ",\"library\":" + quote(library) + '}');
        check(reply.find("\"context\":1") != std::string::npos, "wire preparation: " + reply);
        f.session.prepareSound(n, "Mono", {}, {}, {}, a, library, "");
        check(f.call("sounds.select", R"({"node":"sampler","context":1,"channel":-1})").find("\"accepted\":true") != std::string::npos, "wire selection");
        f.render(); f.session.note(true, 64, 1); f.render(); const auto before = f.left;
        double energy = 0; for(float v : before) energy += std::abs(v); check(energy > 10, "saved sound is audible");
        const auto saved = temp.path / "performance", moved = temp.path / "moved";
        f.session.savePerformance(saved.string()); fs::rename(saved, moved);
        size_t copied = 0; for(const auto& e : fs::recursive_directory_iterator(moved)) copied += e.path().extension() == ".wav";
        check(copied == 0, "samples are dependencies, not copied into the Performance");
        { std::ifstream in(moved / "performance.json"); const std::string manifest((std::istreambuf_iterator<char>(in)), {});
          check(manifest.find("\"assets\":[{\"library\":\"" + library + "\"") != std::string::npos && manifest.find("\"name\":\"Sampler fixtures") != std::string::npos, "the manifest lists sample references and library names"); }
        Fixture g; g.session.loadPerformance(moved.string()); g.render(); g.session.note(true, 64, 1); g.render();
        check(g.left == before, "moved Performance reopens and plays identically");
        const auto sounds = g.session.sounds(g.session.nodeId("sampler"));
        check(sounds.contexts.size() == 3 && sounds.contexts[1].name == "Pitched" && sounds.contexts[2].name == "Mono", "every prepared sound is restored");
        { // Another device: a fresh asset store does not know the library until the folder is registered,
          // under a new ID; the samples then resolve by content.
            setenv("OSCAIRN_DATA_HOME", (temp.path / "other-device").c_str(), 1);
            Fixture other; std::string why;
            try { other.session.loadPerformance(moved.string()); } catch(const std::exception& e) { why = e.what(); }
            check(why.find("missing sound assets: sampler: 1 of 1 files from library 'Sampler fixtures") != std::string::npos, "an unknown library is named before loading: " + why);
            check(other.session.registerAssetFolder(dir.string()).library != library, "the same folder gets a new library ID on another device");
            other.session.loadPerformance(moved.string()); other.render(); other.session.note(true, 64, 1); other.render();
            check(other.left == before, "samples resolve by content hash under the new library");
            setenv("OSCAIRN_DATA_HOME", (temp.path / "data").c_str(), 1);
        }
        auto refused = [&](const std::string& reason) { const auto revision = g.session.graphState().revision; std::string why;
            try { g.session.loadPerformance(moved.string()); } catch(const std::exception& e) { why = e.what(); }
            return why.find(reason) != std::string::npos && g.session.graphState().revision == revision; };
        writeWavFile(dir / "ton \xC3\xA9.wav", tone(24000, 330), 1, 48000);
        check(refused("missing sound assets: sampler: 1 of 1 files") && refused("ton \xC3\xA9.wav changed since it was saved"), "a changed sample is named and fails before replacing the live graph");
        fs::remove_all(dir);
        check(refused("ton \xC3\xA9.wav not found in library Sampler fixtures"), "a missing sample is named and fails before replacing the live graph");
        g.render(); check(g.left != std::array<float, 512>{}, "the current sound keeps playing");
    }
    std::cout << "ok: sampler playback, ownership, validation, sharing and sample dependencies\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }

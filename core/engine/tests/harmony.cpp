// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0033 step 5, the host's side: chord recognition, and a part's key, scale and chord
// reaching a plugin through oscairn.harmony.
//   - Common chords and their inversions read from held keys: triads, sixths, sevenths, ninths,
//     suspended and power chords; C6 and Am7 told apart by the bass; nothing for one key or a
//     cluster.
//   - Through the graph, in the engine and in a worker, at 256- and 37-sample blocks: the
//     follower fixture in a part keyed to D Dorian, whose zone transposes up a tone, sees the
//     key from the first sample and each chord change at the sample of the note that made it;
//     the host extension gives the latest on the main thread.
//   oscairn-harmony-test WORKER follower-fixture.clap
#include "oscairn/graph.h"
#include "oscairn/harmony.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <unistd.h>
#include <vector>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    oscairn_harmony_t held(std::initializer_list<int> keys)
    {
        oscairn_harmony_t h{};
        for(int k : keys) h.held[k / 64] |= uint64_t(1) << (k % 64);
        return h;
    }
    void chord(std::initializer_list<int> keys, int root, int type, int bass, const char* name)
    {
        auto h = held(keys);
        oscairn::recognizeChord(h);
        check(h.chord_root == root && h.chord_type == type && h.chord_bass == bass,
              std::string(name) + ": root " + std::to_string(h.chord_root) + ", type " + std::to_string(h.chord_type) + ", bass " + std::to_string(h.chord_bass));
    }

    void recognition()
    {
        chord({60, 64, 67}, 0, OSCAIRN_CHORD_MAJOR, 0, "C major");
        chord({64, 67, 72}, 0, OSCAIRN_CHORD_MAJOR, 4, "C major, first inversion");
        chord({55, 60, 64}, 0, OSCAIRN_CHORD_MAJOR, 7, "C major, second inversion");
        chord({36, 52, 67, 72, 76}, 0, OSCAIRN_CHORD_MAJOR, 0, "C major spread and doubled");
        chord({57, 60, 64}, 9, OSCAIRN_CHORD_MINOR, 9, "A minor");
        chord({60, 64, 69}, 9, OSCAIRN_CHORD_MINOR, 0, "A minor, first inversion");
        chord({59, 62, 65}, 11, OSCAIRN_CHORD_DIMINISHED, 11, "B diminished");
        chord({60, 64, 68}, 0, OSCAIRN_CHORD_AUGMENTED, 0, "C augmented, on its bass");
        chord({64, 68, 72}, 4, OSCAIRN_CHORD_AUGMENTED, 4, "E augmented: the bass names the symmetric chord");
        chord({60, 62, 67}, 0, OSCAIRN_CHORD_SUS2, 0, "Csus2");
        chord({55, 60, 62}, 7, OSCAIRN_CHORD_SUS4, 7, "Gsus4, the same notes on G");
        chord({48, 55, 60}, 0, OSCAIRN_CHORD_POWER, 0, "C5");
        chord({60, 64, 67, 69}, 0, OSCAIRN_CHORD_MAJOR6, 0, "C6");
        chord({57, 60, 64, 67}, 9, OSCAIRN_CHORD_MINOR7, 9, "Am7, the same notes on A");
        chord({60, 63, 67, 69}, 0, OSCAIRN_CHORD_MINOR6, 0, "Cm6");
        chord({55, 59, 62, 65}, 7, OSCAIRN_CHORD_DOMINANT7, 7, "G7");
        chord({59, 62, 65, 67}, 7, OSCAIRN_CHORD_DOMINANT7, 11, "G7, first inversion");
        chord({53, 55, 59, 62}, 7, OSCAIRN_CHORD_DOMINANT7, 5, "G7, third inversion");
        chord({60, 64, 67, 71}, 0, OSCAIRN_CHORD_MAJOR7, 0, "Cmaj7");
        chord({62, 65, 69, 72}, 2, OSCAIRN_CHORD_MINOR7, 2, "Dm7");
        chord({60, 63, 67, 71}, 0, OSCAIRN_CHORD_MINOR_MAJOR7, 0, "Cm(maj7)");
        chord({59, 62, 65, 69}, 11, OSCAIRN_CHORD_HALF_DIMINISHED7, 11, "Bm7b5");
        chord({59, 62, 65, 68}, 11, OSCAIRN_CHORD_DIMINISHED7, 11, "Bdim7");
        chord({62, 65, 68, 71}, 2, OSCAIRN_CHORD_DIMINISHED7, 2, "Ddim7, the same notes on D");
        chord({60, 64, 68, 70}, 0, OSCAIRN_CHORD_AUGMENTED7, 0, "C7#5");
        chord({55, 60, 62, 65}, 7, OSCAIRN_CHORD_DOMINANT7_SUS4, 7, "G7sus4");
        chord({60, 62, 64, 67}, 0, OSCAIRN_CHORD_ADD9, 0, "Cadd9");
        chord({60, 62, 63, 67}, 0, OSCAIRN_CHORD_MINOR_ADD9, 0, "Cm(add9)");
        chord({48, 52, 58, 62, 67}, 0, OSCAIRN_CHORD_DOMINANT9, 0, "C9");
        chord({48, 52, 59, 62, 67}, 0, OSCAIRN_CHORD_MAJOR9, 0, "Cmaj9");
        chord({48, 51, 58, 62, 67}, 0, OSCAIRN_CHORD_MINOR9, 0, "Cm9");
        chord({60}, 0, OSCAIRN_CHORD_NONE, 0, "one key");
        chord({62, 74}, 0, OSCAIRN_CHORD_NONE, 2, "an octave");
        chord({60, 61, 62}, 0, OSCAIRN_CHORD_NONE, 0, "a cluster");
        chord({}, 0, OSCAIRN_CHORD_NONE, 0, "no keys");
    }

    constexpr uint16_t dorian = 1 | 1 << 2 | 1 << 3 | 1 << 5 | 1 << 7 | 1 << 9 | 1 << 10;
    float code(int type, int root, int bass) { return float(type + 100 * root + 10000 * bass); }

    void delivery(const std::string& worker, const std::string& fixture, uint32_t block, bool inWorker)
    {
        const std::string where = std::string(inWorker ? "worker" : "engine") + ", " + std::to_string(block) + "-sample blocks: ";
        oscairn::setPlacementPolicy({inWorker ? worker : std::string(), oscairn::Placement::Worker});
        constexpr double rate = 48000;
        oscairn::Graph g(rate, block, oscairn::ProcessingMode::Offline);
        const auto f = g.add(fixture);
        oscairn::NoteZone up; up.transpose = 2;
        g.setHarmonies({{1, {f}, true, {up}, 2, dorian}});
        g.commit();
        // Keys pressed: C E G at 100 (D major after the zone), E released at 300 (D5), E flat at
        // 500 (D minor). The transport stays stopped.
        struct Press { int64_t at; bool on; int key; };
        const std::vector<Press> presses{{100, true, 60}, {100, true, 64}, {100, true, 67}, {300, false, 64}, {500, true, 63}};
        clap_event_transport_t transport{};
        transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
        transport.tempo = 120;
        std::vector<float> left(block), right(block), chords, scales, beats;
        float* outputs[]{left.data(), right.data()};
        for(int64_t at = 0; at < 1024; at += block)
        {
            std::vector<kick::clap::Event> events;
            for(const auto& p : presses) if(p.at >= at && p.at < at + block) events.push_back(kick::clap::Event::note(p.on, p.key, .8, uint32_t(p.at - at)));
            check(g.process(outputs, block, &transport, events.data(), uint32_t(events.size())), where + "process");
            for(uint32_t c = 0; c < 3; ++c)
            {
                const float* seen = g.audioOutput(f, c);
                auto& into = c == 0 ? chords : c == 1 ? scales : beats;
                if(seen) into.insert(into.end(), seen, seen + block); else into.insert(into.end(), block, -2.f);
            }
        }
        check(scales[0] == float(dorian + 10000 * 2), where + "the part's key, D Dorian, from the first sample: " + std::to_string(scales[0]));
        check(chords[0] == 0 && chords[99] == 0, where + "no chord before a key is held");
        check(chords[100] == code(OSCAIRN_CHORD_MAJOR, 2, 2), where + "D major at sample 100 (C E G through the zone): " + std::to_string(chords[100]));
        check(chords[299] == code(OSCAIRN_CHORD_MAJOR, 2, 2) && chords[300] == code(OSCAIRN_CHORD_POWER, 2, 2), where + "D5 at sample 300: " + std::to_string(chords[300]));
        check(chords[499] == code(OSCAIRN_CHORD_POWER, 2, 2) && chords[500] == code(OSCAIRN_CHORD_MINOR, 2, 2), where + "D minor at sample 500: " + std::to_string(chords[500]));

        // On the main thread, the harmony as of the last block (the fixture saves what it reads).
        const auto dir = std::filesystem::temp_directory_path() / ("oscairn-harmony-" + std::to_string(getpid()));
        std::filesystem::remove_all(dir);
        g.host(f).saveState(dir.string());
        std::ifstream in(dir / "state.clap", std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(in), {}};
        std::filesystem::remove_all(dir);
        oscairn_harmony_t h{};
        check(bytes.size() == sizeof h, where + "the host extension gives the harmony");
        if(bytes.size() == sizeof h) std::memcpy(&h, bytes.data(), sizeof h);
        const auto keys = held({62, 65, 69});
        check(h.key_root == 2 && h.scale == dorian && h.chord_root == 2 && h.chord_type == OSCAIRN_CHORD_MINOR && h.chord_bass == 2 &&
              h.held[0] == keys.held[0] && h.held[1] == keys.held[1], where + "the main thread reads D Dorian, D minor, its keys");
    }

    // Turning a part's input off (or moving its zones off the channel) while keys are down must
    // not leave the harmony holding them: the releases never reach it, and every block would
    // re-send a chord nobody holds.
    void staleKeys(const std::string& fixture)
    {
        oscairn::setPlacementPolicy({"", oscairn::Placement::Worker});
        constexpr double rate = 48000; constexpr uint32_t block = 256;
        for(bool byZones : {false, true})
        {
            const std::string where = std::string(byZones ? "a zone change dropping the channel: " : "the input turned off: ");
            oscairn::Graph g(rate, block, oscairn::ProcessingMode::Offline);
            const auto f = g.add(fixture);
            g.setHarmonies({{1, {f}, true, {}, 0, dorian}});
            g.commit();
            std::vector<float> left(block), right(block);
            float* outputs[]{left.data(), right.data()};
            std::vector<kick::clap::Event> keys{kick::clap::Event::note(true, 53, 1), kick::clap::Event::note(true, 57, 1), kick::clap::Event::note(true, 60, 1)};
            check(g.process(outputs, block, nullptr, keys.data(), uint32_t(keys.size())), where + "process F A C");
            const auto chord = [&](uint32_t at) { return g.audioOutput(f, 0)[at]; };
            check(chord(block - 1) == code(OSCAIRN_CHORD_MAJOR, 5, 5), where + "F major from the held keys: " + std::to_string(chord(block - 1)));
            oscairn::NoteZone otherChannel; otherChannel.channels = 0xFFFE;
            if(byZones) g.setHarmonies({{1, {f}, true, {otherChannel}, 0, dorian}});
            else g.setHarmonies({{1, {f}, false, {}, 0, dorian}});
            g.commit();
            check(g.process(outputs, block, nullptr) && chord(block - 1) == 0, where + "the chord is gone, not re-sent every block: " + std::to_string(chord(block - 1)));
        }
    }
}

int main(int argc, char** argv)
{
    if(argc != 3) return std::fprintf(stderr, "usage: oscairn-harmony-test WORKER follower-fixture.clap\n"), 2;
    recognition();
    for(bool inWorker : {false, true})
        for(uint32_t block : {256u, 37u}) delivery(argv[1], argv[2], block, inWorker);
    staleKeys(argv[2]);
    if(failures) return 1;
    std::printf("ok: common chords and inversions recognized; key, scale and chord changes reach a plugin at their samples, in the engine and a worker\n");
    return 0;
}

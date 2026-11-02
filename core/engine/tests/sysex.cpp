// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0032 step 1: SysEx between plugins. The fixture's source sends notes and SysEx on a note
// wire to its echo, which sends on what it receives; the graph's listener sees what the echo
// sent. At 256- and 37-sample blocks, with both plugins in the engine and both in workers:
//   - a 60 KiB message at sample 37 arrives at sample 37, byte for byte, in order among the
//     notes and the second message at that sample;
//   - a message over the 64 KiB limit is dropped and counted, not truncated;
//   - of five 60 KiB messages at one sample, the four that fit a block's 256 KiB arrive and
//     the fifth is dropped and counted;
//   - nothing is allocated on the audio thread (the engine's side, in workers).
//   oscairn-sysex-test WORKER sysex-fixture.clap
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include "sysex_fixture.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    // What the echo sent: each event's absolute sample, type, key, and for SysEx its bytes. Fixed
    // storage, as the listener runs on the audio thread.
    struct Seen
    {
        int64_t at = 0; uint16_t type = 0; int16_t key = -1; uint32_t offset = 0, size = 0;
    };
    struct Recorder final : oscairn::Graph::Listener
    {
        oscairn::NodeId node = 0;
        int64_t blockStart = 0;
        std::array<Seen, 64> seen{}; uint32_t count = 0;
        std::vector<uint8_t> bytes = std::vector<uint8_t>(1 << 20); uint32_t used = 0;
        bool overflow = false;
        void events(oscairn::NodeId n, const kick::clap::Event* list, uint32_t total, bool emitted) noexcept override
        {
            if(n != node || !emitted) return;
            for(uint32_t i = 0; i < total; ++i)
            {
                const auto* h = list[i].header();
                if(h->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
                if(count == seen.size()) { overflow = true; return; }
                Seen s{blockStart + h->time, h->type};
                if(h->type == CLAP_EVENT_NOTE_ON) s.key = reinterpret_cast<const clap_event_note_t*>(h)->key;
                if(h->type == CLAP_EVENT_MIDI_SYSEX)
                {
                    const auto& x = *reinterpret_cast<const clap_event_midi_sysex_t*>(h);
                    if(used + x.size > bytes.size()) { overflow = true; return; }
                    std::memcpy(bytes.data() + used, x.buffer, x.size);
                    s.offset = used; s.size = x.size; used += x.size;
                }
                seen[count++] = s;
            }
        }
    };

    void run(const std::string& worker, const std::string& fixture, uint32_t block, bool inWorker)
    {
        const std::string where = std::string(inWorker ? "worker" : "engine") + ", " + std::to_string(block) + "-sample blocks: ";
        oscairn::setPlacementPolicy({inWorker ? worker : std::string(), oscairn::Placement::Worker});
        oscairn::Graph g(48000, block, oscairn::ProcessingMode::Offline);
        const auto source = g.add(fixture, "org.oscairn.test.sysex-source"), echo = g.add(fixture, "org.oscairn.test.sysex-echo");
        g.connectNotes(source, echo);
        g.commit();
        Recorder recorder; recorder.node = echo;
        g.setListener(&recorder);
        std::vector<float> left(block), right(block);
        float* outputs[]{left.data(), right.data()};
        clap_event_transport_t transport{};
        transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
        for(int64_t at = 0; at < 2048; at += block)
        {
            recorder.blockStart = at;
            forbidAllocation = true;
            const bool ok = g.process(outputs, block, &transport);
            forbidAllocation = false;
            check(ok, where + "process");
        }
        g.setListener(nullptr);
        check(!recorder.overflow, where + "the recorder's storage held everything");

        std::vector<uint8_t> expected(sysexFixture::limit + 1);
        uint32_t next = 0;
        auto note = [&](int64_t at, int16_t key) {
            const bool ok = next < recorder.count && recorder.seen[next].type == CLAP_EVENT_NOTE_ON && recorder.seen[next].key == key && recorder.seen[next].at == at;
            check(ok, where + "note " + std::to_string(key) + " at " + std::to_string(at) + " in its place");
            ++next;
        };
        auto sysex = [&](int64_t at, uint32_t seed, uint32_t size) {
            sysexFixture::fill(expected.data(), size, seed);
            const auto* s = next < recorder.count ? &recorder.seen[next] : nullptr;
            const bool ok = s && s->type == CLAP_EVENT_MIDI_SYSEX && s->at == at && s->size == size && std::memcmp(recorder.bytes.data() + s->offset, expected.data(), size) == 0;
            check(ok, where + "message " + std::to_string(seed) + " (" + std::to_string(size) + " bytes) at " + std::to_string(at) + ", byte for byte, in its place" +
                      (s ? " (got type " + std::to_string(s->type) + " at " + std::to_string(s->at) + ", " + std::to_string(s->size) + " bytes)" : " (missing)"));
            ++next;
        };
        note(37, 60); sysex(37, 1, sysexFixture::big); note(37, 61); sysex(37, 2, 16);
        for(uint32_t seed = 10; seed < 14; ++seed) sysex(1000, seed, sysexFixture::big);
        check(recorder.count == next, where + std::to_string(recorder.count) + " events arrived, " + std::to_string(next) + " expected: the over-limit ones dropped");
        // The source's host refused the message over 64 KiB and the fifth at sample 1000.
        check(g.host(source).statistics().droppedEvents == 2, where + "two drops counted, got " + std::to_string(g.host(source).statistics().droppedEvents));
        check(g.host(echo).statistics().droppedEvents == 0, where + "nothing dropped at the echo");
    }
}

int main(int argc, char** argv)
{
    if(argc != 3) return std::fprintf(stderr, "usage: oscairn-sysex-test WORKER sysex-fixture.clap\n"), 2;
    for(bool inWorker : {false, true})
        for(uint32_t block : {256u, 37u}) run(argv[1], argv[2], block, inWorker);
    if(failures) return 1;
    std::printf("ok: SysEx delivered in order, byte for byte, at its sample; over-limit messages dropped and counted; engine and worker, two block sizes\n");
    return 0;
}

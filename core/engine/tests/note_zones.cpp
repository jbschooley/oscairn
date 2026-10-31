// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0011: zones on note connections. The notes a zone passes reach a transpose plugin set to
// 0 semitones, whose output shows exactly what arrived.
//   oscairn-note-zones-test TRANSPOSE.clap
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace oscairn;
using kick::clap::Event;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }

    struct Arrived { bool on; int key, channel; double velocity; };
    struct Probe
    {
        Graph graph{48000, 128};
        NodeId node;
        std::array<float, 128> l{}, r{};
        float* out[2]{l.data(), r.data()};
        Probe(const char* plugin, const std::vector<NoteZone>& zones) : node(graph.add(plugin))
        {
            graph.connectNotes(graphInput, node, zones); graph.commit();
        }
        std::vector<Arrived> play(std::vector<Event> events)
        {
            forbidAllocation = true;
            const bool ok = graph.process(out, 128, nullptr, events.data(), uint32_t(events.size()));
            forbidAllocation = false;
            check(ok, "render");
            std::vector<Arrived> result;
            uint32_t n = 0; const auto* e = graph.host(node).outputEvents(n);
            for(uint32_t i = 0; i < n; ++i)
            {
                const auto* h = e[i].header();
                if(h->space_id != CLAP_CORE_EVENT_SPACE_ID || (h->type != CLAP_EVENT_NOTE_ON && h->type != CLAP_EVENT_NOTE_OFF)) continue;
                const auto& note = *reinterpret_cast<const clap_event_note_t*>(h);
                result.push_back({h->type == CLAP_EVENT_NOTE_ON, note.key, note.channel, note.velocity});
            }
            return result;
        }
    };
    std::vector<int> keys(const std::vector<Arrived>& a, bool on) { std::vector<int> k; for(const auto& x : a) if(x.on == on) k.push_back(x.key); return k; }
}

int main(int argc, char** argv)
{
    if(argc < 2) return 2;
    const char* transpose = argv[1];
    try
    {
        { // A split: only the low keys pass, and the high key's release is dropped with it.
            NoteZone low; low.highKey = 59;
            Probe p(transpose, {low});
            auto a = p.play({Event::note(true, 48, .8, 0), Event::note(true, 72, .8, 1)});
            check(keys(a, true) == std::vector<int>{48}, "the low zone passes 48 and drops 72");
            a = p.play({Event::note(false, 48, 0, 0), Event::note(false, 72, 0, 1)});
            check(keys(a, false) == std::vector<int>{48}, "only the passed note is released");
        }
        { // Two zones layer one instrument an octave apart; one release ends both.
            NoteZone same, octave; octave.transpose = 12;
            Probe p(transpose, {same, octave});
            auto a = p.play({Event::note(true, 60, .5, 0)});
            check(keys(a, true) == std::vector<int>{60, 72}, "a key plays in each zone");
            a = p.play({Event::note(false, 60, 0, 0)});
            check(keys(a, false) == std::vector<int>{60, 72}, "its release ends both");
            // MIDI notes too, with the oldest-first release per zone.
            a = p.play({Event::midi(0x90, 64, 100), Event::midi(0x90, 64, 100, 1)});
            check(keys(a, true) == std::vector<int>{64, 76, 64, 76}, "MIDI note-ons layer");
            a = p.play({Event::midi(0x80, 64, 0)});
            check(keys(a, false) == std::vector<int>{64, 76}, "one MIDI release ends the oldest note in each zone");
            a = p.play({Event::midi(0x80, 64, 0)});
            check(keys(a, false) == std::vector<int>{64, 76}, "the second ends the other");
        }
        { // Velocity range, curve and fixed velocity.
            NoteZone hard; hard.lowVelocity = .5; hard.curve = 2;
            NoteZone fixed; fixed.lowKey = 100; fixed.fixedVelocity = .7;
            Probe p(transpose, {hard, fixed});
            const auto a = p.play({Event::note(true, 60, .4, 0), Event::note(true, 62, .8, 1), Event::note(true, 100, .1, 2)});
            check(a.size() == 2 && a[0].key == 62 && std::abs(a[0].velocity - .64) < 1e-9, "below the range dropped; .8 curved to .64");
            check(a.size() == 2 && a[1].key == 100 && std::abs(a[1].velocity - .7) < 1e-9, "a fixed velocity zone");
        }
        { // MIDI channel filter.
            NoteZone second; second.channels = 1u << 1;
            Probe p(transpose, {second});
            const auto a = p.play({Event::note(true, 60, .8, 0, 0), Event::note(true, 61, .8, 1, 1), Event::midi(0x90, 62, 100), Event::midi(0x91, 63, 100, 2)});
            check(keys(a, true) == std::vector<int>{61, 63}, "only channel 2 passes, CLAP and MIDI notes");
        }
        { // Editing the zone while a note is held still releases the key it played.
            NoteZone up; up.transpose = 12;
            Probe p(transpose, {up});
            auto a = p.play({Event::note(true, 60, .8, 0)});
            check(keys(a, true) == std::vector<int>{72}, "transposed up an octave");
            p.graph.connectNotes(graphInput, p.node, {}); p.graph.commit();
            a = p.play({Event::note(false, 60, 0, 0), Event::note(true, 60, .8, 1)});
            check(keys(a, false) == std::vector<int>{72} && keys(a, true) == std::vector<int>{60}, "the held note's release finds 72; new notes play unchanged");
            check(p.graph.describe().noteZones.empty(), "no zones described once removed");
        }
        { // Invalid zones are refused.
            NoteZone bad; bad.lowKey = 80; bad.highKey = 40;
            Graph g(48000, 128); const auto n = g.add(transpose);
            bool refused = false; try { g.connectNotes(graphInput, n, {bad}); } catch(const std::invalid_argument&) { refused = true; }
            check(refused, "an inverted key range is refused");
            refused = false; try { g.connectNotes(graphInput, n, std::vector<NoteZone>(9)); } catch(const std::invalid_argument&) { refused = true; }
            check(refused, "more than eight zones are refused");
        }
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: note zones split, layer, shape velocity, filter channels and keep releases");
    return failures ? 1 : 0;
}

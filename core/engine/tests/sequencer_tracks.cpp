// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../../../plugins/step-sequencer/Parameters.h"
#include "oscairn/transport.h"
#include "../../clap-host/tests/AllocationGuard.h"
#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>
#include <oscairn-clap/param-target.h>
#include <map>
#include <algorithm>
#include <set>
#include <iostream>
using namespace oscairn;
namespace ids = oscairn::sequencer;
static void check(bool ok, const char *why)
{
    if (!ok)
        throw std::runtime_error(why);
}
struct Note
{
    int64_t time;
    int type, channel, key, id;
    bool operator==(const Note &) const = default;
};
static kick::clap::Event parameter(uint32_t id, double value, uint32_t time = 0)
{
    clap_event_param_value_t e{};
    e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    e.param_id = id;
    e.note_id = e.port_index = e.channel = e.key = -1;
    e.value = value;
    return kick::clap::Event::from(e);
}
static std::vector<Note> render(const kick::clap::Descriptor &plugin, uint32_t block, int beats = 4)
{
    kick::clap::Host host;
    check(host.load(plugin, 48000, 512), "load");
    check(host.setParameter(ids::mute(1), 0), "unmute second track");
    check(host.setParameter(ids::lockScope(0), 1), "channel-scoped first-track locks");
    check(host.setParameter(ids::lockScope(1), 0), "explicit global second-track locks");
    for (uint32_t p = 0; p < 2; ++p)
        for (uint32_t t = 0; t < 2; ++t)
        {
            check(host.setParameter(ids::length(p, t), p ? (t ? 4 : 2) : (t ? 5 : 3)), "length");
            for (uint32_t i = 0; i < 16; ++i)
                check(host.setParameter(ids::step(p, t, ids::Note, i), 40 + p * 20 + t * 10 + i), "note");
        }
    std::multimap<int64_t, kick::clap::Event> changes{{1000, parameter(ids::step(1, 0, ids::Note, 0), 99)}, // editing B must not change A
                                                      {7000, parameter(ids::mute(1), 1)},
                                                      {17000, parameter(ids::mute(1), 0)},
                                                      {13000, parameter(ids::channel(0), 2)},
                                                      {20000, parameter(ids::requestedPattern, 1)},
                                                      {30000, parameter(ids::requestedPattern, 0)},
                                                      {35000, parameter(ids::requestedPattern, 1)}};
    std::vector<Note> result;
    std::map<int, std::pair<int, int>> held;
    Transport transport(48000, 120);
    const int64_t boundary = beats * 24000;
    for (int64_t now = 0; now < 2 * boundary;)
    {
        const auto frames = uint32_t(std::min<int64_t>(block, 2 * boundary - now));
        std::vector<kick::clap::Event> input;
        for (auto it = changes.lower_bound(now); it != changes.end() && it->first < now + frames; ++it)
        {
            input.push_back(it->second);
            reinterpret_cast<clap_event_header_t *>(input.back().bytes.data())->time = uint32_t(it->first - now);
        }
        auto t = transport.current();
        t.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        t.tsig_num = uint16_t(beats);
        t.tsig_denom = 4;
        t.bar_start = int64_t((now / boundary) * beats) * CLAP_BEATTIME_FACTOR;
        forbidAllocation = true;
        const bool ok = host.process(nullptr, 0, nullptr, 0, frames, input.data(), uint32_t(input.size()), &t);
        forbidAllocation = false;
        check(ok, "render");
        uint32_t count;
        const auto *events = host.outputEvents(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto *h = events[i].header();
            if (h->space_id == kick::clap::Host::paramTargetSpace)
            {
                const auto &e = *reinterpret_cast<const oscairn_event_param_target_t *>(h);
                check(e.channel == (e.lane == 1 ? -1 : now + h->time < 13000 ? 0 : 2), "explicit channel/global lock scope");
                continue;
            }
            if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
                continue;
            if (h->type == CLAP_EVENT_PARAM_VALUE)
            {
                const auto &e = *reinterpret_cast<const clap_event_param_value_t *>(h);
                if (e.param_id == ids::activePattern && e.value == 1)
                    check(now + h->time == boundary, "pattern launches on exact bar boundary");
                continue;
            }
            if (h->type != CLAP_EVENT_NOTE_ON && h->type != CLAP_EVENT_NOTE_OFF)
                continue;
            const auto &e = *reinterpret_cast<const clap_event_note_t *>(h);
            const auto at = now + h->time;
            result.push_back({at, h->type, e.channel, e.key, e.note_id});
            if (h->type == CLAP_EVENT_NOTE_ON)
            {
                check(e.note_id >= 0 && !held.contains(e.note_id), "distinct live note IDs");
                held[e.note_id] = {e.channel, e.key};
                check(at % 6000 == 0, "exact step timing");
                const int track = e.channel == 1 ? 1 : 0;
                check(e.channel == (track ? 1 : at < 13000 ? 0 : 2), "track channel");
                check(!(track && at == 12000), "muted track did not play");
                const bool b = at >= boundary;
                const auto phase = (at - (b ? boundary : 0)) / 6000;
                const auto step = phase % (b ? (track ? 4 : 2) : (track ? 5 : 3));
                check(e.key == (b && !track && !step ? 99 : 40 + 20 * b + 10 * track + step),
                      "independent lengths, inactive edits and pattern reset");
            }
            else
            {
                check(held.contains(e.note_id) && held.at(e.note_id) == std::pair<int, int>{e.channel, e.key},
                      "release preserves note identity/channel");
                held.erase(e.note_id);
            }
        }
        transport.advance(frames);
        now += frames;
    }
    check(held.empty(), "all notes released");
    check(host.parameterValue(ids::activePattern) == 1, "playing pattern readback");
    check(std::count_if(result.begin(), result.end(),
                        [](const auto &n) { return n.time == 7000 && n.type == CLAP_EVENT_NOTE_OFF && n.channel == 1; }) == 1,
          "mute releases immediately");
    check(std::count_if(result.begin(), result.end(),
                        [](const auto &n) { return n.time == 13000 && n.type == CLAP_EVENT_NOTE_OFF && n.channel == 0; }) == 1,
          "channel edit releases on old channel");
    transport.setPlaying(false);
    host.setParameter(ids::requestedPattern, 0);
    auto t = transport.current();
    check(host.process(nullptr, 0, nullptr, 0, block, nullptr, 0, &t), "stopped processing");
    check(host.parameterValue(ids::activePattern) == 0 && host.parameterValue(ids::playhead(0)) == -1, "stopped selection is immediate");
    return result;
}
static void sameKey(const kick::clap::Descriptor &plugin)
{
    kick::clap::Host host;
    check(host.load(plugin, 48000, 512), "same-key load");
    host.setParameter(ids::mute(1), 0);
    host.setParameter(ids::channel(1), 0);
    for (uint32_t track = 0; track < 2; ++track)
        host.setParameter(ids::step(0, track, ids::Note, 0), 60);
    Transport transport(48000, 120);
    auto t = transport.current();
    check(host.process(nullptr, 0, nullptr, 0, 512, nullptr, 0, &t), "same-key start");
    std::vector<int32_t> voices;
    uint32_t count = 0;
    auto *events = host.outputEvents(count);
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].header()->space_id == CLAP_CORE_EVENT_SPACE_ID && events[i].header()->type == CLAP_EVENT_NOTE_ON)
            voices.push_back(reinterpret_cast<const clap_event_note_t *>(events[i].header())->note_id);
    check(voices.size() == 2 && voices[0] != voices[1], "same channel/key tracks have distinct ownership");
    transport.advance(512);
    t = transport.current();
    const auto mute = parameter(ids::mute(1), 1, 88);
    check(host.process(nullptr, 0, nullptr, 0, 512, &mute, 1, &t), "same-key mute");
    events = host.outputEvents(count);
    int offs = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].header()->space_id == CLAP_CORE_EVENT_SPACE_ID && events[i].header()->type == CLAP_EVENT_NOTE_OFF)
        {
            const auto &e = *reinterpret_cast<const clap_event_note_t *>(events[i].header());
            check(e.note_id == voices[1] && e.channel == 0 && e.key == 60 && e.header.time == 88,
                  "mute releases only its own same-key voice");
            ++offs;
        }
    check(offs == 1, "one track release");
    transport.setPlaying(false);
    t = transport.current();
    check(host.process(nullptr, 0, nullptr, 0, 512, nullptr, 0, &t), "same-key stop");
    events = host.outputEvents(count);
    offs = 0;
    for (uint32_t i = 0; i < count; ++i)
        if (events[i].header()->space_id == CLAP_CORE_EVENT_SPACE_ID && events[i].header()->type == CLAP_EVENT_NOTE_OFF)
        {
            check(reinterpret_cast<const clap_event_note_t *>(events[i].header())->note_id == voices[0], "stop releases remaining track");
            ++offs;
        }
    check(offs == 1, "stop releases remaining note once");
}
int main(int argc, char **argv)
{
    try
    {
        check(argc == 2, "sequencer path required");
        const auto scan = kick::clap::inspect(argv[1]);
        check(!scan.plugins.empty(), "scan");
        std::set<uint32_t> idsSeen;
        for (uint32_t i = 0; i < ids::parameterCount; ++i)
            check(idsSeen.insert(ids::parameterId(i)).second && ids::parameterIndex(ids::parameterId(i)) == i,
                  "parameter IDs unique and roundtrip");
        for (int beats : {3, 4})
            check(render(scan.plugins.front(), 37, beats) == render(scan.plugins.front(), 512, beats),
                  "event stream independent of block size");
        sameKey(scan.plugins.front());
        std::cout
            << "ok: two tracks, independent lengths/channels, exact mutes, A/B launch/cancel in 3/4 and 4/4, allocation-free blocks\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

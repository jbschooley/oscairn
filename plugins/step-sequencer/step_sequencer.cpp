// SPDX-License-Identifier: AGPL-3.0-or-later
// Two tracks, two editable patterns. Requests launch at a bar boundary; note releases
// retain their original ID/channel across edits, mutes and pattern changes.
#include "Parameters.h"
#include <clap/clap.h>
#include <oscairn-clap/param-target.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    namespace ids = oscairn::sequencer;
    constexpr std::array<int16_t, 16> notes{45, 57, 52, 57, 48, 60, 55, 60, 43, 55, 50, 55, 47, 59, 52, 59};
    constexpr std::array<double, 16> locks{.8, .2, .5, .35, .8, .2, .5, .35, .65, .15, .4, .25, .65, .15, .4, .25};
    constexpr double stepBeats = .25, gateLength = .5;
    const char *const features[]{CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr};
    const clap_plugin_descriptor_t descriptor{CLAP_VERSION_INIT,
                                              "org.oscairn.step-sequencer",
                                              "Oscairn Step Sequencer",
                                              "Oscairn",
                                              "https://github.com/jbschooley/oscairn",
                                              "",
                                              "",
                                              "0.2.0",
                                              "Two tracks and two patterns following the host transport",
                                              features};

    bool paramsInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info)
    {
        if (index >= ids::parameterCount)
            return false;
        *info = {};
        info->id = ids::parameterId(index);
        info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
        if (index < 2)
        {
            info->max_value = 1;
            std::snprintf(info->name, sizeof(info->name), "%s", index ? "Playing pattern" : "Launch pattern");
            if (index)
                info->flags = CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
            return true;
        }
        index -= 2;
        if (index < 4 * ids::tracks)
        {
            const auto track = index / 4, kind = index % 4;
            std::snprintf(info->module, sizeof(info->module), "Track %u", track + 1);
            if (kind == 0)
            {
                info->max_value = 15;
                info->default_value = track;
                std::strcpy(info->name, "MIDI channel");
            }
            else if (kind == 1)
            {
                info->max_value = 1;
                info->default_value = track ? 1 : 0;
                std::strcpy(info->name, "Mute");
            }
            else if (kind == 3)
            {
                info->max_value = 1;
                info->default_value = 0;
                std::strcpy(info->name, "Channel-scoped locks");
            }
            else
            {
                info->min_value = -1;
                info->max_value = 15;
                info->default_value = -1;
                info->flags = CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_STEPPED;
                std::strcpy(info->name, "Playing step");
            }
            return true;
        }
        index -= 4 * ids::tracks;
        const auto bank = index / ids::parametersPerTrack, member = index % ids::parametersPerTrack;
        const auto pattern = bank / ids::tracks, track = bank % ids::tracks;
        std::snprintf(info->module, sizeof(info->module), "Pattern %c/Track %u", 'A' + pattern, track + 1);
        if (!member)
        {
            info->min_value = 1;
            info->max_value = ids::steps;
            info->default_value = ids::steps;
            std::strcpy(info->name, "Length");
            return true;
        }
        const auto kind = (member - 1) / ids::steps, step = (member - 1) % ids::steps;
        if (kind == 0)
        {
            info->max_value = 1;
            info->default_value = 1;
            std::snprintf(info->name, sizeof(info->name), "Step %u On", step + 1);
        }
        else if (kind == 1)
        {
            info->max_value = 127;
            info->default_value = notes[step] + 12 * track + 5 * pattern;
            std::snprintf(info->name, sizeof(info->name), "Step %u Note", step + 1);
        }
        else
        {
            info->max_value = 1;
            info->default_value = locks[step];
            info->flags = CLAP_PARAM_IS_AUTOMATABLE;
            std::snprintf(info->name, sizeof(info->name), "Step %u Lock", step + 1);
        }
        return true;
    }
    struct Held
    {
        int64_t step = -1;
        int16_t key = -1, channel = 0;
        int32_t id = -1;
    };
    struct Sequencer
    {
        clap_plugin_t plugin{};
        const clap_host_t *host = nullptr;
        bool canLock = false;
        uint16_t lockSpace = 0;
        double sampleRate = 48000;
        std::array<std::atomic<double>, ids::parameterCount> values{};
        std::array<Held, ids::tracks> held{};
        std::array<bool, ids::tracks + 1> readbackDirty{};
        int64_t lastStep = -1, patternStart = 0;
        double launchBeat = -1;
        uint32_t queued = 0, nextNote = 0;
        Sequencer()
        {
            for (uint32_t i = 0; i < values.size(); ++i)
            {
                clap_param_info_t info{};
                paramsInfo(nullptr, i, &info);
                values[i] = info.default_value;
            }
        }
        double value(uint32_t id) const
        {
            return values[ids::parameterIndex(id)].load(std::memory_order_relaxed);
        }
    };
    Sequencer &self(const clap_plugin_t *p)
    {
        return *static_cast<Sequencer *>(p->plugin_data);
    }
    void applyParam(Sequencer &s, const clap_event_header_t *h)
    {
        if (!h || h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE ||
            h->size < sizeof(clap_event_param_value_t))
            return;
        const auto &e = *reinterpret_cast<const clap_event_param_value_t *>(h);
        const auto index = ids::parameterIndex(e.param_id);
        clap_param_info_t info{};
        if (!paramsInfo(nullptr, index, &info) || (info.flags & CLAP_PARAM_IS_READONLY) || !std::isfinite(e.value) || e.note_id != -1 ||
            e.port_index != -1 || e.channel != -1 || e.key != -1)
            return;
        auto value = std::clamp(e.value, info.min_value, info.max_value);
        if (info.flags & CLAP_PARAM_IS_STEPPED)
            value = std::round(value);
        s.values[index] = value;
    }
    void readback(Sequencer &s, uint32_t id, double value, uint32_t time, const clap_output_events_t *out)
    {
        const auto index = ids::parameterIndex(id), dirty = id == ids::activePattern ? 0 : (id - ids::playhead(0)) / 10 + 1;
        if (s.values[index].load() == value && !s.readbackDirty[dirty])
            return;
        s.values[index] = value;
        clap_event_param_value_t e{};
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = id;
        e.value = value;
        e.note_id = e.port_index = e.channel = e.key = -1;
        s.readbackDirty[dirty] = !out->try_push(out, &e.header);
    }
    bool pushNote(const clap_output_events_t *out, uint32_t time, bool on, const Held &note, double velocity)
    {
        clap_event_note_t e{};
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, uint16_t(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
        e.note_id = note.id;
        e.port_index = 0;
        e.channel = note.channel;
        e.key = note.key;
        e.velocity = velocity;
        return out->try_push(out, &e.header);
    }
    bool release(Sequencer &s, uint32_t track, uint32_t time, const clap_output_events_t *out)
    {
        auto &held = s.held[track];
        if (held.key < 0)
            return true;
        if (!pushNote(out, time, false, held, 0))
            return false;
        held = {};
        return true;
    }
    bool pushLock(const Sequencer &s, const clap_output_events_t *out, uint32_t time, uint32_t track, int16_t channel, double value)
    {
        oscairn_event_param_target_t e{};
        e.header = {sizeof(e), time, s.lockSpace, OSCAIRN_EVENT_PARAM_TARGET, 0};
        e.lane = track;
        e.mode = OSCAIRN_PARAM_TARGET_SET;
        e.note_id = e.port_index = e.key = -1;
        e.channel = s.value(ids::lockScope(track)) ? channel : -1;
        e.value = value;
        return out->try_push(out, &e.header);
    }
    void stop(Sequencer &s)
    {
        s.lastStep = -1;
        s.patternStart = 0;
        s.launchBeat = -1;
        s.held = {};
    }
    clap_process_status process(const clap_plugin_t *p, const clap_process_t *block)
    {
        auto &s = self(p);
        const auto *t = block->transport;
        const auto frames = block->frames_count;
        const auto *out = block->out_events;
        const auto *in = block->in_events;
        const uint32_t count = in ? in->size(in) : 0;
        uint32_t input = 0;
        const bool playing = t && (t->flags & CLAP_TRANSPORT_IS_PLAYING) && (t->flags & CLAP_TRANSPORT_HAS_TEMPO) &&
                             (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && std::isfinite(t->tempo) && t->tempo >= 1 && t->tempo <= 1000;
        auto applyUntil = [&](uint32_t time)
        {
            while (input < count)
            {
                const auto *h = in->get(in, input);
                if (h && h->time > time)
                    break;
                applyParam(s, h);
                ++input;
            }
        };
        if (!playing)
        {
            applyUntil(frames);
            for (uint32_t track = 0; track < ids::tracks; ++track)
                if (!release(s, track, 0, out))
                    return CLAP_PROCESS_CONTINUE;
            stop(s);
            readback(s, ids::activePattern, s.value(ids::requestedPattern), 0, out);
            for (uint32_t track = 0; track < ids::tracks; ++track)
                readback(s, ids::playhead(track), -1, 0, out);
            return CLAP_PROCESS_CONTINUE;
        }
        const double beatStart = double(t->song_pos_beats) / CLAP_BEATTIME_FACTOR, samplesPerBeat = s.sampleRate * 60 / t->tempo;
        auto offset = [&](double beat) -> uint32_t
        {
            const auto v = (beat - beatStart) * samplesPerBeat;
            return v <= 0 ? 0 : v >= frames ? frames : uint32_t(std::llround(v));
        };
        int64_t next = s.lastStep + 1;
        const bool starting = s.lastStep < 0;
        if (starting || next * stepBeats < beatStart - stepBeats || s.lastStep * stepBeats > beatStart + 1e-9)
        {
            for (uint32_t track = 0; track < ids::tracks; ++track)
                if (!release(s, track, 0, out))
                    return CLAP_PROCESS_CONTINUE;
            const auto current = int64_t(std::floor(beatStart / stepBeats + 1e-9));
            next = std::max<int64_t>(beatStart - current * stepBeats < gateLength * stepBeats ? current : current + 1, 0);
            if (!starting)
            {
                s.patternStart = 0;
                s.launchBeat = -1;
            }
        }
        double barBeats = 4;
        if ((t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) && t->tsig_num && t->tsig_denom)
            barBeats = double(t->tsig_num) * 4 / t->tsig_denom;
        const double barStart = double(t->bar_start) / CLAP_BEATTIME_FACTOR;
        uint32_t time = 0;
        // Bound adversarial hosts; normal audio rates/blocks and the host's event limits fit comfortably.
        for (uint32_t work = 0; work < 4096; ++work)
        {
            for (uint32_t track = 0; track < ids::tracks; ++track)
                if (s.held[track].key >= 0 && offset((s.held[track].step + gateLength) * stepBeats) <= time &&
                    !release(s, track, time, out))
                    return CLAP_PROCESS_CONTINUE;
            applyUntil(time);
            for (uint32_t track = 0; track < ids::tracks; ++track)
                if (s.held[track].key >= 0 && (s.value(ids::mute(track)) || s.held[track].channel != s.value(ids::channel(track))) &&
                    !release(s, track, time, out))
                    return CLAP_PROCESS_CONTINUE;
            const auto request = uint32_t(s.value(ids::requestedPattern));
            if (request == s.value(ids::activePattern))
                s.launchBeat = -1;
            else if (starting && time == 0)
            {
                readback(s, ids::activePattern, request, time, out);
                s.launchBeat = -1;
            }
            else if (s.launchBeat < 0 || s.queued != request)
            {
                const double beat = beatStart + time / samplesPerBeat;
                s.launchBeat = barStart + std::ceil((beat - barStart) / barBeats - 1e-9) * barBeats;
                s.queued = request;
            }
            if (s.launchBeat >= 0 && offset(s.launchBeat) <= time)
            {
                for (uint32_t track = 0; track < ids::tracks; ++track)
                    if (!release(s, track, time, out))
                        return CLAP_PROCESS_CONTINUE;
                s.patternStart = next = std::max<int64_t>(0, int64_t(std::ceil(s.launchBeat / stepBeats - 1e-9)));
                readback(s, ids::activePattern, request, time, out);
                s.launchBeat = -1;
            }
            readback(s, ids::activePattern, s.value(ids::activePattern), time, out);
            if (offset(next * stepBeats) <= time)
            {
                for (uint32_t track = 0; track < ids::tracks; ++track)
                    if (!release(s, track, time, out))
                        return CLAP_PROCESS_CONTINUE;
                const auto pattern = uint32_t(s.value(ids::activePattern));
                s.lastStep = next;
                for (uint32_t track = 0; track < ids::tracks; ++track)
                {
                    const auto length = int64_t(s.value(ids::length(pattern, track)));
                    const auto step = uint32_t(((next - s.patternStart) % length + length) % length);
                    readback(s, ids::playhead(track), step, time, out);
                    if (s.value(ids::mute(track)) || !s.value(ids::step(pattern, track, ids::On, step)))
                        continue;
                    Held held{next, int16_t(s.value(ids::step(pattern, track, ids::Note, step))), int16_t(s.value(ids::channel(track))),
                              int32_t(s.nextNote)};
                    s.nextNote = (s.nextNote + 1) & 0x7fffffff;
                    if (s.canLock && !pushLock(s, out, time, track, held.channel, s.value(ids::step(pattern, track, ids::Lock, step))))
                        return CLAP_PROCESS_CONTINUE;
                    if (!pushNote(out, time, true, held, step % 4 == 0 ? 1 : .7))
                        return CLAP_PROCESS_CONTINUE;
                    s.held[track] = held;
                }
                ++next;
            }
            uint32_t due = offset(next * stepBeats);
            for (uint32_t track = 0; track < ids::tracks; ++track)
            {
                if (s.held[track].key >= 0)
                    due = std::min(due, offset((s.held[track].step + gateLength) * stepBeats));
                readback(s, ids::playhead(track), s.value(ids::playhead(track)), time, out);
            }
            if (input < count)
            {
                const auto *h = in->get(in, input);
                due = std::min(due, h ? h->time : time);
            }
            if (s.launchBeat >= 0)
                due = std::min(due, offset(s.launchBeat));
            if (due >= frames)
                return CLAP_PROCESS_CONTINUE;
            time = std::max(time, due);
        }
        return CLAP_PROCESS_ERROR;
    }
    const clap_plugin_note_ports_t notePorts{[](const clap_plugin_t *, bool input) -> uint32_t { return input ? 0 : 1; },
                                             [](const clap_plugin_t *, uint32_t index, bool input, clap_note_port_info_t *info) -> bool
                                             {
                                                 if (input || index)
                                                     return false;
                                                 *info = {};
                                                 info->id = 0;
                                                 info->supported_dialects = info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
                                                 std::strcpy(info->name, "Tracks");
                                                 return true;
                                             }};
    const clap_plugin_params_t params{[](const clap_plugin_t *) -> uint32_t { return ids::parameterCount; },
                                      paramsInfo,
                                      [](const clap_plugin_t *p, clap_id id, double *value) -> bool
                                      {
                                          const auto index = ids::parameterIndex(id);
                                          if (index == ids::parameterCount)
                                              return false;
                                          *value = self(p).values[index];
                                          return true;
                                      },
                                      [](const clap_plugin_t *, clap_id id, double value, char *text, uint32_t size) -> bool
                                      {
                                          clap_param_info_t info{};
                                          if (!paramsInfo(nullptr, ids::parameterIndex(id), &info))
                                              return false;
                                          if (id == ids::requestedPattern || id == ids::activePattern)
                                              std::snprintf(text, size, "%c", value >= .5 ? 'B' : 'A');
                                          else if (id == ids::channel(0) || id == ids::channel(1))
                                              std::snprintf(text, size, "%.0f", value + 1);
                                          else if (!(info.flags & CLAP_PARAM_IS_STEPPED))
                                              std::snprintf(text, size, "%.0f%%", value * 100);
                                          else
                                              std::snprintf(text, size, "%.0f", value);
                                          return true;
                                      },
                                      [](const clap_plugin_t *, clap_id id, const char *text, double *value) -> bool
                                      {
                                          clap_param_info_t info{};
                                          if (!paramsInfo(nullptr, ids::parameterIndex(id), &info) || (info.flags & CLAP_PARAM_IS_READONLY))
                                              return false;
                                          if (id == ids::requestedPattern && (std::strcmp(text, "A") == 0 || std::strcmp(text, "B") == 0))
                                              *value = text[0] == 'B';
                                          else
                                          {
                                              char *end = nullptr;
                                              *value = std::strtod(text, &end);
                                              if (end == text || !std::isfinite(*value))
                                                  return false;
                                              if (id == ids::channel(0) || id == ids::channel(1))
                                                  *value -= 1;
                                              else if (!(info.flags & CLAP_PARAM_IS_STEPPED))
                                                  *value /= 100;
                                          }
                                          *value = std::clamp(*value, info.min_value, info.max_value);
                                          if (info.flags & CLAP_PARAM_IS_STEPPED)
                                              *value = std::round(*value);
                                          return true;
                                      },
                                      [](const clap_plugin_t *p, const clap_input_events_t *in, const clap_output_events_t *)
                                      {
                                          for (uint32_t i = 0; i < in->size(in); ++i)
                                              applyParam(self(p), in->get(in, i));
                                      }};
    const oscairn_plugin_param_target_lanes_t lanes{
        [](const clap_plugin_t *) -> uint32_t { return ids::tracks; },
        [](const clap_plugin_t *, uint32_t index, oscairn_param_target_lane_info_t *info) -> bool
        {
            if (index >= ids::tracks)
                return false;
            info->lane = index;
            std::snprintf(info->name, sizeof(info->name), "Track %u Lock", index + 1);
            return true;
        }};
    const clap_plugin_t *create(const clap_host_t *host)
    {
        auto *s = new Sequencer;
        s->host = host;
        s->plugin.desc = &descriptor;
        s->plugin.plugin_data = s;
        s->plugin.init = [](const clap_plugin_t *p)
        {
            auto &s = self(p);
            const auto *r = static_cast<const clap_host_event_registry_t *>(s.host->get_extension(s.host, CLAP_EXT_EVENT_REGISTRY));
            s.canLock = r && r->query && r->query(s.host, OSCAIRN_EVENT_SPACE_PARAM_TARGET, &s.lockSpace);
            return true;
        };
        s->plugin.destroy = [](const clap_plugin_t *p) { delete &self(p); };
        s->plugin.activate = [](const clap_plugin_t *p, double rate, uint32_t, uint32_t)
        {
            if (!std::isfinite(rate) || rate < 8000)
                return false;
            self(p).sampleRate = rate;
            stop(self(p));
            return true;
        };
        s->plugin.deactivate = [](const clap_plugin_t *) {};
        s->plugin.start_processing = [](const clap_plugin_t *) { return true; };
        s->plugin.stop_processing = [](const clap_plugin_t *) {};
        s->plugin.reset = [](const clap_plugin_t *p) { stop(self(p)); };
        s->plugin.process = process;
        s->plugin.get_extension = [](const clap_plugin_t *, const char *id) -> const void *
        {
            if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
                return &notePorts;
            if (!std::strcmp(id, CLAP_EXT_PARAMS))
                return &params;
            if (!std::strcmp(id, OSCAIRN_EXT_PARAM_TARGET_LANES))
                return &lanes;
            return nullptr;
        };
        s->plugin.on_main_thread = [](const clap_plugin_t *) {};
        return &s->plugin;
    }
    const clap_plugin_factory_t factory{[](const clap_plugin_factory_t *) -> uint32_t { return 1; },
                                        [](const clap_plugin_factory_t *, uint32_t index) -> const clap_plugin_descriptor_t *
                                        { return index ? nullptr : &descriptor; },
                                        [](const clap_plugin_factory_t *, const clap_host_t *host, const char *id) -> const clap_plugin_t *
                                        { return !std::strcmp(id, descriptor.id) ? create(host) : nullptr; }};
} // namespace
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, [](const char *) { return true; }, []() {},
                                                            [](const char *id) -> const void *
                                                            { return !std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr; }};

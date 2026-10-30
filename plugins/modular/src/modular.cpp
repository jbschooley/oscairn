// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Modular: chillbeast's Modular instrument engine (../upstream) as a CLAP instrument.
// Notes, pitch bend, mod wheel, pressure and the sustain pedal reach the engine at their
// exact sample: the block is rendered in pieces between events. Parameters are the
// engine's eight macros and level, plus a stepped Preset that loads a factory patch.
// State is the parameter values and the patch as XML.
//
// The engine evaluates modulation on a 16-sample grid that restarts at every render call,
// so output depends slightly on where blocks and events fall (fix to be offered upstream).
#include "prefix.h"
#include <clap/clap.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.modular",
        "Oscairn Modular",
        "Oscairn",
        "https://github.com/jbschooley/oscairn",
        "",
        "",
        "0.1.0",
        "A patchable modular synth by chillbeast",
        features,
    };

    constexpr clap_id presetParam = 100;
    constexpr float bendSemitones = 2.f; // the engine's bend range

    struct Modular
    {
        clap_plugin_t plugin{};
        const clap_host_t* host = nullptr;
        std::unique_ptr<mt::ModularEngine> engine;
        std::vector<mt::InstrumentParam> info;
        std::vector<std::atomic<double>> values;
        juce::StringArray presetNames;
        std::atomic<int> preset{0}, pendingPreset{-1};
        juce::ValueTree patch; // main thread
        std::array<bool, 128> sustained{};
        bool sustain = false;
    };
    Modular& self(const clap_plugin_t* p) { return *static_cast<Modular*>(p->plugin_data); }

    void apply(Modular& s, const clap_event_header_t* h)
    {
        if(h->space_id != CLAP_CORE_EVENT_SPACE_ID) return;
        auto& e = *s.engine;
        auto off = [&](int key) {
            if(key < 0 || key > 127) return;
            if(s.sustain) s.sustained[(size_t)key] = true;
            else e.noteOff(key);
        };
        switch(h->type)
        {
        case CLAP_EVENT_NOTE_ON:
        {
            const auto& n = *reinterpret_cast<const clap_event_note_t*>(h);
            if(n.key >= 0 && n.key < 128) { s.sustained[(size_t)n.key] = false; e.noteOn(n.key, float(n.velocity)); }
            break;
        }
        case CLAP_EVENT_NOTE_OFF: off(reinterpret_cast<const clap_event_note_t*>(h)->key); break;
        case CLAP_EVENT_NOTE_CHOKE:
        {
            const auto key = reinterpret_cast<const clap_event_note_t*>(h)->key;
            if(key < 0) { e.allNotesOff(); s.sustained.fill(false); }
            else if(key < 128) { s.sustained[(size_t)key] = false; e.noteOff(key); }
            break;
        }
        case CLAP_EVENT_NOTE_EXPRESSION:
        {
            const auto& x = *reinterpret_cast<const clap_event_note_expression_t*>(h);
            if(x.expression_id != CLAP_NOTE_EXPRESSION_PRESSURE) break;
            if(x.key >= 0 && x.key < 128) e.aftertouch(x.key, float(x.value));
            else e.channelPressure(float(x.value));
            break;
        }
        case CLAP_EVENT_PARAM_VALUE:
        {
            const auto& v = *reinterpret_cast<const clap_event_param_value_t*>(h);
            if(v.param_id < s.info.size()) { s.values[v.param_id].store(v.value); e.setParam(int(v.param_id), float(v.value)); }
            else if(v.param_id == presetParam)
            {
                const int index = juce::jlimit(0, s.presetNames.size() - 1, int(std::lround(v.value)));
                s.preset.store(index); s.pendingPreset.store(index);
                if(s.host && s.host->request_callback) s.host->request_callback(s.host); // patches change on the main thread
            }
            break;
        }
        case CLAP_EVENT_MIDI:
        {
            const auto& m = *reinterpret_cast<const clap_event_midi_t*>(h);
            const int status = m.data[0] & 0xf0, a = m.data[1] & 0x7f, b = m.data[2] & 0x7f;
            if(status == 0x90 && b > 0) { s.sustained[(size_t)a] = false; e.noteOn(a, b / 127.f); }
            else if(status == 0x80 || status == 0x90) off(a);
            else if(status == 0xa0) e.aftertouch(a, b / 127.f);
            else if(status == 0xd0) e.channelPressure(a / 127.f);
            else if(status == 0xe0) e.pitchBend((float((b << 7) | a) - 8192.f) / 8192.f * bendSemitones);
            else if(status == 0xb0 && a == 1) e.modWheel(b / 127.f);
            else if(status == 0xb0 && a == 64)
            {
                s.sustain = b >= 64;
                if(!s.sustain) for(int k = 0; k < 128; ++k) if(s.sustained[(size_t)k]) { s.sustained[(size_t)k] = false; e.noteOff(k); }
            }
            else if(status == 0xb0 && (a == 120 || a == 123)) { e.allNotesOff(); s.sustained.fill(false); s.sustain = false; }
            break;
        }
        default: break;
        }
    }

    clap_process_status process(const clap_plugin_t* p, const clap_process_t* b)
    {
        auto& s = self(p);
        if(b->audio_outputs_count < 1 || b->audio_outputs[0].channel_count < 2) return CLAP_PROCESS_ERROR;
        float* const out[]{b->audio_outputs[0].data32[0], b->audio_outputs[0].data32[1]};
        if(b->transport && (b->transport->flags & CLAP_TRANSPORT_HAS_TEMPO)) s.engine->setTempo(b->transport->tempo);
        const uint32_t frames = b->frames_count, count = b->in_events ? b->in_events->size(b->in_events) : 0;
        uint32_t done = 0;
        auto render = [&](uint32_t until) {
            if(until <= done) return;
            float* const part[]{out[0] + done, out[1] + done};
            s.engine->process(part, 2, int(until - done));
            done = until;
        };
        for(uint32_t i = 0; i < count; ++i)
        {
            const auto* h = b->in_events->get(b->in_events, i);
            render(std::min(h->time, frames));
            apply(s, h);
        }
        render(frames);
        return CLAP_PROCESS_CONTINUE;
    }

    void setPatch(Modular& s, juce::ValueTree patch)
    {
        s.patch = std::move(patch);
        s.engine->setPatch(s.patch);
    }

    const clap_plugin_audio_ports_t audioPorts{
        [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 0 : 1; },
        [](const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) -> bool {
            if(input || index) return false;
            *info = {}; info->id = 0; info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->channel_count = 2;
            info->port_type = CLAP_PORT_STEREO; info->in_place_pair = CLAP_INVALID_ID;
            std::snprintf(info->name, sizeof(info->name), "Output");
            return true;
        }};

    const clap_plugin_note_ports_t notePorts{
        [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 1 : 0; },
        [](const clap_plugin_t*, uint32_t index, bool input, clap_note_port_info_t* info) -> bool {
            if(!input || index) return false;
            *info = {}; info->id = 0; info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
            info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
            std::snprintf(info->name, sizeof(info->name), "Notes");
            return true;
        }};

    const clap_plugin_params_t params{
        [](const clap_plugin_t* p) -> uint32_t { return uint32_t(self(p).info.size() + 1); },
        [](const clap_plugin_t* p, uint32_t i, clap_param_info_t* out) -> bool {
            auto& s = self(p);
            *out = {};
            if(i < s.info.size())
            {
                const auto& q = s.info[i];
                out->id = i; out->flags = CLAP_PARAM_IS_AUTOMATABLE;
                out->min_value = q.minValue; out->max_value = q.maxValue; out->default_value = q.defaultValue;
                std::snprintf(out->name, sizeof(out->name), "%s", q.name.toRawUTF8());
                return true;
            }
            if(i != s.info.size()) return false;
            out->id = presetParam; out->flags = CLAP_PARAM_IS_STEPPED;
            out->min_value = 0; out->max_value = std::max(0, s.presetNames.size() - 1); out->default_value = 0;
            std::snprintf(out->name, sizeof(out->name), "Preset");
            return true;
        },
        [](const clap_plugin_t* p, clap_id id, double* value) -> bool {
            auto& s = self(p);
            if(id < s.info.size()) { *value = s.values[id].load(); return true; }
            if(id == presetParam) { *value = s.preset.load(); return true; }
            return false;
        },
        [](const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t n) -> bool {
            auto& s = self(p);
            if(!std::isfinite(value)) return false;
            if(id < s.info.size()) { std::snprintf(text, n, "%.2f", value); return true; }
            if(id != presetParam) return false;
            std::snprintf(text, n, "%s", s.presetNames[juce::jlimit(0, s.presetNames.size() - 1, int(std::lround(value)))].toRawUTF8());
            return true;
        },
        [](const clap_plugin_t* p, clap_id id, const char* text, double* value) -> bool {
            auto& s = self(p);
            if(id == presetParam) { const int i = s.presetNames.indexOf(juce::String::fromUTF8(text)); if(i < 0) return false; *value = i; return true; }
            if(id >= s.info.size()) return false;
            char* end = nullptr; const double v = std::strtod(text, &end);
            if(end == text || *end || !std::isfinite(v) || v < s.info[id].minValue || v > s.info[id].maxValue) return false;
            *value = v; return true;
        },
        [](const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*) {
            for(uint32_t i = 0; in && i < in->size(in); ++i) apply(self(p), in->get(in, i));
        }};

    // State: a header line, the parameter values, the preset index, then the patch as XML.
    constexpr char stateHeader[] = "oscairn-modular-1";
    const clap_plugin_state_t state{
        [](const clap_plugin_t* p, const clap_ostream_t* stream) -> bool {
            auto& s = self(p);
            juce::String text = juce::String(stateHeader) + "\n";
            for(const auto& v : s.values) text << juce::String(v.load(), 17) << " ";
            text << s.preset.load() << "\n" << s.patch.toXmlString();
            const auto bytes = text.toStdString();
            for(size_t at = 0; at < bytes.size();)
            {
                const auto n = stream->write(stream, bytes.data() + at, bytes.size() - at);
                if(n <= 0) return false;
                at += size_t(n);
            }
            return true;
        },
        [](const clap_plugin_t* p, const clap_istream_t* stream) -> bool {
            auto& s = self(p);
            std::string bytes; char buffer[4096];
            for(;;)
            {
                const auto n = stream->read(stream, buffer, sizeof buffer);
                if(n < 0 || bytes.size() + size_t(n) > (8u << 20)) return false;
                if(n == 0) break;
                bytes.append(buffer, size_t(n));
            }
            const auto lines = juce::StringArray::fromLines(juce::String::fromUTF8(bytes.data(), int(bytes.size())));
            if(lines.size() < 3 || lines[0] != stateHeader) return false;
            auto numbers = juce::StringArray::fromTokens(lines[1], " ", "");
            numbers.removeEmptyStrings();
            if(numbers.size() != int(s.values.size()) + 1) return false;
            juce::StringArray xml(lines); xml.removeRange(0, 2);
            auto patch = juce::ValueTree::fromXml(xml.joinIntoString("\n"));
            if(!patch.hasType(mt::modular::PatchIDs::MODULAR_PATCH)) return false;
            for(size_t i = 0; i < s.values.size(); ++i)
            {
                const double v = numbers[int(i)].getDoubleValue();
                if(!std::isfinite(v) || v < s.info[i].minValue || v > s.info[i].maxValue) return false;
                s.values[i].store(v); s.engine->setParam(int(i), float(v));
            }
            s.preset.store(juce::jlimit(0, std::max(0, s.presetNames.size() - 1), numbers[int(s.values.size())].getIntValue()));
            setPatch(s, patch);
            return true;
        }};

    const clap_plugin_t* create(const clap_host_t* host)
    {
        auto* s = new Modular;
        s->host = host;
        s->engine = std::make_unique<mt::ModularEngine>();
        s->info = s->engine->getParams();
        s->values = std::vector<std::atomic<double>>(s->info.size());
        for(size_t i = 0; i < s->info.size(); ++i) s->values[i].store(s->info[i].defaultValue);
        s->presetNames = s->engine->getPresetNames();
        setPatch(*s, s->engine->createPreset(0));
        s->plugin.desc = &descriptor;
        s->plugin.plugin_data = s;
        s->plugin.init = [](const clap_plugin_t*) { return true; };
        s->plugin.destroy = [](const clap_plugin_t* p) { delete &self(p); };
        s->plugin.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t maxFrames) {
            if(!std::isfinite(rate) || rate < 8000 || rate > 768000) return false;
            self(p).engine->prepare(rate, int(maxFrames));
            return true;
        };
        s->plugin.deactivate = [](const clap_plugin_t*) {};
        s->plugin.start_processing = [](const clap_plugin_t*) { return true; };
        s->plugin.stop_processing = [](const clap_plugin_t*) {};
        s->plugin.reset = [](const clap_plugin_t* p) { auto& s = self(p); s.engine->reset(); s.sustained.fill(false); s.sustain = false; };
        s->plugin.process = process;
        s->plugin.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
            if(!std::strcmp(id, CLAP_EXT_AUDIO_PORTS)) return &audioPorts;
            if(!std::strcmp(id, CLAP_EXT_NOTE_PORTS)) return &notePorts;
            if(!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
            if(!std::strcmp(id, CLAP_EXT_STATE)) return &state;
            return nullptr;
        };
        s->plugin.on_main_thread = [](const clap_plugin_t* p) {
            auto& s = self(p);
            const int index = s.pendingPreset.exchange(-1);
            if(index >= 0) setPatch(s, s.engine->createPreset(index));
        };
        return &s->plugin;
    }

    const clap_plugin_factory_t factory{
        [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
        [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* { return index == 0 ? &descriptor : nullptr; },
        [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
            if(std::strcmp(id, descriptor.id)) return nullptr;
            try { return create(host); } catch(...) { return nullptr; }
        }};
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char*) { return true; },
    []() {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; },
};

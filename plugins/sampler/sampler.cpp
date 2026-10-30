// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Sampler: plays a one-region SFZ patch over a preloaded mono or stereo WAV,
// using the shared native sound contexts. The supported opcodes are the
// `oscairn-sfz-baseline-0` profile in docs/rfcs/0008-sfz-native-samples-and-conversion.md;
// anything else rejects the patch with its line number. Parsing, file access, decoding
// and freeing happen on the main thread; contexts that use identical audio share one
// immutable decoded copy. Playback reads it with linear interpolation. Samples are
// dependencies of a saved Performance, not part of it: snapshots record where each one
// is and its content hash, and loading fails if it is missing or has changed.

#include "../common/SoundContexts.h"
#include <clap/ext/log.h>
#include <sndfile.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace fs = std::filesystem;

    const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };
    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT, "org.oscairn.sampler", "Oscairn Sampler", "Oscairn",
        "https://github.com/jbschooley/oscairn", "", "", "0.1.0",
        "Plays SFZ sample patches with shared, preloaded audio", features,
    };

    // shortcut: fixed preload budgets; replace with a measured memory policy when streaming lands.
    constexpr uint64_t maxAssetBytes = 64ull << 20, maxTotalBytes = 256ull << 20;
    constexpr int maxRate = 192000, minRate = 8000;

    struct Sample
    {
        int rate = 0, channels = 0;
        size_t frames = 0;
        std::vector<float> data; // interleaved
        uint64_t hash = 0;
        size_t bytes() const { return data.size() * sizeof(float); }
    };

    uint64_t contentHash(const Sample& s)
    {
        // FNV-1a over format and PCM: identity follows content, not the file name.
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) { auto* b = static_cast<const unsigned char*>(p); for(size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; } };
        mix(&s.rate, sizeof(s.rate)); mix(&s.channels, sizeof(s.channels)); mix(s.data.data(), s.bytes());
        return h;
    }

    struct Region
    {
        std::string sample;
        int lokey = 0, hikey = 127, lovel = 1, hivel = 127, keycenter = 60, transpose = 0;
        double keytrack = 100, tune = 0, volume = 0, release = .001;
    };

    struct Patch
    {
        std::shared_ptr<const Sample> sample;
        Region region;
        fs::path directory; // canonical directory the sample path is relative to
    };

    struct Voice : oscairn::contexts::VoiceBase
    {
        const Sample* sample = nullptr;
        double position = 0, step = 0, gain = 0, level = 1, releaseFactor = 1, releaseDelta = 0;
        int64_t releaseLeft = 0; // samples until the release ends; 0 while held
        bool releasing = false;
    };

    struct Error { std::string message; };
    [[noreturn]] void fail(int line, const std::string& what) { throw Error{(line ? "line " + std::to_string(line) + ": " : std::string()) + what}; }

    // SFZ note numbers or names (c4 = 60).
    int parseKey(const std::string& v, int line)
    {
        char* end = nullptr; const long n = std::strtol(v.c_str(), &end, 10);
        if(end != v.c_str() && !*end) { if(n < 0 || n > 127) fail(line, "key out of range: " + v); return int(n); }
        static const int steps[] = {9, 11, 0, 2, 4, 5, 7}; // a..g
        if(v.size() < 2) fail(line, "invalid key: " + v);
        const char letter = char(std::tolower(static_cast<unsigned char>(v[0])));
        if(letter < 'a' || letter > 'g') fail(line, "invalid key: " + v);
        size_t at = 1; int note = steps[letter - 'a'];
        if(v[at] == '#') { ++note; ++at; } else if(v[at] == 'b' && at + 1 < v.size()) { --note; ++at; }
        const long octave = std::strtol(v.c_str() + at, &end, 10);
        if(end == v.c_str() + at || *end) fail(line, "invalid key: " + v);
        const long key = (octave + 1) * 12 + note;
        if(key < 0 || key > 127) fail(line, "key out of range: " + v);
        return int(key);
    }
    double parseNumber(const std::string& v, int line, double low, double high)
    {
        char* end = nullptr; const double n = std::strtod(v.c_str(), &end);
        if(end == v.c_str() || *end || !std::isfinite(n) || n < low || n > high) fail(line, "value out of range: " + v);
        return n;
    }
    int parseInteger(const std::string& v, int line, int low, int high)
    {
        const double n = parseNumber(v, line, low, high);
        if(n != std::floor(n)) fail(line, "integer expected: " + v);
        return int(n);
    }

    Region parse(const std::string& text)
    {
        Region r; bool region = false, sample = false;
        int line = 1; size_t at = 0;
        auto space = [&](char c) { return c == ' ' || c == '\t' || c == '\r'; };
        while(at < text.size())
        {
            const char c = text[at];
            if(c == '\n') { ++line; ++at; continue; }
            if(space(c)) { ++at; continue; }
            if(text.compare(at, 2, "//") == 0) { while(at < text.size() && text[at] != '\n') ++at; continue; }
            if(text.compare(at, 2, "/*") == 0) fail(line, "block comments are not supported");
            if(c == '#') fail(line, "preprocessor directives are not supported");
            if(c == '<')
            {
                const auto close = text.find('>', at);
                if(close == std::string::npos) fail(line, "unterminated header");
                const auto header = text.substr(at + 1, close - at - 1);
                if(header != "region") fail(line, "unsupported header <" + header + ">");
                if(region) fail(line, "only one <region> is supported");
                region = true; at = close + 1; continue;
            }
            const auto equals = text.find('=', at);
            const auto lineEnd = std::min(text.find('\n', at), text.size());
            if(equals == std::string::npos || equals > lineEnd) fail(line, "expected opcode=value");
            const auto opcode = text.substr(at, equals - at);
            if(!region) fail(line, "opcode " + opcode + " outside <region>");
            at = equals + 1;
            std::string value;
            if(opcode == "sample")
            {
                // Sample paths may contain spaces: the value ends at the line, a comment, or the next opcode.
                size_t end = at;
                while(end < lineEnd && text.compare(end, 2, "//") != 0)
                {
                    if(space(text[end]))
                    {
                        size_t next = end; while(next < lineEnd && space(text[next])) ++next;
                        size_t word = next; while(word < lineEnd && (std::isalnum(static_cast<unsigned char>(text[word])) || text[word] == '_')) ++word;
                        if(word > next && word < lineEnd && text[word] == '=') break;
                        if(next < lineEnd && text[next] == '<') break;
                    }
                    ++end;
                }
                value = text.substr(at, end - at);
                while(!value.empty() && space(value.back())) value.pop_back();
                at = end;
            }
            else
            {
                size_t end = at; while(end < text.size() && !space(text[end]) && text[end] != '\n' && text[end] != '<') ++end;
                value = text.substr(at, end - at); at = end;
            }
            if(value.empty()) fail(line, opcode + " has no value");
            if(opcode == "sample") { r.sample = value; sample = true; }
            else if(opcode == "lokey") r.lokey = parseKey(value, line);
            else if(opcode == "hikey") r.hikey = parseKey(value, line);
            else if(opcode == "key") r.lokey = r.hikey = r.keycenter = parseKey(value, line);
            else if(opcode == "lovel") r.lovel = parseInteger(value, line, 1, 127);
            else if(opcode == "hivel") r.hivel = parseInteger(value, line, 1, 127);
            else if(opcode == "pitch_keycenter") r.keycenter = parseKey(value, line);
            else if(opcode == "pitch_keytrack") r.keytrack = parseNumber(value, line, -1200, 1200);
            else if(opcode == "transpose") r.transpose = parseInteger(value, line, -127, 127);
            else if(opcode == "tune") r.tune = parseNumber(value, line, -100, 100);
            else if(opcode == "volume") r.volume = parseNumber(value, line, -144, 6);
            else if(opcode == "ampeg_release") r.release = parseNumber(value, line, 0, 100);
            else fail(line, "unsupported opcode " + opcode);
        }
        if(!region) fail(0, "patch has no <region>");
        if(!sample) fail(0, "region has no sample");
        if(r.lokey > r.hikey) fail(0, "lokey is above hikey");
        if(r.lovel > r.hivel) fail(0, "lovel is above hivel");
        return r;
    }

    // Resolve a relative sample path inside root, rejecting escapes through ".." or links.
    fs::path resolve(const fs::path& root, std::string name)
    {
        std::replace(name.begin(), name.end(), '\\', '/');
        const fs::path relative(name);
        if(relative.is_absolute() || relative.has_root_name()) fail(0, "sample path must be relative: " + name);
        std::error_code ec;
        const auto path = fs::canonical(root / relative, ec);
        if(ec) fail(0, "sample not found: " + name);
        const auto [rootEnd, pathAt] = std::mismatch(root.begin(), root.end(), path.begin(), path.end());
        if(rootEnd != root.end()) fail(0, "sample is outside the patch directory: " + name);
        if(!fs::is_regular_file(path, ec)) fail(0, "sample is not a regular file: " + name);
        return path;
    }

    std::shared_ptr<Sample> decode(const fs::path& path, uint64_t budget)
    {
        SF_INFO info{};
        SNDFILE* file = sf_open(path.c_str(), SFM_READ, &info);
        if(!file) fail(0, "cannot read WAV " + path.filename().string() + ": " + sf_strerror(nullptr));
        struct Close { SNDFILE* f; ~Close() { sf_close(f); } } close{file};
        const int major = info.format & SF_FORMAT_TYPEMASK, sub = info.format & SF_FORMAT_SUBMASK;
        if(major != SF_FORMAT_WAV && major != SF_FORMAT_WAVEX) fail(0, "only WAV samples are supported");
        if(sub != SF_FORMAT_PCM_16 && sub != SF_FORMAT_PCM_24 && sub != SF_FORMAT_PCM_32 && sub != SF_FORMAT_FLOAT)
            fail(0, "unsupported WAV encoding (use 16/24/32-bit PCM or 32-bit float)");
        if(info.channels < 1 || info.channels > 2) fail(0, "samples must be mono or stereo");
        if(info.samplerate < minRate || info.samplerate > maxRate) fail(0, "unsupported sample rate");
        if(info.frames < 1) fail(0, "sample is empty");
        // Check decoded size before allocating.
        const uint64_t bytes = uint64_t(info.frames) * uint64_t(info.channels) * sizeof(float);
        if(bytes > maxAssetBytes) fail(0, "sample exceeds the 64 MiB decoded limit");
        if(bytes > budget) fail(0, "sampler memory budget (256 MiB) exhausted");
        auto s = std::make_shared<Sample>();
        s->rate = info.samplerate; s->channels = info.channels; s->frames = size_t(info.frames);
        s->data.resize(s->frames * size_t(s->channels));
        if(sf_readf_float(file, s->data.data(), info.frames) != info.frames) fail(0, "sample is truncated");
        if(!std::all_of(s->data.begin(), s->data.end(), [](float v) { return std::isfinite(v); })) fail(0, "sample contains non-finite values");
        s->hash = contentHash(*s);
        return s;
    }

    struct Sampler : oscairn::contexts::Engine<Sampler, Voice, Patch>
    {
        double sampleRate = 48000;
        const clap_host_t* host = nullptr;
        const clap_host_log_t* log = nullptr;
        static constexpr const char* stateVersion = "oscairn-sampler-1";
        static constexpr size_t maxStateBytes = 1 << 20;

        bool accepts(const Patch& p, const clap_event_note_t& note)
        {
            const int velocity = int(std::lround(std::clamp(note.velocity, 0.0, 1.0) * 127));
            const auto& r = p.region;
            return p.sample && note.key >= r.lokey && note.key <= r.hikey && velocity >= r.lovel && velocity <= r.hivel;
        }
        void start(Voice& v, Context& c)
        {
            const auto& r = c.patch.region;
            v.sample = c.patch.sample.get();
            const double cents = (v.key - r.keycenter) * r.keytrack + r.transpose * 100.0 + r.tune;
            v.step = v.sample->rate / sampleRate * std::pow(2.0, cents / 1200.0);
            // SFZ default amp_veltrack=100: gain follows velocity squared.
            v.gain = std::pow(10.0, r.volume / 20.0) * v.velocity * v.velocity;
        }
        // Exponential release reaching -60 dB at ampeg_release, then silence: liquidsfz's curve.
        void release(Voice& v)
        {
            if(v.releasing) return;
            const double ratio = .001;
            v.releaseLeft = std::max<int64_t>(std::llround(contexts[v.context].patch.region.release * sampleRate), 1);
            v.releaseFactor = std::exp(-std::log((ratio + 1) / ratio) / double(v.releaseLeft));
            v.releaseDelta = -ratio * v.level * (1 - v.releaseFactor);
            v.releasing = true;
        }
        void selectedGlobally(Context&, const clap_output_events_t*) {}
        void applyParam(const clap_event_header_t*) {}
        void render(const clap_process_t* process, uint32_t from, uint32_t to)
        {
            auto& out = process->audio_outputs[0];
            for(uint32_t i = from; i < to; ++i)
            {
                eventTime = i;
                double left = 0, right = 0;
                for(auto& v : voices)
                {
                    if(!v.active) continue;
                    const auto& s = *v.sample;
                    const auto index = size_t(v.position);
                    if(index >= s.frames) { finishVoice(v); continue; } // no_loop: the sample end ends the voice
                    if(v.releasing) { if(v.releaseLeft-- <= 0) { finishVoice(v); continue; } v.level = v.level * v.releaseFactor + v.releaseDelta; }
                    const double fraction = v.position - double(index), gain = v.gain * v.level;
                    auto at = [&](size_t frame, int channel) { return frame < s.frames ? double(s.data[frame * s.channels + channel]) : 0.0; };
                    const double l = at(index, 0) + (at(index + 1, 0) - at(index, 0)) * fraction;
                    const double r = s.channels == 2 ? at(index, 1) + (at(index + 1, 1) - at(index, 1)) * fraction : l;
                    left += l * gain; right += r * gain;
                    v.position += v.step;
                }
                if(out.data32 && out.data32[0] && out.data32[1]) { out.data32[0][i] = float(left); out.data32[1][i] = float(right); }
            }
        }

        // Main thread. Shares audio already held by another context when the content matches.
        Patch load(const std::string& text, const fs::path& directory)
        {
            Patch patch; patch.region = parse(text);
            std::error_code ec;
            const auto root = fs::canonical(directory, ec);
            if(ec || !fs::is_directory(root)) fail(0, "sample not found: " + patch.region.sample + " (its folder is missing)");
            patch.directory = root;
            uint64_t used = 0; std::vector<const Sample*> counted;
            for(const auto& c : contexts) if(c.prepared && c.patch.sample && std::find(counted.begin(), counted.end(), c.patch.sample.get()) == counted.end())
            { counted.push_back(c.patch.sample.get()); used += c.patch.sample->bytes(); }
            auto decoded = decode(resolve(root, patch.region.sample), maxTotalBytes - used);
            for(const auto& c : contexts)
                if(c.prepared && c.patch.sample && c.patch.sample->hash == decoded->hash && c.patch.sample->rate == decoded->rate &&
                   c.patch.sample->channels == decoded->channels && c.patch.sample->data == decoded->data)
                { patch.sample = c.patch.sample; return patch; }
            patch.sample = std::move(decoded);
            return patch;
        }
        static uint32_t prepare(const clap_plugin_t* p, const char* name, const oscairn_sound_parameter_t*, uint32_t count,
                                const char* text, const char* directory, char* message, uint32_t capacity)
        {
            auto& s = self(p);
            try
            {
                if(!name || std::strlen(name) > 63) fail(0, "invalid sound name");
                if(count) fail(0, "the sampler has no parameters; describe the sound in SFZ");
                if(!text || !directory) fail(0, "an SFZ patch and its directory are required");
                auto* c = s.freeSlot();
                if(!c) fail(0, "no free sound slot");
                c->patch = s.load(text, directory);
                return s.publish(*c, name);
            }
            catch(const Error& e) { if(message && capacity) std::snprintf(message, capacity, "%s", e.message.c_str()); }
            catch(const std::exception& e) { if(message && capacity) std::snprintf(message, capacity, "%s", e.what()); }
            return CLAP_INVALID_ID;
        }
        void discarded(Context& c) { c.patch = Patch{}; } // frees unshared audio on the main thread

        // Snapshots store, per sound, its sample directory, a normalized one-region SFZ and the
        // sample's content hash. The audio itself stays where it is, like the plugin binary.
        static std::string normalized(const Patch& p)
        {
            if(!p.sample) return {};
            const auto& r = p.region; char text[256];
            std::snprintf(text, sizeof(text), "<region> lokey=%d hikey=%d lovel=%d hivel=%d pitch_keycenter=%d pitch_keytrack=%.17g transpose=%d tune=%.17g volume=%.17g ampeg_release=%.17g sample=",
                r.lokey, r.hikey, r.lovel, r.hivel, r.keycenter, r.keytrack, r.transpose, r.tune, r.volume, r.release);
            return text + r.sample + "\n";
        }
        bool writePatch(std::ostream& out, const Patch& p)
        {
            out << ' ' << std::quoted(p.directory.string()) << ' ' << std::quoted(normalized(p)) << ' ' << (p.sample ? p.sample->hash : 0);
            return true;
        }
        bool readPatch(std::istream& in, Patch& p)
        {
            std::string directory, text; uint64_t hash;
            if(!(in >> std::quoted(directory) >> std::quoted(text) >> hash)) return false;
            if(text.empty()) { p = Patch{}; return true; }
            // The host shows the logged reason when it reports the rejected snapshot.
            auto reject = [&](const std::string& why) { if(log) log->log(host, CLAP_LOG_ERROR, why.c_str()); return false; };
            try { p = load(text, directory); }
            catch(const Error& e) { return reject(e.message + " (in " + directory + ")"); }
            catch(const std::exception& e) { return reject(e.what()); }
            if(p.sample->hash != hash) return reject("sample changed since the sound was saved: " + p.region.sample + " (in " + directory + ")");
            return true;
        }
        void restored() {}
    };

    Sampler& self(const clap_plugin_t* p) { return *static_cast<Sampler*>(p->plugin_data); }

    const clap_plugin_state_t snapshot{Sampler::saveSnapshot, Sampler::loadSnapshot};
    const oscairn_plugin_sound_contexts_t soundContexts{Sampler::capacity, Sampler::get, Sampler::prepare, Sampler::select, Sampler::discard, &snapshot};

    const clap_plugin_audio_ports_t audioPorts{
        [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 0 : 1; },
        [](const clap_plugin_t*, uint32_t index, bool input, clap_audio_port_info_t* info) -> bool {
            if(input || index) return false;
            info->id = 0; info->flags = CLAP_AUDIO_PORT_IS_MAIN; info->channel_count = 2; info->port_type = CLAP_PORT_STEREO;
            info->in_place_pair = CLAP_INVALID_ID; std::snprintf(info->name, sizeof(info->name), "Main"); return true;
        },
    };
    const clap_plugin_note_ports_t notePorts{
        [](const clap_plugin_t*, bool input) -> uint32_t { return input ? 1 : 0; },
        [](const clap_plugin_t*, uint32_t index, bool input, clap_note_port_info_t* info) -> bool {
            if(!input || index) return false;
            info->id = 0; info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
            info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP; std::snprintf(info->name, sizeof(info->name), "Notes"); return true;
        },
    };

    const clap_plugin_t* create(const clap_host_t* host)
    {
        auto* s = new Sampler;
        s->host = host;
        s->plugin.desc = &descriptor;
        s->plugin.plugin_data = s;
        s->plugin.init = [](const clap_plugin_t* p) { auto& s = self(p); s.log = static_cast<const clap_host_log_t*>(s.host->get_extension(s.host, CLAP_EXT_LOG)); return true; };
        s->plugin.destroy = [](const clap_plugin_t* p) { delete &self(p); };
        s->plugin.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t) { auto& s = self(p); s.sampleRate = rate; s.resetVoices(); return true; };
        s->plugin.deactivate = [](const clap_plugin_t*) {};
        s->plugin.start_processing = [](const clap_plugin_t*) { return true; };
        s->plugin.stop_processing = [](const clap_plugin_t*) {};
        s->plugin.reset = [](const clap_plugin_t* p) { self(p).resetVoices(); };
        s->plugin.process = Sampler::process;
        s->plugin.get_extension = [](const clap_plugin_t*, const char* id) -> const void* {
            if(std::strcmp(id, OSCAIRN_EXT_SOUND_CONTEXTS) == 0) return &soundContexts;
            if(std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &audioPorts;
            if(std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &notePorts;
            return nullptr;
        };
        s->plugin.on_main_thread = [](const clap_plugin_t*) {};
        return &s->plugin;
    }

    const clap_plugin_factory_t factory = {
        [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
        [](const clap_plugin_factory_t*, uint32_t index) -> const clap_plugin_descriptor_t* { return index == 0 ? &descriptor : nullptr; },
        [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) -> const clap_plugin_t* {
            return std::strcmp(id, descriptor.id) == 0 && host ? create(host) : nullptr;
        },
    };
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    [](const char*) { return true; },
    []() {},
    [](const char* id) -> const void* { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; },
};

// SPDX-License-Identifier: AGPL-3.0-or-later
// Reference renders: the same one-region patches through liquidsfz and the Oscairn
// Sampler (loaded as a CLAP plugin). Prints per-case differences; not linked into Oscairn.
#include <clap/clap.h>
#include <oscairn-clap/sound-contexts.h>
#include <liquidsfz.hh>
#include <sndfile.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
static void check(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
constexpr int rate = 48000, block = 128, frames = 48000;

struct Case { const char* name; std::string region; int key; int velocity; int releaseAt; };

std::vector<float> liquid(const fs::path& sfz, const Case& c)
{
    LiquidSFZ::Synth synth; synth.set_sample_rate(rate); synth.set_preload_time(3000); synth.set_max_voices(16);
    check(synth.load(sfz.string()), "liquidsfz load");
    std::vector<float> out; std::vector<float> l(block), r(block); float* o[]{l.data(), r.data()};
    for(int at = 0; at < frames; at += block)
    {
        if(at == 0) synth.add_event_note_on(0, 0, c.key, c.velocity);
        if(at == c.releaseAt) synth.add_event_note_off(0, 0, c.key);
        synth.process(o, block); out.insert(out.end(), l.begin(), l.end());
    }
    return out;
}

struct Events { std::vector<clap_event_note_t> notes; };
std::vector<float> oscairn(const char* module, const fs::path& dir, const std::string& patch, const Case& c)
{
    void* library = dlopen(module, RTLD_NOW | RTLD_LOCAL); check(library, "open sampler");
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry")); check(entry && entry->init(module), "clap entry");
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{CLAP_VERSION_INIT, nullptr, "compare", "", "", "1", [](const clap_host_t*, const char*) -> const void* { return nullptr; },
        [](const clap_host_t*) {}, [](const clap_host_t*) {}, [](const clap_host_t*) {}};
    const auto* plugin = factory->create_plugin(factory, &host, "org.oscairn.sampler"); check(plugin && plugin->init(plugin), "create sampler");
    check(plugin->activate(plugin, rate, 1, block) && plugin->start_processing(plugin), "activate");
    auto* contexts = static_cast<const oscairn_plugin_sound_contexts_t*>(plugin->get_extension(plugin, OSCAIRN_EXT_SOUND_CONTEXTS));
    char message[256]{};
    const auto id = contexts->prepare(plugin, "compare", nullptr, 0, patch.c_str(), dir.c_str(), message, sizeof(message));
    if(id == CLAP_INVALID_ID) throw std::runtime_error(message);
    check(contexts->select(plugin, id, -1), "select");
    std::vector<float> out, l(block), r(block); float* channels[]{l.data(), r.data()};
    clap_audio_buffer_t buffer{}; buffer.data32 = channels; buffer.channel_count = 2;
    for(int at = 0; at < frames; at += block)
    {
        Events e;
        auto note = [&](bool on) { clap_event_note_t n{}; n.header = {sizeof(n), 0, CLAP_CORE_EVENT_SPACE_ID, uint16_t(on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            n.note_id = -1; n.port_index = 0; n.channel = 0; n.key = int16_t(c.key); n.velocity = c.velocity / 127.0; e.notes.push_back(n); };
        if(at == 0) note(true);
        if(at == c.releaseAt) note(false);
        clap_input_events_t in{&e, [](const clap_input_events_t* l) { return uint32_t(static_cast<Events*>(l->ctx)->notes.size()); },
            [](const clap_input_events_t* l, uint32_t i) -> const clap_event_header_t* { return &static_cast<Events*>(l->ctx)->notes[i].header; }};
        clap_output_events_t outEvents{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
        clap_process_t process{}; process.frames_count = block; process.steady_time = -1; process.audio_outputs = &buffer; process.audio_outputs_count = 1;
        process.in_events = &in; process.out_events = &outEvents;
        check(plugin->process(plugin, &process) == CLAP_PROCESS_CONTINUE, "process");
        out.insert(out.end(), l.begin(), l.end());
    }
    plugin->stop_processing(plugin); plugin->deactivate(plugin); plugin->destroy(plugin); entry->deinit(); dlclose(library);
    return out;
}

int main(int argc, char** argv)
{
    try
    {
        check(argc == 3, "usage: compare SAMPLER.clap NEW_DIRECTORY");
        const fs::path dir = argv[2]; check(!fs::exists(dir) && fs::create_directory(dir), "create new output directory");
        // One second of a decaying 220 Hz tone with a harmonic, mono 48 kHz float.
        std::vector<float> wave(rate);
        for(int i = 0; i < rate; ++i) wave[i] = float(.4 * std::exp(-i / 20000.0) * (std::sin(i * 2 * M_PI * 220 / rate) + .3 * std::sin(i * 2 * M_PI * 660 / rate)));
        SF_INFO info{}; info.channels = 1; info.samplerate = rate; info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
        SNDFILE* file = sf_open((dir / "tone.wav").c_str(), SFM_WRITE, &info); check(file, "write WAV");
        check(sf_writef_float(file, wave.data(), rate) == rate && !sf_close(file), "write WAV");
        const Case cases[]{
            {"unity, full velocity", "", 60, 127, frames},
            {"velocity 64", "", 60, 64, frames},
            {"up 7 semitones", "", 67, 127, frames},
            {"down 5 semitones", "", 55, 127, frames},
            {"volume -6 tune +30", " volume=-6 tune=30", 60, 127, frames},
            {"release 0.1 s at 0.25 s", " ampeg_release=0.1", 60, 127, 12032},
        };
        std::printf("%-26s %10s %10s %14s %16s\n", "case", "gain", "max diff", "rel. RMS diff", "shape residual");
        for(const auto& c : cases)
        {
            const std::string patch = "<region> sample=tone.wav pitch_keycenter=60" + c.region + "\n";
            const auto sfz = dir / (std::string("case") + std::to_string(&c - cases) + ".sfz");
            { std::ofstream(sfz) << patch; }
            const auto a = liquid(sfz, c), b = oscairn(argv[1], dir, patch, c);
            double ea = 0, eb = 0, cross = 0, diff = 0, worst = 0;
            for(size_t i = 0; i < a.size(); ++i) { ea += a[i] * a[i]; eb += b[i] * b[i]; cross += a[i] * b[i]; diff += (a[i] - b[i]) * (a[i] - b[i]); worst = std::max(worst, double(std::abs(a[i] - b[i]))); }
            // Residual after the best single gain: what differs besides level.
            const double residual = std::sqrt(std::max(0.0, 1 - cross * cross / (ea * eb)));
            std::printf("%-26s %10.4f %10.5f %14.5f %16.6f\n", c.name, std::sqrt(eb / ea), worst, std::sqrt(diff / ea), residual);
        }
        std::printf("gain = Oscairn RMS / liquidsfz RMS; relative RMS diff = RMS(Oscairn - liquidsfz) / RMS(liquidsfz);\n"
                    "shape residual = relative RMS difference left after scaling Oscairn by the best single gain\n");
        return 0;
    }
    catch(const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}

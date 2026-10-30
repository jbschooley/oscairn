// SPDX-License-Identifier: AGPL-3.0-or-later
// Headless prior-art probe; not linked into Oscairn.
#include "../../core/clap-host/tests/AllocationGuard.h"
#include <liquidsfz.hh>
#include <sndfile.h>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>
static void check(bool ok, const char *message)
{
    if (!ok)
        throw std::runtime_error(message);
}
int main(int argc, char **argv)
{
    try
    {
        check(argc == 2, "usage: probe NEW_DIRECTORY");
        const std::filesystem::path dir = argv[1];
        check(!std::filesystem::exists(dir), "directory must not exist");
        check(std::filesystem::create_directory(dir), "create new output directory");
        std::vector<float> wave(96000);
        for (size_t i = 0; i < wave.size(); ++i)
            wave[i] = float(.3 * std::sin(i * 6.283185307179586 * 220 / 48000));
        SF_INFO info{};
        info.channels = 1;
        info.samplerate = 48000;
        info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
        auto *file = sf_open((dir / "tone.wav").c_str(), SFM_WRITE, &info);
        check(file, "create WAV");
        const auto written = sf_writef_float(file, wave.data(), wave.size());
        const auto closed = sf_close(file);
        check(written == static_cast<sf_count_t>(wave.size()) && !closed, "write WAV");
        {
            std::ofstream out(dir / "a.sfz");
            out << "<region> sample=tone.wav key=60 ampeg_release=0.1\n";
            out.close();
            check(bool(out), "write A");
        }
        {
            std::ofstream out(dir / "b.sfz");
            out << "<region> sample=tone.wav key=60 transpose=7 ampeg_release=0.1\n";
            out.close();
            check(bool(out), "write B");
        }
        LiquidSFZ::Synth a, ref;
        a.set_sample_rate(48000);
        ref.set_sample_rate(48000);
        a.set_preload_time(3000);
        ref.set_preload_time(3000);
        a.set_max_voices(16);
        ref.set_max_voices(16);
        check(a.load((dir / "a.sfz").string()), "load A");
        const auto one = a.cache_size();
        check(a.cache_file_count() == 1, "one cached sample");
        check(ref.load((dir / "a.sfz").string()), "load reference");
        check(a.cache_size() == one && a.cache_file_count() == 1, "reference shares sample cache");
        std::array<float, 128> al{}, ar{}, rl{}, rr{}, bl{}, br{};
        float *ao[]{al.data(), ar.data()};
        float *ro[]{rl.data(), rr.data()};
        float *bo[]{bl.data(), br.data()};
        a.add_event_note_on(0, 0, 60, 100);
        ref.add_event_note_on(0, 0, 60, 100);
        double energy = 0, energyB = 0, difference = 0;
        auto renderA = [&]
        {
            forbidAllocation = true;
            a.process(ao, 128);
            ref.process(ro, 128);
            forbidAllocation = false;
            for (size_t i = 0; i < 128; ++i)
            {
                check(std::isfinite(al[i]) && std::isfinite(ar[i]), "finite audio");
                check(al[i] == rl[i] && ar[i] == rr[i], "old voice unchanged");
                energy += al[i] * al[i] + ar[i] * ar[i];
            }
        };
        renderA();
        {
            LiquidSFZ::Synth b;
            b.set_sample_rate(48000);
            b.set_preload_time(3000);
            b.set_max_voices(16);
            check(b.load((dir / "b.sfz").string()), "load B while A held");
            check(a.cache_size() == one && a.cache_file_count() == 1, "different patch shares sample cache");
            b.add_event_note_on(0, 0, 60, 100);
            for (int block = 0; block < 100; ++block)
            {
                renderA();
                forbidAllocation = true;
                b.process(bo, 128);
                forbidAllocation = false;
                for (size_t i = 0; i < 128; ++i)
                {
                    check(std::isfinite(bl[i]) && std::isfinite(br[i]), "finite B");
                    energyB += bl[i] * bl[i] + br[i] * br[i];
                    difference += std::abs(bl[i] - al[i]);
                }
            }
            b.all_sound_off();
        }
        renderA();
        check(energy > 1 && energyB > 1 && difference > 1, "audible independent sounds");
        check(a.cache_miss_count() == 0, "fully preloaded playback has no cache misses");
        std::cout << "PASS: one sample, three instances, two patch tunings; shared decoded cache=" << one
                  << " bytes; held voice unchanged; cache misses=" << a.cache_miss_count() << "\n";
        return 0;
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

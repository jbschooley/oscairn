// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0021 step 5: seamless switching. A note held while a part's Sound is replaced keeps the
// outgoing chain sounding (and its echo tail) while new notes play the incoming Sound, in
// uneven block sizes, with no allocation on the audio thread: the output is the old chain alone
// plus the new Sound alone. The old plugins are hidden at once and removed once idle; undoing
// the switch brings them back. A note held through importing a Performance goes on.
//   oscairn-seamless-test FIXTURE.clap DELAY.clap
#include "oscairn/session.h"
#include "AllocationGuard.h"
#include "../../plugin-host/src/StateFiles.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace oscairn;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    constexpr uint32_t maxFrames = 128;
    constexpr uint32_t sizes[] = {64, 17, 128, 1, 50, 99, 3};
    constexpr int blocks = 3000; // about four seconds: the old echo's tail, and gaps between its repeats

    // The old Sound: a synth into an echo.
    uint32_t oldPart(Session& s, const char* fixture, const char* delay)
    {
        const auto synth = s.add("Old Synth", fixture, "org.kick.test.synth"), echo = s.add("Old Echo", delay);
        s.setParameter(synth, 7, .25);
        const auto part = s.addPart("Keys");
        s.insertIntoPart(part, 0, synth); s.insertIntoPart(part, 1, echo); s.commit();
        return part;
    }

    // Renders a session through the same blocks and notes: `at(block)` acts before each block.
    template<class At> std::vector<float> render(Session& s, At&& at)
    {
        std::vector<float> out;
        std::array<float, maxFrames> l{}, r{};
        float* o[2]{l.data(), r.data()};
        for(int block = 0; block < blocks; ++block)
        {
            at(block);
            s.service();
            const auto frames = sizes[size_t(block) % std::size(sizes)];
            forbidAllocation = true;
            const bool ok = s.process(nullptr, 0, o, 2, frames);
            forbidAllocation = false;
            check(ok, "render");
            out.insert(out.end(), l.begin(), l.begin() + frames);
        }
        return out;
    }
    double energy(const std::vector<float>& v, size_t from, size_t to) { double e = 0; for(size_t i = from; i < std::min(to, v.size()); ++i) e += std::abs(v[i]); return e; }
    size_t sampleAt(int block) { size_t n = 0; for(int b = 0; b < block; ++b) n += sizes[size_t(b) % std::size(sizes)]; return n; }
}

int main(int argc, char** argv)
{
    if(argc < 3) { std::fprintf(stderr, "usage: %s FIXTURE DELAY\n", argv[0]); return 2; }
    const char* fixture = argv[1]; const char* delay = argv[2];
    try
    {
        state_files::TemporaryDirectory home;
        ::setenv("OSCAIRN_DATA_HOME", home.path.c_str(), 1);
        constexpr int hold = 2, swap = 10, play = 20, release = 40, stop = 60;

        // The new Sound: another synth, brighter, without the echo; and a Performance of it.
        std::string sound;
        const auto other = std::filesystem::path(home.path) / "Other.oscairn";
        {
            Session maker(48000, maxFrames);
            const auto synth = maker.add("New Synth", fixture, "org.kick.test.synth");
            maker.setParameter(synth, 7, .6);
            const auto part = maker.addPart("Lead"); maker.insertIntoPart(part, 0, synth); maker.commit();
            sound = maker.savePartAsSound(part, "Bright").id;
            maker.savePerformance(other.string());
        }

        // The old chain alone: the held note, released later.
        Session a(48000, maxFrames);
        oldPart(a, fixture, delay);
        const auto alone = render(a, [&](int b) { if(b == hold) a.note(true, 60, 1); if(b == release) a.note(false, 60, 0); });

        // The new Sound alone: the note played after the switch.
        Session n(48000, maxFrames);
        const auto np = n.addPart("Keys"); n.commit(); n.loadSound(np, sound);
        const auto fresh = render(n, [&](int b) { if(b == play) n.note(true, 64, 1); if(b == stop) n.note(false, 64, 0); });

        // Both: the Sound switched in while 60 is held.
        Session s(48000, maxFrames);
        const auto part = oldPart(s, fixture, delay);
        size_t hiddenAt = 0, retiringAfter = 0;
        const auto both = render(s, [&](int b) {
            if(b == hold) s.note(true, 60, 1);
            if(b == swap) { s.loadSound(part, sound); hiddenAt = s.graphState().names.size(); retiringAfter = s.retiringPlugins(); }
            if(b == play) s.note(true, 64, 1);
            if(b == release) s.note(false, 60, 0);
            if(b == stop) s.note(false, 64, 0);
        });

        check(energy(alone, sampleAt(swap), sampleAt(release)) > 0 && energy(alone, 48000, alone.size()) > 0, "the old chain sounds while held, and its echo a second later");
        check(energy(fresh, sampleAt(play), fresh.size()) > 0, "the new Sound sounds");
        double worst = 0;
        for(size_t i = 0; i < both.size(); ++i) worst = std::max(worst, double(std::abs(both[i] - (alone[i] + fresh[i]))));
        check(worst < 1e-5, "switching mid-note: the old chain's held note and tail plus the new Sound, sample for sample (worst " + std::to_string(worst) + ")");
        check(hiddenAt == 1 && retiringAfter == 2, "the old plugins are hidden at once and sounding out");
        // Silence until the old echo has died away: then they go.
        std::array<float, maxFrames> l{}, r{}; float* o[2]{l.data(), r.data()};
        for(int i = 0; i < 4000 && s.retiringPlugins(); ++i) { s.service(); s.process(nullptr, 0, o, 2, maxFrames); }
        check(s.retiringPlugins() == 0, "once idle, the old plugins are removed");

        // Importing a Performance while a note is held: the held note and its echo go on as if
        // nothing happened (the imported part had no note-on, so its release does nothing there).
        Session i(48000, maxFrames);
        oldPart(i, fixture, delay);
        const auto imported = render(i, [&](int b) {
            if(b == hold) i.note(true, 60, 1);
            if(b == swap) i.importPerformance(other.string());
            if(b == release) i.note(false, 60, 0);
        });
        double importWorst = 0;
        for(size_t k = 0; k < imported.size(); ++k) importWorst = std::max(importWorst, double(std::abs(imported[k] - alone[k])));
        check(i.parts().size() == 2 && importWorst < 1e-6, "a note held through an import goes on (worst " + std::to_string(importWorst) + ")");

        // Undo puts the old chain back, even while it is still sounding out.
        Session u(48000, maxFrames);
        const auto up = oldPart(u, fixture, delay);
        u.note(true, 60, 1); { std::array<float, maxFrames> x{}, y{}; float* xo[2]{x.data(), y.data()}; u.service(); u.process(nullptr, 0, xo, 2, maxFrames); }
        u.loadSound(up, sound);
        check(u.retiringPlugins() == 2, "sounding out after the switch");
        const auto midSwitch = std::filesystem::path(home.path) / "mid.oscairn";
        u.savePerformance(midSwitch.string());
        Session v(48000, maxFrames); v.loadPerformance(midSwitch.string());
        check(v.graphState().names.size() == 1 && v.graphState().names.begin()->second == "New Synth", "saved mid-switch, only the new chain is in the Performance");
        u.undo();
        const auto names = u.graphState().names;
        check(u.retiringPlugins() == 0 && u.parts()[0].chain.size() == 2 && std::any_of(names.begin(), names.end(), [](const auto& x) { return x.second == "Old Synth"; }),
              "undo brings the old chain back");
    }
    catch(const std::exception& e) { forbidAllocation = false; std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    if(!failures) std::puts("ok: seamless switching keeps held notes and tails, and retires the old chain");
    return failures ? 1 : 0;
}

# Oscairn

*Pronounced "OSS-cairn": **osc**illator + **cairn**, sound built up in layers.*

Oscairn is an open instrument platform: one realtime host for synthesis engines, samplers,
effects and sequencers that runs on hardware instruments, on the desktop and inside a DAW.
A sound or performance is not tied to one device.

> **Status:** early. What runs today: a CLAP sequencer plugin playing a CLAP synth plugin,
> sample-accurately, rendered offline to a WAV file. No live audio, MIDI input, UI or
> LV2/VST3 yet.

## Try it

Needs CMake 3.21+, Ninja and a C++20 compiler (tested on macOS with Apple clang).

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build                     # host, scanner, timing and render tests

build/hosts/oscairn-render demo.wav --seconds 16 --tempo 120 \
    build/plugins/oscairn-step-sequencer.clap build/plugins/oscairn-basic-synth.clap
```

`oscairn-render` chains the plugins in the order given: each one plays the notes of the
one before it. Any CLAP plugin can go in the chain (`path.clap#plugin-id` picks one plugin
from a module).

What is in place:

- **CLAP hosting** (`core/clap-host`): Randy Chabot's host core from
  [KICK PR #13](https://github.com/MusicTribeOpenSource/0700-AAM/pull/13), imported with its
  history. Parameters, state, latency, tail, note and audio ports, timers, thread checks,
  sample-accurate events, out-of-process plugin scanning with crash and timeout isolation.
  Changed here so note-only plugins (sequencers) load and their note output reaches the next
  plugin in the same block.
- **Engine** (`core/engine`): the transport (tempo, time signature, position) and a plugin
  chain, the first step towards the graph.
- **Plugins** (`plugins/`): Oscairn Step Sequencer (a 16-step pattern following the
  transport) and Oscairn Basic Synth.
- **Tests:** every sequencer note lands on its exact sample when processed in uneven blocks,
  and rendering with 512- and 37-sample blocks gives byte-identical files.

## Design in brief

- **Event sources.** Keyboards, pads, MIDI, sequencers and the DAW are all event sources that
  feed the same graph. Live playing, groovebox sequencing and DAW use are one model.
- **Plugins do the timing-critical work.** Engines, effects, sequencers and arpeggiators are
  plugins (CLAP preferred; LV2 and VST3 hosted too). They also run in a DAW.
- **One UI, separate from the audio.** The UI talks to the core through a control API and
  never runs on the audio thread.
- **The core is a library.** It does not assume it owns the audio device or the process, so the
  same core runs as device firmware, a desktop app, a headless test host and a DAW plugin.
- **Hierarchy.** Patch → Sound (note effects + engine + inserts) → Performance (Sounds,
  zones, routing, scenes) → Setlist; and Project → tracks, patterns, song for sequencing.
- **Files first.** Everything a user owns is a plain file in a documented format. A library
  view hides files for people who just want to play; a Files view shows them for those who
  want it.

## Layout

| Folder | Contents |
| --- | --- |
| `core/` | Graph, transport and clock, event routing, plugin hosting, library index. A library. |
| `hosts/` | Programs built on the core: headless (render to file, for CI), desktop, later device and plugin builds. |
| `plugins/` | First-party plugins: step sequencer, arpeggiator, test synth. |
| `docs/rfcs/` | Design decisions, one short document each. |

Related: [`oscairn-panel`](https://github.com/jbschooley/oscairn-panel), a virtual front panel
for developing without hardware.

## Licence

Oscairn is licensed under the GNU Affero General Public License v3.0 or later. See
[`LICENSE`](LICENSE) and [RFC 0001](docs/rfcs/0001-licence.md).

Plugins loaded through CLAP, LV2 or VST3 are separate works and may use any licence.

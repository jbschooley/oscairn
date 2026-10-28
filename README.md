# Oscairn

*Pronounced "OSS-cairn": **osc**illator + **cairn**, sound built up in layers.*

Oscairn is an open instrument platform: one realtime host for synthesis engines, samplers,
effects and sequencers that runs on hardware instruments, on the desktop and inside a DAW.
A sound or performance is not tied to one device.

> **Status:** early. What runs today: a graph of CLAP plugins (for example a sequencer
> playing a synth through a delay, with per-step parameter locks), sample-accurately,
> following its own tempo or an external MIDI clock, rendered offline to a WAV file, and a
> desktop demo app with live audio, MIDI input and switchable panel layouts. No LV2/VST3,
> patch library or device build yet.

## Try it

Needs CMake 3.21+, Ninja and a C++20 compiler. CI builds and tests on Ubuntu 24.04 (GCC)
and macOS 15 (Apple clang), plus a ThreadSanitizer run.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
ctest --test-dir build

P=build/plugins
build/hosts/oscairn-render demo.wav --seconds 16 --tempo 120 \
    $P/oscairn-step-sequencer.clap $P/oscairn-basic-synth.clap $P/oscairn-delay.clap

# With per-step parameter locks: the sequencer's lane 0 controls the synth's Brightness:
build/hosts/oscairn-render demo-locks.wav --seconds 16 --map 0.0=1.Brightness \
    $P/oscairn-step-sequencer.clap $P/oscairn-basic-synth.clap $P/oscairn-delay.clap

# The same chain following a (simulated) external MIDI clock at 140 BPM:
build/hosts/oscairn-render demo-140.wav --seconds 16 --midi-clock 140 \
    $P/oscairn-step-sequencer.clap $P/oscairn-basic-synth.clap $P/oscairn-delay.clap
```

The desktop app (downloads JUCE 9.0.4 the first time):

```bash
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOSCAIRN_STANDALONE=ON
ninja -C build-app oscairn_standalone
# macOS: build-app/hosts/standalone/oscairn_standalone_artefacts/RelWithDebInfo/oscairn-standalone.app
#   --layout desktop|kick|keys49|file.json   (or the View menu)
#   --snapshot out.png [--play SECONDS]      render the panel to a PNG and quit
```

`oscairn-render` chains the plugins in the order given: each one plays the notes of the
one before it and processes the audio of the last one that produced audio. Any CLAP plugin
can go in the chain (`path.clap#plugin-id` picks one plugin from a module).

What is in place:

- **CLAP hosting** (`core/clap-host`): Randy Chabot's host core from
  [KICK PR #13](https://github.com/MusicTribeOpenSource/0700-AAM/pull/13), imported with its
  history. Parameters, state, latency, tail, note and audio ports, timers, thread checks,
  sample-accurate events, out-of-process plugin scanning with crash and timeout isolation.
  Changed here so that note-only plugins (sequencers) load and their notes reach the next
  plugin in the same block, main-thread work and state saves no longer silence audio, a bad
  event no longer resets the plugin, and an instance uses 0.41 MB instead of 1.55 MB.
- **Engine** (`core/engine`): the graph (plugin nodes; audio connections summed, note
  connections merged; processed in dependency order each block; edited while audio runs,
  without locks) and the transport (tempo, time signature, position; its own tempo or an
  external MIDI clock). `oscairn-render` builds a straight chain on the graph.
- **oscairn.param-target** ([RFC 0002](docs/rfcs/0002-param-target.md)): a CLAP
  extension for parameter changes between plugins, such as a sequencer's per-step locks,
  routed by the graph to the target's parameter in the same block.
- **Plugins** (`plugins/`): Oscairn Step Sequencer (a 16-step pattern following the
  transport's beat position, with a lock lane; each step's on/off, note and lock are
  parameters), Oscairn Basic Synth (Brightness and Level
  parameters) and Oscairn Delay (tempo-synced ping-pong).
- **Tests:** every sequencer note lands on its exact sample when processed in uneven blocks;
  rendering with 512- and 37-sample blocks gives byte-identical files; following a 140 BPM
  MIDI clock, steps land within 2 samples of their pulses; graph fan-out, summing, note
  merging, cycle rejection and live editing; parameter locks applied from each step's first
  sample; step edits applied from their exact sample; host behaviour for live use (bad events, queued-event overflow, mono output).

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

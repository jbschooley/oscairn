# Roadmap

How Oscairn gets from nothing to the [architecture](architecture.md): the smallest steps
that each produce something that runs. Nobody has the target hardware yet, so everything
early runs on a desktop.

Status as of October 2026.

## Phase 0: a device to develop against, without hardware (done)

The KICK firmware, published by Music Tribe under the AGPL, runs on a desktop with a
simulated front panel, thanks to community work by chillbeast and Eight4aWish. It serves
as a reference device: how the panel, screen and audio fit together.

## Phase 1: prior art and decisions (done)

Reviewed existing hosts and instrument platforms (MOD, Zynthian, norns, the Deluge
community firmware, Carla, Sushi, Tracktion Engine, Ardour, Zrythm and others). None
covers the core, so Oscairn has its own:

- a new core in C++20, with no JUCE in the core;
- CLAP hosting seeded from Randy Chabot's host in KICK PR #13, imported with its history;
- AGPL-3.0-or-later, with plugin loading as the licence boundary
  ([RFC 0001](rfcs/0001-licence.md)).

The control API uses JSON-RPC over a local socket ([RFC 0003](rfcs/0003-control-api.md)).
The UI toolkit on devices is still to decide.

## Phase 2: headless core on the desktop (milestone met)

Milestone: a script builds a graph (CLAP sequencer → CLAP synth → CLAP effect), plays it
in sync with MIDI clock, and renders an identical file in CI on every run.

Done:

- realtime graph with lock-free live editing; CLAP hosting; sample-accurate event routing;
- transport with its own tempo or an external MIDI clock;
- the core as a library that does not own the audio device or the process;
- parameter events between plugins ([RFC 0002](rfcs/0002-param-target.md));
- offline renderer, and CI on Linux and macOS with a ThreadSanitizer run.

Not done: the hardware-services interface, file formats and the library index, the
complete control API, CPU and deadline measurements. Its in-process foundation is in
place (Phase 3 below).

## Phase 3: first sequencer and first workflow (in progress)

Goal: something new running "on the device" before anyone has the device.

Done:

- a 16-step sequencer plugin whose steps (on/off, note, lock) are parameters, with one
  parameter-lock lane;
- a desktop app (JUCE) with live audio, MIDI input, a Groovebox screen for editing the
  pattern, and panel layouts described in data: a desktop window, the KICK's panel, and a
  concept keyboard.
- `core/control`: a JUCE-free graph/transport session and initial in-process control
  interface for parameters, transport and UI notes, used by the desktop adapter; headless
  audio-equivalence, clock, queue-overflow and concurrent-control tests.
- initial JSON-RPC adapter for those controls, with a published wire contract/schema,
  protocol tests and JSON-driven audio equivalence tests; staged graph edits, coalescing,
  ordered gestures and independent client subscriptions.
- bounded Unix socket server/client, cached remote controls and host/device status;
  `oscairn-engine` owns live I/O and the desktop can use `--connect SOCKET`. Tests kill
  a controller process, verify audio continues, and reconnect to the existing state.

To do:

- tracks, patterns, swing and more lanes in the sequencer;
- complete the control API: automatic helper launch/supervision and live GUI crash/reopen
  testing before making separate-process mode the desktop default ([RFC 0003](rfcs/0003-control-api.md));
- LV2 hosting, right after the control API: a large catalog of open plugins already
  built for ARM Linux;
- define hardware capabilities, timestamped input, scheduled output and engine backend
  selection before hardware MIDI/CV implementation
  ([RFC 0004](rfcs/0004-hardware-services.md), proposed); the control API remains outside
  this performance-event path;
- the KICK's real panel as a control surface.

Done when: pressing pads and transport buttons programs and plays a pattern through the
new core, from a UI that talks to it only through the control API.

## Phase 4: hardware (started)

- Done: an Oscairn OS image built with current Buildroot, booting into Oscairn in QEMU's
  generic ARM64 machine (no emulator models the target chips).
- Hardware services ([RFC 0004](rfcs/0004-hardware-services.md)): first a CPU backend and
  simulated clocks/queues, then a board adapter. Inventory actual peripheral ownership,
  firmware availability and clock domains before assigning work to a real-time core or
  panel MCU. Validate input timestamps, scheduled MIDI/CV/clock output, cancellation,
  overflow handling and alignment with audio under load.
- Optional engine backends: capability discovery and resource reservations, then one
  reference engine on one available DSP, FPGA or real-time core. Compare sound, complete
  processing latency and sustained capacity with its CPU backend. Only use acceleration
  when a compatible implementation and measured benefit exist; keep CPU-only devices
  and offline rendering supported.
- The KICK (i.MX8M Mini), once beta units exist: an image for its board, and a
  hardware-services adapter for its panel, screen and audio.
- The Ignite (BE 0716-AAF), which uses the same board family and panel protocol: a 64-track
  MIDI and CV sequencer with a General MIDI sound engine. Its features need several MIDI
  ports (5 DIN pairs, USB device and USB host), CV/Gate with calibration, sync in and out,
  MIDI Machine Control, and sequencers that drive external instruments as well as internal
  engines.
- A DJ deck, once Behringer publishes its details (it has been teased; nothing is public
  yet): several clocks with decks as the master, high-resolution jog input, a track player
  with key lock, track analysis, DJ mixer outputs and cue, waveform streams, and the three
  ways of working with DJ software on a computer (see
  [architecture](architecture.md#dj-devices)). First Mixxx running on the device as its
  application, then a native Oscairn deck; no certification programmes.
- USB audio and MIDI in both directions, as a USB device to a computer and as a host for
  interfaces and controllers, on every device whose hardware allows it.
- An RK3588 board, once the target hardware is known: real-time kernel, audio on isolated
  cores, measured latency.

## Phase 5: the rest of the architecture

Each starts as an RFC: VST3 hosting; the sampler workflow (slicing, resampling,
looper); the Live workflow (splits, layers, scenes); file formats and the library;
persistent engines; engine recovery; signed updates with A/B system images; the platform as
a DAW plugin; and:

- **Packages:** sound packs, libraries and installable plugins, engines and sequencers in
  one package format, stored once by content hash and loaded once in memory, with no copy
  protection and no need for a network
  (see [architecture](architecture.md#packages-sound-packs-libraries-and-plugins)); the
  platform profile of built-in plugins; engines with factory sample libraries, user
  samples and the host's asset extension; then online catalogs, and sync with a paired
  computer.
- **Keeping plugins loaded:** a memory budget, unloading the least recently used, setlist
  preparation options and a setlist check; CPU, memory and storage meters.
- **Plugin looks:** pages and themes described in data, then sandboxed custom views.
- **Developer kit:** the MIT SDK and templates, the builder image, the `oscairn` tool,
  the validator, and plugin isolation in sandboxed workers.

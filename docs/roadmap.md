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

Still to decide: the control API's transport (local socket with OSC or JSON, or similar),
and the UI toolkit on devices.

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
control API, CPU and deadline measurements.

## Phase 3: first sequencer and first workflow (in progress)

Goal: something new running "on the device" before anyone has the device.

Done:

- a 16-step sequencer plugin whose steps (on/off, note, lock) are parameters, with one
  parameter-lock lane;
- a desktop app (JUCE) with live audio, MIDI input, a Groovebox screen for editing the
  pattern, and panel layouts described in data: a desktop window, the KICK's panel, and a
  concept keyboard.

To do:

- tracks, patterns, swing and more lanes in the sequencer;
- the control API, with the UI moving to its own process (RFC 0003, proposed);
- LV2 hosting, right after the control API: a large catalog of open plugins already
  built for ARM Linux;
- the KICK's real panel as a control surface.

Done when: pressing pads and transport buttons programs and plays a pattern through the
new core, from a UI that talks to it only through the control API.

## Phase 4: hardware (started)

- Done: an Oscairn OS image built with current Buildroot, booting into Oscairn in QEMU's
  generic ARM64 machine (no emulator models the target chips).
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
  [architecture](architecture.md#dj-devices)).
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

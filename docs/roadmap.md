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
- the control API, with the UI moving to its own process;
- the KICK's real panel as a control surface.

Done when: pressing pads and transport buttons programs and plays a pattern through the
new core, from a UI that talks to it only through the control API.

## Phase 4: hardware (started)

- An Oscairn OS image built with current Buildroot, booted first in QEMU's generic ARM64
  machine (no emulator models the target chips).
- An RK3588 development board with an I2S codec or a class-compliant USB interface:
  real-time kernel, audio on isolated cores, measured latency.
- The KICK (i.MX8M Mini) as a second target once units exist: a hardware-services adapter
  for its panel, screen and audio.

## Phase 5: the rest of the architecture

Each starts as an RFC: LV2 and VST3 hosting; the sampler workflow (slicing, resampling,
looper); the Live workflow (splits, layers, scenes); file formats and the library;
persistent engines; engine recovery; signed updates with A/B system images; the platform as
a DAW plugin.

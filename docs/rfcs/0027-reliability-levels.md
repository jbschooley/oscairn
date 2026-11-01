# RFC 0027: Reliability levels and the guaranteed audio path

Status: proposed, 2026-11-01. Required for every product Oscairn runs on. Builds on realtime
execution and failure ([RFC 0004](0004-hardware-services.md#realtime-execution-and-failure)),
plugin worker isolation ([RFC 0010](0010-plugin-worker-isolation.md)), device profiles
([RFC 0024](0024-device-profiles.md)) and resource budgets ([RFC 0025](0025-resource-budgets.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. The levels in the profile, and the engine's settings following from them
- [ ] 2. A watchdog outside the engine, restarting it within a stated time
- [ ] 3. Pass-through: audio in to out continuing through an engine failure
- [ ] 4. Measured: missed deadlines, recovery time and pass-through, on each product's hardware

## Problem

A desktop app that drops a block of audio is annoying. A stage keyboard that does it in a gig is
worse, and a guitar pedal, a mixer or a stagebox that goes silent stops the whole band: the
guitar, vocal or PA goes with it. RFC 0004 says what happens on a missed deadline (a defined mute
or bypass, recovery outside audio processing) and RFC 0010 keeps a crashing plugin from taking
the engine with it, but nothing says how hard each product must try, or what happens when the
engine itself fails.

## Outcome

Each product's profile names its **reliability level**, and the engine, the image and admission
follow it. At the highest level, audio coming in keeps going out whatever happens to the engine.

## Levels

| Level | For | Real-time setup | Admission margin (RFC 0025) | When the engine fails |
| --- | --- | --- | --- | --- |
| `desktop` | the desktop app, a DAW plugin | the operating system's audio, as configured | small | it stops; the app reports it |
| `instrument` | keyboards, modules, grooveboxes, DJ units, rack units | a real-time kernel, cores reserved for audio, locked memory | larger | the watchdog restarts it, the Performance reloads; silence until then, stated in seconds |
| `pass-through` | pedals, multi-effects, mixers, stageboxes, vocal processors | as `instrument`, with the smallest blocks the hardware allows | largest | audio in goes on to out, unprocessed or through a fixed fallback, while the engine restarts |

## The watchdog

A small process apart from the engine (the system's init or a supervisor it starts) watches the
engine's heartbeat: each audio block it completes. If the engine stops completing blocks, or
exits, the watchdog restarts it and has it reopen the Performance that was open, with the scene
that was selected. The restart time is measured per product and stated to the user; it does not
recover held notes or tails.

## Pass-through

Audio must keep passing with the engine gone, so it cannot depend on the engine:

- **Hardware bypass**, where the product has it: a relay or analog switch from input to output
  (a pedal's true bypass), closed by the watchdog when the engine stops, opened when it is back.
- **A fallback path** in the audio driver or a minimal process apart from the engine: input copied
  to output, or through a fixed, preallocated processing chain (a gain and a limiter for a mixer),
  with no plugins.

A `pass-through` product needs one or the other, and admission (RFC 0025) keeps the margin that
makes using it rare. Switching to and from it is click-free (a short crossfade), and is shown.

## Settings that follow from the level

- The block size and cores from the profile; at `instrument` and above, memory locked and the
  audio threads at real-time priority on reserved cores. How memory is locked, and the memory
  margin per level, are proposed in [RFC 0025](0025-resource-budgets.md#locking).
- Plugin worker isolation (RFC 0010) on for every plugin at `instrument` and above, so a plugin's
  crash silences the plugin, not the engine.
- Logging and diagnostics off the audio path, always.

## Open questions

- Whether `pass-through` products also need a second engine running hot, for a faster takeover
  than a restart.
- How a mixer's fallback chain is configured (fixed per product, or a minimal mix the user sets).

## Ruled out

- **One reliability level for every product:** a desktop does not need a watchdog and a relay;
  a pedal cannot do without.
- **Recovering held notes and tails after an engine restart** (RFC 0004 already rules out promising it).

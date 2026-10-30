# Modular engine evaluation

Research note, 2026-10-30. Whether to port chillbeast's *Modular* instrument engine from his
KICK firmware fork ([chillbeast/0700-AAM](https://github.com/chillbeast/0700-AAM), branch
`desktop-build` at `3598ab49`) as an Oscairn instrument plugin, and what keeping that port up to
date would cost. **Nothing is imported yet.**

## What it is

A patchable synth in the style of Drambo or Bitwig's Grid. Modules sit in an 8 × 6 grid: audio
runs down each column and the columns are mixed. Modules can be sources (oscillators, noise),
processors (filter, formant, ring mod, VCA, drive, delay, crusher, pan) or modulators (envelope, LFO, random, math). Any knob can
take modulation from a modulator, velocity, note, gate, mod wheel, bend, pressure or eight
macros. Cables add one audio module's output to another's input, which allows FM and feedback.
After the voices come up to four stereo effects (stereo delay, chorus, reverb, saturator). It also has
poly, mono and legato modes, glide, an arpeggiator, tempo-synced rates, factory and user
presets, and a full-screen patch editor. AGPL-3.0-or-later, the same licence as Oscairn.

| Part | Files | Lines |
| --- | --- | --- |
| Engine: graph build, voices, arpeggiator, crossfaded patch swaps | `mt_modular_engine.*` | 1,204 |
| Module and effect library | `mt_modular_modules.*` | 1,212 |
| Patch format, builder, factory presets, editing operations, user presets | `mt_modular_patch.*` | 1,311 |
| Patch editor screen (KICK's 1024 × 600 UI) | `kick_ModularEditor.*` | 1,182 |

All paths are under `Kick/modules/engine/audio_program/instrument/modular/`, except the editor,
which is in `Kick/modules/program_edit_ui/sources/instrument/`.

The engine implements KICK's `IInstrumentEngine` interface. That interface carries notes,
parameters, bend, mod wheel, pressure, tempo and `process()`, and nothing else from KICK, so the
engine does not depend on the sequencer, mixer or audio manager.

## Findings

- **The JUCE it uses is light in the DSP and heavy in the patch layer.**
  - The engine and modules use helpers: `jlimit`, `jmin`, `SpinLock`, `FloatVectorOperations`,
    `Decibels`, and `juce::Reverb` for the reverb effect.
  - The patch layer is built on JUCE types: `ValueTree` (49 uses), `UndoManager`, `File` and
    `XmlDocument` for user presets.
- **Events arrive at the start of each block.** KICK's `InstrumentObject` applies all queued
  events, then renders the whole block (`mt_instrument_object.cpp`), so there are no sample
  offsets.
  - An Oscairn wrapper can split rendering at each event's sample.
  - However, `render()` evaluates modulation on a 16-sample grid that restarts at every call. With
    splitting, the output would depend on where blocks and events fall, so it would not match
    Oscairn's rule that output is independent of block size.
  - Keeping the grid's position across calls is a small engine change that would be better
    contributed upstream than carried as a local patch.
- **Real-time safety looks deliberate:**
  - structural edits build a new graph off the audio thread and swap it in under a spin lock,
    crossfading over 20 ms;
  - knob and depth edits only write atomics;
  - the old graph is freed on the editing thread.

  The audio thread can still spin while the editing thread holds the lock to copy values and
  carry held notes over. I did not measure that wait.
- **Timing is free-running.** The arpeggiator counts samples from the tempo, and synced LFOs
  restart on notes; neither follows the transport's beat position. That is acceptable for an
  instrument, but an arpeggiator locked to the bar would need the CLAP transport.
- **Tests:** KICK's engine test app mentions Modular in its audit and optimisation regression
  tests (15 and 8 mentions); there is no Modular test in `InstrumentVoiceTests.h`. These tests
  run inside KICK's test app, so Oscairn would need its own render tests.
- **The upstream code changes fast.** The engine appeared on 2026-10-27, and its first three days
  brought six commits to the engine folder, about 3,900 added lines in all. It is one
  person's active work, so a port should expect frequent upstream changes for now.

## Ways to port it

| | A. Keep upstream files and JUCE | B. Plain C++ port |
| --- | --- | --- |
| Engine, modules and patch files | Imported unchanged, with history (`git filter-repo`, as planned for the sequencer) | Rewritten: helpers replaced; patch layer redone in Oscairn's patch text |
| Reverb | `juce::Reverb` as is | Replace (JUCE's Freeverb code may be reused with credit if the licence allows) |
| New Oscairn code | CLAP wrapper: events split at their sample, parameters (8 macros, level), state as patch XML, presets; a copy of `IInstrumentEngine` | The same wrapper, plus the rewritten patch layer |
| Patch editor | Not usable as is; needs an Oscairn editor in either case | Same |
| JUCE | First plugin to link JUCE (`juce_core`, `juce_audio_basics`, `juce_data_structures`); the hosts already fetch JUCE 9.0.4 | None |
| Upstream sync | Re-import and merge. Conflicts only where upstream changes the engine interface or files we patched | Every upstream change hand-ported into diverged code |
| Ongoing cost | Low: the wrapper changes only when the interface does | High while upstream is active |

## Recommendation

- **Take A:** import the three engine files unchanged with their history and credit, and write a
  thin CLAP wrapper with render tests.
- **Offer chillbeast the sample-grid change**, plus any other fixes, upstream rather than carrying
  them locally.
- **Look at B again once upstream settles**, or if JUCE inside a plugin causes trouble on the
  device (binary size, start-up, threads).
- **Leave the editor for later.** Until Oscairn has plugin editors, macros and presets give the
  musician control.

Not done:
- nothing was built or played;
- the engine's DSP was read for structure, not reviewed line by line;
- I did not check JUCE 9's licence terms for linking it into a plugin, or measure the spin-lock
  wait.

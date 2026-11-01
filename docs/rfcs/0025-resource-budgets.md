# RFC 0025: Resource budgets: knowing before loading whether it fits

Status: in progress, 2026-11-01: steps 1–4 built ([status](#status)). Required for
every product Oscairn runs on. Builds on the
voice budget ([RFC 0018](0018-voice-budget-and-stealing.md)), device profiles
([RFC 0024](0024-device-profiles.md)), packages' resource estimates
([RFC 0006](0006-packages-and-portability.md#platform-builds-and-hardware)) and hardware
reservations ([RFC 0004](0004-hardware-services.md#select-and-reserve-before-activation)).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. Measuring: each plugin's CPU per block (at rest and per voice) and memory, recorded on the device
- [x] 2. Estimating a Performance from those measurements and the device's budget
- [x] 3. Admission: loading, switching and editing check the estimate; refuse or warn with the reason
      (loading a Performance and adding a plugin are checked and refused with the reason; a warning instead of a refusal is not offered yet)
- [x] 4. Seamless changes counted: the outgoing Performance's plugins until they ring out
- [ ] 5. Estimates in packages and Sounds, checked at install and in the library

## Problem

A Performance that needs more than the device has fails while it plays: the audio misses its
deadline, or memory runs out while a sampler loads. On a desktop that is an annoyance; on a
pedal or a stage keyboard it is the failure that matters most. Today nothing knows, before a
Performance loads, whether it fits. RFC 0018 handles too many notes, once playing, by stealing
voices; it does not stop a Performance whose plugins alone are too heavy, and seamless changes
([RFC 0021](0021-sounds-performances-and-library.md#seamless-switching)) now hold two
Performances' plugins at once for a few seconds.

## Outcome

Before anything loads, Oscairn estimates what it needs (CPU per audio block, memory, voices,
reserved hardware) against the device's budget, and refuses, or warns and asks, saying what does
not fit. Estimates come from measurements on the device, not from the plugin's word alone.

## Measuring

- **CPU:** the plugin hosts already time each process call (CLAP and LV2, in the engine and in
  workers: the last and the largest block's time). Kept per plugin and build, on this device: the
  time per block at rest and the added time per sounding voice, as a high percentile (not the
  mean), at the device's block size.
- **Memory:** what a plugin instance holds after activation and after loading its state (samples
  included), from the worker process's accounting where it runs isolated (RFC 0010), else from
  the plugin's declaration.
- **Unmeasured plugins** get the package's declared estimate (RFC 0006), else a conservative
  default, and are marked unmeasured until they have played.
- Measurements are the device's own, in the user data, never published (RFC 0006 keeps private
  inventories private).

## The budget

The device profile gives the ceiling: the cores reserved for audio, the block size's deadline,
memory. The budget keeps a margin (by reliability level, RFC 0027: more on `pass-through` than on
`desktop`) and counts:

- every plugin's rest cost, plus its voices at the part's note limit (RFC 0018), or its declared
  maximum;
- the graph's own work (routing, mixing, meters);
- during a seamless change, the outgoing Performance's plugins until they ring out;
- memory for every plugin and its samples, plus what a seamless change holds twice.

## Admission

- **Loading a Performance or a Sound, adding a plugin, importing:** checked first. Over budget:
  refused with what does not fit ("needs 140% of the audio CPU: the Strings part's reverb and
  three samplers"), or, where the profile allows, offered with a warning.
- **A seamless change that would not fit with both Performances:** offered as a cut instead
  (the old Performance stops at once), saying so, before it happens. This is the "a change that
  cannot be seamless says so first" left open in RFC 0021.
- **While playing:** voices over budget are stolen (RFC 0018); a plugin that runs over its
  measurement is noted and its estimate raised.
- **Library and packages:** a Sound or Performance shows whether it fits this device, from its
  plugins' estimates, before it is opened.

## Status

Built 2026-11-01 (`core/control/src/budget.cpp`; `setBudget`, `estimate`; `budget.set`,
`budget.estimate`). The app sets the engine's budget from its device profile: one audio thread's
time less the reliability level's margin (desktop 90%, instrument 75%, pass-through 60%) and 80%
of the profile's memory. Until a budget is set nothing is measured or checked, so tests and tools
never write the user's data folder.

- **Measured:** at each service tick, every plugin's last block as a share of the block's time,
  kept as a slowly falling high-water mark (about ten minutes' half-life) per plugin build (ID
  and version), skipping its first 32 blocks; memory every second, a worker's share of its
  process's resident memory or, in the engine's process, what the process grew by while the
  plugin loaded. Kept in `measurements.json` in the data folder, saved every 30 seconds when
  changed and when the session ends. One sample a tick can miss a rare spike between ticks.
  Each sample is also split by the notes sounding through the plugin in that block (held, or
  sustained by the pedal; the graph counts them for every plugin that takes notes): with none,
  it updates the plugin's cost **at rest**; with some, its cost **per voice** (the time above
  rest, divided by the notes), each a falling high-water mark too, and the most notes seen at
  once. A plugin that takes no notes (an effect) is all at rest.
- **Estimated:** the open Performance, or one in a folder from its document's plugin identities;
  a plugin never measured here counts at 2% of the audio time and 32 MiB, marked unmeasured. A
  measured plugin in a part with a note limit (RFC 0018) counts at rest plus its cost per voice
  for that many notes, so the estimate is the part's worst case; with no limit, or before a
  voice was measured, it counts at its busiest block seen.
- **Admitted:** a Performance over the budget is refused, and so is a plugin that would take the
  open one over it (it is unloaded again), each saying what is needed, what is allowed and the
  three heaviest plugins. A Performance that fits alone but not beside the one playing (and any
  still ringing out) opens as a cut; the desktop says so after opening it.

`control-parts` checks a plugin measured while it plays and known to the next session, a plugin
and a Performance refused, and a change that becomes a cut when both would not fit and rings out
when they would. `desktop-profile` checks the profile's budget figures.

## Open questions

- How shared accelerators and DSP backends (RFC 0004) count against the budget.
- Whether a Performance may opt into "load what fits" (parts beyond the budget left off) for
  rehearsal.
- How long measurements stay valid across plugin and Oscairn updates.

## Ruled out

- **Trusting declared estimates alone:** plugins do not know the device, and some would claim less.
- **Silently degrading quality to fit** (fewer voices, lower oversampling) without the user's
  choice (RFC 0006: resource or quality reductions need an explicit policy).

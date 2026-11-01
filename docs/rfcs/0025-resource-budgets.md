# RFC 0025: Resource budgets: knowing before loading whether it fits

Status: proposed, 2026-11-01. Required for every product Oscairn runs on. Builds on the
voice budget ([RFC 0018](0018-voice-budget-and-stealing.md)), device profiles
([RFC 0024](0024-device-profiles.md)), packages' resource estimates
([RFC 0006](0006-packages-and-portability.md#platform-builds-and-hardware)) and hardware
reservations ([RFC 0004](0004-hardware-services.md#select-and-reserve-before-activation)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Measuring: each plugin's CPU per block (at rest and per voice) and memory, recorded on the device
- [ ] 2. Estimating a Performance from those measurements and the device's budget
- [ ] 3. Admission: loading, switching and editing check the estimate; refuse or warn with the reason
- [ ] 4. Seamless changes counted: the outgoing Performance's plugins until they ring out
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

- **CPU:** the engine already times each plugin's process call. Kept per plugin and build, on this
  device: the time per block at rest and the added time per sounding voice, as a high percentile
  (not the mean), at the device's block size.
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

## Open questions

- How shared accelerators and DSP backends (RFC 0004) count against the budget.
- Whether a Performance may opt into "load what fits" (parts beyond the budget left off) for
  rehearsal.
- How long measurements stay valid across plugin and Oscairn updates.

## Ruled out

- **Trusting declared estimates alone:** plugins do not know the device, and some would claim less.
- **Silently degrading quality to fit** (fewer voices, lower oversampling) without the user's
  choice (RFC 0006: resource or quality reductions need an explicit policy).

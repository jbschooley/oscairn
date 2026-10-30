# RFC 0008: SFZ-based native samples and Yamaha conversion

Status: in progress, 2026-10-29. This records the agreed direction to build on SFZ and
extend it where necessary. The [step 1 decision](#step-1-decision-baseline-profile-and-engine)
is recorded; step 2 is implemented as the Oscairn Sampler with the baseline profile (see
[Step 2 status](#step-2-status)). Extension syntax is not defined.

## Outcome and boundary

Musicians can build sample-based sounds in Oscairn, reuse SFZ libraries, share samples
between prepared sounds, and move complete Performances between supported devices.
Converters eventually support Oscairn ↔ selected SFZ profiles and Oscairn ↔ selected
Yamaha models/formats, with a preview of any missing or changed behavior. Jacob's MODX M
library is the first complete-library migration target.

Use SFZ as the starting point for the native sampler's editable patch description.
Preserve existing opcode meanings. Add documented Oscairn extensions only for a concrete
requirement that existing semantics cannot express. Do not create a parallel native
sampler format merely to rename SFZ regions, key ranges or envelope parameters.

Keep the existing [Performance container](../performance-format.md) for graph routing,
MIDI/channel assignments, multiple engines, shared effects and controller relationships.
FM, organ and other plugins retain their own documented state contracts. Sample-patch
text references owned audio assets; it does not contain engine binaries, active voices
or the entire Performance. Source SFZ, native snapshots and compiled runtime data must
have defined roles: compile a validated patch for playback, and save the current edited
patch rather than silently reverting to its original import. Compiled data is disposable,
not another portable file format.

This is an Oscairn dialect/profile, not a claim to define an official new SFZ version.
Publish the specification, conformance fixtures and implementation under explicit open
licenses; retain upstream notices for reused code. Propose generally useful extensions
upstream without making Oscairn releases depend on their adoption. Initial development
may replace drafts; [released formats require compatibility](../developer-guide.md#initial-development-compatibility-policy).

## Map behavior before adding syntax

Build a feature matrix using public SFZ documentation, the inspected open-source players,
Yamaha manuals and controlled exports from MODX M/ESP. For each feature record its units,
range, scope, event timing, reference behavior and disposition: existing opcode, required
extension, Performance-level feature, or deferred/unsupported. Track the tested player
and version; syntax support alone is insufficient evidence of equivalent sound.

| Area | Starting representation | What must be checked |
| --- | --- | --- |
| Key Banks / Waveforms | SFZ sample regions, note/velocity ranges and reusable includes where sufficient | Root pitch, tuning, stereo links, loop/end conventions, gains and fades |
| Elements and layers | Region/group defaults with explicit articulation conditions | Inheritance is not a shared DSP bus; sample reuse must not accidentally share envelopes, filters or voices |
| Articulations | Existing release, legato, sequence, random, switch and controller opcodes where supported | Selection/reset rules, overlapping notes, sustain, release velocity and deterministic test seeds |
| Envelopes, filters and modulation | Existing opcodes first | Curve/time units, modulation order, smoothing, per-voice versus shared scope and exact event application |
| Per-sound effects and outputs | Existing graph/effect-context contracts | Output ownership and outgoing tails; extend sampler-local routing only if the graph cannot express the needed voice-local behavior |
| Parts, Scenes, macro controls, arpeggios and other synthesis engines | Performance/control/event-plugin or engine-specific state | Do not flatten these into sampler opcodes or silently discard them on SFZ export |

SFZ's [hierarchy](https://sfzformat.com/headers/) and
[existing extensions](https://sfzformat.com/versions/) provide precedent, not an automatic
one-to-one Yamaha mapping. The [research note](../sample-players-and-formats.md) contains
player evaluations, converter candidates and the limits of the ESP inspection.

## Baseline and extension contract

Select a small, named baseline from features verified in the chosen player. Start with
one mono/stereo WAV region, root note, pitch/key tracking, ranges and defined gain/release
behavior; expand the profile only with working tests. Do not advertise full SFZ v1/v2
support from a partial implementation. UI sample import should produce this same patch
representation, so manual import and conversion use the same preparation path.

An extension proposal must provide:

- an actual sound/fixture requiring it, and why existing SFZ semantics are insufficient;
- a collision-resistant Oscairn name and documented placement/inheritance rules;
- value types, units, defaults, limits, modulation/event behavior and DSP ownership;
- a required capability/version identifier, validation rules and executable examples;
- import/export mappings and explicit behavior when the destination lacks the feature.

Use native manifest metadata to declare the baseline and required extensions. Define the
exact fields with the implementation and update the owning schema at that time. When
reading a standalone SFZ, inspect all supported includes/preprocessing and derive its
requirements; do not rely on metadata being present. Unsupported opcodes, headers or
preprocessor constructs must produce diagnostics with source location and affected sound.
Required unsupported behavior blocks native activation. An explicitly accepted conversion
may produce a reduced variant; it must not silently relabel the original as compatible.

Other SFZ players may ignore unfamiliar opcodes. Therefore an extended patch must be
identified as requiring Oscairn capabilities; ordinary-SFZ export is a deliberate operation
for a named player/profile, not merely copying or renaming the native file. Export each
supported instrument separately when the destination cannot represent a whole Performance.
Publish a machine-readable capability/mapping table with the converter so the UI and CLI
give the same results.

## Runtime, resources and persistence

Reuse the [native context lifecycle](0005-sound-contexts.md): prepare off the audio thread,
publish at the processing boundary, keep held voices on their original sound, and retire
only after selections, voices and tails release it. Preserve CLAP note IDs, repeated-key
ownership, sustain, poly/channel pressure, NOTE_END and overflow recovery.

Share immutable decoded audio between contexts using the same asset. Keep voice playback
positions, envelopes and mutable DSP separate. Establish immutable asset identity during
preparation; a filename alone must not reuse stale decoded data after an edit. Measure
sample storage, per-context/voice storage and total cost separately. No cross-process RAM
sharing or shared voice pool may be claimed merely because a library shares PCM.

The first slice fully preloads bounded WAV files. Validate decoded sizes before allocation;
bound channels, frames, rates, patch/region counts, include depth and total asset use.
Resolve relative paths within approved import roots, handle spaces/Unicode, and reject
traversal, escaping links, include cycles and unsupported encodings without changing the
playing graph. Parsing, file access, decoding and resource destruction stay off audio.
Streaming follows only with a measured memory/deadline policy and explicit underrun tests.

Samples are **dependencies** of a Performance, like the engine that plays them, not part
of its saved folder (Jacob, 2026-10-29: copying a library into every Performance costs
too much storage). Save every prepared sound's current patch through the existing snapshot
transaction, with each sample's location and content identity, never its audio. Loading
resolves and verifies each sample; a missing or changed sample, or an unsupported
requirement, fails without changing the current graph or the saved folder, as a missing
plugin does today. Moving Performances between devices is a packaging operation in
[RFC 0006](0006-packages-and-portability.md): it installs each sample library once in a
shared store and resolves references to it, together with future unavailable-node
preservation. [RFC 0009](0009-sound-assets-and-dependencies.md) defines the asset references,
libraries and plugin contract that replace the current absolute locations.

## Conversion and MODX M migration

The converter runs outside audio processing and uses one tested intermediate model for
supported instrument data plus the existing Performance model for composition. It is an
implementation detail, not an additional public file format. Native extensions must retain
information until an explicit destination mapping decides whether it can be represented.

Every conversion produces a structured report and a musician-facing summary. Identify
the affected Performance/Part/region/control, source and destination profiles, and classify
the result as **mapped**, **approximated**, **rendered to samples**, or **unavailable**.
Track audible verification separately: a translated parameter is not proof of equal DSP.
Examples of useful diagnostics are “Part 2 filter replaced; resonance differs” and
“Part 4 uses a factory waveform whose audio is unavailable,” not just “conversion warning.”
Show losses before export and require deliberate acceptance; write a new destination.
Keep source provenance and unknown data for later recovery, without pretending preserved
bytes implement missing behavior or guarantee a lossless round trip after edits.

Evaluate ConvertWithMoss for sample/SFZ adapters and YSFC Forge for MODX M/ESP Y2U/Y2L
mapping before writing replacements. Audit and pin the exact reused paths and preserve
author/license notices. Older MODX X8 support does not establish MODX M Y2 support.
Reader and writer support must be tracked independently for each model/firmware profile.

Inventory all source Performances, Parts/engines, sample/library references, effects,
Scenes, controllers, arpeggios and Live Set relationships. Distinguish included user PCM,
factory-ROM references, unavailable libraries and unsupported compression. File parsing
does not supply missing samples or reproduce FM-X, AN-X, filters and effects. Implement
those mappings incrementally; do not copy Yamaha DSP code or bundle factory content from
ESP. Optional multisampling from an authorized source is a clearly labeled alternative
with lost editability/expression, never an automatic substitute for conversion.

The migration goal is to account for every item without silent loss. Do not declare the
library ready because its files parsed. Verify converted behavior against hardware/ESP
reference recordings and controller sequences. Yamaha write-back must pass read-back and
target-device tests, including resource limits, before claiming supported export.

## Build order and completion gates

| Step | Deliverable | Gate before advancing |
| --- | --- | --- |
| 1. Profile and engine decision | Minimal opcode table, AWM2 gap matrix and pinned reuse evaluation; no speculative extension list | Compare existing candidates against native note/expression, preparation, state and resource contracts. Choose reuse/adaptation or a minimal implementation from demonstrated gaps. |
| 2. Native WAV/SFZ slice — Phase 3 | One-region patch, referenced WAV, root note, themed import/prepare/select and full snapshot with sample dependencies | Independent simultaneous sounds, held-note survival, stereo isolation, exact event timing, malformed/oversized inputs, bounded preparation, zero render allocations and safe concurrent retirement; a moved save reopens, and a missing or changed sample fails without replacing the live graph. |
| 3. Multisample baseline and ordinary-SFZ conversion | Regions/layers, loops and required articulations; profile-aware import/export reports | Boundary/trigger fixtures, reference renders, reported unsupported features, loss-preview tests and export/re-import comparisons. Expand the profile incrementally. |
| 4. Demonstrated Oscairn extensions | Only features required by tested sounds that step 3 cannot express | Published semantics, capability rejection, DSP/resource tests and a tested reduced-SFZ export or explicit unsupported result. |
| 5. MODX M migration | Read-only inventory, then one self-contained user-sample sound, then multi-Part/library conversion | Verify inventory coverage, sample identities, controls and sound against exported fixtures/ESP/hardware; every unresolved item remains visible. Add synthesis/effect mappings separately. |
| 6. Yamaha write-back | Writers for explicitly named models/firmware | Supported-subset round trips, resource validation and actual ESP/hardware load/play checks; source data unchanged. |

Steps 3–6 are later increments, not blockers for the first sample sound. Development
fixtures use generated or appropriately licensed samples. Audible acceptance remains
queued until Jacob is available; offline renders do not count as listening approval.
Cross-platform validation must cover macOS/Linux and the supported Buildroot target;
hardware performance claims require measurements on the target, not just QEMU.

## Step 1 decision: baseline profile and engine

Recorded 2026-10-29, before implementation. The opcode table is the profile for the first
slice only; it is not a claim of SFZ v1 conformance. Meanings follow the
[SFZ opcode reference](https://sfzformat.com/opcodes/). Reference renders in step 2 must
compare against a named player (liquidsfz at the pinned revision) before this table
becomes published opcode documentation.

### Baseline profile `oscairn-sfz-baseline-0`

| Opcode | Meaning, default and accepted range | Note |
| --- | --- | --- |
| `<region>` | Exactly one region per patch. | Any other header is unsupported. |
| `sample` | Relative path to a mono or stereo WAV, resolved from the patch's directory. Required. | Absolute paths, `..` escapes, links leaving the import root and other formats are rejected. |
| `lokey`, `hikey` | Note range, 0–127, defaults 0 and 127. Accept MIDI numbers and note names. | `lokey` > `hikey` is an error. |
| `key` | Sets `lokey`, `hikey` and `pitch_keycenter` together. | Same parser as above. |
| `lovel`, `hivel` | Velocity range, 1–127, defaults 1 and 127. | Outside the range: region does not sound. |
| `pitch_keycenter` | Root note, 0–127, default 60. | |
| `pitch_keytrack` | Cents per key, −1200–1200, default 100. | |
| `transpose` | Semitones, −127–127, default 0. | |
| `tune` | Cents, −100–100, default 0. | |
| `volume` | dB, −144–6, default 0. | |
| `ampeg_release` | Seconds, 0–100, default 0.001. | Sample end before release ends the voice (SFZ `no_loop`). |

Fixed behavior in this profile: `loop_mode=no_loop`, the default `amp_veltrack=100`
velocity curve, no attack/decay/sustain shaping beyond SFZ defaults, linear interpolation
for pitch and sample-rate conversion, and `//` line comments. Every other opcode, header,
`#include`/`#define` and block comment is a diagnostic that blocks preparation, with line
number. Loops, layers, articulations, filters, pan and preprocessing are step 3.

### AWM2 gap matrix (documentation level)

Terms come from the MONTAGE M manuals and the [ESP observations](../yamaha-esp-samples-research.md).
Units, curves and selection rules are **not** verified against exports or hardware.

| Yamaha concept | SFZ starting point | Disposition |
| --- | --- | --- |
| Key Bank note/velocity range, center note | `lokey`/`hikey`, `lovel`/`hivel`, `pitch_keycenter` | Existing opcodes; in this profile. |
| Key Bank level, tuning | `volume`, `tune` | Existing opcodes; unit mapping unverified. |
| Key Bank pan | `pan` | Existing opcode; step 3. |
| Waveform loop points | `loop_mode`, `loop_start`, `loop_end` | Existing opcodes; step 3. |
| Element note/velocity limits and crossfades | ranges, `xfin_*`/`xfout_*` | Existing opcodes; step 3. |
| XA control (legato, key-off, cycle, random, assignable switches) | `trigger`, `seq_*`, `lorand`/`hirand`, `sw_*`/`on_locc` | Existing opcodes; selection/reset rules must be compared before mapping. |
| Element amplitude, pitch and filter EGs | `ampeg_*`, `pitcheg_*`, `fileg_*` | Existing opcodes; curve and time units likely differ, so **approximated** until measured. |
| Element filter types | `fil_type` | Partial; types SFZ lacks are the first extension candidates, only with a fixture. |
| Element insertion-effect connection | Performance graph, [RFC 0007](0007-effect-contexts.md) | Not a sampler opcode. |
| Motion Sequencer, Super Knob, Scenes, arpeggios | Performance/control/event plugins | Not sampler opcodes. |
| FM-X, AN-X Parts | Other engines | Not sampler opcodes. |
| Factory-ROM or compressed waveforms | — | **Unavailable** unless audio is supplied by the user. |

### Engine decision: native voice engine, reused SFZ parts

Decided by Jacob, 2026-10-29, after the [ESP library inventory](../yamaha-esp-samples-research.md#jacobs-library-and-factory-content-2026-10-29).
Converting Yamaha sounds requires envelopes, filters and interpolation tuned against ESP
measurements, which existing SFZ players do not model; large SFZ pianos require mature
parsing and disk streaming, which they do.

Evidence: liquidsfz `4cc901d1` (see the [research note](../sample-players-and-formats.md#integration-gaps-verified-in-source)).
Its public API cannot satisfy the native note contract without patching the library:

- `note_off(channel, key)` releases every attack voice on that key, so neither CLAP
  note-ID release nor raw-MIDI oldest-held-first repeated-key release is expressible.
- There are no note IDs, `NOTE_END` reports, per-context voice counts or pressure inputs;
  only aggregate `active_voice_count()`.
- Samples load by filename through a cache keyed on the path, with a background loader
  thread. Content identity of an owned asset and preparation from validated in-memory PCM
  need a different entry point.
- `load()` stops voices, so each context would need a separate `Synth` and voice pool.

The baseline above needs only a one-region parser, a WAV decode and a pitched,
interpolated read with a release envelope. Adapting liquidsfz would mean forking its voice
allocation and cache, which is larger than the baseline and would leave a second lifetime
implementation. Therefore:

- Write the voice DSP in Oscairn. Decode with **libsndfile**, made an explicit production
  dependency in desktop CI and Buildroot.
- Before writing a parser, check whether the sfizz (BSD-2-Clause) or liquidsfz (MPL-2.0)
  SFZ parser and streaming code can be separated from their voice engines; liquidsfz's
  `loader.cc` includes `synth.hh` and the sample cache, so it is not separable unedited.
  Reuse whichever needs fewer changes, pinned and credited.
- Put sample DSP in a new **Oscairn Sampler** CLAP plugin. Move Basic Synth's context,
  selection, voice-ownership and snapshot code into one shared header used by both plugins,
  with Basic Synth's existing tests unchanged as the regression check. Adding samples to
  the Basic Synth would mix its filter/parameters into SFZ playback semantics.
- Keep liquidsfz as the reference renderer for step 2 fixtures and as the first candidate
  to revisit for step 3, when multisample features make a parser of our own expensive.
  Revisit if its upstream gains note IDs and per-voice release.
- Extend `prepare` with an explicit patch text plus asset directory and a diagnostic buffer
  (ABI draft 3). Samples are referenced dependencies, so no asset-collection API is needed.

### Step 2 status

Implemented 2026-10-29 and checked by the automated tests named below; **audible
acceptance has not happened** and is queued with the other listening tests.

- [Oscairn Sampler](../../plugins/sampler/sampler.cpp) plays the baseline profile over
  preloaded mono/stereo WAV with libsndfile; behavior and limits are in the
  [plugins guide](../../plugins/README.md#sampler-sounds). Basic Synth and Sampler share
  [one context implementation](../../plugins/common/SoundContexts.h).
- `sounds.prepare` takes `sfz` with an asset `library` and `directory` (RFC 0009) through typed, JSON and remote controls; the
  desktop picker imports a `.wav` or `.sfz` file through the same model code.
- The `sampler` test covers exact timing, pitch, velocity, held-note survival, simultaneous
  sounds, note-ID release, stereo isolation, ranges, sample end, release, every rejection
  path, content-based sharing within one instance, the 256 MiB budget, concurrent
  preparation/retirement with the allocation guard, JSON diagnostics and saved sample
  dependencies, including reopen failures that name the missing or changed sample. It and the picker
  test also pass under ThreadSanitizer and AddressSanitizer/UBSan on macOS (ASan with
  `detect_odr_violation=0`: every plugin exports `clap_entry`).
- Linux and macOS CI, including Linux ThreadSanitizer, pass with the libsndfile dependency.
- Reference renders against liquidsfz `4cc901d1` ([comparison tool](../../tools/sampler-research/liquidsfz_compare.cpp),
  [results](../sample-players-and-formats.md#reference-renders-against-liquidsfz)): after
  matching level, pitch, transposition, tuning, volume, velocity and release agree within
  0.34% RMS. The release uses liquidsfz's exponential curve, and (Jacob, 2026-10-30) the
  Sampler adopts liquidsfz's default level: CC7 volume 100 on its squared curve and the
  sine pan law at centre, together −7.2 dB. Levels now agree within 0.05 dB; the remaining
  0.6% is not in liquidsfz's gain path and is unexplained. CC7 and CC10 changes are not
  implemented yet; only their default values are matched.
- Step 3 groundwork: reuse sfizz's parser and value helpers, implement the region model and
  streaming in Oscairn following sfizz's `FilePool` design ([research](../sample-players-and-formats.md#parser-and-streaming-reuse-for-multisample-step-3-groundwork)).
- Not done: FLAC, multisample/streaming (step 3, required by the Salamander Grand Piano:
  641 regions, release triggers, round robin and pedal triggers in 12 groups), and an
  interactive run of the desktop import, queued as [listening test 3](../listening-tests.md#3-sampler-import-and-playback-rfc-0008-step-2).

## Integration checklist

Implementation must update the native context ABI and all its callers/fixtures together,
CLAP/plugin-host asset/state handling, typed/JSON/remote controls and schemas, Performance
snapshot tests, the themed sound picker, production dependency declarations, desktop CI,
Buildroot configuration and developer examples. Reuse the Basic Synth's context ownership
without maintaining a second competing lifetime implementation. The engine-selection
step decides whether sample DSP lives there or in an adapted native plugin.

Keep this RFC as the contract and build order, the research note as evidence, and the
roadmap as current status. Publish opcode documentation, extension requirements, conversion
coverage and known limitations alongside each implemented slice. Do not add unused schema
fields or placeholder DSP now. Deferred work includes full Yamaha emulation, streaming,
package-wide deduplication, and a universal preset format for every synthesis engine.

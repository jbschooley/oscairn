# Roadmap

How Oscairn gets from nothing to the [architecture](architecture.md): the smallest steps
that each produce something that runs. Nobody has the target hardware yet, so everything
early runs on a desktop.

Status as of October 29, 2026. Implementation status below includes the current branch;
it does not mean every completed item has merged to `main`.

Initial development has [no backward-compatibility obligation](developer-guide.md#initial-development-compatibility-policy)
for earlier Oscairn builds. Replace obsolete structures and recreate development fixtures;
keep strict validation and clear rejection of unsupported data.
Backward compatibility becomes a requirement with the first public product release;
define its version, migration and deprecation policy before that release.

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

- a two-track sequencer with editable A/B patterns, independent 1–16-step lengths, MIDI
  channels, mutes and lock lanes; exact bar-boundary launches, KICK controls and Performance
  save/reopen. [Listening checks remain queued](listening-tests.md);
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
- default managed desktop helper: bundled executable/plugins, concurrent launch protection,
  background engine lifetime, UI reattachment and explicit recovery after engine failure.
- audible verification of the default separate-process desktop in the KICK layout:
  Jacob confirmed SMK37 notes, pressure response and PLAY/STOP on October 28, 2026.
- optional LV2 hosting through Lilv: mixed CLAP/LV2 graphs, sample-timed control ports,
  MIDI/pressure translation and time atoms, with fixture and upstream-example tests.
- [LV2 prior-art audit](../core/lv2-host/AUDIT.md) against Jalv, MOD mod-host, Carla and
  Ardour, with contract/recovery fixes, regression tests and source credits.
- LV2 Worker scheduling adapted from Jalv, with bounded queues, draining teardown,
  synchronous offline work and independent upstream sampler Worker tests.
- shared CLAP/LV2 plugin state API; LV2 default/bundle presets, portable mapped sample
  assets and bounded patch properties, with upstream sampler and production samplv1 tests.
  State export, preset enumeration and typed patch properties now also have typed/JSON/remote
  controls; confirmed readback and state round trips are tested. Performance persistence is implemented below.
- explicit graph audio ports/channel ranges, external inputs, independent outputs,
  sidechains and gain-controlled sums; typed/JSON/remote routing, desktop device-channel
  mapping and availability reporting. Independent master/cue/record buses and mixed
  CLAP/LV2 routing are tested; the desktop's default stereo workflow is preserved.
- initial native sound contexts in the Basic Synth: prepared patches, independent channel
  selection, held-voice preservation, note-ID/FIFO ownership, sustain and safe retirement;
  typed/JSON/remote controls and concurrent reference-audio tests ([RFC 0005](rfcs/0005-sound-contexts.md)).

- prepared-instance switching for ordinary CLAP/LV2 instruments: defaults, state/preset
  preparation, independent channel parts, held-note ownership and explicit tail retirement;
  typed/JSON/remote controls and fixture/production reference-audio tests.

- transpose and MIDI delay CLAP reference plugins, including mapped releases/pressure,
  sustain, bounded queues, exact timing, stop/overflow recovery and portable state;
  mixed audio-to-MIDI graph regression and documented feedback/desktop-host boundaries.
- desktop sound picker for the loaded instrument: Basic Synth variations, advertised
  presets, explicit prepare/use/remove, playing readback and safe retirement. Separate-engine
  requests run off the UI thread; prepared sounds survive UI reconnect. Native and LV2
  remote tests, actual KICK UI controls and live 128-frame reconnect/playback checks passed.
  Demo brightness locks are opt-in so sequencing preserves prepared sound differences;
  render regression covers sequencer playback followed by pads.
  Basic Synth contexts also own oscillator mode, saw detune and pitch LFO settings;
  detuned-saw and square-vibrato sounds overlap and play as independent channel parts.
  In-process diagnostics disable preparation; Performance persistence is implemented below.


- desktop plugin browser: isolated CLAP/LV2 discovery, search and folder selection,
  generic parameters/presets and atomic instrument/effect replacement. Failed loads preserve
  the working graph; compatible main buses remap, ambiguous buses and parameter mappings
  require explicit edits. The first UI exposes the demo instrument/effect slots.
- shared [design language and installable themes](design-language.md): semantic colours,
  typography and control styling across layouts, browser and generated editors; built-in
  Dark/Light shipped as replaceable theme folders with the bundled Inter font, validated
  theme-folder installation and per-device selection shared across UI processes.
  Theme changes do not touch audio or routing.

To do:

- swing, additional tracks/patterns and more lanes in the sequencer;
- automatic Sound/effect-chain preparation and retirement, asset-bearing native patches and Setlist/library persistence;
- carry stable format/plugin identities and state provenance into persistent library data,
  following [RFC 0006](rfcs/0006-packages-and-portability.md). Installation paths locate
  binaries but must not become portable package identities. Custom plugin pages and
  broader graph editing follow the current generic editor.
- define hardware capabilities, timestamped input, scheduled output and engine backend
  selection before hardware MIDI/CV implementation
  ([RFC 0004](rfcs/0004-hardware-services.md), proposed); the control API remains outside
  this performance-event path;
- the KICK's real panel as a control surface.
- shared controller mappings with knob/slider pickup, direct position access,
  relative-encoder tracking and explicit feedback/ownership rules; plugins may opt
  into physical observations, accepted changes or both
  ([planned architecture](architecture.md#physical-controls-takeover-and-direct-position-access-planned)).

Done when: pressing pads and transport buttons programs and plays a pattern through the
new core, from a UI that talks to it only through the control API.

### Next build and test order

1. **Native effect contexts and per-sound audio — implemented foundation:** Delay contexts,
   Basic Synth output buses and typed/JSON/remote controls now pass independent-reference,
   lifetime, tail and resource tests ([RFC 0007](rfcs/0007-effect-contexts.md)). Explicit
   routing preserves outgoing voices/chains; automatic chain management remains future work.
2. **Minimal save/reopen — implemented:** [versioned JSON Performance folders](performance-format.md)
   retain graph, all prepared sounds, shared effect state, assets and mappings. Restore
   validates a replacement before publication. A missing dependency preserves the current
   graph and original folder; opening unresolved nodes as disabled placeholders, archive
   packaging and dependency resolution remain future work. Process restart, missing
   plugin/assets, save failure and render/state comparisons are tested.
3. **Two tracks and A/B patterns — implemented:** independent lengths, channels and mutes,
   bar-boundary launches, per-track sound selection and full parameter persistence.
   Automated timing/roundtrip tests pass; Jacob listened on October 30, 2026
   ([results](listening-tests.md)); holding a pad while switching tracks remains untested.
4. **Native sample patches — first slice implemented, multisample pending:**
   [sample players/formats and the first slice](sample-players-and-formats.md), including
   a reproducible liquidsfz cache-sharing probe and [ESP observations](yamaha-esp-samples-research.md).
   The Oscairn Sampler plays one-region SFZ patches over preloaded WAV with shared PCM,
   held-note ownership, desktop import and samples saved as Performance dependencies
   ([RFC 0008 step 2](rfcs/0008-sfz-native-samples-and-conversion.md#step-2-status));
   Jacob listened on October 30, 2026. Next: multisample, FLAC and streaming for large
   pianos, reusing sfizz/liquidsfz parsing where it separates cleanly. Extend SFZ only for
   demonstrated gaps; SoundFont remains a separate engine candidate.
5. **Sound assets as dependencies — steps 1–2 implemented:** asset store, registered
   folders, the `oscairn.assets` contract and Performance preflight
   ([RFC 0009](rfcs/0009-sound-assets-and-dependencies.md#implementation-status)).
6. **Plugin worker isolation — next:** run plugins in supervised worker processes so a
   crash, hang or overrun silences only its own node and the plugin recovers
   ([RFC 0010](rfcs/0010-plugin-worker-isolation.md)). Moved here from Phase 5 on October 30,
   2026, ahead of Phase 4 hardware work and further third-party plugin features, starting with
   the hand-off measurements.
7. **Sequencer evaluation — written, decisions pending:** [the comparison](sequencer-evaluation.md)
   of the KICK firmware's sequencer (with chillbeast's work), zynseq and hosted plugins
   recommends the KICK model and workflow with sample-accurate playback rewritten in an
   Oscairn plugin. Swing and further sequencer workflow follow from that choice.
   Keep the shared theme and generated-editor contract current as new screens are added.

The subsequent [conversion plan](rfcs/0008-sfz-native-samples-and-conversion.md#conversion-and-modx-m-migration)
covers SFZ interchange and Jacob's MODX M library migration: inventory first, explicit
fidelity/loss reports, then tested sound/Performance adapters and model-specific Yamaha
write-back. It is planned work, not part of the first WAV implementation.

Package-manager comparison comes at the package milestone, before permanent distribution
format commitments: evaluate libmamba/Nix and libsolv as a narrower fallback using the
[defined prototype](rfcs/0006-packages-and-portability.md#implementation-candidates-and-decision-timing).
No package-manager dependency or prototype is required for the next desktop step.

## Phase 4: hardware (started)

- Map the existing logical audio endpoints onto each board's real inputs/outputs and
  validate simultaneous capture/playback, channel identities, isolation, clocking and
  latency. DJ master/booth/cue controls use the existing independent-bus routing; their
  complete workflow and UI follow with the DJ application.

- Done: an Oscairn OS image built with current Buildroot, booting into Oscairn in QEMU's
  generic ARM64 machine (no emulator models the target chips).
- Hardware services ([RFC 0004](rfcs/0004-hardware-services.md)): first a CPU backend and
  simulated clocks/queues, then a board adapter. Inventory actual peripheral ownership,
  firmware availability and clock domains before assigning work to a real-time core or
  panel MCU. Validate input timestamps, scheduled MIDI/CV/clock output, cancellation,
  overflow handling and alignment with audio under load.
- Optional engine backends: capability discovery, conditional backend-package resolution
  and whole-performance resource reservations, first with simulated devices and then one
  reference engine on one available DSP, FPGA or real-time core. Separate equivalent
  implementations from explicit hardware-specific sound/quality modes. Install only each
  target's selected backend dependencies; reject unavailable chip services, incompatible
  FPGA regions/firmware and CPU fallbacks that cannot sustain the sound. Compare sound,
  complete processing latency and sustained capacity with its CPU backend. Only use acceleration
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

Each starts as an RFC: VST3 hosting, macOS Audio Units hosting (AUv2/AUv3 scope to decide),
Windows desktop/process/IPC support; the sampler workflow (slicing, resampling,
looper); the Live workflow (splits, layers, scenes); file formats and the library;
persistent engines; engine recovery; signed updates with A/B system images; the platform as
a DAW plugin; and:

- **Packages and portability** ([RFC 0006](rfcs/0006-packages-and-portability.md), proposed):
  first a clean-machine, offline Performance/Setlist transfer with managed CLAP packages,
  project manifests/exact dependency locks, target builds, project-local dependency views
  backed by a shared installed store, shared runtime/sample packages and unavailable-node
  preservation. Default exports to supported Buildroot Linux profiles, with advanced target
  selection; import installs only relevant artifacts, fetches exact missing target builds
  when enabled, or reports required installations. Keep disposable download caches separate
  from pinned offline installations. Define revision ancestry and per-device assignments,
  variations and profile versions with the persistence schema. Include Used by, grouped
  unused-content prompts, per-device Keep installed policy and safe removal/reinstall in
  the first package slice. Track retained history, shared dependencies and live resources;
  preserve user data and unavailable-node state when a used package is deliberately removed.
  Prove dependency-only repair from existing bundles and offline missing-content requests
  fulfilled by a computer, without reverting musical edits. Later add catalog acquisition
  and USB companion transfer through the same repair/import path, with stale-request checks.
  Storage deduplication and RAM sharing have separate contracts. Prove state portability across supported CPU/OS/runtime profiles, then broaden
  to LV2, external installations and AU/VST3 alongside their adapters; cross-format state
  conversion requires tested mappings. Baseline profiles identify what may be omitted from
  a complete export. Catalogs and paired-device delta sync follow the same package model;
  vendor-specific installers/activation remain explicit external dependencies. Maintain a
  reviewed Git capability-profile catalog with authenticated offline snapshots and explicit
  verification status. Add multiple-device preparation/readiness and revision-aware editing
  round trips without silently merging opaque states or overwriting rehearsed setlists.
- **Keeping plugins loaded:** a memory budget, unloading the least recently used, setlist
  preparation options and a setlist check; CPU, memory and storage meters.
- **Native plugins outside Oscairn:** first one engine as a direct CLAP/VST3 plugin on
  supported Windows/macOS/Linux targets and AU on macOS, with shared canonical state/assets
  and tested host/format behavior; evaluate existing wrappers. Then the whole Oscairn graph
  as a DAW plugin and optional individual standalone shells. Neither path requires the
  separate Oscairn app to be running. Test old DAW-session state, runtime versions and
  missing assets; another DAW does not automatically understand our package lock.
- **Plugin looks:** pages and themes described in data, then sandboxed custom views.
- **Developer kit:** the MIT SDK and templates, the builder image, the `oscairn` tool,
  the validator. (Plugin isolation in sandboxed workers moved up to Phase 3, item 6.)

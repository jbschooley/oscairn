# Roadmap

How Oscairn gets from nothing to the [architecture](architecture.md): the smallest steps
that each produce something that runs. Nobody has the target hardware yet, so everything
early runs on a desktop.

Status as of October 31, 2026. Implementation status below includes the current branch;
it does not mean every completed item has merged to `main`.

A checked box is built and tested; an unchecked one is still to do, with what is already done
said beside it. A box is checked in the change that builds it, together with the
**Progress** list of its RFC ([RFCs](rfcs/README.md)).

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

- [x] a new core in C++20, with no JUCE in the core;
- [x] CLAP hosting seeded from Randy Chabot's host in KICK PR #13, imported with its history;
- [x] AGPL-3.0-or-later, with plugin loading as the licence boundary
  ([RFC 0001](rfcs/0001-licence.md)).

The control API uses JSON-RPC over a local socket ([RFC 0003](rfcs/0003-control-api.md)).
The UI toolkit on devices is proposed in [RFC 0019](rfcs/0019-adaptive-ui.md#toolkit): JUCE for
the desktop and touch panels, chosen later for small screens.

## Phase 2: headless core on the desktop (milestone met)

Milestone: a script builds a graph (CLAP sequencer → CLAP synth → CLAP effect), plays it
in sync with MIDI clock, and renders an identical file in CI on every run.

- [x] realtime graph with lock-free live editing; CLAP hosting; sample-accurate event routing;
- [x] transport with its own tempo or an external MIDI clock;
- [x] the core as a library that does not own the audio device or the process;
- [x] parameter events between plugins ([RFC 0002](rfcs/0002-param-target.md));
- [x] offline renderer, and CI on Linux and macOS with a ThreadSanitizer run;
- [ ] the hardware-services interface ([RFC 0004](rfcs/0004-hardware-services.md));
- [x] file formats and a library index: Performances, Sounds and the library
  ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)); packages and Projects remain
  (Phase 5);
- [ ] the complete control API: transport, engine and UI separation done (Phase 3); the OS
  services split and the OSC bridge remain ([RFC 0003](rfcs/0003-control-api.md#progress));
- [ ] CPU and deadline measurements.

## Phase 3: first sequencer and first workflow (milestone met; one check open)

Goal: something new running "on the device" before anyone has the device.

- [x] a two-track sequencer with editable A/B patterns, independent 1–16-step lengths, MIDI
  channels, mutes and lock lanes; exact bar-boundary launches, KICK controls and Performance
  save/reopen. [Listening checks remain queued](listening-tests.md);
- [x] a desktop app (JUCE) with live audio, MIDI input, a Groovebox screen for editing the
  pattern, and panel layouts described in data: a desktop window, the KICK's panel, and a
  concept keyboard.
- [x] `core/control`: a JUCE-free graph/transport session and initial in-process control
  interface for parameters, transport and UI notes, used by the desktop adapter; headless
  audio-equivalence, clock, queue-overflow and concurrent-control tests.
- [x] initial JSON-RPC adapter for those controls, with a published wire contract/schema,
  protocol tests and JSON-driven audio equivalence tests; staged graph edits, coalescing,
  ordered gestures and independent client subscriptions.
- [x] bounded Unix socket server/client, cached remote controls and host/device status;
  `oscairn-engine` owns live I/O and the desktop can use `--connect SOCKET`. Tests kill
  a controller process, verify audio continues, and reconnect to the existing state.
- [x] default managed desktop helper: bundled executable/plugins, concurrent launch protection,
  background engine lifetime, UI reattachment and explicit recovery after engine failure.
- [x] audible verification of the default separate-process desktop in the KICK layout:
  Jacob confirmed SMK37 notes, pressure response and PLAY/STOP on October 28, 2026.
- [x] optional LV2 hosting through Lilv: mixed CLAP/LV2 graphs, sample-timed control ports,
  MIDI/pressure translation and time atoms, with fixture and upstream-example tests.
- [x] [LV2 prior-art audit](../core/lv2-host/AUDIT.md) against Jalv, MOD mod-host, Carla and
  Ardour, with contract/recovery fixes, regression tests and source credits.
- [x] LV2 Worker scheduling adapted from Jalv, with bounded queues, draining teardown,
  synchronous offline work and independent upstream sampler Worker tests.
- [x] shared CLAP/LV2 plugin state API; LV2 default/bundle presets, portable mapped sample
  assets and bounded patch properties, with upstream sampler and production samplv1 tests.
  State export, preset enumeration and typed patch properties now also have typed/JSON/remote
  controls; confirmed readback and state round trips are tested. Performance persistence is implemented below.
- [x] explicit graph audio ports/channel ranges, external inputs, independent outputs,
  sidechains and gain-controlled sums; typed/JSON/remote routing, desktop device-channel
  mapping and availability reporting. Independent master/cue/record buses and mixed
  CLAP/LV2 routing are tested; the desktop's default stereo workflow is preserved.
- [x] initial native sound contexts in the Basic Synth: prepared patches, independent channel
  selection, held-voice preservation, note-ID/FIFO ownership, sustain and safe retirement;
  typed/JSON/remote controls and concurrent reference-audio tests ([RFC 0005](rfcs/0005-sound-contexts.md)).

- [x] prepared-instance switching for ordinary CLAP/LV2 instruments: defaults, state/preset
  preparation, independent channel parts, held-note ownership and explicit tail retirement;
  typed/JSON/remote controls and fixture/production reference-audio tests.

- [x] transpose and MIDI delay CLAP reference plugins, including mapped releases/pressure,
  sustain, bounded queues, exact timing, stop/overflow recovery and portable state;
  mixed audio-to-MIDI graph regression and documented feedback/desktop-host boundaries.
- [x] desktop sound picker for the loaded instrument: Basic Synth variations, advertised
  presets, explicit prepare/use/remove, playing readback and safe retirement. Separate-engine
  requests run off the UI thread; prepared sounds survive UI reconnect. Native and LV2
  remote tests, actual KICK UI controls and live 128-frame reconnect/playback checks passed.
  Demo brightness locks are opt-in so sequencing preserves prepared sound differences;
  render regression covers sequencer playback followed by pads.
  Basic Synth contexts also own oscillator mode, saw detune and pitch LFO settings;
  detuned-saw and square-vibrato sounds overlap and play as independent channel parts.
  In-process diagnostics disable preparation; Performance persistence is implemented below.

- [x] desktop plugin browser: isolated CLAP/LV2 discovery, search and folder selection,
  generic parameters/presets and atomic instrument/effect replacement. Failed loads preserve
  the working graph; compatible main buses remap, ambiguous buses and parameter mappings
  require explicit edits. The first UI exposes the demo instrument/effect slots.
- [x] shared [design language and installable themes](design-language.md): semantic colours,
  typography and control styling across layouts, browser and generated editors; built-in
  Dark/Light shipped as replaceable theme folders with the bundled Inter font, validated
  theme-folder installation and per-device selection shared across UI processes.
  Theme changes do not touch audio or routing.

Done when: pressing pads and transport buttons programs and plays a pattern through the
new core, from a UI that talks to it only through the control API.

**Milestone met** (October 31, 2026): the two-track sequencer, the Groovebox screen and the
KICK layout drive a separate engine process only through the control API; Jacob checked
notes, pressure and PLAY/STOP on October 28 and listened to the sequencer on October 30
([results](listening-tests.md)). One check is open: holding a pad while switching tracks.
The work still open moved to [Phase 3b](#phase-3b-performance-instrument-in-progress).

- [ ] holding a pad while switching tracks, checked by listening ([listening tests](listening-tests.md))

### Finished in the build order

- [x] **Native effect contexts and per-sound audio — implemented foundation:** Delay contexts,
  Basic Synth output buses and typed/JSON/remote controls now pass independent-reference,
  lifetime, tail and resource tests ([RFC 0007](rfcs/0007-effect-contexts.md)). Explicit
  routing preserves outgoing voices/chains; automatic chain management remains future work.
- [x] **Minimal save/reopen — implemented:** [versioned JSON Performance folders](performance-format.md)
  retain graph, all prepared sounds, shared effect state, assets and mappings. Restore
  validates a replacement before publication. A missing dependency preserves the current
  graph and original folder; opening unresolved nodes as disabled placeholders, archive
  packaging and dependency resolution remain future work. Process restart, missing
  plugin/assets, save failure and render/state comparisons are tested.
- [x] **Two tracks and A/B patterns — implemented:** independent lengths, channels and mutes,
  bar-boundary launches, per-track sound selection and full parameter persistence.
  Automated timing/roundtrip tests pass; Jacob listened on October 30, 2026
  ([results](listening-tests.md)); holding a pad while switching tracks remains untested.
- [x] **Sound assets as dependencies — steps 1–2 implemented:** asset store, registered
  folders, the `oscairn.assets` contract and Performance preflight
  ([RFC 0009](rfcs/0009-sound-assets-and-dependencies.md#implementation-status)).
- [x] **CI failing since worker step 2 — fixed:** GitHub Actions failed on every push since "Run
  plugins in worker processes" (October 30, 2026); all three jobs pass again from run
  41152651659 (October 31). The causes:
  - `worker-failures` on the hosted VMs, which cannot keep real time (as in Docker): it now
    runs with `--no-timing` when `CI` is set, as under sanitizers, and then compares the
    healthy instrument's sound only if it met every deadline. Timing stays for local and
    device runs.
  - An aborting worker could be reported as hung, not crashed, when its exit outlasted the
    250 ms hang limit (ThreadSanitizer). The worker's signal handler now marks its shared
    blocks, and the supervisor names the crash.
  - `worker-recovery` found no workers on Linux, which shortens process names to 15
    characters; it now matches the command line.
  - `worker-host` compared output events byte for byte, alignment padding included (a note's
    between key and velocity), which differs between processes on x86_64 Linux; it now
    compares them field by field. Reproduced and confirmed in an x86_64 Ubuntu 24.04
    container.

## Phase 3b: performance instrument (in progress)

Goal: the desktop app as a complete performance instrument before the hardware: parts that
layer, split and share effects, scenes and controls, editing on the device's own screen
without a plugin window or the Routing view, sound engines worth playing, and a plugin that
fails silencing only itself.

Done when (proposed): a Performance with layered and split parts, shared effects, scenes and
a control tree is built, played, edited and undone at 1024 × 600, with every plugin edited
through Oscairn's own editors, while a crashing plugin in it is restarted without a dropout
in the others.

### Next build and test order

1. **Performance UI** ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#progress))
   - [x] step 1: wire ownership, parts, implicit Mixes and port-level routes with channel
     rules, saved in the Performance format (now version 3) and in the control protocol
     ([status](rfcs/0011-performance-ui-and-editors.md#step-1-status))
   - [x] step 2: zones, shared buses, the desktop's Parts and Part views, scenes
   - [x] step 3: splits (lanes with zones and a Mix, joining after) in the model, protocol and
     Part page, including left/right, mid/side and band splits through the bundled Split and
     Join plugins; the Mixer with part solo and meters
     ([status](rfcs/0011-performance-ui-and-editors.md#step-3-status))
   - [x] undo for every edit ([status](rfcs/0011-performance-ui-and-editors.md#undo-status));
     preset and state loads remain
   - [x] step 4: the Routing view on the desktop
     ([status](rfcs/0011-performance-ui-and-editors.md#step-4-status))
   - [ ] the adaptive UI ([RFC 0019](rfcs/0019-adaptive-ui.md#progress)): view models drawn
     for the desktop and the device panel (done), a desktop shell like Gig Performer's with a
     keyboard-and-mouse or touch input mode (started), described plugin editors covering
     step 5's knob pages (started)
   - [ ] plugin windows, then step 6's physical controls
   - [ ] the full scene model: blocks and wires enabled per scene, and what each scene saves
     ([RFC 0011](rfcs/0011-performance-ui-and-editors.md))
2. **Native sample patches** ([sample players and formats](sample-players-and-formats.md),
   [ESP observations](yamaha-esp-samples-research.md)). The Sampler lives in its own
   repository, [oscairn/sampler](https://github.com/oscairn/sampler), since October 31, 2026
   (like the FM and analog engines' forks), fetched at a pinned commit.
   - [x] first slice: one-region SFZ patches over preloaded WAV with shared PCM, held-note
     ownership, desktop import and samples saved as Performance dependencies
     ([RFC 0008 step 2](rfcs/0008-sfz-native-samples-and-conversion.md#step-2-status)),
     including a reproducible liquidsfz cache-sharing probe; Jacob listened on October 30, 2026
   - [ ] multisample, FLAC and streaming for large pianos, reusing sfizz/liquidsfz parsing
     where it separates cleanly. Extend SFZ only for demonstrated gaps; SoundFont remains a
     separate engine candidate.
   - [ ] an engine that can do everything an AWM2 Part can (decided by Jacob, October 30,
     2026; [RFC 0013](rfcs/0013-awm2-class-sample-engine.md), proposed), specified from
     Yamaha's documentation and black-box measurements, with no Yamaha code; Yamaha
     waveforms imported from the user's own installation, never included. Order:
     multisample (RFC 0008 step 3), the measurement harness and AEG tables, elements and XA
     Control, filters and FEG, PEG and LFOs, Drum Parts, then full converter coverage.
   - [ ] the other MODX M engines on the same rules: an 8-operator FM engine starting from
     MSFA ([RFC 0014](rfcs/0014-fm-engine.md)), an analog-modelling engine
     ([RFC 0015](rfcs/0015-analog-modelling-engine.md)) and effects covering every MODX M
     effect type and slot ([RFC 0016](rfcs/0016-effects-suite.md)), all proposed
3. **Plugin worker isolation** ([RFC 0010](rfcs/0010-plugin-worker-isolation.md#progress)):
   plugins in supervised worker processes, so a crash, hang or overrun silences only its own
   node and the plugin recovers. Moved here from Phase 5 on October 30, 2026, ahead of Phase 4
   hardware work and further third-party plugin features.
   - [x] step 1: hand-off measured on macOS
     ([results](rfcs/0010-plugin-worker-isolation.md#step-1-measurements))
   - [x] step 2: one plugin per worker with identical rendering
     ([status](rfcs/0010-plugin-worker-isolation.md#step-2-status))
   - [x] step 3: crashes, hangs and overruns contained to their node, on macOS
     ([status](rfcs/0010-plugin-worker-isolation.md#step-3-status))
   - [x] step 4: a failed plugin restarted with its prepared sounds' state and parameters
     ([status](rfcs/0010-plugin-worker-isolation.md#step-4-status))
   - [x] step 5: native engines (Basic Synth, Sampler, Delay with its effect contexts) in
     workers, with sound assets across the boundary and a placement policy with per-plugin
     overrides ([status](rfcs/0010-plugin-worker-isolation.md#step-5-status))
   - [ ] step 6: memory and CPU limits, sandboxing
   - [ ] step 7: LV2 in workers
   - [ ] measurements on native Linux and the target; workers by default in the app
4. **Sequencers** ([the comparison](sequencer-evaluation.md) of the KICK firmware's sequencer
   with chillbeast's work, zynseq and hosted plugins)
   - [x] the evaluation, recommending the KICK model and workflow with sample-accurate
     playback rewritten in an Oscairn plugin
   - [x] decided October 30, 2026: more than one sequencer type (KICK-style linear sequences
     and launcher-style patterns and scenes), settled on or combined later
   - [ ] swing and further sequencer workflow, following that choice
   - [ ] keep the shared theme and generated-editor contract current as screens are added
5. **Modular instrument** ([plugins/modular](../plugins/modular/README.md))
   - [x] chillbeast's engine with its history, wrapped as a CLAP instrument with factory
     presets and bundled in the desktop app; playable
   - [ ] the patch editor
   - [ ] the upstream sample-grid fix
   - [ ] native sound contexts
6. **Voice budget** ([RFC 0018](rfcs/0018-voice-budget-and-stealing.md#progress)): stealing
   the least audible notes across parts, fading them first, with per-part note limits,
   reserves and priority; later, plugins that opt in steal their own voices.
   - [x] a choke fades a voice over 3 ms in the SDK helper (Basic Synth and Sampler);
     per-note volume; the host's 5 ms volume fade before it chokes a stolen note
   - [x] per-part note limits that steal released notes first and spare the bass and melody
     ([step 2](rfcs/0018-voice-budget-and-stealing.md#step-2-status))
   - [ ] reserves and priority per part
   - [ ] step 3's shared budget
   - [ ] step 4's extension for plugins that steal their own voices
7. **Sounds, Performances and the library**
   ([RFC 0021](rfcs/0021-sounds-performances-and-library.md#progress)) and apps that ask each
   other for them ([RFC 0022](rfcs/0022-apps-and-requests.md#progress))
   - [x] Performance format 3 with an info block (name, description, tags, color)
   - [x] the library of Performances and Sounds, with its cache, favourites and recents;
     Save and Save As New
   - [x] Sounds: a part saved without its seat, loaded into a part as one undoable step or as
     a new part, with controls owned by parts
   - [x] the desktop's Library app, Setlist app (list, scrolling list and grid; per-entry
     colors) and Part menu
   - [x] RFC 0022 step 1: actions between the built-in apps
   - [x] **where a Sound came from:** each part records the Sound and revision it was loaded
     from; **Save** writes that Sound's next revision, **Save As New** makes another, the
     parts list marks a part edited since (`*`), and **Update from library** loads a newer
     revision (Part menu); a Sound deleted from the library breaks no Performance
   - [ ] authors in the info block; preset and state loads marking a part edited
   - [ ] **libraries:** a sound pack, exported library or the factory content, its Sounds and
     samples locked (Save As New makes your own copy); items can be deleted and libraries
     uninstalled, and reinstalling puts back whatever is missing
     ([design](rfcs/0021-sounds-performances-and-library.md#libraries))
   - [ ] **importing:** as a locked library or into your own storage (editable, new IDs), and
     all of it or chosen items; the same choice of parts for any package, such as only its
     plugin ([RFC 0006](rfcs/0006-packages-and-portability.md#choosing-what-to-import))
   - [ ] **exporting a Performance with its Sounds:** parts keep where they came from and the
     export brings those Sounds and their samples; today a folder export drops the link
     ([design](rfcs/0021-sounds-performances-and-library.md#exporting-a-performance))
   - [ ] hiding tags from the tag list, and tag colors
   - [ ] **importing a Performance:** all of another Performance's parts, buses, their wires
     and controls added to the open one, in one step
     ([design](rfcs/0021-sounds-performances-and-library.md#importing-a-performance))
   - [ ] **seamless switching:** loading a Sound into a part keeps the notes already held and
     the outgoing chain's tails, with the incoming chain prepared off the audio thread, and
     says beforehand when it cannot
     ([RFC 0021 step 5](rfcs/0021-sounds-performances-and-library.md#seamless-switching), on
     RFC 0007's chain preparation and retirement); then seamless Performance changes for
     setlists
   - [ ] tag editing and suggestions, plugin presets in the library, `LibraryModel`, and the
     Library on the panel and compact screens (RFC 0021 steps 2–7)
   - [ ] actions over the control API, then choosing between apps (RFC 0022 steps 2–3, also
     in Phase 5)

Also open:

- [ ] swing, additional tracks/patterns and more lanes in the sequencer
- [ ] automatic Sound/effect-chain preparation and retirement, and asset-bearing native
  patches (Setlist and library persistence are done: item 7)
- [ ] stable format/plugin identities and state provenance in persistent library data,
  following [RFC 0006](rfcs/0006-packages-and-portability.md). Installation paths locate
  binaries but must not become portable package identities. Custom plugin pages and
  broader graph editing follow the current generic editor.
- [ ] hardware capabilities, timestamped input, scheduled output and engine backend
  selection, defined before hardware MIDI/CV implementation
  ([RFC 0004](rfcs/0004-hardware-services.md), proposed); the control API remains outside
  this performance-event path
- the KICK's real panel as a control surface:
  - [x] pad mapping: a per-Performance pad map (note, channel, velocity, pressure per pad,
    banks A–H, chromatic, drum and in-key fills) with the desktop's pad editor
    ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#pads))
  - [ ] the panel itself driving Oscairn
- MIDI input curves and filters:
  - [x] note zones (key, velocity and channel ranges, transpose, velocity curve or fixed
    velocity) on part inputs and note wires
    ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#step-1-status)); the Part page edits
    key and velocity ranges and transpose
  - [x] parts choosing which MIDI inputs play them, by kind, port or name, whatever channel
    they send on ([RFC 0012 selectors](rfcs/0012-sync-external-midi-and-multi-device.md#choosing-devices-selectors))
  - [ ] the velocity curve editor
- [ ] shared controller mappings with knob/slider pickup, direct position access,
  relative-encoder tracking and explicit feedback/ownership rules; plugins may opt
  into physical observations, accepted changes or both
  ([planned architecture](architecture.md#physical-controls-takeover-and-direct-position-access-planned))
- [ ] recording as an engine service: any graph input, output, part or bus and the notes
  played, to WAV/RF64 off the audio thread, with retroactive capture
  ([RFC 0020](rfcs/0020-recording.md), proposed)

Plugins and apps (sequencers, instruments, the UI) must stay separate components, each in
its own folder with its own build target and depending only on Oscairn's public headers, so
they can later move to their own repositories or ship as packages. The sound-contexts helper
they shared is now part of the MIT plugin SDK beside the extension headers
(`core/clap-ext/include/oscairn-clap/sound-contexts-plugin.hpp`), so a plugin in its own
repository vendors that folder.

The subsequent [conversion plan](rfcs/0008-sfz-native-samples-and-conversion.md#conversion-and-modx-m-migration)
covers SFZ interchange and Jacob's MODX M library migration: inventory first, explicit
fidelity/loss reports, then tested sound/Performance adapters and model-specific Yamaha
write-back. It is planned work, not part of the first WAV implementation.

Package-manager comparison comes at the package milestone, before permanent distribution
format commitments: evaluate libmamba/Nix and libsolv as a narrower fallback using the
[defined prototype](rfcs/0006-packages-and-portability.md#implementation-candidates-and-decision-timing).
No package-manager dependency or prototype is required for the next desktop step.

## Phase 4: hardware (started)

- [ ] Sample rates: 48 kHz is the default, and 44.1, 96 and 192 kHz must work where users or
  hardware call for them. The engine already runs at the rate the audio device reports, and
  plugins are activated at that rate, but changing the device's rate while running stops
  audio instead of reactivating the graph, and the tests render almost only at 48 kHz.
  Needed: reactivation at a new rate, render tests at each rate, and the device's ALSA
  configuration.
- [ ] External MIDI and CV, clock and Link out, and setlists across several devices
  ([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md)): needed by the Ignite and by
  rigs of several Oscairn devices. Steps 1–2 (MIDI out, clock out) come first.

- [ ] Map the existing logical audio endpoints onto each board's real inputs/outputs and
  validate simultaneous capture/playback, channel identities, isolation, clocking and
  latency. DJ master/booth/cue controls use the existing independent-bus routing; their
  complete workflow and UI follow with the DJ application.

- [x] an Oscairn OS image built with current Buildroot, booting into Oscairn in QEMU's
  generic ARM64 machine (no emulator models the target chips).
- [ ] Hardware services ([RFC 0004](rfcs/0004-hardware-services.md)): first a CPU backend and
  simulated clocks/queues, then a board adapter. Inventory actual peripheral ownership,
  firmware availability and clock domains before assigning work to a real-time core or
  panel MCU. Validate input timestamps, scheduled MIDI/CV/clock output, cancellation,
  overflow handling and alignment with audio under load.
- [ ] Optional engine backends: capability discovery, conditional backend-package resolution
  and whole-performance resource reservations, first with simulated devices and then one
  reference engine on one available DSP, FPGA or real-time core. Separate equivalent
  implementations from explicit hardware-specific sound/quality modes. Install only each
  target's selected backend dependencies; reject unavailable chip services, incompatible
  FPGA regions/firmware and CPU fallbacks that cannot sustain the sound. Compare sound,
  complete processing latency and sustained capacity with its CPU backend. Only use acceleration
  when a compatible implementation and measured benefit exist; keep CPU-only devices
  and offline rendering supported.
- [ ] The KICK (i.MX8M Mini), once beta units exist: an image for its board, and a
  hardware-services adapter for its panel, screen and audio.
- [ ] The Ignite (BE 0716-AAF), which uses the same board family and panel protocol: a 64-track
  MIDI and CV sequencer with a General MIDI sound engine. Its features need several MIDI
  ports (5 DIN pairs, USB device and USB host), CV/Gate with calibration, sync in and out,
  MIDI Machine Control, and sequencers that drive external instruments as well as internal
  engines.
- [ ] A DJ deck, once Behringer publishes its details (it has been teased; nothing is public
  yet): several clocks with decks as the master, high-resolution jog input, a track player
  with key lock, track analysis, DJ mixer outputs and cue, waveform streams, and the three
  ways of working with DJ software on a computer (see
  [architecture](architecture.md#dj-devices)). First Mixxx running on the device as its
  application, then a native Oscairn deck; no certification programmes.
- [ ] USB audio and MIDI in both directions, as a USB device to a computer and as a host for
  interfaces and controllers, on every device whose hardware allows it.
- [ ] An RK3588 board, once the target hardware is known: real-time kernel, audio on isolated
  cores, measured latency.

## Phase 5: the rest of the architecture

Each starts as an RFC:

- [ ] VST3 hosting
- [ ] macOS Audio Units hosting (AUv2/AUv3 scope to decide)
- [ ] Windows desktop, process and IPC support
- [ ] the sampler workflow: slicing, resampling, looper
- [ ] the Live workflow (splits, layers, scenes): parts with splits, layers and scenes exist
  (Phase 3b, item 1); the workflow itself is still to be specified
- [ ] file formats and the library: Performances, Sounds and the library are done (Phase 3b,
  item 7); Projects and Samples in the library, and packages, remain
- [ ] persistent engines
- [ ] engine recovery: plugin recovery in workers is done (Phase 3b, item 3); recovering the
  engine process itself remains
- [ ] signed updates with A/B system images
- [ ] the platform as a DAW plugin

- [ ] **Packages and portability** ([RFC 0006](rfcs/0006-packages-and-portability.md), proposed):
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
- [ ] **Keeping plugins loaded:** a memory budget, unloading the least recently used, setlist
  preparation options and a setlist check; CPU, memory and storage meters.
- [ ] **Native plugins outside Oscairn:** first one engine as a direct CLAP/VST3 plugin on
  supported Windows/macOS/Linux targets and AU on macOS, with shared canonical state/assets
  and tested host/format behavior; evaluate existing wrappers. Then the whole Oscairn graph
  as a DAW plugin and optional individual standalone shells. Neither path requires the
  separate Oscairn app to be running. Test old DAW-session state, runtime versions and
  missing assets; another DAW does not automatically understand our package lock.
- [ ] **Plugin looks:** pages and themes described in data, then sandboxed custom views.
- [ ] **Choosing between apps** ([RFC 0022](rfcs/0022-apps-and-requests.md) step 3): when two
  installed apps offer one action (two setlist apps, two libraries), the musician picks one
  once and may change the default per action in settings; nothing ranks Oscairn's own apps
  first. Today the first app to offer an action answers it. Before this: step 2, actions
  over the control API with app manifests, so apps in other processes can offer them.
- [ ] **From Gig Performer's feature set** (compared October 31, 2026):
  - [ ] **Backing tracks:** an audio file player that follows the transport and setlist, as
    an app or plugin.
  - [ ] **Recorder:** an app on the engine's recording service ([RFC 0020](rfcs/0020-recording.md)).
  - [ ] **OSC** control in and out, alongside MIDI and the control protocol.
  - [ ] **Smooth transitions:** parameter changes on a scene or variation change glide over a
    set time instead of jumping.
  - [ ] **Scripting:** user scripts that react to events and drive the graph and controls,
    sandboxed like plugins; language and API to be decided in an RFC.
- [ ] **Developer kit:** the MIT SDK and templates, the builder image, the `oscairn` tool,
  the validator. (Plugin isolation in sandboxed workers moved up to Phase 3b, item 3.)

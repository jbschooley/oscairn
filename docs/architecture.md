# Architecture

This is where Oscairn is going: the design the code is built towards. Most of it is not
built yet. The [roadmap](roadmap.md) says what exists and in what order the rest comes;
design decisions are recorded as [RFCs](rfcs/README.md).

## What exists today

| Built | Not built yet |
| --- | --- |
| Graph of CLAP plugins: audio and note connections, dependency order, lock-free live editing | LV2 and VST3 hosting |
| Sample-accurate event routing; renders identical at different block sizes (tested in CI) | Control API; the UI as its own process |
| Transport: internal tempo or external MIDI clock | Ableton Link, DAW and CV clock sync |
| Parameter events between plugins (`oscairn.param-target`, [RFC 0002](rfcs/0002-param-target.md)) | Sounds, Performances, Setlists, Projects; file formats and library |
| Step sequencer, basic synth and delay plugins | Scenes, persistent engines, crash recovery |
| Offline renderer; desktop app with live audio, MIDI input and panel layouts | Hardware-services interface; device images; the platform as a DAW plugin |

## Core idea

One realtime host for synthesis engines, samplers, effects and sequencers, running on
Linux on hardware instruments, on the desktop and inside a DAW. Everything a sound needs
(engines, layers, splits, effects, routing, sequencing) is a node in a graph, and a sound
or performance is not tied to one device.

The aims: the immediacy of a hardware instrument, the routing of a live-performance host,
an open plugin ecosystem, and one sound format shared by hardware and desktop software.

## Event sources

Live playing, groovebox sequencing and DAW use are the same graph with different event
sources:

```text
Event sources ─► Event processors ─► Engines ─► Audio graph ─► Outputs
 keyboard/pads    zones, splits,      CLAP/LV2/    effects,      audio, MIDI out,
 MIDI in          arpeggiators,       VST3         buses         CV/Gate, clock out
 sequencers       transpose
 DAW host
         ▲
    Transport / clock (internal, MIDI clock, Link, DAW, CV clock)
```

A keyboard split is an event processor. A pattern sequencer is an event source. In a DAW,
the DAW is the event source, and the platform's own sequencers follow its transport or
switch off. Sequencers drive engines through the plugin API's sample-accurate note and
parameter events, so engines need nothing special to be sequenced.

## Layers

The core provides primitives. Sequencers and workflows are replaceable parts on top, so
users can add their own or port a DAW.

| Layer | What it is | Owns |
| --- | --- | --- |
| 1. Hardware services | Platform | Audio I/O, panel, pads and LEDs, display, MIDI ports, CV/Gate, clock I/O, storage |
| 2. Host core | Platform | Graph, plugin hosting, transport and clock sync, sample-accurate event routing, stable addressing of every node parameter, bus recording, scene and mute snapshots, the project container |
| 3. Event sources and processors | Plugins, official and community | Sequencers, arpeggiators, loopers, splits and zones, chord and generative tools |
| 4. Engines and effects | Plugins, official and community | Synths, samplers (including slicing and pad banks), drum engines, effects |
| 5. UI | First-party, or anything that speaks the control API | Screen UI, control-surface mapping, browsing and editing; a separate process from the audio core |

### Two kinds of thing: plugins and the UI

To avoid an app framework, there are only plugins and the UI:

1. **Anything timing-related is an event plugin**: sequencers, arpeggiators, loopers.
   These are what the community builds most, and they also work in a DAW.
2. **One first-party UI**, with Live (sounds, setlists) and Groovebox as screens inside it.
3. **The UI is a separate process using the control API**, so a UI crash cannot stop
   audio. Anything else that speaks the control API is therefore already an "app".
4. **Plugins can bring their own screens** (CLAP GUIs), shown inside the UI.

|  | Event plugin | UI |
| --- | --- | --- |
| Runs | Inside the audio graph, every block, sample-accurate | Outside the audio path, through the control API |
| Does | Takes transport and events in; emits notes and parameter events | Owns the screen and panel; decides what graph exists; browsing, editing, setlists |
| Data | Its node state (for example the pattern's steps) | Organisation: setlists, layouts, preferences, kept as library files |
| In a DAW | Works as a note-effect plugin | Becomes the editor window of the platform plugin |

Rule of thumb: if it has to happen at an exact sample, it is a plugin; if it is about what
you see, press or organise, it is the UI.

"Plugins that play other plugins" means event plugins upstream in the graph, not
container plugins that host plugins inside themselves. Containers would hide nodes from
routing, crash recovery and the licence boundary. A rack-like grouping, if wanted, is a
sub-graph node that the core still manages.

### Custom sequencers, workflows and DAW ports

- **A custom sequencer** is a CLAP note-effect plugin: it reads the host transport,
  outputs notes, and outputs parameter events for other nodes through `oscairn.param-target`.
  Because it is a normal plugin, it also runs in a desktop DAW, without the cross-plugin
  parameter feature.
- **A custom workflow** is a program that talks to the core through the control API
  (graph edits, parameters, transport, scenes, project load and save, panel and display
  events). The first-party UI uses the same API.
- **A ported DAW** either uses the core as its engine through the control API, keeping its
  own UI, or runs as a standalone app that owns the audio device through the hardware
  services layer, with an explicit mode switch.

### Parameter events between plugins

One extension beyond standard CLAP is needed: a sequencer's per-step parameter locks and
automation lanes must change parameters of *other* plugins. A plugin sends values on
numbered lanes without knowing what they control; the graph maps each (plugin, lane) to a
(target plugin, parameter) and delivers standard CLAP parameter events to the target in the
same block, at the exact sample, before notes at that sample. See
[RFC 0002](rfcs/0002-param-target.md). Built.

## Sounds, performances and projects

**Patch** (engine state) → **Sound** (event chain + engine + insert effects) →
**Performance** (Sounds, zones, routing, scenes) → **Setlist**. On the groovebox side:
**Project** → patterns and song, with Sounds as track instruments.

```text
Performance
 ├─ Zone C1–B2 ──► Sound "Bass"       : FM synth → compressor
 └─ Zone C3–C8 ─┬► Sound "Pad"        : analog synth → chorus → reverb
                └► Sound "Arp Pluck"  : arpeggiator (1/16, latch) → wavetable synth → delay
```

- Only the layer with the arpeggiator is arpeggiated; other layers on the same keys get
  the notes directly. The arpeggiator's settings are saved with the Sound, so it is
  reusable across performances.
- Zones support key and velocity ranges, overlapping layers, transpose, MIDI channels,
  controller filtering and mapping, sustain behaviour, MPE and polyphonic aftertouch, and
  external MIDI destinations.
- There is no fixed number of parts: add engines, layers and effects while the hardware
  can run them reliably, with headroom kept for live playing and transitions.

Where groovebox objects live:

| Object | Layer | Notes |
| --- | --- | --- |
| Transport, tempo, swing grid, clock in and out | Host core | One clock for everything; sequencers read it |
| Tracks, patterns, steps, probability, ratchets, micro-timing | Sequencer plugin | In the plugin's state, in a published schema |
| Parameter locks and automation lanes | Sequencer plugin, routed by the host | `oscairn.param-target` |
| Motion recording | Sequencer plugin, fed by the host | The host forwards parameter changes to the armed recorder |
| Pattern chaining, song mode, fills | Sequencer plugin | Workflows may present them differently |
| Scenes and mute groups | Host core | Snapshots of node bypass, mute and parameter values |
| Slicing, pad banks, velocity layers | Sampler plugin | |
| Resampling | Host core and the UI | The core records any bus; the UI decides where the result goes |
| Project file | Host core | The graph, every node's state, and UI metadata |

### Scenes and seamless switching

A scene is a state within a Performance for a section of a song. Scene and sound changes
should keep held notes and effect tails rather than rebuild the graph and cut audio.
Parameter and routing changes differ from transitions that load new engines, plugin state
or samples: seamless transitions may need the outgoing and incoming sounds prepared and
running together for a moment, with note-offs and sustain still reaching the outgoing
voices. The host reports when a transition cannot be seamless.

## Plugin formats

- **CLAP** is the preferred format for new engines, effects and sequencers: a clean C ABI,
  a good realtime and threading model, per-note expression, MPE, and extensibility.
- **LV2** is first-class, for the large existing Linux ecosystem.
- **VST3** is hosted for compatibility with existing plugins (the SDK is MIT-licensed
  since 3.8).

The graph is format-neutral: a CLAP synth, an LV2 reverb and a VST3 effect are all just
nodes. Format support does not mean binary compatibility: an ARM64 Linux instrument needs
ARM64 Linux builds of its plugins.

Plugins loaded through a plugin API are separate works and may use any licence
([RFC 0001](rfcs/0001-licence.md)).

## Live reliability

- Check dependencies and resources before activating a Performance, and keep headroom for
  peak load and transitions.
- Predictable voice stealing and overload behaviour, reported clearly; no silent removal of
  layers.
- Scan plugins outside the audio process, so a failed scan cannot interrupt playing.
  (The host has an out-of-process scanner tool; the graph still scans in process.)
- Isolate plugin failures from the output core and unaffected engines; keep the UI and
  background services out of the audio process.
- Recoverable software updates, with rollback and saved work preserved.

### Persistent engines and fast sound switching

The host manages engine code, shared read-only assets (samples, tables), patch
definitions and running sound contexts (voices, envelopes, effect history) separately.
Six organ patches are six patch definitions plus enough running contexts for simultaneous
layers and transitions, not six independently initialised organs.

- Platform engines may implement optional, narrow capabilities: prepare patches in the
  background, share read-only data, activate a prepared context at a scheduled audio
  boundary, keep outgoing voices separately, report when a context is silent.
- Ordinary CLAP, LV2 and VST3 plugins are supported through persistent instances and a
  pool of prepared instances: new notes go to the prepared instance while the old one
  finishes its notes and tails.
- Changing setlists keeps resources common to both, prepares missing ones in the
  background and releases the rest when safe.

### Engine recovery

A recovery record kept outside each engine worker (engine identity and version, last
committed state, controller values, routing) lets a supervisor restart a crashed or hung
worker outside the audio path, restore its state, reconnect it at an audio boundary and
fade it in. The output core never waits for a failed worker; it mutes or bypasses the
affected node. Retries are bounded. Restarting may lose held voices and tails: the promise
is that the sound comes back automatically, not that recovery is inaudible.

## Latency

Latency is measured from a key press or MIDI input to analog output, including key
scanning, scheduling, synthesis, buffering and converter delay. Development starts at
48 kHz with 64-sample blocks, then the smallest block size that holds up under worst-case
load is chosen from measurements. Parallel paths are latency-compensated, and paths over
the live-playing budget are reported.

## CPU and FPGA

Every product runs the platform on its main CPU. Larger hardware may add an FPGA for
workloads where it measurably helps (analog modelling with heavy oversampling, large
polyphony, tonewheel generation).

- Every official portable engine has a CPU implementation.
- An accelerated engine is still a plugin, with a CPU backend and an FPGA backend reached
  through the platform's hardware services. The FPGA is a DSP backend, not a plugin format.
- Backends share a reference sound, with cross-backend tests and defined tolerances.
  Quality modes are explicit and saved with the patch; nothing changes the sound silently.

## Portable sounds

A patch describes what the instrument is, not how a particular board implements it:
engine ID and version, complete plugin state, parameters, modulation, macros, sample
references and metadata. A Performance adds zones, routing, effects, controller mappings,
scenes and a dependency manifest. Nothing in either is hardware-specific.

The goal is moving sounds between hardware, the desktop app and the DAW plugin without
rebuilding them. Smaller hardware may reduce polyphony under an explicit policy or refuse a
Performance it cannot run, and says so before activating it.

- Stable engine, parameter and macro IDs; versioned schemas with migration rules and tests.
- Complete plugin state saved alongside the graph.
- A dependency manifest of plugin identities, versions and required features.
- Missing dependencies keep their saved state, so installing them restores the Performance.

### Controls

Sounds store semantic controls ("Macro 1 = Brightness"), not physical ones ("Knob 4 =
cutoff"), and each device maps them to what it has: a dedicated knob, a touchscreen macro,
an encoder, or a DAW automation parameter. Plugins without macros get a host-generated
parameter editor with search, groups and saved assignments.

The desktop app's panel layouts already work this way for the screen and controls: one
engine behind any layout ([layouts](../hosts/standalone/layouts/README.md)).

## User data

People who just want to play never think about files; advanced users get a real file
system.

- **Library (default):** browse Sounds, Performances, Setlists, Projects and Samples by
  category, tag and search. Saving just saves. SD cards and USB drives appear as sources.
- **Files (opt in):** the real folder tree of internal storage, SD and USB.
- **Files first, index second.** Everything a user owns is a plain file or folder in a
  documented format. The core keeps an index (stable IDs, revisions, dependencies, content
  hashes), rebuilt by scanning, so files added from a computer appear in the library.
  Moved samples are found again by hash.
- **One bundle format, three scopes:** a selection (one item and its dependencies), a user
  export, and a full backup. Import merges by ID and reports missing dependencies.
- **Sync** between devices, the desktop app and the DAW plugin uses the same mechanism,
  moving only changed files. Plugins are per-platform binaries, so sync reports what is
  missing rather than copying executables.

## The platform as a DAW plugin

Oscairn itself ships as a CLAP, VST3 and AU instrument: the core, its hosted plugins, and
the UI as the editor window. The DAW supplies audio, is the transport, and stores the
platform's state in its project. The standalone build of that plugin is also the desktop
simulator.

Constraints adopted now because they are hard to retrofit:

- **The core is a library.** It never assumes it owns the audio device, its threads or the
  process, and processes whatever block size it is given. (Built this way.)
- **Hardware services are an interface**, with implementations for devices, the desktop,
  the DAW plugin and the simulator.
- **The UI renders into a window** that can be a plugin editor.
- **Offline rendering is deterministic.** (Built; CI checks it.)

## Host architecture

```text
UI / Performance / Setlist manager            (separate process, control API)
          │
          ▼
Engine resource manager
├── dependency and asset cache
├── prepared sound contexts, instance pools
└── worker supervisor, recovery state
          │ prepared resources and graph changes
          ▼
Realtime graph
├── format adapters: CLAP / LV2 / VST3
└── engine, effect and event plugin nodes
    ├── CPU backend  → CPU DSP
    └── FPGA backend → hardware services → FPGA DSP
          │
          ▼
Routing, buses, latency compensation
          │
          ▼
Audio I/O (hardware services)
```

Resource preparation and recovery run outside the realtime path; the graph processes
ready contexts and applies prepared changes at block boundaries.

## Engines

Oscairn should come with a few strong engines and leave room for many more:

- an analog-modelling engine that scales from CPU-only devices to FPGA-assisted ones;
- an FM engine, starting from an existing open engine where licences allow (such as
  Dexed / MSFA);
- a tonewheel organ with drawbars, percussion, vibrato and chorus, and a rotary speaker;
- a multisample engine (SFZ, streaming from storage; sfizz is a candidate);
- drum and percussion engines;
- and community engines of any kind.

## Community SDK

Make it straightforward to add engines, samplers, effects, sequencers, controller layouts,
sample libraries and presets: CLAP guidance, LV2 and VST3 compatibility notes, a desktop
test host, reference plugins, capability and realtime-safety tests, packaging, and CI
templates. Platform-specific extensions only where the standards genuinely cannot express
something.

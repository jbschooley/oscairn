# Architecture

This is where Oscairn is going: the design the code is built towards. Most of it is not
built yet. The [roadmap](roadmap.md) says what exists and in what order the rest comes;
design decisions are recorded as [RFCs](rfcs/README.md).

## What exists today

| Built | Not built yet |
| --- | --- |
| Graph of CLAP plugins: audio and note connections, dependency order, lock-free live editing | LV2 and VST3 hosting |
| Sample-accurate event routing; renders identical at different block sizes (tested in CI) | Automatic helper launch/supervision |
| JUCE-free session and typed local/remote Control interface, including host/device status | Gesture ownership/undo |
| JSON-RPC graph editing, parameter coalescing/gestures, notifications and bounded Unix sockets; explicit engine/UI separation | Live GUI crash/reopen verification and helper mode as desktop default |
| Transport: internal tempo or external MIDI clock | Ableton Link, DAW and CV clock sync |
| Parameter events between plugins (`oscairn.param-target`, [RFC 0002](rfcs/0002-param-target.md)) | Sounds, Performances, Setlists, Projects; file formats and library |
| Step sequencer, basic synth and delay plugins | Scenes, persistent engines, crash recovery |
| Offline renderer; desktop app with live audio, MIDI input and panel layouts | Hardware-services interface; the platform as a DAW plugin |
| A Linux image (Buildroot) that boots into Oscairn, tested in QEMU | Images for real hardware |
| | Packages: sound packs, libraries, installable plugins |

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
| 1. Hardware services | Platform | Audio I/O (built-in, and USB audio in both directions), panel, pads, jog wheels and LEDs, display, MIDI ports (several DIN ports, USB device and host), CV/Gate with calibration, clock and sync I/O, storage, network |
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

### Keeping plugins loaded

Plugins stay loaded once used, so going back to a sound is instant. When loading a sound
would go over the memory budget, the plugins used longest ago are unloaded first.

- **A budget, not swap.** The budget is the device's RAM minus a reserve for the system and
  the UI. Audio never depends on swap.
- **Idle plugins cost no CPU.** A loaded plugin that is not in the playing graph is not
  processed; it only holds memory.
- **Never unload what is in use:** anything in the playing Performance, still sounding (held
  notes, tails), pinned, or prepared for the current setlist. Setlist items far from the
  current position go before nearby ones.
- **Know the cost before loading.** Each plugin's memory is measured once it is loaded
  (exactly, when plugins run in their own worker processes, which also makes unloading
  return all of it) and remembered per plugin and Sound, so later loads are predicted.
  Samples' sizes are known from their files, and streaming samplers can keep only the
  starts of samples in memory. A sound whose plugin has never been loaded is estimated
  with headroom.
- **Say so instead of failing.** A Sound or Performance that cannot fit even after
  unloading everything else is refused with the reason, rather than starting and running
  out of memory.
- **Setlists choose how much to prepare:** everything (when it fits), the next few songs,
  or pinned items only. A setlist check before a performance reports what fits, what will
  load on the way, and how long each change will take.

### Meters

CPU, memory and storage are visible on the device and through the control API:

- **CPU:** time each plugin and the whole graph take per audio block, against the block's
  deadline; deadline misses (dropouts), per plugin and in total.
- **Memory:** per plugin, Sound, Performance and pack; total against the budget; what is
  loaded but idle and could be unloaded.
- **Storage:** used and free on internal storage, SD and USB, per pack.

### Engine recovery

A recovery record kept outside each engine worker (engine identity and version, last
committed state, controller values, routing) lets a supervisor restart a crashed or hung
worker outside the audio path, restore its state, reconnect it at an audio boundary and
fade it in. The output core never waits for a failed worker; it mutes or bypasses the
affected node. Retries are bounded. Restarting may lose held voices and tails: the promise
is that the sound comes back automatically, not that recovery is inaudible.

### USB audio and MIDI, both ways

Every device can be both a USB device and a USB host, where its hardware allows (the
i.MX8M Mini and the RK3588 both have the USB controllers for it):

- **As a USB device**, plugged into a computer: a class-compliant audio interface (several
  channels each way) and MIDI device, so a computer can play through it, record it, or
  control it, with no driver needed. Linux provides this through USB gadget functions.
- **As a USB host:** class-compliant audio interfaces, MIDI controllers and keyboards
  plugged into it.
- **Clocks:** a USB link runs on its own clock, so the audio crossing it is resampled
  continuously and adaptively to stay in step with the device's audio clock, without drift
  or clicks; the device reports to a computer as the clock master where it can.

### External controllers and hardware

Other hardware plugged into an Oscairn device works with it, on any device:

- **Controllers over USB:** MIDI and HID controllers (a keyboard, a Launchpad, a DJ
  controller, a CDJ in its controller mode) control the device through **controller
  mappings**, shipped as packages. Oscairn reads Mixxx's mapping format (XML with
  JavaScript), so its library of community mappings works too; scripts run in a small
  sandboxed JavaScript engine (for example QuickJS, MIT) in the controller service, outside
  the audio thread. Mappings can light LEDs and drive controllers' displays. Jog wheels
  and other continuous controls keep to the direct event path's latency (about a
  millisecond added at most).
- **Audio from other hardware** comes in as channels in the graph on devices with inputs.
- **Sync:** MIDI clock, Ableton Link, and on DJ devices Pro DJ Link (below).

### Touch

The KICK and the Ignite have touchscreens, and most devices will. UIs handle several
fingers at once (two pads, or a pad and a fader); JUCE 9 provides multi-touch on Linux
through XInput2, and any later device UI toolkit must too.

## DJ devices

A DJ deck or all-in-one DJ system needs no synth engines, but the same core runs it:
decks, the mixer and DJ effects are plugins in the graph; jog wheels, platters and the
mixer section come through hardware services; the UI is a layout and a UI process like any
other; music on USB sticks is another library source, with cue points and beat grids as
metadata. A device profile without synth engines is just a smaller set of built-in
plugins. What it needs beyond the rest of this document:

- **Several clocks.** Each deck has its own tempo and beat position, and one deck (or the
  master tempo) is the master the others sync to. The transport can follow a node, so the
  master deck drives it the way an external MIDI clock does; tempo-synced effects, Ableton
  Link and MIDI clock out then follow the master.
- **Fast, fine-grained jog input.** Scratching needs platter movement and touch at the deck
  plugin within a few milliseconds, placed at its sample, at higher rate and resolution
  than MIDI. Jog events take the same direct path into the graph as played MIDI (never the
  control API), as an Oscairn event type.
- **Playing long audio files:** a player plugin that streams tracks from storage with
  read-ahead and seeks instantly; decoders for WAV, AIFF, FLAC, MP3 and AAC;
  time-stretching for key lock (for example Signalsmith Stretch, MIT, or Rubber Band,
  GPL-2.0-or-later); file access through the host's asset extension, as for samplers.
- **Track analysis:** BPM, beat grid, key and waveform overviews, worked out in the
  background and kept in the library index.
- **A DJ mixer's outputs:** main, booth and headphone cue (pre-fader listening, and split
  cue), so the graph needs several outputs and cue buses.
- **Waveforms on screen:** scrolling waveforms for several decks at 60 frames a second, which
  need the control API's shared-memory streams.
- **Mix recording**, through the core's bus recording.

### Working with DJ software on a computer

DJ software divides the work with hardware in one of three ways, and a deck should offer
all of them:

| Mode | The computer does | The deck does |
| --- | --- | --- |
| Controller with an audio interface | Playback, time-stretching, mixing, EQ, effects, waveforms | Sends jog wheels, faders and buttons (USB MIDI, or HID where high-resolution jog wheels need it); plays the computer's audio on its main and headphone outputs; records its own inputs to the computer |
| External mixer | Playback and per-deck effects, each deck sent as its own stereo channel | Mixing, EQ, filters, master effects and cueing, in its own graph |
| Standalone | Nothing (it may prepare the library) | Everything |

- **Mappings for DJ software that accepts them:** Mixxx (open source) first, then Traktor and
  VirtualDJ, which accept mappings for any controller, jog wheels included. Mappings are
  files, shipped as packages.
- **No certification programmes.** Oscairn devices are open: they do not join vendor
  certification or hardware-unlock schemes, and carry no authentication for them. Serato
  and rekordbox unlock features for hardware their vendors certify. With a paid licence
  both map other controllers' buttons, faders and pads, but Serato does not map other
  controllers' platters, and rekordbox's mapping did not cover jog wheels when last
  documented, so on an Oscairn deck they work only partly. That is their choice.
- **USB sticks DJs already carry:** reading the libraries exported by rekordbox, Serato and
  Engine DJ (cue points, beat grids, playlists) would make a deck useful at once. These
  formats are not officially documented; rekordbox's has been worked out publicly.
- **Sync with other players** on a network (Pro DJ Link, also worked out publicly, for example
  by Deep Symmetry's beat-link): following or leading CDJs and mixers' tempo and beat, and
  their track information. Needs the network permission.
- **Timecode control:** a CDJ or turntable plays a timecode track, and the deck follows its
  platter by reading that audio from an input, as digital vinyl systems do (Mixxx does this
  with the xwax library, GPL).

### Mixxx

[Mixxx](https://mixxx.org) is the open DJ software (GPL-2.0-or-later, so its code can be used
in Oscairn with its notices kept). It is a whole application, not a plugin: its own audio
engine, mixer, device handling, Qt interface, library and controller scripting. Two routes,
in this order:

1. **Mixxx as the deck's application**, for a working deck early: the Oscairn image runs
   Mixxx as a standalone application that owns the audio device (the "ported DAW" case),
   and Oscairn provides the system, updates, USB audio both ways, and a panel driver that
   presents the deck's jog wheels, faders, buttons and LEDs to Mixxx as a controller, with a
   Mixxx mapping. Needs a touchscreen interface for Mixxx, and its performance on the
   device's chip measured.
2. **A native Oscairn deck:** the player, mixer and DJ effects as plugins in Oscairn's graph,
   borrowing from Mixxx where it helps (beat and key analysis, effects, importing Mixxx
   libraries, cues and crates, its controller-mapping format), so DJ effects are shared with
   instruments and the DAW plugin, and plugins are isolated and packaged like any other.

Improvements made for the first route (a touchscreen interface, ARM performance, the deck's
mapping) go back to the Mixxx project.

## Latency

Latency is measured from a key press or MIDI input to analog output, including key
scanning, scheduling, synthesis, buffering and converter delay. Development starts at
48 kHz with 64-sample blocks, then the smallest block size that holds up under worst-case
load is chosen from measurements. Parallel paths are latency-compensated, and paths over
the live-playing budget are reported.

## CPU, real-time cores, DSPs and FPGAs

Devices expose capabilities through hardware services: some have only an application CPU;
others also have a real-time core, a panel MCU, dedicated DSPs or an FPGA. Engines adapt by
selecting an explicitly supported implementation, while Sounds keep their musical state.
The design is specified in [RFC 0004](rfcs/0004-hardware-services.md); it is not implemented yet.

- **The application CPU runs the host.** Every official portable engine has a CPU
  implementation, usable on a CPU-only device, on the desktop and in a DAW. The device
  must still have enough resources for the requested sound and quality.
- **Timing services and audio acceleration are separate capabilities.** A real-time core
  or panel MCU can timestamp inputs and deliver scheduled MIDI, clock and CV/Gate outputs
  when its firmware and peripheral connections support them. A DSP, FPGA or real-time
  core can also execute specific audio algorithms, but only with a compatible engine
  backend. A chip name alone establishes neither capability.
- **Plugins own algorithms; the host owns shared hardware.** A plugin declares its CPU
  and optional hardware backends, their requirements and compatible state/quality modes.
  The resource manager selects and reserves a backend before activation; hardware services
  mediate device access. Ordinary plugins continue to run on the CPU. Oscairn does not
  automatically translate arbitrary plugin code to a DSP or FPGA.
- **Selection preserves the sound.** Compatible backends share engine and parameter IDs,
  portable state and a tested reference sound, with defined numerical tolerances. Quality
  modes are explicit and saved with the patch. Resource limits, added latency and any
  required reduction in polyphony or quality are reported before activation.
- **Execution is prepared and bounded.** Firmware loading, memory allocation and backend
  changes happen outside the audio callback. Hardware processing includes transfer and
  buffering latency in its deadline budget and latency report. Live backend switching or
  recovery needs prepared resources and an explicit transition policy; seamless fallback
  is not assumed.

### Hardware event timing

Sequencers remain plugins that decide what plays. Hardware services determine when events
reach physical outputs. Inputs retain capture timestamps; outputs carry deadlines mapped
between the audio sample timeline and device clocks. A capable real-time core executes
queued events at those deadlines, reducing dependence on Linux wake-up timing.

This requires clock synchronisation, bounded queues, an explicit scheduling lead time,
and cancellation on stop, seek or edits. Events emitted by a plugin for the current audio
block are not automatically available in advance: the host must budget output lead time
and align internal audio with external outputs. Live input latency is measured separately.
Missed deadlines and queue overflows are reported, with defined note-off/gate-reset and
disconnect behaviour. CPU-only backends implement the same event contract and report their
measured timing limits. Played events never pass through the UI control API.

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

Plugins can look like themselves without bringing GUI code to a device, in levels:
generated pages from the parameter list, styled like the rest of the UI; pages and a theme
described in data (layout, colours, fonts, images, custom knob graphics), drawn natively by
the UI on any device and mapped to hardware controls; later, custom views (an envelope
editor, a waveform) as sandboxed code running in the UI process, not the engine; and a
plugin's own native GUI on the desktop and in a DAW. First-party plugins use the same
system as third parties. The UI keeps its system areas (header, navigation, transport)
consistent; a plugin owns its page.

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
  moving only changed files. Plugins are per-platform binaries: sync copies a plugin only
  if its package has a build for the other side, and otherwise reports it missing. A
  computer syncs with a device only once paired with it, for example by confirming a code
  shown on the device's screen.

## Packages: sound packs, libraries and plugins

Like the libraries on a MODX or Montage, people can share and reuse sound packs; unlike
them, without fixed slots, and without DRM. Third-party plugins (engines, effects,
sequencers), layouts and themes install the same way.

- **Everything installable is a package:** a manifest (ID, version, author, licence,
  dependencies, the platforms its plugins are built for) and its files. Kinds: content
  (Sounds, Performances, Projects, samples, presets), code (CLAP, LV2 or VST3 plugins, with
  builds for ARM64 Linux, x86_64 Linux, macOS and Windows as available), and looks (panel
  layouts, themes, plugin pages).
- **Stored once.** Installed package files live in a content-addressed store, by hash, the
  way git stores objects. A pack that bundles a plugin or samples already installed costs
  no extra storage: its manifest points at files already in the store. A user's own work
  stays plain files in readable folders; installed packages appear in the library, and as
  read-only folders in the Files view.
- **Loaded once.** The engine loads assets by content hash and plugins by ID and version,
  so two libraries using the same sample or engine share it in memory (see persistent
  engines).
- **Libraries are read-only.** An installed pack appears in the library next to the user's
  own work. Sounds refer to its items by ID and version, so editing never changes the pack;
  saving makes the user's own copy. Updates install side by side, and saved work keeps the
  version it was made with until moved on deliberately.
- **No copy protection.** The platform has no licence keys, no activation, no activation
  servers and no licensing API, and the default catalog does not accept packages that need
  them: a plugin must work with nothing but its files. Copy protection and licensing are
  strongly discouraged, and the library and catalogs point users to open alternatives.
  Oscairn cannot stop a vendor shipping a plugin with its own licensing, but it does not
  support it, and such a plugin is confined to its sandbox like any other, so it cannot
  affect anything else. Packages
  can be signed, which shows who published a package and that it has not been altered:
  authenticity, not copy protection. Licence terms ("do not redistribute") are stated in
  the manifest, not enforced. Paid packs are just files.
- **Network is a permission.** Plugins have no network access unless granted. A package
  that needs it (a plugin that streams audio, talks to other software or hardware over the
  network, or browses an online sample library) declares it and why, and the user grants it
  for that plugin, as phone apps ask for permissions. Services that many plugins would want
  are provided by the host instead, so most plugins never need the network themselves:
  tempo sync (Ableton Link), network MIDI, OSC.
- **Works offline.** Many devices will have no internet, or no network at all, so
  everything works without one: installing from USB, SD or a computer, signature checks
  (against keys on the device), the library, and sync over USB. Online catalogs are
  optional.
- **Where packages come from:** files from USB, SD or a computer, sync with a paired
  computer, and online catalogs: indexes of packages that anyone can host, with a default
  community catalog.

### Engines with sample libraries

A sample-playback engine (a multisample and PCM engine, like AWM2 on a MODX) comes with a
large factory library, and users add their own samples. These are separate packages, so
neither has to move with the other:

- **The engine is one package, its factory samples another.** The engine package depends on
  its factory library. Updating the engine does not download gigabytes of samples again,
  and a library can serve several versions of the engine.
- **Sample packs are more packages:** third-party libraries made for the engine depend on it
  and install alongside the factory one.
- **User samples are the user's own files:** WAVs in the Samples folder, imported from USB,
  SD or a computer, or recorded on the device. Multisamples a user builds (zones across
  keys and velocities) are user content too, and can use the user's samples, factory
  samples or both.
- **An open format for multisamples.** Sample maps are stored as SFZ, an open format other
  sample players read, with the engine's own settings saved beside them; sample packs are
  then useful beyond Oscairn.
- **Samples are referenced, not copied.** A Sound records the samples it uses by content
  hash. Exporting it includes the user samples it uses and refers to factory and pack
  samples by package, so the export is small and complete.
- **The host gives plugins their samples.** Plugins run sandboxed, so a sampler does not read
  arbitrary files: it asks the host for an asset by ID and gets it read-only, memory-mapped
  from the store. The host shares one copy between every instance and engine that uses
  it, can stream large samples from storage, and knows exactly which samples each Sound
  needs (for export, sync, and the memory budget). This is an Oscairn extension, with the
  SDK.
- **Old versions stay while used.** A library update that changes a sample keeps the old one
  in the store while any saved Sound still refers to it; unused files are removed later.

### Built-in plugins and other manufacturers' defaults

Different devices will ship different built-in plugins: Oscairn's own, and engines a
manufacturer adds. Packs should work across them without carrying copies of everyone's
defaults.

- **A baseline every device has.** A versioned platform profile ("Oscairn Core 1") lists
  plugins every Oscairn device includes. A pack that uses only those works everywhere.
- **Manufacturers' built-ins are ordinary packages**, pre-installed in the read-only system
  area of their devices. Installed on another device, where a build for it exists and its
  licence allows, they work the same way.
- **Packs refer to plugins; they do not embed built-ins.** A pack's manifest lists the
  plugins it needs by ID and version range, and the profile it expects. It may include
  whole packages of third-party plugins it needs, which the store deduplicates; it never
  includes a device's built-in plugins.
- **Missing plugins are reported, not guessed.** Opening a Sound whose plugin is missing
  says which package provides it and where it might come from, and keeps the Sound's saved
  state so it plays once the plugin is installed (see portable sounds). A manufacturer can
  declare one of its plugins a compatible replacement for another, with a tested mapping of
  parameters and state; nothing is substituted silently.
- **Installing code needs care.** A plugin is native code running in the engine. Installs
  show who signed the package and what it contains and ask before installing code; plugins
  will run in separate worker processes without network access by default (see engine
  recovery).

## Building and sharing plugins

Developers should be able to build their own engines, effects and sequencers, install them,
share them, and not be able to break each other or the host.

- **SDK (MIT):** Oscairn's CLAP extension headers (such as `oscairn.param-target`), templates
  for an instrument, an effect and a sequencer, and documentation. MIT so that plugins
  under any licence can use them.
- **Builder:** a container image with the same cross-compilers as the OS images, and a CI
  template, so anyone can build for every device from any computer. Builds made with it are
  reproducible: anyone can rebuild a package from its source and check it matches.
- **The `oscairn` tool:** create a plugin from a template, build, validate, package, sign
  (offline), and install to the desktop or to a device over USB or a paired connection. A
  development mode loads a plugin straight from its build folder.
- **Validation:** conformance and realtime-safety tests (no allocation or locking on the
  audio thread, meeting deadlines, state that saves and loads) before a plugin is installed
  or published.
- **Isolation:** each plugin has its own data folder; versions install side by side; plugins
  run in sandboxed worker processes with no network unless granted, memory and CPU limits,
  and a deadline watchdog, so a plugin that crashes or hangs is recovered on its own and cannot take down
  the host or other plugins (see engine recovery).
- **Distribution:** a package carries its builds for each platform, its licence, and where
  its exact source is (an archive in the package, or a repository and commit). Catalogs
  list packages from anyone; the source travels with the binaries.

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
    ├── CPU backend      → CPU DSP
    └── hardware backend → hardware services → DSP / FPGA / real-time core
          │
          ▼
Routing, buses, latency compensation
          │
          ▼
Audio I/O and scheduled MIDI / CV / clock I/O (hardware services)
```

Resource preparation, capability discovery, hardware reservations and recovery run outside
the realtime path; the graph processes ready contexts and applies prepared changes at block
boundaries. Timestamped hardware input feeds the graph directly. Devices without auxiliary
processors use CPU implementations of the same interfaces.

## Engines

Oscairn should come with a few strong engines and leave room for many more:

- an analog-modelling engine that scales from CPU-only devices to supported DSP- or
  FPGA-assisted ones;
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

# Architecture

This is where Oscairn is going: the design the code is built towards. Most of it is not
built yet. The [roadmap](roadmap.md) says what exists and in what order the rest comes;
design decisions are recorded as [RFCs](rfcs/README.md).

## What exists today

| Built | Not built yet |
| --- | --- |
| Graph of CLAP and optional LV2 plugins: audio/note connections, dependency order, lock-free live editing; LV2 Worker, state/presets and scalar patch properties | Remaining LV2 extensions (UI, CV), VST3 hosting |
| Sample-accurate event routing; renders identical at different block sizes (tested in CI) | Persistent session recovery |
| JUCE-free session and typed local/remote Control interface, including host/device status | Gesture ownership/undo |
| JSON-RPC graph editing, parameter coalescing/gestures, notifications and bounded Unix sockets; default engine/UI separation with startup, reattachment and explicit recovery | UI engine shutdown and session persistence |
| Transport: internal tempo or external MIDI clock | Ableton Link, DAW and CV clock sync |
| Parameter events between plugins (`oscairn.param-target`, [RFC 0002](rfcs/0002-param-target.md)) | Sounds, Performances, Setlists, Projects; file formats and library |
| Step sequencer, basic synth and delay plugins | Scenes, persistent engines, crash recovery |
| Basic Synth prepared sound contexts, channel parts and held-note ownership; typed/JSON/remote controls ([RFC 0005](rfcs/0005-sound-contexts.md)); prepared-instance pools for ordinary CLAP/LV2 instruments; desktop picker for the loaded instrument | Native effect-context transitions, asset-bearing native patches, persistent pools and general graph-editing UI |
| Isolated CLAP/LV2 browser, generic parameter/preset editor and atomic slot replacement; installable shared themes | Custom plugin pages, arbitrary graph editor and managed package installation |
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
| 5. UI and controller services | First-party, or anything that speaks the control API | Screen UI, control-surface mapping, browsing and editing; outside the audio thread. Controller services can run without the screen UI. |

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

### Live note routes

Each note connection has a bounded ownership ledger, shared by successive render plans.
The graph assigns distinct destination note IDs and remaps releases, expression and
explicit per-note parameter events. Removing a route or its source releases its voices
when the new plan becomes active. Native CLAP destinations receive scoped chokes; their
`NOTE_END` events reclaim ownership slots. CLAP wildcard addressing stays intact; only
raw MIDI repeated presses use FIFO release. Pedal values pass through unchanged for plugin
sustain/half-pedal handling. On route retirement, CC64 returns to the highest value remembered
by the remaining routes on that port/channel (zero if none). Pedal control is channel-wide;
individual native voices can still be choked by their route-owned IDs.

There are 512 tracked voices per connection and 512 merged destination events per block.
Exhaustion discards the burst and panics that destination, including sustain. The desktop
also recovers from physical MIDI FIFO and callback-buffer overflow and reports
`midiEventDrops`. A panic can cut other sources sharing that destination. Destination IDs
are not reused during a graph's lifetime; exhausting the signed 31-bit ID range requires
recreating the graph. MIDI-only destinations have no per-voice IDs: overlapping sources
on the same channel/key and pedal cannot be isolated completely; use separate channels
or instances where that distinction matters. Retiring a MIDI-only route sends All Sound Off
on its owned channels, which can cut other routes sharing those channels.

### MIDI effects, hybrid plugins and feedback

Audio and note connections share one dependency order. A plugin may emit sound and MIDI
at once; an audio analyzer can send generated notes to another instrument while forwarding
its audio. Fan-out and merge work for both. Pure MIDI processors such as transpose and
MIDI delay are ordinary graph nodes; the [reference implementations](../plugins/midi-effects/README.md)
exercise mapped note identities, expression, sustain and sample-timed releases. A regression
fixture passes audio through, generates notes at threshold crossings and plays a downstream
synth through transpose, checking both outputs against a directly scheduled reference.

All current cycles are rejected, including mixed audio/MIDI/parameter cycles. Adding an
ordinary delay plugin does not remove the dependency cycle. Feedback requires an explicit
**delayed return edge** with a documented minimum delay, cross-block storage, deterministic
ordering, cancellation, bounded growth, panic handling and latency reporting. That scheduler
work is planned; do not implement feedback as recursive processing or silently use previous
block contents. MIDI echo feedback also needs repetition/voice limits to avoid event storms.

The planned Live screen presents named instrument layers, keyboard/velocity zones, MIDI
processors and effect chains, backed by the same graph an advanced routing editor uses.
Neither that zone editor nor a full arbitrary parameter-mapping matrix exists today.
Parameter-target lanes currently have one target each. Hybrid instruments with note outputs
work as ordinary graph nodes but are deliberately excluded from prepared-instance pooling:
multiple outgoing MIDI owners need a separate sound-transition contract.

### Desktop performance-host compatibility (planned)

Oscairn should also serve as a desktop performance host: prepare sounds before playing,
keep held notes on their owners, expose clear zones/layers and allow advanced audio/MIDI
routing. This is a product direction, not a claim of Gig Performer feature or performance
parity. Measure full input-to-output latency, deadline misses, memory, plugin loading and
sound switches under realistic sustained load before making such claims.

Current format adapters are CLAP and optional LV2. Plan **VST3** for macOS/Windows/Linux
and **Audio Units** for macOS; AUv2 and AUv3 require separate hosting/lifecycle decisions.
VST3 is the principal additional Windows format; AU is Apple-specific. VST2 compatibility
is not promised. Gig Performer currently advertises VST2, VST3 and AU
([vendor](https://gigperformer.com/)); VST3's supported desktop platforms are documented by
[Steinberg](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/VST%2BModule%2BArchitecture/Index.html).
Keep format-specific state, bus negotiation, note dialects, parameters, GUI ownership and
preset discovery inside adapters, retaining the shared graph/control model.

Windows desktop support also needs process supervision, local IPC, filesystem/state helpers,
packaging and audio/MIDI backend validation; the current desktop/helper uses POSIX facilities.
Plugin scanning, custom GUI embedding, crash isolation and compatibility testing are separate
work from loading a new format. Current engine plugins execute inside the engine process;
a crashing plugin can still stop it. Desktop binaries cannot run unchanged on ARM Linux
hardware, and plugins do not gain DSP support without a compatible backend implementation.

### Parameter events between plugins

One extension beyond standard CLAP is needed: a sequencer's per-step parameter locks and
automation lanes must change parameters of *other* plugins. A plugin sends values on
numbered lanes without knowing what they control; the graph maps each (plugin, lane) to a
(target plugin, parameter) and delivers standard CLAP parameter events to the target in the
same block, at the exact sample. Global locks precede notes; explicit per-note locks
follow same-sample notes after their destination IDs are assigned. See
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
- **LV2** is first-class, for the large existing Linux ecosystem. The initial optional
  host supports audio/control ports, MIDI/time atom sequences and Worker scheduling
  (background work live, synchronous work when rendering offline);
  [implementation scope](../core/lv2-host/README.md) lists the remaining extensions.
- **VST3** hosting is planned for desktop and compatible Linux hardware builds.
- **AU** hosting is planned for macOS; AUv2/AUv3 lifecycle and distribution requirements
  belong to that adapter, not to the portable package contract.

The graph is format-neutral: a CLAP synth, an LV2 reverb and a VST3 effect are all just
nodes. Format support does not mean binary compatibility: an ARM64 Linux instrument needs
ARM64 Linux builds compatible with the device's runtime/ABI and host features. Package
self-containment and cross-format state migration are defined in [RFC 0006](rfcs/0006-packages-and-portability.md).

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

- Native Oscairn instruments must support simultaneous sound contexts and prepared
  sound switching: held notes retain their old sound while new notes use the selected
  sound. Native effects must preserve outgoing contexts/tails when used in transitions.
  Share engine code and suitable read-only assets; keep patch-dependent voices,
  envelopes, modulation and effect history separate. These are platform requirements,
  not guarantees provided by CLAP or LV2 themselves.
- Platform engines expose narrow capabilities: prepare patches in the
  background, share read-only data, activate a prepared context at a scheduled audio
  boundary, keep outgoing voices separately, report when a context is silent.
- Ordinary CLAP, LV2 and VST3 plugins are supported through persistent instances and a
  pool of prepared instances: new notes go to the prepared instance while the old one
  finishes its notes and tails.
- Note ownership survives a sound change: note-offs, sustain and expression are routed
  to the contexts owning the notes, including overlapping presses of the same key.
  Multitimbral third-party plugins may supply their own parts/contexts; otherwise use
  prepared instances. Do not assume a program-change message preserves old voices.
- State restore into an existing instance may pause/reset it and is explicitly separate
  from seamless sound switching. Prepare state on an unused context/instance first;
  only switch new-note routing when it is ready, and retire the outgoing context after
  its voices and tails finish. The Basic Synth now implements native prepared contexts,
  channel selection and note ownership through the control API ([RFC 0005](rfcs/0005-sound-contexts.md)). Ordinary CLAP/LV2 instruments now use a bounded prepared-instance pool.
  It preserves note ownership, channel parts and state/preset preparation; processed instances
  keep rendering until explicit retirement because arbitrary plugin tail completion is unknown.
  Native effect-context transitions and asset-bearing native patches remain to do.
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

### Audio ports and hardware routing

Stereo and multiple independent audio inputs/outputs are platform requirements.
Preserve plugin port identities and channel layouts, and route explicit source and
destination ports/channels rather than implicitly folding everything into stereo.
A DJ configuration can have per-deck stereo inputs/outputs, master, booth, headphone
cue (including split cue), recording buses, and effect sends/returns or sidechains.
The hardware audio service exposes the available device channels; the graph and
control API describe, connect and persist those routes. Mono/stereo-only hardware
uses the same model with fewer endpoints. Missing endpoints are reported rather
than silently sending cue audio to the main output. Routing and buffers are
prepared outside the audio callback, with explicit mono/stereo mapping policies.

**Current implementation:** the graph now allocates each plugin's actual channels and
routes explicit port IDs/channel ranges, including external inputs and independent
output buses. Connections can fan out, sum and apply explicit gains. The C++ Control,
JSON-RPC and RemoteControl interfaces expose the routes and external port metadata.
The desktop/helper maps logical channels to selected device channels at startup and
reports availability. Existing node-only connections retain their first-stereo-pair
behavior, including centred mono; the default demo still opens no inputs and two outputs.

This is routing infrastructure, not a DJ mixer UI or project-file persistence. The
renderer/Chain convenience interface still defaults to stereo. Hardware-specific
channel naming, clock alignment and physical I/O validation belong to Phase 4. The
current host uses the plugin's active port configuration; alternate CLAP audio-port
configurations, dynamic ports and automatic LV2 port-group interpretation remain future
work. Structural CLAP port changes fail the instance until reloaded, preserving existing
route identities. See [the control contract](control-protocol.md#audio-routing) and
[device selection](../hosts/README.md#audio-device-channels).

### Physical controls, takeover and direct position access (planned)

The shared **controller-mapping service** owns soft takeover (pickup), bank/scene
assignments and relative-encoder value tracking. The same implementation serves
built-in panels and external controllers, including knobs, sliders and pedals.
It runs without the screen UI. This is a planned contract, not an implemented
control API or plugin extension.

Hardware services report calibrated physical positions or encoder deltas, control
identity, available resolution and timestamps. Drivers handle electrical noise and
calibration; they do not decide which parameter a control owns in a scene. Preserve
the available measurement precision until a destination adapter requires quantization.
An encoder delta must not be presented as an absolute physical position.

Keep three distinct values:

| Value | Meaning and owner |
| --- | --- |
| Physical position/movement | What the hardware reported, before takeover. "Direct" means unaffected by pickup, not an unfiltered ADC reading or precision beyond the hardware's resolution. |
| Accepted mapped value | The value the mapping permits to reach its destination after takeover, scaling and range conversion. |
| Destination value | The current plugin parameter or external-device value, when known. Host/plugin feedback is authoritative; an unconfirmed last-sent MIDI value is only an estimate. |

Mappings select the intended behavior per assignment:

- **Pickup:** for absolute knobs and sliders, suppress destination writes until
  movement reaches or crosses the current target in the mapping's control-position
  space. Use a tolerance appropriate to resolution/noise, including nonlinear or
  reversed mappings. Show waiting/caught-up status and direction where a display or
  LEDs permit it. A bank or scene change alone must not send the physical position
  as a parameter edit.
- **Direct:** pass the position through immediately when the player moves the
  control. This is an explicit choice for controls whose position is itself the
  intended input, such as a performance fader. It deliberately permits a value jump.
- **Relative:** apply encoder movement to the destination's known value, maintaining
  independent values for each destination across banks. Convert to absolute MIDI
  CC only at an output that requires it. Relative MIDI encodings are decoded by
  the controller adapter.

**Plugins may request direct position input, accepted mapped input, or both.**
Ordinary plugin parameters receive accepted changes and need no takeover awareness.
A plugin needing the physical position can opt into a separately routed controller
input, or the user can explicitly bind a direct mapping to one of its parameters.
For example, a plugin may use accepted cutoff changes for sound while using the
physical fader position for an indicator or a separate performance calculation.
Receiving both must not write the same parameter twice: direct observations and
accepted edits remain distinct inputs, even while pickup is waiting. The plugin
declares the inputs it supports; the mapping/user chooses what to connect. Plugins
do not gain implicit access to every physical control.

Use portable logical control identities rather than board-specific ADC channels.
Events carry their source timestamp and binding generation so consumers can correlate
physical observations with accepted edits and reject stale events after reassignment.
A subscription/rebinding snapshot must be distinguishable from new player movement.
Define the optional graph event/capability contract and CLAP/LV2 adapters before
implementation; do not assume existing plugin formats universally expose both streams.
Plugins consuming direct position as an intentional performance input may implement
their own interpretation, without changing takeover policy for other mappings.

Pickup state belongs to each physical control/binding, not globally to a parameter:
two sliders can target the same parameter while only one has caught up. Refresh
targets on scene/preset changes and relevant feedback from other controllers, plugin
edits or automation; distinguish acknowledgments of the mapping's own writes to
avoid feedback loops. Rebinding, reconnects and lost-event resynchronization must
invalidate stale pickup state. Define active-gesture/automation arbitration explicitly
before implementation so feedback cannot repeatedly seize and release a moving control.
When an external device supplies no feedback, expose the uncertainty and an explicit
initialization policy; remembering the last sent value cannot guarantee jump-free
behavior after an unseen preset change. Persist mappings/policies, not a claim that
a physical control is still caught up after restarting.

The control API configures mappings and exposes value/takeover status. Physical
performance events retain timestamps through the bounded direct event path described
in [RFC 0004](rfcs/0004-hardware-services.md); they need not round-trip through the
screen UI or JSON-RPC. Motorized-control feedback and touch sensing are separate
capabilities and must not be mistaken for new player movement. Implementation tests
must cover bank/scene changes, pickup crossings, sliders, encoders, direct/both-input
consumers, competing controllers, automation, feedback echoes and reconnect/overflow.

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

### Backend-specific installation

One engine release can publish alternative CPU, FPGA and dedicated-sound-chip backends.
Common state/assets and the selected backend's dependencies are separate package components;
install only the destination's resolved execution plan, retaining other variants as metadata.
A hardware instrument still needs its small CPU control adapter, but need not store the full
CPU DSP implementation or unrelated FPGA/chip payloads. CPU reference availability is a
portability requirement, not a demand to install or run it on an underpowered device.

Resolve by exact board/service/firmware capabilities and supported sound modes, not just
Linux/ARM64 or a chip name. The package manager selects files; the resource manager separately
admits the whole performance and reserves parts/voices, memory, accelerator regions and CPU
control/transfer budget. Defaults and offline exports use these richer destination profiles.
See [backend packaging](rfcs/0006-packages-and-portability.md#backend-variants-and-device-specific-dependencies)
and [hardware/sound semantics](rfcs/0004-hardware-services.md#backend-packages-and-sound-modes).

Equivalent backends can be selected automatically within a saved sound/quality contract.
If hardware changes the tone or offers an unmatched quality mode, save that mode explicitly;
unsupported devices must report it or use a user-chosen alternative. A dedicated chip may
only expose fixed synthesis operations and resident assets, not accept uploaded DSP code.
MODX/SWX100 access is an unverified research example, not implemented Oscairn support.

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
scenes and an exact dependency lock. The document separates logical intent from local
installation paths and device mappings; a plugin's opaque state may still be platform-specific.

The goal is moving sounds between hardware, the desktop app and the DAW plugin without
rebuilding them. Smaller hardware may reduce polyphony under an explicit policy or refuse a
Performance it cannot run, and says so before activating it.

- Stable engine, parameter and macro IDs; versioned schemas with migration rules and tests.
- Complete plugin state saved alongside the graph.
- A dependency manifest and lock of product/format identities, exact releases/artifacts,
  required features, state provenance and assets.
- Missing dependencies keep their saved state and routes; compatible installation can restore
  the Performance. A matching product name or successful plugin load does not prove compatibility.

[RFC 0006](rfcs/0006-packages-and-portability.md) defines the proposed one-import transfer
workflow: collect the entire Performance/Setlist dependency closure, choose destination builds,
reuse shared packages and preflight before activation. Same-format target builds are preferred.
AU→VST3 or LV2→CLAP/VST3 changes require tested state, parameter and port mappings; otherwise
retain an unavailable-node placeholder. Cross-platform state migration and resource admission
are separate requirements from copying package files.

### Preparing and editing across devices (planned)

A library can remember several performance devices and assign the same or different Sounds,
Performances and Setlists to each. Share musical revisions; keep target dependency locks,
controller/output mappings and explicit device variations separate. Show readiness per item
and device, with missing downloads, incompatible modes, capacity and transition/load limits.
Local audition requires an equivalent implementation; connected hardware can produce the
sound where supported. Hardware-only offline sounds must not masquerade as live previews.

Save instrument edits as revisions with ancestry and full state/assets. Transfer can update
a shared revision or an explicit variation; concurrent edits preserve both versions for
resolution. Rehearsed setlists remain pinned until deliberately updated. Derived states and
destination checks become stale after edits; preserve unavailable hardware-specific sounds
instead of silently substituting or resurrecting old state on another device.

Use a Git-maintained public catalog of declarative instrument capability profiles, published
as versioned authenticated snapshots for offline clients. Separate model/firmware profiles
from actual device inventory and private musician settings. Evidence and runtime checks,
not the model name or catalog entry, establish support. See the complete
[multi-device workflow](rfcs/0006-packages-and-portability.md#preparing-for-several-performance-devices).

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

System areas now use a shared semantic palette and look-and-feel. Users can install
validated data-only themes and apply one appearance across layouts, plugin browsing and
generated parameter editors; other UI processes share the selection. Layout files own
geometry/bindings, not colours. See the implemented [design language and theme contract](design-language.md).
The desktop browser scans CLAP/LV2 in isolated helper processes, replaces instrument/effect
slots through validated `graph.replace`, and exposes generic parameters and advertised
presets. Plugin identity is format plus stable ID; filesystem paths locate installations.

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
  export, and a full backup. Export can collect the full transitive dependency closure for
  selected destination profiles, including every scene and setlist item. Import reconciles
  IDs/revisions without overwriting divergent work and reports unresolved dependencies. A
  lightweight document copy or references-only export is not a complete offline transfer.
- **Sync** between devices, the desktop app and the DAW plugin uses the same mechanism,
  moving only changed files. Plugins are per-platform binaries: sync copies a plugin only
  if its package has a compatible build and state contract for the other side, and
  otherwise reports the specific incompatibility. Same CPU architecture alone is insufficient. A
  computer syncs with a device only once paired with it, for example by confirming a code
  shown on the device's screen.

## Packages: sound packs, libraries and plugins

Like the libraries on a MODX or Montage, people can share and reuse sound packs; unlike
them, without fixed slots, and without DRM. Third-party plugins (engines, effects,
sequencers), layouts and themes install the same way.

This is planned behavior. [RFC 0006](rfcs/0006-packages-and-portability.md) specifies
managed packages, shared runtime dependencies, target selection, external vendor plugins
and missing-plugin handling. A managed package is self-contained through its declared
dependency closure and platform baseline; it need not duplicate every shared library.
Import prepares all managed dependencies in one operation. Conventional vendor installers
and activation services remain external requirements, visibly outside that guarantee.

Projects declare packages/version requirements in a readable manifest and lock the resolved
transitive dependency graph. Each project gets its own dependency view backed by a shared,
immutable store, so projects using the same release reuse stored files. This is the familiar
manifest/lock/install workflow with pnpm-like storage sharing, not a dependency on Node/npm.
A disposable download cache is separate from installed content pinned for offline use.
Offline installs use the same lock and local/imported packages; missing files never trigger
an unrequested version substitution.

Exports default to the required binaries/runtimes for a versioned set of supported Oscairn
Buildroot Linux profiles, with shared content/assets. A Mac/PC exporter does not include its
own desktop binaries by default. Advanced settings select other or multiple targets; an
explicit named-device export can target just that device. The export reports offline
completeness per profile and warns if a required hardware build is unavailable.

Import installs only compatible target artifacts and shared dependencies, leaving unrelated
binaries out of the installed store while retaining their release metadata. It first reuses
local content, then downloads exact missing destination builds from configured sources when
enabled, or reports what must be installed. A Linux-default package imported on a Mac/PC can
therefore acquire desktop binaries without carrying them on every offline hardware device.
The source archive is preserved; full-archive retention is an explicit backup choice. See
[the target policy](rfcs/0006-packages-and-portability.md#default-export-targets-and-selective-installation).

Planned **Settings > Installed content** manages plugins/apps/libraries per device, with
versions, installation reasons, Used by paths, import/export, retention, reclaimable storage
and batch removal of unused content. Shared dependencies are optionally visible. Deleting
musical documents does not implicitly uninstall their engines: default to a grouped cleanup
prompt, with a per-device Keep installed without asking option. Retained revisions and live
resources protect dependencies; intentional removal of a used package warns about affected
documents and preserves their state for reinstall. User recordings/presets survive uninstall.
See [removal rules and package-manager prior art](rfcs/0006-packages-and-portability.md#installed-content-and-removal).

Missing content can be restored from any compatible verified bundle without replacing saved
musical edits. Offline instruments export a Missing content request; a computer collects the
exact target packages/assets from its store or configured sources and returns a repair bundle.
A supported USB companion connection automates the same exchange. Revalidate the current
device and affected documents before marking them ready; never activate repairs mid-show.
Offer backup before removal and distinguish backup availability from download availability.
See [offline repair](rfcs/0006-packages-and-portability.md#repairing-missing-dependencies-on-offline-instruments).

**Oscairn-native** identifies a tested portability contract: declared dependencies, stable
engine/state/assets, builds for advertised profiles and normal operation in other hosts.
It is open to third-party authors and is independent of file format. **External** plugins
have no such guarantee: the destination needs a compatible installed implementation or an
approved tested conversion. Otherwise the node is disabled with a visible reason while
preserving state/routes, and the affected performance is not ready. Native plugins also
report unsupported targets/resource limits. The existing sound-context `mode: native` alone
does not establish package portability.

- **Everything installable is a package:** a manifest (ID, version, author, licence,
  dependencies, the platforms its plugins are built for) and its files. Kinds: content
  (Sounds, Performances, Projects, samples, presets), code (plugins and shared runtimes,
  with builds for supported Linux, macOS and Windows CPU/ABI profiles), and looks (panel
  layouts, themes, plugin pages). CLAP/LV2/future VST3 can use managed package roots; AU
  distributions that need OS/app registration may remain external dependencies.
- **Stored once.** Installed package files live in a content-addressed store, by hash, the
  way git stores objects. A pack that references an already stored plugin or sample reuses
  those bytes; manifests and any required materialized copies still have a cost. A user's own work
  stays plain files in readable folders; installed packages appear in the library, and as
  read-only folders in the Files view.
- **Shared dependencies, private DSP state.** Related plugins can depend on one pinned
  runtime library or sample package. Different versions coexist; code must match the
  destination ABI. Storage deduplication does not guarantee one RAM copy: immutable code
  mappings and cooperating asset consumers can share, while decoded buffers, voices, delay
  lines and other per-instance state remain private. Loader conflicts require isolation
  or rejection, never a global runtime replacement (see persistent engines and RFC 0006).
- **Libraries are read-only.** An installed pack appears in the library next to the user's
  own work. Sounds refer to its items by ID and version, so editing never changes the pack;
  saving makes the user's own copy. Updates install side by side, and saved work keeps the
  version it was made with until moved on deliberately.
- **No copy protection.** The platform has no licence keys, no activation, no activation
  servers and no licensing API, and the default catalog does not accept packages that need
  them: a plugin must work with nothing but its files. Copy protection and licensing are
  strongly discouraged, and the library and catalogs point users to open alternatives.
  Oscairn cannot stop a vendor shipping a plugin with its own licensing, but it does not
  manage or bypass that activation. Such plugins are external dependencies; planned
  sandboxing does not make their installation or licensing portable. Runtime plugin
  isolation is not implemented yet. Packages
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
  hash. A complete offline export includes required user samples and factory/pack assets
  or their packages, except content guaranteed by the destination baseline. References
  alone suffice only when the destination already has those exact assets. When a plugin
  cannot enumerate its dependencies, include its complete permitted library or report the
  gap; the host cannot infer hidden file references from an opaque preset.
- **The host gives plugins their samples.** Plugins run sandboxed, so a sampler does not read
  arbitrary files: it asks the host for an asset by ID and gets it read-only, memory-mapped
  from the store. Cooperating instances can share immutable mappings and stream large
  samples; private decoded/processed buffers remain separately budgeted. Complete asset
  declarations let the host collect what a Sound needs for export/sync. This planned
  Oscairn SDK extension does not make arbitrary third-party file access enumerable.
- **Old versions stay while used.** A library update that changes a sample keeps the old one
  in the store while any saved Sound still refers to it; unused files become candidates for
  the device's explicit cleanup/retention workflow.

### Built-in plugins and other manufacturers' defaults

Different devices will ship different built-in plugins: Oscairn's own, and engines a
manufacturer adds. Packs should work across them without carrying copies of everyone's
defaults.

- **A declared baseline.** A versioned platform profile ("Oscairn Core 1") lists plugins
  and state capabilities each conforming target includes. A baseline-only pack has known
  dependencies on those targets; its graph must still fit the device's resources and I/O.
- **Manufacturers' built-ins are ordinary packages**, pre-installed in the read-only system
  area of their devices. Installed on another device, where a build for it exists and its
  licence allows, they work the same way.
- **Packs resolve dependencies by target.** A manifest records IDs, compatible ranges and
  the required baseline; a lock pins concrete releases. An offline export includes missing
  permitted packages, including manufacturer extras when the destination lacks them. It
  omits baseline packages only when the target profile guarantees them. The store
  deduplicates whole-package content; source-device built-ins are not assumed universal.
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
- **Distribution:** a package carries available target builds, licences/notices and exact
  source/build provenance where available. First-party packages ship source/build recipes;
  third-party redistributions preserve their applicable source and notice requirements.
  Catalogs list target availability explicitly; unavailable builds are not created by
  copying a binary from another OS. External vendor references remain distinct.

## Native plugins and the platform in DAWs (planned)

Native plugins must work directly in compatible DAWs without running the Oscairn app or
its background engine. Planned first-party desktop artifacts are CLAP/VST3 on supported
Windows, macOS and Linux profiles, plus AU on macOS. They share engine/state/assets and
work with ordinary host APIs when Oscairn extensions are absent. MIDI-effect routing and
other advanced features depend on the host/format; this does not promise every DAW or AAX.
Evaluate existing clap-wrapper support before writing new format bridges. An optional
standalone shell can expose an individual plugin's audio/MIDI without a DAW.

Separately, Oscairn itself will ship as a CLAP, VST3 and AU plugin: the core, its hosted
plugins and the UI as editor, so a whole graph can be used inside a DAW. The DAW supplies
audio/transport and stores the graph state plus dependency lock. Oscairn standalone is the
same core with its own device adapter; it does not require a DAW. These are distinct uses,
not a requirement to run the whole Oscairn graph just to load one native instrument.

The package manager can publish DAW-facing bundles to standard discovery locations while
reusing the shared store where signing/filesystems permit. Ordinary DAWs do not understand
Oscairn package locks: wrappers must retain compatible state, version and asset resolution,
and old sessions must not silently bind to the newest engine. The direct-plugin route does
not make an entire third-party DAW project automatically portable. See
[RFC 0006](rfcs/0006-packages-and-portability.md#native-plugins-in-daws-and-standalone-applications)
for offline dependencies, version pins, missing-plugin behavior and validation requirements.
No VST3/AU export or Oscairn DAW container is implemented yet.

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

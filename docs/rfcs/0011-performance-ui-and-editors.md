# RFC 0011: Performance UI, routing views and plugin editors

Status: in progress, 2026-11-01. Steps 1–4, undo and Play implemented (see [Progress](#progress)). Builds on the graph, note routes and parameter
lanes that exist today ([architecture](../architecture.md#live-note-routes)), the planned
controller-mapping service ([physical controls](../architecture.md#physical-controls-takeover-and-direct-position-access-planned))
and the theme contract ([design language](../design-language.md)).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. Model: wire and part ownership, implicit Mixes, port routes, saved and in the protocol ([status](#step-1-status))
- [x] 2. Parts and Part views: parts list, chain, zones, scenes, shared buses
- [x] 3. Split, join, buses, the Mixer ([status](#step-3-status))
- [x] 4. Routing view on the desktop ([status](#step-4-status))
- [x] Undo for every edit ([status](#undo-status)); preset and state loads remain
- [ ] 5. Editors: knob pages for every plugin done through [RFC 0019](0019-adaptive-ui.md)'s
      generated editors ([status](#step-5-status)); what modulates each knob and the Modular
      grid remain
- [ ] 6. Physical controls: default layers, overrides, learn, banks (layers, banks, overrides saved, learn on a panel's knobs, plugin roles and screen knobs following the editor page done; [status](#step-6-status))
  - [x] [instant controls](#instant-controls): live gains for part levels, mutes, pans, send levels, the master and scene glides ([status](#live-gains-status))
  - [x] the controller-mapping service in the engine, on its MIDI input threads ([status](#engine-mapping-status))
  - [x] the latency measured in a test, against one block ([status](#latency-status))
  - [x] values pushed to clients (`controls.changed`), so the UI and devices follow without reading the graph ([status](#latency-status))
  - [x] faders and mutes edited on screen shown at once and sent without waiting ([status](#screen-edits-status))
- [ ] 7. Groups, pedal view, move, copy and share
- [x] Play: the Performance app's view for the stage ([design](#play-the-performance-apps-first-view), [status](#play-status))
- [x] The full scene model (blocks and wires enabled per scene, what each scene saves),
      seamless on the engine's draining and retiring ([status](#scenes-status))
  - [x] plugins and wires switched off and on, seamlessly
  - [x] scenes record all of it, each with its scope
  - [x] the desktop's controls for it
  - [x] transitions: a glide per scene along a curve, launching on the beat or bar, and a control morphing between two scenes ([status](#scenes-status))
  - [x] the mappings' scenes laid out as one table ([status](#scenes-status))

## Problem

Oscairn's graph can already route audio, notes and parameter lanes between any plugins, but
there is no way for a musician to build a performance on the device: no parts, layers,
shared effects, mixer or editors. Two familiar models each solve half of it:

- **Gig Performer** wires anything to anything, but every input and output is drawn by
  hand for every performance.
- **The MODX** organizes a Performance as parts with zones, inserts and shared effects,
  which is quick to set up but fixed in shape.

Both also leave most physical controls to be assigned by hand, unless you start from a
factory performance (Jacob's experience with both).

The device must support building and editing sounds without a computer. The desktop is a
second, larger surface for the same work.

## Outcome

- One graph, several views of it. Most playing and setup happens in a **Parts** list.
  Routing appears only where the parts cannot express something, and nothing drawn in
  Routing is hidden from the other views.
- Common shapes need no wiring:
  - a new part listens to the keyboard (or an audio input) and plays to Main;
  - layers, splits, parallel effects and shared effects are made with dedicated controls.
- Edits in one view never silently break work done in another.
- Every plugin gets a usable editor on the device. First-party and willing third-party
  plugins can ask Oscairn to draw a richer editor in the system theme, controlled by the
  device's physical controls.
- **Physical controls work as soon as a sound is loaded** and can be reassigned to anything
  in the performance.

## Views

| View | Shows | Like |
| --- | --- | --- |
| **Parts** | One row per part: input and zone, chain, sends, level, mute. Shared buses as rows under the parts. Scenes. | MODX parts, Gig Performer's front panel |
| **Part** | One part's chain, left to right, with split lanes, its zone and quick controls. A **Pedal view** of the same chain shows bypass lights and footswitches. | Ableton device chain, pedalboards |
| **Routing** | The real graph as nodes and wires, in columns: inputs, note processing, instruments, effects and buses, outputs | Gig Performer's wiring view, Bitwig |
| **Mixer** | A strip per part, bus and output: sends, pan, fader, meter, mute and solo | Any mixer |
| **Plugin editor** | Knob pages, or a structured editor the plugin describes, or the plugin's own window (desktop) | |

Mockups of these views at the device's 1024 × 600 size were reviewed on 2026-10-30.

### Play, the Performance app's first view

The Performance app (RFC 0030's tile) opens in **Play**, for the stage; **Edit**, the app's main
action, goes to the views above. Decided by Jacob on 2026-11-01 from the
[mockup](https://claude.ai/artifact/VVoBQGnVFYqEjvbGSPmXCN):

- **Scenes** along the top, one tap each (and the panel's pads, where the profile has them).
- **Previous and Next** stay when the Performance came from a setlist, besides the Setlist app
  and the product's buttons.
- **A row of knobs** following the Performance's controls (Super Knobs and the others), the
  first ones on the panel's knobs; a switch moves the row to the selected part's own controls
  when a part is selected.
- **A strip per part and bus**: level, a meter per channel, mute and solo; muted parts dimmed.
- **The theme's style** for the strips and knobs, as the Mixer uses it (sections, edges,
  sliders and marks), not a look of their own.
- One touch for everything; nothing behind a menu, a long press or a dialog.

## The model behind the views

The graph stays the only model. The views are projections of it, plus a small amount of
layout data (part order, names, colors, which nodes are grouped), saved in the Performance.

### Parts

A **part** is an input selection plus the chain of nodes reached from it, ending at an
output. The input is any of:

- a note source with a zone (keyboard, pads, a MIDI port or channel, key and velocity range);
- an audio input (a guitar part is a part whose input is Audio In 1);
- a generator inside the chain, such as a sequencer.

A node belongs to a part when everything feeding it comes from that part. A node fed by
several parts is **shared** (see below).

### Who owns a wire

Every wire records who made it:

- **Part wires** are created and maintained by the Parts and Part views (the input into the
  first box, box to box, the last box to the output, sends).
- **User wires** are drawn in Routing.

The rules that keep the views from breaking each other:

- The part views change only part wires.
- User wires are attached to a node and port, not to a position in the chain, so they move
  with the node when it is moved, reordered or copied.
- Inserting a node between A and B replaces the part wire A→B with A→new→B; user wires on
  A or B stay.
- Deleting a node with user wires first lists them and asks whether to reconnect them to
  its neighbors or remove them.
- Replacing a plugin remaps wires by port (main to main, sidechain to sidechain) using the
  channel rules below. A wire that cannot be remapped stays visible as unconnected; the
  replacement is not refused. (Implemented in step 1.)
- Nodes with user wires show a badge in the part views; tapping it opens Routing at that node.
- Every edit is compiled and checked before publication, as today, and every edit is undoable.

### Split and join

Host-provided nodes, drawn as narrow pills about a third of a box's width, that can appear
anywhere in a chain. Each split pairs with a join, and the lanes between them stack
vertically.

| Split | Lanes get | Join | Result |
| --- | --- | --- | --- |
| Notes: Layer, Keys, Velocity, Channel | the same notes, or a zone of them | Mix | summed audio |
| Audio: Copy | a full copy each | Mix | parallel processing, dry/wet |
| Audio: Left / Right | one mono channel each | Join to L / R | stereo |
| Audio: Mid / Side | mid and side | Mid / Side to stereo | stereo |
| Audio: Bands | frequency bands from a built-in crossover | Mix | multiband processing |

- **Mix** has a level, pan, mute and solo for each lane, shown on the chain itself.
- **Plugins with several outputs** are drawn the same way: a crossover or a multi-out drum
  sampler is a plugin box followed by one lane per output port. The graph already supports
  plugins with several ports and one output feeding several inputs, so the built-in Bands
  mode and crossover plugins coexist.

### Shared buses and shared nodes

A **shared bus** is a Mix node followed by a chain, playing to an output. Parts reach it in
one of two ways:

- **Send:** a copy at a set level, after the part's chain, alongside the dry sound;
- **Output:** the part plays through the bus instead of straight to Main.

These are not the plugin sound buses of [RFC 0007](0007-effect-contexts.md), which are
output ports of one native engine.

How a bus shows in each view:

- **Parts:** a row under the parts, showing what feeds it and its chain. Part rows show
  their send levels.
- **Mixer:** a strip.
- **Routing:** a Mix node (labeled with the bus name) in the effects and buses column,
  followed by the bus chain. Sends are part wires into the Mix node, each labeled with its
  level.

**Shared effects wired by hand.** A musician may skip buses and, in Routing, wire two parts
into one reverb. The graph then has a node with two sources. The views treat this the same
as a bus, because it is one:

- Wiring a second source into an input that already has one inserts an implicit Mix node in
  front of it, so levels, mute and solo stay available per source.
- The Parts list shows the shared node as an unnamed bus row ("Shared · Hall Reverb, fed by
  Piano and Pad") that can be named.
- Each feeding part's chain ends in an exit chip ("→ Hall Reverb, shared") instead of Out.

So whether a musician uses the bus controls or draws wires, the result is the same structure
and every view shows it.

### Groups

Several boxes can be grouped into one box that opens into its contents. A group is a
sub-graph node that the core still manages ([architecture](../architecture.md#two-kinds-of-thing-plugins-and-the-ui)),
so routing, crash recovery and parameter addressing still see every plugin inside it. A group
can expose its own knobs, each driving any parameters inside it.

### Moving, copying and sharing an effect

From a box's menu:

- **Move** to another part, with its settings and its user wires.
- **Copy** into several parts, each with its own settings from then on.
- **Share:** one instance on a new or existing bus, fed by the chosen parts by send or output.

The dialog states what will change before it is applied.

### Long chains

About five boxes fit between In and Out on the device screen. Longer chains scroll sideways
with snapping, edge fades and position dots; the selected box stays in view, and the
selection knob moves through it. A compact mode shows boxes by name only. Grouping is the
remedy for chains that stay long.

## Ports and channels

Plugins declare audio ports (main, sidechain, auxiliary) with channel counts. The UI wires
ports to ports, and the core applies these rules when a port connects to a port:

| From → to | Rule |
| --- | --- |
| Same channel count | One to one |
| Mono → stereo | Copied to both sides, with a pan control on the wire |
| Stereo → mono | Summed, at −3 dB |
| Anything else | A channel picker on the wire; never guessed |

A chain follows main ports. Extra outputs appear as sockets on the box, each able to start
lanes or another chain. Extra inputs, such as a sidechain, appear as sockets on the box's
left that stay empty until wired.

The graph stores routes per channel; since step 1 the Session's wires are port-level and
compile into them with these rules.

## Plugin roles

A role is worked out from the plugin's ports, not chosen by hand. CLAP's feature strings
(`instrument`, `audio-effect`, `note-effect`, `note-detector`, `analyzer`) are hints.

| Takes in | Puts out | Role |
| --- | --- | --- |
| notes | notes | Note effect (transpose, arpeggiator, MIDI delay) |
| transport only | notes | Note generator (sequencer) |
| notes | audio | Instrument |
| audio | audio | Effect |
| audio + notes | audio | Effect played by notes (vocoder, keyboard-tracked filter) |
| audio | notes | Audio to notes (pitch or drum trigger) |
| nothing | audio | Generator (looper playback, test tone) |
| audio | nothing | Analyzer (tuner, meter, recorder) |
| anything | parameter lanes | Modulator ([RFC 0002](0002-param-target.md) lanes) |
| notes | notes + audio | Instrument with note output |

The color of a box follows its main output (notes, instrument, effect). A second tag names
the input when it is unusual ("Effect · played by notes").

## Plugin editors

Oscairn draws editors for plugins on the device. Plugin windows exist only on the desktop
and for plugins with no description.

1. **Knob pages, for every plugin:** generated from the plugin's parameters, grouped by
   CLAP parameter module paths where present, in pages matching the device's knob banks.
   Each knob shows what modulates it (parameter lanes, aftertouch).
2. **Structured editors**, which a plugin describes and Oscairn draws in the system theme
   with the physical controls mapped: a module grid (chillbeast's Modular,
   [evaluation](../modular-engine-evaluation.md)), a sample map, a step lane, an envelope.
   Editing actions travel as plugin-defined operations through the control API, not as
   pixels.
3. **The plugin's own window:** for third-party plugins on the desktop. On the device such a
   plugin gets knob pages. The process hosting the plugin, its worker
   ([RFC 0010](0010-plugin-worker-isolation.md)), shows the window, as a window of its own:
   macOS cannot embed one process's view in another's window
   ([RFC 0019](0019-adaptive-ui.md#plugin-windows)).

The description format is a new Oscairn extension, `oscairn.ui`, defined in
[RFC 0019](0019-adaptive-ui.md#plugin-editor-descriptions) with how each surface (desktop,
device panel, small screen) draws it.

## Physical controls

Controls follow the device: the KICK, the Ignite and a MODX-style keyboard have different
knobs, faders and buttons. A device's **profile** ([RFC 0024](0024-device-profiles.md#control-groups))
describes them in **groups**, each with its kind (knobs, encoders, faders, buttons, wheels, a
touch strip...), its count and its **role**:

- **notes:** keys and pads, which play notes (pads through the pad map below);
- **screen:** knobs that play what the screen shows. In Play they are the knob row, as many
  tiles as the device has screen knobs (six on a MODX M); in an editor, the page's top
  parameters by priority ([RFC 0019](0019-adaptive-ui.md)). They are never overridden;
- **assign:** controls that play banks. An assign group of knobs either has hardware **rows**,
  each a bank its row button picks directly and names on screen (the MODX M's four control
  knobs: Assign 1, Assign 2, Tone, EG, EQ, Effects; Quick Edit's 24 parameters are six rows of
  four), or is **paged** by a button or the screen (the KICK's P1 to P4). Faders may have
  **modes**: part (part levels), element (the sound's element, operator or oscillator levels)
  and audio (the levels of parts playing audio inputs);
- **fixed:** a dedicated function, named per control (volume, master, rec gain, tempo, a Super
  Knob, scene and part buttons), which assignments leave alone.

A device **layout** places the controls on its panel ([design language](../design-language.md)).
The controller-mapping service owns pickup, relative encoders and bank state (below).

An external MIDI controller's controls join the device's while it is connected, numbered on
after them ([RFC 0024's controllers](0024-device-profiles.md#controllers)): its knobs play the
banks, its faders part levels, its wheels pitch and modulation, exactly as a panel's would.

### Defaults, so nothing must be programd

Assignments resolve in layers. Each layer fills only what the layers above it left empty.

1. **Performance overrides** set by the musician (below).
2. **The sound's own assignments**, saved with the sound by its author: Assign 1–8 and
   named macros, and controls assigned by identity (`wheel:2` to the filter, say) to its own
   parameters and controls. They travel with the sound to another performance or device.
3. **Fixed role banks**, filled from roles the plugin declares (cutoff, resonance, attack,
   release, effect mix). They are the same on every instrument. A role the plugin lacks
   shows as a blank control rather than something unrelated. Third-party plugins without
   declared roles get Assign banks from their first parameters until someone maps them.
4. **Performance conventions:**
   - faders control part levels (in their part mode);
   - buttons control part mute, or scenes, as their group's `target` says;
   - knobs follow the selected part's banks;
   - the mod wheel, pedals and aftertouch are sent to every part as standard controllers,
     as their group's `functions` say.

**The mod wheel is the Sound's.** It is a `wheel` control like any other, sent to every part as
CC 1, which each plugin turns into its own default (vibrato in Oscairn's SDK). A Sound can assign
it to one of its own parameters or controls instead; then moving the wheel plays that target in
that part, and the parts whose Sounds leave it unassigned still get the plugin's default. A
Performance override for the wheel plays its target in place of every part's. The same holds for
every control of a group with `functions` (pitch bend, pedals, the ribbon).
5. **Effects bank:** the main control of each effect box in the selected part, in chain order.

**Banks are as wide as the group playing them**, not a fixed four: a group of six knobs gets
Assign 1–6. Each role bank has an order longer than four (Amp EG: attack, decay, sustain,
release, then hold, velocity, key tracking and level), so a group of any width takes its first
as many roles and fills its row with no blanks where the roles exist. A paged group pages
through the banks (Assign 1–4, Assign 5–8, Filter, Amp EG, Effects on the KICK); a group with
rows shows each row's bank, the Nth row of a kind taking the Nth bank of that kind. Two banks
side by side would only fit a group twice a bank's width, and every bank is as wide as its
group, so a group shows one bank at a time.

### The controller-mapping service

What turns a moved control into a value at its target, the same for a panel and a controller
([architecture](../architecture.md#physical-controls-takeover-and-direct-position-access-planned)):

- **Finding the control:** a message from a controller's port is looked up by port, channel and
  CC, note or pitch bend in its profile's groups; what it plays is the control's resolved target
  now (the layers above, with each group's page, row or mode). A group with rows sending a number
  per row moves to the row the message came from.
- **Absolute knobs and faders pick up** (soft takeover): after the target changes (another
  page, part or override), or its value is changed by something else (the screen, a scene, another
  control), the control sends nothing until it reaches the target's value (within 1/127) or
  crosses it, then follows. Pickup state is per control, never per parameter, and is not saved.
- **Relative encoders** add each step, 1/127 of the target's range per step and the step's size
  with acceleration, to the target's current value: no pickup is needed.
- **Buttons and footswitches:** a momentary one toggles a mute or selects a scene on each press; a latching
  one sets the mute from its state, or selects a scene on each change (each press, on or off).
- **Passed through:** a control with a standard-controller function and nothing assigned does
  nothing in the service: its message reaches every part on its own as that controller.

Built first in the app, where the profiles and the groups are, applying values through the
control API like the screen's knobs. That was too slow to play (below), so it moves into the
engine.

### Instant controls

**The problem, measured (2026-11-02).** On the converted Performance "Drifting", the mod wheel
plays control 1 ("Mod Wheel"), whose one target is a part's level (logarithmic, 0.707 down to
0.0021). Driven over a virtual MIDI port in the running app, the part's level settled 28 to 70 ms
after the last CC. A message took this path: the app's MIDI thread queued it; a 10 ms timer on
the app's UI thread ran the mapping; `controls.set` and a `graph.get` read-back went over the
socket to the engine, which answers requests from a 5 ms timer; there `setMacro` rebuilt every
part's wires and compiled a new graph plan, which the audio thread adopted at its next block
(with a 5 ms ramp). Each stage waits for a timer, and the plan compile grows with the
Performance.

**The target.** A control's move reaches the sound within about one audio block (2.7 ms at 128
frames and 48 kHz) after the engine receives the message, ramped across that block so a sweep
does not zipper, with no UI thread, socket round trip or plan compile on the way. It is
measured in a test that feeds MIDI into the engine's controller input with a timestamp, renders
blocks paced like an audio device, and reports the time to the first block whose output has the
new level (the median and worst over many moves, against one block). The figure covers the
engine; the MIDI driver's own delay before the engine sees the message is outside it.

**Live gains.** Part levels and mutes, and the Performance's master level and pan, are not baked
into the plan's route gains. The graph keeps a fixed table of live gains (256 slots, each an
atomic target the audio thread reads once per block); a route names up to four slots whose
product scales it, and the audio thread ramps each route from the product at the last block to
the new one across the block. The Session gives each part a slot (the master two, left and
right) and compiles a part's fader wires with the part's level at unity and unmuted, naming the
slots of its own fader and of the empty buses it passes; a slot's value is the part's level, or
0 while muted. Moving a fader then writes a slot and never compiles a plan; the wires the views
read are rebuilt with the new levels on the control thread. Edits of the structure (a plugin
added, a wire, a solo, a send's level) still compile plans, and a scene or undo writes the slots
with its compile. Where a wire cannot be live (two ways to one place through different empty
buses, merged into one wire, or more than four slots), it keeps its baked gain and a move of
those faders compiles a plan as before. A Performance's plugins ringing out after a change keep
the gains they had (their routes are frozen when they are carried over).

**The mapping runs in the engine.** The engine already opens every MIDI input for notes; it
now also runs the controller-mapping service on its MIDI input threads:

- The app resolves the profiles, the musician's choices of profile per port and the device's
  own groups (RFC 0024), as it does now, and sends the engine the result when it changes: the
  joined control groups with their MIDI, and which input (by name) is which profile's port
  (`controllers.set`). Profiles stay the app's: the engine needs only what they resolve to.
  The Performance's assignments and controls are the engine's already.
- The app also sends what the screen shows when it changes: the part the knobs follow, the
  knob group's page, and the editor page shown for screen knobs (`controllers.view`).
- On the control thread the engine compiles a snapshot of what the mapping reads (the
  assignment source of the open Performance, the groups, each input's binding, the parts' live
  slots, the controls' targets with their bases) whenever the Performance or those settings
  change. A MIDI input thread maps a message against it under a lock that only the MIDI threads
  and the control thread's short snapshot swaps take, never the audio thread.
- What a message plays is applied at once where that is lock-free: a part's level or mute (on
  its own, or as a control's target, as the mod wheel's control above) is written to the live
  gain slot, which the audio thread reads at its next block. The MIDI thread evaluates a
  control's targets with the same summing rule the Session uses, from the snapshot.
- Everything else waits for the control thread, which the MIDI thread wakes at once rather
  than at its next tick: parameter targets (a plugin's parameters are set on the control thread,
  through its host, which for a plugin in a worker is a request to the worker), scenes, morphs,
  and faders that are not live. These skip the UI and the socket, but not the control thread.
- The control thread then applies the moves to the Session's model, as the latest value per
  target: control values, part levels and mutes (for the views, scenes, saving and undo, as
  playing a control is today), without writing the live slots again (a newer move may be on its
  way), unless the model's value differs from what the MIDI thread wrote and no newer move has
  come (a control rebased by something else moving its destination).
- The pickup state, the groups' positions (pages and rows) and the control touched last (for
  learn) are the engine's; `controllers.get` reads them, and the engine notifies clients when
  they change.
- The app keeps the UI: it shows the values, learn reads the control touched last from the
  engine, and values go back to devices (knob values, Mackie Control's LEDs, faders and names)
  from the app, as now. **Mackie Control stays in the app** for this step: its
  surface selection, touch state and feedback are one object; its faders become live gains like
  any fader, so they no longer compile plans, but still take the app's timer and the socket.
  Moving it into the engine is a later step.

Pickup, latching and momentary buttons, relative encoders, groups with pages, rows and modes,
the part the knobs follow, screen knobs and a Sound's own assignments behave as before: the
engine runs the same `ControllerMapping` and `resolveControls`.

**Values back to the UI and devices.** The UI lagged behind the wheel as well: the app learned a
control's new value from `graph.changed` and then read the whole graph again (`graph.get`), a
blocking round trip on its UI thread for every move. Now the engine pushes the values that
changed, and nothing else: each control's value, each part's level and mute, the master
(`controls.changed`, at most 60 times a second, the interval counted from the last one sent).
A level moved live is no graph change, so no client reads the graph for it (a mute still is,
since the views' wires show it). The app's cached graph takes the pushed values (no round trip);
Play shows them at its next tick, a display frame (16 ms); motor faders and knob values going
back to devices follow the same values, read as they change rather than five times a second.
**Target:** a client holds a move's value within one display frame (16.7 ms) of the engine
receiving the message, so the UI shows it by the frame after; measured in the same test as the
sound.

**Edits on screen.** A mute clicked or a fader dragged in the app took about a second to show
(measured 2026-11-02 on a converted Performance of three or four parts): each edit was
`parts.level` and `graph.commit`, each waited for its reply on the UI thread and then read the
whole graph and every plugin's parameters again, and the views showed the result at their next
timer, up to 200 ms later. The engine took 5 to 7 ms per call. Now the app's cache takes a part's
level and mute, and the master, the moment they are edited, and the requests go without waiting
for their replies (`LocalClient::send`): the server answers a client's requests in order, so a
later move never lands before an earlier one. The values sent stay over the cache until the
engine has answered them, so a `controls.changed` sent before the engine had them cannot show
the old value for a moment; a refusal is said as the edit's error, and the graph read again in
place of what the cache showed. A `graph.commit` after only such edits is sent the same way and
reads nothing back; a commit after any other edit still reads the graph, so `graphState()` is
current on return for structural edits. A mute is still a `graph.changed`, read once for the
views' wires. The app's `Engine` broadcasts a change after each edit and whenever the engine
changes the graph or the live values; the parts list, the Mixer and Play redraw at once (the
Mixer and Play when on screen), in every window showing them, rather than at their timers.
**Target:** the views show the move within one display frame (16 ms), and the UI thread does
not wait on the socket for it.

#### Live gains status

Built 2026-11-02 (`Graph::setLiveGain`, `AudioConnection::live`; the Session's `faderSlots_`,
`Wire::faders`, `fixed` and `live`). Moving a part's fader (`parts.level`), muting it, the master
(`master.set`) and a control whose targets are part levels or mutes write live gains and compile
no plan; `graph.commit` after them compiles none unless other edits are staged. A slot just given
to a part starts at its value rather than ramping to it. A recording's post-fader tap follows the
same ramp (`Recorder::Source::live`), so the parts' taps still sum to Main. A Performance loaded
works out its wires' faders from them built again aside, keeping the wires read, so saving it
again gives the same file. `control-parts` (`liveGains`) checks a logarithmic control on a part's
level (as "Drifting"'s mod wheel) reaching the level by the end of the next block for five
moves, each block's samples ramping from the last level to the new one, the model's level for
the views, a fader, a mute and the master through the control API, an empty bus's fader
multiplied with the part's, a loaded Performance's faders, all with no plan compiled
(`Graph::plansCompiled`). Checks elsewhere that read a level right after a move now read the
block's last sample, since the move ramps across the block.

A part's pan, a send's level and a scene's glide followed (2026-11-05). Each part has two more
slots, its pan's left and right balance (`panSlots_`), and each send one (`sendSlots_`); a part's
fader wire into a stereo input takes the pan slot for its channel beside its faders and the
master's, and its own send wire the send's slot, with `Wire::unsent` its level apart from the
send. A pan is live where the wire's pan is the part's alone (no lane's or empty bus's added to
it); a send where its routes are its own (a send into an empty bus whose routes merge with the
part's own, or an empty bus's send, stays baked); the pans and sends of an empty bus on another
wire's way stay baked, as the slots run out past 253 do. `parts.pan` and a send's level moved
then write their slots and compile no plan, and `controls.changed` carries each part's pan. A
scene's edit (`Session::editMix`) checks what changed: when only live levels, mutes, pans, send
levels and the master did, as in each step of a glide or a morph, it writes the slots and
compiles nothing; a plugin switched on or off, a solo, a lane or a user wire compiles as before.
`control-parts` (`liveGains`) checks a pan heard by the end of the next block and among the live
values, a send's level into a bus with a box, and a 40 ms glide moving through the values between
to the scene's, with no plan compiled (before: one for the pan, one for the send, 61 for the
glide's 60 steps). `control-controller-input` (`panLatency`) measures a morph control moving the
pan between two scenes and a scene's 20 ms glide on one part, under heavy load on the
development Mac (load average about 95): the morph's pan heard 2 to 5.5 ms median and about 10 ms
worst after the move (2.3 to 8.2 and 13 ms before); a glide's first step heard 4.5 to 4.8 ms
median and 7 to 9.5 ms worst (4.6 to 4.8 and 10 to 13 ms before), its end 24 ms median after the
select (25.3 before). With one part the plan compiles quickly, so most of what remains is the
service tick a glide step waits for and the block. Not yet: a parameter target played by a
controller still waits for the control thread, which a load can hold up: the MIDI thread would
have to enqueue to the plugin's host itself, whose queue has one producer, whose parameter list
the main thread may rewrite, and which for a plugin in a worker is a request to the worker.

#### Screen edits status

Built 2026-11-02 (`LocalClient::send` and `wait`; `RemoteControl::setPartLevel`, `setMaster`
and `commit`, `takeError`). `desktop-gaps` checks, through a tap that counts the requests and
can hold a method's: a level and a mute edited with `parts.level`'s reply held return at once
and show in `Engine::graph()` and `liveValues()` before any reply, and stay after the engine's
answers and pushes; a mute reads the graph once and a level not at all; a drag of 60 moves
reads no graph and ends where it was left; a refused move shows until the refusal arrives, which
then shows as the edit's error with the engine's level back. Measured on the development Mac
(ten runs, the test's engine polling its socket every millisecond rather than every 5 ms): the
state 16 to 24 µs after the edit, the engine's `controls.changed` 0.3 to 1.7 ms after it. The
desktop's parts list, drawn off screen, shows a mute made elsewhere within 5 ms of message
loop, four times running (`Engine` is a `juce::ChangeBroadcaster`; `DesktopShell`, `PartsView`
and `PlayView` listen). The Mixer and Play listen only while on screen, which no headless test
reaches, so their redraw is unchecked.

A mute's `graph.changed` read the whole graph and every plugin's parameters again on the UI
thread after the mute showed: one blocking round trip per node, 23 waits and 126 ms on a
Performance of 20 plugins served on the engine's 5 ms timer. Now `RemoteControl::service()`
starts that read and returns: `graph.get`, then the nodes' `params.list` and the values, several
requests out at once, their replies taken by later calls, and the cache swapped for the read
once it is all in (`Engine` then broadcasts a change). An edit's own read back waits as before,
sent the same way, so it takes a few of the engine's ticks rather than one per node.
`control-local-stream` checks a graph changed by another client is read with no wait and shows
the node and its parameters; `ui-waits` (the 20 plugins, the server polled every 5 ms) measures
a mute and a drag of 60 fader moves at 0 waits on the UI thread.

The views' polls still waited when their cache expired: the meters twenty times a second, the
undo names, the assignments, the takes and the open Performance four times, and a plugin
window's availability when a view was built (280 waits and 1.3 s of every 2 s on the 20
plugins). Now `Engine` answers them from what the engine last said and asks again without
waiting (`RemoteControl::fetchPartMeters` and the like, taken by `service()` at its timer, which
now runs while a job does too, as nothing in it waits). One not asked for in a second (no view
polls it) is still read at once, so a caller asking once is not shown an old value; after a job
the reads it may have changed are asked afresh and the replies to those sent before it dropped.
A plugin window found later broadcasts a change and the Parts view builds again with its button.
`control-local-stream` checks reads sent and taken without a wait, one of a kind out at a time
and a forgotten one never given; `ui-waits` measures two seconds of the views' polls at 0 waits.

Opening a Performance cost the app 289 ms after the engine's answer on the 20 plugins (from
the load's reply to `Engine` showing the new graph and name, with no job left), 28 waits and
150 ms of them on the UI thread: the job's read back, then the UI's own read of every node, one
round trip each, and the pad map. With the reads above it was about 150 ms: each step of the UI's
read waited for the next 16 ms timer tick. Now `Engine` looks every 2 ms while a graph read is
out (`RemoteControl::reading()`), and the pad map after a load is fetched like the polls.
`ui-waits` opens one Performance of the 20 plugins and then the other: 45 to 63 ms after the
engine's answer in ten runs, with no wait on the UI thread (131 to 148 ms from asking, which
adds the job's own connection and reads, the estimate and the engine's load).

Every request still waited for the engine's 5 ms socket timer, up to a tick each: about 3 ms
median and 6 ms worst a round trip, and an edit reading the graph back took one tick per window
of requests. Now the engine answers a request as it arrives (`LocalServer::watch` wakes its main
thread) and sends what changed after each service tick, with no socket timer
(`hosts/engine/Pump.h`). `engine-pump` measures 0.03 ms median and 0.1 ms worst a round trip,
another client's change heard within 0.1 ms, and no rise in idle CPU (about 1% in its process,
as before). In `ui-waits`, served the same way, a rename (a structural edit, which reads the
graph and the 20 plugins' parameters back before it returns: 48 waits) waits 2.5 to 2.9 ms in all
on the UI thread rather than 84 ms, and opening a Performance shows 20 to 28 ms after the engine
answered (33 to 39 ms from asking) rather than about 50 (120).

A plugin knob dragged still waited on every move: `params.set`, and `params.text` for each new
value's text (120 waits and 31 to 43 ms on the UI thread for a drag of 60 moves in `ui-waits`).
Now `RemoteControl::setParameter` is a live edit like a fader (the cache shows the value at
once, the newest sent stays over the engine's pushes until answered, a refusal comes through
`takeError()`), and `Engine::paramText` asks the engine for a text it has not cached without
waiting (`RemoteControl::fetchParameterText`, one out per parameter, the latest value asked next),
showing the parameter's last text until it comes and broadcasting a change then. `ui-waits`
measures the drag at 0 waits, ending at the value left and the engine's text for it.

A pan, a solo and a scene selected each waited for their edit's reply and a read of the graph
and the 20 plugins' parameters back (48, 48 and 24 waits; 6 to 8, 6 to 7 and about 4 ms on the
UI thread in `ui-waits`). Now they are live edits too (`RemoteControl::setPartPan`,
`setPartSolo`, `selectScene`): the cache shows them at once and keeps them until the engine has
answered, and the engine's `graph.changed` read confirms them. `ui-waits` measures each at 0
waits, shown on return and still after the engine's answer. Structural edits keep reading the
graph back before they return, now at the engine's round trip rather than its timer (above).

#### Engine mapping status

Built 2026-11-02 (`oscairn/controller_input.h`; `controllers.set`, `controllers.view`,
`controllers.get` and the `controllers` topic). The engine's `AudioEngine` hands each controller
message from its MIDI input threads to the Session's `ControllerInput`, which maps it with
`ControllerMapping` against its snapshot and wakes the control thread (`juce::AsyncUpdater`,
`Session::serviceControllers`: the moves to the model and the parameters to their plugins, and
nothing else of `service()`). The control's evaluation is `Session::drive`, the summing rule
taken out of `driveMacro` so the Session and the controller input share it. The snapshot keeps,
over the model's, only the values a MIDI thread set since the control thread last took the moves,
so a value changed elsewhere (on screen, a scene) is picked up again as before. The app's
`Controllers` keeps matching ports, Mackie Control and the values sent back; it sends the engine
the joined groups and the ports (`Engine::setControllers`), and Play's part and page and the
editor page shown (`setKnobPart`, `setControlPosition`, `showScreenPage`); the engine's positions
and the control moved last come back for learn (`Engine::readControllers`).

`control-controller-input` checks, with MIDI given to the Session as the MIDI thread would: a
fader picked up at its value and then heard at the next block before the control thread runs
(the model catching up when it does), learn's control, another input's message ignored, pickup
waiting after a move on screen until the fader crosses the new value, the mod wheel's control
setting a part's level by its logarithmic curve at the next block and the control's value in the
model, a latching button muting and unmuting at the next block, a momentary one toggling on its
presses only, no plan compiled for any of it, a relative knob stepping a parameter up three and
down two, a row's number moving its group, the screen moving it back, and a missing group
refused. `control-physical-controls` still checks `ControllerMapping` itself, unchanged.
Not yet: Mackie Control in the engine; nothing checks the app's `Controllers` against a real
device, and the latency on hardware is unmeasured.

#### Latency status

Measured 2026-11-02 by `control-controller-input` (`latency`), on the development Mac with the
release build: the engine's threads as they run (an audio thread rendering 128 frames at 48 kHz
paced by the clock, the control thread polling the socket and servicing every 5 ms and woken at
once by a move, a MIDI thread, and a `RemoteControl` client over a socket pair), 60 moves of the
mod wheel 40 to 43 ms apart, each playing a control whose target is a part's level
(logarithmic, .707 to .0021, as on "Drifting"). From the MIDI thread handing the message over:
to the end of the first block whose output has the new level, median 1.45 ms, worst 3.20 ms (a
block is 2.67 ms); to the client's cached value, median 2.90 ms, worst 5.21 ms. The test fails
past a median of two blocks or a worst of 10 ms for the sound, and past one display frame for
the client. Before this work the same move took 28 to 70 ms to be heard in the running app.
The figures leave out the MIDI driver before the engine and the audio device's output latency
after the block; they vary run to run with the machine's load (an earlier run: 1.62 and 3.05
ms). `Engine::liveValues` gives the app the pushed values; Play checks them each 16 ms tick and
shows a change without resolving the knobs again, and `Controllers` sends knob values and
Mackie faders back whenever they change. Not measured: the app's own frame on screen, a sweep's
values coalesced at 60 a second, and anything on hardware.

### Pads

Which note each pad plays is part of the Performance, not of the device: a **pad map** of
eight banks (A–H) of 16 pads, numbered from the bottom-left as on the KICK. Each pad has a
note, a MIDI channel (or the channel of the track being played), a velocity (as played, or
fixed) and how pressure is sent (poly aftertouch, channel pressure or not), as in the stock
KICK firmware's MIDI Control mode. Fills set the notes of a bank or all banks at once:
chromatic from a note (the KICK's own mapping is A01 = 0), a General MIDI drum kit, or in a
key with rows that continue or rise a fourth or a third, like chillbeast's layouts and
Push. A pad's release ends the note its press started, even if the map or bank changed in
between. The pad editor shows the grid, plays a pad as it is selected, and applies every
change at once; "set all" copies a pad's channel, velocity and pressure to its bank.
Implemented 2026-10-31 (`pads.h`, the `pads.*` control methods and the desktop's **Pads...**
editor); a "learn" mode that takes a note from the keyboard is still to do.

### The control tree

Every assignable control is a **control**: a named value from 0 to 1 saved in the
Performance. A sound's Assign 1–8, a MODX-style Super Knob, a macro and a group knob are all
controls; physical knobs, faders, buttons, MIDI CC and sequencer lanes drive them.

- A control has **targets**. A target is a destination (a node's parameter, or a part's
  level or mute) or **another control**, which makes a tree: a Super Knob drives Assign
  1–8, which drive whatever each is set to.
- Each target has its own **start** and **end** (end below start reverses it, like the
  MODX's negative ratio) and **curve**: linear, exponential, logarithmic, S-shaped, a fader's
  taper in decibels, or a power law (below). A
  target is **absolute** (it sets the value) or **relative** (it moves the value around
  its base, the MODX's controller boxes).
- Controls form a tree without loops: an assignment that would loop is refused.
- **Several controls driving one parameter add up**, as the MODX sums the boxes of every
  source assigned to one destination (the summing rule, below). When two controls drive the
  same *control* (a Super Knob and Assign 3's own knob both move Assign 3), the control moved
  last sets it, as on the MODX, whose Super Knob moves the Assign knobs.
- Moving a control sets its targets through the ordinary parameter path, as sample-timed
  events. For sequencer lanes that must morph many parameters at an exact sample, the tree
  is flattened when the graph is committed into each lane's final targets with their
  combined ranges and curves (a later step).
- Scenes store control values as they store part faders.

**Curves** take the control's 0 to 1, `x`, to 0 to 1, which the target's start and end scale:
linear `x`; exponential `(e^4x − 1) / (e^4 − 1)`; logarithmic `ln(1 + (e^4 − 1)x) / 4`;
S-shaped `x²(3 − 2x)`; and, for levels (2026-11-01):

- **dB**, `{"kind": "db", "min": M, "max": X}` with −144 ≤ M < X ≤ 0: `x` moves evenly in
  decibels from M to X, giving the gain `10^((M + (X − M)x) / 20)`, and silence (0) at `x` = 0,
  as a fader's bottom is off. A part level is a gain, so with start 0 and end 1 a `partLevel`
  target at `x` plays the part at that many dB (`{"kind": "db", "min": -96, "max": 0}`: −48 dB
  at half way).
- **power**, `{"kind": "power", "exponent": E}` with 0.1 ≤ E ≤ 10: `x^E`. A level that follows
  a power law (E = 2: a square-law taper, −12 dB at half way).

**The summing rule** (decided 2026-11-01). A parameter, a part's level and a part's mute each
have a **base**: their value with no control moving them, one per destination, kept on each
of its targets (`base`). Each target on it contributes an **offset**, at its control's value
`x`, from its point `p = start + (end − start) × curve(x)`:

- a **relative** target offsets by `p − ½`: the middle of its range is no change, so
  start 0.5 and end 0.9 add nothing at 0 and +0.4 at 1 (a unipolar box), and start 0.3 and
  end 0.7 move ±0.2 (a bipolar one);
- an **absolute** target offsets by `p − base`: alone it sets the value to `p`, as before;
  beside others it adds how far `p` is from the base.

The destination is `base + Σ offsets`, clamped to its range (0 to 1 of it; a mute is on at
0.5 or more). A target that is off in the scene last selected adds nothing. Clamping is not
kept: the base survives it, so the value comes back as the controls go back.

- **A new destination's base** is its value when it gets its first target. Adding a target to a
  destination some control already drives keeps its base, as does editing a control's other
  targets.
- **A direct edit moves the base, not the offsets.** Anything but the controls that moves the
  value (the screen, a part's fader, undo, a scene's stored values, a glide or morph, the plugin
  itself) is taken as where the musician wants it with the controls where they are: the next
  time a control moves, the base becomes the value less the offsets, and the controls go on
  from there. With exactly one absolute target the base does not count (that target sets the
  value), so an edit lasts until it moves. The base may then lie outside 0 to 1, where the
  offsets reach past the range.
- **Scenes** store and restore the control values, then the parameter values and faders they
  hold, which have the last word: a stored value plays as stored, and the controls offset
  from it afterwards. Glides and morphs move them alike.

**Converter notes** (MODX and MONTAGE Performances, format version 11):

- **Controller boxes** (several sources on one destination) become one control per source, each
  with a relative target on the destination; they add up as on the instrument. A box moving the
  destination by up to `u` of its range (at most 0.5) is start 0.5, end 0.5 + `u` when unipolar
  (0 to +`u`), and start 0.5 − `u`, end 0.5 + `u` when bipolar (−`u` to +`u`); a negative ratio
  swaps start and end. The curve's shape goes on the target. The parameter's stored value is the
  base.
- **Part Volume** on a knob or slider is a `partLevel` target with a `db` curve
  (`{"kind": "db", "min": M, "max": 0}`, M the taper's floor) or a `power` curve, whichever
  matches the instrument's table, in place of the nearest of the four older curves.
- **Aftertouch, breath and Assign Switch 1 and 2** are assigned by identity: `aftertouch:1`,
  `breath:1`, `button:1` and `button:2`, as Performance overrides or a Sound's own assignments;
  a device without that source keeps the assignment and plays it when one is connected.
- **Performance Volume and Pan** go to the `master` level and pan instead of being folded into
  each part; a scene that stores the mix stores them.
- Any physical control on any device can be bound to any control, so a controller without
  a Super Knob gets one by binding a knob to the top of a tree. Pickup, relative encoders
  and feedback stay with the controller-mapping service.

Implemented 2026-10-31 (as `Macro` in `parts.h`): controls with parameter, part level, part
mute and control targets, each with its range, curve and relative mode; loops refused;
last-moved wins; scenes store control values; saved in the Performance; `controls.*` in the
control protocol; a **Controls** page in the Parts view; and panel layout knobs bound as
`control:NAME`. `control-parts` checks a Super control driving an Assign control (with an
exponential curve) that drives a gain and a reversed part level, a relative target, loops,
scenes and saving. The summing rule, 2026-11-01 (`Session::driveMacro`, Performance format
version 11): `control-parts` checks two controls on one parameter adding up, the sum clamped,
the base surviving the clamp, a direct edit moving the base under the offsets, and an absolute
target beside a relative one. Not yet: flattening the tree for sample-accurate sequencer lanes,
default Assign controls from each sound, MIDI CC learn, and part faders as direct gain
updates rather than a new graph plan per move.

### Overrides

Any physical control can be assigned to anything in the performance: a parameter of any
node, a control (above), a mix level, a send, a bypass, a scene, or the transport. Ways
to assign:

- **learn:** move the control, then touch the target on screen;
- from a target's menu;
- in a mapping list per performance.

Overrides are stored by logical control identity. Opening a performance on a device that
lacks a control falls back to the defaults for the others, and the performance reports any
override it could not apply.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Model | Part and wire ownership, implicit Mix nodes, port-level routes with channel rules, saved in the Performance format | Edits through the part views never remove user wires; replacing a plugin keeps or flags every wire; round-trips through save and load |
| 2. Parts and Part views | Parts list, chain, zones, scenes; desktop and device | A two-layer split and a shared reverb built without opening Routing |
| 3. Split, join, buses | Split and join nodes, Mix with mute and solo, shared buses, Mixer | Rendering identical to the same graph wired by hand |
| 4. Routing view | Nodes and wires, part wires shown and hidable, user wires, badges | Every structure from steps 2–3 appears; wiring a second source creates a shared row |
| 5. Editors | Knob pages for every plugin, structured-editor description draft, Modular grid as the first case | Basic Synth, Sampler and one third-party plugin editable on the device without a plugin window |
| 6. Physical controls | Default layers, overrides, learn, banks per control group | A new performance with three sounds has working knobs, faders and buttons with no programming |
| 7. Groups, pedal view, move/copy/share | | |

## Step 1 status

Implemented 2026-10-31 in `core/control` (`parts.h`, `src/parts.cpp`) and the Performance
format, version 2:

- **Wires** are kept by the Session: audio wires port to port, note wires node to node, each
  with its owner (a part, or the user) and its source's level, pan, mute and solo. Every
  edit compiles them into the graph's channel routes and note connections, changing only
  what differs. The existing connection calls (node to node, channel ranges, notes) make
  user wires, so the control protocol and the desktop are unchanged.
- **Channel rules** as above: equal counts one to one, mono to stereo copied with a pan,
  stereo to mono summed at −3 dB, anything else refused unless channel pairs are given.
- **Implicit Mix:** the wires into one input port are its Mix. Solo silences the unsoloed
  sources, mute silences a source even when soloed. It compiles into route gains, so no
  audio node is added; the Routing view can still draw it.
- **Parts:** an input (notes, an audio input port, or none for a generator), a chain of
  nodes and a graph output port. Adding, inserting, removing, reordering and removing a
  part rebuild only that part's wires, keeping the Mix settings of wires it recreates.
- **Replacing a plugin** moves its wires to the new plugin's ports in the same role (main to
  main, the other ports in order) in the same commit. A wire with no counterpart stays,
  unconnected; a main-port wire reconnects when a plugin with a main port returns.
- **Removing a node** either removes its user wires or reconnects its user sources to its
  user destinations.

Verification: `control-parts` checks that inserting, reordering, removing from a chain and
removing a part keep every user wire; the channel rules, pan, level, mute and solo against
computed output; replacement keeping or flagging every wire; reconnection on removal; and
that parts, wires and Mix settings round-trip through save and load, play identically, save
the same manifest again, and that a version 1 document is refused. Deliberately breaking
the −3 dB sum or letting part edits remove user wires makes it fail. `plugin-selection` now
expects incompatible replacements to be accepted with flagged wires. All tests pass on
macOS, under AddressSanitizer and ThreadSanitizer, and in the desktop build; the manifest
validates against the schema.

Parts and wires are in the control protocol (`parts.*`, `wires.*`, `graph.get`, the remote
client), tested end to end over the local socket (`control-remote-parts`) and against the
protocol schema with a live `graph.get`.

**Zones** followed (step 2's model): note wires and part inputs carry up to eight zones,
each a key, velocity and MIDI channel range with a transpose and a velocity curve or fixed
velocity. The graph applies them where it routes notes; each accepting zone plays its own
copy (layers), and held notes keep their releases when zones change. Tested in
`note-zones` (split, layer, MIDI and CLAP notes, velocity, channels, editing while held),
`control-parts` (a two-part keyboard split moved while notes are held) and over the socket.
Zones also carry a source mask, which a part's device selector sets
([RFC 0012](0012-sync-external-midi-and-multi-device.md#choosing-devices-selectors)).

**Shared buses** followed: a bus is a part with input kind `bus`, fed by other parts' sends
(each with a level) and outputs, with its own chain and output; loops are refused, and
removing a bus returns its parts to the main output. `control-parts` builds step 2's gate,
a two-layer keyboard split with both layers sending to a shared delay, from part edits only
and checks it renders sample for sample as the same graph wired by hand on the engine's
`Graph`.

**The Parts and Part views** followed on the desktop (`PartsView`, `PartsModel`), opened by
**Parts...** over the main window so they fit a device's single screen. Parts lists each
part (where it plays from with its zone over the keyboard, its chain as role-colored boxes,
level, mute and a send per bus) and the shared buses under them; **+ Add part** and **+ Add
bus** make them. Tapping a part opens its chain (IN, the boxes with **+** between them to
insert any plugin the isolated scanner finds, OUT with its sends), where a box can be moved
or removed, and its keys, transpose, output, level, mute and sends. Parts have a level and
mute; sends are post-fader. `desktop-parts-model` builds step 2's gate (a two-layer split and
a shared delay) with only the views' edits; `desktop-pad-editor` renders both views at
1024 × 600 against a session over the control socket.

**Scenes** followed: a scene first recorded every part's level and mute and a value for each
scene parameter chosen from a box's **Scene controls...**; it now records the full model
([status](#scenes-status)). The Parts view's scene buttons switch scenes; **+ Scene** and **Store**
capture the current state. Switching changes route gains and sends parameter events, so held
notes and tails go on; `control-parts` switches six times while a note is held and checks
there is no gap. Scenes now also glide, launch on the beat and morph ([status](#scenes-status)).

Not yet: bypass in scenes (with the pedal view), the device's physical controls for these
views, groups and undo.

### Scenes, the full model (decided 2026-10-31)

What a scene can hold, all of it by default:

- **Enabled or disabled, per wire and per block:** every wire and every node in the
  Performance can be switched on or off, and a scene records that for all of them. Switching a
  part between two instruments, or a pedal on and off, is a scene enabling one block and
  disabling another. It is seamless, on the engine's draining and retiring
  ([RFC 0021](0021-sounds-performances-and-library.md#seamless-switching)): a disabled block's
  note connections drain, so notes held on it play on until released, with their releases,
  expressions and pedal, and its tail rings out; new notes go to what the scene enables.
- **Each part's mute and solo.**
- **The mix:** each part's level and pan, each send's level, each wire's Mix row, and the
  Performance's master level and pan.
- **Plugin parameters.**
- **Control mappings:** which mappings are active, so a knob can drive the filter in the
  verse and the delay in the chorus.

What each scene saves is the user's choice: a scene has a scope, the kinds above it records,
and a kind it does not record is left as it was when the scene is selected. The default scope
is everything.

For advanced users:

- **Per plugin,** which parameters scenes save, and which they never touch (a sample's
  tuning, an output level the user sets by ear). The default is every automatable parameter.
- **Per mapping,** the scenes it is active in, so mappings can be laid out across scenes as a
  table rather than captured one scene at a time.

Built, with the scope, as the status below says; the desktop's controls for it are next.

### Scenes status

**Plugins and wires switched off and on** implemented 2026-11-01 (`setNodeEnabled`,
`setWireEnabled`; `graph.enable`, `wires.enable`). An off plugin's note connections drain:
notes it holds play on until released, with their releases and pedal, and new notes go only to
what is on. Sound into it stops; its own sound goes on (a tail rings out) until it is idle, by
the engine's retiring count: no note sounding, and asleep or three seconds of silence, or 30
seconds without a note. In a part's chain, what reached an off plugin also passes around it to
what follows, so an effect off is bypassed and an instrument off lets the notes go on. An off
note wire takes no new notes; an off audio wire is silent at once. Switching is an edit (undo
says "Turn off ..."), saved in the Performance and Sound (format version 4: `enabled` on nodes
and wires); a plugin loaded off has nothing to ring out. Routing draws wires on an off plugin
disabled, or draining while it still sounds, and a part with such a plugin takes the draining
tint. `control-parts` checks a held note playing through the switch and released after it, a
new note reaching only what is on, ringing out after three seconds of silence, undo, the
bypassed effect, the off note wire and the round-trip.

**Scenes record all of it** implemented 2026-11-01. Storing a scene captures which plugins and
wires are off, each part's mute and solo, the mix (each part's level, pan and sends, each split
lane's Mix, each user wire's Mix row), every parameter scenes touch, and the controls' values.
Scenes touch every automatable, writable parameter except those set aside (`setSceneUntouched`,
`scenes.untouched`; a box's **Scene controls...** unticks them) and a split's own nodes, which
follow its kind. Which control mappings are off in a scene is kept on each mapping
(`MacroTarget::offIn`); the mappings off in the scene last selected that applies mappings are
skipped. Each scene has a scope (`setSceneScope`, `scenes.scope`): selecting it applies only those
kinds and leaves the rest as they are; the default is everything. Wires are named by what they
join and their place in their part, as part wires are rebuilt under new IDs. Selecting sends only
the parameter values that differ, past the pending commands' limit if need be (each parameter
keeps at most one waiting value, so they never outnumber the parameters; a plugin takes what it
can each tick). Removing a plugin, loading a Sound and importing a Performance carry all of it
(an import's scenes keep what was here as it is now, and this Performance's scenes keep what
arrives as it arrives). `control-parts` checks the scenes switching plugins with a held note
ringing out, a mapping off in one scene and on in another, parameters set aside and taken back,
a scope of mutes only, a scene applied past a full pending store, and the round-trip;
`control-performance-format` checks scenes against both schemas.

**The desktop's controls** implemented 2026-11-01. A box's panel in the Part view and a plugin's
in Routing have **Turn off** / **Turn on**; an off box is drawn faint and marked OFF, and a
collapsed part's chip says "off". Each source row of an input in Routing has **On**, switching
that wire. **Scope...** beside **Store** ticks the kinds switching to the selected scene changes
(plugins and wires on or off; mutes and solos; levels, pans and sends; parameters and controls;
control mappings). A control's target row has **Scenes...**, ticking the scenes the mapping works
in. **Scene controls...** ticks the parameters scenes touch. `desktop-parts-model` checks an off
plugin's box and Routing chip; the rest was checked by rendering, not by clicking through.

**Transitions** implemented 2026-11-01 (`setSceneTransition`, `scenes.transition`, the
Performance's scenes' `glide`, `curve` and `launch`; a `morph` control target). Each scene has a
glide (0 to 60 seconds), a curve (linear; ease in and out; equal power, which moves levels by
their power so a crossfade does not dip) and a launch (at once, or at the next beat or bar while
the transport plays, 4 beats to the bar, checked at the service tick). Selecting a scene with a
glide captures where everything is and moves levels, pans, sends, lane and user-wire Mix rows,
control values and smooth parameters along the curve in audio time, one step per service tick,
ending exactly on the scene; what cannot glide (plugins and wires on or off, solos, stepped
parameters) changes at the start; a part, lane or user wire muted on one side fades from or to
silence and is muted at the end with its level kept. Moving a part's fader while it glides stops
the glide there. A control with a `morph` target moves between two scenes by hand, what cannot
glide switching halfway, and the control keeps the place it was moved to. A glide and a waiting
scene both pause while graph edits are staged, rather than committing them. On the desktop:
**Transition...** beside **Scope...** (glide, curve, launch), and **Morph between scenes** in a
control's **+ Target**. `control-parts` checks a linear glide halfway and at its end, an equal-power
fade in, a mute fading out, a fader stopping a glide, a launch waiting for the beat, a morph
before and after halfway, and the round-trip; removing a scene removes morphs to it.

**The mappings by scene** implemented 2026-11-01: **By scene** on the Controls page shows every
control's mappings as one table, a row per mapping and a column per scene, ticked where the
mapping works (its `offIn`, the per-mapping table this RFC asks for advanced users). Checked by
rendering, not by clicking through.


## Step 3 status

**The master** (2026-11-01, Performance format version 11): the Performance has a level (a gain,
0 to 2) and a pan (a balance) on its main output, the first audio output port, applied to
everything that plays there (each part's and bus's fader wire and each user wire into it) as
route gains, like the parts' faders. Saved (`master`), set through `setMaster` (`master.set`),
shown in `graph.get`, an edit undone like any other, and stored by scenes with the mix, gliding
with them (by power on an equal-power glide). The Mixer and Play end with a **Master** strip: its
fader and, in the Mixer, its pan and level in dB; it has no meter yet. `control-parts` checks the
master halving the sound, a pan to the right, refusals, a scene restoring it and one without the
mix leaving it, undo, and the round-trip with the scenes'; `control-performance-format` checks
`master.set` and the saved `master` against both schemas and refuses a level past 2.

**Splits** implemented 2026-10-31 (the first slice of step 3). A part's chain may hold a
split: lanes, each its own chain with zones and a Mix (level, pan, mute, solo), joining
again by summing into whatever follows the split, or where the part plays. Notes reach each
lane through its zones after the part's own (`intersectZones` composes them: transpose and
velocity curve included), and every lane gets a copy of the sound. That covers layers, key,
velocity and channel splits, and dry/wet parallel effects. A lane may hold another split,
and a shared bus may split: sound sent to it enters every lane, at the lane's Mix.
The Session compiles splits into ordinary graph routes and note edges, so nothing new runs
on the audio thread. Splits are saved in the Performance, in the control protocol and the
remote client.

On the desktop's Part page, **+** offers "Split here"; a split shows its lanes stacked
between a split pill and a join pill, each lane with its own **+**, its keys and its level.
Tapping a pill offers Add lane and Remove split (the first lane's plugins stay); tapping a
lane shows its keys, level, mute, solo and Remove lane.

`control-parts` checks the gate: two zoned layers inside the part's zone joining into one
effect, and a dry lane beside a wet one, render sample for sample as the same graphs wired
by hand on the engine's `Graph`, also after saving and opening again. It also covers solo,
mute, nesting and edits; `desktop-parts-model` covers the views' edits and
`desktop-pad-editor` renders a split.

**Left/right, mid/side and band splits** followed. They divide the sound itself with two
plugins in one bundle (`plugins/splitters`): **Split** (one stereo input, four stereo
outputs) and **Join** (four stereo inputs, one stereo output). Their ports never change, as
CLAP requires; a mode parameter decides what flows through them, so one pair of plugins
serves every kind. Left/right and mid/side have two lanes and a Join; bands (Linkwitz-Riley
crossovers, 24 dB per octave, with allpass compensation so the bands sum flat) have two to
four lanes and join by summing. The Session owns a split's nodes: it sets their modes and
band count, keeps them out of chains, and removes them with the split. Every lane still gets
the notes. The Part page's **+** offers each kind; a band split shows its crossovers.
`splitters` checks that a Split and Join give back their input and that bands sum flat and
separate; `control-parts` checks a left/right split with a delay on one side renders exactly
as wired by hand, also after saving and opening, and mid/side and band lanes.

**The Mixer** followed: **Mixer** on the Parts view shows a strip per part and bus, with its
sends, pan, a fader (−12 dB halfway up), its level in dB, mute, solo and a meter; a strip
another part's solo silences fades, as a muted one does, and so does its Parts row. Parts gained
solo: while any part is soloed, only soloed parts, the parts that feed them and the buses
they feed are heard, so a soloed part keeps its reverb bus. The graph records each node's
largest output sample; the Session turns these into each part's peak after its fader, held
and falling by about 17 dB a second, and the Mixer reads them 20 times a second
(`parts.meters`). A part with several lanes shows its loudest lane, not their sum. A bus with
no box that takes sound has no wire of its own: the part wires passing it record their part's
fader and each such bus on their way, with their level, pan and mute up to there, and the part
and each bus meter those. Two ways from a part to one place (an empty bus's output, and its send
to another empty bus that plays there too) are one wire at their summed level.
`control-parts` covers solo through sends and buses, the meters and persistence; the socket
test and `desktop-pad-editor` the rest.

Not yet:
- **Mixer details:** meters that sum a part's lanes.
- **Nested splits in the views:** a split inside a lane shows as a gap there.
- **Smooth crossover changes:** a new crossover frequency applies at the next block.

## Step 4 status

The **Routing** view implemented 2026-10-31 on the desktop (`PartsView`, `readRouting`,
`layoutRouting` and `inputsOf` in `PartsModel`), opened by **Routing** on the Parts view. It is
laid out by signal flow (layered): each box one column after what feeds it, inputs first and
outputs last; a wire that skips columns keeps a slot in each it crosses; each column is ordered
to cross fewer wires; each wire leaves and enters at its own place on a box's edge. Curves run
only in the gaps between columns, so no wire passes under a box (`desktop-parts-model` checks
this). Nothing is dragged into place. Parts are **collapsed** by default, each one box with its
plugins as chips and a button to expand it in place, as a frame holding its plugins laid out
the same way; the choice is remembered (`routing-view.txt` beside the theme selection), and
**Parts collapsed** off shows every part expanded. Choosing a plugin or part keeps its wires and
what they reach bright and dims the rest. It draws every node and every wire in the theme's wire colors (theme format 4, now 5): enabled audio and note wires,
part wires fainter, wires drawn here bright; disabled ones (muted; off in a scene
with the full scene model) and draining ones (seamless changes) dashed in their own colors;
unconnected ones apart. An audio wire is a line per channel its source sends (the source
channels its pairs take), side by side like one wire split in two or more, up to eight. With
**Meters** on, an audio wire carrying sound takes its level's color, after its level and pan,
in the Mixer meters' zones: green, yellow from -12 dB, red from -3 (`wires.meters`, held and
falling like the part meters; both read the plugins' peaks once a tick). A part's row in the parts lists takes a tint when muted (the mute's)
or draining (the accent's). Tapping a node offers **Connect audio to...** and
**Connect notes to...** (user wires, main port to main port), **Remove** for a node outside the
parts (its sources are wired to its destinations), or **Open part**, and lists each of its
inputs with a row per source: that source's level, mute and solo in the input's implicit Mix,
and **Disconnect** for a user wire. **+ Plugin** adds a node outside the parts.

The part views show what Routing did: a box with user wires has a badge (a split's chip
carries its lanes'), and **Routing...** for a selected box opens Routing there. A node outside
the parts that several parts are wired into is a **Shared** row under the buses ("Shared ·
Hall, fed by Bass, Lead"), and each feeding part's OUT lists "→ Hall (shared)".

The gate, in `desktop-parts-model`: every node of a Performance with parts, a split and its
nodes, a bus and sends appears once in Routing with its part and column, and every wire;
wiring two parts' instruments into a reverb gives that reverb's input a Mix row per source and
makes it a shared row, and disconnecting one undoes both. `desktop-pad-editor` renders the
view.

Not yet: drawing wires by dragging, choosing ports other than the main ones, editing a
wire's channel pairs, naming a shared row, scrolling a Performance with more nodes than fit,
and the device's version of the view.

## Undo status

Implemented 2026-10-31 (`core/control/src/undo.cpp`). After each commit, and at each control
tick when nothing is staged, the Session compares its model (parts, wires, scenes, controls,
pads, parameter mappings, and its nodes, held) with the last recorded one; a difference is
a step. So a view's action, which ends in one commit, is one step, and edits that need no
commit (scenes, controls, pads) are steps too. Parameter values set through the control
interface are steps of their own, and repeated changes to the same thing within 1.5 s merge.

Undo applies only what a step changed, field by field, onto the model as it is now: undoing
a rename keeps a level a scene set since. Steps hold the nodes they removed or replaced, so
undoing a removal brings back the same plugin, with its state and prepared sounds, under its
ID. Switching scenes, moving controls and devices coming and going are not edits. History is
100 steps, and opening a Performance clears it. Each removed or replaced plugin it holds stays
in memory (a sampler's samples, a worker process), so it keeps at most eight of them, dropping
the oldest steps past that (from the 2026-10-31 audit). **Undo** and **Redo** are on the Parts views
and in the desktop's **Edit** menu, named for what they would do, and on Cmd-Z and
Cmd-Shift-Z (Ctrl elsewhere). `control-undo` covers
grouping, merging, removal with state, scenes and controls, pads, splits with their nodes and
loading; the socket test and `desktop-pad-editor` cover the protocol and the engine.

Not yet: undo of preset and state loads, which replace a plugin's
whole state outside this model; and a plugin's own edits in its window, which reach the host
as parameter changes it reports, not edits made here.

## Step 5 status

Knob pages for every plugin are [RFC 0019](0019-adaptive-ui.md#step-1-status)'s generated
editors, 2026-11-01: every CLAP plugin's parameters as pages from its module paths (or, for a
large plugin without any, from its names), drawn as tiles with the control each wants on the
Desktop and on a device's Panel, where the device's knobs play the line selected on screen,
and as lists for four encoders on the Compact test layout. A mode that hides controls redraws
the pages. `desktop-shell` checks the gate's plugins on all three: the Basic Synth, the
Sampler (which has no parameters yet, so nothing to edit there) and Odin 2 (258 parameters, 26
pages, checked where it is built beside the checkout); the bundled Delay, Transpose and Step
Sequencer were rendered and looked at. Two-valued parameters written 0 and 1 are switches
labeled Off and On. Not yet: what modulates each knob (parameter lanes, aftertouch), the
structured-editor description beyond `oscairn.ui`'s hints, and the Modular grid; the Sampler's
zones, sounds and envelope wait on its parameters and RFC 0019 step 5.

## Step 6 status

Started 2026-11-01 (`core/control/include/oscairn/physical_controls.h`,
`src/physical_controls.cpp`); control groups the same day. Assignments resolve per control
group of the device ([RFC 0024](0024-device-profiles.md#control-groups)) in the layers above,
each filling only what the ones above left empty:

1. **Overrides**, saved in the Performance (format version 8, `assignments`) by the control's
   logical identity, numbered across the device's assign groups: `knob:N` (knobs and encoders),
   `fader:N`, `button:N`, `wheel:N`, `strip:N`, `jog:N`, `pedal:N`, `footswitch:N`, and (format
   version 11) `aftertouch:N` (channel pressure) and `breath:N` (CC 2), sources like the wheels:
   passed to every part as their standard controller until something is assigned. **Buttons**
   (version 11): the assignable switches (a buttons group with `target: switches`, the MODX's
   ASSIGN 1 and 2) are `button:1` up, in group order, the same whatever other buttons a device
   has, and play nothing until assigned; the scene and mute buttons count on from `button:101`.
   On a device with fewer switches, an override of `button:N` beyond them plays nothing and is
   kept, so it plays again on a device or controller that has that switch. Each sets a
   parameter, a control, a part's level or mute, or a scene. Screen knobs are not overridden.
   An override whose target is gone falls through to the defaults and is left out at save.
   Assigning is an edit, undone like any other.
2. **The sound's own assignments**: assignments with a `part`, saved with the Performance and
   with the Sound (version 6, `assignments`), each to one of the part's plugins' parameters or
   its controls; they play when their part is selected, and for a control with a function (the
   mod wheel) in their part whenever it moves. Then the selected part's controls, as Assign banks.
3. **Role banks**: Filter (cutoff, resonance, filter envelope, key tracking, then the filter
   envelope's stages), Amp EG (attack, decay, sustain, release, hold, velocity, key tracking,
   level), EQ (low, mid and high gain and frequencies, mid Q, output) and Levels (element,
   operator or oscillator levels 1 to 8), each taking as many roles as its group is wide; a
   role the sound lacks is blank, and a bank with none of its roles is left out. Plugins
   declare roles in `oscairn.ui` (draft 3, a `role` per parameter hint); a plugin declaring
   none gets roles from its parameters' names ("Cutoff", "Resonance", "Attack", an "Attack" in
   a module named for the filter as the filter envelope's, "Wet" as `mix`), never mixed with
   declared ones. With no role bank filled and no controls, two Assign banks from the part's
   instrument's first parameters in `oscairn.ui` priority order.
4. **Conventions**: faders play part levels (parts, then buses), the Levels bank or the audio
   parts' levels by their mode; assign buttons play part mutes, or scenes (`target: scenes`);
   controls with `functions` (wheels, pedals) pass their standard controller to every part;
   with no part selected the knobs follow the Performance's own controls.
5. **The effects bank**: each effect's main control (its `mix` role, else its first parameter
   by priority that is not stepped, else its first) in chain order.

**Screen knobs** play what the screen shows: Play's knob row, or, while a generated editor
page shows (a Desktop or Panel `ParameterGrid`, or a Compact page), that page's first
parameters by priority, as many as the group has knobs, in the Editor layer under the page's
name. The editor gives the engine its page while it shows and withdraws it when hidden
(`Engine::showScreenPage`); the controller-mapping service reads it into the
`AssignmentSource` (`screen`), so a controller's screen group follows it, and back in Play the
screen knobs play the knob row again.

`resolveControls` takes the device's groups and each group's position (page, row or mode);
`groupPositions` says how many it has. `assignments.get` and `assignments.set` carry overrides
over the control protocol. Play's knob row shows the device's screen knobs, or where it has
none its first assign group of knobs (or, on the desktop and panel layouts, the layout's row
knobs, else four), each tile with its bank (or row, or Override); on six or more tiles the text
goes under the dial. A layout's `row:N` knobs play the bank shown. **Learn** (a top bar
action on a layout with row knobs): move a knob, then touch a strip (its level), its M (its
mute), a scene or a knob (its target); Default removes the override.

`control-physical-controls` checks each layer and banks sized to their group (six wide, with
Amp EG's sixth role), the KICK (four encoders paged, fixed knobs, wheel, pads, buttons and
strip resolving nothing), the MODX M (six screen knobs, four knobs in six rows, eight faders in
part, element and audio modes), an eight-knob device with assign buttons, overrides (and not on
screen knobs), refusals, falling through, undo and redo, and saving and loading; roles
(2026-11-01): names suggesting roles, the Basic Synth's and Delay's declared roles read through
the host, its role banks (four wide, Filter with Brightness as the cutoff and blanks after it,
Amp EG left out; eight wide, Amp EG with only its level), a fixture's "Cutoff" filling Filter
from its name, a declared role overriding names, and no roles giving Assign banks; and screen
knobs playing an editor page's first parameters, and the knob row again without one. It also
checks external controllers (2026-11-01): a controller's knobs numbered after a panel's and
playing eight-wide banks, its faders part levels, its wheels passed through, scene and mute
buttons; the mapping service (`ControllerMapping`) picking up an absolute knob at and across
the value and losing it when the value moves elsewhere, a sign-magnitude knob stepping, a
fader, a scene button on the press, a momentary mute toggling, a row's own numbers moving its
group to that row, and messages from another device or channel ignored; each relative
encoding decoded; and a Sound's own assignments (the mod wheel playing its part's parameter, a
knob's when its part is selected, refusals of anything not its own, a Performance override
over every part's, saving with the Performance). `control-sounds` checks the mod wheel's
assignment saved with a Sound and replacing the part's own when it is loaded.
`desktop-shell` checks Play's knobs through the layers (the Basic Synth's Filter bank for a part
with no controls), learn, and six screen knobs giving six tiles and a six-wide bank, and the
Basic Synth's editor page, shown, giving six screen knobs its Brightness, Level and Oscillator
first, and hidden, none; `kick-clap-host` checks a declared role kept and an unknown one
dropped, and `control-json-protocol` the Basic Synth's roles in `params.list`;
`desktop-profile` checks the new profiles and the groups' refusals.
Snapshots of Play with four and six knobs, and the app with `--profile modx-m` at 800 x 480,
were looked at.

Roles and the editor page's screen knobs, 2026-11-01: the Basic Synth declares Brightness as
its cutoff and Level as its level, and the Delay its Wet as `mix`; Split, Join, Transpose and the
Step Sequencer have no parameter that fits a role, and the Sampler (its own repository) has
none yet to declare. A panel layout's own `row:N` knobs still play the line selected in an
editor (the first by default, which is its top parameters in priority order), as before.

Aftertouch, breath and switches, 2026-11-01: `control-physical-controls` checks aftertouch and
breath passing through with nothing assigned, switches numbered `button:1` and `button:2` beside
scene buttons from `button:101`, a device without switches having no `button:1`, channel pressure
(two bytes), CC 2 and a switch playing their overrides, and those overrides round-tripping. The
`modx-m` and `modx-m-usb` profiles gain the keys' aftertouch and the ASSIGN 1 and 2 switches.

Not yet: name matching beyond exact names (per-oscillator levels, other spellings); the
Sampler's roles; no panel layout binds faders, buttons or a row-switching group, so
those resolve but are not yet played, and row buttons have no layout binding; targets for
sends, bypass and the transport; overrides in a Performance import; MIDI CC learn; reporting
overrides a device could not apply.

## Play status

Implemented 2026-11-01 (`hosts/standalone/PlayView.h`, `PlayView.cpp`). The Performance app
opens in Play on both surfaces. On a device's screen it sits under the top bar, whose actions
are **Edit** (the app's main action, opening the parts views on the part selected in Play) and
**Save**; Edit's views have **Play** there instead, and their Done goes back to Play. On the
desktop, Play fills the window under its toolbar, and the toolbar's **Edit** shows the parts
list and tabs (and **Play** returns); `--view Play` names it.

- Scenes: eight tiles, one tap each; the first empty one adds a scene. Only the first eight
  show for now.
- Previous and Next at the ends of the scenes, while the open Performance is the Setlist app's
  current entry; they step the setlist.
- Knobs: a bank of four (eight on an eight-knob device) of the Performance's controls, each
  with its name, what it drives and its value; with more, a Bank button pages them. Tapping a
  strip's name selects its part, and a switch beside the knobs moves them to that part's own
  controls. A device's knobs bound `row:N` play the bank shown.
- Strips: one per part and bus, with its fader, a meter per channel, mute and solo, dimmed
  while muted or silenced; more than six scroll sideways.
- Drawn with the theme's sections, edges, sliders, rotary knobs and marks (no colors of its
  own), with text placed by its ink (`drawInked`, Theme.h).

`desktop-shell` renders Play in every built-in theme and both input modes, and the Performance
app at 1024 × 600 under its top bar, checking that it opens in Play, the knobs follow the
Performance's controls and switch to the selected part's, and Edit and Play swap the views.
The app's `--profile kick --app performance --snapshot` was looked at too. Not yet: the pads
selecting scenes in Play, a glide's progress on a scene's tile, and the strips' part colors
(parts have none yet).

## Open questions

- Structured editor descriptions beyond `oscairn.ui`'s hints and roles.
- Whether a part may send from the middle of its chain (sends are post-fader, from its end).
- How scenes store the bypass and mute state shown in the pedal view.
- The Ignite's physical controls, and default layouts for each device.
- How the desktop and device share an editing session when both are open.

## Ruled out

- Wiring every input and output by hand for common performances.
- A separate part model alongside the graph: the views must not drift from what plays.
- Container plugins that host plugins inside themselves (already ruled out in the
  architecture); groups are sub-graphs instead.
- Plugin-drawn windows as the default editor on the device.

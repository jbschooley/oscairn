# RFC 0011: Performance UI, routing views and plugin editors

Status: proposed, 2026-10-30. Not implemented. Builds on the graph, note routes and parameter
lanes that exist today ([architecture](../architecture.md#live-note-routes)), the planned
controller-mapping service ([physical controls](../architecture.md#physical-controls-takeover-and-direct-position-access-planned))
and the theme contract ([design language](../design-language.md)).

## Problem

Oscairn's graph can already route audio, notes and parameter lanes between any plugins, but
there is no way for a musician to build a performance on the device: no parts, layers,
shared effects, mixer or editors. Two familiar models each solve half of it:

- **Gig Performer** wires anything to anything, but every input and output is drawn by
  hand for every performance.
- **The MODX** organises a Performance as parts with zones, inserts and shared effects,
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

## The model behind the views

The graph stays the only model. The views are projections of it, plus a small amount of
layout data (part order, names, colours, which nodes are grouped), saved in the Performance.

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
  its neighbours or remove them.
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
- **Routing:** a Mix node (labelled with the bus name) in the effects and buses column,
  followed by the bus chain. Sends are part wires into the Mix node, each labelled with its
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

The colour of a box follows its main output (notes, instrument, effect). A second tag names
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
   plugin gets knob pages. Windows from worker processes ([RFC 0010](0010-plugin-worker-isolation.md))
   are an open question there.

The description format is a new Oscairn extension, defined in a later revision of this RFC.

## Physical controls

Controls follow the device: the KICK, the Ignite and a MODX-style keyboard have different
knobs, sliders and buttons. A device **layout** names its logical controls
([design language](../design-language.md)). The planned controller-mapping service owns
pickup, relative encoders and bank state.

### Defaults, so nothing must be programmed

Assignments resolve in layers. Each layer fills only what the layers above it left empty.

1. **Performance overrides** set by the musician (below).
2. **The sound's own assignments**, saved with the sound by its author: Assign 1–8 and
   named macros. They travel with the sound to another performance or device.
3. **Fixed role banks**, filled from roles the plugin declares (cutoff, resonance, attack,
   release, effect mix). They are the same on every instrument. A role the plugin lacks
   shows as a blank control rather than something unrelated. Third-party plugins without
   declared roles get Assign banks from their first parameters until someone maps them.
4. **Performance conventions:**
   - sliders control part levels;
   - buttons control part mute, or scenes;
   - knobs follow the selected part's banks;
   - the mod wheel, pedals and aftertouch are sent to every part as standard controllers.
5. **Effects bank:** the main control of each effect box in the selected part, in chain order.

Banks come in fours. A four-knob device pages through them (Assign 1–4, Assign 5–8, Filter,
Amp EG, Effects), and an eight-knob device shows two at once.

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
controls; physical knobs, sliders, buttons, MIDI CC and sequencer lanes drive them.

- A control has **targets**. A target is a destination (a node's parameter, or a part's
  level or mute) or **another control**, which makes a tree: a Super Knob drives Assign
  1–8, which drive whatever each is set to.
- Each target has its own **start** and **end** (end below start reverses it, like the
  MODX's negative ratio) and **curve**: linear, exponential, logarithmic or S-shaped. A
  target is **absolute** (it sets the value) or **relative** (it moves the value around
  the value it had when the control was last stored, the MODX's bipolar setting).
- Controls form a tree without loops: an assignment that would loop is refused.
- When two controls drive the same one (a Super Knob and Assign 3's own knob both move
  Assign 3), the control moved last sets it, as on the MODX.
- Moving a control sets its targets through the ordinary parameter path, as sample-timed
  events. For sequencer lanes that must morph many parameters at an exact sample, the tree
  is flattened when the graph is committed into each lane's final targets with their
  combined ranges and curves (a later step).
- Scenes store control values as they store part faders.
- Any physical control on any device can be bound to any control, so a controller without
  a Super Knob gets one by binding a knob to the top of a tree. Pickup, relative encoders
  and feedback stay with the controller-mapping service.

Implemented 2026-10-31 (as `Macro` in `parts.h`): controls with parameter, part level, part
mute and control targets, each with its range, curve and relative mode; loops refused;
last-moved wins; scenes store control values; saved in the Performance; `controls.*` in the
control protocol; a **Controls** page in the Parts view; and desktop layout knobs bound as
`control:NAME`. `control-parts` checks a Super control driving an Assign control (with an
exponential curve) that drives a gain and a reversed part level, a relative target, loops,
scenes and saving. Not yet: flattening the tree for sample-accurate sequencer lanes,
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
| 6. Physical controls | Default layers, overrides, learn, per-layout banks | A new performance with three sounds has working knobs, sliders and buttons with no programming |
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
part (where it plays from with its zone over the keyboard, its chain as role-coloured boxes,
level, mute and a send per bus) and the shared buses under them; **+ Add part** and **+ Add
bus** make them. Tapping a part opens its chain (IN, the boxes with **+** between them to
insert any plugin the isolated scanner finds, OUT with its sends), where a box can be moved
or removed, and its keys, transpose, output, level, mute and sends. Parts have a level and
mute; sends are post-fader. `desktop-parts-model` builds step 2's gate (a two-layer split and
a shared delay) with only the views' edits; `desktop-pad-editor` renders both views at
1024 × 600 against a session over the control socket.

**Scenes** followed: a scene records every part's level and mute and a value for each
scene parameter (the plugin parameters chosen to follow scenes, from a box's **Scene
controls...**). The Parts view's scene buttons switch scenes; **+ Scene** and **Store**
capture the current state. Switching changes route gains and sends parameter events, so held
notes and tails go on; `control-parts` switches six times while a note is held and checks
there is no gap. Values jump for now; smooth transitions are on the roadmap.

Not yet: bypass in scenes (with the pedal view), the device's physical controls for these
views, groups and undo.

## Step 3 status

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
sends, a fader (−12 dB halfway up), its level in dB, mute, solo and a meter. Parts gained
solo: while any part is soloed, only soloed parts, the parts that feed them and the buses
they feed are heard, so a soloed part keeps its reverb bus. The graph records each node's
largest output sample; the Session turns these into each part's peak after its fader, held
and falling by about 17 dB a second, and the Mixer reads them 20 times a second
(`parts.meters`). A part with several lanes shows its loudest lane, not their sum.
`control-parts` covers solo through sends and buses, the meters and persistence; the socket
test and `desktop-pad-editor` the rest.

Not yet:
- **Mixer details:** pan per part, strips dimmed when another part's solo silences them,
  and meters that sum a part's lanes.
- **Lane pan in the views:** it is set over the protocol.
- **Nested splits in the views:** a split inside a lane shows as a gap there.
- **Smooth crossover changes:** a new crossover frequency applies at the next block.

## Step 4 status

The **Routing** view implemented 2026-10-31 on the desktop (`PartsView`, `readRouting` and
`inputsOf` in `PartsModel`), opened by **Routing** on the Parts view. It draws every node in
its column (inputs, notes, instruments, effects and buses, outputs) with the part it belongs
to, and every wire: audio and notes in their own colours, part wires faint and hidable, wires
drawn here bright, unconnected ones red. Tapping a node offers **Connect audio to...** and
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
100 steps, and opening a Performance clears it. **Undo** and **Redo** are on the Parts views
and in the desktop's **Edit** menu, named for what they would do, and on Cmd-Z and
Cmd-Shift-Z (Ctrl elsewhere). `control-undo` covers
grouping, merging, removal with state, scenes and controls, pads, splits with their nodes and
loading; the socket test and `desktop-pad-editor` cover the protocol and the engine.

Not yet: undo of preset and state loads, which replace a plugin's
whole state outside this model; and a plugin's own edits in its window, which reach the host
as parameter changes it reports, not edits made here.

## Open questions

- The extension for plugin roles (cutoff, attack and so on) and structured editor
  descriptions.
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

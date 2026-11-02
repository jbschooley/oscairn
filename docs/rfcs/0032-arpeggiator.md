# RFC 0032: The arpeggiator

Status: proposed, 2026-11-01. Steps 1 and 3 built in the host (below). The plugin is built in its own repository,
`oscairn/arpeggiator` (MIT, private for now), from its plan (`doc/plan.md` there, read at
commit d00c3b6 and again before this RFC was committed). This RFC is Oscairn's side: where the
plugin sits, what the host must add for it, how conversion places it, and how it is tested.
Builds on the parts and plugin roles of [RFC 0011](0011-performance-ui-and-editors.md),
parameter lanes ([RFC 0002](0002-param-target.md)), the transport and clock out
([RFC 0012](0012-sync-external-midi-and-multi-device.md)), note capture
([RFC 0020](0020-recording.md)), the library and assets
([RFC 0021](0021-sounds-performances-and-library.md), [RFC 0009](0009-sound-assets-and-dependencies.md)),
editor descriptions ([RFC 0019](0019-adaptive-ui.md#plugin-editor-descriptions)) and the
measurement approach and IP rules of [RFC 0013](0013-awm2-class-sample-engine.md).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. SysEx between plugins: the graph and workers carry `CLAP_EVENT_MIDI_SYSEX`
- [ ] 2. The plugin in a part: phrase playback, state and push, hosted and tested in Oscairn
- [x] 3. A shared beat grid while the transport is stopped, so parts start together
- [ ] 4. Conversion: each Part's arpeggio settings and phrases placed by the converter
- [ ] 5. Measured against the reference: the most used settings, then every playback rule
- [ ] 6. User phrases: files, MIDI import and recorded notes, as library items
- [ ] 7. The phrase editor: a grid widget, operations that return data, and undo
- [ ] 8. Motion-sequencer lanes on the same engine (a later RFC)

## Problem

Jacob's MODX M Performances use arpeggios everywhere. Oscairn has a transpose and a MIDI delay
as note effects, and the KICK sequencer, but nothing that turns held keys into a pattern. The
architecture names the arpeggiator as an event plugin in a Sound's chain
([architecture](../architecture.md#sounds-performances-and-projects)), but no RFC says what it
must do, how conversion uses it, or what the host must add.

A plain up/down arpeggiator is not enough. A MONTAGE M / MODX M arpeggio is a **phrase**: a
looped pattern whose notes refer to the held chord, with tracks that play only for some
numbers of held keys, walkers, accents, random effects and style transposition. No
open-source arpeggiator plays chord-relative phrases, so it is built from scratch.

## Outcome

- One arpeggiator plugin does **everything a MONTAGE M / MODX M arpeggio does**: phrase
  playback with chord-relative events and walkers, and every playback rule in the plan's
  table (R1–R25), each measured against the reference.
- A converted Performance keeps its arpeggios: each Part gets the plugin at the front of its
  chain, with its settings and its eight phrases.
- Phrases are in an **open, documented, versioned text format** (`oscairn-phrase 1`, `.ophr`).
  Phrases from a Yamaha installation are converted on the user's machine and never shipped.
- Users make their own phrases: by hand, from a MIDI clip, by playing, and in an editor. They
  live in the library.
- The phrase engine can grow into a motion sequencer: lanes of values aimed at parameters.

Non-goals: bit-exact timing with the instrument (the target is the measured tolerance below),
and Yamaha's names in the product (RFC 0013's IP rule 5).

## Prior art

| Product | What it does | Relevance |
| --- | --- | --- |
| MONTAGE M / MODX M arpeggio | Phrase-based: up to 16 tracks per phrase, chord-relative and fixed notes, held-key ranges per track, 8 phrases per Part, playback rules set per Part | The target. Behavior from the Operation Manual and the Data List, then measured |
| Classic arpeggiators (up, down, random over octaves) | Play the held keys in order | One selection rule of twelve (the walkers, R5) |
| Logic's Arpeggiator, Bitwig's Arpeggiator | Pattern steps over held notes, with per-step velocity and gate | No chord-relative templates or held-count tracks |
| Open-source CLAP and LV2 arpeggiators | Step or ordered arps | None reads or plays chord-relative phrases (the plan's survey), so nothing to adopt |

These summaries are from the products' documentation as the plan describes them; they were not
rechecked for this RFC.

## Design

### The plugin

A CLAP note effect, `org.oscairn.arpeggiator`: one note input, one note output, no audio ports,
CLAP and MIDI dialects. Its parameters (the plan's section 4.3) carry stable IDs and never
change. Its state is text: every parameter, then each of the eight slots' phrase text
(`phrase-format.md`, "Plugin state" in the plugin's repository). A saved Sound therefore holds
its phrases and moves between machines with nothing else.

Real time follows the plan: fixed storage (4096 events per phrase, 16 tracks, 32 held keys,
256 sounding notes), no allocation, locking or I/O on the audio thread, and a seeded random
generator in the state so offline renders repeat exactly.

### In the part

RFC 0011 works out a plugin's role from its ports: notes in, notes out is a **note effect**.
The arpeggiator goes in a part's chain before the instrument, as the architecture's "Arp Pluck"
Sound shows. Nothing in the part model changes:

- **Only that part is arpeggiated.** A layer on the same keys without the plugin gets the notes
  directly.
- **Zones apply first.** The part's zone (keys, velocity, channel, transpose) filters what the
  plugin receives. The plugin's own note and velocity limits (R16) then split the zone's notes
  into keys it arpeggiates and keys it passes through.
- **Its output can leave the device.** A part whose chain ends in a MIDI out (RFC 0012 step 1)
  sends the arpeggio to external gear; with clock out (step 2) that gear follows the tempo.
- **Its notes are the plugin's own.** Output notes carry the plugin's note IDs. Voice stealing
  and part note limits (RFC 0018) apply to the instrument after it, as to any notes.
- **Sustain** (CC 64) latches the arp's keys while down, and passes on only for keys the arp
  does not own (the plan's decision 2, for Jacob to confirm).

Performance-level settings (the master switch, arp select, common offsets, Arpeggio Group)
become Performance controls that target each part's parameters through RFC 0011's controls
and RFC 0002 lanes, or the converter folds them into each part when no scene changes them.

### Transport and tempo

- **Tempo:** the plugin's clock runs at the host tempo (`clap_event_transport.tempo`), so the
  arpeggio follows Oscairn's transport, internal or following MIDI clock, and later Link
  (RFC 0012). Unit Multiply scales against it.
- **Stopped transport:** the instrument's arpeggios play with the sequencer stopped, and so
  does this plugin.
- **Parts starting together (Sync Quantize, R22)** need one beat grid shared by every
  instance. Today there is none while the transport is stopped: Oscairn's transport freezes its
  beat position when stopped (`Transport::advance`), and each plugin's `steady_time` starts at
  zero when it activates. The host adds a **free-running beat position**: the same for every
  plugin in a block, advancing at the tempo whether or not the transport plays, and carried in
  the shared block to workers (RFC 0010). The transport event itself keeps CLAP's meaning
  (stopped means its position does not move). The mechanism (a host extension read on the audio
  thread, or an Oscairn event each block) was open; step 3 chose the event (below).
- **Playing to stopped** ends all sounding arp notes. A jump in song position while playing
  restarts phrases at the next grid line.
- **Clock out:** the arpeggio's timing comes from the same transport that clock out writes, so
  external gear and the arpeggio agree with no extra work.

#### Step 3 status

Built in the host. The free-running beat is an Oscairn event, `oscairn.beat`
(`core/clap-ext/include/oscairn-clap/beat.h`, MIT): an event rather than a host extension
because events already cross the worker's shared block, so a plugin in a worker gets the same
value with nothing more to carry. A plugin asks for it by querying the event space
`oscairn.beat` through CLAP's event registry; it then gets `oscairn_event_beat` (the position in
`clap_beattime` and the tempo) at sample 0 of every block, before the block's other events.
Plugins that do not ask get nothing, and LV2 plugins never do.

The graph keeps the position: 0 when it is made, moving on each block at the transport's tempo
(120 BPM without a transport), playing or stopped, and through seamless Performance changes,
which keep the same graph. It never goes back, so starting, stopping or moving the transport
does not touch it. The transport event keeps CLAP's meaning.

Tested in `free-beat` with a fixture that follows the beat: with the transport stopped at
120 BPM the position runs at two beats a second; a second follower added 100 ms later has the
same position at every sample, so both reach beat 1 on the same sample (the gate's two parts,
which the arpeggiator's Sync Quantize turns into a shared start once it reads the event); in
the engine and in workers, at 256- and 37-sample blocks. The test fails without the change.

### Pushing a phrase: SysEx between plugins

The plugin accepts a phrase in band on its note input, as a CLAP `MIDI_SYSEX` event:
`F0 7D 4F 50 48 52 SLOT <phrase text> F7` ("OPHR", slot 0–7). This is how the script block
([RFC 0033](0033-script-block.md)), a sequencer or the host sends a generated phrase at an
exact sample.

**The graph cannot carry it today.** Oscairn's events are fixed 64-byte records
(`Event::capacity`), and the host's output check refuses SysEx (`validEvent` in
`core/clap-host/src/Host.cpp`). The host adds:

- a **byte pool per block** for SysEx payloads, preallocated, so a SysEx event's pointer stays
  valid until the block's consumers have run;
- the same pool in each worker's **shared block**, copied across with the events (RFC 0010);
- **limits:** a size per message (proposed 64 KiB, enough for a large phrase) and per block
  (proposed 256 KiB); a message over either is dropped and counted, never truncated;
- **routing** along note wires like any other note-port event; SysEx to a hardware MIDI out
  follows RFC 0012's output rules.

The plan has the plugin parse a pushed phrase into a spare slot without allocating and swap it
at the next event; its format document says the text is parsed on the main thread and takes
over a few milliseconds later. Either way, a push never blocks the audio thread, and its
latency is stated in the format document. Which one ships is open below.

#### Step 1 status

Built. A SysEx event (`CLAP_EVENT_MIDI_SYSEX`) is an `Event` like any other, pointing at bytes in
a block's `SysexPool` (`core/clap-host/include/kick_clap/Host.h`): 256 KiB, preallocated and
emptied each block, so nothing is allocated on the audio thread.

- **From a plugin:** the host copies a message into its pool while the plugin pushes it, as
  the plugin's buffer is valid only during the push. A message must be whole (`F0` … `F7`), at
  most 64 KiB, with 7-bit data bytes. One over a limit, or past the block's 256 KiB, is refused
  (`try_push` returns false) and counted in the host's dropped events, never truncated.
- **To a plugin:** delivered among the block's other events in time order, at its sample, on a
  note port whose dialects include MIDI (CLAP's rule for SysEx); refused on other ports. The
  control queue refuses SysEx: its bytes could not outlive the caller there.
- **Along note wires:** zones pass it unchanged, as other non-note events.
- **Workers:** the shared block carries an input and an output pool of 256 KiB each, an event
  holding its offset there; the engine copies a worker's output out of the shared block and
  checks every offset, and counts a message that does not fit with the plugin's dropped events.
- **Not built:** SysEx to hardware MIDI outputs (a part's MIDI out drops it), SysEx from
  hardware MIDI inputs, and LV2 plugins (their host drops it).

Tested in `sysex`: the fixture's 60 KiB message at sample 37 arrives at the next plugin at
sample 37, byte for byte, in order with the notes and a second message at that sample, at 256-
and 37-sample blocks, in the engine and with both plugins in workers; a message one byte over
64 KiB and the fifth of five 60 KiB messages at one sample are dropped and counted; the engine
allocates nothing while rendering. The test fails without the change.

### Conversion

The converter (`performance-converter`, MIT, outside this repository; RFC 0008's conversion
plan) places the arpeggiator as RFC 0016 places effects: in the ordinary graph, so a converted
Performance can be rearranged.

| On the instrument | In Oscairn |
| --- | --- |
| A Part with an arpeggio set up | The arpeggiator first in the part's chain, before the instrument |
| Part arp parameters | The plugin's parameters, by the value tables in the format document's "Converter notes" |
| Arp 1–8 Number | The eight slots' phrase text, plus each phrase's asset reference in the converted library |
| Common and scene offsets, Unit Multiply Common | Folded into each part when static; Performance controls when scenes change them |
| Arpeggio Master Switch, Arpeggio Select | Performance controls on every part's Switch and Slot |
| Arpeggio Group | Performance controls targeting each grouped part |
| Scene arp memorize switches | Oscairn scenes, which store parameter values |
| Drum Parts | Fixed SD/BD on, where the Part sets it |

The node may carry a role (`arpeggiator`), set by the converter, so a profile's arp button and
page find it, as RFC 0016's node roles do for effects.

Every conversion reports per RFC 0008: **mapped**, **approximated** (style tracks, R25, until
measured) or **unavailable**. A Part whose arpeggio the converter could not read says so.

**Where phrases come from.** Factory arpeggios are files inside the instrument plugin's bundle;
user and library arps are in its user content folder. The converter reads them on the user's
machine into `.ophr` files and an index, outside any repository. Reading these data files is
interoperability (RFC 0013's IP rule 3). Their layout knowledge stays in the converter. The
plugin never reads Yamaha files, and no table is taken from the instrument's code: the playback
rules come from the manual and from measurement.

The converted phrases are installed as a library on that machine. Like Yamaha factory content
in RFC 0009, an export lists it as a reference the destination must already have. A Sound's
own embedded phrase text is still its state: exporting a Sound that uses a converted factory
phrase would carry that phrase, so export warns and offers to leave the phrase as a reference
only. That rule is open below.

### Testing against the reference

The method is RFC 0013's: black-box renders of the reference, compared with Oscairn's own,
with tolerances stated per feature. Reference renders stay out of every repository.

- **The reference** is the instrument's macOS plugin, rendering a Performance while a recorder
  plays timed notes and controllers at 120 BPM.
- **A probe sound:** an FM Part with one sine carrier, instant attack and release, full sustain
  and full velocity sensitivity. Onsets, pitch, level and note length are read per key by
  demodulating at each key's frequency. Chords sit high enough that neighboring keys separate
  within 20 ms.
- **Calibration:** a velocity sweep with the arp off gives level against velocity.
- **Alignment:** each render is aligned on its first arp note, since the recorder jitters by an
  I/O buffer.
- **Comparison:** `arp-render` plays the same phrase, settings and key events offline, and
  notes are matched in order.
- **Order:** the settings and phrases most used in Jacob's Performances first, then each rule.
  Each rule in the plan's table is marked Manual, Measured or To measure; none is done until
  Measured or accepted by Jacob.

**Proposed gate (for Jacob to confirm):** every note matched, keys exact, onsets within 2 ms of
the reference after alignment, velocity within ±1 after calibration, gate length within 5 ms. A
probe passes only if every note does.

Inside Oscairn the plugin also meets the host's own gates: identical output at several block
sizes, no allocation under the render allocation guard, and the same output in a worker as in
the engine.

### User phrases

Jacob decided that users can make their own phrases four ways. Each produces the same thing: a
`.ophr` file.

1. **By hand.** The format is plain text, meant to be written in an editor. The format document
   is the reference, with examples.
2. **From a MIDI clip.** A clip becomes a phrase with absolute keys, keys relative to the lowest
   held key, or chord-relative positions against a chord the user states (the instrument's
   Fixed, Normal and Org-Note conversions). The plan has this as a Python tool; on the device it
   is a plugin operation (`phrase.import-midi`) taking the clip's asset reference, so the
   conversion lives in one C++ library in the plugin's repository.
3. **By playing.** RFC 0020's note capture now records the notes a part receives
   (`record.notes`, step 2). A **Make phrase** action takes a take, or later the retroactive
   buffer ("that was good"), a length in bars and the chord held while playing, and runs the
   same conversion as a MIDI clip. One recorder, and retroactive capture comes free. The plan's
   own record mode in the plugin (its milestone M5) does the same inside the plugin; whether
   both are needed is open below.
4. **In an editor** (below).

**In the library.** A phrase is a small text asset (RFC 0009): immutable, by SHA-256, an edit
making a new one. An item type `oscairn.phrase` (RFC 0023) makes phrases browsable, searchable
and exportable:

- storage **by content**, recognized by the `.ophr` extension and its first line;
- an extractor, shipped with the arpeggiator's package, reads the name, meter, length and
  track count into the index;
- the editor picks a phrase for a slot with the request `oscairn.pick.oscairn.phrase` (RFC 0023, RFC 0022).

The plugin's state keeps each slot's phrase text, and records the slot's asset reference once
`oscairn.assets` exists. The embedded text is the fallback, so a Sound never breaks when a
phrase is deleted from the library.

### The phrase editor

The editor works on the file's model: a header, tracks with their rules, and each track's
events. A grid per track shows time against pitch source: note-list positions (with octave
lanes) for chord-relative tracks, keys for absolute ones. Each cell holds velocity and gate.
Controllers are lanes of points. Edits: add, move, delete, set velocity and gate, quantize, set
a track's rule and held-key range, duplicate a track for another held count, audition with a
test chord.

Oscairn draws it (RFC 0011: no plugin windows on the device). RFC 0019's `oscairn.ui` needs:

- **A pattern grid** rich widget: rows and steps, cells with two values, and lanes of points.
  It generalizes the step lane already listed, and the KICK sequencer's pattern editor can use
  it too.
- **Operations with arguments,** as RFC 0019 already has: `phrase.set-event`, `phrase.insert`,
  `phrase.delete`, `track.set`, `phrase.audition`.
- **Operations that return data.** RFC 0019's operations send edits only. The editor needs a
  read (`phrase.get`, returning the phrase text) and a notification when it changes. This is
  new in `oscairn.ui` and in the control API.
- **Undo through the host.** RFC 0011 undoes every edit. For plugin operations the host keeps
  the plugin's state before each operation and restores it on undo; phrase states are small,
  so a snapshot per edit is cheap.
- **Audition** holds a test chord inside the plugin, so the editor plays without a keyboard.

No editor is built in the plugin's repository; the plugin provides the operations and the
description.

### Toward the Motion Sequencer

The engine's clock, Unit Multiply, swing, key-on reset and loop logic are not specific to
notes. A motion-sequencer lane is a track of value events aimed at a parameter through an
RFC 0002 lane: the plugin declares lanes (`oscairn.param-target-lanes`), and the user or the
converter maps them. The phrase format gains a `value` event in a later version. This RFC only
requires that nothing in steps 1–7 rules that out; the motion sequencer gets its own RFC
(step 8).

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | SysEx between plugins: byte pool, worker shared block, limits, routing | A fixture sends a 60 KiB SysEx at sample 37 of a block; the next plugin receives it at sample 37, byte for byte, in the engine and in a worker, at 256- and 37-sample blocks; a message over the limit is dropped and counted; no allocation on the audio thread |
| 2 | The plugin in a part (the plan's M1 hosted in Oscairn) | A part with the arpeggiator before the Basic Synth renders the same at several block sizes and in a worker; state round-trips through Save and Load; a pushed phrase takes over its slot at the stated time |
| 3 | A free-running beat grid | Two parts started 100 ms apart on a stopped transport, both with Sync Quantize, start on the same sample |
| 4 | Conversion | Every Part with an arpeggio in Jacob's factory and user Performances converts with its plugin, settings and phrases, and the report accounts for each |
| 5 | Measured against the reference | The proposed gate on each probe, most used settings first, then R1–R25 |
| 6 | User phrases | A hand-written phrase, a MIDI clip and a captured take each become a library item and play in a slot; a phrase deleted from the library leaves its Sounds playing |
| 7 | The phrase editor | Every edit above made on the panel without a plugin window, each undone; the KICK sequencer's pattern editor drawn with the same widget |
| 8 | Motion-sequencer lanes | Its own RFC |

## Open questions

- The measurement gate (2 ms onset, keys exact, velocity ±1, gate 5 ms), for Jacob to confirm.
- ~~How the free-running beat reaches plugins.~~ Decided in step 3: an Oscairn event each block
  (`oscairn.beat`), which workers carry like any event.
- Pushed phrases parsed on the audio thread (plan) or the main thread (format document), and
  the latency each gives.
- `7D` is MIDI's non-commercial manufacturer ID, meant for private use. That is fine inside the
  graph, but a push that reaches a hardware MIDI out leaves the device. Whether to filter it at
  MIDI outs, or apply for an ID.
- Exporting a Sound whose embedded phrase is a converted factory phrase: warn, or always keep it
  as a reference.
- Recording by playing: the host's note capture alone, or the plugin's record mode as well.
- Whether `oscairn-phrase 1` gains `tags` and `author` statements before it is frozen and
  published, so the library can index them.
- Style tracks (R25): where the transposition tables come from. They must be measured, not read
  from the instrument's code.

## Ruled out

- **Shipping phrases converted from Yamaha content,** in any repository or package. They are
  made on the user's machine.
- **The plugin reading Yamaha files at run time.** That belongs in the converter.
- **A fixed arp slot in the part model.** The arpeggiator is an ordinary plugin in the chain, as
  RFC 0016 decides for effects.
- **Scripting inside the arpeggiator.** Jacob decided scripting is a general block
  ([RFC 0033](0033-script-block.md)) that talks to the arpeggiator over its note input.
- **A plugin window for the phrase editor on the device.** Oscairn draws it from the
  description.

# RFC 0012: Sync, external MIDI devices and multi-device setlists

Status: proposed, 2026-10-30. Input device selectors implemented (below); the rest not
implemented. Covers two related needs:
- the Ignite's role as a sequencer for external MIDI and CV instruments;
- several Oscairn devices (and other gear) following one performance.

Builds on the graph's event routing ([architecture](../architecture.md#event-sources)),
timestamped hardware I/O ([RFC 0004](0004-hardware-services.md#one-event-timeline-explicit-device-clocks)),
prepared sounds ([RFC 0005](0005-sound-contexts.md)) and the Parts views
([RFC 0011](0011-performance-ui-and-editors.md)).

## Problem

Oscairn only receives today: physical MIDI input reaches the graph, and the transport can
follow an external MIDI clock (`transport.set clockSource "midi"`). It cannot send notes,
controllers or clock to other devices. It has no Ableton Link and no scenes. Its control API
is served only over a local Unix-domain socket.

Two setups need more:

1. **An Ignite-style sequencer.** Dozens of tracks playing external synths, drum machines
   and modular gear over several DIN and USB MIDI ports and CV/Gate, as well as internal
   engines, with the Ignite as the clock source.
2. **A rig of several devices.** For example a keyboard, a KICK and a loop pedal, each
   running Oscairn, possibly with a MODX or a commercial looper alongside. Changing the
   song or scene on one device must change the others together, at a musically sensible
   moment, without cutting sound.

## Outcome

- Any change or control can send arbitrary MIDI to any device or plugin.
- External MIDI and CV destinations are graph nodes. A sequencer or a part plays them
  exactly as it plays an internal instrument, with timing and latency compensation.
- One transport drives everything: MIDI clock, song position, transport commands, CV clock
  and Ableton Link go out as well as in.
- A setlist can span devices. Selecting a song or scene on the leader prepares it on every
  device ahead of time and switches all of them on the same beat.
- **Standards first:** anything standard MIDI can express travels as standard MIDI, so
  ordinary gear can lead or follow. Oscairn's own network protocol adds only what MIDI cannot
  express, and the switching never depends on it.

## External MIDI and CV destinations

- **Ports as nodes.** Each MIDI output port is a graph node with 16 channels: DIN, USB
  device, USB host and network MIDI. So is each CV/Gate output. Notes, expression and
  parameter-lane events (RFC 0002) route to them like any other node. Lanes map to CC,
  NRPN, pitch bend or a CV voltage.
- **Timing.** Graph events carry their sample position. The hardware service sends each
  MIDI message at that timestamp (RFC 0004), never when the audio callback happens to
  finish. Each destination has a latency offset, measured or entered, so an external synth's
  sound lines up with internal engines. Messages are sent early by that offset where the
  transport allows it.
- **External instruments in the Parts view.** An *external instrument* pairs a MIDI output
  and channel with an optional audio return input, so a hardware synth appears as an ordinary
  part. Its "sound" is a Bank Select and Program Change pair, named by hand or from a MIDNAM
  patch-name file. Changing that part's sound sends the messages at the switch point.
- **Note ownership.** The existing note-route ledger applies. Removing a route or switching
  a sound releases the notes the route owns on that port and channel. Hanging-note recovery
  sends All Notes Off on owned channels only.
- **CV.** Voltage and gate outputs follow the device's calibration (pitch per octave, offset,
  range), stored as a device setting, not in the Performance.

## Choosing devices: selectors

On a MODX, playing a part from an external controller means giving the part its own MIDI
channel and setting the controller to send on it; a controller that cannot change channel,
or sends on all, cannot play one part while the built-in keys play another. Gig
Performer's MIDI In blocks choose a device, but naming one device breaks when the
controller changes. Oscairn chooses devices by **selector**, separate from channels.

A selector is a list of rules; an event or device matching any of them is selected:

| Rule | Matches |
| --- | --- |
| Built in | the device's own keys, pads and panel |
| All external | every MIDI input or output but the built-in ones |
| All | everything |
| Port *n* | a numbered DIN or TRS port on the device (the Ignite has several) |
| All USB | every USB MIDI device |
| USB named | a USB device whose name equals a text, contains it, or matches a regular expression; a list of these |
| Network | a named network MIDI session |

- **Inputs:** a part's input says which devices it hears, and optionally which channels.
  Parts no longer have to differ by channel: an external controller on channel 1 can play
  part 3 while the built-in keys, also on channel 1, play part 1. Notes keep the channel
  the part's instrument expects.
- **Outputs:** an external destination (above) sends to the devices its selector matches:
  all outputs, port 1, 2 or 5, all USB, or USB devices by name, pattern or list.
- **The Performance stores selectors, not devices.** The running device keeps a table of
  its endpoints (built in, port number, USB name, network session) and resolves each
  selector when the Performance opens and whenever a device is connected or removed, so
  swapping one controller for another that matches needs no edits.
- A selector that matches nothing leaves its part silent and badged "no device"; nothing
  else changes.
- **Learn:** press a key on a controller and the part's selector becomes that device by
  name; it can then be widened to a pattern.
- Device profiles (below) attach to the devices a selector matches.

**Implementation status (inputs).** Every event entering the graph carries a source: 0 for
the panel (and notes sent over the control protocol), 1–63 for the host's MIDI inputs, or
untagged. Note zones ([RFC 0011](0011-performance-ui-and-editors.md#step-1-status)) gained a
source mask; a part's input stores its rules, and the Session resolves them into its input
wire's zones whenever parts change, a Performance opens, or the host's list of inputs
changes (applied at once unless edits are staged). Notes and other channel messages from an
unselected input do not reach the part, and a release or pressure from one input does not
touch another input's held note on the same key. A Performance stores the rules. The
desktop app numbers each MIDI input by device and keeps the number while it runs; it
reports every input as USB, since telling ports and network sessions apart needs the
device's own input table. The Part page's **Plays from** menu picks kinds and connected
devices by name, and a part matching nothing is shown "No device". Tested in `note-zones`,
`control-parts` (resolution, hotplug, staging, persistence), over the socket and in the
desktop views. Not yet: learn, pattern entry in the Part page (patterns are set over the
protocol), one sustain pedal per input (a pedal still holds every input's notes on its
channel), and output selectors, which come with external destinations (step 1).

## Transport and clock

One transport per device; sync sources and destinations are chosen per port.

| Signal | In | Out |
| --- | --- | --- |
| MIDI clock, Start/Stop/Continue, Song Position Pointer | follow (clock already works; the rest added) | send to chosen ports |
| Ableton Link (tempo, beat and phase over the network) | join a session | lead or join; no master in Link itself |
| MIDI Machine Control (Play, Stop, Record, Locate) | follow | send, for recorders and loopers |
| CV clock and reset | follow | send, with pulses per quarter note chosen per jack |

Ableton Link is open source under GPL-2.0-or-later, which can be combined with Oscairn's
AGPL-3.0-or-later. Link joins the network permission rules in the
[architecture](../architecture.md#packages-sound-packs-libraries-and-plugins): the host
provides it, and plugins never open the network for it.

## Multi-device setlists

A **setlist entry** names what each device plays: for example "Song 3" is "Open Road" on the
keyboard, "Open Road beats" on the KICK and "Song 3 loops" on the loop pedal. Each device
keeps its own Performances, so it also works alone. Devices are identified by a name chosen
at pairing.

### Who leads

- **Fixed leader** (default): one device selects songs and scenes, and the others follow.
- **Any device leads** (option per setlist): a change made on any device is sent to all.
  Two changes at once are resolved by their time on the shared timeline: the earlier one
  wins, and the later device is told its change was not applied.

### Preparing ahead

Per setlist, with **prepare everything that fits** as the default:
- Each device estimates the memory each Performance needs (prepared sounds, decoded
  samples, effect contexts). It prepares songs in order of distance from the current one
  until it reaches its memory limit, which is set per device.
- Songs that did not fit are prepared while the song before them plays.
- The leader shows, per device and song, whether it is ready.
- Streaming sample playback (RFC 0008) would let much larger setlists fit.

### Switching together

1. **Cue:** the leader sends a cue to all devices, which prepare it if they have not
   already.
2. **Go:** the leader sends go with a target point, by default the next bar on the shared
   timeline (Link or MIDI clock). Every device switches at that point, keeping held notes
   and tails as the scene rules require
   ([architecture](../architecture.md#scenes-and-seamless-switching)).
3. **Not ready:** a device that is not ready by then keeps playing what it has and reports
   it. Nothing cuts out, and it switches at the next bar once it is ready, unless the
   setlist says to stay.

### Wire format

Standards carry everything they can. Our own protocol adds the rest and is optional.

| Job | Carried by |
| --- | --- |
| Shared tempo and bar position | Ableton Link over the network; MIDI clock and Song Position Pointer over cables |
| Song and scene changes for ordinary gear | Program Change with Bank Select, or Song Select, sent when a Performance or scene loads. The same messages, when received, select songs and scenes, with a mapping per setlist |
| Prepare, then switch together | MIDI Show Control: LOAD prepares a cue, GO fires it, TIMED_GO fires it at a time. These are universal SysEx messages, so they travel over DIN, USB and network MIDI |
| Readiness, error reports, setlist and song names, device discovery and pairing | The existing control API, carried over the network with mDNS discovery and paired keys. MIDI has no reliable two-way acknowledgement or naming |

### Device profiles

Devices disagree on how to select a program, song or scene. A **device profile** describes
one model's conventions as data, in both directions. Oscairn uses a profile to drive that
device, and to read what that device sends when it leads. A profile is chosen per port, or
per port and channel, and can be edited, shared and installed as a package (RFC 0006).

A profile lists, for each action it supports (select song, program or Performance, select
scene, start, stop and so on), the message that performs it:

| Kind | Example |
| --- | --- |
| Program Change, optionally after Bank Select MSB/LSB | Most synths; 0- or 1-based numbering declared |
| A CC whose value range picks the target | MODX scenes: a configurable "Scene CC" (default 92), values split into eight ranges of 16 for Scenes 1–8 (forum-reported; confirm against Yamaha's Data List) |
| A CC per target, or a CC value per target | Loopers and footswitch-driven gear |
| NRPN | Devices with large program ranges |
| Note on a given channel | Pedals that select by note |
| Song Select, MIDI Show Control, MIDI Machine Control | Standard cueing and transport |
| A SysEx template with fields for bank, number or name | Devices with vendor-specific selection |

Each entry also declares:
- the channel (fixed, the device's own, or a setting);
- whether a CC number is the device's setting, as the MODX's Scene CC is;
- delays some devices need between messages (for example after Bank Select);
- whether the device confirms a change, and how.

A setlist entry then names targets rather than raw messages ("Scene 3", "Program A12"), and
the profile turns them into bytes.

Receiving works the same way, through a profile or a hand-made mapping. A MODX leading the
rig sends its Performance's Program Change and its Scene CC, and each Oscairn device maps
those to its own song and scene. A **generic** profile covers gear without one: the musician
picks the message kind, channel and numbers directly.

The first profiles to write: Oscairn itself (Song Select and MIDI Show Control), the MODX and
MODX M, and a generic Program Change device.

### Actions: arbitrary messages

Profiles cover the common cases; actions cover everything else. An **action** is a list of
messages attached to a trigger, sent to any destination.

- **Triggers:**
  - a setlist entry, Performance or scene loading or being left;
  - a part's sound changing;
  - a physical control or footswitch, through the assignments of
    [RFC 0011](0011-performance-ui-and-editors.md#physical-controls);
  - transport start or stop, or a bar or beat number;
  - a received MIDI message matching a pattern.
- **Messages:** any MIDI 1.0 message written out in full: status, channel and data bytes,
  CC, NRPN, Program Change, SysEx bytes. A message can also name a profile action ("MODX:
  Scene 3"). A value can come from the trigger, such as the control's position or the
  received message's value, scaled to a range.
- **Destinations:**
  - any MIDI output port and channel;
  - any graph node's note input, as MIDI events at their exact sample, so plugins receive
    them exactly like played MIDI;
  - a node's parameter, as an ordinary parameter change.
- **Timing:** at once, at the switch point, a set time before or after it, or quantised to
  the next beat or bar. Messages in one action keep their order, with optional gaps between
  them.

Actions are saved with whatever owns the trigger: a setlist entry, a Performance or scene, a
part, or a device's control assignments. Each runs on the engine's event timeline, not in the
UI, so a heavy screen never delays it. The UI lists every action a Performance will send,
and a learn mode records a message from a connected device into an action.

Oscairn-to-Oscairn switching therefore works over plain MIDI cables with clock. The network
layer only adds readiness display, names and setlist sharing. A device can follow a MODX's
Program Changes, and a MODX or looper can follow an Oscairn leader.

Sources disagree on some MIDI Show Control command numbers, and none of those checked gives
LOAD's data format. Pin both down from the MIDI Association's published specification before
implementing, and test against at least one third-party MIDI Show Control device or app.

### Network control and safety

Network control is off until enabled, and devices must be paired before they can send cues
or control requests to each other. Paired devices may cue, prepare, switch and report. They
may not install plugins, read files or change device settings. MIDI received on a port is
trusted as the musician's own cabling, as it is today.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. MIDI out | MIDI output ports as graph nodes, timestamped sending, latency offsets, note ownership on external ports | A sequencer's notes leave a USB MIDI port within 1 ms of their timestamp (measured on a loopback); removing a route releases its external notes |
| 2. Clock out | MIDI clock, Start/Stop/Continue and Song Position out; Song Position and transport in | An external drum machine follows start, stop and a jump to bar 9 without drift over ten minutes |
| 3. Link | Ableton Link in the transport | Two Oscairn devices and one other Link app stay within 1 ms of phase |
| 4. External instruments | Parts for external synths: Program Change/Bank Select sounds, audio return, MIDNAM names | A MODX part changes sound with the Performance and plays through its audio return |
| 5. Setlist cues | Actions (arbitrary messages to ports, plugins and parameters); device profiles (send and receive); Program Change/Song Select; MIDI Show Control LOAD/GO/TIMED_GO; Oscairn, MODX and generic profiles | A keyboard leader switches a KICK and a MODX (Performance and scene) on the same bar, over cables only; a MODX leading switches an Oscairn device; a scene action sends a SysEx string to a port and a CC to a plugin at the switch bar |
| 6. Network ensemble | Discovery, pairing, readiness, names, setlist sharing; any-device leading | The leader shows each device's readiness; a missing device keeps the others switching |
| 7. CV and MMC | CV/Gate and clock outputs with calibration; MIDI Machine Control | An Ignite drives a modular voice in tune across five octaves |

## Open questions

- How a follower maps a leader's song to its own Performance when names differ: by setlist
  position, by a shared song ID, or by an explicit table.
- Whether Oscairn's own scenes travel as Show Control cues inside a song, or as separate
  messages from its profile.
- A profile file format, and whether to read existing community formats for device MIDI
  maps.
- MIDI 2.0: whether Property Exchange should later carry names and readiness instead of
  our network protocol.
- Measuring external latency automatically (send a note, record the audio return).
- The Ignite's own sequencer workflow and track count are a sequencer plugin's concern
  ([sequencer evaluation](../sequencer-evaluation.md)), not this RFC's.

## Ruled out

- A proprietary protocol for things standard MIDI already does.
- Switching that depends on the network being present.
- Plugins opening the network themselves for sync or control.

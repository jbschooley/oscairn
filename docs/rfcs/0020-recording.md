# RFC 0020: Recording

Status: proposed, 2026-10-31. Not implemented. Makes concrete the "bus recording" the
[architecture](../architecture.md#layers) gives the host core, and the groovebox table's
"the core records any bus; the UI decides where the result goes"
([architecture](../architecture.md#sounds-performances-and-projects)). Builds on the graph
and parts ([RFC 0011](0011-performance-ui-and-editors.md)), the event timeline of
[RFC 0004](0004-hardware-services.md#one-event-timeline-explicit-device-clocks), sound assets
([RFC 0009](0009-sound-assets-and-dependencies.md)) and the surfaces of
[RFC 0019](0019-adaptive-ui.md).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Capture core
- [ ] 2. Parts and notes
- [ ] 3. Retroactive capture and sync
- [ ] 4. Control API and UI
- [ ] 5. Inputs and multitrack

## Problem

Oscairn cannot record anything. Three uses need it:

- **A lick.** Someone plays a good phrase on a synth and wants to keep it, usually after
  the fact, without having pressed anything first.
- **Grooveboxes and similar apps.** Resampling a part or Main into a Sampler pad, recording
  four bars from the next bar, and turning what was just played on the pads into a pattern.
- **Recorder boxes and digital mixers.** Oscairn may later run on devices like a Zoom or
  Tascam multitrack recorder, or a digital mixer: many inputs recorded at once, for hours,
  to an SD card, with nothing lost.

The architecture already names the pieces: the host core owns bus recording, a DJ setup
has recording buses, and the Phase 5 list asks for a recorder. Nothing says how it works.

There is no recording code today. The offline renderer writes a whole buffer to a 32-bit
float WAV file in one call (`writeWavFile` in `core/engine/include/oscairn/wav_file.h`),
which is not usable while playing.

## Outcome

- **Recording is an engine service, not a plugin.** It can tap any point of the graph: a
  graph audio input, a graph output such as Main, a part or bus (before or after its
  fader), a plugin's output port, and the notes entering the graph or leaving a node.
- **The audio thread only copies.** Each tap copies its block into a preallocated ring in
  memory. A writer thread turns rings into files. No allocation, locks or file I/O on the
  audio thread.
- **Nothing is lost silently.** An overrun, a full disk or a failed card stops or marks the
  take, keeps the files valid, and is reported with the exact samples affected.
- **Retroactive capture.** A rolling buffer keeps the last minutes of Main and the last
  notes played. "Save the last 30 seconds" writes them out after the fact.
- **One timeline.** Every tap in a take starts and ends on the same sample, so its files
  line up. Starts and stops can fall on a bar line, including one that has already passed.
- **Apps use the same service** through the control API: the first-party UI, a groovebox
  screen, a recorder app.
- **Multitrack is not blocked.** The design takes any number of inputs. What a recorder box
  still needs is listed, not designed here.

## Prior art

| Product | What it does | Source |
| --- | --- | --- |
| Ableton Live, Capture MIDI | Keeps what was played on MIDI tracks that are armed or input-monitored, without Record pressed, and turns it into a clip on request | [Live manual](https://www.ableton.com/en/manual/recording-new-clips/) |
| Logic Pro, Flashback Capture (named Capture Recording before 11.2) | Captures MIDI on a record-enabled track whether or not the project is playing; captures audio on one record-enabled track only after playing for at least four seconds | [Sound On Sound](https://www.soundonsound.com/techniques/logic-pro-flashback-capture) |
| Bitwig Studio, Master Recording | Records the master output whatever the transport is doing, into the project's master-recordings folder | [Bitwig user guide](https://www.bitwig.com/userguide/latest/master_recording) |
| Zoom H6essential, Pre Rec | Keeps the input buffered so a recording starts up to 2 seconds before the record key was pressed; the time depends on the sample rate | H6essential operation manual, "Capturing audio before recording starts" |

From these, Oscairn borrows: capture without arming (Ableton, Logic), a recording that
ignores the transport (Bitwig), and a pre-roll on the record key (Zoom). It extends the
pre-roll from seconds to minutes and applies it to audio and notes alike. MPC-style
retrospective recording was not checked against Akai's documentation, so nothing here
relies on it.

## Why a service and not a plugin

The [layers](../architecture.md#two-kinds-of-thing-plugins-and-the-ui) say anything at an
exact sample is a plugin. Recording is at an exact sample, but a plugin is the wrong
place for it:

- **A plugin sees only what is wired into it.** Tapping a part after its fader means
  knowing the Session's wires and the part's level, pan, mute and solo, which live in the
  Session, not in any node. A recorder plugin would need a wire from every tapped point,
  and those wires would show in Routing and change the graph.
- **Plugins run sandboxed** in workers ([RFC 0010](0010-plugin-worker-isolation.md#limits-and-sandboxing))
  with only their own data folder writable, and ask the host for assets. A recorder must
  write to the user's Recordings folder and to SD cards and USB drives.
- **Retroactive capture should always be running** without being in the Performance.
- **Apps are not plugins.** A groovebox screen or a recorder app talks to the engine
  through the control API, outside the audio thread.

A third-party recorder or looper plugin is still possible: RFC 0011 gives "audio in,
nothing out" the Analyzer role (tuner, meter, recorder). It is not the platform's path.

## What can be recorded

A **tap** is a point in the graph that a take records. Each tap becomes one file.

| Tap | Reads | Channels | Fader |
| --- | --- | --- | --- |
| **Graph output port** (Main, a cue or booth output) | The output buffers after every route into them | The port's channels | What is heard there |
| **Graph input port** | The external input buffer, before any route | The port's channels, or a range of them | None |
| **Part or bus, post-fader** (the default for parts) | The part's fader wires, summed: its last node's main output, or each lane's end, times the gain each wire plays it with | The meter rule: the part's last plugin's main audio output as wired to where it plays (`parts.meters`' `channels`) | Level, pan, mute and solo, as heard |
| **Part or bus, pre-fader** | The same wires without the part's level, pan and mute | As above | None; lanes keep their own Mix |
| **Node output port** | One plugin port's buffer, untouched | The port's channels | None |
| **Notes** | Events entering the graph (keyboard, pads, MIDI, UI notes, each with its source), or a node's note output (a sequencer) | n/a | n/a |

**Why parts need a computed sum.** A part's fader is not a buffer. The Session compiles
the fader into the gains on the part's wires to where it plays
([RFC 0011 step 1](0011-performance-ui-and-editors.md#step-1-status)), and a split part ends
in several wires. The meters already compute the per-channel gain pairs of those wires
(`Session::meter` in `core/control/src/parts.cpp`). A post-fader tap uses the same pairs,
but sums the samples into a tap buffer instead of taking their peak. So a post-fader tap
records exactly the part's contribution to where it plays, and the post-fader taps of every
part playing straight to Main sum to Main.

**Where in the block.** Each plan step keeps its own output buffer through the block
(`Plan::Step::outputBuffer` in `core/engine/src/graph.cpp`), so every audio tap copies at
the end of the block. Node note outputs are copied right after that node's step, where the
graph already reads them.

**Channel counts are fixed when a take starts.** If a plugin replacement changes a part's
channel count during a take, the file keeps its count: extra channels are dropped and
missing ones are silent, and both are reported in the take.

**Limits.** No limit beyond the graph's own (256 channels per endpoint set) and memory. A
16-channel input port can be one 16-channel file or sixteen mono taps of one channel each.

## Notes and MIDI

**What is recorded.** The events the graph receives, with their sample time and source
number (note_zone.h), and the events a chosen node emits. Played notes and a sequencer's
notes can be recorded side by side.

**What reaches the graph today.** The desktop's MIDI input passes notes, CC 64, 120 and 123,
polyphonic aftertouch, channel pressure, clock, start, continue, stop and song position, and
drops everything else (`AudioEngine::handleIncomingMidiMessage` in
`hosts/audio/AudioEngine.cpp`). Pitch bend, the mod wheel and other controllers therefore
cannot be recorded. Passing them into the graph is separate work, and step 2 needs it.

**File format.** A Standard MIDI File, type 1, at 960 ticks per quarter note, with the
tempo map from the transport. It is not sample-exact: at 120 BPM one tick is about
0.52 ms. The take's sidecar (below) keeps the first event's exact sample time. CLAP data
with no MIDI equivalent (per-note expressions on CLAP note IDs) is dropped from the file and
counted; a richer format is an open question.

**Into a pattern.** Recording notes into a sequencer's pattern stays the sequencer plugin's
job, as the [groovebox table](../architecture.md#sounds-performances-and-projects) says for
motion recording. The service gives apps the captured notes as data (`record.notes`), and
the app writes them into the pattern through the sequencer's own operations
([RFC 0011](0011-performance-ui-and-editors.md#plugin-editors)).

## Real-time safety

**Three threads.**

| Thread | Does | Never does |
| --- | --- | --- |
| Audio | Copies each armed tap's block into its ring; advances the ring's write count | Allocate, lock, make system calls, touch files |
| Writer, one per engine, normal priority | Reads rings, converts samples, writes files, updates headers, flushes | Run on a real-time priority, or block the control thread |
| Control | Validates requests, allocates rings, publishes the tap table, sends notifications | Wait for the writer or the audio thread |

**Rings.** Each tap has a single-producer ring of 32-bit float samples, allocated off the
audio thread when the tap is armed, touched and locked in memory so the audio thread never
takes a page fault on it ("audio never depends on swap",
[architecture](../architecture.md#keeping-plugins-loaded)). The write count is an absolute
sample number on the engine timeline. Readers keep their own position. A reader checks the
write count before and after copying, so it knows when the audio thread lapped it. The audio
thread never waits for a reader.

**The tap table.** Armed taps are compiled into a small table on the control thread and
handed to the audio thread through an atomic pointer swap, with the old table retired to
the control thread, the way graph plans are. It is not published through `graph.commit`,
because a commit would also publish other clients' staged edits.

**One ring serves both jobs.** Its length is the retroactive window, and it is also how
long the writer may stall before data is lost. A tap armed only for live recording gets a
short ring, 10 seconds by default.

**Overrun.** If the writer falls a whole ring behind:

- the lost span is written as silence, so the take's files stay aligned;
- the span is marked in the file (a cue point) and in the sidecar;
- the take reports `overruns` with each lost sample range, the status counter rises, and a
  notification goes out.

Before that, a ring past half full sends a "falling behind" notification, so a UI can warn
while nothing is lost yet. The notes ring counts events it had to drop, reported the same
way.

**Slow SD cards.** The writer writes in large chunks (about a second of audio per tap), grows
files in large steps where the file system can preallocate, and updates headers at most
every 2 seconds. Before a take starts, the service compares the data rate it needs with
what was measured on that drive (a short test write the first time a drive is used), and
refuses or warns. Sustained write speed and the length of stalls on real cards are
unmeasured; step 5 measures them.

**Disk full.** The writer stops each file before the drive's last 64 MiB, closes it with a
correct header, and reports `diskFull`. Audio goes on. The rings keep rolling, so the last
seconds are still in memory and can be saved to another drive.

**Write errors** (a card pulled out): files that can be closed are closed, the take is
marked `failed` with the reason, and the rings still hold the recent audio.

**Crashes and power loss.** A take in progress has a small journal file next to its audio.
When the engine starts, it finds takes still marked open and repairs their headers from the
file length. Power loss can lose what was written since the last header update and flush,
and whatever the drive itself had not yet stored.

**Plugin failures.** Taps read the engine's buffers, after worker outputs are copied back
(RFC 0010), so recording never crosses into a worker. A failed node is silenced; the take
records the silence and notes the failure's time and node.

## File format

**WAV with Broadcast Wave metadata, RF64 past 4 GiB.**

- RIFF/WAV addresses at most 4 GiB with its 32-bit sizes. RF64 adds a `ds64` chunk with
  64-bit sizes. A writer that does not know the final size reserves a `JUNK` chunk the size
  of `ds64` at the start and turns it into `ds64` only if the file passes 4 GiB, so shorter
  files stay ordinary Broadcast Wave ([EBU Tech 3306](https://tech.ebu.ch/docs/tech/tech3306v1_1.pdf),
  "Breaking the 4 gigabyte barrier"). Oscairn writes files that way.
- 4 GiB is about 3.1 hours of stereo 32-bit float at 48 kHz, but only about 15.5 minutes of
  32 channels of 24-bit. Multitrack needs RF64.
- Software that cannot read RF64 can still read every file under 4 GiB. Whether to split
  long files instead is an open question.

**Bit depth.** 32-bit float by default. 24-bit integer as an option.

- Float is what the engine computes. A part, bus or Main tap may go past 0 dBFS inside the
  engine, and float keeps it without clipping.
- Float records the engine's output exactly, so a take of Main can be tested sample for
  sample against an offline render, which already writes 32-bit float.
- 24-bit saves a quarter of the space and more software reads it. For a hardware input from
  a 24-bit converter it holds everything delivered. Samples past full scale are clipped and
  counted in the take.
- No dither and no resampling at record time. Files use the engine's sample rate.

**Names.** One folder per take, named by its local start time and the Performance:
`2026-10-31 21-04-17 Open Road/`. Inside, one file per tap, named after the part, bus or
port (`Lead.wav`, `Main.wav`, `Input 3.wav`, `Notes.mid`), with characters FAT and exFAT
refuse replaced, since SD cards and USB drives use them.

**Metadata.**

- **`bext` chunk** ([EBU Tech 3285](https://tech.ebu.ch/docs/tech/tech3285.pdf)): Description
  (Performance, tap, take), origination date and time, and `TimeReference`, the first
  sample's count since midnight. Every file of a take has the same `TimeReference`, so
  software that reads it can line the files up.
- **Cue points** for overruns and other marks.
- **A sidecar, `take.json`,** documented like the Performance format: sample rate, start and
  end on the engine timeline (with RFC 0004's epoch), tempo and beat position at the start,
  each tap's source and channel layout, the latency applied to it, overruns, clipped
  samples, dropped notes and plugin failures. Files first: it is the readable record of the
  take.

**Where files live.** The [user data](../architecture.md#user-data) rules apply: plain
files and folders, with the index rebuilt by scanning.

- On internal storage under `OSCAIRN_DATA_HOME/recordings`, beside `assets`
  ([control protocol](../control-protocol.md#sound-assets)), or in an `Oscairn/Recordings`
  folder on a chosen SD card or USB drive.
- The Library shows them as **Recordings**. Files shows the folders.
- When a file is closed it is hashed once and indexed as a host-owned library,
  `recordings`, through RFC 0009's registered-folder mechanism. A Sampler then refers to it
  by content, like any asset. A closed file is never changed; trimming makes a new one.
- Recordings are user data: they survive uninstalls, go into backups, and are exported only
  when a Sound refers to them (as user assets, RFC 0009).

## Retroactive capture

**How it works.** Saving the last N seconds starts a take whose first sample is N seconds
in the past. The writer reads the ring from there. The take ends now, or, with `keepGoing`,
carries on as a normal recording. One mechanism covers "save the last minute", "start
recording 2 seconds early" and "start from the bar line I just missed".

**What is on by default.** Main (stereo) and the notes entering the graph. Hardware inputs
are not captured by default: a microphone input held in memory without being asked is a
privacy problem. The buffer is never written to disk unless someone asks. The panel shows
that capture is on.

**Length.** Set per device. Proposed defaults: 2 minutes on a 2 GB device of the i.MX8M Mini
class, 10 minutes on a desktop. The notes ring keeps the last 65,536 events (about 2 MB at
32 bytes an event). Aftertouch streams fill it fastest, so whether that is enough is measured
in step 3.

**Memory.** At 48 kHz, 32-bit float costs 11.52 MB per channel per minute.

| Retroactive taps | Length | Memory |
| --- | --- | --- |
| Main, stereo | 2 min | 46 MB |
| Main, stereo | 10 min | 230 MB |
| Main and 8 stereo parts | 2 min | 415 MB |
| 16 mono inputs | 2 min | 369 MB |
| 32 mono inputs, live recording only | 10 s | 61 MB |

On 2 GB, the default costs about 2% of RAM. Every ring is reserved from the memory budget of
[keeping plugins loaded](../architecture.md#keeping-plugins-loaded), so loading sounds can't
take it, it can't push sounds out, and it shows in the memory meters. A request for more
than fits is refused with the reason.

**CPU.** The audio thread copies 2 channels × the block per stereo tap, about 384 KB a second
at 48 kHz; a post-fader part tap adds a multiply-add per channel and wire. This should be
small next to the plugins, but it is unmeasured; step 3 measures it on the target.

## Sync

**One timeline.** The transport already counts samples since it was created
(`Transport::now_` in `core/engine/include/oscairn/transport.h`); RFC 0004 adds the epoch.
Every ring counts on it, and all taps are written in the same block, so a take's files
align by construction. Notes are stamped on the same timeline.

**Start and stop.**

- **Now:** the start of the block in which the request arrives.
- **Seconds ago:** retroactive, as above.
- **On a bar line:** the next bar, or the last one. The audio thread works out the
  boundary's sample within the block from the transport's beat position and tempo, so it is
  exact with the internal clock. With an external MIDI clock it is as exact as the tempo
  estimate.
- **For a length:** a number of bars or seconds. "Four bars from the next bar" is the
  groovebox case.
- **Punch on the transport:** start when the transport starts, stop when it stops, or punch
  in and out at set beats.

The last bar line is possible because of the ring: pressing record just after the downbeat
starts the take on the downbeat.

**Several devices.** Each device records its own files. The shared `TimeReference` and the
sidecar's beat position let them be lined up afterwards. Starting takes together across
devices, through RFC 0012's MIDI Machine Control or the setlist protocol, is an open
question.

## Monitoring and latency compensation

**Monitoring.** A hardware input is heard by routing it, for example as a part with an
audio input. Recording an input is separate: an input can be recorded without being heard.
Direct monitoring in an audio interface's own mixer is a hardware service capability (RFC
0004), not this RFC.

**Inputs arrive late.** A musician plays along with what they hear. What reaches the graph
at sample t left the converter one input latency earlier, and what they heard was rendered
one output latency before they heard it. So an input tap is aligned with the internal taps
and the bar grid by reading it from S + output latency + input latency for a take starting
at S. The writer applies the offset, so the audio thread does nothing extra; the ring must be
that much longer, and a take ends that much later. All inputs of one interface get the same
offset, so their alignment with each other is exact either way.

**What is missing.** The desktop adapter does not read the device's latencies today, though
the `device_bench` tool does (`getInputLatencyInSamples` and `getOutputLatencyInSamples`,
`core/clap-host/tools/device_bench.cpp`). Reported values are often not the real round trip,
so step 5 adds a loopback measurement: play a click out, record it back in, and store the
result per device and interface.

**Plugin latency.** The architecture says parallel paths are latency-compensated, but the
graph does not do it yet; a host only reports a plugin's `clap.latency`. Until it does, a
part tap includes the latency of the plugins on its path, and the sidecar records it so a
later import can correct it. Once the graph compensates, taps follow it.

## Multitrack for recorder and mixer devices

**Already there.**

- Graph inputs and outputs are logical ports with any channel layout, up to 256 channels per
  endpoint set, mapped to device channels by the host
  ([audio ports](../architecture.md#audio-ports-and-hardware-routing)).
- Parts can take an audio input, and shared buses and independent output buses exist.
- Per-part meters with one bar per channel.
- This RFC: any number of input taps in one take, sample-aligned, to one drive.

**Missing, for later RFCs or Phase 4.**

- Input meters per graph input channel, for setting gain; `parts.meters` covers parts only.
- Input gain, pads and phantom power: hardware service controls (RFC 0004).
- Channel names from the board, and changing the layout without restarting the engine (the
  [control protocol](../control-protocol.md#audio-routing) says layout changes need a
  restart).
- Throughput on the real drive: 32 channels of 24-bit at 48 kHz is 4.6 MB a second,
  sustained for hours.
- Playing tracks back, overdubbing and mixing them: a streaming player, like the DJ deck's
  track player and the backing-track player on the roadmap. Not this RFC.

A digital mixer's "record every channel and the mix" is a take of each part pre-fader plus
Main, which this design covers.

## Control API

Proposed only; [control-protocol.md](../control-protocol.md) is unchanged until it is built.
Capability `record`.

| Method | `params` | `result` |
| --- | --- | --- |
| `record.taps` | omitted or `{}` | `taps`: each tap that can be recorded: `kind` (`output`, `input`, `part`, `node`, `notes`), its `port`, `part` or `node`, `name` and `channels` |
| `record.get` | omitted or `{}` | `retro`: its `seconds` and `taps`; `drives`: each with `path`, `free` bytes, measured write rate and minutes left at the current settings; `takes` (as `record.takes`) |
| `record.retro` | `seconds`, `taps` (as in `record.start`) | `{"accepted":true}`, or `-32007` with `reason: "memory"` |
| `record.start` | `taps`: each `{kind, port \| part \| node, fromPort, channels: [first, count], fader: "post" \| "pre"}`; optional `from` (`"now"`, the default; `{"secondsAgo":s}`; `{"bar":"next" \| "last"}`; `"play"`), `until` (`{"bars":n}`, `{"seconds":s}` or `"stop"`), `keepGoing`, `format` (`float32`, the default, or `int24`), `drive`, `name` | numeric `take` |
| `record.stop` | `take`, optional `at` (`"now"`, the default, or `{"bar":"next"}`) | `{"accepted":true}` |
| `record.takes` | omitted or `{}` | `takes`: `take`, `state` (`waiting`, `recording`, `finishing`, `done`, `failed`), `folder`, `files`, `start`, `frames`, `overruns` (sample ranges), `clipped`, `droppedNotes`, `error` |
| `record.notes` | optional `seconds` (default 30), optional `source` | `events`: `time` in seconds before now, `beat`, `kind`, `key`, `velocity`, `channel`, `source`; at most what fits the 64 KiB message limit, and `truncated` if more |
| `record.delete` | `take` | `{"accepted":true}`; refused while a Sound refers to one of its files |

**Notifications.** A new `record` topic for `subscribe`: take state changes, how far behind
the writer is, overruns, `diskFull` and errors, at most 10 times a second. Overruns and
errors are never coalesced away.

**Errors.** A new code, `-32007`, recording refused, with `data.reason`: `memory`, `disk`,
`throughput`, `unknownTap` or `drive`.

Example, saving the last 30 seconds of Main and the notes:

```json
{"jsonrpc":"2.0","id":3,"method":"record.start","params":{"taps":[{"kind":"output","port":0},{"kind":"notes"}],"from":{"secondsAgo":30}}}
```

```json
{"jsonrpc":"2.0","id":3,"result":{"take":12}}
```

## How apps and the desktop use it

- **A lick:** a **Capture** button saves the retroactive buffer: Main and the notes, the
  last 30 seconds by default, or as long as it holds. On any surface it is one press, and it
  can be bound to a physical button through RFC 0011's overrides.
- **Desktop** ([RFC 0019](0019-adaptive-ui.md#desktop)): a record button and Capture in the
  toolbar beside the tempo; an arm button on each Mixer strip; a **Recordings** pane in the
  browser with takes, files, overruns and the drive's minutes left; dragging a file onto a
  Sampler or a pad.
- **Panel:** Record and Capture in the strip of quick controls and on the device's own
  buttons; a full-screen take view with time, levels and minutes left.
- **Compact:** Capture on one button. Nothing else is needed for a pedal.
- **A groovebox:** resampling is `record.start` on a part or Main from the next bar for four
  bars, then preparing a Sampler sound from the new file. "What I just played" is
  `record.notes`, written into a pattern through the sequencer's operations.
- **A recorder app:** arms input taps, shows input meters and minutes left, and starts and
  stops takes. It is an app on the control API like any other.

## Playback of recordings

Kept to the minimum:

- **Audition** a short capture by playing it through the Sampler as a one-region sound, the
  path imported WAV files already take
  ([RFC 0009](0009-sound-assets-and-dependencies.md#implementation-status)). The Sampler
  loads whole files into memory today, so this is for captures of seconds to a few minutes.
  Whether it reads 32-bit float WAV was not checked (it lives in its own repository); if
  not, step 4 adds it.
- **Drag into a Sampler:** a closed file is an asset in the `recordings` library, so a pad or
  zone refers to it by content and it travels with the Sound.
- **Long takes** are copied to a computer, or wait for a streaming player.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Capture core | Taps on graph outputs, graph inputs and node ports; rings; tap table; writer; float WAV with `bext`, RF64 past 4 GiB; `take.json`; journal and repair; overrun, disk-full and write-error handling | A take of Main matches an offline render sample for sample at several block sizes; no allocation on the audio thread under the render allocation guard; a writer stalled on purpose produces one overrun with the exact lost range, aligned files and a notification; a full disk leaves valid files; a file grown past 4 GiB opens as RF64 |
| 2. Parts and notes | Post- and pre-fader part and bus taps from the fader wires; notes from the graph input and node outputs; SMF writer; more MIDI messages into the graph | The post-fader taps of parts playing straight to Main sum to Main; mute, solo and pan match what is heard; a split part records its lanes summed; a recorded notes file plays back within one tick |
| 3. Retroactive capture and sync | Retroactive rings in the memory budget; `secondsAgo`, bar lines past and next, lengths in bars, punch on the transport | On the i.MX8M Mini target, Capture during a heavy Performance adds no deadline misses (measured, with the copy cost reported); a take from the next bar starts on the bar line's exact sample in an offline render |
| 4. Control API and UI | The methods and notifications above; Capture and record on the desktop and the panel; the Recordings pane; drag into the Sampler | Over the socket: capture the last 10 seconds, prepare a Sampler sound from the file and play it, with no file path typed |
| 5. Inputs and multitrack | Device latency reports and loopback measurement; input offsets; input meters; drive throughput measurement and admission | A click played out and recorded back lines up with the internal click within a few samples; 16 inputs at 48 kHz, 24-bit, recorded for an hour to an SD card on the target with no overrun |

## Open questions

- The retroactive defaults on a 2 GB device: 2 minutes of Main, longer, or the selected part
  as well. Whether capture is on by default at all.
- RF64 or split files for takes past 4 GiB, for software that cannot read RF64.
- Float or packed 24-bit samples in the rings: 24-bit would cut retroactive memory by a
  quarter, at a conversion cost on the audio thread.
- A note format that keeps CLAP note expressions and note IDs, beyond MIDI files.
- Whether a sequencer plugin should read the notes ring through a host extension, rather
  than through an app.
- Starting takes on several devices together (RFC 0012), and whether received MIDI Machine
  Control Record should start one.
- What recording means when Oscairn runs as a DAW plugin, where the DAW records.
- Whether a safety copy of an input tap at lower gain belongs here or in a recorder app.

## Ruled out

- **Recording as a plugin node as the platform's path.** It would need wires from every tap,
  could not see a part's fader, and runs sandboxed. Third-party recorder plugins remain
  possible.
- **File I/O, locks or allocation on the audio thread,** including opening files at record
  start.
- **Losing samples silently.** Every overrun, dropped note, clipped sample and write error
  is counted and reported.
- **Compressed formats at record time** (FLAC, MP3): more CPU on a busy device, and harder
  recovery after power loss. Converting afterwards is fine.
- **Resampling or dither at record time.**
- **Writing the retroactive buffer to disk continuously:** it would wear SD cards and store
  audio nobody asked to keep.

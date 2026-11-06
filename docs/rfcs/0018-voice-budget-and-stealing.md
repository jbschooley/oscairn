# RFC 0018: Voice budget and stealing

Status: proposed, 2026-10-31. Steps 1 to 3 implemented (below; step 3's device gate unverified). When a Performance asks for more than the
processor can play, Oscairn gives up the least audible notes across every part, without
clicks, rather than missing the audio deadline. The host does this with notes (option B),
and plugins that opt in do it with their own voices (option C).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. Fade and choke ([status](#step-1-status))
- [x] 2. Part limits ([status](#step-2-status))
- [x] 3. Shared budget ([status](#step-3-status); the gate on the device unverified)
- [ ] 4. The `oscairn.voice-budget` extension

## Problem

A synth with a fixed voice pool steals its own voices when the pool is full: it prefers a
voice already releasing, then the same key struck again, then the oldest or softest held
voice. It protects the highest and lowest held keys, and it fades the stolen voice over a
few milliseconds. Oscairn's vendored Modular does this inside its 16 voices, keeping the
stolen voice's phase and level so the hand-over doesn't click
(`plugins/modular/upstream/modular/mt_modular_engine.cpp:299`).

Oscairn runs many plugins at once, and each owns its voices. Nothing decides between them.
Today a Performance that asks for too much misses the audio deadline. In a worker process
([RFC 0010](0010-plugin-worker-isolation.md)), a block that runs over half the block's time
is silenced whole, which is worse than losing one quiet note.

The Kronos shows the target. Korg sets aside part of its processor for voices and shares it
among nine engines as needed, so each engine's quoted polyphony (HD-1 140, CX-3 200, STR-1
40, MOD-7 52) is only reached when that engine has the whole budget
([Korg](https://www.korg.com/us/products/synthesizers/kronos3/nine_engines.php)). The limit is a
processing budget, not a voice count, and a heavy engine costs more per note. Korg doesn't
document how it chooses what to steal. Users report it takes the notes "contributing the
least", which is anecdotal.

## Notes or voices

The host can only see notes:

- CLAP's model gives the host one note per `NOTE_ON`, which lives until the plugin sends
  `NOTE_END` (`clap/events.h`). What the plugin plays for that note is hidden: unison
  copies, layers, or an AWM2-class sound's elements, up to 128 per note
  ([RFC 0013](0013-awm2-class-sample-engine.md)).
- `clap.voice-info` reports how many voices a patch *may* use and the plugin's capacity, not
  how many are sounding (`clap/ext/voice-info.h`).
- The host's handles are note IDs. `NOTE_CHOKE` and per-note expressions address a note,
  never one voice inside it.

So the host's stealing works on notes. It still accounts for voices implicitly, because it
measures what each note costs: a note playing 8 unison voices costs about 8 times as much,
and the budget sees that.

Choosing between voices *inside* a note, such as dropping a quiet element but keeping the
loud ones, needs the plugin. Option C below keeps that split: the host shares out the
budget, and each plugin chooses which of its own voices to give up.

## No clicks

The host never cuts a sounding note. A note is stolen in two stages:

1. **Fade:** the host ramps the note's CLAP volume expression (`CLAP_NOTE_EXPRESSION_VOLUME`)
   to silence over the fade time, 5 ms by default, in steps at sample offsets.
2. **Choke:** after the ramp ends, it sends `NOTE_CHOKE` for that note ID, which frees the
   voice.

Choosing what to steal also avoids clicks:

- **Released notes first:** a note in its release is already falling, and is quieter.
- **Steal early:** stealing starts below the hard limit (85% of the budget by default), so
  fades have time. A note still costs processing while it fades.
- **Choke without a fade only at the hard limit:** cutting a note is better than missing the
  deadline, which silences or crackles everything. This is counted and reported.

CLAP doesn't say how a plugin should handle a choke, or require it to honor volume
expressions. So:

- The porting guide recommends treating a host choke as a fast fade of 2–5 ms, and handling
  `CLAP_NOTE_EXPRESSION_VOLUME`.
- The host can't detect whether a plugin honors volume expressions. A plugin may declare it
  through C's extension; otherwise the host assumes it doesn't, and prefers that plugin's
  released notes.
- The native instruments do both, with tests that check a stolen note leaves no
  discontinuity in the rendered audio.

**Plugins the host can't address per note:** a MIDI-only plugin, or one without note IDs,
can't have one note choked or faded. Its notes aren't stolen. Its cost still counts against
the budget, and other parts give way.

## Option B: a shared budget across parts

**The budget**
- A share of each block's time for plugin processing, 70% by default and set per device.
  The remainder is headroom for the system and effects.
- In a worker, a node's share is bounded as RFC 0010 already bounds it.

**Cost per note**
- Each host already reports how long its last `process` call took (`Statistics::lastMicroseconds`).
- The engine keeps a smoothed average for each node: time minus the node's idle cost, divided
  by its live notes.
- Live notes come from the routing ledger (`NoteRoute` in `core/engine/src/NoteRoute.h`).
  It already holds every note sent to a node until that node's `NOTE_END`, with key,
  velocity, input, zone, note ID and age, so released notes still count.
- A plugin with native sound contexts reports its active voices, which refines the
  estimate.

**When to steal:** at note-on, not a block later, the same moment a hardware instrument
steals.
- An incoming note is costed at its node's measured average.
- If the total would pass the soft limit, the host chooses notes to fade before passing the
  new note on.
- Measurement then corrects the estimate each block.
- A burst, such as a ten-note chord on a heavy patch, is admitted on the estimate. If
  measurement still shows the hard limit passed, the host steals more in the next block.

**What to steal, in order:**
1. Notes already fading, so they finish their fade.
2. Released notes: quietest velocity first, then oldest.
3. Held notes beyond a part's **reserve**.
4. Among those: lowest velocity, then oldest. The highest and lowest held key of each part
   are protected (melody and bass).
5. Never a part's reserved notes, unless reserves alone pass the hard limit.

**Part settings:** these are stored in the Performance, edited on the Part page, and shown
with live counts on the Parts list.
- **Reserve:** notes guaranteed to the part, like Roland's Voice Reserve.
- **Note limit:** the part's maximum. Past it, the part steals from itself first, which
  covers the simple per-part polyphony case.
- **Priority:** which parts give way first.

**Real time:**
- The ledger is a fixed array, and choosing victims is a scan of it, with no allocation.
- Notes and their fades are events, as in today's routing, so a stolen note's choke reaches
  the plugin at an exact sample.
- The panel shows notes stolen and hard chokes per part, so a Performance that steals too
  often can be fixed.

## Option C: plugins steal their own voices

C would be an optional Oscairn CLAP extension (`oscairn.voice-budget`), MIT-licensed like
the others. A plugin that implements it:

- **Reports:** its active voices, an estimate of each voice's cost, and its current loudness
  for each note. The host can then compare notes across plugins by how audible they are, not
  only by velocity and age.
- **Gets a limit:** the host sets its voice limit at the block boundary, and changes it as
  the shared budget moves between parts. This is the Kronos's dynamic allocation.
- **Steals itself:** within that limit, it gives up voices with its own fades, choosing by
  voice: one element of a layered note, one unison copy, or the quietest voice in its pool.
- **Declares volume expressions:** it can say it honors volume expressions, so B's fade is
  known to work.

The native instruments implement it first, and the AWM2-class engine needs it most. A plugin
without the extension gets option B.

## Mono and legato

Mono and legato are part modes, not stealing, but they reuse the same ledger and the same
fade and choke. They are listed here so they are built once:

- **Mono:** one note per part; a new note fades the old one.
- **Legato:** a new note while one is held glides from it without re-triggering, which needs
  the plugin. CLAP has no legato flag, so the host forwards it and the plugin decides.

## Step 1 status

The plugin side, 2026-10-31: the SDK's sound-contexts helper fades a choked voice over 3 ms,
then ends it (and its note), and applies `CLAP_NOTE_EXPRESSION_VOLUME` per voice; an
instrument multiplies each voice's samples by `amplitude(voice, offset)`, and a voice that
does not still ends when its fade would have. The Basic Synth and the Sampler do; the
`sampler` test checks the Sampler's fade sample by sample. `choke-fade` checks a
choked note against the same note left alone: most of its level in the first millisecond,
less than 60% in the second half of the fade, silence and `NOTE_END` after 3 ms, and per-note
volume halving it; a cut, or a fade that ignored the ramp, fails it.

The host side, 2026-10-31: when a note limit steals a note, the note router lowers its
`CLAP_NOTE_EXPRESSION_VOLUME` from the last value sent to it to silence in eight steps over
5 ms, at their sample times and across blocks, then chokes it. The new note plays at once, so
the part sounds one note over its limit during the fade. The stolen note's own volume
expressions stop reaching it. `note-routing` checks the steps, their times and the choke at
the end of the fade, and through the graph in 64-frame blocks that the old note sounds
through its fade and then stops.

## Step 2 status

Note limits, 2026-10-31: a part's input has `maxNotes` (0 to 128, 0 for none), saved, in the
protocol and on the Part page as **Notes at once**. It rides on the part's input wires as a
zone setting, and the note router enforces it where the notes enter the instrument: before
a note past the limit, it fades out and chokes the oldest released note still sounding, else
the oldest held one, sparing the highest and lowest held keys (melody and bass) while another can go. A stolen note stops counting at once, is not released again, and frees its slot
when the plugin ends it. Only where the plugin takes CLAP notes; a MIDI-only plugin is not
limited, since MIDI cannot choke one note. With lanes, each lane's instrument has the limit.
`note-routing` checks the order (released first, though older notes are held; the bass and
melody spared), the stolen note's release going nowhere, and no limit; `control-parts` the
Session and saving it.

Reserve, priority and one count across lanes, 2026-11-06: a part's input also has `reserve`
(0 to 128, within `maxNotes` when it has one) and `priority` (0 to 10, lower giving way first),
saved, in the protocol and on the Part page as **Reserved notes** and **Priority**; a lower
note limit brings the reserve down to it. They take effect with step 3's shared budget, as
stealing across parts; within one part the note limit alone decides. The note limit is now one
count for the whole part: its input wires' zones carry the part's ID (`NoteZone::part`), and
the graph gives each such connection the others of its part, so a note past the limit in one
lane may take another lane's note. That note's fade starts at the beginning of its
instrument's block, this one or, when its instrument was already given this block's events,
the next. `note-routing` checks two instruments split at middle C under a part limit of two:
a third note in the upper lane leaves one note in each (without the shared count, two and one);
`control-parts` that the limit's zone names the part, the reserve and priority refused past
their ranges and round-tripping through a saved Performance; `parts-model` the Part page's
setters; the socket test reads them back over the protocol.

## Step 3 status

The shared budget, 2026-11-06 (`VoiceBudget` in `core/engine/src/NoteRoute.h`;
`Graph::setVoiceBudget`). It is RFC 0025's CPU budget, not a second one: with `budget.set`'s
`cpu` set, the hard limit is that share of the block's time for all the plugins, and the soft
limit 85% of it. Until a budget is set, nothing is stolen.

- **Cost per note:** after each block the graph reads each plugin's last `process` time
  (`Statistics::lastMicroseconds`, the worker's own for a plugin in one) and the notes its
  connections hold in the routing ledger until their `NOTE_END`, released and fading ones
  included. With none, the time updates the node's cost at rest; with some, the time above rest
  shared among them updates its cost per note, both smoothed: rising by a twentieth and a tenth
  of each block's difference, falling by a half and three tenths. A block's sample counts at
  most four times the estimate, so a thread held up from outside does not make every note look
  dear (before this, one such block while resting made a part's cost at rest 100 µs, and a
  reserve was choked, on this loaded Mac). Until a node has its own figures, RFC 0025's
  measurements of its plugin (rest and per voice) stand in, given by `Session::measure` each
  service tick. A plugin taking no CLAP notes (an effect) counts at its last block's time and
  gives way to nothing.
- **Stealing at note-on:** before a note passes on, its node's cost per note is added to the
  estimate (every node at rest, plus every note not given up at its node's cost). Past the soft
  limit, notes fade out (the 5 ms ramp, then the choke) until it fits. Including the notes still
  fading, past the hard limit, notes are choked at once: those fading first, then in the order
  below. After a block, when the measured total passed the hard limit in this block and the one
  before, notes are choked from the next block's start until the estimate, scaled to the
  measurement (at most 1.25 times), fits; that path never takes a reserve.
- **The order:** released notes; then held notes beyond their part's reserve, the part's
  highest and lowest held keys last; within each, the part of lower priority first, then the
  lowest velocity, then the oldest. A reserve gives way only at the hard limit at note-on,
  when the plugins at rest and every part's reserved held notes alone pass it. Only CLAP notes of nodes whose notes have a measured cost are
  taken. A part is its input wires' zones' part (step 2), so its lanes are one part; a
  connection without zones is a part of its own.
- **Across threads:** routing (the note-on decision) and reading each step's time both run on
  the graph's own thread, in plan order, also when plugins render in waves on the audio helpers
  (RFC 0034) or in workers, so the budget's state is that thread's alone; the per-node counters
  and costs are atomics, read by the control thread. The ledger is the fixed array it was;
  choosing a victim is a scan of it, with no allocation.
- **Counted:** each node counts the notes it gave up, faded and hard-choked (choked before any
  fade; a fading note choked early counts once, as faded); `parts.meters`
  sums them per part (`stolen`, `hardChokes`), and the desktop's Parts list shows them after the
  part's instrument ("12 stolen, 1 cut").
- A stolen note in another node's connection fades from that node's block start: this block's,
  or the next when that node already had this block's events.

Tests: `voice-budget` (offline, one node after another; `-parallel` on three audio helpers;
`-workers` in worker processes) plays a chart on three instances of a fixture whose every voice
costs 30 µs a 64-frame block: B's sixteen loud notes first (priority 5, reserving 8), then A's
bass and melody, eight notes A releases (each a block after it starts), eight quiet ones and
eight loud ones, and C's sixteen, a note every other block. Released notes are newer and as loud as held ones, so neither age nor
velocity alone puts them first. The budget is the plugins at rest plus half what the notes add,
by the median block of a run without it. It checks that without the budget at least 80% of the
measured blocks pass it, with it at most a quarter as many, no released or quiet note that was
sounding when a loud held one was given up given up after it, and B losing the fewest and never
its reserve; it allocates
nothing while rendering. Without `setVoiceBudget` taking effect it fails ("with the budget the
plugins stay under it"); without the order's released-first rank and priority, "released and
quiet notes before any loud held note (of those sounding then)"; without priority alone, "the part of higher priority
loses the fewest". `control-parts` sets a CPU budget on a Session with two such parts, B giving
way first but reserving four of its six notes, and checks the parts' meters: A gives notes up,
B keeps its reserve (the first failed with the budget never set, both with reserves ignored). The three `voice-budget` runs and `control-parts` pass under ThreadSanitizer with no reports
(`control-parts` once failed there, "B keeps its reserve: 3", when a fading note choked early
counted as stolen and hard-choked both; it counts once now).

Mac measurements, 2026-11-06 (development Mac, load average about 50 to 120 from other work;
offline rendering, 64-frame blocks at 48 kHz, a block's deadline 1333 µs; no audio device):
`oscairn-voice-budget-test`'s chart, the budget from a run without it. "Over" counts the
81 measured blocks whose plugins' times summed past the budget; "late", past the block's time (a
serial engine's deadline); a plugin deadline miss is one plugin's block past it.

| Run | Budget (at rest) | Without: over, late, misses | With: over, late, misses | Cost per note (A, B, C) | Stolen; hard chokes (A, B, C) |
| --- | --- | --- | --- | --- | --- |
| Fixture, 30 µs a voice, in the engine | 871 µs (1) | 81, 81, 15 | 2, 2, 11 | 36.6, 30.0, 30.0 µs | 23, 0, 12; 10, 7, 0 |
| Fixture, in workers | 872 µs (2) | 81, 81, 0 | 0, 0, 0 | 30.0, 30.0, 30.0 µs | 22, 0, 10; 7, 0, 1 |
| Basic Synth (4 per group, budget at rest + 75% of the notes), in the engine | 37 µs (13) | 81, 0, 0 | 9, 0, 0 | 2.1, 1.3, 2.5 µs | 14, 0, 3; 2, 0, 5 |
| Basic Synth, in workers | 41 µs (15) | 81, 0, 0 | 0, 0, 0 | 2.9, 1.2, 1.4 µs | 13, 0, 2; 1, 0, 0 |

With the fixture, the first sixteen notes given up were A's released and quiet ones, then loud
held ones of A and C; B (priority 5) lost none to fades, and in the engine run seven of its sixteen
to hard chokes at the end, once A's and C's were gone, keeping its reserve of eight. Plugin deadline
misses count the whole run, the chart's start included, on a machine whose other work held the
thread up. The Basic Synth costs about 1 to 3 µs a note against about 4 to 5 µs at rest per
instance, and with 16 voices it cannot reach a real budget, so its budget here is set low on
purpose; its notes given up followed the same order (a note A releases may go in the block it
starts, before its release). No run had audio-device deadlines, which offline rendering does
not have.

The gate (a Performance that misses deadlines without the budget plays a stress chart on the
device with none, stealing only quiet or released notes) is **unverified**: it needs the
device. Not yet: the plugin's own voice count refining the estimate (step 4's extension),
splitting a note's cost by key or velocity, and effects' tails giving way.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Fade and choke | The two-stage steal of one note in `NoteRoute`; the porting guide's choke and volume-expression advice; the Basic Synth and Sampler honor both | A stolen note in the rendered output has no step larger than a reference fade; no allocation |
| 2. Part limits | Note limit, reserve and priority per part, stealing within the part; persisted and in the protocol; Part page fields | A part limited to 4 notes plays 8 keys without clicks, keeping the newest 4 and the protected keys |
| 3. Shared budget | Cost per note from measurement, stealing across parts at note-on, soft and hard limits, counts on the Parts list | A Performance that misses deadlines without the budget plays a stress chart on the device with no deadline misses, and only quiet or released notes stolen |
| 4. Extension | `oscairn.voice-budget` header, host support, the native instruments implementing it | A layered AWM2-class sound under pressure drops quiet elements before whole notes |

## Before loading

Stealing handles too many notes once playing. Whether a Performance fits at all, before it
loads (its plugins' cost at rest and at their note limits, and during a seamless change both
Performances), is admission, in [RFC 0025](0025-resource-budgets.md), which counts this RFC's
budget among the rest.

## Open questions

- The soft and hard limits, and the fade time, on the real device.
- Whether a note's measured cost should be split by key range or velocity, since sample
  playback costs vary.
- Whether effects with long tails (reverbs) should give way too, by shortening tails, or
  only instruments.
- Whether C's loudness report should be per note or per voice, and how often it is sent.
- What CLAP's own work on voice management offers. Following it, and proposing the
  extension upstream, is better than a permanent private one.

## Ruled out

- **Host access to plugin voices without the plugin's help.** CLAP gives the host no handle
  on a voice.
- **A fixed voice count per plugin.** It wastes the budget when one part is idle, which is
  the problem the Kronos's shared budget solves.
- **Choking without a fade as the normal path.** It clicks.

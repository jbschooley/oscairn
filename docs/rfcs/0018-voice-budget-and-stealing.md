# RFC 0018: Voice budget and stealing

Status: proposed, 2026-10-31. Step 1's plugin side implemented (below). When a Performance asks for more than the
processor can play, Oscairn gives up the least audible notes across every part, without
clicks, rather than missing the audio deadline. The host does this with notes (option B),
and plugins that opt in do it with their own voices (option C).

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

CLAP doesn't say how a plugin should handle a choke, or require it to honour volume
expressions. So:

- The porting guide recommends treating a host choke as a fast fade of 2–5 ms, and handling
  `CLAP_NOTE_EXPRESSION_VOLUME`.
- The host can't detect whether a plugin honours volume expressions. A plugin may declare it
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
- **Declares volume expressions:** it can say it honours volume expressions, so B's fade is
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
does not still ends when its fade would have. The Basic Synth does. `choke-fade` checks a
choked note against the same note left alone: most of its level in the first millisecond,
less than 60% in the second half of the fade, silence and `NOTE_END` after 3 ms, and per-note
volume halving it; a cut, or a fade that ignored the ramp, fails it.

Not yet: the host's own volume ramp before a choke, for third-party plugins that fade on
neither; and stealing itself, starting with step 2's note limits.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Fade and choke | The two-stage steal of one note in `NoteRoute`; the porting guide's choke and volume-expression advice; the Basic Synth and Sampler honour both | A stolen note in the rendered output has no step larger than a reference fade; no allocation |
| 2. Part limits | Note limit, reserve and priority per part, stealing within the part; persisted and in the protocol; Part page fields | A part limited to 4 notes plays 8 keys without clicks, keeping the newest 4 and the protected keys |
| 3. Shared budget | Cost per note from measurement, stealing across parts at note-on, soft and hard limits, counts on the Parts list | A Performance that misses deadlines without the budget plays a stress chart on the device with no deadline misses, and only quiet or released notes stolen |
| 4. Extension | `oscairn.voice-budget` header, host support, the native instruments implementing it | A layered AWM2-class sound under pressure drops quiet elements before whole notes |

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

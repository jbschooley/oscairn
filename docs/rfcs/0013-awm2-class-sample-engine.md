# RFC 0013: An AWM2-class sample engine

Status: proposed, 2026-10-30. Not implemented. Extends the Oscairn Sampler and the SFZ
profile of [RFC 0008](0008-sfz-native-samples-and-conversion.md). Factory and user Yamaha
waveforms reach it only through import, as described [below](#yamaha-content-import-not-inclusion).

## Problem

RFC 0008 builds the Sampler up from one SFZ region and extends SFZ "only for demonstrated
gaps". That gets ordinary SFZ libraries playing, but it doesn't commit to being able to
voice what a Yamaha AWM2 Part does. Jacob's MODX M library depends on that: elements with
their own envelopes, filters and LFOs, articulation switching, and key-bank waveforms.
Without a stated target, each gap gets argued case by case and "approximated" becomes the
default answer.

## Outcome

The Oscairn Sampler can do **everything an AWM2 Part can do** as a sound engine. Every AWM2
element and drum-key parameter has a native equivalent with the same range, unit and
behavior, so a converted Yamaha sound plays with no conversion loss in the sampler. Effects,
scenes, arpeggios and controller assignments stay at the Performance level, as RFC 0008
already decides.

The engine is a **new implementation written for Oscairn**. It is not a port, and it
doesn't use Yamaha code. Behavior is specified from public documentation (the MONTAGE M /
MODX M manuals and Data List) and from black-box measurements of rendered output. No Yamaha
code, tables or DSP are copied, disassembled into the engine, or linked.

Non-goals:
- Bit-exact output. The target is measured equivalence within tolerances this RFC defines
  per feature.
- FM-X and AN-X. Those are separate engines with their own RFCs.
- Yamaha effects. Their behavior goes through [RFC 0007](0007-effect-contexts.md) effect
  contexts, separately.

## Capability target

Factory usage counts were taken on 2026-10-30 over the 3,631 factory Performances: 5,170
active AWM2 Parts and 22,459 active elements, decoded with Jacob's MODX M
reverse-engineering tools, not with Oscairn. They set the **order** of work, not the scope.
Everything in the table is in scope.

| Area | AWM2 behavior | Factory usage | Representation |
| --- | --- | --- | --- |
| Waveforms and key banks | A waveform is a set of key banks: note and velocity range, root note, coarse and fine tune, level, pan, mono or stereo, loop start and end or one-shot, and its own sample rate | every element | SFZ regions in an includable waveform file; `loop_*`, `pan`, `tune`, `transpose`, `volume` |
| Elements | Up to 128 per Part (up to 67 seen in factory content; 1–8 typical). Each has wave, note and velocity limits, level, pan and tuning | every Part | One `<group>` per element over the waveform's regions. Element state must stay per voice, never shared |
| Velocity Cross Fade | Fades between velocity-split elements | 789 | `xfin_*` / `xfout_*`. Check that the curve matches |
| XA Control | Normal, Legato, Key Off, Cycle, Random, Assignable Switch Off/1/2 | Key Off 918, Legato 375, Cycle 391, Random 713, A.SW 3,221 | `trigger`, `seq_*`, `lorand`/`hirand`, `sw_*`/`on_locc`. Selection and reset rules must be measured |
| Element Group (XA groups) | Elements grouped for exclusive playback and switching | – | `group` / `off_by`, or an extension if the semantics differ |
| Key On Delay | Delay in time or tempo-synced note length | 884 | `delay`, `delay_beats` |
| AEG | Initial, attack, decay and release levels and times; time key-follow with center note; velocity time sensitivity per segment; release adjustment | every element | Multi-point envelope (ARIA `egN_*` shape) with **Oscairn time and level tables** fitted to measurements |
| PEG | Hold, attack, decay, release levels and times; depth; depth velocity sensitivity and curve; key follow | 18,116 non-default | Same multi-point envelope routed to pitch |
| FEG | Hold, attack, decay, release levels and times; depth; velocity and key follow | 19,697 non-default | Same, routed to cutoff |
| Filter | 19 types: LPF24D, LPF24A, LPF18, LPF18s, LPF12+HPF12, LPF6+HPF12, HPF24D, HPF12, BPF12D, BPFw, BPF6, BEF12, BEF6, DualLPF/HPF/BPF/BEF, LPF12+BPF6, Thru. Cutoff, resonance or width, gain, Dual distance, HPF cutoff, key follow, scaling breakpoints, cutoff and resonance velocity sensitivity | LPF12+HPF12 in 50% of elements; LPF24A, LPF24D, LPF18 next | Native filters. Types without an SFZ equivalent (serial pairs, Dual, LPF18s) are the first Oscairn extensions |
| Level and pitch scaling | Level and cutoff scaling over 4 breakpoints with offsets; level key follow; velocity sensitivity, offset and curve; pitch key follow with center; pitch velocity sensitivity; fine-tune key scaling | most elements | Breakpoint curves (extension) and existing `amp_*` / `pitch_*` where they match |
| Pan | Element pan, random, alternate and scaling pan depth | 1,191 random or alternate | `pan`, `pan_random`; alternate and scaling pan need extensions |
| Random pitch | Per-note random pitch | 1,191 | `pitch_random` |
| Element LFO | Wave, speed (normal or extended), delay, fade-in, key-on reset; amplitude, pitch and filter depth | 1,041 | Per-voice LFO (ARIA `lfoN_*`) |
| Part LFO | Shared LFO with 3 destination boxes, phase offset and per-element depth ratio | – | Per-Part modulation source inside the sound context. Per-element depth is an extension |
| Element EQ | Type, frequency, gain, Q per element | – | Per-voice EQ (`eqN_*` where it matches) |
| Half Damper | Continuous sustain behavior with damper time | – | Extension: SFZ sustain is a switch |
| Element output | Element → Insertion A or B | 915 to B | Two per-context outputs feeding the Part's insert chain ([RFC 0007](0007-effect-contexts.md)) |
| Part voicing | Mono or poly, key assign, portamento (switch, mode, time, time mode) | Part level | Sound-context settings; ARIA portamento where it matches |
| Controller Sets, Motion Control | 32 controller sets per element | very common | Destinations are engine parameters. Assignment is Performance-level ([RFC 0002](0002-param-target.md), RFC 0011) |
| Drum Parts | 73 drum keys, each with its own wave, level and velocity sensitivity, pan (random, alternate), coarse and fine tune, pitch velocity sensitivity, LPF (cutoff, resonance, velocity sensitivity), HPF, a short AEG (attack, decay 1 and 2, decay level), 2-band EQ, reverb and variation sends, alternate group, key assign, receive note-off and output | 701 factory Drum Parts | Regions with per-key settings; alternate groups map to `group`/`off_by` |
| Interpolation and pitch range | High-quality resampling across very large pitch offsets (keybanks span octaves; rates up to 192 kHz) | everywhere | Replaces the baseline's linear interpolation. Measure aliasing against ESP |

Where existing SFZ or ARIA opcodes have the same meaning, use them. Every row marked
"extension" follows RFC 0008's extension contract: fixture, name, units, defaults, export
behavior. None of the syntax is defined in this RFC.

## How behavior is specified

1. **Documentation first.** Ranges and parameter lists come from the Data List and manuals.
   Unit tables and curves the documents don't publish (EG time tables, velocity curves,
   filter responses, LFO shapes) are measured.
2. **Black-box measurement.** A reference harness renders the ESP plugin offline with a fixed
   MIDI sequence and one parameter swept per test. It compares the output with the same patch
   rendered in Oscairn: envelope timing and level, filter frequency response, pitch, LFO rate
   and shape, articulation selection. Tables are fitted from these renders. The harness and
   fitted tables are Oscairn's own work. Reference renders and Yamaha audio stay out of the
   repository; fixtures use generated test tones or licensed samples.
3. **Per-feature tolerances.** Each row gets a measurable acceptance target, for example
   EG segment time within 2%, filter response within 1 dB up to the cutoff, pitch within
   1 cent. A feature is "done" when its tests pass, not when it parses.
4. **Listening acceptance** stays with Jacob, as for every sound feature
   ([listening tests](../listening-tests.md)).

## Yamaha content: import, not inclusion

The Oscairn repository and packages contain **no Yamaha samples, waveforms, Performances or
code**. A user who owns the ESP plugin or a MONTAGE M / MODX M brings their own content:

- **Factory waveforms.** A separate tool, run by the user against their own installation,
  writes the waveforms out as SFZ plus FLAC, with key banks and loop points, into a sample
  library the user owns. Oscairn imports that library like any other
  ([RFC 0009](0009-sound-assets-and-dependencies.md)). This tool is not part of Oscairn; the
  Yamaha codec is not reimplemented in or linked by Oscairn. That keeps RFC 0008's rule of
  not reproducing Yamaha's codec, while still answering its "factory-ROM waveforms:
  unavailable unless audio is supplied by the user" row: the user supplies it through this
  import.
- **User waveforms.** These come from `.Y2U` / `.Y2L` exports (plain PCM, per the
  [ESP research](../yamaha-esp-samples-research.md)) or from the same tool.
- **Performances.** RFC 0008's converter maps Parts, elements and drum keys onto this
  engine. The conversion report lists anything the engine can't yet represent, against this
  RFC's table.

Imported content is identified by content hash and by the waveform number it came from.
Converted Performances can then reference waveforms without embedding them.

## IP rules

These apply to this RFC and to every engine and effect RFC that refers to it (0014–0016).

1. **Clean room.** Engines are written from public documentation (manuals, Data Lists) and
   black-box measurements of rendered audio. No Yamaha code, binaries, firmware, tables or
   anything derived from disassembling them enters Oscairn. Reverse-engineering notes are
   kept outside this repository, and the people writing engine code don't work from them.
2. **No Yamaha content.** No Yamaha samples, waveforms, Performances, presets or reference
   renders in the repository or in packages. Fixtures and bundled sounds are generated, or
   come from content whose licence allows commercial redistribution (public domain, CC0,
   CC-BY or equivalent). No NC or ND licences.
3. **Import, don't decode.** Oscairn imports standard formats (SFZ, WAV, FLAC) and reads
   documented, unencrypted user files for interoperability. Anything that decodes Yamaha's
   proprietary sample codec stays outside Oscairn and isn't distributed with it.
4. **Measurement sources.** Reference renders come from legally obtained Yamaha software or
   hardware, are used only to fit Oscairn's own tables and tests, and are never committed.
   Models trained or captured from audio (for example amp captures) must not be trained on
   Yamaha output.
5. **Names.** Yamaha's names (AWM2, FM-X, AN-X, MONTAGE, MODX, Motion Control, Super Knob,
   XA and others) may be used in design documents for reference, but not in product,
   plugin, parameter or UI names. Compatibility statements describe facts ("imports MODX M
   Performances"), and each is reviewed before release.
6. **Patents.** Before any commercial release, counsel reviews each engine and effect
   feature for freedom to operate. Patent analysis stays with counsel and is not stored in
   this repository; contributors are not asked to search patents. Design changes that
   result are recorded here as ordinary design decisions.

## Build order

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | RFC 0008 step 3: multisample regions, groups, loops, FLAC, streaming, pan, crossfades, triggers, round robin and random | Plays an imported factory waveform library (all 7,647 waveforms load, loops continuous) and the Salamander piano |
| 2 | Measurement harness and the first fitted tables: AEG, then velocity curves | Repeatable renders. Tolerances met on swept AEG tests |
| 3 | Element model: up to 128 elements per sound with per-voice state; Key On Delay; XA Control | Factory articulation fixtures (key-off, legato, cycle, random, A.SW) select the same samples as ESP |
| 4 | Filters: LPF12+HPF12 and the LPF family first, then the rest; FEG; scaling breakpoints | Frequency-response tests per type |
| 5 | PEG, LFOs (element and Part), pan modes, random pitch, element EQ, Half Damper | Per-feature measurement tests |
| 6 | Drum Parts and element outputs to inserts A/B | Drum-kit fixtures; routing tests with RFC 0007 |
| 7 | Converter coverage: every AWM2 element and drum-key parameter mapped | The conversion report shows no loss for engine parameters across the factory set |

Each step also meets RFC 0008's runtime gates: off-audio preparation, no allocation while
rendering, held-note survival, and sanitizer runs. CPU and memory are measured on the target
hardware before any polyphony or element-count claim.

## Open questions

- The polyphony and element budget on the target hardware. 128 elements times many voices is
  expensive, so measure before promising.
- Whether serial and Dual filters are best as one extended `fil_type` or as an explicit
  second filter stage.
- How Controller Set destinations (32 per element) are exposed as engine parameters without
  an unmanageable parameter count.
- Filter Type codes 19 and 21 appear in 320 factory elements but aren't in the Data List's
  list of types. Identify them by measurement before mapping.

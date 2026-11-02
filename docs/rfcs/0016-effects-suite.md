# RFC 0016: An effects suite covering the MODX M platform

Status: proposed, 2026-10-30; revised 2026-11-01 (no slot model: Yamaha's slots map onto
ordinary routing). Not implemented. Builds on [RFC 0007](0007-effect-contexts.md)
effect contexts and the bus and send model. Same rules as [RFC 0013](0013-awm2-class-sample-engine.md):
effects written for Oscairn or reused from open-source projects, with no Yamaha code.

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Routing for converted effects: the converter's mapping onto parts, buses and wires;
      node roles for device profiles (roles stored and exposed, [status](#node-roles-status);
      mapping profile controls to them remains); a test of side-chain on effect contexts
- [ ] 2. The most-used types
- [ ] 3. The remaining categories
- [ ] 4. Converter coverage

## Outcome

Oscairn ships effects that cover every effect type a MONTAGE M / MODX M Performance can use,
in every place it can use them, so a converted Performance keeps its effects. Each Yamaha type maps to
an Oscairn effect with equivalent parameters, measured against ESP renders, as RFC 0013
describes. This RFC does not make each type a separate plugin. Several types can be modes of
one effect (for example one reverb with Hall, Room, Plate and Stage algorithms).

## Target

### Where the effects go

The MONTAGE M and MODX M have fixed effect slots (from the Data List and the manual):
- Per Part: Insertion A and B, a 3-band and a 2-band Part EQ, and the element and drum-key
  routing to A or B.
- Per Performance: Reverb and Variation sends, Master Effect, 5-band Master EQ, the A/D-input
  insertion, and the VCM Rotary slot.
- Side-chain or modulator input for the marked types.

Oscairn has no slot model. Each slot is a place in the ordinary graph
([RFC 0011](0011-performance-ui-and-editors.md)'s parts, chains, shared buses and wires), so a
converted Performance is a normal Performance the musician can rearrange:

| Yamaha slot | In Oscairn |
| --- | --- |
| Insertion A, Insertion B, Part EQ | Plugins in the part's chain, in Yamaha's order (A and B in series or parallel as the Part sets them; parallel is a split) |
| Element and drum-key routing to A or B | The sample engine's two outputs per context ([RFC 0013](0013-awm2-class-sample-engine.md)) wired to the two insertion chains |
| Reverb and Variation sends | Two shared buses, each a Mix node and its chain; the Part's send levels are its sends, Variation-to-Reverb a send from one bus to the other |
| Master Effect, Master EQ | A bus every part plays through, its chain the master effect then the EQ |
| A/D-input insertion | A part with an audio input and its chain |
| VCM Rotary slot | A plugin in the chain where the Performance has it |
| Side-chain or modulator input | A wire into the plugin's side-chain port (RFC 0011 keeps it when the plugin is replaced) |

An effect is an ordinary CLAP plugin; nothing in it knows which slot it fills. A new part gets no
fixed EQ or sends: a part template can add them, and the Mixer shows the Performance's own buses
(RFC 0019).

Two additions remain:

- **Node roles.** A hardware panel's buttons and pages name an effect ("edit Insertion A",
  "Master EQ"). A node may carry an optional role (`insertion-a`, `insertion-b`, `part-eq`,
  `reverb`, `variation`, `master-effect`, `master-eq`), which the converter sets and the musician
  can change; a device profile ([RFC 0024](0024-device-profiles.md)) maps its controls to roles,
  and a Performance without a node in that role shows nothing there. Saved with the node in the
  Performance format.
  **Node roles status** (2026-11-01): a node carries one role or none, `insertion-a`,
  `insertion-b`, `part-eq`, `reverb`, `variation`, `master-effect`, `master-eq` or, for
  [RFC 0032](0032-arpeggiator.md), `arpeggiator` (`setNodeRole`; `graph.role`, `graph.get`'s
  `role`); any other name is refused. It is an edit, undone like any other, saved with the node
  in the Performance (format version 11) and the Sound (version 7) as `role`, and goes with its
  node when the node is removed. Nothing reads it yet: a profile mapping its controls to roles is
  still to do. `control-parts` checks setting, refusing, undoing, clearing, the round-trip and
  removal; `control-performance-format` checks `graph.role` and the saved role against both schemas.
- **A test of side-chain on effect contexts.** Each context of an engine shared through
  [RFC 0007](0007-effect-contexts.md) is a full CLAP interface the host loads like any plugin,
  ports included, so a context can declare a side-chain input and take a wire to it. Nothing in
  the host is missing, but no plugin with contexts has a side-chain yet (the Delay's contexts have
  main ports only), so it is untested.

### Types

There are 104 types in the Data List's Effect Type List, excluding No Effect and Thru. Types
marked * take a side-chain or modulator input.

| Category | Count | Types |
| --- | --- | --- |
| Reverb | 15 | HD Hall, REV-X Hall, R3 Hall, SPX Hall, HD Room, REV-X Room, R3 Room, SPX Room, HD Plate, R3 Plate, SPX Stage, Space Simulator, Gated, Reverse, Shimmer |
| Delay | 9 | Cross, Tempo Cross, Tempo Mono, Tempo Stereo, Control, L/R, L/C/R, Analog Retro, Analog Modern |
| Chorus | 6 | G Chorus, 2 Modulator, SPX Chorus, Symphonic, Ensemble Detune, GS1 Ensemble & Tremolo |
| Flanger | 5 | VCM, Classic, Tempo, Dynamic*, Control |
| Phaser | 5 | VCM Mono, VCM Stereo, Tempo, Dynamic*, Control |
| Tremolo and rotary | 7 | Auto Pan, Tremolo, Rotary 1 and 2, VCM Rotary Classic, Overdrive, Studio |
| Distortion and amps | 12 | Amp Sim 1 and 2, Comp Distortion (+Delay), US Combo, Jazz Combo, US High Gain, British Lead, Multi FX, Small Stereo, British Combo, British Legend |
| Dynamics | 7 | VCM Compressor 376*, Classic*, Multi Band*, Uni Comp Down* and Up*, Parallel, M/S EQ Compressor |
| Wah | 3 | VCM Auto, Touch, Pedal |
| Lo-fi | 4 | Lo-Fi, Noisy, Digital Turntable, Bit Crusher |
| Tech | 18 | Ring Mod*, CS Ring Mod, Dynamic Ring Mod*, Dynamic Filter*, Auto Synth, Spiralizer P and F (+Tempo), Isolator, Slice, Tech Modulation, Control Filter, Vinyl Break, Beat Repeat (Even, Triplet, Even+Triplet, Free) |
| Misc | 13 | VCM EQ 501, Presence, Harmonic Enhancer, Stereophonic Optimizer, Talking Modulator, Wave Folder, VCM Mini Filter, VCM Mini Booster, Damper Resonance, Pitch Change, Early Reflection, Vocoder*, Noise Gate+Comp+EQ |

The full list with type numbers and slot availability is `effect_types.json` in Jacob's MODX
M research repo. Factory usage: VCM EQ 501 is the most common Insertion A effect (32% of
Parts), then none, VCM Compressor 376, Classic Compressor and Amp Simulator 1.

## Open-source coverage

Candidates to evaluate. Licenses must be confirmed per project, and each reuse pinned and
credited.

| Need | Candidates | Gap |
| --- | --- | --- |
| Algorithmic reverbs, early reflections, gated, reverse, shimmer | Dragonfly Reverb (GPL-3.0, Freeverb3-based), Airwindows reverbs (MIT), Surge XT reverbs (GPL-3.0) | Matching each Yamaha algorithm's character needs tuning; shimmer and reverse need work |
| Delays, tempo sync, analog delay | Airwindows, Surge XT; straightforward to write | Small |
| Chorus, ensemble, flanger, phaser, tremolo, auto pan | Airwindows, Surge XT (chorus, ensemble, flanger, phaser) | Small; VCM (circuit-modeled) types need analog-style models |
| Rotary speaker | setBfree (GPL-2.0-or-later; check compatibility), Surge XT rotary | Good existing models |
| Amp and cab simulation | Guitarix (GPL), Neural Amp Modeler (MIT) | Need models per amp type; NAM requires training captures, which must not be captures of Yamaha's output |
| Compressors, multiband, M/S, gate | LSP Plugins (LGPL-3.0), Airwindows | Small; VCM 376 is a circuit-style model |
| Wah, filters, ring modulation, bit crusher, lo-fi, wave folder | Airwindows, Surge XT | Small |
| Vocoder | Surge XT vocoder, LSP | Needs the side-chain input |
| Pitch change | Signalsmith Stretch (MIT), Rubber Band (GPL-2.0+ or commercial) | Already named in the architecture for key lock |
| Beat Repeat, Slice, Isolator, Vinyl Break, Spiralizer, Talking Modulator, Damper Resonance | No direct equivalents | Write in Oscairn |
| Part EQs (3-band, 2-band), 5-band Master EQ, VCM EQ 501 | Standard biquad EQs; 501 is a circuit-style model | Small |

## Build order

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | Routing for converted effects: the converter's mapping of each slot onto parts, buses and wires (above), node roles, a side-chain test on RFC 0007 contexts | Converted factory Performances route as on the instrument, measured against ESP renders; a profile's buttons reach their roles |
| 2 | The most-used types first: VCM EQ 501, compressors, reverbs, delays, chorus, rotary, amp simulator 1 | Measured against ESP renders; listening |
| 3 | The remaining categories | Per-type measurements |
| 4 | Converter coverage for every type and parameter | The factory set converts with no effect-parameter loss |

## Open questions

- One plugin per category with algorithm modes, or one plugin per type. Per-category matches
  RFC 0007's shared native engines and keeps the plugin count manageable.
- How tempo-synced types follow Oscairn's clock ([RFC 0012](0012-sync-external-midi-and-multi-device.md)).
- The factory usage figures here count Insertion A only. Count every slot before finalizing
  priorities.

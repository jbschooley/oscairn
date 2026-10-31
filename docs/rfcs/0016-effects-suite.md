# RFC 0016: An effects suite covering the MODX M platform

Status: proposed, 2026-10-30. Not implemented. Builds on [RFC 0007](0007-effect-contexts.md)
effect contexts and the bus and send model. Same rules as [RFC 0013](0013-awm2-class-sample-engine.md):
effects written for Oscairn or reused from open-source projects, with no Yamaha code.

## Outcome

Oscairn ships effects that cover every effect type and effect slot a MONTAGE M / MODX M
Performance can use, so a converted Performance keeps its effects. Each Yamaha type maps to
an Oscairn effect with equivalent parameters, measured against ESP renders, as RFC 0013
describes. This RFC does not make each type a separate plugin. Several types can be modes of
one effect (for example one reverb with Hall, Room, Plate and Stage algorithms).

## Target

### Slots

From the Data List and the MONTAGE M manual:
- Per Part: Insertion A and B, a 3-band and a 2-band Part EQ, and the element and drum-key routing to A
  or B.
- Per Performance: Reverb and Variation sends, Master Effect, 5-band Master EQ, the A/D-input
  insertion, and the VCM Rotary slot.
- Side-chain or modulator input for the marked types.

Each slot is an effect context on a bus, which RFC 0007 already provides. Side-chain needs a
second input per context.

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

Candidates to evaluate. Licences must be confirmed per project, and each reuse pinned and
credited.

| Need | Candidates | Gap |
| --- | --- | --- |
| Algorithmic reverbs, early reflections, gated, reverse, shimmer | Dragonfly Reverb (GPL-3.0, Freeverb3-based), Airwindows reverbs (MIT), Surge XT reverbs (GPL-3.0) | Matching each Yamaha algorithm's character needs tuning; shimmer and reverse need work |
| Delays, tempo sync, analog delay | Airwindows, Surge XT; straightforward to write | Small |
| Chorus, ensemble, flanger, phaser, tremolo, auto pan | Airwindows, Surge XT (chorus, ensemble, flanger, phaser) | Small; VCM (circuit-modelled) types need analog-style models |
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
| 1 | Slot model: Part insertions A/B, Part EQ, Reverb and Variation sends, Master Effect and EQ, side-chain inputs on RFC 0007 contexts | Routing tests; converted Performances keep their structure |
| 2 | The most-used types first: VCM EQ 501, compressors, reverbs, delays, chorus, rotary, amp simulator 1 | Measured against ESP renders; listening |
| 3 | The remaining categories | Per-type measurements |
| 4 | Converter coverage for every type and parameter | The factory set converts with no effect-parameter loss |

## Open questions

- One plugin per category with algorithm modes, or one plugin per type. Per-category matches
  RFC 0007's shared native engines and keeps the plugin count manageable.
- How tempo-synced types follow Oscairn's clock ([RFC 0012](0012-sync-external-midi-and-multi-device.md)).
- The factory usage figures here count Insertion A only. Count every slot before finalizing
  priorities.

# RFC 0014: An 8-operator FM engine

Status: proposed, 2026-10-30. Not implemented. Follows the [architecture](../architecture.md#engines)'s
plan for "an FM engine, starting from an existing open engine where licences allow (such as
Dexed / MSFA)". Same rules as [RFC 0013](0013-awm2-class-sample-engine.md): a new engine
written for Oscairn, with no Yamaha code or content.

## Outcome

An Oscairn FM instrument that can do everything a MONTAGE M / MODX M FM-X Part can do, and
everything a DX7 can do. Converted FM-X Parts and imported DX7 SysEx banks play with no
conversion loss in the engine's parameters. Behavior is specified from the published Data
List and manuals plus black-box measurements of rendered output, exactly as RFC 0013
describes. Effects and Performance-level controls stay outside the engine.

## Target: what an FM-X Part has

From the MONTAGE M Data List: Part common, 8 operator blocks, and the filter and pitch
sections. Factory usage counts cover 1,457 active FM-X Parts across the 3,631 factory
Performances.

| Area | FM-X | Factory usage |
| --- | --- | --- |
| Operators | **8** operators. Ratio or fixed frequency, coarse, fine, detune, key-on phase reset | 562 Parts use more than 6 operators; 428 use a fixed frequency |
| Algorithms | **88** algorithms with operator feedback (0–7) | 71 algorithms used; 457 Parts use an algorithm above 32 |
| Operator waveforms | Spectral Form: Sine, All 1, All 2, Odd 1, Odd 2, Res 1, Res 2, with Spectral Skirt (0–7) and Spectral Resonance (0–99) | 642 Parts have at least one non-sine operator |
| Operator EG | Hold time; attack, decay 1, decay 2 and release levels and times (0–99); time key follow | every Part |
| Operator pitch EG | Initial and attack levels, attack and decay times | – |
| Level scaling | Break point; low and high depth; curves −Lin, −Exp, +Exp, +Lin; velocity sensitivity; level controller sensitivity | most |
| Pitch | Key follow, velocity sensitivity, pitch controller sensitivity | – |
| Part LFO | Wave, speed (normal and extended 0–415), phase, delay, key-on reset, per-operator depth offsets and Part LFO destination ratios | – |
| 2nd LFO | Triangle, saw down and up, square, sine, S/H; speed, phase, delay, key-on reset; pitch, amplitude and filter depth | – |
| Part PEG | Initial, attack, decay 1 and 2, release levels and times; depth (8, 2, 1 or 0.5 oct); velocity and key follow | – |
| Filter | The same 19-type filter, FEG and scaling breakpoints as an AWM2 element | – |
| Macro controls | FM Depth, Harmonics, Attack, Decay, Sustain, Release, Texture (±99) | – |
| Voice | Key On Delay, random, alternate and scaling pan, random pitch, mono or poly, portamento | – |

The filter, FEG, LFO and key-on-delay blocks are the same building blocks as RFC 0013's.
Build them once and share them between the two engines.

## How close Dexed is

[Dexed](https://github.com/asb2m10/dexed) (GPL-3.0) emulates the DX7. Its FM core, MSFA, is
Apache-2.0.

| | Dexed / MSFA | FM-X | Gap |
| --- | --- | --- | --- |
| Operators and algorithms | 6 operators, 32 algorithms | 8 operators, 88 algorithms | Generalize the operator graph. The 88 topologies are already transcribed from the Data List chart (`fmx_algorithms.json` in Jacob's MODX M research repo) |
| Operator waveform | Sine only | Sine plus 6 spectral forms with skirt and resonance | New oscillator: band-limited spectra, measured against renders |
| Operator EG | DX7 4-rate / 4-level | 4 levels and times, plus hold and key follow | Close relative. Extend with hold, and fit FM-X's time table |
| Level scaling | Break point, depths, the same four curves | Same concept and ranges | Nearly identical. Verify the curve shapes |
| Rate scaling, velocity, feedback | Yes (feedback 0–7) | Yes | Same ranges. Verify the response |
| Pitch EG | One 4-rate / 4-level pitch EG | Per-operator PEG plus a Part PEG with depth ranges | Extend |
| LFO | One LFO (6 waves, delay, PMD, AMD) | Part LFO plus 2nd LFO with phase, key reset and filter depth | Extend |
| Filter, FEG | None | Full AWM2-class filter | Add the shared filter block |
| Macros, key-on delay, pan modes, portamento | Partial portamento | Full | Add |
| Format | CLAP, VST3 | – | – |

**Assessment:** Dexed matches about a DX7's worth of FM-X: the per-operator core, the EG and
level-scaling model, velocity and feedback. That's the hard-won part, with years of
DX7-accuracy work behind it. Everything FM-X added on top is missing: 8 operators, 88
algorithms, spectral waveforms, filter, extra LFO and EG stages, and macros. Dexed's code is
organized around the fixed DX7 voice, so extending it means restructuring it, not
configuring it.

**Recommendation:** use MSFA's operator, EG and scaling code (Apache-2.0, compatible with
AGPL) as the starting point for the operator core. Write the voice structure, the general
operator graph and the spectral oscillators in Oscairn. Keep DX7 SysEx import as a
first-class feature: a DX7 voice is a strict subset of the engine. Reusing GPL-3.0 Dexed code
is also licence-compatible (GPLv3 §13 allows combining with AGPLv3), but it isn't needed if
MSFA covers the core. Pin and credit whatever is reused.

## Build order

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | Operator core from MSFA, general graph, all 88 algorithms, feedback; native sound contexts ([RFC 0005](0005-sound-contexts.md)) | DX7 SysEx banks render like Dexed (reference renders); every algorithm passes a topology test |
| 2 | 8 operators, fixed frequency, FM-X EG with hold, level and pitch scaling | Measured against ESP renders within RFC 0013-style tolerances |
| 3 | Spectral forms with skirt and resonance | Spectrum tests per form, skirt and resonance setting |
| 4 | Shared filter, FEG, PEG, LFOs, key-on delay and pan modes (from RFC 0013's blocks) | Per-block tests |
| 5 | Macro controls, mono, portamento; converter coverage for FM-X Parts | The factory FM-X set converts with no engine-parameter loss |

## Open questions

- How the spectral forms are generated: fixed spectra, or band-limited operator waveforms
  shaped by skirt and resonance. Measure first.
- Whether Dexed's DX7-accurate quirks (EG rate table, output quantization) belong in a "DX7"
  compatibility mode or are the general behavior.

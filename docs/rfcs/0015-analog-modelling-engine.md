# RFC 0015: An analog-modelling engine

Status: proposed, 2026-10-30. Not implemented. Follows the [architecture](../architecture.md#engines)'s
plan for "an analog-modelling engine that scales from CPU-only devices to supported DSP- or
FPGA-assisted ones". Same rules as [RFC 0013](0013-awm2-class-sample-engine.md): a new engine
written for Oscairn, with no Yamaha code or content.

## Outcome

An Oscairn virtual-analog instrument that can do everything a MONTAGE M / MODX M AN-X Part
can do, with no loss when an AN-X Part is converted, and a voice structure open enough to be
a good general analog engine. Behavior is specified from the Data List and manuals plus
black-box measurements of rendered output.

## Target: what an AN-X Part has

From the MONTAGE M Data List. Factory usage counts cover 814 active AN-X Parts.

| Area | AN-X | Factory usage |
| --- | --- | --- |
| Oscillators | **3**. Waves Saw1, Saw2, Square, Triangle, Sine; octave 64'–1'; pitch ±1200 cents | every Part |
| Per-oscillator shaping | Pulse width (1–99%), Wave Shaper (0–255), Self Sync pitch (0–4800 cents), FM level, Ring level. Each has velocity, EG and LFO depth | self sync 167, osc FM 158, ring 100, wave shaper 92 |
| Per-oscillator routing | Out select (filter or amp), invert, level and velocity | – |
| **Per-oscillator EG and LFO** | Every oscillator has its own ADSR and its own LFO: 5 waves, speed 0–415, 16 phase steps, delay, fade-in, key reset | – |
| Noise | Tone, out select (filter or amp), level and velocity | – |
| Filters | **2**. Thru, LPF24, LPF18, LPF12, LPF6, HPF24, HPF18, HPF12, HPF6, BPF12, BPF6. Cutoff 0–1023, resonance, key follow (off to 2 oct), saturator drive (0–60 dB), out level, EG and LFO depth (±9600 cents), velocity | every Part |
| Modifier | Wave folder (soft or hard, texture), with its own EG and LFO | 30 |
| Amplifier | Level, velocity, key follow, saturator drive, amp EG and LFO | every Part |
| Pitch | Pitch EG and pitch LFO | – |
| Voice character | Unison off, 2 or 4 with detune and spread; OSC Reset (off, phase, tune, full); Voltage Drift; Ageing (±100) | unison 271 |
| Part level | Part LFO with destination ratios, Key On Delay, random, alternate and scaling pan, mono or poly, portamento, controller boxes | – |

## How close existing open-source analog synths are

| Synth (licence) | Matches | Missing compared with AN-X |
| --- | --- | --- |
| **[Surge XT](https://surge-synthesizer.github.io/)** (GPL-3.0) | 3 oscillators per scene (Classic, Modern, Sine, FM2/FM3, …); FM routing between oscillators; ring modulation 1×2 and 2×3; noise with colour; **2 filters** with serial, dual and other routings and many models; waveshaper; unison; oscillator drift; 6 voice and 6 scene LFOs; mono modes | AN-X's per-oscillator EG and LFO (Surge has a flexible mod matrix, so this is a mapping, not a gap); per-oscillator wave shaper and self-sync depth as AN-X defines them; per-oscillator out select to amp (bypassing filters); filter and amp saturators with AN-X's drive range; wave folder with soft/hard and texture; Ageing |
| [Odin 2](https://github.com/TheWaveWarden/odin2) (GPL-3.0) | 3 oscillators, 2 filters, FM and ring, modulation matrix | Similar gaps; fewer filter models than Surge |
| [OB-Xd](https://github.com/reales/OB-Xd) (GPL-3.0) | Classic 2-oscillator architecture and a well-regarded filter | Too small: 2 oscillators, no ring or fold |
| [Vital](https://github.com/mtytel/vital) (GPL-3.0) | Wavetable oscillators can produce AN-X waves; 2 filters; modulation | A wavetable engine, not a VA voice; heavy for embedded targets |

**Assessment:** Surge XT covers most of AN-X's topology: 3 oscillators, noise, 2 filters,
FM, ring, a shaper, unison and drift. Its modulation system can express per-oscillator
envelopes and LFOs. What's missing are AN-X-specific blocks (self sync as a pitch amount,
the per-oscillator wave shaper, the wave folder, saturators, ageing) and AN-X's exact
responses. Surge is a large, scene-based synth with its own patch model, so embedding it
wholesale would bring a second voice and state architecture into Oscairn.

**Recommendation:** write the voice and modulation structure in Oscairn. Evaluate the Surge
team's DSP libraries (sst-filters, sst-waveshapers, sst-basic-blocks; licences to check per
library) for oscillators, filters and shapers, pinned and credited. GPL-3.0 code is
compatible with Oscairn's AGPL (GPLv3 §13). Design the voice so CPU-only and
DSP/FPGA-assisted backends ([RFC 0004](0004-hardware-services.md)) can implement the same
blocks.

## Build order

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | 3 band-limited oscillators with PW, sync, FM and ring; noise; 2 filters with the 11 types; amp; per-section ADSR and LFO; native sound contexts | Aliasing and frequency-response tests; CPU budget on the target |
| 2 | Wave shaper, saturators, wave folder, out-select routing, invert | Per-block measurements against ESP renders |
| 3 | Unison, OSC Reset, drift and ageing; pitch EG and LFO; Part LFO | Statistical tests for drift and ageing; listening |
| 4 | Converter coverage for AN-X Parts | The factory AN-X set converts with no engine-parameter loss |

## Open questions

- What Ageing and Voltage Drift model exactly (static detune, slow random walk, component
  spread). Measure first.
- Whether the AN-X filter types are the same models as the AWM2/FM-X filter of the same
  name. If so, share the code with RFCs 0013 and 0014.

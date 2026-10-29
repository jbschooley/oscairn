# Reference plugins

These are ordinary CLAP modules, built by `plugins/CMakeLists.txt`. They do not depend on
JUCE or a particular panel layout. Start with the [developer guide](../docs/developer-guide.md)
for build commands and the contracts for a new plugin.

| Plugin | Source | Demonstrates |
| --- | --- | --- |
| Basic Synth | [basic_synth.cpp](basic-synth/basic_synth.cpp) | Polyphonic instrument, sample-timed parameters/expression, native sound contexts, channel parts and per-voice output buses |
| Step Sequencer | [step_sequencer.cpp](step-sequencer/step_sequencer.cpp) | Note-only source, transport timing, editable steps and parameter-lock lanes |
| Delay | [delay.cpp](delay/delay.cpp) | Independent native effect contexts, parameter/state APIs, tails and tempo glide |
| Transpose / MIDI Delay | [midi_effect.cpp](midi-effects/midi_effect.cpp), [behavior](midi-effects/README.md) | Held-note mapping, pressure/sustain, bounded sample-timed event queues and portable state |

There is no arpeggiator in the current tree. Automatic Sound/effect-chain management is
not implemented; explicit routing supports independent chains and preserved tails. These small plugins are executable references, not a complete production
synth/preset library. Keep stable IDs and license notices when adapting their source.

## Basic Synth sounds

One instance shares 16 voices across up to eight prepared sound contexts (including
Default) and 16 MIDI channel assignments. Each context owns all eight parameters;
held voices retain their context when new notes switch to another sound. Oscillator
and sine pitch-LFO phases are per voice and restart at note-on.

| Parameter ID | Control | Plain values |
| --- | --- | --- |
| 1 | Brightness | 0–1; envelope opens the two-pole low-pass filter |
| 2 | Level | 0–1 |
| 3 | Pressure | 0–1; aftertouch filter depth |
| 4 | Oscillator | 0 = saw, 1 = detuned saw pair, 2 = square |
| 5 | Detune | 0–50 cents either side of the note, saw-pair mode only |
| 6 | Pitch LFO rate | 0–40 Hz |
| 7 | Pitch LFO depth | 0–200 cents either side of the note |
| 8 | Output bus | 0 = Main, 1–8 = separate stereo sound buses; captured at note-on |

The saw and square edges use PolyBLEP correction; the saw pair is averaged to keep
its level comparable to a single saw. A zero LFO rate or depth disables vibrato.
Continuous parameters support sample-timed automation/modulation; oscillator mode is
stepped. The envelope remains fixed: 2 ms attack, 250 ms exponential decay toward
0.35 sustain, and a 120 ms release time constant. Stereo outputs carry the same signal.

The desktop picker includes **Detuned saws** (±7 cents) and **Square vibrato**
(18 Hz, ±70 cents), alongside Warm, Bright and Pressure swell. Choose a prepared sound
and press **Use sound**. Per-channel assignments are available through `sounds.select`;
the picker switches all channels together. These sounds still share the basic filter
and envelope; there is no general modulation matrix, sample playback or FM synthesis.

The proposed [native portability and DAW delivery contract](../docs/rfcs/0006-packages-and-portability.md)
adds manifest/lock-based installation, shared dependencies and standard-format desktop
exports. "Native" does not require running the Oscairn app: direct plugins should work in
compatible hosts using standard APIs. Current references build as CLAP only; VST3/AU and
individual standalone exports, and the native portability qualification, remain planned.

## Independent Delay contexts

Delay supports `oscairn.effect-contexts/draft`: up to 16 logical effects share an engine,
with independent CLAP lifecycle, parameters, state and histories. Its IDs are 1 = Delay
beats (0.0625–4, default 0.75), 2 = Feedback (0–0.95, default 0.45), and 3 = Wet (0–1,
default 0.3). Dry stays at unity. The history capacity is two seconds; longer beat values
are capped to that capacity at slow tempos. Default tempo without transport is 120 BPM.

Use `Graph::addEffectContext(source)` or `effects.create` through control, then connect
normal audio routes. This preserves serial dependency ordering even when siblings share
an engine. `effects.resources` reports memory/capacity and whether history remains.
See [RFC 0007](../docs/rfcs/0007-effect-contexts.md) for retirement and measurements.

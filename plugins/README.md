# Reference plugins

These are ordinary CLAP modules, built by `plugins/CMakeLists.txt`. They do not depend on
JUCE or a particular panel layout. Start with the [developer guide](../docs/developer-guide.md)
for build commands and the contracts for a new plugin.

| Plugin | Source | Demonstrates |
| --- | --- | --- |
| Basic Synth | [basic_synth.cpp](basic-synth/basic_synth.cpp) | Polyphonic instrument, sample-timed parameters/expression, native sound contexts, channel parts and per-voice output buses |
| Step Sequencer | [step_sequencer.cpp](step-sequencer/step_sequencer.cpp) | Note-only source, transport timing, editable steps and parameter-lock lanes |
| Delay | [delay.cpp](delay/delay.cpp) | Independent native effect contexts, parameter/state APIs, tails and tempo glide |
| Sampler | [sampler.cpp](sampler/sampler.cpp) | One-region SFZ patches over preloaded WAV audio shared between sound contexts, with portable saved assets |
| Transpose / MIDI Delay | [midi_effect.cpp](midi-effects/midi_effect.cpp), [behavior](midi-effects/README.md) | Held-note mapping, pressure/sustain, bounded sample-timed event queues and portable state |

There is no arpeggiator in the current tree. Automatic Sound/effect-chain management is
not implemented; explicit routing supports independent chains and preserved tails. These small plugins are executable references, not a complete production
synth/preset library. Keep stable IDs and license notices when adapting their source.

Basic Synth and Sampler share one implementation of native context ownership, voice
release rules and snapshots: [SoundContexts.h](common/SoundContexts.h).

## Sampler sounds

The Sampler has no parameters: each prepared sound is an SFZ patch in the
`oscairn-sfz-baseline-0` profile of [RFC 0008](../docs/rfcs/0008-sfz-native-samples-and-conversion.md#baseline-profile-oscairn-sfz-baseline-0)
(one `<region>`; `sample`, key/velocity ranges, `pitch_keycenter`, `pitch_keytrack`,
`transpose`, `tune`, `volume`, `ampeg_release`). Anything else is rejected with its line
number. Samples are 16/24/32-bit PCM or 32-bit float WAV, mono or stereo, 8–192 kHz, at
most 64 MiB decoded each and 256 MiB per instance. Contexts with identical audio share one
decoded copy, identified by content rather than file name.

Playback uses linear interpolation; the sample end finishes the voice (`no_loop`);
release falls exponentially to −60 dB over `ampeg_release` and then stops, the curve liquidsfz uses. Velocity gain is squared (the
SFZ `amp_veltrack=100` default). Mono samples play on both outputs. Pressure is tracked
per voice but not mapped to sound. The single stereo output is unsaturated.

Saving uses `clap.resource-directory`: each distinct sample is written once as a float WAV
named by its content hash, and the `oscairn-sampler-1` snapshot stores a normalized SFZ per
sound that refers to it.

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

## Step Sequencer 0.2.0

Two tracks and A/B patterns share transport; each pattern has a 1–16-step length per track.
Tracks have separate MIDI channels and mutes. Track 2 defaults muted, channels default to
MIDI 1 and 2. A launch request waits for the next bar (CLAP time signature, 4/4 fallback),
or selects immediately while stopped. Launch resets both cycle phases; stop/seek/channel
changes/mutes send matching note releases. Sound release tails continue in the instrument.
Sixteenth-note steps use a half-step gate. Swing and variable gate length are not implemented.

[`Parameters.h`](step-sequencer/Parameters.h) is the shared ID contract. All IDs below use
zero-based pattern `p`, track `t`, and step `i`:

| ID | Meaning / values |
| --- | --- |
| 1 | Requested pattern, 0=A / 1=B |
| 2 | Playing pattern, read-only |
| `10 + 10*t` | MIDI channel, 0–15 (displayed as 1–16) |
| `11 + 10*t` | Mute, 0/1 |
| `12 + 10*t` | Playing step, read-only, −1 stopped / 0–15 |
| `13 + 10*t` | Lock scope, 0=global (default) / 1=track channel |
| `10000*p + 1000*t + 50` | Track length, 1–16 |
| `10000*p + 1000*t + 100 + i` | Step enabled, 0/1 |
| `10000*p + 1000*t + 200 + i` | MIDI note, 0–127 |
| `10000*p + 1000*t + 300 + i` | Normalized lock, 0–1 |

There are 206 parameters, 203 writable. The parameter-only snapshot captures all patterns,
channels, mutes, scopes and the launch request; live playheads/queued timing are not persisted.
Performance reopening stops/rewinds and makes the saved request active. Recreate development
saves after incompatible changes; supporting earlier Oscairn plugin versions is not required
by the [development policy](../docs/developer-guide.md#initial-development-compatibility-policy).

Lanes 0/1 belong to tracks 1/2. Locks precede their notes and use wildcard note/key/port;
channel scope selects the track's channel. Native contexts support this addressing.
Global is the plugin default so generic mappings to global-only targets keep working.
For an ordinary LV2 control port or a CLAP parameter without per-channel automation, keep
that track's scope global. Global locks affect the entire target; two lanes
mapped to the same global parameter are ordered by track. The desktop Brightness locks
control sets channel scope when enabling its lane. If two channels select the same native
context, they share its patch edits; select distinct contexts for independent locks.

`sequencer-tracks` checks different lengths/channels, mute/channel release identity,
inactive edits, launch cancellation and exact 3/4 and 4/4 boundaries across 37/512-frame
blocks with allocation guards. Performance and sound-picker tests cover persistence and
channel-specific sound/mapping selection. Audible acceptance is tracked separately.

# Reference plugins

These are ordinary CLAP modules, built by `plugins/CMakeLists.txt`. They do not depend on
JUCE or a particular panel layout. Start with the [developer guide](../docs/developer-guide.md)
for build commands and the contracts for a new plugin.

| Plugin | Source | Demonstrates |
| --- | --- | --- |
| Basic Synth | [basic_synth.cpp](basic-synth/basic_synth.cpp) | Polyphonic instrument, sample-timed parameters/expression, native sound contexts and channel parts |
| Step Sequencer | [step_sequencer.cpp](step-sequencer/step_sequencer.cpp) | Note-only source, transport timing, editable steps and parameter-lock lanes |
| Delay | [delay.cpp](delay/delay.cpp) | Stereo effect, tempo-synced delay and smooth tempo changes |
| Transpose / MIDI Delay | [midi_effect.cpp](midi-effects/midi_effect.cpp), [behavior](midi-effects/README.md) | Held-note mapping, pressure/sustain, bounded sample-timed event queues and portable state |

There is no arpeggiator in the current tree. Native effect-context transitions are also
not implemented. These small plugins are executable references, not a complete production
synth/preset library. Keep stable IDs and license notices when adapting their source.

# MIDI effects

Two ordinary CLAP note effects share [midi_effect.cpp](midi_effect.cpp). They have no audio
ports, one note input/output, and parameter ID 1. They accept CLAP notes/expressions and
MIDI 1.0; note output uses CLAP identities, with other MIDI channel messages passed through.
The Oscairn graph translates for MIDI-only destinations.

| Module / descriptor ID | Parameter 1 | Behavior |
| --- | --- | --- |
| `oscairn-transpose.clap` / `org.oscairn.transpose` | Semitones, integer −48…48, default 0 | Transpose each new note; out-of-range notes are suppressed |
| `oscairn-midi-delay.clap` / `org.oscairn.midi-delay` | Milliseconds, integer 0…2000, default 250 | Delay one event stream, including releases, pressure and controllers |

Transpose remembers the output key and identity for each input note. Moving its parameter
while holding a note changes subsequent presses; the held note's release and expression
still address its original output. Raw MIDI repeated keys release in press order.
Sustain retains expression ownership until pedal-up. `NOTE_END` returns the input address
when this processor finishes owning it; downstream instruments own their audio release tails.

MIDI Delay is a single delayed copy, with no dry signal or repeated echoes. Connect the
source directly to a synth **and** through the delay for a dry/wet note layer. Delay changes
wait until the event stream has no queued messages or owned notes, preventing releases or
controllers overtaking earlier notes. Milliseconds are rounded to samples at the actual
sample rate; delivery is independent of block size. A scoped choke cancels pending notes
immediately. Transport's playing-to-stopped transition cancels pending events and chokes
sounding output; live input while already stopped still works. MIDI clock/transport bytes
are not a second transport implementation; hosts supply the CLAP transport.

Each instance reserves 128 note mappings and 1024 queued events. Overflow or rejected output
triggers a bounded choke/end recovery instead of retaining notes with lost releases. Host
reset also cancels queued work at the next process call. Both modules save parameter 1 in a
versioned eight-byte, byte-order-independent CLAP state blob; transient notes/queues are not
presets. No memory allocation, file access or locks occur in processing.

Example offline chain, with the default 250 ms note delay:

```sh
build/hosts/oscairn-render delayed.wav --seconds 4 \
  build/plugins/oscairn-step-sequencer.clap \
  build/plugins/oscairn-midi-delay.clap \
  build/plugins/oscairn-basic-synth.clap
```

For custom routing use `graph.add`, `graph.connect` with `kind:"notes"`, `params.set` and
`graph.commit` through the [control API](../../docs/control-protocol.md). An ordinary MIDI
delay plugin does **not** legalize a feedback cycle; graph-level delayed returns are planned.
Tests compare a transpose/delay/synth chain with directly scheduled notes, check 37/512-frame
schedules, pressure/sustain, repeated keys, state, stop/overflow recovery and mixed audio/MIDI
routing, including cycle rejection.

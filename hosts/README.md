# Hosts

Programs built on the core: a headless host that renders to file (for CI), a desktop host with
live audio and MIDI, and later the device firmware and the platform as a DAW plugin.

- `render/`: `oscairn-render`, the headless host.
- `standalone/`: `oscairn-standalone`, the demo instrument as a desktop app (JUCE 9): the
  step sequencer and pads playing the basic synth through the delay, with live audio, every
  MIDI input playing it (notes and aftertouch, and clock when MIDI SYNC is on), and a choice of panel
  layouts (`standalone/layouts/`). The UI reaches the engine only through `Engine`'s
  public methods (nodes by name, parameters by id, plain values), which delegate to
  `core/control`'s in-process `Control`. `Engine` retains device I/O and timestamp conversion;
  the core `Session` owns the graph and transport. The app is still one process.

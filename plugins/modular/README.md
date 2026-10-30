# Modular

chillbeast's *Modular* instrument engine from his KICK firmware fork, to become an Oscairn
CLAP instrument. See the [evaluation](../../docs/modular-engine-evaluation.md).

## Upstream

`upstream/` is imported with its history (`git subtree`) from
[chillbeast/0700-AAM](https://github.com/chillbeast/0700-AAM), branch `desktop-build` at
`3598ab49` (v0.1.5), path `Kick/modules/engine/audio_program/instrument/`: `modular/` and
the `mt_instrument_engine.h` interface it implements. AGPL-3.0-or-later; the notices in
the files are authoritative.

Keep `upstream/` unchanged so later versions merge cleanly. Oscairn's code (the CLAP
wrapper, tests) lives beside it, not inside it; offer fixes to chillbeast upstream.

To update:

```sh
git clone --single-branch -b desktop-build https://github.com/chillbeast/0700-AAM.git /tmp/modular
cd /tmp/modular
D=Kick/modules/engine/audio_program/instrument
git filter-repo --path $D/mt_instrument_engine.h --path $D/modular/ --path-rename $D/:
cd -   # back to the Oscairn root
git subtree pull --prefix=plugins/modular/upstream /tmp/modular desktop-build
```

## Oscairn plugin

`org.oscairn.modular` ([src/modular.cpp](src/modular.cpp)), built with `-DOSCAIRN_MODULAR=ON`
or with the desktop app, which bundles it. It links JUCE 9 (`juce_core`,
`juce_audio_basics`, `juce_data_structures`); [src/prefix.h](src/prefix.h) supplies what
KICK's build included for the upstream files, and [src/compat.cpp](src/compat.cpp) puts
user presets under Oscairn's data directory.

- **Notes:** CLAP notes or MIDI, with velocity, pitch bend (±2 semitones), mod wheel,
  polyphonic and channel pressure, sustain pedal, all-notes-off. Each event reaches the
  engine at its sample: the block is rendered in pieces between events.
- **Parameters:** the engine's Macro 1–8 and Level, plus a stepped Preset that loads one of
  the factory patches on the main thread.
- **State:** parameter values, the preset index and the patch as XML.
- **Not yet:** the patch editor (RFC 0011 structured editors), user presets from Oscairn,
  native sound contexts, and NOTE_END reporting (the engine does not say when a voice ends).

The engine's 16-sample modulation grid restarts at every render call, so output changes
slightly with block size and event positions until that is fixed upstream.

`modular` test: notes make sound; the same input renders the same output; a second preset
sounds different; saved state restores the same, non-default patch; rendering never
allocates. `worker-host` was also run on it by hand: identical in a worker process.

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

## Status

Not built yet. The CLAP wrapper is the next step for this component: events split at their
sample, the eight macros and level as parameters, the patch as state, factory presets. It
links JUCE (`juce_core`, `juce_audio_basics`, `juce_data_structures`), which the engine
and patch code use. The engine's 16-sample modulation grid restarts at every `process()`
call, so output changes with block splits until that is fixed upstream.

# Sequencer evaluation

Research note, 2026-10-30. Roadmap [Phase 3b, item 4](roadmap.md#next-build-and-test-order) (Phase 3, item 7, when written): before extending Oscairn's own step
sequencer, compare reusing the KICK firmware's sequencer (Behringer's release plus chillbeast's
work), porting Zynthian's zynseq, and hosting an existing sequencer plugin. **Nothing is
decided or imported yet.** The current [`plugins/step-sequencer`](../plugins/step-sequencer/step_sequencer.cpp)
was written to exercise the host's contracts (sample-timed notes, parameter locks, MIDI clock,
bar-aligned launches) and stays as a test fixture whatever is chosen.

## Candidates

### KICK firmware sequencer

Source: `Kick/modules` in the [0700-AAM](https://github.com/MusicTribeOpenSource/0700-AAM)
release, as extended in [chillbeast's fork](https://github.com/chillbeast/0700-AAM) (`desktop-build`,
checked out locally). AGPL-3.0-or-later, the same as Oscairn.

- **Model and workflow (the valuable part).** MPC-style: sequences of tracks with a length in
  bars, loop points, per-sequence or master tempo with a tempo map, transpose, solo and mute,
  next-sequence chaining at the end of a sequence, record arming and MIDI-file export
  (`engine/audio_engine/engine/mt_audio_sequence.*`, `mt_audio_track.*`). Notes live in a
  1,600-line note list with editing operations: copy and insert bars, time-quantize to a grid,
  pitch-quantize, list editing (`mt_audio_note.*`). Swing appears only in the list-edit screen,
  not in playback.
- **chillbeast's contribution to sequencing** is small and clean: about 180 lines across seven
  commits, chiefly *Pad recording into sequences* (`fa8b3220`): live notes are recorded into
  the playing track at the current tick, and each take undoes as one step. His larger work, the
  Modular instrument engine with patch editor, effects, presets and an arpeggiator, is an
  instrument, not a sequencer, and is worth a separate look as an Oscairn instrument plugin.
- **Timing (the part that does not carry over).** Ticks come from a `juce::HighResolutionTimer`
  (`audio_midi/source/CMidiClock.*`, 24–960 PPQN); a separate thread waits for each tick and calls
  `playingSequence->onTick()` (`mt_audio_manager_transport.cpp:284-300`). Tracks emit
  `juce::MidiMessage` lists with no sample positions (`mt_audio_track.cpp:285-325`). Events are
  therefore timed by a timer thread, not placed on samples in the audio block, and offline
  renders are not deterministic. An Oscairn sequencer plugin must instead compute events inside
  `process()` at exact sample offsets.
- **Coupling.** State is `juce::ValueTree` with `juce::UndoManager`; classes depend on the
  firmware's `MTAudioManager`, global playhead, tempo manager and `sigslot` signals, and the
  files compile as part of a JUCE module. Extracting means adapting, not copying.
- **History.** Behringer released the firmware as one squashed commit; the sequencer files have
  three commits in all (MUSIC Tribe's release, chillbeast's desktop checkpoint, Randy Chabot's
  PR #13 change) plus chillbeast's later sequencing commits. `git filter-repo` limited to the
  sequencer paths can import them with their authors, as `git subtree` did for the CLAP host;
  credit then rests mainly on author fields, file headers and CREDITS.

### zynseq

Source: `zynlibs/zynseq` in [zynthian-ui](https://github.com/zynthian/zynthian-ui) (checked at
`f43b7da5`, 2026-10-28), by Brian Walton. GPL-2.0-or-later in file headers (the bundled
`LICENSE.txt` is GPL-3.0): compatible with Oscairn's AGPL-3.0-or-later.

- **Model.** Scene → phrase → sequence → track → pattern, with step events, per-pattern swing
  (amount and division), a tempo map, MIDI clock in and out. About 10,000 lines of C++.
- **Timing.** Events are written at frame offsets within each JACK period (`zynseq.cpp:179-240`):
  sample-accurate within the block, like Oscairn's plugins.
- **JACK is confined to `zynseq.cpp`** (63 references); `pattern`, `sequence`, `track`,
  `timebase` and `sequencemanager` have none. A port replaces one file's JACK glue with CLAP
  `process()` and transport, which is much smaller than first assumed. Its own comment notes
  a vector that may allocate on the audio thread (`zynseq.cpp:72`); that must be fixed.
- **Workflow.** Launcher-style patterns and scenes (Zynthian's pad grid), not MPC-style linear
  sequences.

### Existing sequencer plugins

No mature, maintained open LV2 or CLAP step sequencer was found. [Lockstep](https://github.com/chalkwalk/lockstep/wiki)
is a promising performance step sequencer with a CLAP build, but unreleased (built from source;
wiki updated October 2026). [B.SEQuencer](https://wiki.linuxaudio.org/_export/xhtml/apps/all/bsequencer)
(LV2, 2021) and the [MOD step sequencer](https://aur.archlinux.org/packages/mod-step-sequencer.lv2-git)
(LV2, alpha, 2021) are not current. Noteahead, Non-Sequencer and Seq66 are applications, not
plugins. Oscairn can host any of them once they suit, but none should be the main sequencer.

## Recommendation

1. **Take the KICK sequencer's model and editing workflow**, including chillbeast's pad
   recording: it is the workflow KICK owners already know, and the hardware's controls were
   designed around it. Import the sequencer files with their history and credit (MUSIC Tribe,
   chillbeast, Randy Chabot), keeping the imported commits separate from Oscairn's adaptation.
2. **Replace its timing with sample-accurate playback inside an Oscairn CLAP sequencer plugin**,
   computing note events in `process()` from the CLAP transport, as the current test sequencer
   does. Borrow zynseq's frame-offset scheduling and swing where useful, with credit.
3. **Keep zynseq's scenes and phrases in view** as a possible second mode, if launcher-style
   performance matters on the device.
4. **Do not make a hosted third-party sequencer the default**; re-check Lockstep when released.

## Decisions needed from Jacob

- MPC-style linear sequences (KICK) as the main workflow, launcher-style patterns and scenes
  (zynseq), or both.
- Whether the sequencer plugin may keep JUCE (`ValueTree`, undo) internally, or the model is
  ported to plain C++ with undo handled by Oscairn's control layer. The core avoids JUCE; a
  plugin may use it, at a cost in size and in sharing code with non-JUCE hosts.
- Chillbeast's Modular engine as an Oscairn instrument plugin: evaluated separately in
  [Modular engine evaluation](modular-engine-evaluation.md).

Not done: zynseq's pattern and timebase code was skimmed, not reviewed in depth; neither
candidate was built or run here.

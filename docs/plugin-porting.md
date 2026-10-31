# Porting a plugin to Oscairn

For developers bringing an existing CLAP plugin to Oscairn. A plugin that already follows
the CLAP specification usually needs **no changes** to load and play. This page lists what
the host requires, what it does not support yet, and the optional Oscairn extensions that
make a plugin fit the instrument better. LV2 plugins load too when Oscairn is built with
LV2 (see the [developer guide](developer-guide.md)); this page is about CLAP.

This page describes the current host and is updated as features land. Each fact below was
checked against `core/clap-host/src/Host.cpp` and `core/plugin-host/` unless it says
otherwise.

## Required: what the host checks when it loads a plugin

A plugin that fails one of these is refused with the message shown.

Presets are tested with a fixture plugin (`clap-presets`); no third-party plugin has been
tried yet.

| Requirement | Refused with |
| --- | --- |
| A `clap_entry` with a plugin factory | "No CLAP plugin factory" |
| Every function of the plugin interface (`init`, `activate`, `process`, `get_extension`, `on_main_thread` …) | "Incomplete CLAP plugin interface" |
| Audio outputs, note outputs or both (a pure tuner or meter has neither) | "Plugin has neither audio nor note outputs" |
| At most 64 audio channels in each direction across all ports; no port without channels | "Audio bus count exceeds the 64-channel budget", "Invalid or unsupported audio channel layout" |
| At most 16 note ports in each direction | "More than 16 note ports are unsupported" |
| If it has the params extension, all its functions; every parameter with a unique ID and finite minimum ≤ default ≤ maximum; at most 32,768 | "Incomplete CLAP parameter interface", "Invalid CLAP parameter metadata", "Duplicate CLAP parameter ID", "Plugin advertises too many parameters" |
| If it has the state or audio-ports extension, all its functions | "Incomplete plugin state interface", "Incomplete CLAP audio-ports interface" |
| Activates at the host's sample rate and maximum block size | "Plugin activation failed" |
| An audio port layout that does not change while loaded (parameter and note-port changes need a restart request) | "CLAP audio port layout changed; reload this graph node before reconnecting audio" |
| A build for the machine: macOS for the desktop, Linux ARM64 for the device | the scanner does not list it |

Everything else is handled for you:

- **Notes:** the host offers CLAP notes, MIDI and MPE, and converts at the plugin's
  boundary. A plugin that only reads MIDI, or only CLAP notes, works.
- **Audio:** 32-bit float buffers. CLAP requires every plugin and host to support 32-bit
  audio; 64-bit is optional (`CLAP_AUDIO_PORT_SUPPORTS_64BITS`) and Oscairn does not offer
  it, so no plugin is excluded by it.
- **Blocks:** any length up to the maximum, with events at their sample offsets; at most 512
  events from the graph per block.
- **Host extensions:** thread-check, timer support, POSIX file descriptors, tail, log,
  latency, event registry, params, state, audio ports, note ports and preset-load.
- **Presets:** a plugin's library that offers CLAP preset discovery has its presets listed in
  Oscairn's sound lists: presets inside the plugin, and preset files of its declared types in
  its declared folders (at most 10,000 per folder), each only for the plugins it names. One
  is loaded with the plugin's preset-load extension, never during `process`, and can be
  prepared as an instant-switch sound. Plugins that only offer presets in their own window
  have none listed.

## Known limits

- **The plugin's own window is not shown yet.** The host does not implement CLAP's GUI
  extension; every plugin gets a generated editor of knob pages from its parameters. The
  plan ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#plugin-editors)): the plugin's
  window on the desktop, and on the device editors Oscairn draws, from the parameters or
  from a description the plugin gives (below).
- **Without the state extension**, a Performance stores only parameter values, for plugins
  with up to 2,048 writable parameters. Implement state if the sound depends on anything
  else.
- **Worker processes** ([RFC 0010](rfcs/0010-plugin-worker-isolation.md)) are optional and
  not yet used by the desktop app. In a worker, LV2 patch properties are not forwarded,
  a block that runs longer than half the block's time is silenced, and a plugin that keeps
  missing is restarted with its last loaded state and current parameter values.
- **Instant sound switching** for plugins without native sound contexts uses a whole extra
  instance per prepared sound (up to eight), and only for instruments: note input, audio
  output, no audio input, no note output.
- **Not implemented:** remote controls, voice info, note names, audio-ports configurations,
  render mode, and watching preset folders for changes (they are read when listed). (From
  the host's source; not exercised with a plugin.)

## Recommended: behaving well

- **Real-time safety:** no allocation, locks, file access or waiting in `process`. In a
  worker process, the time limit enforces this.
- **State:** implement the state extension; make it deterministic and independent of CPU
  architecture, and keep ports, note dialect and parameter metadata the same whatever state
  is loaded. Saving, instant switching, crash recovery and moving Performances between the
  desktop and the device all depend on it.
- **CLAP notes,** not only MIDI: note IDs keep repeated notes on one key apart, per-note
  expression works, and `NOTE_END` tells the host when a voice and its tail have finished.
- **Chokes and per-note volume:** treat a host `NOTE_CHOKE` as a fast fade of 2–5 ms, not a
  cut, and honour `CLAP_NOTE_EXPRESSION_VOLUME`. The planned voice budget
  ([RFC 0018](rfcs/0018-voice-budget-and-stealing.md)) fades a stolen note with the volume
  expression, then chokes it; without both, stolen notes click.
- **Parameter metadata:** clear names, CLAP module paths for grouping, value text, and the
  stepped and read-only flags. The generated editors are built from them.

## Optional Oscairn extensions

Headers are in `core/clap-ext/include/oscairn-clap/`, under the MIT licence, so plugins of
any licence can include them. Each is optional; a plugin without them works as above.

| Extension | What it gives | Example |
| --- | --- | --- |
| Native sound contexts (`sound-contexts.h`, [RFC 0005](rfcs/0005-sound-contexts.md)) | Multitimbral playing: up to 8 prepared sounds in one instance, each MIDI channel choosing one, held notes keeping theirs, without an extra instance per sound. New native Oscairn instruments implement it. | Basic Synth, Sampler |
| Native effect contexts (`effect-contexts.h`, [RFC 0007](rfcs/0007-effect-contexts.md)) | Many independent effect slots sharing one engine's memory, with tails reported. | Delay |
| Parameter-target lanes (`param-target.h`, [RFC 0002](rfcs/0002-param-target.md)) | For sequencers and modulators: change other plugins' parameters at an exact sample in the same block (parameter locks). | Step Sequencer |
| Sound assets (`assets.h`, [RFC 0009](rfcs/0009-sound-assets-and-dependencies.md)) | For samplers: the host resolves sample files, and Performances list their samples so they can be checked and moved between machines. | Sampler |
| Editor descriptions ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#plugin-editors)) | Planned: describe an editor (a module grid, a sample map, an envelope) for Oscairn to draw in the system theme with the device's knobs. Not defined yet. | Modular, first |

## Testing

Copy the reference plugins' tests in `core/engine/tests/` and `plugins/`: deterministic
offline rendering with `oscairn-render`, rendering that never allocates, state round trips,
and the same tests with the plugin in a worker process (the `*-workers` tests). Run them
under AddressSanitizer and ThreadSanitizer. See
[Test before publishing a plugin](developer-guide.md#test-before-publishing-a-plugin).

## Licences

Keep a reused engine's licence and notices with it and credit its authors in
[CREDITS](../CREDITS.md). Chillbeast's Modular shows the pattern: imported unchanged with its
history, wrapped as a CLAP plugin beside it ([plugins/modular](../plugins/modular/README.md)).

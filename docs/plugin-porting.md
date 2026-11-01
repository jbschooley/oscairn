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
  extension; every plugin gets a generated editor from its parameters. On the desktop it is
  pages of tiles: a page per top-level CLAP module path, a heading per module below that, and
  a tile per parameter. Every parameter that is not hidden is reachable there, so anything a
  sound depends on must be a parameter (or a structured editor, below) to be editable
  without the plugin's window. The
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
  cut, and honor `CLAP_NOTE_EXPRESSION_VOLUME`. When a part's note limit steals a note, the
  host lowers its volume expression to silence over 5 ms in eight steps, then chokes it
  ([RFC 0018](rfcs/0018-voice-budget-and-stealing.md)); a plugin that does neither clicks. An instrument built on the
  SDK's `sound-contexts-plugin.hpp` gets both: it multiplies each voice's samples by
  `amplitude(voice, offset)`, and a choked voice fades over 3 ms before it ends.
- **Say when you are silent:** return `CLAP_PROCESS_SLEEP` from `process` when the plugin will
  make no sound until it gets input or events (an instrument with no voice playing, an effect
  whose tail has ended). A plugin switched off by a scene, or left behind by a Performance
  change ([RFC 0021](rfcs/0021-sounds-performances-and-library.md#seamless-switching)), plays
  out its held notes and tail and stops once it is idle: one that sleeps stops as soon as it
  is done; one that never does waits for three seconds of silence. Until then it runs beside
  the new Performance, so memory and CPU for both are needed for a moment.
- **Parameter metadata:** clear names, CLAP module paths for grouping, value text, and the
  stepped and read-only flags. The generated editors are built from them:
  - the first level of the module path names the page (`Operator 1`, `LFO 2`); numbered pages
    of one kind are kept together in number order. A page of more than 24 parameters gets a
    tab for each second level (`Operator 1/Envelope`); the rest of the path is a heading. So
    give a large page second levels of a dozen or so parameters each;
  - a search box finds parameters on any page by name or module, so keep names specific;
  - an envelope's, LFO's or filter's parameters recognized by name (below) are linked into
    one group of tiles, as a hint's group would;
  - an envelope is drawn as a curve you can drag when one module holds its parameters under
    one of these sets of names: `Attack`, `Decay`, `Sustain`, `Release` (optionally `Delay`
    and `Hold`); `Rate 1`–`4` with `Level 1`–`4` (DX7); or `Attack Time`, `Attack Level`,
    `Decay 1 Time` and so on with `Release Time` (FM-X). A stopgap until plugins can describe
    their editors;
  - an LFO's waveform is previewed when one module has a `Wave` (or `Type`, `Shape`) with a
    `Speed` (or `Rate`, `Frequency`), and optionally `Delay`, `Fade In`, `Phase` and
    `User Step 1`… The wave's value text picks the drawing (sine, triangle, square, saw up or
    down, sample and hold or noise, user steps); others show as drawn by the plugin;
  - list parameters in the order a player reads them (attack before decay), since tiles follow
    the plugin's order within a page;
  - a stepped parameter with 2 to 4 values is a row of buttons (Off and On: a switch), with
    up to 64 a list of their value texts, otherwise a slider; a symmetric range with a
    default of 0 fills from the center;
  - a filter's response is drawn when one module has `Cutoff` and `Resonance`, and its
    `Type`'s value text names it (`LP 24 dB`, `LPF24D`, `HPF12`, `BEF6`; `Off` or `Thru`).

## Optional Oscairn extensions

Headers are in `core/clap-ext/include/oscairn-clap/`, under the MIT license, so plugins of
any license can include them. Each is optional; a plugin without them works as above. For native
sound contexts, `sound-contexts-plugin.hpp` (also MIT) implements the extension's slot,
selection, voice-ownership and snapshot rules; an instrument supplies its patch and voice DSP,
as the Basic Synth and Sampler do. A plugin in its own repository vendors the folder at a
pinned commit.

| Extension | What it gives | Example |
| --- | --- | --- |
| Native sound contexts (`sound-contexts.h`, [RFC 0005](rfcs/0005-sound-contexts.md)) | Multitimbral playing: up to 8 prepared sounds in one instance, each MIDI channel choosing one, held notes keeping theirs, without an extra instance per sound. New native Oscairn instruments implement it. | Basic Synth, Sampler |
| Native effect contexts (`effect-contexts.h`, [RFC 0007](rfcs/0007-effect-contexts.md)) | Many independent effect slots sharing one engine's memory, with tails reported. | Delay |
| Parameter-target lanes (`param-target.h`, [RFC 0002](rfcs/0002-param-target.md)) | For sequencers and modulators: change other plugins' parameters at an exact sample in the same block (parameter locks). | Step Sequencer |
| Sound assets (`assets.h`, [RFC 0009](rfcs/0009-sound-assets-and-dependencies.md)) | For samplers: the host resolves sample files, and Performances list their samples so they can be checked and moved between machines. | Sampler |
| Editor hints (`ui.h`, `oscairn.ui/draft-2`, [RFC 0019](rfcs/0019-adaptive-ui.md)) | A hint per parameter: the control it wants (knob, slider, buttons, switch, list, or a range with its high parameter), a priority, 1 first, which orders its tiles and will order the physical knobs, and a group: parameters of one module in the same group are drawn joined, edge to edge, and kept together on narrow screens. The host keeps a hint only where it fits (a switch on two values, a range whose high end exists). Every parameter stays reachable either way. Describing whole editors (a module grid, a sample map) comes later. | Basic Synth |

## Testing

Copy the reference plugins' tests in `core/engine/tests/` and `plugins/`: deterministic
offline rendering with `oscairn-render`, rendering that never allocates, state round trips,
and the same tests with the plugin in a worker process (the `*-workers` tests). Run them
under AddressSanitizer and ThreadSanitizer. See
[Test before publishing a plugin](developer-guide.md#test-before-publishing-a-plugin).

## Licenses

Keep a reused engine's license and notices with it and credit its authors in
[CREDITS](../CREDITS.md). Chillbeast's Modular shows the pattern: imported unchanged with its
history, wrapped as a CLAP plugin beside it ([plugins/modular](../plugins/modular/README.md)).

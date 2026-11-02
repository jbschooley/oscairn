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
| `init` and `destroy` before `init`; once `init` returns true, every other function of the plugin interface (`activate`, `process`, `get_extension`, `on_main_thread` …), as plugins built on clap-helpers fill them in during `init` | "Incomplete CLAP plugin interface" |
| Audio outputs, note outputs or both (a pure tuner or meter has neither) | "Plugin has neither audio nor note outputs" |
| At most 64 audio channels in each direction across all ports; no port without channels | "Audio bus count exceeds the 64-channel budget", "Invalid or unsupported audio channel layout" |
| At most 16 note ports in each direction | "More than 16 note ports are unsupported" |
| If it has the params extension, all its functions; every parameter with a unique ID and a finite minimum ≤ maximum (a default outside that range, or not a number, is brought into it: it only misplaces a reset); at most 32,768 | "Incomplete CLAP parameter interface", "Invalid CLAP parameter metadata", "Duplicate CLAP parameter ID", "Plugin advertises too many parameters" |
| If it has the state or audio-ports extension, all its functions | "Incomplete plugin state interface", "Incomplete CLAP audio-ports interface" |
| Activates at the host's sample rate and maximum block size | "Plugin activation failed" |
| An audio port layout that does not change while loaded (parameter and note-port changes need a restart request) | "CLAP audio port layout changed; reload this graph node before reconnecting audio" |
| A build for the machine: macOS for the desktop, Linux ARM64 for the device | the scanner does not list it |

Everything else is handled for you:

- **Notes:** the host offers CLAP notes, MIDI and MPE, and converts at the plugin's
  boundary. A plugin that only reads MIDI, or only CLAP notes, works.
- **Hardware MIDI input:** from a keyboard or controller, a plugin gets notes, polyphonic
  aftertouch and channel pressure (as CLAP notes and the pressure note expression, or as MIDI),
  and, as MIDI, pitch bend and the performance controllers: CC 1 mod wheel, CC 2 breath,
  CC 4 foot, CC 11 expression, CC 64–67 (sustain, portamento, sostenuto and soft pedals),
  CC 120 all sound off and CC 123 all notes off. Other controllers and program changes do
  not reach plugins; physical-control mapping routes them. Clock, start, continue, stop and
  song position drive the transport. To hear pitch bend and the mod wheel, a plugin offers
  the MIDI dialect on its note port.
- **Hardware MIDI output:** a part can send its MIDI to hardware outputs too ([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md#step-1-status)).
  A sequencer's or MIDI effect's note output reaches them as MIDI: notes, pressure (as
  aftertouch) and its MIDI events' channel messages; parameter events and other CLAP events
  stay inside.
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
- **Worker processes** ([RFC 0010](rfcs/0010-plugin-worker-isolation.md)): the desktop app
  runs every CLAP plugin in its own worker process, so a crash, hang or overrun silences only
  that plugin (`OSCAIRN_PLUGIN_WORKER=`, set but empty, runs plugins in the engine instead).
  Test your plugin both ways. In a worker, LV2 patch properties are not forwarded,
  a block that runs longer than half the block's time is silenced, and a plugin that keeps
  missing is restarted with its last loaded state and current parameter values. A worker may
  use 4 GiB of memory (`OSCAIRN_WORKER_MEMORY_MB` changes it), or, when its plugin declares its
  memory (`oscairn.memory`, below), what it declares plus half again and 64 MiB; past that it is
  stopped and restarted the same way. A plugin that holds more than it declares is stopped there.
- **Instant sound switching** for plugins without native sound contexts uses a whole extra
  instance per prepared sound (up to eight), and only for instruments: note input, audio
  output, no audio input, no note output.
- **Not implemented:** remote controls, voice info, note names, audio-ports configurations,
  render mode, and watching preset folders for changes (they are read when listed). (From
  the host's source; not exercised with a plugin.)

- **Moving to another device:** an exported Performance or Sound
  ([archive](library-archive.md)) carries every build of each CLAP plugin it uses that the
  exporting machine has: its own, and other platforms' added to its plugin store, and
  installing it puts them in the store (`Plugins/clap/<id>@<version>/<platform>/` in the data
  folder). A Performance made on a Mac opens on Linux with a stand-in unless a Linux build of
  the same version went with it or is installed. Keep the ID and version stable across platforms, and keep everything
  the plugin needs inside its bundle; files it installs elsewhere do not go with it.
- **A plugin that is not here,** in another version, or that fails to load opens as a
  stand-in: silent, its ports and parameters as saved, its state written back unchanged, so
  installing the plugin later brings the sound back.

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
- **Pitch bend and the mod wheel:** apply MIDI pitch bend per channel, to held and released
  voices, without zipper noise. An instrument built on the SDK keeps both per channel; calling
  `followControls(voice)` each sample moves the voice's `bend` (-1 to 1 of the instrument's
  range) and `modWheel` (0 to 1) smoothly toward its channel's. The instrument sets the range
  and what the wheel does; Basic Synth bends ±2 semitones by default and uses the wheel for
  vibrato. Per-note bend (`CLAP_NOTE_EXPRESSION_TUNING`, MPE) is not applied by the SDK.
- **Say when you are silent:** return `CLAP_PROCESS_SLEEP` from `process` when the plugin will
  make no sound until it gets input or events (an instrument with no voice playing, an effect
  whose tail has ended). A plugin switched off by a scene, or left behind by a Performance
  change ([RFC 0021](rfcs/0021-sounds-performances-and-library.md#seamless-switching)), plays
  out its held notes and tail and stops once it is idle: one that sleeps stops as soon as it
  is done; one that never does waits for three seconds of silence. Until then it runs beside
  the new Performance, so memory and CPU for both are needed for a moment; when both would not
  fit the device's budget, the change is a cut instead. A setlist prepares its next
  Performance while one plays: that one's plugins are created, activated and given their saved
  state, and get their main-thread callbacks, perhaps minutes before they first process.
- **Keep your cost steady and your version honest:** the host measures each plugin's CPU per
  block and its memory on the device, by plugin ID and version, and refuses a Performance, or
  an added plugin, that would go over the device's budget, saying why
  ([RFC 0025](rfcs/0025-resource-budgets.md)). CPU is a slowly falling high-water mark of its
  block times, so a heavy passage counts for minutes after it, split into a cost at rest and a
  cost per sounding note; in a part with a note limit, the estimate is at rest plus that many
  notes, so keep your per-voice cost proportional to the voices actually sounding. In the engine's process,
  memory is what the process grew by while the plugin loaded. A plugin never measured on the
  device counts as 2% of the block's time and 32 MiB until it is, and a new version starts
  unmeasured.
- **Parameter metadata:** clear names, CLAP module paths for grouping, value text, and the
  stepped and read-only flags. The generated editors are built from them:
  - the first level of the module path names the page (`Operator 1`, `LFO 2`); numbered pages
    of one kind are kept together in number order. A page of more than 24 parameters gets a
    tab for each second level (`Operator 1/Envelope`); the rest of the path is a heading. So
    give a large page second levels of a dozen or so parameters each;
  - without module paths, a plugin of more than 24 parameters gets pages from its names'
    leading words (`Osc2 Detune` is `Detune` on page `Osc2`; `Arp Step 6 Transpose` is
    `Transpose` on page `Arp`, tab `Step 6`), pages in name order; give module paths to choose
    the pages and their order yourself. The host lists parameters in ID order, so number IDs
    in reading order where you can;
  - a mode or algorithm (a reverb's Hall, Room and Plate) is best a stepped parameter with
    its names as value texts: up to 4 values are buttons, more a list, and a hint can ask for
    buttons up to 8. Put it first by priority. When a mode changes which controls matter, hide
    the others (`CLAP_PARAM_IS_HIDDEN`) or change names or modules, and call
    `clap_host_params.rescan(CLAP_PARAM_RESCAN_INFO)` from the main thread: the host takes the
    new info in place, without deactivating the plugin, so the sound does not stop, and the
    editors redraw their pages within half a second. Adding or removing parameters still needs
    `CLAP_PARAM_RESCAN_ALL` and a restart. A plugin in a worker process gets the same: the
    worker forwards the new info with its next service;
  - value texts are shown as written, except a bare number with more than three decimals,
    which is shortened (`0.0100000` shows as `0.01`), so write units and names where they help;
  - a search box finds parameters on any page by name or module, so keep names specific;
  - a small screen with a few encoders (RFC 0019's Compact surface, tried with the Compact
    layout) lists the same pages, then a page's parameters four at a time in priority order,
    so a page's first four by priority (or in your order, without hints) are what a pedal-sized
    device reaches first;
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
  - a stepped parameter with 2 to 4 values is a row of buttons (Off and On, or 0 and 1: a switch), with
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
| Native effect contexts (`effect-contexts.h`, [RFC 0007](rfcs/0007-effect-contexts.md)) | Many independent effect slots sharing one engine's memory, with tails reported. The next Performance's effects become new contexts of an engine already playing while the old ones ring out, so an engine outlives its first context and must refuse `create` past its capacity. | Delay |
| Parameter-target lanes (`param-target.h`, [RFC 0002](rfcs/0002-param-target.md)) | For sequencers and modulators: change other plugins' parameters at an exact sample in the same block (parameter locks). | Step Sequencer |
| Declared memory (`memory.h`, `oscairn.memory/draft`, [RFC 0025](rfcs/0025-resource-budgets.md#declarations)) | `estimate(rate, max_frames)`, called after the state loads and before activating, says what activating would allocate: shared bytes (once per instance or engine), bytes per context (an instance without contexts is one) and decoded asset bytes for the current state; `current`, after activating, what it holds. A device near its limit can then refuse a plugin before its large allocation. Oscairn asks at every activation (a load, a state load, a new effect context, whose cost is its context and asset bytes) and, with a memory budget, refuses before activating a plugin declaring more than the budget leaves, saying how much it needs. It counts the larger of the declaration and its own measurement, and keeps its measurements per declared size, so a plugin's modes (a reverb's algorithms) are not counted at the largest. Without it, an effect engine's `oscairn.effect-contexts` resources are counted instead. | KICK memory test fixture |
| Sound assets (`assets.h`, [RFC 0009](rfcs/0009-sound-assets-and-dependencies.md)) | For samplers: the host resolves sample files, and Performances list their samples so they can be checked and moved between machines. | Sampler |
| Editor hints (`ui.h`, `oscairn.ui/draft-3`, [RFC 0019](rfcs/0019-adaptive-ui.md)) | A hint per parameter: the control it wants (knob, slider, buttons, switch, list, or a range with its high parameter), a priority, 1 first, which orders its tiles and the physical knobs (a device's screen knobs play an editor page's first parameters in this order; a part with no controls of its own and no roles gets Assign banks from its instrument's first parameters in this order; an effect without a `mix` role has its first parameter as its knob in the Effects bank, [RFC 0011](rfcs/0011-performance-ui-and-editors.md#step-6-status)), a group: parameters of one module in the same group are drawn joined, edge to edge, and kept together on narrow screens, and a **role** (`enum oscairn_role`): cutoff, resonance, filter envelope amount, key tracking and the filter envelope's stages; attack, decay, sustain, release, hold, velocity, amp key tracking and level; the EQ's gains, frequencies, mid Q and output; levels 1 to 8 (elements, operators, oscillators); `mix`, an effect's main control; and pan. Roles put a parameter on the same knob in every instrument's role banks (Filter, Amp EG, EQ, Levels); a role the plugin lacks is a blank knob. A plugin that declares no role gets roles from its parameters' names ("Cutoff", "Resonance", "Attack", "Wet"), which miss most spellings, so declare them. The host keeps a hint only where it fits (a switch on two values, a range whose high end exists, a role it knows). Every parameter stays reachable either way. Describing whole editors (a module grid, a sample map) comes later. Draft 3 added the role, so the struct grew: a plugin built against draft 2 must rebuild. | Basic Synth (cutoff and level), Delay (Wet is its `mix`) |

## Testing

Copy the reference plugins' tests in `core/engine/tests/` and `plugins/`: deterministic
offline rendering with `oscairn-render`, rendering that never allocates, state round trips,
and the same tests with the plugin in a worker process (the `*-workers` tests). Run them
under AddressSanitizer and ThreadSanitizer. Two checks run on a plugin as built, without
writing tests:

- `core/lv2-host/tests/production/run.py ... --case NAME,PATH,ID,synth|effect,PARAM,VALUE`:
  renders at 44.1 and 48 kHz, 37- and 512-frame blocks, offline and realtime, a fresh-instance
  repeat and a parameter edit that must change the sound
  ([how](../core/lv2-host/tests/production/README.md)).
- `oscairn-plugin-check PLUGIN ID synth|effect [--param ID=VALUE]`: the path a musician's sound
  takes. The plugin plays in a part, is saved to the library and opened again with every
  parameter as saved, is exported in an archive with its build, and is installed on a fresh
  device, where it opens from the plugin store and plays the same. With
  `--add-build linux-arm64=PATH --keep-archive FILE`, the archive also carries another
  platform's build; `oscairn-plugin-check --install FILE synth|effect` on that platform installs
  it and checks it opens from the store and plays. See
[Test before publishing a plugin](developer-guide.md#test-before-publishing-a-plugin).

## Licenses

Keep a reused engine's license and notices with it and credit its authors in
[CREDITS](../CREDITS.md). Chillbeast's Modular shows the pattern: imported unchanged with its
history, wrapped as a CLAP plugin beside it ([plugins/modular](../plugins/modular/README.md)).

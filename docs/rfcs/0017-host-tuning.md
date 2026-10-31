# RFC 0017: Host-level tuning

Status: proposed, 2026-10-31. Not implemented. The Performance owns tuning, and Oscairn
publishes it to plugins through CLAP's draft tuning extension and as the MTS-ESP master.
[RFC 0009](0009-sound-assets-and-dependencies.md#which-plugins-provide-or-need-files) already lists tuning
as "one tuning for every instrument in a Performance". This RFC decides where that tuning
lives and how it reaches each engine.

## Problem

Today Oscairn has no tuning. The host doesn't implement any tuning extension (nothing in
`core/clap-host/src` mentions it), the Sampler computes pitch from 12-TET only
([`src/sampler.cpp`](https://github.com/oscairn/sampler/blob/main/src/sampler.cpp)), and the Performance format has no tuning field. Two engines on
the way already have their own tuning: Dexed, which Octet forks ([RFC 0014](0014-fm-engine.md)),
and Surge XT, which the analog engine forks ([RFC 0015](0015-analog-modelling-engine.md)). Both
include an MTS-ESP client (`MTS_RegisterClient` in Dexed's `source/PluginProcessor.cpp` and
Surge's `src/common/SurgeStorage.cpp`).

The first plan dropped MTS-ESP. If a third-party master plugin sets the tuning, the tuning
isn't part of the saved Performance, so the Performance plays differently on another machine.
That problem goes away if Oscairn is the master and saves the tuning with the Performance.
The checks below test that design before this RFC commits to it.

## Outcome

- A Performance stores its tuning: one default tuning, plus one per Part where it differs.
  Each tuning is Scala text (`.scl` plus an optional `.kbm`). A Performance plays the same
  wherever it is loaded, or its load report says which plugins can't follow its tuning.
- Native engines (the Sampler, Octet and the analog engine) read the host's tuning through
  `clap.tuning`, per Part, at sample offsets.
- Third-party plugins follow it through `clap.tuning` where they support it, or as MTS-ESP
  clients where they don't. A plugin that supports neither plays 12-TET, and the load report
  says so.
- Built-in scales ship as Scala data written for Oscairn, with no Yamaha content or names.

## Findings

Checked on 2026-10-31 against [ODDSound/MTS-ESP at f214739](https://github.com/ODDSound/MTS-ESP/tree/f214739b8832e7f297cb9970d0c0efbf783f1462)
(2026-02-04) and [free-audio/clap at a47f6ba](https://github.com/free-audio/clap/tree/a47f6badb49d948fd009998f28309cdab78979c9).

### 1. Licences

- **SDK sources.** The repository [LICENSE](https://github.com/ODDSound/MTS-ESP/blob/main/LICENSE)
  and the header of every source file (`Client/libMTSClient.*`, `Master/libMTSMaster.*`) give
  the Zero-Clause BSD grant: "Permission to use, copy, modify, and/or distribute this software
  for any purpose with or without fee is hereby granted", with no notice requirement. This
  is compatible with AGPL and GPL code. Oscairn can compile the master sources into the core,
  and GPL plugins can compile the client sources.
- **libMTS.** This is the shared library that connects the master to its clients. It is
  shipped only as binaries under [`libMTS/`](https://github.com/ODDSound/MTS-ESP/tree/main/libMTS),
  and its source isn't published. No licence file covers the binaries separately. The
  [README](https://github.com/ODDSound/MTS-ESP/blob/main/README.md#libmts) says "Installers are provided
  which you can bundle into your own installer or, if you prefer, just include the library
  files and install to the above locations". ODDSound's [developer page](https://oddsound.com/devs.php)
  states no licence, fee or trademark terms. Whether the repository LICENSE covers the
  binaries is **unverified**; that is the most likely reading, but nothing says so outright.
- **Who installs it.** Both the client and the master load libMTS at run time from a fixed
  path with `dlopen` / `LoadLibraryW` (`libMTSClient.cpp` `load_lib()`, and the same in
  `libMTSMaster.cpp`). The paths are `/Library/Application Support/MTS-ESP/libMTS.dylib`,
  `/usr/local/lib/libMTS.so` and `Common Files\MTS-ESP\LIBMTS.dll`. Nothing is linked at build
  time. A master needs libMTS installed, by the user or by the master's installer.
- **AGPL.** Oscairn can *use* a libMTS the user installed, the same way GPL Surge and Dexed
  already do. *Shipping* the closed binary in the device image is a different question.
  AGPL §1 counts as Corresponding Source "shared libraries and dynamically linked
  subprograms that the work is specifically designed to require", and Oscairn can't supply
  libMTS's source. Whether an optional, `dlopen`ed library falls under that clause is
  **unverified**. It goes to counsel ([RFC 0013 IP rule 6](0013-awm2-class-sample-engine.md#ip-rules)
  covers the process). RFC 0001 lets code move to and from the KICK firmware, so a licence
  exception from Jacob alone may not cover every line.

### 2. Platforms

- The repository ships libMTS for Windows (32- and 64-bit DLLs and an installer), macOS
  (a universal x86_64/arm64 dylib and notarised installers) and Linux. For Linux it ships
  `libMTS/Linux/x86_64`, `arm32hf` and **`arm64`**. `file` reports the arm64 build as
  "ELF 64-bit LSB shared object, ARM aarch64", and it references only `GLIBC_2.17` symbol
  versions. The README's Linux install path is `/usr/local/lib`. It names no Linux
  installer, and its CHANGELOG (latest 1.03, 2025-09-04) doesn't say which platforms each
  version covers. Not loaded on the device: **unverified** there.
- **Without libMTS,** the client degrades quietly. `load_lib()` returns early, all function
  pointers stay null, `isOnline()` is false, and queries return the client's local table:
  12-TET, or whatever MIDI Tuning Standard SysEx it has received
  (`libMTSClient.cpp` lines 70 and 253–276). The master functions become no-ops the same way.
- **A compatible implementation** is possible from the sources alone. The interface is
  exactly the set of C symbols that `libMTSClient.cpp` and `libMTSMaster.cpp` resolve with
  `dlsym` (`MTS_RegisterClient`, `MTS_GetTuningTable`, `MTS_GetMultiChannelTuningTable`,
  `MTS_UseMultiChannelTuning`, `MTS_SetNoteTunings`, `MTS_CanRegisterMaster` and the rest).
  Their behaviour is set out in the two headers, and the 0BSD grant allows modifying those
  sources. One detail matters. The client fetches `const double *` pointers to the 128
  frequencies in Hz once, when the library loads, and reads them directly afterwards. A
  replacement must keep those tables at fixed addresses for the life of the process.
  Trademark use of "MTS-ESP" and any patents are **unverified** and go to counsel.

### 3. Across processes

The README's [IPC Support](https://github.com/ODDSound/MTS-ESP/blob/main/README.md#ipc-support)
section says MTS-ESP "supports inter-process communication, to allow for hosts which can run
plug-ins in separate processes". IPC is on unless `ipc_support = 0` in a system-wide
`MTS-ESP.conf` (`/usr/local/etc` on Linux). `libMTSMaster.h` says that with IPC "the shared
memory will persist" after a crash. The arm64 binary imports `shm_open`, `mmap`, `ftruncate`
and `shm_unlink` and contains the string `/libMTSShared`. That points to POSIX shared memory,
but the mechanism comes from inspecting the binary, not from documentation. So the tables are
shared between every process that loads libMTS with IPC enabled. A master in Oscairn's engine
reaches clients in RFC 0010 workers, provided the worker sandbox (RFC 0010 step 6) allows the
library and the shared-memory object. Not tested; macOS sandbox profiles in particular are
**unverified**.

### 4. Another master, and channels

- **One master per libMTS state.** `libMTSMaster.h`: "only one Master may be registered at any
  one time. Don't call MTS_RegisterMaster() if [MTS_CanRegisterMaster] returns false." With
  IPC on, that means one master per machine. With IPC off, it means one per process. The
  clients don't choose: each one reads whatever table libMTS holds.
- **Crashes.** A master that crashes leaves the flag set. `MTS_Reinitialize()` clears it, but
  the header says to offer it only when `MTS_CanRegisterMaster()` is false and `MTS_HasIPC()`
  is true, and only as a user choice "in case there was a crash".
- **Per-channel tables.** The master marks a channel with `MTS_SetMultiChannel(true, ch)` and
  fills that channel's table with `MTS_SetMultiChannelNoteTunings(freqs, ch)`. A client uses a
  channel's table only when it passes a channel 0–15 to `MTS_NoteToFrequency` /
  `MTS_RetuningInSemitones` and that channel is in use. Otherwise it reads the global table.
  The README says masters "should also provide a default single-channel tuning table as a
  fallback". The header asks clients not to pass a channel in MPE mode. Surge passes one only
  when `mtsUseChannelWhenRetuning` is set, and that flag defaults to `false`
  ([SurgeVoiceState.h](https://github.com/surge-synthesizer/surge/blob/main/src/common/dsp/SurgeVoiceState.h),
  [SurgeVoice.cpp](https://github.com/surge-synthesizer/surge/blob/main/src/common/dsp/SurgeVoice.cpp)).
  Where Surge sets the flag is **unverified**, and so is Dexed's channel use.
- The tables are keyed by **channel number across the whole machine**, not by plugin instance.
  Two Parts that reach two different plugins on the same channel number get the same
  MTS-ESP table.

### CLAP tuning

The pinned CLAP 1.2.10 (`core/clap-host/third_party/clap`) already contains
`include/clap/ext/draft/tuning.h`, and it matches upstream `main`. Its ID is
`clap.tuning/2`, and it is still a draft. The host provides `get_relative(tuning_id, channel,
key, sample_offset)` in semitones against 12-TET at A4 = 440 Hz, plus `should_play`, and both
are marked "[audio-thread & in-process]". It also provides a pool of named tunings
(`get_tuning_count`, `get_info`). A `clap_event_tuning` event selects a tuning per note port
and channel, in an event space the plugin queries from the event registry, which Oscairn's
host already offers. The plugin side has only `changed()`. A GitHub code search for
`CLAP_EXT_TUNING` found no use in Surge, Dexed or Vital, mostly copies of the header. Few
plugins use it (**unverified** beyond that search). Vital's public source has no MTS-ESP
client code either.

## Design

### Tuning in the Performance

The Performance gains a `tunings` list and a `tuning` reference:

| Field | Meaning |
| --- | --- |
| `tunings[]` | ID, name, `scl` text, optional `kbm` text, and the built-in ID and content hash it was copied from, if any |
| `tuning` (top level) | The default tuning: the MTS-ESP global table, and every Part without its own |
| `parts[].tuning` | Optional: a tuning ID for this Part |

The Scala text is stored inline, even for built-ins. Changing a built-in later then can't
change an old Performance, and the Performance needs no tuning library on the machine that
opens it. The reader rejects unknown fields (see the [performance format](../performance-format.md)),
so an older build refuses a Performance with tunings rather than playing it in 12-TET.
Incompatible changes during initial development replace the format, as that document says.

A sound's own stored tuning (a Dexed or Surge patch with a scale, or a converted Part's
micro tuning) becomes the Part's tuning. The converter and the sound picker copy it into
`tunings` and say so. That keeps a single source: what's saved is what plays.

### Built-in scales

Oscairn ships a small set of `.scl` files written for it: just-intonation major and minor,
historical well-temperaments and meantones, and quarter- and eighth-tone equal divisions.
Each file is computed from its historical definition, with that source cited in the file's
comment lines. A `.kbm` sets the root key, which plays the role of the "tuning root" setting
on workstations. No Yamaha table values, list order or names are used (RFC 0013 IP rules 2
and 5). Names describe the tuning, for example "Werckmeister III" or "24-tone equal". Yamaha
presets with no public historical definition become user tunings when a Performance is
converted. They are fitted from renders as RFC 0013 describes, saved in that Performance,
and never shipped as built-ins.

### One computation, two publications

The engine parses the Scala text once, off the audio thread, into one table per tuning:
128 frequencies in Hz and 128 "unmapped" flags (`.kbm` "x" entries). Both publications are
read from this table: MTS-ESP takes the frequencies, and `clap.tuning` returns
`12·log2(f/440) − (key − 69)`. The two can't disagree. The parser and table code is
Oscairn's own (AGPL) and has fixtures, including the `.kbm` reference-key and
period-degree cases.

### clap.tuning: native engines and plugins that support it

- **The pool** holds the Performance's tunings, in order, with stable IDs.
- **Selection:** when a Performance loads, and when a Part's tuning changes, the host sends
  `clap_event_tuning` to each node in the Part's chain. The event carries the note port and
  each channel the Part's note wires deliver on, and it lands at the block boundary, before
  that block's notes. Native engines with sound contexts select a context per channel
  ([RFC 0005](0005-sound-contexts.md)), so the Parts sharing one engine already arrive on
  different channels and get different tunings.
- **Sampler:** each voice's tuned key is `key + get_relative(...)`. Its pitch in cents is
  `(tunedKey − pitch_keycenter) · pitch_keytrack + transpose · 100 + tune`, so `pitch_keytrack=0`
  regions such as drums stay untuned. The voice re-reads the tuning each block if the tuning
  is marked dynamic. An unmapped key starts no voice and reports `NOTE_END`, as an
  out-of-range key does now.
- **Octet and the analog engine:** both forks follow this order: host `clap.tuning` if
  offered, then their own stored tuning, then MTS-ESP, then 12-TET. The middle two follow
  the SDK's advice that local tuning should override MTS-ESP (`libMTSClient.h` step 7). In
  another host they behave as today, and inside Oscairn host tuning always wins.
- **Third-party plugins** that export `clap.tuning` get the same treatment.

### MTS-ESP master

- The core compiles `libMTSMaster.cpp` (0BSD) and uses whatever libMTS is installed. If none
  is installed, publishing to MTS-ESP is off and the load report says so.
- Oscairn registers only while the open Performance has a tuning other than 12-TET, and only
  if a preference allows it (default on). It deregisters when the Performance stops needing
  it and when the app quits, so an idle Oscairn doesn't block a DAW's master.
- **Global table:** the Performance's default tuning.
- **Per-Part tables:** for each Part whose tuning differs from the default, the host marks
  the channel numbers its notes reach plugins on and fills those channel tables. If two Parts
  with different tunings use the same channel number, there is a conflict. The default wins
  on that channel, and the load report names both Parts so the musician can move one. This
  reaches only clients that pass a channel, which stock Surge doesn't by default.
- **Which plugins are clients:** libMTS reports only a total (`MTS_GetNumClients`). The host
  records the count before and after it creates each plugin instance and marks a node as a
  client when the count rises. This is a heuristic to prove in step 3. It can't tell
  anything apart when two instances are created at once, so the host creates them one at a
  time.

### Plugins that support neither

They play 12-TET, and the load report lists them under "doesn't follow this Performance's
tuning". The Performance then plays the same everywhere, because 12-TET is all such a plugin
ever does. As an opt-in per node, the host can send each note a `CLAP_NOTE_EXPRESSION_TUNING`
(stable CLAP, ±120 semitones, `events.h`) at the note-on's sample offset. A plugin that
handles note expressions then plays in tune. This isn't automatic: CLAP has no way to ask
whether a plugin handles that expression, and a plugin that is secretly an MTS-ESP client
would be retuned twice.

### Worker processes

- **clap.tuning** must be answered in-process, so the worker shim implements
  `clap_host_tuning` itself. The engine sends the pool's tables over the control socket when a
  Performance loads or a tuning changes. The worker's audio thread swaps them in at a block
  boundary, double-buffered, with no allocation on the audio path. Tuning events travel in the
  shared event list like any other event ([RFC 0010](0010-plugin-worker-isolation.md#control-and-host-extensions)).
- **MTS-ESP:** the master stays in the engine process. Clients in workers read libMTS's shared
  tables (finding 3). If a worker crashes, nothing happens to the master. A recovered worker's
  client reconnects when its plugin library loads. The worker sandbox grants read access to
  the libMTS path and access to its shared-memory object.

### Another master already running

On the desktop:

1. If `MTS_CanRegisterMaster()` is true, register.
2. Otherwise, don't override. Show "Another MTS-ESP master controls MTS-ESP plugins". Oscairn's
   own engines and `clap.tuning` plugins still follow the Performance. The load report marks
   MTS-ESP client nodes as following an external tuning.
3. **Stale flag:** if Oscairn's own runtime record shows that its previous engine process held
   the master and that process is gone, the flag is Oscairn's own. No other master could have
   registered while it was set, so the host calls `MTS_Reinitialize()` and registers. In any
   other case it offers reinitialising as a user choice, only when `MTS_HasIPC()` is true, as
   the header prescribes.

Oscairn never edits `MTS-ESP.conf`. It is system-wide and belongs to the user.

**The device** has no other host, so the engine is the only master. At engine start it
reinitialises and registers. It needs a libMTS at `/usr/local/lib/libMTS.so`, the path the
client sources hard-code. Two choices:

- **(a) Bundle ODDSound's arm64 binary.** The repository has one, and the README invites
  bundling it. That needs counsel's answer on AGPL §1 (finding 1), and ideally ODDSound's
  agreement.
- **(b) An Oscairn implementation of the same C interface,** written from the 0BSD headers
  and client and master sources only, MIT-licensed like the CLAP extension headers so any
  plugin may load it, with its tables in its own POSIX shared-memory object.

**Recommendation: (b), unless ODDSound or counsel clears (a).** Every byte in the device
image then has source, the tables are testable, and its shared memory is designed for RFC
0010's sandbox. It is a few hundred lines. It isn't put on the desktop, where it would
compete with ODDSound's library at the same path.

### Determinism

- The Scala text is saved inline, and the tables are computed by one piece of code. Tests
  compare tables bit for bit between macOS and Linux arm64.
- `clap.tuning` selection is sample-accurate and identical in and out of workers. That is
  covered by the existing `*-workers` identical-render tests, extended with tuning.
- MTS-ESP is not sample-accurate. A client reads the table when it chooses: at note-on or
  continuously (`libMTSClient.h` steps 3 and 4). Tables are set before the first block after
  a load and change only on edits, so a Performance renders the same unless the musician
  changes tuning while notes play.
- Offline rendering (`oscairn-render`) registers as master like the engine. If another master
  holds the flag (the desktop app while a render runs, for example), MTS-ESP client nodes are
  reported as untuned for that render instead of silently using another tuning.
- **What's guaranteed:** native engines, `clap.tuning` plugins and plugins with neither
  extension always play the Performance's tuning, or 12-TET. MTS-ESP clients do so when
  Oscairn holds the master, the client isn't overriding with its own tuning, and, for
  per-Part tables, it passes channels. The load report states which case applies to each node.

## Build order

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | Performance `tunings` and `parts[].tuning`; Scala and `.kbm` parser; tables; built-in `.scl` set with sources | Parser fixtures (including unmapped keys and reference keys); identical tables on macOS and Linux arm64; save, reopen and convert round trips |
| 2 | Host `clap.tuning` (pool, events, `get_relative`, `should_play`) in the engine and the worker shim; the Sampler reads it | Sampler pitch within 0.1 cent of the table for every key; unmapped keys start no voice; identical renders in the engine and in a worker; no allocation on the audio path |
| 3 | MTS-ESP master on the desktop with the user's libMTS: register on demand, global and channel tables, conflict and stale-flag rules, client detection, load report | Stock Surge XT and Dexed, in the engine and in workers, play the Performance's tuning; a second master is refused with the warning; a killed engine is recovered without asking; client detection is right for each reference plugin |
| 4 | Octet and the analog engine read `clap.tuning` with the stated order (on the forks' `oscairn` branches) | The same pitch tests as the Sampler; in a DAW with an MTS-ESP master they still follow it |
| 5 | Device libMTS, (a) or (b) after counsel | Stock Surge XT in a worker on the device follows the Performance; sandboxed workers can read the tables |
| 6 | Opt-in note-expression tuning per node | A note-expression fixture plugin plays in tune; the default is off |

## Open questions

- Counsel: may an AGPL device image ship ODDSound's libMTS binary (AGPL §1)? May Oscairn
  ship a library of the same interface under that file name? Does "MTS-ESP" in the UI need
  ODDSound's permission?
- Ask ODDSound whether they prefer (a), (b), or publishing libMTS's source.
- Whether `clap.tuning` changes before it leaves draft. Octet and the analog engine are
  first-party and can follow it, but third-party adoption is unknown.
- Whether the MIDI Tuning Standard is worth sending as SysEx to plugins that accept it. The
  host has no SysEx path today, and the MTS-ESP client uses SysEx only when no master is
  connected.
- Whether a dynamic tuning (scale changes while notes are held) is ever needed, given that
  MTS-ESP can't be sample-accurate.

## Ruled out

- **Third-party masters as the source of truth:** the tuning wouldn't be in the Performance,
  which is the original objection.
- **Per-plugin tuning files as the source of truth:** they're invisible to the host and differ
  for each plugin.
- **Editing `MTS-ESP.conf` to turn IPC off:** it's system-wide, and with IPC off, clients in
  workers would lose the master.
- **Overriding another live master with `MTS_Reinitialize()`:** the header limits it to
  recovering from crashes.

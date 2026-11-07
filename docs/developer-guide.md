# Developing for Oscairn

Oscairn hosts ordinary CLAP plugins and, when enabled at build time, LV2 plugins. You do
not need a separate Oscairn plugin format or the desktop UI to write DSP. Platform-native
instruments additionally implement concurrent sound contexts. Start with the path below
that matches your component; the [documentation index](README.md) links the full contracts.
Bringing an existing CLAP plugin over? See [Porting a plugin](plugin-porting.md) for what it
must meet, today's limits and the optional extensions.

## Initial-development compatibility policy

Oscairn is in initial development. Our own APIs, extension drafts, data structures and
saved formats have no backward-compatibility guarantee. Replace obsolete designs outright;
update callers, tests, fixtures, schemas and documentation together. Do not retain old
readers, aliases, adapters or parallel implementations solely to support earlier Oscairn
builds. Recreate development fixtures when a format changes instead of adding migrations.

Keep version/identity checks and input validation: unsupported data must fail clearly,
without corrupting the current session or overwriting the source files. Version tags
identify contracts; they do not promise support for earlier versions.

This policy does not remove support for external standards such as CLAP, LV2 and MIDI,
or the planned cross-device portability contract. **Backward compatibility becomes a
product requirement with the first public product release.** Before that release, define
the supported API/format versions, migration and deprecation rules, and regression fixtures
for released user data. Preserve released work through supported upgrades; do not apply
today's recreate-the-fixture policy to customers' saved sounds or Performances. This does
not require preserving experimental pre-release formats now.

## Choose the boundary

| Component | Interface today | Reference |
| --- | --- | --- |
| Instrument, sampler or effect | CLAP; optional LV2 hosting | [First-party plugins](../plugins/README.md), [LV2 subset](../core/lv2-host/README.md) |
| Sequencer or note processor | CLAP note output ports and transport; optional parameter-target events | [Step Sequencer](../plugins/step-sequencer/step_sequencer.cpp), [MIDI effects](../plugins/midi-effects/README.md), [RFC 0002](rfcs/0002-param-target.md) |
| Native multitimbral instrument | CLAP plus `oscairn.sound-contexts/draft-5` | [Basic Synth](../plugins/basic-synth/basic_synth.cpp), [Legacy Sampler](https://github.com/oscairn/sampler), [shared contexts](../core/clap-ext/include/oscairn-clap/sound-contexts-plugin.hpp), [RFC 0005](rfcs/0005-sound-contexts.md) |
| UI, editor or automation script | Typed `Control` or JSON-RPC over the local Unix socket | [Protocol and schema](control-protocol.md) |
| New host/application | C++20 `Session` or `Graph`, supplied audio buffers and events | [Core](../core/README.md), [hosts](../hosts/README.md) |
| Hardware panel/layout | Desktop JSON layouts and device profiles today; timestamped hardware services are proposed | [Layouts](../hosts/standalone/layouts/README.md), [profiles](rfcs/0024-device-profiles.md), [RFC 0004](rfcs/0004-hardware-services.md) |

VST3 hosting, plugin GUI embedding, a distributable Oscairn SDK/package registry,
automatic DSP/auxiliary-core offloading, and a native DJ deck engine
are not implemented. A plugin binary must match the host OS, CPU architecture and ABI.
A macOS plugin cannot be copied unchanged onto an ARM Linux image.

## Build a working reference

Run from the repository root, with CMake 3.21+, Ninja and a C++20 compiler:

```sh
cmake -S . -B build-dev -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-dev
ctest --test-dir build-dev --output-on-failure
build-dev/core/clap-host/kick-clap-scan build-dev/plugins/oscairn-basic-synth.clap
build-dev/hosts/oscairn-render reference.wav --seconds 4 \
    build-dev/plugins/oscairn-step-sequencer.clap \
    build-dev/plugins/oscairn-basic-synth.clap \
    build-dev/plugins/oscairn-delay.clap
```

The first configure fetches the [Legacy Sampler](https://github.com/oscairn/sampler), which lives in its own repository, at the
commit pinned in `plugins/CMakeLists.txt`, so it needs network access. Only the tests use it; it is not bundled or installed. `-DBUILD_TESTING=OFF` skips it
and the tests that need it. To build a local checkout of it instead, add
`-DFETCHCONTENT_SOURCE_DIR_OSCAIRN_SAMPLER=path/to/sampler`.
The renderer writes an offline WAV; it does not open an audio device. Socket tests need
permission to bind local sockets. For live output build the [desktop/helper](../hosts/README.md).
For LV2 add `-DOSCAIRN_LV2=ON` and install Lilv/LV2 development packages as described in
the [LV2 guide](../core/lv2-host/README.md). The default build does not require JUCE.
`-DOSCAIRN_MODULAR=ON` (implied by `OSCAIRN_STANDALONE`) also builds the
[Modular instrument](../plugins/modular/README.md), which downloads and links JUCE.

### Run the tests

`ctest --test-dir build-dev` runs every test the configuration built; `-R` picks some by name
(`ctest --test-dir build-dev -R '^control-'`). The names group by area:

| Names | What they cover | Built with |
| --- | --- | --- |
| `kick-clap-*` | The CLAP host: loading, state, sessions, the scanner, benchmarks | default |
| `graph`, `graph-lifecycle`, `note-routing`, `audio-routing`, `param-target`, `sequencer-*`, `midi-*`, `transport-tempo`, `sound-contexts`, `effect-contexts`, `prepared-instances`, `choke-fade`, `wheels`, `pressure`, `sampler`, ... | The engine and the reference plugins | default |
| `worker-host`, `worker-failures`, `worker-recovery`, `handoff-bench*` | Plugin worker processes | default (`handoff-bench` on Unix) |
| `*-workers` | The same engine and control tests with every plugin in a worker (`OSCAIRN_PLUGIN_WORKER` set) | default |
| `control-*` | The session, control API, library, Performances, archives, signing (`control-signing`, `control-archives`), budgets (`control-plugin-failures`), physical controls and control surfaces | default |
| `render-*` | `oscairn-render` output: repeatable and independent of block size | default |
| `engine-process-*` | Starting and reconnecting to an engine process | default, on Unix |
| `desktop-*`, `midi-monitor` | The desktop app's models and screens, the MIDI monitor | `OSCAIRN_STANDALONE` (`midi-monitor` also with `OSCAIRN_ENGINE`) |
| `lv2-*`, `*-lv2`, `plugin-state` | LV2 hosting | `OSCAIRN_LV2` |
| `modular` | The Modular instrument | `OSCAIRN_MODULAR` |

`desktop-shell` also drives a third-party plugin when `OSCAIRN_THIRD_PARTY_CLAP` (a CMake cache
path) names one that exists.

### Tools

Programs in `tools/`, and the ones built beside the tests, for plugin work:

| Program | Built as | Use |
| --- | --- | --- |
| `kick-clap-scan` | `build-dev/core/clap-host/kick-clap-scan` | List a CLAP module's plugins and why any fails |
| `oscairn-render` | `build-dev/hosts/oscairn-render` | Render a chain of plugins offline to a WAV |
| `oscairn-plugin-worker` | `build-dev/core/oscairn-plugin-worker` | The worker process that runs a plugin outside the engine |
| `oscairn-plugin-check` | `build-dev/core/control/oscairn-plugin-check` | Check a plugin through the library, an archive and the plugin store ([below](#test-before-publishing-a-plugin)) |
| `oscairn-sign` (`tools/oscairn-sign`) | `build-dev/core/control/oscairn-sign` | Make a signing key, sign a plugin build or an archive, verify an archive, sign and verify a package repository's files ([below](#signing-keys-and-plugin-builds)) |
| `oscairn-midi-monitor` (`tools/midi-monitor`) | with `OSCAIRN_STANDALONE` or `OSCAIRN_ENGINE` | Print what a MIDI controller sends, for writing its profile |
| `oscairn-handoff-bench` (`tools/worker-bench`) | `build-dev/core/oscairn-handoff-bench`, on Unix | Measure handing audio blocks to worker processes and back |

`tools/sampler-research` holds a research script, not a build tool.

### Environment variables

| Variable | Effect |
| --- | --- |
| `OSCAIRN_PLUGIN_WORKER` | The worker program that runs plugins ([RFC 0010](rfcs/0010-plugin-worker-isolation.md)). Unset, the desktop's engine helper uses the worker beside it or where it was built, and other hosts run plugins in the engine. Set but empty, plugins run in the engine. LV2 plugins always run in the engine |
| `OSCAIRN_PLUGIN_SCAN` | The scan helper a loaded plugin's preset listing reads through ([RFC 0005](rfcs/0005-sound-contexts.md#presets)). Unset, the engine uses the one beside it or where it was built, and the desktop's in-process mode the one beside it; with none, a plugin in a worker lists no presets (saying why), and one in the engine's own process indexes there |
| `OSCAIRN_WORKER_MEMORY_MB` | A worker's memory limit when its plugins do not declare their memory; default 4096, 0 for none |
| `OSCAIRN_CGROUP` | Linux: a delegated cgroup v2 subtree. The engine moves into `engine/`, each worker gets a cgroup under `workers/` with its memory limit as `memory.max`, and memory pressure is read from the subtree. Without it, workers have no cgroups and pressure comes from `/proc/pressure/memory` |
| `OSCAIRN_RELIABILITY` | `instrument` or `pass-through`: on Linux the engine and workers lock their memory and take their places in the out-of-memory order, and say on stderr what they could not do ([RFC 0025](rfcs/0025-resource-budgets.md), [RFC 0027](rfcs/0027-reliability-levels.md)) |
| `OSCAIRN_TIME` | `known` or `unknown`: whether the wall clock is right ([RFC 0035](rfcs/0035-system-base.md#time)). Unset, it is known unless it reads before 2026-11-01. With it unknown, recordings are named `Take 0001 …` instead of by the time |
| `OSCAIRN_DATA_HOME` | The data folder: the library, `assets/`, the plugin store `Plugins/`, signing keys `Keys/`, `trust.json` and `measurements.json`. Default `~/Library/Application Support/Oscairn` on macOS, `$XDG_DATA_HOME/oscairn` or `~/.local/share/oscairn` on Linux. Point it at an empty folder to test as a fresh device |
| `OSCAIRN_CONFIG_HOME` | The desktop's configuration folder (themes and the selected theme; see [design language](design-language.md)) |
| `OSCAIRN_RUNTIME_DIR` | Where the desktop and its engine helper put their socket |
| `OSCAIRN_WORKER_BUDGET_US`, `OSCAIRN_WORKER_REPLY_SECONDS` | Tests: a worker's time per block in microseconds, and how long a control call waits for a worker |
| `OSCAIRN_REQUIRE_SOCKET_BIND` | Tests: fail, rather than skip, when local sockets cannot be bound |

## Make a first CLAP plugin

The [reference plugins](../plugins/README.md) are working code rather than pseudocode.
Basic Synth shows an instrument on the SDK's `sound-contexts-plugin.hpp` with `oscairn.ui`
roles, pitch bend and the mod wheel; Delay shows native effect contexts, state and a `mix`
role ([what each demonstrates](plugin-porting.md#examples)).
For an in-tree native instrument, copy `plugins/basic-synth/basic_synth.cpp` into a new
plugin directory. Change its descriptor ID, name and vendor before loading it alongside
the original. Keep the lifecycle, event and sound-context handling while changing DSP.
Do not copy source without retaining its license notices; see the repository [license](../LICENSE).

Register the source in `plugins/CMakeLists.txt` using the existing helper:

```cmake
oscairn_add_plugin(my_synth my-synth my-synth/my_synth.cpp)
```

Then rebuild and replace the synth path in the renderer command with
`build-dev/plugins/my-synth.clap`. The plugin must export `clap_entry`, supply its factory
and descriptor, and return supported extensions from `get_extension`. The helper gives
it the CLAP and Oscairn extension headers and exports a `.clap` module. An out-of-tree
plugin can use its own build system and the same CLAP C ABI; it does not link the Oscairn
engine into its DSP binary. Vendored SDK declarations are in
[`core/clap-host/third_party/clap/include`](../core/clap-host/third_party/clap/include).
The pinned SDK revision is recorded alongside them in `upstream.json`.

Stable descriptor, parameter and port IDs are identities, not display names or array
indices. Give parameters truthful ranges, defaults, units/text and flags. Host-facing
parameter values use plain units, not an assumed normalized 0..1 range. Declare stepped
and read-only parameters accurately. Do not change an ID's meaning between presets.

## Audio, events and real-time behavior

- Allocate voices, buffers and queues during initialization, activation or explicit
  control-thread preparation. Do not allocate/free, wait on a mutex, access files,
  perform network I/O or log synchronously inside `process`. CLAP host services run
  through the host's main-thread service loop; LV2 background work uses Worker.
- Honor the actual frame count, sample rate, audio-port inventory and transport validity
  flags. Calls can be shorter than the negotiated maximum. Process events at their
  sample offsets; do not apply every event at block start or assume 128/512 frames.
- Support the note dialects you advertise. CLAP note IDs, ports, channels and keys have
  separate meanings; match wildcard note-offs/expressions correctly. Emit `NOTE_END`
  when a CLAP voice really finishes or is stolen, including after its release envelope.
  Repeated presses of the same key are separate notes when IDs identify them.
- Handle sustain and emergency release/reset. Poly pressure addresses a key/voice;
  channel pressure can affect all matching voices. Oscairn converts incoming MIDI notes
  and pressure at its boundaries; a plugin must still implement the expression it accepts.
- Audio ports may be mono, stereo or multichannel, with independent sidechains and outputs.
  Declare main ports where supported. The graph sums explicit routes without automatic
  normalization or limiting. A cue output must not silently become a master output.
- Keep the audio layout stable while loaded in a graph. The CLAP adapter rejects
  structural port rescans that invalidate compiled routes. A state/preset prepared for
  an instance pool must also preserve port layout, note dialects and parameter metadata.
- Keep output events bounded and valid. The shared CLAP host permits 512 note events from
  the graph per call (a few more with the block's beat and harmony events) and a separate
  bounded control queue; overflow can invoke panic.
  The graph does not promise arbitrary event counts or unlimited polyphony.

The graph gives each note destination its own IDs, tracks ownership across source routes,
and forwards note-off/expression to the owner. MIDI 1.0 plugins cannot distinguish every
same-key voice or choke just one sustained voice: channel-wide all-sound-off may be
necessary. Test those cases rather than assuming CLAP note identity survives MIDI translation.

Transport currently represents one session timeline. Sequencers follow its beat position
and flags instead of counting fixed-sized callbacks. Independent DJ-deck playheads,
scratching and time stretching need deck-specific engines; they are not supplied by that
shared timeline. The graph and multichannel master/cue routing can be reused around them.

## Sounds, state and presets

There are two switching mechanisms:

| Capability from `sounds.get` | Responsibility |
| --- | --- |
| `mode: "native"` | Plugin implements concurrent contexts and knows its own voice/tail lifetime. Basic Synth is the reference. |
| `mode: "instances"` | Host prepares separate instances of an ordinary CLAP/LV2 instrument and preserves note ownership. |
| `mode: "unsupported"` | No sound-switching interface for this node. Its ordinary processing still works. |

All new native Oscairn instruments must support multitimbral contexts: selecting a new
sound changes new notes while old notes retain their patch, expression, sustain and
release. Share immutable samples/tables when appropriate; keep patch-dependent voices
and DSP history independent. Follow the exact threading, pinning, IDs and retirement
contract in [RFC 0005](rfcs/0005-sound-contexts.md) and its
[C header](../core/clap-ext/include/oscairn-clap/sound-contexts.h). The extension is a draft,
not a standard CLAP extension. Plugins must still behave normally in hosts that do not use it.
Native Delay now implements independent effect contexts; see [the contract below](#insert-chains-and-shared-effects).

Ordinary instruments need no custom extension for the host pool. Today it supports
instruments with note input, audio output, no audio input and no note/MIDI output. It has
eight slots including protected Default slot 0. The pool preserves separate channel
selections and can prepare defaults, a saved state directory, or a preset (an LV2 preset
URI, or a CLAP preset the plugin lists through CLAP preset discovery and loads with
preset-load), then apply numeric parameter overrides. It never restores state over the outgoing sound.
Effects and event-producing instruments need other transition policies and are not pooled.

Every processed pooled instance keeps rendering until explicitly discarded. There is no
silence detector or automatic tail timeout: a quiet passage does not prove a tail is over.
`cutTail: true` permits discarding an unselected instance only after its owned notes are
gone; it can cut remaining DSP tails. `activeVoices` for a pooled MIDI plugin counts owned
keys/sustained notes, not its private DSP voices. Plan CPU/RAM for all retained instances.
Use [the protocol](control-protocol.md#prepared-sounds) for exact requests and error behavior.

For persistent plugin data, implement the format's state extension. Parameter lists alone
cannot reconstruct sample assets, modulation or arbitrary internal state. The C++
`PluginHost` state API saves a **new** directory and reports failure rather than overwriting
an existing bundle. Raw adapter restore can reset/silence that instance; graph-pooled
instruments instead prepare and select a spare. State export, preset enumeration and typed LV2 patch-property methods also use the
[control API](control-protocol.md#state-export-presets-and-patch-properties). Query capabilities
and distinguish queued property acceptance from plugin-confirmed readback.
Complete projects are still future work. Sounds, Performances and the library are in
[RFC 0021](rfcs/0021-sounds-performances-and-library.md#progress).

## Packaging and portability

Built today ([RFC 0006](rfcs/0006-packages-and-portability.md)): an exported Performance or
Sound is a [library archive](library-archive.md) (format 3) that carries every build of each
CLAP plugin it uses that the exporting machine has, each file hashed. Installing one puts the
builds in the data folder's plugin store, and a Performance's `lock` records each build's
digest ([format](performance-format.md)). A plugin that is missing, in another version, or
fails to load opens as a stand-in that keeps its state. Builds, items and whole archives can be
signed; an install refuses plugin builds that are unsigned or signed by a key the device does
not trust, until the user agrees. LV2 plugins are not packaged: they are installed on each
device. See [Porting a plugin](plugin-porting.md#packages-and-signing) for a plugin author's
side.

### Signing keys and plugin builds

```sh
build-dev/core/control/oscairn-sign key author.key
build-dev/core/control/oscairn-sign build author.key clap com.example.synth 1.2.0 macos-arm64 MySynth.clap
build-dev/core/control/oscairn-sign archive author.key Export.olib
build-dev/core/control/oscairn-sign verify Export.olib
```

`key` makes an Ed25519 key readable only by you and prints its ID (the first 16 hex digits
of the SHA-256 of the public key) and public key. `build` adds this key's signature to
`MySynth.clap.sig` beside the build, over its format, ID, version, platform and digest;
re-sign after every rebuild. `plugin-package` packs builds and their `.sig` files into a
[plugin package](library-archive.md#plugin-packages), an archive with no Sounds or
Performances, signed whole with `--key`; a bundle holding several plugins is signed for all of
them at once (`build` takes the IDs comma-separated) and packed once, each plugin named with
`--plugin <id> <name>`. It refuses a build loading libraries from outside the system and its
bundle ([rules](library-archive.md#plugin-packages)) unless `--allow-outside-libraries`, which
it reports. `--license`, `--license-file` and `--source <url> <commit-or-tag>` give its license,
license texts and source, which a GPL, LGPL or AGPL plugin package must have
([license and source](library-archive.md#license-and-source)). `archive` signs an archive whole; `verify` checks every signature
and hash in one and says who signed what (`--into <folder>` picks where it unpacks to check,
on the file system with room for it). The app's own keys, for signing exports, are
`Keys/<name>.key` in the data folder (`keys.make`, `keys.list`); the keys a device trusts, and
for what (`plugins`, `apps`, `content`), are its `trust.json` (`trust.list`, `trust.set`,
`trust.remove`; [protocol](control-protocol.md)).

### Publishing a package

Oscairn's package repository takes submissions as pull requests to a GitHub repository, with no
server ([RFC 0031](rfcs/0031-package-repository-and-distribution.md#hosting-and-cost)). Sign each
plugin build and the archive as above, then follow the submission steps in the repository's
README; its template is kept here, in
[tools/package-repository/template](../tools/package-repository/template/README.md). Its CI
checks archives with `oscairn-sign verify`; `oscairn-sign file` and `verify-file` sign and check
the repository's root and index key. A device installs from the built repository as a folder
(`packages.list`, `packages.install`; [protocol](control-protocol.md)) once its root key is
trusted for `repositories`: copy each blob from the builder's cache to `blobs/<ab>/<sha256>`
beside `repository.json`.

### Planned

A managed package installer and repository are planned
([RFC 0031](rfcs/0031-package-repository-and-distribution.md)). The plan uses a project manifest and exact
lockfile with a project-local dependency view backed by a shared durable content store.
A download cache is separate; clearing it must not remove installed offline dependencies.
Design new engines with stable logical/format
identities, versioned architecture-independent state and complete asset enumeration.
Declare shared code/data as versioned dependencies rather than relying on a developer's
installed system libraries. Build separately for each supported OS/CPU/runtime profile;
Buildroot hardware needs its target SDK, not an arbitrary Linux binary. Native package
releases should publish artifacts and hashes per supported profile so Linux-default exports
can include hardware builds while other hosts acquire their exact destination builds later.
Imports install only relevant target artifacts; absent builds remain explicit failures,
not permission to upgrade or substitute another format. Advanced exports can include other
platforms for offline use. Keep mutable DSP state independent even when immutable assets
or library code can share storage/mappings.

A package may supply several formats for one engine, but AU, CLAP, LV2 and VST3 states and
parameter/port IDs are not interchangeable by name. A migration must be declared and tested,
including control mappings, assets, behavior and edits after transfer. Native managed
engines should share a canonical state representation across their supported builds.
Current per-plugin state export is not proof of complete project or cross-platform
portability; preserve opaque third-party state and mark unsupported transfers explicitly.

For accelerated engines, declare CPU/FPGA/DSP/sound-chip implementations as alternative
execution plans with shared state/assets and conditional dependencies. Install only the
selected plan's artifacts, including its CPU control adapter where needed. Match actual
board/service/firmware capabilities; CPU fallback is allowed only when its sound and resource
requirements fit. A hardware implementation that changes the sound needs an explicit saved
mode or separate engine. See [backend packaging](rfcs/0006-packages-and-portability.md#backend-variants-and-device-specific-dependencies)
and [hardware contracts](rfcs/0004-hardware-services.md#backend-packages-and-sound-modes).
No automatic conversion of arbitrary CPU code to hardware is implied.

Device edits must export complete current state and assets, retaining backend provenance;
declare whether conversion is reversible and invalidate derived alternate states after edits.
Separate immutable package payloads from user presets/recordings and mutable app data so
uninstall preserves musical work. Declare every runtime/asset dependency so Installed content
can explain usage and safely remove orphaned packages. See the planned
[device round trip](rfcs/0006-packages-and-portability.md#editing-on-one-instrument-and-transferring-to-another)
and [content management](rfcs/0006-packages-and-portability.md#installed-content-and-removal).

## Native versus external; other DAWs (planned)

"Oscairn-native" means meeting the package/state/asset portability contract for advertised
targets, not being first-party or merely exporting CLAP. Third-party authors can implement
it. External plugins require a compatible destination installation or tested conversion;
unavailable nodes retain state/routes and are disabled with a visible reason. Sound-context
`mode: "native"` is a narrower implemented capability, not the future portability badge.

Native engines must also function outside the Oscairn app. The desktop delivery target is
ordinary CLAP/VST3 for supported Windows/macOS/Linux profiles and AU for macOS, using the
same engine and canonical state. Use standard notes/audio/parameters/state and explicit
capability checks when Oscairn host extensions are absent. Provide package-backed assets
and a usable editor without requiring Oscairn's control socket. A wrapper does not make
unsupported MIDI routing or per-note features work in every host.

Individual plugins, the whole Oscairn graph as a DAW plugin, and standalone use are separate
delivery paths. Prefer evaluating existing format wrappers to duplicating DSP. Keep exact
runtime/state compatibility for old DAW projects: a standard host's format ID does not
identify an Oscairn package version. Publish native formats through the package manager,
but do not claim that another DAW's entire project becomes a complete offline bundle.
See [the delivery contract](rfcs/0006-packages-and-portability.md#native-plugins-in-daws-and-standalone-applications).
The current references are CLAP-only; these additional delivery paths remain planned.

## Controllers, hosts and hardware

A plugin's render callback should not call the JSON control API. Audio and performance
events use the graph; UI commands use `Control`. Query `session.hello` capabilities, then
metadata and IDs; stage graph edits and publish them with `graph.commit`. Handle rejected
requests and queue pressure. `accepted` means queued, not rendered or audible. The
[protocol](control-protocol.md) specifies socket framing, subscriptions, numeric units,
errors, resource limits and routing. There is no WebSocket or gRPC endpoint today.

A custom C++ host owns its device and threads. Construct/control/service `Session` on
one control thread, call `process` on one audio thread, and stop processing before
teardown. The core does not open ALSA/JACK/CoreAudio itself. Current live hosts use JUCE's
device layer; Buildroot uses the Linux host. See [host configuration](../hosts/README.md)
for rate, buffer and channel mapping and [OS instructions](../os/README.md) for the image.

Keep physical-control state (absolute position, relative encoder deltas, pickup/soft
takeover, bank ownership) in the hardware/control-mapping layer, not in every synth.
Plugins expose musical parameters; designs needing raw positions must request that
separate control source. This distinction is documented in the [architecture](architecture.md)
and hardware RFC. External MIDI controllers are mapped through controller profiles, banks and
Mackie Control ([RFC 0024](rfcs/0024-device-profiles.md#controllers),
[RFC 0011](rfcs/0011-performance-ui-and-editors.md#step-6-status)); a plugin's `oscairn.ui`
roles and priorities decide which parameters those knobs reach. The hardware-services and
acceleration APIs are not implemented yet.
No plugin currently gets automatic work on a DSP or auxiliary microcontroller core.

## Test before publishing a plugin

Start with the scanner and renderer above. Add a fixture or reference-audio test appropriate
to the component, using the existing CMake test conventions. Useful executable examples:

- [`sound_contexts.cpp`](../core/engine/tests/sound_contexts.cpp): native held-note switching,
  exact-sample expression, channel parts, sustain and concurrency.
- [`prepared_instances.cpp`](../core/engine/tests/prepared_instances.cpp): ordinary CLAP/LV2
  switching, state/presets, FIFO ownership, release tails and allocation-free rendering.
- [`audio_routing.cpp`](../core/engine/tests/audio_routing.cpp): mono/stereo/multichannel ports,
  independent buses, sidechains and route validation.
- [`sequencer_timing.cpp`](../core/engine/tests/sequencer_timing.cpp): event timing across
  uneven block sizes.
- [Production LV2 checks](../core/lv2-host/tests/production/README.md): real instruments,
  effects, state assets and cross-platform measurements.

Two checks run on a plugin as built, CLAP or LV2, without writing tests:

- `build-dev/core/control/oscairn-plugin-check PLUGIN ID synth|effect [--param ID=VALUE]`
  plays the plugin in a part, saves it to the library and opens it again with every parameter
  as saved, exports it in an archive with its build, and installs that on a fresh device (a
  new `OSCAIRN_DATA_HOME`), where it must open from the plugin store and play the same. An LV2
  plugin stops after the export. `--juce-state NAME=VALUE` sets an attribute in a JUCE
  plugin's saved state; `--add-build PLATFORM=PATH --keep-archive FILE` carries another
  platform's build, and `--install FILE synth|effect` on that platform checks it there.
- `core/lv2-host/tests/production/run.py PROBE OUT --case NAME,PATH,ID,synth|effect,PARAM,VALUE`
  renders at two rates, two block sizes, offline and realtime, repeats with a fresh instance
  and checks that a parameter edit changes the sound. Its probe is built only with
  `-DOSCAIRN_LV2=ON`.

Run both again with `OSCAIRN_PLUGIN_WORKER=build-dev/core/oscairn-plugin-worker` to test the
plugin in a worker process, as the desktop app runs it.

Exercise 37- and 512-frame blocks, multiple sample rates, simultaneous instances, repeated
keys, sustain, malformed inputs and bounded-queue failures. Test cleanup after failed
preparation and concurrent control/rendering under ThreadSanitizer. Use ASan/UBSan for
memory errors; a clean offline render is not a target-hardware latency measurement.
The host's [benchmark tools](../core/clap-host/README.md) can measure plugin workloads.

Scanning has process crash isolation. Playback runs plugins inside the engine process unless
the placement policy puts them in worker processes ([RFC 0010](rfcs/0010-plugin-worker-isolation.md)),
where a crash, hang or overrun silences only that plugin, which is restarted. The desktop app
runs every CLAP plugin in a worker; `oscairn-render` and the desktop's `--in-process` mode run
them in the engine unless `OSCAIRN_PLUGIN_WORKER` names the worker. LV2 plugins always run in
the engine.
Workers are not a sandbox: a plugin keeps the user's file and network access. In the engine,
a native plugin memory fault can kill it. Do not describe the scanner
or render error checks as a security sandbox. Public APIs are still evolving; pin the
Oscairn revision you build/test against and state your supported platforms and limits.

## Discovery, generic editors and appearance

`oscairn::scanPlugins` (`oscairn/discovery.h`) scans CLAP and optional LV2 bundles through
`oscairn-plugin-scan`, using the existing bounded worker protocol, per-bundle timeout and
cancellation. The UI never loads libraries to inspect metadata. A descriptor's format plus
CLAP ID/LV2 URI is its identity; the path locates the installed binary. Discovery is not an
engine-compatibility guarantee. Loaded plugins execute inside the audio engine, or in a
supervised worker process when the [placement policy](rfcs/0010-plugin-worker-isolation.md)
puts them there.

Expose complete parameter names, ranges, stepped/read-only flags, text formatting and
presets to make the generated editor useful. `Control::replace` / `graph.replace` prepares
a candidate and optional preset before publishing one replacement graph plan. The old
node ID/name remains, and its wires move to the new plugin's ports in the same roles; wires
that no longer fit are kept, marked unconnected, rather than the replacement being refused
([RFC 0011](rfcs/0011-performance-ui-and-editors.md#step-1-status)). A plugin that fails to
load or prepare leaves the old graph playing.
Replacement cuts that slot's old notes/tails and prepared sounds. Use prepared sound
contexts for seamless switching within an engine instead.

System UI components use the shared semantic palette and look-and-feel; device layouts
contain geometry and bindings, not private color schemes. Follow the
[design language and theme schema](design-language.md) when adding a screen. Generated
plugin controls inherit the active theme. Vendor-native GUIs do not; declarative custom
plugin pages and managed theme packages are future work.

### Insert chains and shared effects

The graph can connect an instrument through several effect nodes, fan its output into
parallel chains, and sum several instruments into one shared effect. Per-route gain can
serve as a send amount; route the dry and wet paths deliberately. These capabilities exist
in typed/JSON controls; the initial desktop browser exposes only the demo instrument and
effect slots, not a general rack/routing editor.

A shared effect instance processes the summed input with one setting/state. Two effect
nodes with different settings need independent processing state, even if both receive the same
instrument. Each needs its own mutable DSP state (delay/reverb buffers, envelopes, filter
history); executable code and explicitly shared immutable resources may be shared.
Native Delay implements [effect-contexts.h](../core/clap-ext/include/oscairn-clap/effect-contexts.h).
Create a sibling using `Control::addEffectContext(name, source)` / `effects.create`, then
use ordinary graph routes, parameters and state saving. Each sibling is a distinct graph
node and CLAP interface sharing native resources. Its engine/module survives removal of
the originating node. Other plugins continue using separate instances unless they provide
this extension. Explicit creation fails for unsupported plugins rather than claiming sharing.

Basic Synth's Output bus parameter (ID 8) selects Main or bus 1–8 for new notes. Pass it
in a prepared sound patch and route that port to its effect chain. Held voices retain
that bus, and voices routed separately are absent from Main. Buses are persistent routing
destinations; multiple sounds assigned the same bus intentionally share its chain.
Keep old chains routed until their voices and effects finish. `effects.resources` reports
Delay tail activity; removing a node deliberately cuts its tail. Automatic chain management
remains future work. Complete Performance persistence is described in the
[versioned JSON format](performance-format.md). [RFC 0007](rfcs/0007-effect-contexts.md)
records measured costs and validation; sharing Delay saves essentially no history RAM.

## Saving a Performance

Use the typed `savePerformance(directory)` / `loadPerformance(directory)` methods or
`performance.save` / `performance.load` RPCs. The [Performance format and schema](performance-format.md)
are the shared save/open/import/export contract. Native engines must snapshot all prepared
contexts; ordinary hosted instruments are saved instance by instance. The app's Performance
menu uses a control worker, so disk I/O and plugin preparation stay outside the UI/audio paths.
Keep this format, the schema, protocol and round-trip tests in sync when adding persistent data.

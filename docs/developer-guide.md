# Developing for Oscairn

Oscairn hosts ordinary CLAP plugins and, when enabled at build time, LV2 plugins. You do
not need a separate Oscairn plugin format or the desktop UI to write DSP. Platform-native
instruments additionally implement concurrent sound contexts. Start with the path below
that matches your component; the [documentation index](README.md) links the full contracts.

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
or the planned cross-device portability contract. Long-term compatibility and migration
proposals describe future product behavior, not an obligation to preserve today's
experimental formats. A stable-release compatibility policy must be adopted explicitly.

## Choose the boundary

| Component | Interface today | Reference |
| --- | --- | --- |
| Instrument, sampler or effect | CLAP; optional LV2 hosting | [First-party plugins](../plugins/README.md), [LV2 subset](../core/lv2-host/README.md) |
| Sequencer or note processor | CLAP note output ports and transport; optional parameter-target events | [Step Sequencer](../plugins/step-sequencer/step_sequencer.cpp), [MIDI effects](../plugins/midi-effects/README.md), [RFC 0002](rfcs/0002-param-target.md) |
| Native multitimbral instrument | CLAP plus `oscairn.sound-contexts/draft-2` | [Basic Synth](../plugins/basic-synth/basic_synth.cpp), [RFC 0005](rfcs/0005-sound-contexts.md) |
| UI, editor or automation script | Typed `Control` or JSON-RPC over the local Unix socket | [Protocol and schema](control-protocol.md) |
| New host/application | C++20 `Session` or `Graph`, supplied audio buffers and events | [Core](../core/README.md), [hosts](../hosts/README.md) |
| Hardware panel/layout | Desktop JSON layouts today; timestamped hardware services are proposed | [Layouts](../hosts/standalone/layouts/README.md), [RFC 0004](rfcs/0004-hardware-services.md) |

VST3 hosting, plugin GUI embedding, a distributable Oscairn SDK/package registry, Setlist/library
persistence, automatic DSP/auxiliary-core offloading, and a native DJ deck engine
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

The renderer writes an offline WAV; it does not open an audio device. Socket tests need
permission to bind local sockets. For live output build the [desktop/helper](../hosts/README.md).
For LV2 add `-DOSCAIRN_LV2=ON` and install Lilv/LV2 development packages as described in
the [LV2 guide](../core/lv2-host/README.md). The default build does not require JUCE.

## Make a first CLAP plugin

The [reference plugins](../plugins/README.md) are working code rather than pseudocode.
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
- Keep output events bounded and valid. The shared CLAP host permits 512 direct graph
  input events per call and a separate bounded control queue; overflow can invoke panic.
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
selections and can prepare defaults, a saved state directory, or an LV2 preset URI,
then apply numeric parameter overrides. It never restores state over the outgoing sound.
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
Complete projects, context-pool persistence and a sound-library UI are still future work.

## Packaging and portability (planned)

[RFC 0006](rfcs/0006-packages-and-portability.md) is the proposed distribution contract;
there is no managed package installer yet. The plan uses a project manifest and exact
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
and hardware RFC, but the hardware mapping/acceleration APIs are not implemented yet.
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

Exercise 37- and 512-frame blocks, multiple sample rates, simultaneous instances, repeated
keys, sustain, malformed inputs and bounded-queue failures. Test cleanup after failed
preparation and concurrent control/rendering under ThreadSanitizer. Use ASan/UBSan for
memory errors; a clean offline render is not a target-hardware latency measurement.
The host's [benchmark tools](../core/clap-host/README.md) can measure plugin workloads.

Scanning has process crash isolation; playback currently runs plugins inside the engine
process. A native plugin memory fault can kill that engine. Do not describe the scanner
or render error checks as a security sandbox. Public APIs are still evolving; pin the
Oscairn revision you build/test against and state your supported platforms and limits.

## Discovery, generic editors and appearance

`oscairn::scanPlugins` (`oscairn/discovery.h`) scans CLAP and optional LV2 bundles through
`oscairn-plugin-scan`, using the existing bounded worker protocol, per-bundle timeout and
cancellation. The UI never loads libraries to inspect metadata. A descriptor's format plus
CLAP ID/LV2 URI is its identity; the path locates the installed binary. Discovery is not an
engine-compatibility guarantee. Loaded plugins still execute inside the audio engine.

Expose complete parameter names, ranges, stepped/read-only flags, text formatting and
presets to make the generated editor useful. `Control::replace` / `graph.replace` prepares
a candidate and optional preset before publishing one replacement graph plan. The old
node ID/name remains; incompatible routing or preparation failures preserve the old graph.
Replacement cuts that slot's old notes/tails and prepared sounds. Use prepared sound
contexts for seamless switching within an engine instead.

System UI components use the shared semantic palette and look-and-feel; device layouts
contain geometry and bindings, not private colour schemes. Follow the
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

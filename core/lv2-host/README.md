# LV2 hosting: initial subset

The graph loads CLAP modules and LV2 bundles through `oscairn::PluginHost`. The LV2
adapter uses [Lilv](https://gitlab.com/lv2/lilv) to read Turtle metadata and instantiate
plugins. It does not parse Turtle itself or wrap LV2 in a synthetic CLAP plugin.
The graph/session retain the existing CLAP-shaped event and metadata structures;
translation happens at the LV2 boundary. There is no JUCE dependency in the adapter.

## Build and load

```sh
# macOS
brew install lilv lv2
# Debian/Ubuntu: sudo apt-get install liblilv-dev lv2-dev
cmake -S . -B build-lv2 -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOSCAIRN_LV2=ON
cmake --build build-lv2
ctest --test-dir build-lv2 --output-on-failure
```

`OSCAIRN_LV2` defaults to OFF. Loading an LV2 bundle in a build without it returns a
specific error. When enabled, missing Lilv headers/libraries fail configuration.
Lilv >= 0.24 is required. Plugins must match the host's OS and CPU architecture.

Pass the `.lv2` bundle directory, not its binary or a Turtle file. For a bundle with one
plugin the URI can be omitted. Multiple-plugin bundles require an explicit URI:

```cpp
auto node = graph.add("/path/to/plugin.lv2", "https://example.org/plugin");
```

The same operation is available through `graph.add` in the control API:

```json
{"jsonrpc":"2.0","id":1,"method":"graph.add","params":{"name":"effect","path":"/path/to/plugin.lv2","pluginId":"https://example.org/plugin"}}
```

Connections and `graph.commit` work as for CLAP. Numeric parameter IDs are LV2 control-port
indices; plugin identity is its URI. Metadata names come from the bundle, and descriptor
features include `lv2`. Port symbols are not yet exposed as persistent parameter addresses.

The renderer accepts mixed chains. For example, with upstream examples installed by
Debian's `lv2-examples` package:

```sh
build-lv2/hosts/oscairn-render example.wav --seconds 4 \
    build-lv2/plugins/oscairn-step-sequencer.clap \
    '/usr/lib/lv2/eg-fifths.lv2#http://lv2plug.in/plugins/eg-fifths' \
    build-lv2/plugins/oscairn-basic-synth.clap \
    /usr/lib/lv2/eg-amp.lv2
```

This routes sequencer notes through the LV2 fifths filter into the CLAP synth, then its
audio through the LV2 amplifier. Mono outputs are centered by the graph. Audio ports
are flattened in port-index order internally; explicit graph connections address the
LV2 audio-port IDs and can route independent inputs/outputs and sidechains. See the
[audio routing contract](../../docs/control-protocol.md#audio-routing). The node-only
connection helper retains stereo behavior; there is no surround/bus editor.

## Supported behavior

- Audio ports: preallocated float buffers, separate input/output storage, silent missing
  input channels, unused outputs retained in scratch buffers.
- Control ports: input parameters, read-only output telemetry, ranges/defaults, integer
  and toggle normalization, and sample-rate-scaled bounds (defaults stay in port units).
  Missing bounds use 0/1 as the generic control range; this is a UI/control policy,
  not a claim that LV2 requires those bounds. Formatting is numeric. Gesture
  notifications remain in the control API; LV2 control ports have no gesture message.
- Control changes and mapped CLAP parameter locks: processing splits at the exact sample
  of each change. Plugins requiring fixed or power-of-two block lengths are rejected.
- Atom Sequence ports: frame-timed MIDI input/output and `time:Position` input. Notes,
  poly pressure and channel pressure translate between the graph and MIDI 1.0.
  MIDI CC, program and pitch-bend messages can pass through the graph's raw MIDI events;
  the physical desktop input currently forwards only its existing notes/pressure/clock
  subset. Wildcard note-offs issue all-notes-off on the addressed channels/ports.
- Transport: play/stop speed, tempo, sample position, beat, bar and time signature.
  Positions are rebased when control changes split a block, including tempo increments,
  meter-beat units and the host's current bar origin. A tempo ramp is sampled at each
  run boundary; the LV2 time vocabulary has no standard tempo-ramp field.
- Features: URID map/unmap, options (sample rate, min/max block length, sequence capacity),
  Worker scheduling, bounded block length and separate buffers for `inPlaceBroken` plugins. Required
  options are checked before instantiation. Unsupported optional Atom ports are
  connected to null; unsupported required ports are rejected.
- One control thread and one audio thread per instance, plus a background thread for
  each live instance implementing Worker. Controls use a bounded SPSC queue;
  telemetry is atomic. Plugin creation, metadata reading and destruction happen on the
  control thread. The live host render path uses cached URIDs and preallocated queues;
  it does not allocate or wait for background work. The plugin-facing URID map/unmap callbacks are **not realtime safe**,
  as permitted by LV2: they serialize access and mapping may allocate. Plugins must
  cache their IDs before performance-critical processing.

Limits: 256 ports, 512 input/output graph events per block,
2048 queued control events. Each block can drain 512 queued controls independently of
the 512 graph-event budget, so busy MIDI cannot starve controls. The URID map grows
dynamically and preserves IDs and returned string addresses across insertions. All atom
output sizes/times are validated before traversal.
Malformed/unsupported events and full event buffers increment drop/error counters.
Input MIDI overflow schedules all-sound/all-notes-off for the next block. Output overflow
or malformed MIDI output emits a wildcard note-off to release downstream voices,
replacing the final event when the output buffer is full.
Native plugin code is not sandboxed and can still crash or violate its realtime contract.

### Atom buffer memory policy

Each connected Atom Sequence port gets at least 16 KiB by default, increased to its
declared `resize-port:minimumSize` and rounded up to eight-byte alignment. Defaults
cap each port at 1 MiB and the sum of Atom buffers at 4 MiB **per plugin instance**.
These are memory capacities, not audio block lengths or extra audio latency.
Required ports get the budget first; optional ports that exceed either limit remain
disconnected. Required over-budget ports reject loading with a specific error.
Invalid or overflowing size declarations never become small allocations by truncation.

Hosts can choose another `Lv2BufferLimits` policy at construction through `loadPlugin`,
`Graph`, `Chain`, or `Session`. For example, a constrained hardware host can use:

```cpp
oscairn::Lv2BufferLimits limits;
limits.maxPortBytes = 256 * 1024;
limits.maxTotalBytes = 1024 * 1024;
oscairn::Session session(sampleRate, maxFrames, limits);
```

Limits count aligned allocations including Atom headers, across both input and output
ports. They apply separately to every LV2 instance; this does not impose a whole-graph
memory budget, and CLAP hosting is unchanged. There is no runtime control API setter.
The default must fit a Sequence header and both caps; per-port capacities must fit the
signed Atom Int used for `buf-size:sequenceSize`.

All buffers are allocated before instantiation. `sequenceSize` reports the largest
connected buffer (or the configured default when there are none). Each input packing,
output-capacity announcement and output-validation check uses that port's own size.
Processing neither allocates nor resizes buffers. This implements static `minimumSize`
requests, not the dynamic `resize-port:resize` feature or `asLargeAs` relationships.
Larger buffers do not enable custom UIs or larger graph event queues. Patch requests
and notifications have their own bounded queues, described below.

## Worker and offline rendering

The [Worker implementation](Worker.cpp) adapts David Robillard's Jalv Worker design
under the ISC license. [Provenance](third_party/jalv/upstream.json) records the exact
revision, upstream/local files and changes; [credits](../../CREDITS.md) name the authors.
It uses C++ threads/semaphores and complete-packet SPSC queues instead of adding Zix.

Live processing enqueues copied work requests and wakes one background thread per
Worker-capable instance. Plugin `work()` calls are serialized. Responses are delivered
after `run()` in the audio context, followed by `end_run()` after **every** run, including
sub-runs introduced by parameter edits. Each live response pass consumes at most its
initial queue snapshot, so a producer cannot make it run indefinitely.

Queues are allocated on attachment, outside rendering; plugins without Worker do not
pay for their storage. Request and response queues each hold 32 messages of up to 4096 bytes, including
zero-byte messages. Oversize/full queues return `LV2_WORKER_ERR_NO_SPACE`; invalid
requests or scheduling outside the attached lifecycle return `LV2_WORKER_ERR_UNKNOWN`.
Drops and failed plugin callbacks contribute to the existing host statistics. Plugins
must handle rejected requests/responses and retain or release their own resources
appropriately. Message bytes are copied, but any plugin-owned objects referenced by
those bytes must remain valid under the plugin's Worker protocol.

After graph retirement has stopped rendering an instance, destruction drains accepted
requests and joins the worker before deactivation or cleanup. Successful responses are
delivered in the quiescent processing context, and any follow-up work they schedule runs
synchronously. This supports patterns such as replacing a sample and freeing the old
one from `work_response()`. A plugin whose work never returns can still block teardown;
there is no safe in-process cancellation of arbitrary native plugin code.

`loadPlugin`, `Graph` and `Chain` accept `ProcessingMode::Offline` at construction;
the default remains `Realtime`. `oscairn-render` selects Offline. LV2 work then runs
synchronously, with responses applied before `schedule_work()` returns (after the work
callback exits, avoiding overlapping work calls). Offline processing may allocate and
block inside plugin work; it must not be used on a live audio callback. Mode is fixed
for an instance's lifetime. CLAP processing behavior is unchanged; this does not add
CLAP render-mode extension negotiation or LV2 freewheel-control handling.

A plugin that also requires an unsupported feature remains rejected, and a required
Worker feature with a missing or invalid plugin Worker interface fails loading with a
specific error. State operations exclude rendering, drain/join the worker, deliver
pending responses and synchronous follow-up work, then resume the background worker.
This serialization also applies to plugins that do not advertise thread-safe restore.

## State, presets and patch properties

The shared C++ `PluginHost` API now exposes `supportsState()`, `saveState(directory)`
and `loadState(directory)` for CLAP and LV2. Access it directly or through `Graph::host`.
State export, preset enumeration and typed patch properties are also exposed through
[Control/JSON-RPC](../../docs/control-protocol.md#state-export-presets-and-patch-properties).
Eligible instruments load state/presets through `sounds.prepare` followed by `sounds.select`.
The desktop sound picker and whole-graph/project persistence remain future work.

Saving requires a **new directory**. A private staging directory is published only after
successful serialization; an existing destination is never deliberately overwritten.
LV2 uses Lilv for Turtle, symbol-addressed control-port values, default state and path
mapping. File assets registered by the plugin through `state:mapPath` are copied into
the snapshot, including materializing Lilv's external-file links. `makePath` provides
instance-local scratch storage; directory assets are currently rejected. Snapshot
relocation depends on plugins using the LV2 path contract correctly; arbitrary strings
or embedded absolute paths cannot be repaired automatically. Missing mapped assets and
plugin save/restore errors are reported. A failed restore silences the affected instance
until a successful restore, rather than rendering partially restored state. State Turtle
and CLAP opaque-state files are limited to 64 MiB; sample asset sizes are not capped.

CLAP wraps the existing host state stream with plugin identity. It does **not** rewrite
or copy asset paths hidden inside opaque CLAP data. CLAP preset discovery/loading
extensions remain unimplemented. LV2 `presets()` and `loadPreset(uri)` enumerate/load
preset resources associated with the selected bundle; there is no global preset-library
scan yet. Complete state support is advertised only when the plugin supplies its state
interface, not merely because some parameters can be saved. LV2 default state is loaded
before activation when declared; plugins without one retain their normal startup state.

`properties()` reports advertised LV2 patch properties and their known scalar types.
`setProperty(uri, value)` and `requestProperty(uri)` enqueue requests for the next process
block. Supported values are Bool, Int, Long, Float, Double, String and Path (`FilePath`),
with finite numeric validation and existing regular-file checks for outgoing paths.
Acceptance means queued, not successfully loaded. Call `service()` on the control thread
and read `propertyValue(uri)` for the last plugin-confirmed value; it remains absent
until reported. Set and Put notifications are decoded, including safe trailing zero
padding used by samplv1. Unsupported/custom Atom types are not a generic UI transport.
CLAP has no direct equivalent of LV2's generic patch protocol; these capabilities remain
explicitly unsupported in its adapter.

There are at most 256 advertised properties, 32 requests and 32 notifications per
instance, each at most 4096 bytes; outgoing strings/paths are limited to 2048 bytes.
Queues are allocated only for instances with connected patch ports. The first patch
input receives requests. Parsing/strings/cache updates happen in `service()`, while
rendering only copies bounded packets. Requests must also fit the input Atom port; full
queues reject requests, and overflowed output or malformed notifications contribute to
host counters. A full Atom input block can still drop an accepted request, so callers
must check confirmation/counters. No acknowledgements, request IDs, arbitrary nested
property values, or sample-offset scheduling API are implied. Queued patch requests
must be processed before saving; Worker requests already accepted are drained first.

State methods belong to the one control thread and can exclude this instance's audio
processing; live callbacks emit silence instead of waiting. Queued numeric edits are
included when saving and discarded on restore. Missed events trigger note-release
recovery. **In-place restore is not seamless sound switching.** Prepare state on a spare
instance while the old one renders. New-note switching and note ownership are implemented
for eligible instruments by the graph's prepared-instance pool. Previously processed
instances retain their tails until explicit retirement.
Neither plugin format guarantees multitimbrality. Native Oscairn instruments are required
to support concurrent sound contexts; the Basic Synth now implements the native interface
([RFC 0005](../../docs/rfcs/0005-sound-contexts.md)). Ordinary LV2 instances still require
the prepared-instance pool and note-ownership router; see the
[prepared-sound contract](../../docs/control-protocol.md#prepared-sounds).
See the [persistent-engine design](../../docs/architecture.md#persistent-engines-and-fast-sound-switching).

## Not implemented yet

Custom plugin UIs,
CV ports, legacy event/URI-map ports, dynamic ports, SysEx, MIDI 2.0, per-note IDs and
non-pressure CLAP expressions. Required unsupported features or non-optional port types
are rejected with an error instead of pretending to support them. There is no plugin
catalog, LV2 scanner process, latency compensation, or automatic LV2 selection in the
fixed desktop demo. Control-port designations such as tempo, freewheeling and latency
are not yet driven/interpreted automatically; tempo is supplied through Atom Sequence
ports only. A keyed CLAP choke currently translates to MIDI note-off (release semantics).
These are explicit follow-up work, not broad LV2 compatibility claims.

## Verification

[Production-plugin tests](tests/production/README.md) cover three MOD MDA instruments,
three MOD MDA effects, ZamComp and three Guitarix effects through the normal adapter,
plus mixed CLAP/LV2 chains. The report records exact builds, measurements and the
x42 stereo EQ's initial Atom-buffer incompatibility and successful retest after adding
configurable sizing. CI repeats the six pinned MOD plugins on Linux and macOS and the
packaged x42 stereo EQ on Linux; other packaged-plugin tests are opt-in.

`lv2-host` uses a real native bundle with Turtle metadata, an instrument/effect, an atom
MIDI echo, transport telemetry and intentionally malformed output. It checks exact-sample
notes/pressure/control edits, mixed CLAP/LV2 routing in both directions, parameter locks
with 37- and 512-sample blocks, queue overflow, unsupported features/ports, the JSON
control API, and concurrent control/render operation. Rendering runs under the existing
allocation guard. The fixture exercises a bundle path containing a space.

`lv2-worker` checks copied/zero/maximum-sized messages, request and response saturation,
callback errors, serialized background work, reentrant follow-up scheduling, response
thread identity and draining shutdown. The host fixture verifies feature negotiation,
live response application, sample-exact offline work, end-run calls across split blocks
and block-size-independent offline graphs. Worker queue operations and live response
delivery run under the allocation guard.

The host tests also exercise unequal large buffers, alignment, actual per-port output
capacity, the instance `sequenceSize` option, required-before-optional budgeting,
configurable defaults/caps, malformed/overflowing metadata, and input/output bounds.
A large opaque notification followed by MIDI verifies parsing beyond the old 16 KiB
limit; malformed large output triggers the existing release/error policy. Rendering
these cases runs under the allocation guard and Linux ASan/UBSan.

For independent upstream examples, install `lv2-examples` and configure with
`-DOSCAIRN_LV2_EXAMPLE_DIR=/usr/lib/lv2`. Additional tests check eg-amp at unity and eg-fifths
with byte-identical 37/512-sample renders. CI enables LV2 on Linux and macOS and includes
Linux ThreadSanitizer. CLAP-only builds remain supported.

For an independent Worker implementation, configure
`-DOSCAIRN_LV2_SOURCE_DIR=/path/to/lv2-source` with libsndfile and libsamplerate development
packages installed. Linux CI checks out LV2 v1.18.10 at
`0bcde338db1c63bbc503b4d1f6d7b55ed43154af` and compiles its unmodified eg-sampler.
`lv2-worker-upstream` uses a direct plugin ABI harness with Oscairn's actual Worker,
loads/replaces WAV samples through patch events, checks exact-sample offline output
across 37/512 blocks and repeats, and tests live loading and pending-work shutdown.
`plugin-state` additionally loads the unmodified sampler through the normal adapter:
default state, patch sample replacement, relocation after deleting the original sample,
shared CLAP state, live Worker/render exclusion, and two independent instances where
preparing the incoming sound leaves the outgoing sample playing. This is not an automatic
sound-switching test. No upstream sampler source is vendored.

`lv2-state-contract` tests bundle presets, symbol-addressed controls, queued edits,
save/restore failures and recovery, plugin identity, bounded property queues, typed
values, Set/Put notifications and malformed nested atoms/strings. Worker tests include
repeated quiesce/resume with synchronous follow-up work. These paths run under the live
allocation guard and sanitizers. The optional production `oscairn-lv2-sampler-state-test`
checks samplv1 sample loading, Get/Set notifications, audio, queued control state and
relocation with owned assets; Linux CI runs it with Qt's offscreen backend.

The [prior-art audit](AUDIT.md) records the pinned Jalv, MOD mod-host, Carla and Ardour
comparisons, fixes, remaining gaps and reuse decisions. [Credits](../../CREDITS.md)
distinguish dependencies, reference work and imported code.

Reference contracts: [LV2 core](https://lv2plug.in/ns/lv2core),
[Atom](https://lv2plug.in/ns/ext/atom), [URID](https://lv2plug.in/ns/ext/urid),
[buffer sizes](https://lv2plug.in/ns/ext/buf-size), [time](https://lv2plug.in/ns/ext/time).

Local validation also covers Linux AddressSanitizer/UndefinedBehaviorSanitizer and macOS
ThreadSanitizer, plus the muted managed-desktop lifecycle test. Native macOS ASan testing
currently fails in CLAP module global registration/reloading; the existing CLAP graph test
also reproduces a sanitizer global-registration failure on unchanged `main`. It is not
counted as passing. No sanitizer suppression was added. The LV2-enabled Buildroot image
and audible third-party LV2 testing on hardware remain unverified.

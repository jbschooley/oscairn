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
are flattened in port-index order, as for CLAP; there is no surround/bus editor.

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
  bounded block length and separate buffers for `inPlaceBroken` plugins. Required
  options are checked before instantiation. Unsupported optional Atom ports are
  connected to null; unsupported required ports are rejected.
- One control thread and one audio thread per instance. Controls use a bounded SPSC queue;
  telemetry is atomic. Plugin creation, metadata reading and destruction happen on the
  control thread. Host render work uses cached URIDs and does not allocate or wait
  for a lock. The plugin-facing URID map/unmap callbacks are **not realtime safe**,
  as permitted by LV2: they serialize access and mapping may allocate. Plugins must
  cache their IDs before performance-critical processing.

Limits: 256 ports, 16 KiB per atom buffer, 512 input/output graph events per block,
2048 queued control events. Each block can drain 512 queued controls independently of
the 512 graph-event budget, so busy MIDI cannot starve controls. The URID map grows
dynamically and preserves IDs and returned string addresses across insertions. All atom
output sizes/times are validated before traversal.
Malformed/unsupported events and full event buffers increment drop/error counters.
Input MIDI overflow schedules all-sound/all-notes-off for the next block. Output overflow
or malformed MIDI output emits a wildcard note-off to release downstream voices,
replacing the final event when the output buffer is full.
Native plugin code is not sandboxed and can still crash or violate its realtime contract.

## Not implemented yet

Worker scheduling, plugin state/presets, patch/property messages, custom plugin UIs,
CV ports, legacy event/URI-map ports, dynamic ports, SysEx, MIDI 2.0, per-note IDs and
non-pressure CLAP expressions. Required unsupported features or non-optional port types
are rejected with an error instead of pretending to support them. There is no plugin
catalog, LV2 scanner process, latency compensation, or automatic LV2 selection in the
fixed desktop demo. Control-port designations such as tempo, freewheeling and latency
are not yet driven/interpreted automatically; tempo is supplied through Atom Sequence
ports only. A keyed CLAP choke currently translates to MIDI note-off (release semantics).
These are explicit follow-up work, not broad LV2 compatibility claims.

## Verification

`lv2-host` uses a real native bundle with Turtle metadata, an instrument/effect, an atom
MIDI echo, transport telemetry and intentionally malformed output. It checks exact-sample
notes/pressure/control edits, mixed CLAP/LV2 routing in both directions, parameter locks
with 37- and 512-sample blocks, queue overflow, unsupported features/ports, the JSON
control API, and concurrent control/render operation. Rendering runs under the existing
allocation guard. The fixture exercises a bundle path containing a space.

For independent upstream examples, install `lv2-examples` and configure with
`-DOSCAIRN_LV2_EXAMPLE_DIR=/usr/lib/lv2`. Additional tests check eg-amp at unity and eg-fifths
with byte-identical 37/512-sample renders. CI enables LV2 on Linux and macOS and includes
Linux ThreadSanitizer. CLAP-only builds remain supported.

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

# LV2 host prior-art audit

Reviewed on 2026-10-28 against Oscairn's initial LV2 implementation at
`d0af49e9b234a320523807d03b23c3404bb12203`. This is a source-level comparison of
the adapter's contracts and lifecycle, not a security certification or a claim
that every plugin supported by these hosts now works in Oscairn. The upstream
applications were inspected, not built or benchmarked against Oscairn.

Follow-up: Worker support has since been implemented with an attributed C++ adaptation
of Jalv's design. Configurable Atom sizing and normal-host state/presets/patch support
have also landed. State serialization/path mapping reuse Lilv, with rendering/Worker
exclusion, symbol-addressed ports, owned file assets, missing-asset checks and error
propagation; no additional source chunks from the audited hosts were imported.
See [current scope and tests](README.md) and [credits](../../CREDITS.md).
Recommendations and validation below describe the original audit.

## Sources and attribution

All links below pin the revisions inspected. Credits and the policy for any
future imported source are in [CREDITS.md](../../CREDITS.md).

| Host | Revision | Areas inspected |
| --- | --- | --- |
| [Jalv](https://github.com/drobilla/jalv/tree/a0f01621908bfdda55f568d569853429dd2a3544) | `a0f0162` | `src/mapper.c`, `symap.c`, `lv2_evbuf.c`, `features.c`, `control.c`, `process_setup.c`, `worker.c`, `state.c` |
| [MOD mod-host](https://github.com/mod-audio/mod-host/tree/12335c2653740be7fe9ce5b4e017f4086bd5c071) | `12335c2` | `src/effects.c`, `uridmap.c`, `worker.c`, `lv2_evbuf.c` |
| [Carla](https://github.com/falkTX/Carla/tree/97a9e0740baf6df2df942495c02532a624c44682) | `97a9e07` | `source/backend/plugin/CarlaPluginLV2.cpp`, `source/includes/lv2_rdf.hpp`, `source/utils/CarlaLv2Utils.hpp` |
| [Ardour](https://github.com/Ardour/ardour/tree/cc2b139c48bc0ac3077af887d820a055d1d478d1) | `cc2b139` | `libs/ardour/lv2_plugin.cc`, `lv2_evbuf.cc`, `worker.cc` and associated headers |

The LV2 specifications remain authoritative when implementations differ:
[core](https://lv2plug.in/ns/lv2core), [URID](https://lv2plug.in/ns/ext/urid),
[options](https://lv2plug.in/ns/ext/options), [Atom](https://lv2plug.in/ns/ext/atom),
[time](https://lv2plug.in/ns/ext/time), and
[buffer sizes](https://lv2plug.in/ns/ext/buf-size).

## Findings fixed in this audit

1. **URID mapping failed under normal growth or contention.** The original
   mapper stopped at 512 names/511 bytes and returned zero when another caller
   was inserting. LV2 expects normal mappings to succeed and permits a non-RT
   map callback. [Jalv's mapper](https://github.com/drobilla/jalv/blob/a0f01621908bfdda55f568d569853429dd2a3544/src/mapper.c)
   and [MOD's mapper](https://github.com/mod-audio/mod-host/blob/12335c2653740be7fe9ce5b4e017f4086bd5c071/src/uridmap.c)
   serialize access to dynamic maps. Oscairn now uses an original C++ dynamic
   mapper with stable strings, serialized callbacks and failure handling that
   never unwinds through the C interface. Host processing uses cached IDs.
   Tests concurrently map 1,500 distinct URIs from four threads, round-trip a
   2,060-byte URI, and retain an unmapped pointer across growth.

2. **Required options were ignored.** Feature checks alone did not reject a
   plugin requiring an unknown option. [Ardour's discovery checks](https://github.com/Ardour/ardour/blob/cc2b139c48bc0ac3077af887d820a055d1d478d1/libs/ardour/lv2_plugin.cc#L4006)
   provided the useful comparison. Oscairn now checks each `opts:requiredOption`
   against the options actually supplied before instantiation. Tests reject an
   unknown option and verify sample-rate/max-block option types and values from
   inside a native plugin.

3. **Optional unsupported Atom ports caused load failures.** Generic unknown
   optional ports were disconnected, but the Atom branch unconditionally
   rejected unsupported buffer types. [Jalv's port setup](https://github.com/drobilla/jalv/blob/a0f01621908bfdda55f568d569853429dd2a3544/src/process_setup.c#L175)
   distinguishes required from optional connectivity. Oscairn now connects null
   for unsupported optional Atom types or sizes and continues to reject required
   ones. It also searches all declared buffer types for Sequence. The fixture
   verifies null connections for an optional Chunk and an oversized optional
   Sequence, accepts a port declaring both Chunk and Sequence, and checks the
   rejection reason for a required Chunk port.

4. **Sample-rate controls scaled their defaults incorrectly.** `lv2:sampleRate`
   applies to bounds. The old code also multiplied the default, turning a 440 Hz
   default into a value clamped to Nyquist at 48 kHz. Compare [Jalv control metadata](https://github.com/drobilla/jalv/blob/a0f01621908bfdda55f568d569853429dd2a3544/src/control.c#L61),
   [MOD's range/default handling](https://github.com/mod-audio/mod-host/blob/12335c2653740be7fe9ce5b4e017f4086bd5c071/src/effects.c#L4859)
   and [Carla's handling](https://github.com/falkTX/Carla/blob/97a9e0740baf6df2df942495c02532a624c44682/source/backend/plugin/CarlaPluginLV2.cpp#L2852).
   Defaults now stay in port units; tests observe 440 at the plugin and a 24,000 Hz
   upper bound.

5. **Toggle and integer initialization disagreed with subsequent writes.** A
   positive fractional toggle default remained fractional, while a later positive
   fractional direct write became zero through truncation. Integer defaults could
   also remain fractional. LV2 defines toggles by positive/non-positive values.
   Oscairn exposes canonical 0/1 toggles, normalizes input values, and restricts
   stepped integer ranges to their contained integers. Queued and sample-timed
   writes share this policy. Tests inspect the actual connected controls. The
   shared control API still follows its existing CLAP stepped-value convention;
   clients should send integer choices there.

6. **Transport mixed beat units and discarded the host's bar origin.** In 6/8,
   barBeat used eighth notes while beat/BPM remained in quarter notes. Bar numbers
   were inferred from the current meter rather than the supplied bar origin, and
   split runs ignored `tempo_inc`. The time-object fields in [Carla](https://github.com/falkTX/Carla/blob/97a9e0740baf6df2df942495c02532a624c44682/source/backend/plugin/CarlaPluginLV2.cpp#L3853)
   and [Ardour](https://github.com/Ardour/ardour/blob/cc2b139c48bc0ac3077af887d820a055d1d478d1/libs/ardour/lv2_plugin.cc#L2788)
   were checked against the LV2 vocabulary rather than copied. The adapter now
   converts beat and tempo consistently, preserves the supplied bar origin, and
   accounts for tempo increments at splits. Tests cover a 6/8 bar crossing after
   earlier meter history, stopped transport and negative preroll. Ramp updates
   remain sampled at run boundaries; this is not a new continuous-ramp extension.

7. **Busy MIDI could indefinitely starve queued controls.** The shared 512-event
   budget left no space to drain control commands when graph events filled every
   block. Separate bounded budgets now admit up to 512 queued controls plus 512
   graph events. A regression sends a full event block and verifies that a queued
   gain change still reaches the plugin in that block. This finding arose from
   reviewing Oscairn's queue behavior alongside the hosts' separate control/event
   communication paths; it is not transplanted upstream code.

8. **Malformed output could strand downstream notes.** Bounds validation already
   prevented host traversal beyond an Atom buffer, but discarded note-offs could
   leave voices held. Invalid MIDI sequences/events now request a downstream
   wildcard release, including when no valid events were emitted. Overflow retains
   the same bounded release policy. Tests cover invalid sequence sizes, event
   sizes and timestamps, plus existing input/output overflow recovery.

## Checks retained and limits made explicit

- **Atom storage:** compared alignment, Sequence input reset, Chunk output capacity,
  padding and iteration with [Jalv's event buffer](https://github.com/drobilla/jalv/blob/a0f01621908bfdda55f568d569853429dd2a3544/src/lv2_evbuf.c)
  and the MOD/Ardour variants. Oscairn keeps its original implementation: a
  16 KiB total allocation, output body capacity excluding the Atom header, padded
  append bounds, and checked output traversal. Importing the helper wholesale would
  not remove the need for these output checks. Required larger buffers are rejected;
  dynamic resizing is not advertised.
- **Variable run lengths:** min/max options describe actual runs; fixed and
  power-of-two requirements remain rejected. Exact-sample control splitting cannot
  honestly advertise those features. Existing 37/512 render comparisons remain.
- **Lifecycle:** buffers/features outlive plugin cleanup; activation, metadata and
  destruction stay off the audio thread. No Worker or state feature is advertised.
- **Metadata policy:** absent control bounds still use 0/1, a convention also found
  in Carla and MOD. Oscairn clamps control writes to its exposed range. This can
  limit poorly described controls; it is documented rather than mistaken for an
  LV2 requirement. Symbols, scale-point labels and logarithmic presentation remain
  future metadata work.
- **Remaining compatibility gaps:** no Worker, state/presets, patch messages,
  custom UI, CV, legacy event ports, SysEx, plugin latency compensation, or
  automatic handling of designated control ports (tempo, freewheel, latency).
  Keyed CLAP choke is currently approximated by MIDI note-off. Parameter telemetry
  reflects processed values, not synchronous completion of queued edits. These
  limits preclude claiming general Carla/MOD/Ardour-level compatibility.

## What to reuse next

**Keep Lilv as the metadata/instantiation/state foundation.** There is no benefit
in importing another Turtle parser or replacing the graph with a whole host.
Carla is especially valuable as a compatibility reference, but its engine, UI,
bridge and platform integrations make a wholesale backend import disproportionate
for this adapter. MOD's JACK graph is likewise not a replacement for Oscairn's
existing portable graph.

**Worker is the next useful extension.** Start from [Jalv's small Worker implementation](https://github.com/drobilla/jalv/blob/a0f01621908bfdda55f568d569853429dd2a3544/src/worker.c)
and [Ardour's worker lifecycle](https://github.com/Ardour/ardour/blob/cc2b139c48bc0ac3077af887d820a055d1d478d1/libs/ardour/worker.cc).
Jalv's transactional ring publication is preferable to copying the older MOD
two-write packet scheme without analysis. Evaluate direct reuse with Zix against
adapting to Oscairn's queues. Required invariants: complete packet publication,
bounded queues and response work, audio-thread response delivery and `end_run`,
serialized plugin work, shutdown/join before instance destruction, and a defined
offline rendering mode. Tests must include saturation, shutdown with pending work,
zero-length messages and repeatable offline output. A Worker thread is a non-RT
background service, not the hardware DSP/real-time-core abstraction in RFC 0004.

**State follows Worker and a shared plugin-state API.** Reuse Lilv's state machinery
and inspect [Jalv's restore sequencing](https://github.com/drobilla/jalv/blob/a0f01621908bfdda55f568d569853429dd2a3544/src/state.c#L177)
and Carla's path/state features. Restore must exclude processing unless the plugin
explicitly supports thread-safe restore; state workers must share the appropriate
work serialization. Use port symbols for persistence, manage asset paths and their
ownership, and verify relocation and missing assets. Do not present a parameter
snapshot as complete plugin state. Designated controls, latency reporting and a
small representative third-party plugin matrix should follow before broader claims.

## Validation

The native fixture exercises the fixes above under the existing allocation guard.
After the final code changes: macOS passed 33/33 tests, Linux passed 39/39
(including the independent upstream eg-amp and eg-fifths rendering tests), the
macOS ThreadSanitizer LV2 test passed, and the Linux
AddressSanitizer/UndefinedBehaviorSanitizer LV2 test passed. Local-socket tests
were required to bind successfully rather than skip.
See [README.md](README.md#verification) for the existing native macOS ASan limitation
and hardware/Buildroot work that has not been validated. There was no listening
test or new desktop launch in this audit.

No source chunks from the four audited hosts were imported in this change. The
fixes are original implementations informed by the documented contracts and
comparisons above. Any future import must carry its source notices and provenance
when it lands, not as a later documentation task.

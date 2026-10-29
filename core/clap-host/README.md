# CLAP host

Oscairn's raw CLAP adapter retains the `kick::clap` namespace and `kick-clap-*` tool
names from the KICK host imported with its history. It has no device or JUCE dependency.
The [shared PluginHost](../plugin-host/include/oscairn/plugin_host.h) and graph build on it;
new plugin authors should start with the [developer guide](../../docs/developer-guide.md).
The vendored CLAP SDK revision is in `third_party/clap/upstream.json`.

## Build and use

From the Oscairn root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
build/core/clap-host/kick-clap-scan build/plugins/oscairn-basic-synth.clap
```

The raw host can also be built with `cmake -S core/clap-host -B build-clap`.
Its optional JUCE player/session and benchmark adapters are inherited diagnostic tools,
not the main Oscairn desktop. Oscairn's app uses JUCE 9 via `OSCAIRN_STANDALONE=ON`;
see [host instructions](../../hosts/README.md). The optional raw-host tools instead accept
`KICK_CLAP_JUCE_PATH` for a local JUCE checkout, with `KICK_CLAP_BUILD_PLAYER`,
`KICK_CLAP_BUILD_JUCE` or `KICK_CLAP_BUILD_DEVICE_BENCH` as appropriate.

## Processing and lifecycle

- Main-thread operations: library loading, discovery results, metadata queries,
  plugin activation/deactivation, restart handling, state, timers, file descriptors,
  UI, and destruction. Entry initialization/deinitialization is shared per binary
  across instances, and plugins are selected by their stable ID.
- The host-owned render path never acquires a blocking mutex, allocates memory, reads
  files, or performs library loading. Fixed-size queues accept multiple MIDI
  producers. Events are stably ordered, then rebased when blocks exceed the
  plugin's negotiated maximum size. Parameter cookies are refreshed on rescan.
- A per-instance atomic gate excludes state/reconfiguration work from processing.
  The callback returns silence while the gate is held; restore/restart can interrupt
  sound. State save normally runs concurrently with processing as allowed by CLAP;
  queued parameter commands are flushed under the gate only when audio is not progressing.
  The graph prepares separate instances for sound changes. Exclusive stop/reset/flush scopes
  are marked as CLAP's **symbolic audio thread**, as explicitly permitted by the
  thread-check specification. They never run concurrently with processing.
- Input/output port layouts are queried rather than assumed stereo. The core
  supports up to 64 total channels in each direction (distributed across mono or
  multichannel buses), with flattened
  planar routing, silent missing inputs, scratch storage for unused outputs, and
  mono-to-stereo duplication for a mono instrument on the stereo convenience path.
- Native CLAP notes and MIDI events are translated according to input-port dialects.
  Core callers supply sample-timed events. Oscairn's graph adds source ownership and
  prepared sound switching above this raw adapter; see the [developer guide](../../docs/developer-guide.md).
- The caller supplies CLAP transport flags/positions. Oscairn uses its Session transport;
  the raw host does not invent an independent timeline or open a MIDI device.
- Host extensions include thread-check, parameters, state dirty notifications,
  audio/note ports, latency notifications, timers, POSIX file descriptors, tail
  notifications, and bounded log handling. Main-thread callbacks and restart/
  process/flush requests are serviced by the message loop.
- Idle instruments returning `CLAP_PROCESS_SLEEP` skip DSP until an event, input
  signal, wake request, or transport transition (including Play/Stop, tempo and
  seek). Normal beat progression does not itself wake an idle plugin. Invalid
  process statuses and nonfinite mapped output silence and fault that instance. Event overflow is counted and triggers a reset
  to avoid stuck notes. Per-instance callback-time, drop and error counters are
  available without locks. No thread-pool extension is advertised. Deadline/control-pause and idle-skip
  counters distinguish different causes of missing or intentionally skipped DSP.

## Measure candidate plugins on the target

`kick-clap-bench` runs offline against an exact instrument ID. It measures a
stereo host workload with multiple instances, held chords and repeated note
changes, synthetic play/stop/tempo/seek activity, optional parameter edits, and
optional opaque state snapshots. It runs as fast as possible. It does not open
an audio device or measure ALSA xruns.

```sh
build/core/clap-host/kick-clap-bench --plugin /path/to/instrument.clap \
  --id plugin.unique.id --rate 48000 --frames 128 --instances 4 --notes 8 \
  --seconds 30 --warmup 3 --output baseline.json
```

Add `--parameter <clap-id> --automation-hz 30` to exercise a chosen writable
parameter. Add `--snapshot-ms 250` in a separate run to measure the cost of the
current snapshot policy. Snapshot bytes are discarded; no project or preset is
modified. Comparing these runs exposes state-save interruptions as well as DSP
load. An instance count is a count of plugin instances; `--notes` requests notes
per instance, whose actual voice allocation is controlled by the plugin.

The optional JUCE build also produces `kick-clap-device-bench`. For a headless
measurement build, enable only `KICK_CLAP_BUILD_DEVICE_BENCH`; the device tool
links JUCE's audio-device/events modules and does not need the touchscreen panel
or player. On Linux this supports ALSA measurement without an X11 GUI:

```sh
cmake -S core/clap-host -B /tmp/kick-device-bench \
  -DKICK_CLAP_BUILD_DEVICE_BENCH=ON -DKICK_CLAP_JUCE_PATH=/path/to/JUCE \
  -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/kick-device-bench --parallel
```

List and select an output device:

```sh
/tmp/kick-device-bench/kick-clap-device-bench --list-devices
/tmp/kick-device-bench/kick-clap-device-bench --audio --driver ALSA \
  --device 'exact name from the list' --plugin /path/to/instrument.clap \
  --id plugin.unique.id --rate 48000 --frames 128 --instances 4 --notes 8 \
  --seconds 60 --warmup 3 --output device.json
```

Device output is muted by default while plugins still render the full workload.
`--audible` enables the normalized stereo mix at 0.1 gain. The requested device,
sample rate and buffer size must be accepted; the tool does not substitute a
format. Actual device errors, stopped callbacks and unexpected format changes
fail the run. Device-reported input/output latency is in samples, not measured
round-trip latency. `device.native_xruns` is a measurement-period delta, or
`null` when the backend does not supply it. Callback deadline misses remain a
separate metric and cannot prove that a device has no xruns.

JSON reports include configuration, plugin identity/version, configured Git
revision, build type/compiler/architecture, load time (including discovery), measured frames, mean/peak render
load, bounded p50/p95/p99 load estimates, deadline misses, control-gate silent
buffers, idle skips, dropped events, processing errors, peak signal amplitude,
and state-save cost/failures. Load ratio **1.0** consumes the entire buffer
period. Percentile values are upper bounds in 0.01 load-ratio bins; `null` means
the quantile reached the overflow bin (10.23 or higher). Warmup is excluded from
timing. Exit 0 means the workload completed, not that its performance is safe.
Core diagnostic counters are cumulative per host and also appear in the panel:
**missed** counts DSP calls exceeding their buffer period; **paused** counts
buffers silenced while state/control work owns the processing gate.

Use `--output` for machine-readable reports because plugins may print to stdout.
The Python 3.8+ matrix runner supervises each case in its own process, enforces a
wall deadline and output limit, retains crashes/timeouts, and checkpoints results
after every completed case. On POSIX it terminates the owned process group,
including plugin descendants. It hashes regular plugin/executable files and
records the machine/OS alongside each configuration. macOS bundle-directory
hashes are reported as `null`.

```sh
python3 core/clap-host/tools/benchmark_matrix.py \
  --bench /tmp/kick-device-bench/kick-clap-device-bench --audio --driver ALSA \
  --device 'exact name from the list' --plugin /path/to/instrument.clap \
  --id plugin.unique.id --frames 64,128,256 --instances 1,4,8 \
  --notes 8 --seconds 60 --warmup 3 --output target-matrix.json
```

The default case deadline is warmup + duration + 30 wall seconds; override with
`--timeout` when needed. Keep baseline, knob-stress and snapshot-stress reports
separate. These tools measure the CLAP workload, not the rest of Oscairn's graph or
sequencer. Record board revision, image/SDK, CPU governor, temperature and other
active workloads when comparing target runs. Measure sustained playback and
round-trip latency on the board before selecting supported configurations.

Install the tools with `cmake --install <build> --prefix /usr`. Use the matching
Buildroot SDK to build production AArch64 binaries. CI also cross-builds the
core/tools with the Debian/Ubuntu toolchain in
`cmake/aarch64-linux-gnu.cmake`, then runs a fixture smoke check through QEMU.
That checks target compatibility; its timings are not device measurements. Hardware measurements still require the target board and its complete host workload.

## Deliberate limits

Latency is reported, but the Oscairn graph does not yet compensate plugin latency.
Plugin editors and the CLAP thread-pool extension are not exposed by Oscairn's desktop.
Graph note/event routing is separate from physical MIDI output, which is not implemented.
SysEx and pointer-bearing events are deliberately rejected by the bounded event API.

Discovery runs binaries in a separate executable with deadlines and crash detection.
This is crash isolation, not a security sandbox. Playback runs in-process: a plugin memory
fault can terminate the engine. Error statuses and nonfinite audio fault/silence the instance.

## Verification

The raw-host CTest targets use shared-library fixtures for lifecycle/thread checks,
native/MIDI note conversion, sample offsets, ports, parameters, state, idle sleep/wake,
concurrent rendering, overflow recovery, and plugin failures. Scanner fixtures deliberately
crash/hang in child processes. Oscairn's separate engine tests exercise graph ownership,
channel routing and native/pooled sound switching above the raw adapter. Inherited JUCE
player/session tests run only when those optional adapters are enabled.

Enable `-DKICK_CLAP_SANITIZE=ON` for ASan/UBSan on the core, consumers and fixture modules.
The [CI workflow](../../.github/workflows/ci.yml) runs the Oscairn suite on Linux and
macOS, plus Linux ThreadSanitizer. Production LV2 probes are documented separately in
[their test guide](../lv2-host/tests/production/README.md).

Hardware boot, round-trip latency, xruns and sustained plugin performance must be measured
on the target. Desktop tests do not establish a safe voice count or buffer size for an
embedded board.

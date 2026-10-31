# RFC 0010: Plugin worker isolation

Status: proposed, 2026-10-30. Step 1 measured on macOS only ([results](#step-1-measurements));
steps 2 and 3 implemented on macOS ([step 2](#step-2-status), [step 3](#step-3-status)); steps 4–7
not started. Moved ahead of Phase 4 hardware work and further
third-party plugin features ([roadmap](../roadmap.md#next-build-and-test-order)); it makes
concrete the [engine recovery](../architecture.md#engine-recovery) and sandboxed-worker
isolation the architecture already describes.

## Problem

Every plugin runs inside the engine process today. A plugin that crashes takes the whole engine
down with it: every sound stops, held notes and tails are lost, and the desktop only offers a
manual restart. A plugin that hangs or overruns stalls the audio callback for all plugins. A
plugin that leaks memory or misuses threads can damage the rest of the engine before anything
notices. Plugin *scanning* is already isolated in helper processes; running plugins is not.

For an instrument played live with third-party plugins, this is the largest reliability gap.

## Outcome

- A plugin that crashes, hangs or exceeds its limits never stops the output or other sounds.
  Its node is silenced at once, the failure is reported, and the plugin is restarted with its
  last state and faded back in. Held notes and tails of that plugin may be lost.
- Isolation keeps today's guarantees for everything else: sample-accurate events between
  nodes, no added latency in the default configuration, no allocation or locking on the audio
  path, and native sound contexts, parameter targets and sound assets across the boundary.
- Memory and CPU limits per worker; network and file-system access only as granted.
- The same design runs on macOS and on the Linux device, and measured costs decide its defaults.

## Prior art

| Host | Isolation | Notes |
| --- | --- | --- |
| Bitwig Studio | Hosting modes from *Within Bitwig* to *Individually*, plus *Together* (default), *By manufacturer* and *By plug-in*, and per-plugin overrides ([user guide](https://bitwig.com/userguide/latest/vst_plug-in_handling_and_options), [overview](https://www.bitwig.com/en/19/plugin-hosting-video.html)) | A crashed plugin's panel shows a notice; per-plugin processes are described as the safest and most memory-hungry. |
| REAPER | Per plugin *Run as*: native, a shared separate process, or a dedicated process ([REAPER blog](https://reaper.blog/2012/02/run-plugin-as-dedicated-process/)) | A dedicated process limits a crash to that plugin. Community reports, not the manual, say it adds no latency. |
| Carla | Plugin bridges in separate processes, attached through shared-memory audio and control regions ([commit](https://git.kx.studio/falkTX/Carla/commit/6970cb280edae5e55c46968ac5b2c3d11bcbcdce.diff)) | Listed as experimental in its README; the per-block exchange was not reviewed here. |
| Zynthian | One `jalv` process per LV2 plugin, connected through JACK | Isolation follows from the process layout; synchronisation is per JACK period. |

Every option trades memory and a little CPU for crash containment. Grouping several plugins in
one worker is the common middle ground.

## Design

### Workers and groups

A **worker** is a process that hosts one or more plugin instances through the existing CLAP or
LV2 host code, behind a small shim. Which plugins share a worker is a policy:

- **In engine:** first-party plugins that pass the validator, and only when the musician allows.
- **One plugin per worker:** the default for third-party plugins.
- **Shared by engine:** all instances of one native engine with sound or effect contexts run
  together, since they share decoded audio, voices and context lifetimes (RFC 0005, 0007).
- Overrides per plugin and per Sound, as in Bitwig and REAPER.

### Audio hand-off

Each worker shares a block of memory with the engine: input and output audio, the block's
events in time order, transport, parameter changes and the plugin's outgoing events. Per audio
block the engine's graph pass:

1. writes a worker node's inputs and events and wakes the worker (a futex on Linux,
   `os_sync_wait_on_address` or semaphores on macOS);
2. the worker's real-time thread runs the plugin's `process()` and signals completion;
3. the engine waits, bounded by that node's share of the block's deadline, then reads outputs
   and events and continues the graph.

This keeps same-block, sample-accurate routing with no added latency, at the cost of one wake
and one completion per worker per block. Independent graph branches in different workers can
run in parallel on separate cores. A one-block-delay mode, where a worker computes the next
block while the engine uses the previous one, is possible for nodes whose latency the graph
can compensate, but it is not the default.

### Failure handling

- **Deadline miss:** the node's output is silence for that block and the miss is counted.
  Repeated misses within a window mark the worker hung.
- **Crash or hang:** detected by process exit, a closed socket or the hang rule. The node is
  muted at the next block boundary, routes release its notes (`NOTE_END` to sources, as in
  route retirement today), and the supervisor kills and restarts the worker off the audio path.
- **Recovery record:** kept by the engine, not the worker: plugin identity and version, the last
  saved state (refreshed off the audio path after parameter or sound changes, debounced),
  parameter values, prepared sound contexts and routing. A restarted worker restores it,
  reconnects at a block boundary and fades in. Retries are bounded, after which the node stays
  muted and the Performance reports it.

### Control and host extensions

Main-thread calls (parameters, state, sound contexts, presets, asset resolution, timers, file
descriptors, logging, GUIs) cross the boundary over a local socket using the existing control
encoding. The shim implements CLAP host extensions inside the worker and forwards what needs
the engine, such as `oscairn.assets` resolution against the engine's store. Event-based
extensions (`oscairn.param-target`, note expressions) travel in the shared event list
unchanged. Plugin GUIs open in the worker's own window first; embedding comes later.

### Limits and sandboxing

Workers start with memory limits (`RLIMIT_AS`, cgroups v2 on the device), CPU accounting and
real-time priority for their audio thread. File-system and network restrictions (Landlock and
seccomp on Linux, sandbox profiles on macOS) follow, granting asset libraries read-only and the
plugin's own data folder read-write (RFC 0006, 0009).

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Measure | A minimal hand-off: one CLAP plugin in a child process, shared memory and futex or semaphore wake-up | Round-trip time per block (median and 99.9th percentile) for 1, 8 and 32 workers on macOS, Linux x86 and the aarch64 target. Decides default grouping and whether parallel branches are needed. |
| 2. One isolated plugin | Worker shim hosting a CLAP plugin as a graph node, with sample-accurate events, transport and parameters | Renders identical to in-process for the reference plugins; no allocation on the audio path; the existing graph tests pass with workers. |
| 3. Contain failure | Crash, hang and deadline handling | The `process_crash`, `process_hang` and `slow` fixtures stop only their own node; other sounds continue without dropouts. |
| 4. Recover | Supervisor, recovery record, restart and fade-in | A crashed Basic Synth and Sampler return with their prepared sounds; bounded retries. |
| 5. Policies | Grouping, native contexts in one worker, per-plugin overrides | Contexts, effect tails and sound assets work unchanged across the boundary. |
| 6. Limits | Memory and CPU limits, then sandboxing | A plugin exceeding its limit is stopped and reported; others continue. |
| 7. LV2 | LV2 plugins and their Worker extension inside workers | Upstream LV2 tests pass in workers. |

## Step 1 measurements

[`oscairn-handoff-bench`](../../tools/worker-bench/handoff_bench.cpp) forks N workers, hands each
a stereo block through shared memory and waits for it (futex on Linux,
`os_sync_wait_on_address` on macOS), either one after another (a chain) or all at once
(parallel branches). Each worker spends 50 µs of busy work or runs the Basic Synth. *Overhead*
is the block's wall time minus the workers' own processing time.

**macOS, Apple M3 Max, 2026-10-30**, real-time scheduling for engine and workers, 50,000 blocks
per row (20,000 for the Basic Synth), on a loaded desktop (load average 23–30):

| Block | Workers | Chain overhead median / p99.9 (µs) | Parallel overhead median / p99.9 (µs) |
| --- | --- | --- | --- |
| 128 frames (2,667 µs) | 1 | 34 / 87 | 34 / 89 |
| | 2 | 53 / 136 | 34 / 91 |
| | 4 | 91 / 212 | 37 / 157 |
| | 8 | 175 / 365 | 116 / 203 |
| | 16 | 333 / 633 | 264 / 355 |
| | 32 | 263 / 465 | 497 / 628 |
| 64 frames (1,333 µs) | 1 / 8 / 32 | 30 / 81, 147 / 333, 217 / 328 | 31 / 81, 105 / 185, 489 / 589 |
| 256 frames (5,333 µs) | 1 / 8 / 32 | 38 / 96, 189 / 398, 622 / 1,269 | 37 / 97, 124 / 212, 522 / 665 |
| 128, Basic Synth | 1 / 8 / 32 | 30 / 78, 159 / 342, 581 / 1,120 | 31 / 75, 83 / 170, 257 / 523 |

- **No missed deadlines** except where the workers' own work alone exceeded the block: at 64
  frames, 32 chained workers need 1,600 µs of work per 1,333 µs block (every block missed), and
  16 chained missed 48 of 50,000.
- **Each chained hand-off costs about 20 µs median and 40 µs at p99.9.** A real plugin costs the
  same as busy work. Parallel branches are almost free up to 4 workers.
- **Real-time scheduling is required.** At normal priority, 128 frames, p99.9 overhead is 0.9–13 ms
  and 16–1,159 of 20,000 blocks are missed in every configuration.
- Spinning up to 20 µs before sleeping did not lower overhead.
- **Linux is not measured yet.** Docker's linuxkit VM on the same Mac gave about 80 µs median and
  multi-millisecond tails with or without real-time priority, which reflects the VM's scheduling
  rather than Linux. Native Linux x86 and the aarch64 target are still needed to close this step.

**Recommended defaults, pending the target:** keep one worker per third-party plugin and
first-party plugins in the engine. Up to 8 workers in one chain cost under 15% of a 128-frame
block at p99.9; beyond that, group a chain's plugins in one worker. Run independent branches in
parallel: with 8 workers it cuts the p99.9 block time from 766 µs to 253 µs here.

## Step 2 status

Implemented 2026-10-30 in `core/plugin-host` (`Worker.*`, `WorkerHost.cpp`) and the
`oscairn-plugin-worker` executable:

- `loadPluginInWorker` starts the worker with `posix_spawn`, loads the plugin there through
  the ordinary `loadPlugin`, and returns a `PluginHost` the graph uses like any other.
  `loadPlugin` does this for every CLAP plugin when `OSCAIRN_PLUGIN_WORKER` names the
  worker executable. Nothing enables it by default yet.
- **Audio path:** one shared-memory block per worker (created with `shm_open`, unlinked at
  once and passed over the socket), holding audio, the block's events, transport, output
  events, statistics and parameter values. The engine wakes the worker with
  `os_sync_wait_on_address` or a futex and waits for completion. The shared buffer holds
  8,192 frames, so the worker's host splits larger blocks exactly as an in-engine host
  does; larger still are split by the engine.
- **Control path:** a socket carries initialisation, parameter changes, gestures, parameter
  text, state save and load, and `service`. The plugin is serviced only when the engine
  calls `service`, as in the engine; the worker refreshes parameter values every 10 ms.
- **Stayed in the engine:** plugins with native sound or effect contexts (Basic Synth,
  Sampler, Delay), until step 5 moved them into workers. Presets and properties are not
  forwarded yet.
- **Deadline:** see step 3.

Verification: `worker-host` renders 300 blocks of each test plugin in the engine and in a
worker and finds identical audio and output events, parameter values and text, and saved
state. The graph tests `param-target`, `chain-mono`, `audio-routing`, `graph-lifecycle`,
`note-routing`, `prepared-instances` and `midi-effects` run again as `*-workers` with every
eligible plugin in a worker (6 to 127 workers per test), including their checks that
rendering does not allocate. They pass 20 times in a row, and under ThreadSanitizer and
AddressSanitizer on macOS. Not yet run on Linux or the device.

## Step 3 status

Implemented 2026-10-30 in `WorkerHost.cpp`:

- **Budget:** each block may take half the block's time (a 128-frame block at 48 kHz gets
  1.33 ms of its 2.67 ms). An unanswered block is silent and counted as
  a deadline miss; while the worker is still busy with it, later blocks are silent at once
  without waiting. `OSCAIRN_WORKER_BUDGET_US` overrides the budget (the identical-render
  tests use 50 ms because they render faster than real time on ordinary threads). Offline
  rendering waits up to 10 s instead. Shares smaller than half a block for long chains are
  part of step 5's policies.
- **Supervisor:** a thread per worker checks every 10 ms and marks the plugin failed, kills
  its worker and silences the node, until step 4 restarts it, when the worker exits or crashes, leaves a block
  unanswered for 250 ms (hung), or misses the budget, by the clock, in every block for a
  quarter second (too slow). `PluginHost::failure()` names the reason; the pooled instrument
  wrapper passes it through. Control calls to a failed worker fail at once.
- The worker's audio thread now uses the block's period for its real-time policy. With the
  larger shared buffer's period instead, macOS scheduled it behind the engine and healthy
  workers missed their budget.

Verification: `worker-failures` plays a normal instrument next to a plugin that aborts,
sleeps 20 s or takes 2 ms per 2.67 ms block, in one graph, paced in real time on a real-time
thread for one second each. On macOS (M3 Max, load average about 24) the instrument's sound
is identical to playing alone, every failure is detected and named, and no block takes
longer than the block's time: worst 1.4 ms with a failing plugin, 0.15 ms with two healthy
ones, in 10 of 10 runs. Under ThreadSanitizer and AddressSanitizer it runs without the timing
check and passes. In Docker's Linux VM all three failures are detected and named, but the
timing and identity checks fail even with two healthy plugins, as the VM's scheduling did in
step 1; native Linux and the target are still to be measured.

Fixed race, not caused by workers: a newly loaded plugin's first `service()` (its init
callback and parameter flush) held the plugin's gate, and a first block rendered at that moment
was refused as a control silence. `prepared-instances` failed about once in 30 runs. Plugins are
now serviced once when loaded, and prepared instances again before they are published:
0 failures in 200 runs, with and without workers, and 0 in 60 under AddressSanitizer.

## Step 4 status

Implemented 2026-10-30 in `WorkerHost.cpp` (native engines followed in step 5):

- **Restart:** the supervisor thread starts a new worker for the same plugin, off the audio and
  main threads. The engine keeps the shared block, so the new worker reuses it; the plugin must
  describe itself exactly as before (descriptor, parameters, ports), or the restart fails. The
  node is silent meanwhile; control calls fail at once and `service()` does nothing.
- **Recovery record:** the plugin's state, saved by the engine into a private folder each time
  state is loaded (sounds prepared from state, Performances restored from state), then the
  parameter values the old worker last published in the shared block. Each prepared sound is a
  plugin of its own, so each one comes back with its own record.
- **Fade-in:** the restarted plugin's output fades in over 10 ms.
- **Bounded:** at most 3 restarts a minute; after that the node stays silent and `failure()`
  says so ("…; not restarted: 3 restarts within a minute"). `Statistics::restarts` counts
  restarts, so a crash that recovers is still visible.
- In the pooled instrument, one prepared sound's failure now resets only that sound; the graph
  no longer resets the whole node, so the other sounds' notes keep playing. A panic requested
  while a worker is down no longer reaches the new worker, where it would have dropped the
  first block's notes.
- Not yet: state the plugin changes by itself (its own editor, internal edits) is not saved
  into the record between loads; only parameter values are. A debounced save after changes
  is the next refinement (see open questions).

Verification: `worker-recovery` crashes one prepared sound of three, one prepared from
parameters and one from saved state. Each comes back with its value and fades in, while the
others play on unchanged. A plugin that crashes on every block is given up after 3 restarts.
`worker-failures` now runs each crashing, hanging and slow fixture through its restarts until
it is given up, with the normal instrument identical to playing alone and no overruns. On
macOS: `worker-recovery` passes 200 of 200 runs, `worker-failures` 10 of 10, and both pass 15
of 15 under AddressSanitizer and ThreadSanitizer. Not yet run on Linux or the device.

## Step 5 status

Implemented 2026-10-30 in `WorkerHost.cpp`, `Worker.cpp` and `plugin_host.cpp`:

- **Native engines in workers:** every CLAP plugin can now run in a worker, Basic Synth,
  Sampler and Delay included. A worker hosts one plugin and the effect contexts created from
  it, which share its engine as in the engine (the "shared by engine" group). Each is a graph
  node with its own shared block and audio thread in the worker, and every request names its
  node. Destroying the originating node keeps its contexts' engine alive, as the extension
  requires.
- **Across the boundary:** sound contexts (prepare, select, discard, telemetry), effect
  resources and tails, snapshots, asset dependencies, and sound assets: the plugin resolves
  a file through the engine's asset store with a request back over the socket, answered
  before the reply. Errors keep their kind (`std::invalid_argument` stays one).
- **Recovery for native engines:** besides state, the record holds the sounds prepared and
  discarded since and each channel's selection, or a snapshot saved when one is loaded. A
  restarted worker re-creates the effect contexts from a fresh engine and replays the
  record. The engine keeps its own sound IDs, mapped to the worker's, so IDs stay the same
  across a restart; new ones are never reused. Sampler sounds re-resolve their files from
  the supervisor thread, which the store's own lock allows.
- **Parameter values** a plugin reports while processing (the sequencer's active pattern)
  and values set from the engine are published with the block or the request, so they read
  back at once as in the engine. `kick::clap::Host::parameterReports()` counts the reports,
  so a block republishes values only when there are new ones.
- **Placement policy:** `setPlacementPolicy` chooses the worker executable, the standard
  placement (engine or worker) and overrides per plugin ID. It starts from
  `OSCAIRN_PLUGIN_WORKER` as before; the desktop app does not set it, so plugins still run
  in the engine there. LV2 plugins stay in the engine until step 7.
- Not yet: grouping several third-party plugins into one worker (the default stays one per
  worker, pending the target measurements), overrides per Sound rather than per plugin, and
  one audio thread serving all of a worker's nodes instead of one thread per effect context.

Verification: the graph, pressure, sound-context, sampler and effect-context tests, and the
control JSON-RPC, native sounds and effects and Performance save/restore tests, run again
with every plugin in a worker (`*-workers`); a run with a missing worker program fails each
of them, so they do use workers. `worker-recovery` kills the workers of Basic Synth (two
sounds on two channels), the Sampler (a sound from an asset library) and Delay (a root and a
context): each comes back with the same sound IDs and plays exactly as a fresh reference, and
a placement override starts a worker or not as set. On macOS all 75 tests pass, the worker
tests 10 of 10 times; under AddressSanitizer and ThreadSanitizer the suites pass. With four
worker tests in parallel under ThreadSanitizer, `prepared-instances-workers` fell short of its
20 preparations in 5 s in 2 of 5 runs, on a machine at load 30; alone it passed 10 of 10. Not
yet run on Linux or the device.

## Open questions

- Default grouping on the device, which depends on step 1's measurements and memory per worker.
- How often to refresh recovery state for plugins whose state is expensive to save.
- How plugin GUIs are shown on the device's screen from a worker.

## Ruled out

- **JACK as the isolation layer:** it synchronises separate programs per audio period, losing
  sample-accurate routing, and requires a JACK server on every platform.
- **A process per block or per note:** start-up costs far exceed an audio block.
- **One-block latency as the default:** acceptable only where the musician or graph chooses it.

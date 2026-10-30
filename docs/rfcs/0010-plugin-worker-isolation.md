# RFC 0010: Plugin worker isolation

Status: proposed, 2026-10-30. Step 1 measured on macOS only ([results](#step-1-measurements)); nothing else implemented. Moved ahead of Phase 4 hardware work and further
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

## Open questions

- Default grouping on the device, which depends on step 1's measurements and memory per worker.
- How often to refresh recovery state for plugins whose state is expensive to save.
- How plugin GUIs are shown on the device's screen from a worker.

## Ruled out

- **JACK as the isolation layer:** it synchronises separate programs per audio period, losing
  sample-accurate routing, and requires a JACK server on every platform.
- **A process per block or per note:** start-up costs far exceed an audio block.
- **One-block latency as the default:** acceptable only where the musician or graph chooses it.

# RFC 0034: Parallel graph rendering

Status: in progress, 2026-11-05: steps 1 and 2 built ([step 1](#step-1-status), [step 2](#step-2-status)). Builds on plugin worker isolation
([RFC 0010](0010-plugin-worker-isolation.md#audio-hand-off)) and resource budgets
([RFC 0025](0025-resource-budgets.md)).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. Waves: independent nodes start together; plugins in workers render at the same time ([status](#step-1-status))
- [x] 2. Audio helpers: plugins in the engine's process render at the same time, on real-time helper threads ([status](#step-2-status))
- [ ] 3. Measured on the target (a four-core KICK-class board): block time and idle CPU

## Problem

`Graph::process` runs the plan's steps one after another. For a plugin in a worker, the step
wakes the worker and waits for its reply before the next step starts, so only one plugin renders
at a time although each worker is a process the OS could run on another core. Measured on
"Drifting" (four plugins, four workers), a block cost about the sum of the workers' times, around
1.4 ms of a 2.67 ms block. A Performance whose plugins together exceed one core cannot play at
all, though the board has four.

## Outcome

Parts of the graph that do not depend on each other render at the same time, so a block costs
about its critical path (its slowest chain), not the sum of its plugins. The output is sample for
sample what serial rendering gives, and every note, event and parameter route between nodes
behaves exactly as before. The audio thread still never allocates, locks or blocks on anything
but the plugins it waits for.

## Design

### Waves

The plan is compiled in waves. A node's wave is one past the latest wave of its sources (audio,
note and parameter-target connections), and one past that of any earlier node sharing its engine
(effect contexts of one plugin, [RFC 0007](0007-effect-contexts.md), share its state, so they
never render at the same time; neither does one host in two nodes). The plan's steps are ordered
by wave, then by the dependency order as before (lowest node ID first among ready nodes).

Each step is split in three:

1. **prepare**, on the audio thread: its audio inputs mixed, and its notes merged from every
   source (note routes with their zones, ledgers and new note IDs, releases, the beat, the part's
   harmony, parameter-target events) into the step's own event buffer;
2. **render**: the plugin's `process()`;
3. **finish**, on the audio thread: the listener told what the node got and sent, a failed block's
   panic, meters, activity (for retiring nodes and voice counting), note ends back to the routes,
   and its parameter-target lanes turned into events for later steps.

A block runs wave by wave: every step of the wave is prepared in plan order, each plugin in a
worker started as soon as it is prepared; the rest render (step 2: on the audio helpers too);
then the workers are waited for and every step finishes in plan order. Taps and the outputs mix
after the last wave, as before.

**Why it is exact.** Only rendering moves; everything that touches state shared between steps
(the graph's note-ID counter, the note routes, the listener, parameter-target buffers, event-drop
counts) happens on the audio thread in the same order serial rendering uses. A step's prepare
reads only what earlier waves finished; its finish writes only its own routes and later waves'
inputs. Serial rendering (`Graph::setParallel(false)`, or `OSCAIRN_GRAPH_SERIAL=1` for every
graph) runs the same plan one step at a time: prepare, render, finish. The `parallel-render`
test renders a graph with a note effect, three synths, a delay and its effect context both ways,
in the engine and in workers, and compares every sample and every node's events in and out.

### Workers

`PluginHost` gains `startProcess` and `finishProcess`, `process()` in two halves; the default
starts nothing, and the graph calls `process()`. A plugin in a worker writes its inputs, events
and transport into the shared block and wakes the worker at the start, and waits and takes the
outputs at the finish. Its budget counts from the start, so the time the audio thread spent on
other steps meanwhile is the worker's too; a block the worker finished before the audio thread
came for it is never late. A block larger than the shared buffer (split into chunks) is not
started; `process()` renders it as before. A restart still waits for the block: the host is busy
from start to finish.

### Audio helpers

Plugins in the engine's process (LV2, and CLAP plugins the placement policy keeps in the engine)
render in a wave on a small pool of helper threads, one per core but one, at most three, shared by
every graph in the process. They run at real-time priority, as workers' audio threads do
(`makeRealtime`). Hand-off is lock-free and allocates nothing:

- the audio thread publishes the wave (a job function and its count) and wakes as many helpers as
  there are jobs beyond its own; it then takes jobs itself;
- jobs are claimed with a compare-and-swap on one 64-bit word holding the dispatch's generation
  and the next job, so a helper that wakes late can never take a job of a later dispatch;
- the audio thread waits for the last job to finish: briefly spinning, then on a futex (`os_sync`
  on macOS) the helper finishing the last job wakes;
- helpers sleep on a futex between dispatches. They never spin: on a four-core board the CPU an
  idle spinning helper burns matters more than the few microseconds a wake costs, and the audio
  thread takes any job a helper has not woken for yet, so a slow wake only costs parallelism.

One graph uses the pool at a time; another graph's audio thread that finds it busy renders its
own wave alone (try-acquire, never waiting). A wave with fewer than two plugins in the engine
wakes no helper, and neither does one whose plugins, by the time each took in the last block,
would move less than 30 us off the audio thread besides the largest (`OSCAIRN_HELPERS_WORTH_US`):
dispatching every wave made light Performances 5–20 us a block slower, as waking cost more than
their waves took. Helpers run
each job in the dispatching thread's floating-point environment (rounding, flushing denormals),
so where a plugin renders never changes what it renders. `OSCAIRN_AUDIO_HELPERS` sets how many
helpers there are (0: none).

## Step 1 status

Built: waves, the three-part step, `startProcess`/`finishProcess` for plugins in workers,
`Graph::setParallel` and `OSCAIRN_GRAPH_SERIAL`. The `parallel-render` test passes in the engine
and in workers (`parallel-render-workers`); it fails when the steps of a wave share one event
buffer (in the engine) and when the plan is run as a single wave (both). The existing tests
pass with parallel rendering on, the default (control-controller-input's latency check fails
intermittently under load, here as on `main`).

`oscairn-graph-bench` (tools/graph-bench) measures a block's time at the real block rate on a
real-time thread. Measured on an M-series Mac (16 cores) under heavy unrelated load, 128 frames
at 48 kHz (a block is 2667 us), N parts each a Basic Synth holding a 6-note chord into its own
Delay, every plugin in its own worker (2N workers, two waves); mean block time:

| Parts | Serial | Parallel |
| ----- | ------ | -------- |
| 1     | 65 us  | 64 us    |
| 2     | 122 us | 69 us    |
| 4     | 223 us | 82 us    |
| 8     | 459 us | 135 us   |

With 24 voices a part, four parts took 266 us serially and 97 us in parallel. These plugins are
light, so this mostly measures the hand-off: serially each worker's wake and reply add up; in
parallel a wave costs about one. The user's converted Performances load headless through a
`Session`, but their sfizz parts render silence when in workers there (on `main` too), so they
are measured in the engine's process under step 2.

## Step 2 status

Built: `AudioHelpers` (core/engine/src/AudioHelpers.cpp), used by every wave's plugins in the
engine's process. `parallel-render` runs with three helpers dispatched on every wave; it fails
when the audio thread does not wait for the helpers' jobs. The suites pass (109 tests in the
modular build, 117 in the desktop build; control-controller-input failed in some runs, as on
`main`).

Measured as in step 1, plugins in the engine's process, mean block time:

| Graph | Serial | Parallel |
| ----- | ------ | -------- |
| 4 parts, Basic Synth (24 voices) into Delay | 193 us | 91 us |
| "Sick tines" (16 nodes: five sfizz parts with EQs), a chord held on every channel | 427–500 us | 190–217 us |
| "Drifting" (two sfizz parts with EQs) | 499–590 us | 401–595 us |

"Drifting" gains little: one of its two sfizz parts takes nearly the whole block, and a block
cannot be shorter than its slowest part. Performances whose parts were silent here (their
inputs did not hear the chord) took 20–40 us either way, within the noise.

**Spinning or sleeping.** With "Sick tines" playing, helpers that spin 100 us after each dispatch
before sleeping used about 10% more of a core for no consistent gain (133–155 us against
139–140 us); helpers spinning until the next block used over 200% of a core and made blocks
slower and erratic (mean 506–566 us, worst over 15 ms) as they competed with the audio thread.
Helpers sleep.

**Idle CPU.** Idle (a Performance loaded, nothing playing), with the session serviced every 5 ms
as the app does, the engine process used the same CPU on `main`, serially and in parallel, within
the noise: "Drifting" in workers 5.3%, 5.7% and 5.6% of a core (and its workers 2.8–2.9%), in
the engine's process 2.3% all three. Idle, a whole block in the engine's process took under
30 us ("Drifting" 13 us, "Sick tines" 24 us), so no wave was worth the helpers and they slept.

## Alternatives

- **Dynamic dataflow** (each node starts when its last source finishes, not when its wave does)
  shortens the critical path when chains are uneven. Waves are simpler to make exact and cover the
  common shape (parts in parallel, each a chain into the mix); revisit if measurements show waves
  wasting time on real Performances.
- **One-block delay for workers** ([RFC 0010](0010-plugin-worker-isolation.md#audio-hand-off))
  overlaps a worker with the next block, adding latency; not the default and not needed here.
- **Joining the device's audio workgroup** (macOS `os_workgroup`) would tell the scheduler the
  helpers serve the audio deadline; a later refinement on macOS.

## Open questions

- Whether helpers should join the audio device's workgroup on macOS, and how they share the
  board's cores with workers when a Performance has both.

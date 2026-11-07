# RFC 0025: Resource budgets: knowing before loading whether it fits

Status: in progress, 2026-11-01: steps 1–4, 6 and 7 built, 8 to 10 in part ([status](#status));
the memory classes' design is [below](#memory-reserved-committed-and-opportunistic). Required for
every product Oscairn runs on. Builds on the
voice budget ([RFC 0018](0018-voice-budget-and-stealing.md)), device profiles
([RFC 0024](0024-device-profiles.md)), packages' resource estimates
([RFC 0006](0006-packages-and-portability.md#platform-builds-and-hardware)) and hardware
reservations ([RFC 0004](0004-hardware-services.md#select-and-reserve-before-activation)).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. Measuring: each plugin's CPU per block (at rest and per voice) and memory, recorded on the device
- [x] 2. Estimating a Performance from those measurements and the device's budget
- [x] 3. Admission: loading, switching and editing check the estimate; refuse or warn with the reason
      (loading a Performance and adding a plugin are checked and refused with the reason; a warning instead of a refusal is not offered yet)
- [x] 4. Seamless changes counted: the outgoing Performance's plugins until they ring out
- [ ] 5. Estimates in packages and Sounds, checked at install and in the library
- [x] 6. Memory classes: reserved (OS, engine, shell, app reservations), committed and
      opportunistic; admission against what reservations leave ([design](#memory-reserved-committed-and-opportunistic))
      (an audition's set-aside Performance is not counted as committed yet)
- [x] 7. Plugins declare memory before activating (`oscairn.memory`), checked against measurement
      (a plugin holding more than it declared is counted at what it holds; its declaration is not yet distrusted at the next activation)
- [ ] 8. Apps declare reservations in their manifests; running apps and the always-available set
      reserved; the shell admitting or refusing a launch, making room first
      (the engine's admission and reservations, and the desktop shell asking, are built; manifests and measuring apps are not)
- [ ] 9. Opportunistic memory and its eviction order: the sample file cache, recently used items,
      entries prepared further ahead; on demand and on pressure (PSI; macOS memory pressure)
      (built: a next entry prepared to cut, let go on demand and on Linux PSI, then a committed one under pressure; not: recent items, entries further ahead, macOS)
- [ ] 10. Linux: cgroups for the engine, workers and apps, locked memory, the out-of-memory order
      (built: a cgroup per worker with `memory.max` from its declaration and no swap, the engine in its own, locking and scores
      for the engine and workers; not: the engine's `memory.min`, apps' cgroups, opportunistic workers first, the image's wiring)
- [ ] 11. Tested: memory filled, apps launched up to the floor and a committed Performance
      loaded, with no out-of-memory kill, no running app starved and a launch past the floor
      refused with the reason (QEMU at 4 GiB, then the target)

## Problem

A Performance that needs more than the device has fails while it plays: the audio misses its
deadline, or memory runs out while a sampler loads. On a desktop that is an annoyance; on a
pedal or a stage keyboard it is the failure that matters most. Today nothing knows, before a
Performance loads, whether it fits. RFC 0018 handles too many notes, once playing, by stealing
voices; it does not stop a Performance whose plugins alone are too heavy, and seamless changes
([RFC 0021](0021-sounds-performances-and-library.md#seamless-switching)) now hold two
Performances' plugins at once for a few seconds.

## Outcome

Before anything loads, Oscairn estimates what it needs (CPU per audio block, memory, voices,
reserved hardware) against the device's budget, and refuses, or warns and asks, saying what does
not fit. Estimates come from measurements on the device, not from the plugin's word alone.

## Measuring

- **CPU:** the plugin hosts already time each process call (CLAP and LV2, in the engine and in
  workers: the last and the largest block's time). Kept per plugin and build, on this device: the
  time per block at rest and the added time per sounding voice, as a high percentile (not the
  mean), at the device's block size.
- **Memory:** what a plugin instance holds after activation and after loading its state (samples
  included), from the worker process's accounting where it runs isolated (RFC 0010), else from
  the plugin's declaration.
- **Unmeasured plugins** get the package's declared estimate (RFC 0006), else a conservative
  default, and are marked unmeasured until they have played.
- Measurements are the device's own, in the user data, never published (RFC 0006 keeps private
  inventories private).

## The budget

The device profile gives the ceiling: the cores reserved for audio, the block size's deadline,
memory. The budget keeps a margin (by reliability level, RFC 0027: more on `pass-through` than on
`desktop`) and counts:

- every plugin's rest cost, plus its voices at the part's note limit (RFC 0018), or its declared
  maximum;
- the graph's own work (routing, mixing, meters);
- during a seamless change, the outgoing Performance's plugins until they ring out;
- memory for every plugin and its samples, plus what a seamless change holds twice.

## Admission

- **Loading a Performance or a Sound, adding a plugin, importing:** checked first. Over budget:
  refused with what does not fit ("needs 140% of the audio CPU: the Strings part's reverb and
  three samplers"), or, where the profile allows, offered with a warning.
- **A seamless change that would not fit with both Performances:** offered as a cut instead
  (the old Performance stops at once), saying so, before it happens. This is the "a change that
  cannot be seamless says so first" left open in RFC 0021.
- **While playing:** voices over budget are stolen (RFC 0018); a plugin that runs over its
  measurement is noted and its estimate raised.
- **Library and packages:** a Sound or Performance shows whether it fits this device, from its
  plugins' estimates, before it is opened.

## Status

Built 2026-11-01 (`core/control/src/budget.cpp`; `setBudget`, `estimate`; `budget.set`,
`budget.estimate`). The app sets the engine's budget from its device profile: one audio thread's
time less the reliability level's margin (desktop 90%, instrument 75%, pass-through 60%) and 80%
of the profile's memory. Until a budget is set nothing is measured or checked, so tests and tools
never write the user's data folder.

- **Measured:** at each service tick, every plugin's last block as a share of the block's time,
  kept as a slowly falling high-water mark (about ten minutes' half-life) per plugin build (ID,
  version and the digest of its files, so a rebuild without a new version is measured again), skipping its first 32 blocks; memory every second, a worker's share of its
  process's resident memory or, in the engine's process, what the process grew by while the
  plugin loaded. Kept in `measurements.json` in the data folder, saved every 30 seconds when
  changed and when the session ends. One sample a tick can miss a rare spike between ticks.
  Each sample is also split by the notes sounding through the plugin in that block (held, or
  sustained by the pedal; the graph counts them for every plugin that takes notes): with none,
  it updates the plugin's cost **at rest**; with some, its cost **per voice** (the time above
  rest, divided by the notes), each a falling high-water mark too, and the most notes seen at
  once. A plugin that takes no notes (an effect) is all at rest. A block's time is the wall
  time of the plugin's `process` call on its audio thread (the worker's, for a plugin in one),
  so threads a plugin starts itself (an EQ's coefficient matching) are not counted, unless they
  slow that call; a worker's whole CPU time, every thread's, is reported in `session.status`
  but not budgeted.
- **Estimated:** the open Performance, or one in a folder from its document's plugin identities;
  a plugin never measured here counts at 2% of the audio time and 32 MiB, marked unmeasured. A
  measured plugin in a part with a note limit (RFC 0018) counts at rest plus its cost per voice
  for that many notes, so the estimate is the part's worst case; with no limit, or before a
  voice was measured, it counts at its busiest block seen.
- **Admitted:** a Performance over the budget is refused, and so is a plugin that would take the
  open one over it (it is unloaded again), each saying what is needed, what is allowed and the
  three heaviest plugins. A Performance that fits alone but not beside the one playing (and any
  still ringing out) opens as a cut; the desktop says so after opening it. Nor is it prepared
  ahead (RFC 0021): `preparePerformance` returns false, and the Setlist app says it will open
  when cued.

**Memory classes** (step 6, built 2026-11-01; `budget.cpp`): `Budget` takes the device's
memory, the system's share (the OS, the engine's base and the shell), the floor and the margin;
`setAlwaysAvailable`, `launchApp`, `appClosed` and `memoryState` (`memory.*` in the control
protocol) keep the apps' reservations. With them the memory ceiling for opening, adding and
preparing is the committed share, as "Admission's arithmetic" says: device less the system, the
running and always-available apps' reservations (an app counted once) and the margin. Committed
in use is the open Performance, what rings out and a next entry prepared beside it, each plugin
at the larger of its declaration and its measurement. A launch is admitted when
`reserved + its reservation + max(committed, floor) + margin` fits the device; otherwise it is
refused with what is free for apps and which running apps' closing would free enough, or, when
only the committed next entry stands in the way, offered with letting it go. An always-available
app is always admitted. Changing the always-available set is checked as a launch of the apps
added. A Performance over the share names the running apps whose closing would free enough. A
next entry that fits alone but not beside the open one is prepared anyway, as opportunistic data,
when memory not reserved by a running app nor committed allows (decided; before, it was not
prepared); the change to it is still a cut. Opportunistic data is let go when a launch, a
reservation or committed growth needs its room. The only opportunistic item built is that next
entry (order 4): the sample file cache is the kernel's page cache, and recently used items and
entries further ahead are not kept loaded yet. A refused Performance no longer lets the prepared
next entry go.

The desktop app sets the classes from a profile that gives `osMemory` and `performanceMemory`:
the system's share is `osMemory` plus the app's own resident memory when it starts (the shell,
and the engine when in-process; an engine in its own process is not counted yet), the margin 5%
of memory (the RFC's illustrative figure; the margins per level are open), the always-available
apps at its own apps' figures (illustrative, not measured, until manifests, step 8). No built-in
profile gives them yet, so nothing changes on the desktop or the built-in devices. **What is
enforced:** opening, adding and preparing Performances against the committed share, and an app
launch asked through `launchApp`. Oscairn's own apps' placeholder reservations
(`Profile::appMemory`, until their manifests declare them):

| App | ID | Reservation |
| --- | --- | --- |
| Performance | `oscairn.performance` | 150 MiB |
| Library | `oscairn.library` | 120 MiB |
| Setlist | `oscairn.setlist` | 60 MiB |
| Groovebox | `oscairn.groovebox` | 200 MiB |
| Recordings | `oscairn.recordings` | 100 MiB |

**The shell asks:** Home's tiles, the Apps menu and `--app`
ask `launchApp` with the app's placeholder reservation before opening it, and closing one on a
device's screen (the switcher's Close, another layout) or going Home from the desktop's
Performance app tells `appClosed`. Refused, a notice over the screen gives the engine's reason,
with what closing would free; when only the prepared next setlist entry is in the way, **Let
the next song go** asks again with `dropNext` and opens the app (`desktop-gaps` tests the
asking, the notice and letting the next entry go against a session with memory classes; the
shell's own calls are not driven by a test). An app's own
window closed on the desktop keeps its state, so its reservation stays. **Bookkeeping:** apps
run inside the desktop app (RFC 0022 step 1), so a reservation is counted, not enforced or
measured; per-app limits wait for apps as processes (RFC 0022 step 2).

**Linux** (step 10 in part, 2026-11-01; Linux only, guarded): with `OSCAIRN_CGROUP` naming a
delegated cgroup v2 subtree, the engine moves itself into `engine/` at its first worker and puts
each worker in `workers/<pid>` before it loads anything: `memory.max` the worker's limit,
`memory.swap.max` 0, removed when the worker is reaped. A worker whose plugins all declare their
memory is limited to the declarations (shared bytes once, each context's own) plus half again
and 64 MiB, by the supervisor on every platform and by its cgroup on Linux, replacing RFC 0010's
flat 4 GiB; one declaring nothing keeps the policy's limit. The limit, `memory.max` included, is
checked before each activation: the worker sends the engine the plugin's new declaration in band
(as it asks for a sound asset) and the plugin activates only once the engine has answered, so a
mode change through a restart the plugin asked for is never stopped at its old limit, however
long its allocation takes. Before an activation the limit is only raised; it is lowered after
the operation (a state or snapshot load, a new effect context), and then no further than what
the worker holds at that moment plus 64 MiB, so a move to a smaller mode is not stopped for
pages its allocator has not returned yet. A new or restarted worker is held to its
declarations at once; a restart the plugin asked for into a smaller mode keeps the higher limit
until the next such operation. A declaration no worker could hold (over four times the policy's
limit, which is the worker's address space on Linux, or over 1 TiB without one, or one whose
sum overflows) is not taken as the limit: the engine answers "Plugin's memory declaration is
not credible" and the plugin is not activated. **Sounds prepared after activating** (built
2026-11-07): a native sound context costs the declared `context_bytes` (`memory.h`), so each
one prepared on a running instance adds them to its declaration until the next activation's
estimate, which covers every sound the state holds; a discarded one takes them off. The engine
raises the worker's limit for a prepare before sending it, for one sound, or for every free
context for a snapshot's sounds prepared beside (the plugin alone reads how many it holds),
then refits to the sounds made; the same credibility cap applies with them counted. Before
this, the limit stayed at what activation set, and a shared Oscairn Analog instance filling
with sounds (RFC 0021 slice 3) was stopped at its 9th. With a memory room set, the CLAP host
refuses a prepare whose context bytes do not fit before calling the plugin, and a snapshot
whose prepared sounds' bytes do not, discarding them, with activation's reason ("Plugin needs
96 MiB of memory where 40 MiB is left"); the worker plays on. Not built: the session sets no
room around preparing sounds yet, and a snapshot's sounds are checked against the room only
once prepared, since only the plugin can count them. `control-worker-sound-memory` checks it
with a test build of the Basic Synth holding and declaring 16 MiB a sound, in workers: seven
sounds and a six-sound snapshot prepared where activation set the limit for one (both stopped
at 89 MB without the change), a sound and a snapshot past the room refused and the worker
alive, a plugin holding 100 MiB more per sound than it declares still stopped, and eight parts
sharing one instance. With `OSCAIRN_RELIABILITY`
`instrument` or `pass-through` (the app sets it from its profile; workers inherit it), the engine
and every worker call `mlockall(MCL_CURRENT | MCL_FUTURE)` at start (`MCL_FUTURE` only under
an unlimited `RLIMIT_MEMLOCK`; see "Locking") and set `oom_score_adj`:
the engine -999, a worker 300; what cannot be done (no `CAP_SYS_RESOURCE` to lower a score, too
low an `RLIMIT_MEMLOCK`) is printed, not fatal. With memory classes, the session arms a PSI
trigger ("some 100 ms in 1 s", or 2 s windows when refused) on the subtree's `memory.pressure`,
else `/proc/pressure/memory`, polled at each service; each firing lets go of one more item
(`memoryPressure`). Not built: the engine cgroup's `memory.min`, polling `memory.events`, apps'
cgroups (apps are in-process), +1000 for workers holding only opportunistic plugins, zram, and
the image making the subtree (RFC 0028's design note; `oscairn-session` raises
`RLIMIT_MEMLOCK`, below).

Checked in the `oscairn-ports-linux:local` container (Debian 13, arm64, Docker Desktop's Linux
VM, kernel 6.10, `--privileged`), not on the QEMU image or hardware: `control-plugin-failures`
finds a worker at instrument level at 300 with all its resident memory locked and the engine at
-999 and locked, and with a delegated subtree its worker's `memory.max` at the declaration plus
the margin; `control-parts` with a subtree raises `memory.high` pressure on itself and the PSI
trigger lets the prepared entry go (it fails with the polling taken out). On macOS the same
tests check the worker limit from the declaration: a plugin declaring 9 MiB and holding 200 more
is stopped at 77 MB while one holding what it declared plays on (RFC test 4; it fails with the
limit left at 4 GiB). In that container `worker-failures` fails on main as on this branch (block
timing in the VM), and others fail now and then under `-j 6` (`control-archives` on main;
`effect-contexts-workers` and `control-performance-workers` once on this branch, passing alone and
on the next run).

**Declared memory** (step 7, built 2026-11-01): `core/clap-ext/include/oscairn-clap/memory.h`.
The CLAP host asks `estimate` at every activation, after the state is loaded and before
`activate` (a load, a state load, a new effect context, a change of rate), in the engine or in a
worker. With a memory budget, the session gives each plugin the room the budget leaves beside
the others (adding to the open Performance: beside its plugins; a Performance being opened:
beside those of it loaded before, as its estimate is checked alone; a context: beside the open
ones), and a plugin declaring more is refused before it activates: "needs 13 MiB of memory where
12 MiB is left". An instance counts its shared, context and asset bytes, an effect context its
context and asset bytes. Admission counts the larger of the declaration and the measurement, or
the declaration alone until the plugin has been measured. Measurements are kept per plugin build
and declared size (`id@version digest#bytes`), so a plugin whose modes differ (Reverb 1 at 2.2 MB, an
EQ at 1 KB a context) is not counted at the largest seen; a Performance not open, which cannot
ask its plugins, counts each build at the most of its measured declarations. Without
`oscairn.memory`, an effect engine counts the shared and context bytes its effect contexts'
`resources` report and each further context its context bytes, instead of an instance each.
The first activation of a fresh instance, at its default state, is asked too; the Performance
document does not record declarations, so an unopened item still uses measurements (step 5).

`control-parts` checks a plugin measured while it plays and known to the next session, a plugin
and a Performance refused, a change that becomes a cut when both would not fit and rings out
when they would, and a Performance prepared ahead only when both would fit. `desktop-profile` checks the profile's budget figures. `kick-clap-host` checks the fixture's declaration
per state and rate, that it agrees with `current`, and a load and a state load refused before
activating; `control-parts` checks the default mode counted at its own measurement beside a big
mode's, a third plugin refused before activating, a saved state refused at loading when only
another mode's measurement made it look like it fits, and effect contexts counted from their
resources; `control-plugin-failures` checks the declaration and the refusal through a worker.
`control-parts` also runs the memory classes test below in small (simulated, in one process: the
fixture's memory effect declares and allocates set sizes): opportunistic memory filled with a
next entry prepared to cut and let go when an always-available app opens, every app admitted up
to the floor and the one past it refused with the reason, a committed next entry kept through
the refusal, a Performance past the share refused naming the apps to close, a committed
Performance loaded with the last ringing out, and a launch refused only for the committed next
entry offered and admitted by letting it go. `control-local-stream` checks the memory calls
through the protocol; `desktop-profile` the profile fields and their refusals.

## Memory: reserved, committed and opportunistic

Proposed 2026-11-01; steps 6 and 7 built and 8 to 10 in part ([status](#status)).

The goal: use all the memory a device has, never make an app unusable, and never run out. The
budget above is one figure, 80% of the profile's memory (`Profile::memoryBudget`), with the rest
left to whatever else runs. That wastes memory when little else runs and does not protect apps
when much does. Yamaha's hardware avoids the question with a fixed number of effect blocks, each
with its worst case provisioned. Oscairn is open-ended, so it splits memory into three classes
instead.

### The classes

- **Reserved:** the operating system, the engine's own base (its process, graph and control
  thread, without plugins), the shell, and app reservations
  ([RFC 0022](0022-apps-and-requests.md#how-it-works)) for:
  - **every running app**, in front or in the background. A sequencer left running while the
    musician edits a sound keeps its reservation, so the sound editor can never starve it;
  - **the always-available apps**, running or not: by default the Performance app
    (`oscairn.performance`) and the Library (`oscairn.library`), a setting the musician can
    change (the profile gives the default, RFC 0024). These
    always open.

  Installed apps that are not running and not in that set reserve nothing. A reservation is a
  promise, not an allocation. An always-available app that is closed has its share filled with
  opportunistic data, evicted when it opens. A running app's unused share holds only the sample
  file cache, which the kernel reclaims by itself the moment the app needs it (below).
- **Committed:** what must not go while it sounds. The open Performance (its plugins, their
  voices, decoded samples and effect contexts); the outgoing one while its tails ring out
  (step 4: until idle, usually seconds, longer while a note is held across the change, [RFC 0021](0021-sounds-performances-and-library.md#seamless-switching));
  the Performance an audition set aside
  ([RFC 0021](0021-sounds-performances-and-library.md#audition)); recording's rings and a take
  played back ([RFC 0020](0020-recording.md#step-3-status), `Recorder::committedBytes()`, counted
  since 2026-11-01); and the **prepared next entry when it fits beside the open one**. Admission checks committed memory against what the
  reservations leave.
- **Opportunistic:** everything else that fills the rest: a cache of sample files, recently used
  Performances and Sounds kept loaded, entries further ahead in the setlist, and a next entry
  that will open as a cut. Evicting it costs a slower switch, never a failure.

**Where this differs from the shape first discussed:** the prepared next entry is committed, not
opportunistic, when it fits beside the open one. Step 4 already refuses to prepare it unless both
fit, so the room it needs is inside the committed share by construction; evicting it would turn a
seamless change into a slower one for no gain. It is still evicted, last, if the estimates were
wrong and memory runs short (below). A next entry that does *not* fit beside the open one is
prepared anyway as opportunistic (decided; today it is not prepared at all), so the cut at the
cue is fast: 0.6 ms instead of 39 ms on the RFC 0021 bench.

**Decided 2026-11-01** (Jacob): reservations for running apps and an always-available set, not
for every installed app; a launch that does not fit is refused, never made room for by closing a
running app or touching committed memory; a next entry that will cut is still prepared, as
opportunistic; compressed swap (zram) for apps, never for the engine; plugins declare memory in an
extension of their own (`oscairn.memory`), not in effect contexts' `resources`; a launch refused
only because of the committed next entry offers to let that entry go; the Performance app's ID is
`oscairn.performance`. The margins per
reliability level are still open.

### Admission's arithmetic

```
reserved        = OS + engine base + shell
                  + reservations of the running apps and the always-available apps
committed share = device memory − reserved − margin
floor           = the profile's memory for Performances (performanceMemory, RFC 0024)
```

The committed share moves as apps start and stop; the floor does not.

- **The margin** keeps the kernel off its low watermark and covers measurement error: a share of
  device memory by reliability level (RFC 0027), as the CPU margin is. Its values are open, to be
  set from measurements on real hardware.
- **Open a Performance:** it alone must fit the committed share (as step 3, with a new ceiling).
  When it fits the floor but not the share, the reason names the running apps that hold the
  difference ("close the sound editor to free 250 MiB").
- **Seamless change, prepare the next entry ahead:** the open one, what still rings out and the
  next must fit together (as step 4). Otherwise the next is prepared as opportunistic, if free
  memory allows, and the change is a cut, said first.
- **Launch an app** that is not always-available: admitted only if, after evicting
  opportunistic data,

  ```
  reserved + its reservation + max(committed in use, floor) + margin ≤ device memory
  ```

  Otherwise the shell refuses the launch with the reason and what closing would free. It never
  closes a running app or touches committed memory to make room. So any Performance up to the
  floor always opens, whatever runs, and every running app keeps its reservation. When the
  launch is refused only because of the prepared next entry, the shell offers to let that entry
  go: the app then opens, and the next change loads at the cue, slower but still seamless.
- **Change the always-available set:** checked as a launch of the apps added.
- **The profile** is refused when the OS, the always-available apps' declared reservations and
  the floor do not fit its memory.
- **Opportunistic data** may fill everything not committed or in use, up to the margin: the
  reservations of always-available apps that are closed included. In the unused part of a
  running app's reservation it is only the sample file cache, which the kernel reclaims at once.

**Worked example** (figures illustrative, not measured): a device with 4 GiB (4,096 MiB).

| | MiB |
| --- | --- |
| Operating system (`osMemory`) | 600 |
| Engine base and shell | 250 |
| Always-available: the Performance app 150, the Library 120 | 270 |
| **Reserved at start** | **1,120** |
| Margin (5%; the real figure is open) | 205 |
| **Committed share at start** (4,096 − 1,120 − 205) | **2,771** |
| Floor for Performances (`performanceMemory`) | 2,000 |
| Performance A, playing: a 600 MiB piano and a 250 MiB string sampler, two synths, three Reverb 2 contexts at 14.5 MB each, delays, eight workers | 1,050 |
| Performance B, the next entry, prepared ahead | 900 |
| A and B together, against 2,771 | 1,950: fits, so B is prepared and committed |

Memory in use is then 600 + 250 + 1,950, plus what the two apps use, say 100: 2,900 MiB. The
1,196 MiB left, less the margin, is 991 MiB of opportunistic data.

The musician launches apps. Each is checked as reserved + its reservation + max(1,950, 2,000) +
205 ≤ 4,096, evicting opportunistic data as needed:

| Launch | Check | Result | Committed share after |
| --- | --- | --- | --- |
| Setlist, 60 | 1,120 + 60 + 2,000 + 205 = 3,385 | opens | 2,711 |
| Groovebox, 200 | 1,180 + 200 + 2,000 + 205 = 3,585 | opens | 2,511 |
| Sound editor, 250 | 1,380 + 250 + 2,000 + 205 = 3,835 | opens | 2,261 |
| A third-party app, 400 | 1,630 + 400 + 2,000 + 205 = 4,235 | refused: "needs 400 MiB, 261 free for apps; close the Groovebox (200) or the sound editor (250)" | 2,261 |

With every running app using its whole reservation, the total is 600 + 250 + 270 + 60 + 200 +
250 + 1,950 = 3,580 MiB, 311 below the margin. Nothing was killed and nothing committed went.
The Groovebox, in the background, still has its 200 MiB while the sound editor works.

A third entry C of 1,500 MiB fits alone (1,500 ≤ 2,261) but not beside A (2,550): it is
prepared as opportunistic, if the free memory allows it, and its change is a cut, as today.
An entry of 2,600 MiB does not fit the share while those apps run: it is refused when opened,
saying that closing the Groovebox and the sound editor would free enough (2,711). With only the
always-available apps running it would open (2,600 ≤ 2,771).

### The order of eviction

When an app launches, a committed load needs room, or memory runs short, the engine frees
opportunistic data, least likely needed first:

1. **The sample file cache.** Kept in the kernel's page cache (files read, not copied into the
   heap), so the kernel reclaims it by itself and nothing has to ask.
2. **Recently used Performances and Sounds** kept loaded, least recently used first. This is the
   roadmap's "keeping plugins loaded". Proposed 2026-11-05 as plugin instances kept warm once
   the outgoing Performance has rung out, out of the graph, for the next change to take over
   ([RFC 0021](0021-sounds-performances-and-library.md#free-instances-across-a-change-and-instances-kept-warm)):
   counted as opportunistic, let go least recently used first when a load or a launch needs the
   room, and first of all on memory pressure. A change that cuts for want of room keeps none. A
   seamless change keeps the outgoing Performance's free instances rather than taking them over
   while they fit the room this class leaves once the change has happened, the incoming
   Performance counted at its estimate, and the 32-instance bound (RFC 0021's middle rule).
3. **Setlist entries prepared beyond the next one**, furthest ahead first.
4. **A next entry prepared only as opportunistic** (it will cut anyway).
5. Only under pressure, when the estimates were wrong: **the committed next entry.** The Setlist
   shows it as no longer ready and prepares it again when room returns.

Never evicted: the open Performance, tails still ringing, and an audition's set-aside
Performance. These are committed and admission made room for them. A tail is not cut to make
room.

Launching an app does not wait on the kernel. The shell asks the engine to admit the app's
reservation before it starts the app (a proposed control API call). The engine checks the launch
(above), frees whole opportunistic items until the room is there, and answers yes, or no with the
reason, which the shell shows. Freeing a prepared Performance destroys its
plugins, or stops their worker processes, on the control thread; that is not timed here.

### Locking

At `instrument` and above (RFC 0027 already asks for locked memory), the engine and every worker
call `mlockall(MCL_CURRENT | MCL_FUTURE)` at start. Each allocation is then resident and
faulted in when made, during preparation off the audio thread, never during a block. Code pages
stay resident too, so a sampler or a reverb never takes a page fault from disk while playing.

- Everything an Oscairn process allocates is locked, opportunistic data included. That is
  deliberate: opportunistic data is evicted by freeing it, never by paging. That is why the
  engine evicts it before the kernel needs to.
- `MCL_FUTURE` is set only when `RLIMIT_MEMLOCK` is unlimited. Under a finite limit it does
  not fail at `mlockall`; every later mapping past the limit fails, so a plugin load or a
  recorder ring would throw `std::bad_alloc` instead of printing a warning. With a finite
  limit the process locks what it has and reports "future memory not locked: RLIMIT_MEMLOCK
  is N".
- The sample file cache is page cache, not mapped by a locked process, so it stays reclaimable.
- The image raises `RLIMIT_MEMLOCK` to unlimited for the engine and workers: `oscairn-session`
  sets it before starting the app, and they inherit it (RFC 0028). Desktop Linux usually
  has a small default, and the `desktop` level does not lock.
- A future streaming sampler (RFC 0008 step 3) locks its stream buffers and leaves the rest of a
  library in the page cache, opportunistic.

### Linux

The image uses BusyBox init (os/README.md), not systemd, so the session (`oscairn-session`)
creates one cgroup v2 subtree, delegates it to the engine, and the engine manages it:

| Cgroup | Settings |
| --- | --- |
| `engine` | `memory.min` = engine base + committed, so the kernel never reclaims it; no swap (`memory.swap.max` 0) |
| `workers/<plugin>` | `memory.max` from the plugin's estimate plus a margin, replacing RFC 0010's flat 4 GiB; no swap |
| `apps/<app>` | `memory.min` = its reservation while it runs; `memory.max` = its declared maximum; swap to zram allowed |
| `shell` | `memory.low` = its share |

- **Out-of-memory order** (`oom_score_adj`): if it ever comes to the kernel's killer, the order
  matches the eviction order. First workers that hold only opportunistic plugins (+1000: a
  slower switch), then background apps, the app in front, workers of committed plugins (RFC 0010
  restarts them), the shell (the watchdog restarts it, sound goes on), and the engine last
  (-999, not -1000). The engine stays killable in case it is the leak; the watchdog then
  restarts it (RFC 0027).
- **Pressure:** the engine sets PSI triggers on its subtree's `memory.pressure` (for example
  "some 100 ms in 1 s") and polls `memory.events` for `high`. Either evicts one more
  opportunistic item. They catch an app growing past its reservation into free memory.
- **Apps run in-process today** (RFC 0022 step 1: the Library, Setlist and Groovebox are inside
  the desktop app). Until they are separate processes (RFC 0022 step 2), their reservations
  are counted but not enforced per app.
- **Compressed swap (zram) for apps only** (decided): an app's cold pages may be compressed, so a
  background app costs less than its reservation in practice. Admission still counts every
  reservation in full, since the ratio is not known. The engine and workers never swap: their
  cgroups allow none, and their memory is locked anyway.
- **Overcommit stays heuristic.** Strict overcommit (`vm.overcommit_memory=2`) would count
  address space that workers reserve far beyond what they use: RFC 0010 step 6 saw 1.1 GB
  reserved with 6 MB resident. Locking gives committed memory the same guarantee: it is touched
  when allocated.

### macOS and the desktop

- On the desktop, the computer's other programs are the operating system's share. There are no
  app reservations beyond Oscairn's own apps, and the committed share stays a fraction of
  physical memory, as today.
- macOS lets an unprivileged process set no memory limit of its own (RFC 0010 step 6: `RLIMIT_AS`
  and `RLIMIT_DATA` are refused), so worker limits stay the supervisor's (RFC 0010).
- macOS compresses and swaps instead of killing, so running short means slowness, not an
  out-of-memory kill. The engine evicts opportunistic data on the system's memory pressure
  notifications (`DISPATCH_SOURCE_TYPE_MEMORYPRESSURE`, warning and critical) and fills it only
  up to the memory the system reports as available. Not checked here: whether the engine sees
  these notifications as a background process.
- No locking at the `desktop` level. `mlock` on macOS is limited by the system's wired-memory
  limits, and `mlockall` is reported missing (both not checked). Oscairn on a Mac is
  `desktop`, so this costs nothing the level promised.

### Declarations

**Apps** declare memory in their manifest (RFC 0022, read at install): `reserve`, what the app
needs to open and do its job, and `max`, beyond which it is closed. As with plugins, the device
measures each app's peak. An app that goes past its reservation is noted, and its reservation
raised to the measurement at the next start. First-party apps are measured on each profile
before release.

**Plugins** declare what they will allocate before they allocate it. Measurement (step 1)
learns a plugin's memory only after it has loaded. That is too late on a device near its limit:
one Reverb 2 context takes 14.5 MB when prepared, and the figure depends on the sample rate and
mode. A new draft extension in `core/clap-ext`, `oscairn.memory`, beside
`oscairn.effect-contexts`:

```c
static const char OSCAIRN_EXT_MEMORY[] = "oscairn.memory/draft";
typedef struct oscairn_memory {
    uint64_t shared_bytes;  /* once per instance: tables, engine-wide buffers */
    uint64_t context_bytes; /* each context; the instance itself is one */
    uint64_t asset_bytes;   /* decoded assets (samples) for the current state */
} oscairn_memory_t;
typedef struct oscairn_plugin_memory {
    /* Main thread, after init and state load, before activate: what activating at this
     * rate and maximum block size would allocate. No allocation of that size. */
    bool (CLAP_ABI *estimate)(const clap_plugin_t*, double sample_rate,
                              uint32_t max_frames, oscairn_memory_t*);
    /* Main thread, after activate: what it holds now. */
    bool (CLAP_ABI *current)(const clap_plugin_t*, oscairn_memory_t*);
} oscairn_plugin_memory_t;
```

- **Why its own extension:** samplers and synths need it as much as effects do. The
  effect-contexts `resources` call (`shared_bytes`, `context_bytes`, capacity, prepared, tail)
  reports only after preparing and only for effects. It stays as it is, for capacity and tails.
  An engine offering both reports the same bytes through both.
- **Used at preparation:** the host creates the instance, loads its state, asks `estimate`, and
  activates only if the result fits. A plugin that does not fit is refused before its large
  allocation, not after. For a new effect context on a playing engine, the cost is
  `context_bytes`. The sst-effects fork can report this once its buffers are sized by sample
  rate and mode (work in progress elsewhere, not checked here).
- **Checked against measurement:** admission uses the larger of the declaration and the
  measurement (step 1 and "Ruled out" below). A plugin that holds more than it declared is noted,
  and its declaration is no longer trusted on this device.
- **Unopened items** (the library's "fits this device", step 5) cannot ask the plugin, so they use
  measurements and package estimates as now.

### Interactions

- **Seamless changes:** two Performances at once are committed, open and ringing, until the old
  one is idle. At the cue, the prepared next one becomes the open one, and preparing the entry
  after it waits until the room is there: in practice, until the old one has rung out, unless
  free memory allows it as opportunistic. The peak stays two committed Performances.
- **The voice budget (RFC 0018)** is CPU. A plugin allocates its voices when it activates, so
  memory does not grow with notes, and stealing frees none. A part's note limit lowers memory
  only where a plugin sizes its voice pool from it, which no plugin reports today. Memory is not
  checked at note-on.
- **Worker limits (RFC 0010):** a worker's limit becomes its plugins' estimate plus a margin,
  not 4 GiB. On Linux it is also the worker cgroup's `memory.max`. A worker over its limit is
  failed as today, and its measurement raised. Workers holding opportunistic plugins are the
  first the kernel would kill.
- **Effect contexts (RFC 0007):** a context reused across a change (RFC 0021 slice 1) costs
  `context_bytes`, not a new instance. Admission counts the context alone, from its declaration
  or, without one, from its effect contexts' `resources` (built, step 7).
- **Samplers (RFC 0008, 0009, 0013):** decoded samples are committed while their Performance is
  open. The Legacy Sampler's 256 MiB budget per instance (RFC 0008) stays as a bound of its own. Audio
  shared between a Legacy Sampler's contexts is counted once, in `shared_bytes`.

### What the musician sees

- **The next entry cannot be prepared beside the open one:** the Setlist shows it as "opens with
  a cut", with the reason ("needs 1,500 MiB beside the 1,050 playing; 2,411 allowed"). Today
  it says only that the entry will open when cued. It is still prepared as opportunistic when there is room.
- **A prepared entry was evicted:** its ready mark goes. Going to it loads it at the cue (39 ms on
  the bench), and it is still seamless if it fits beside the open one.
- **A Performance that does not fit at all:** refused, as now.
- **An always-available app** always opens, and a running app is never closed or starved to make
  room for another.
- **A launch that does not fit** is refused, with the reason and what closing would free ("needs
  400 MiB, 261 free for apps; close the Groovebox (200) or the sound editor (250)").
- **An app past its declared maximum** is closed by its cgroup, and the shell says "Groovebox used
  too much memory and was closed". Sound goes on.

### Tests that would prove it

On the QEMU image at 4 GiB, then on each product's hardware:

1. **Fill, then claim:** fill every free byte with opportunistic data (recent Performances, extra
   prepared entries, a fixture plugin that allocates a set size), then launch fixture apps that
   allocate their whole reservation until the next would pass the floor, and open a committed
   Performance at the committed share. Pass: no out-of-memory kill (`memory.events` `oom_kill` 0
   in every cgroup, nothing in the kernel log), every admitted launch opens, the one past the
   floor is refused with the reason, the always-available apps open, no missed audio block,
   eviction in the stated order.
2. **A background app not starved:** a fixture sequencer app in the background allocates up to
   its reservation while the app in front allocates up to its own. Pass: the sequencer's
   allocations all succeed and its cgroup shows no `high` or `oom` events. With zram on, the
   engine's and workers' swap use (`VmSwap`) stays 0.
3. **Seamless at the limit:** A and B that fit together: B stays prepared through test 1's
   claims, and the change rings out. A and C that do not: C is prepared as opportunistic, it is
   evicted first, and the change is a cut, said first.
4. **A wrong declaration:** a plugin declaring 10 MB and allocating 200 MB. Its worker is failed
   at its limit, its measurement raised, and nothing else is killed or evicted beyond
   opportunistic data.
5. **An app past its reservation** into free memory: PSI or `memory.high` evicts opportunistic
   data before any kill. Past its maximum, only that app is closed.
6. **Locking:** the engine's and workers' locked memory (`VmLck`) equals their resident memory,
   and no major page fault occurs in them while the page cache is flooded (a large file read in a
   loop) during playback.
7. **Declarations:** activating Reverb 2 at 96 kHz where only the 48 kHz figure fits is refused
   before it allocates (the process grows by less than the context), and `estimate` agrees with
   `current` after activation within a stated tolerance.
8. **macOS:** a simulated memory pressure warning evicts opportunistic data (`memory_pressure
   -S -l warn`; whether this reaches the engine is not checked).

## Open questions

- How shared accelerators and DSP backends (RFC 0004) count against the budget.
- Whether a Performance may opt into "load what fits" (parts beyond the budget left off) for
  rehearsal.
- How long measurements stay valid across plugin and Oscairn updates.
- Memory: the margin per reliability level, to be set from measurements on real hardware.

## Ruled out

- **Trusting declared estimates alone:** plugins do not know the device, and some would claim less.
- **Silently degrading quality to fit** (fewer voices, lower oversampling) without the user's
  choice (RFC 0006: resource or quality reductions need an explicit policy).
- **A fixed number of effect blocks, each with its worst case provisioned** (Yamaha's way):
  Oscairn is open-ended, and the worst case of every possible plugin would leave little memory.
- **Paging committed memory, or relying on swap for it:** a page fault from storage in an audio
  block misses its deadline.
- **Cutting a ringing tail to make room:** tails are committed; admission made room for them.

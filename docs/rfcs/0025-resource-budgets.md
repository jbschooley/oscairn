# RFC 0025: Resource budgets: knowing before loading whether it fits

Status: in progress, 2026-11-01: steps 1–4 built ([status](#status)); memory classes
proposed, steps 6–11 ([design](#memory-reserved-committed-and-opportunistic)). Required for
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
- [ ] 6. Memory classes: reserved (OS, engine, shell, app reservations), committed and
      opportunistic; admission against what reservations leave ([design](#memory-reserved-committed-and-opportunistic))
- [ ] 7. Plugins declare memory before activating (`oscairn.memory`), checked against measurement
- [ ] 8. Apps declare reservations in their manifests; running apps and the always-available set
      reserved; the shell admitting or refusing a launch, making room first
- [ ] 9. Opportunistic memory and its eviction order: the sample file cache, recently used items,
      entries prepared further ahead; on demand and on pressure (PSI; macOS memory pressure)
- [ ] 10. Linux: cgroups for the engine, workers and apps, locked memory, the out-of-memory order
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
  kept as a slowly falling high-water mark (about ten minutes' half-life) per plugin build (ID
  and version), skipping its first 32 blocks; memory every second, a worker's share of its
  process's resident memory or, in the engine's process, what the process grew by while the
  plugin loaded. Kept in `measurements.json` in the data folder, saved every 30 seconds when
  changed and when the session ends. One sample a tick can miss a rare spike between ticks.
  Each sample is also split by the notes sounding through the plugin in that block (held, or
  sustained by the pedal; the graph counts them for every plugin that takes notes): with none,
  it updates the plugin's cost **at rest**; with some, its cost **per voice** (the time above
  rest, divided by the notes), each a falling high-water mark too, and the most notes seen at
  once. A plugin that takes no notes (an effect) is all at rest.
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

`control-parts` checks a plugin measured while it plays and known to the next session, a plugin
and a Performance refused, a change that becomes a cut when both would not fit and rings out
when they would, and a Performance prepared ahead only when both would fit. `desktop-profile` checks the profile's budget figures.

## Memory: reserved, committed and opportunistic

Proposed 2026-11-01, not built (steps 6–11).

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
  - **the always-available apps**, running or not: by default the Performance app and the
    Library, a setting the musician can change (the profile gives the default, RFC 0024). These
    always open.

  Installed apps that are not running and not in that set reserve nothing. A reservation is a
  promise, not an allocation. An always-available app that is closed has its share filled with
  opportunistic data, evicted when it opens. A running app's unused share holds only the sample
  file cache, which the kernel reclaims by itself the moment the app needs it (below).
- **Committed:** what must not go while it sounds. The open Performance (its plugins, their
  voices, decoded samples and effect contexts); the outgoing one while its tails ring out
  (step 4: until idle, usually seconds, longer while a note is held across the change, [RFC 0021](0021-sounds-performances-and-library.md#seamless-switching));
  the Performance an audition set aside
  ([RFC 0021](0021-sounds-performances-and-library.md#audition)); and the **prepared next
  entry when it fits beside the open one**. Admission checks committed memory against what the
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
extension of their own (`oscairn.memory`), not in effect contexts' `resources`. The margins per
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
  floor always opens, whatever runs, and every running app keeps its reservation.
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
   roadmap's "keeping plugins loaded".
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
- The sample file cache is page cache, not mapped by a locked process, so it stays reclaimable.
- The image raises `RLIMIT_MEMLOCK` for the engine and workers (RFC 0028). Desktop Linux usually
  has a small default (not checked), and the `desktop` level does not lock.
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
  `context_bytes`, not a new instance. Admission still counts it at full instance cost today.
  With declarations it can count the context alone.
- **Samplers (RFC 0008, 0009, 0013):** decoded samples are committed while their Performance is
  open. The Sampler's 256 MiB budget per instance (RFC 0008) stays as a bound of its own. Audio
  shared between a Sampler's contexts is counted once, in `shared_bytes`.

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
- Whether a launch refused only because of the committed next entry may offer to let it go (a
  slower, still seamless change) instead.

## Ruled out

- **Trusting declared estimates alone:** plugins do not know the device, and some would claim less.
- **Silently degrading quality to fit** (fewer voices, lower oversampling) without the user's
  choice (RFC 0006: resource or quality reductions need an explicit policy).
- **A fixed number of effect blocks, each with its worst case provisioned** (Yamaha's way):
  Oscairn is open-ended, and the worst case of every possible plugin would leave little memory.
- **Paging committed memory, or relying on swap for it:** a page fault from storage in an audio
  block misses its deadline.
- **Cutting a ringing tail to make room:** tails are committed; admission made room for them.

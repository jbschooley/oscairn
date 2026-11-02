# RFC 0033: The script block

Status: proposed, 2026-11-01. Not implemented. A general MIDI script plugin in Lua, in its own
repository (MIT), with an API Oscairn defines and documents. Builds on parameter lanes
([RFC 0002](0002-param-target.md)), plugin roles ([RFC 0011](0011-performance-ui-and-editors.md#plugin-roles)),
the transport and clock out ([RFC 0012](0012-sync-external-midi-and-multi-device.md)), plugin
worker isolation ([RFC 0010](0010-plugin-worker-isolation.md)), assets, packages and item types
([RFC 0009](0009-sound-assets-and-dependencies.md), [RFC 0006](0006-packages-and-portability.md),
[RFC 0023](0023-item-types.md)), resource budgets ([RFC 0025](0025-resource-budgets.md)) and
reliability levels ([RFC 0027](0027-reliability-levels.md)). Its first partner is the
arpeggiator ([RFC 0032](0032-arpeggiator.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. The runtime: Lua in a fixed arena, host-driven GC, an instruction budget, a cut-down library
- [ ] 2. The plugin: notes in and out, sample-accurate scheduling, transport and clock ticks
- [ ] 3. Errors shown to the user, and recycling a script that keeps failing
- [ ] 4. Parameter lanes and script knobs
- [ ] 5. The part's scale and chord
- [ ] 6. The phrase message to the arpeggiator
- [ ] 7. Scripts as library items and package assets
- [ ] 8. Budgets and reliability: declared memory, a capped CPU cost, measured on the target

## Problem

Some note processing is too particular for a fixed plugin: a chord memory that follows the
song's key, a harmonizer, a strum, a pattern that changes every fourth bar, a controller that
picks the arpeggiator's phrase. Gig Performer answers this with GPScript, Logic with its
Scripter plugin, Kontakt with KSP. Oscairn has nothing; the roadmap lists scripting as
"language and API to be decided in an RFC".

Running user code inside an audio block is risky. A script can loop forever, allocate without
end, or call something slow. Jacob's global rule applies: anything running user code needs a
timeout, a memory limit, recycling and captured errors, and must not block the event loop.

## Outcome

- A **Script** plugin, a note effect like the transpose and the arpeggiator, runs a Lua script
  in a part's chain.
- It talks to other plugins only through what the graph already carries:
  - **notes out**, for example into the arpeggiator's note input;
  - **a "load phrase" message**, as CLAP MIDI SysEx on the note port, which the arpeggiator
    accepts (RFC 0032);
  - **parameter changes** on other plugins through RFC 0002 parameter lanes.
- The API is Oscairn's, versioned and documented: event callbacks, sample-accurate scheduling,
  the part's scale and chord, the phrase message and parameter lanes.
- A script cannot hang the engine, grow without bound, or touch files, the network or the OS.
  Its errors are shown to the user. A script that keeps failing is stopped and restarted, then
  given up, with no hanging notes.
- Scripts are library items and package assets.

Scope: this RFC covers MIDI scripts in the audio graph. Scripts that drive the UI, the library
or graph edits are not covered. Controller mappings keep their JavaScript, outside the audio
path ([architecture](../architecture.md#external-controllers-and-hardware)).

## Prior art

| Product | Language | How it runs | Notes |
| --- | --- | --- | --- |
| Gig Performer, GPScript | Its own language, statically typed | Callbacks for MIDI events, widgets and timers | No official rationale for a custom language was found. The usual argument is real-time safety and no garbage collection |
| Logic Pro, Scripter | JavaScript | A MIDI effect plugin: a callback per event and per block, timing info from the host, events sent at a beat or after a time, script-defined controls | The closest model: a script as a note effect in a chain |
| Kontakt, KSP | Its own language | Callbacks on note, release, controller; `wait()` for timing | Custom language, no garbage collector |
| REAPER, JSFX | EEL2, compiled | Sections run per block and per sample | Compiled to native code, no garbage collector |
| monome norns | Lua | Scripts in a separate process; audio in SuperCollider | Lua for music scripts, but not in the audio block |
| Oscairn's controller mappings | JavaScript (QuickJS proposed) | In the controller service, off the audio thread | For Mixxx mapping compatibility |

These summaries are from the products' documentation as remembered; they were not rechecked
for this RFC.

## Design

### Why Lua, and not a custom language

The case for a custom language like GPScript is real-time safety: no garbage collector pauses,
no unbounded loops, no allocation. A constrained Lua embedding meets each point, and keeps a
language people already know:

| Concern | How the embedding answers it |
| --- | --- |
| Allocation on the audio thread | Lua takes a custom allocator (`lua_newstate`). Every allocation comes from a fixed arena, preallocated before activation, with a constant-time allocator (TLSF, or similar). No system allocator is called |
| Garbage collector pauses | Automatic collection is stopped (`LUA_GCSTOP`). The plugin runs bounded incremental steps (`LUA_GCSTEP`) at the end of each block, and more as the arena fills |
| Runaway loops | A count hook (`lua_sethook`, `LUA_MASKCOUNT`) checks an instruction budget and the block's time, and stops the script with an error |
| Unbounded memory | The arena is the limit. A failed allocation is a Lua memory error, caught like any other |
| File, network and OS access | Those libraries are not opened; see "The cut-down library" |

A custom language would cost years of design, a compiler, documentation and tools, and users
would learn it for Oscairn alone. Lua is small, written in portable C, MIT licensed, and made to
be embedded. Scripts and skills carry over from other Lua hosts.

### Which Lua: 5.5, not LuaJIT

When this RFC was written, lua.org listed **Lua 5.5.1** (August 2026) as current, and
**5.4.9** as the last 5.4 release, with no more planned. The choice is between 5.5 and LuaJIT:

| | Lua 5.5 | LuaJIT 2.1 |
| --- | --- | --- |
| GC under the host's control | Incremental, stepped explicitly; 5.5 also does major collections incrementally in generational mode | Incremental (Lua 5.1 era), stepped explicitly |
| Instruction budget | The count hook runs on every instruction count | Debug hooks do not run inside JIT-compiled code, so the budget holds only with the JIT off |
| arm64 | Interpreted; builds anywhere | arm64 JIT supported; on macOS it needs writable-executable memory and an entitlement; a JIT is also more attack surface for untrusted code |
| Determinism | Same inputs, same output, steady timing | Same output; timing varies as traces compile mid-block |
| Licensing | MIT | MIT |
| Language | 64-bit integers (useful for ticks and samples), `<const>`, `<close>`, declared globals in 5.5 | Lua 5.1 with some extensions |
| Maintenance | Regular releases from PUC-Rio | Rolling releases |

**Decision: Lua 5.5, pinned to an exact release and vendored.** The budget and the timing are
the safety case, and both are weaker with a JIT. With the JIT off, LuaJIT's main advantage is
gone. MIDI scripts do little arithmetic per block, so an interpreter is fast enough; the
target's measurement (step 8) checks that. 5.5's `luaL_openselectedlibs` also makes opening a
chosen set of libraries direct.

This RFC proposes 5.5, the current release; 5.4 has reached its last release, and staying on it
is open below.

### The plugin

A CLAP note effect, `org.oscairn.script`, named "Script": one note input, one note output, no
audio ports, CLAP and MIDI dialects. It also has:

- **Parameter lanes** (RFC 0002): up to 16, named by the script, listed through
  `oscairn.param-target-lanes`.
- **Script knobs:** 8 CLAP parameters the script names and ranges (renamed through a parameter
  info rescan, which workers already forward; RFC 0010). Scenes, Performance controls and
  physical controls reach a script through them like any plugin.
- **State:** the script's source text, its asset reference once `oscairn.assets` exists, the
  knob values and the arena size. A saved Sound holds the source, so it moves with nothing else.

### The runtime

- **One Lua state per instance,** in its own arena. The arena size is in the state (default
  1 MiB, at most what the profile allows; proposed 16 MiB).
- **Loading:** on the main thread. The source is compiled as text only (binary chunks are
  refused: they can crash the VM). The top-level chunk runs once with a larger budget
  (proposed 50 ms), where the script declares its lanes, knobs and clock rate.
- **Swapping:** a new or edited script is compiled into a spare state and arena on the main
  thread, and takes over at a block boundary. If it fails to compile, the old one keeps running
  and the error is shown.
- **Each block,** on the audio thread: input events are delivered to callbacks in time order,
  due scheduled events are sent, then a GC step runs.
- **The instruction budget:** the count hook runs every 1000 instructions. It stops the script
  when the block's instruction count passes the budget, or the time used passes a share of the
  block's deadline (proposed 25%). The callback ends with an error; events already sent in that
  block stay sent.
- **C functions** are not counted by the hook. The ones offered are bounded: string functions
  work on arena-limited strings, and pattern matching has Lua's own recursion limits. Anything
  that still runs long is caught by RFC 0010's deadline rule, which fails the worker.
- **GC:** a step per block sized to the arena (proposed: work equal to 4 KiB of allocation), and
  larger steps above 50% fill. When an allocation fails, Lua may first run an emergency full
  collection (to be confirmed with GC stopped); its cost is bounded by the arena's size. Past
  that, the error is a memory error. Incremental mode by default; whether 5.5's generational
  mode, now with incremental major collections, is better is measured in step 8.
- **Randomness** is a seeded generator in the state, so offline renders repeat.

### The cut-down library

Opened: `base` (without `dofile`, `loadfile`, `load`, `require` and `collectgarbage`),
`string` (without `string.dump`), `table`, `math`, `utf8` and `coroutine`. Not opened: `io`,
`os`, `package`, `debug`. `print` goes to the captured log. `os.clock` and `os.time` are
replaced by the API's time functions. A script cannot load another script; shared code is
pasted into the script for now (open below).

### The API

Version 1, documented in the plugin's repository and frozen once published. A script declares
the version it was written for on its first line:

```lua
-- oscairn-script 1
-- name: Key chords
-- author: Jacob Schooley

knob(1, { name = "Spread", min = 0, max = 24, default = 12 })
lanes { "Cutoff" }
clock.every(1/4)            -- on_tick each sixteenth note

function on_note(e)
  if e.on then
    for _, k in ipairs(harmony.chord_in_scale(e.key, 3)) do
      note.on { key = k, velocity = e.velocity, at = e.at }
    end
  else
    note.off_all_from(e)    -- the notes this key started
  end
end

function on_tick(t)
  lane.set(1, (t.step % 16) / 15, t.at)
end
```

**Callbacks** (each optional):

| Callback | When |
| --- | --- |
| `on_note(e)` | A note on or off at the input: key, velocity (0–1), channel, note ID, `at` |
| `on_cc(e)`, `on_bend(e)`, `on_pressure(e)` | Controllers, pitch bend, channel and poly pressure |
| `on_sysex(e)` | A SysEx message at the input, as a string |
| `on_transport(t)` | The transport starts, stops, jumps, or changes tempo or meter |
| `on_tick(t)` | Each tick at the rate the script set with `clock.every(beats)`: step number, beat, bar, `at` |
| `on_knob(i, value)` | A script knob changed |
| `on_harmony(h)` | The part's scale or chord changed |
| `on_block(b)` | Every block, after the events: its length and beat range |

**Time.** `at` is the event's sample within the block. Scheduling functions take `at` (a sample
in this block), `after` (seconds or beats from now) or `beat` (a beat on the transport, or on
the free-running beat while stopped; RFC 0032). Events later than this block wait in a fixed
queue (proposed 1024 events) held by the plugin, outside Lua, and leave at their exact sample.
A full queue drops the new event and logs it. `after(beats, fn)` runs a function later, from
the same queue.

**Output:**

| Call | Sends |
| --- | --- |
| `note.on{key, velocity, channel, at}` | A note on; returns its ID |
| `note.off(id, at)`, `note.off_all_from(e)` | Note offs |
| `send.cc(n, value, at)`, `send.bend`, `send.pressure` | Controllers |
| `pass(e)` | The input event unchanged |
| `lane.set(i, value, at)`, `lane.modulate(i, amount, at)` | RFC 0002 `SET` (0–1 of the target's range) and `MODULATE` (−1 to 1) on lane `i` |
| `phrase.send(slot, text, at)` | The arpeggiator's load-phrase SysEx: `F0 7D 4F 50 48 52 SLOT text F7` |
| `phrase.write(t)` | Phrase text (`oscairn-phrase 1`) from a Lua table, for `phrase.send` |

Input events not passed are dropped: a script with no callbacks passes nothing. A template
script that passes everything is the starting point.

**Harmony:** `harmony.scale()` (root and the scale's notes), `harmony.chord()` (root, type and
keys of the chord held on the part), `harmony.in_scale(key)`, `harmony.chord_in_scale(key, n)`,
and `held()`, the keys held at the script's own input.

**Notes are tracked.** The plugin keeps every note it has sent and not ended. When a script is
stopped, swapped or recycled, or the plugin deactivates, it sends a note off for each. A script
cannot leave a note hanging.

### The part's scale and chord

Oscairn has no key or scale today. The pad editor's in-key fills have a list of scales
(`padScales()` in `core/control/src/pads.cpp`), but nothing a plugin can read. This RFC adds:

- **A key and scale per Performance,** with an override per part, saved in the Performance
  format; scenes may change it. The scales are the pad editor's list, plus a custom set of
  pitch classes. The pad editor's in-key fills default to it.
- **The chord held on a part,** recognized by the host from the notes at the part's input
  (after its zone), with a fixed chord table.
- **`oscairn.harmony`**, an Oscairn CLAP extension (MIT, in `core/clap-ext`, like
  `param-target`): plugins read the current key, scale and chord on the main thread, and receive
  an Oscairn event in the block where any of them changes, at its sample. Workers carry the event
  like `param-target` events (RFC 0010).

This is not tuning. RFC 0017's scales tune pitches; this says which notes belong to the key.

### Talking to the arpeggiator

A script before the arpeggiator in a part's chain plays it like a keyboard, through its note
input, and can send it phrases:

```text
keys ─► Script ─► Arpeggiator ─► Synth
          └─ lane 1 ─► Synth cutoff
```

`phrase.send` needs SysEx between plugins, which RFC 0032 step 1 adds to the graph and workers,
with its size limits. A phrase message over the limit is refused by `phrase.send` with an
error, never truncated.

### Errors and recycling

- **Captured:** every error (compile, runtime, budget, memory) and every `print` goes into a
  fixed ring of records in the plugin: time, kind, message and traceback, truncated to a fixed
  length. The audio thread only writes the ring; the main thread reads it.
- **Shown:** the plugin's editor shows the log (an `oscairn.ui` operation that returns data, as
  RFC 0032 also needs), and the latest error appears with the engine's plugin failures in
  `session.status` and the Parts view, as a crashed worker's does today.
- **A failing callback** costs only that call: the script keeps running.
- **Recycling:** three errors within ten seconds stop the script: its notes are ended, its
  state discarded, and a fresh state is built from the source on the main thread and swapped
  in. After three recycles in a minute (RFC 0010's worker rule), it is given up: it stays
  stopped until edited or reloaded, and the plugin then passes its input through unchanged or
  stays silent, as the instance is set (pass-through by default).
- **A crash or hang of the worker itself** is RFC 0010's: the node is silenced, the worker
  restarted with its recovery record, and the script reloaded from the state.

### Worker isolation

The script block is the clearest case for RFC 0010. Two walls stand between a script and the
engine:

1. **The Lua sandbox:** arena, budget, cut-down library.
2. **The worker:** the plugin always runs in a worker, one per instance. The in-engine
   placement that RFC 0010 allows first-party plugins is refused for it, so a bug in the
   interpreter or the plugin still cannot reach the engine. The worker's memory limit is the
   arena plus the interpreter and a margin. RFC 0010's file and network sandboxing (step 6, not
   built) then removes what the script library already leaves out.

### Library items and package assets

- **An asset:** a script is a small text file (`.lua`), immutable by SHA-256 like any asset
  (RFC 0009). An edit makes a new one.
- **An item type** `oscairn.script` (RFC 0023): storage by content, recognized by the
  extension and the `-- oscairn-script` first line. The plugin's package ships an extractor
  that reads the header's name, author, description and API version into the index.
- **In packages:** a library or plugin package may carry scripts (RFC 0006), as it carries
  samples. Scripts are data: installing one runs nothing, and needs no approval as native code
  would. They run only inside the sandbox, which grants nothing more to a script from a
  package than to one the user wrote.
- **In Sounds:** the state embeds the source, so a Sound plays where the script's library item
  is missing. A script with an unknown API version is refused with that reason.

### Budgets and reliability

- **Memory (RFC 0025):** the plugin declares its arena through `oscairn.memory` before
  activating (`shared_bytes`: the arena and the interpreter), so admission knows before
  loading. The arena never grows, so the declaration is exact.
- **CPU:** measured like any plugin. The budget caps a script's worst case at its share of the
  deadline, so an unmeasured script is estimated at that cap, not at a guess.
- **Reliability levels (RFC 0027):** the profile sets the budget's share and the largest arena.
  At `instrument` and above the defaults are tighter (proposed 15% of the deadline), and a
  given-up script passes its input through, so the part still plays. A script carries no audio,
  so on a `pass-through` product it cannot touch the audio path that level protects.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1 | The runtime | Fixtures: an endless loop is stopped within its block; a script allocating forever gets a memory error and the arena never grows; `io`, `os.execute`, `require`, `load` and binary chunks are unavailable; no system allocation on the audio thread under the render allocation guard |
| 2 | The plugin: notes, scheduling, transport, ticks | A script delaying every note by 37 samples renders exactly so at 256- and 37-sample blocks, in a worker; ticks at a sixteenth land on their samples at 133 BPM over 400 uneven blocks |
| 3 | Errors and recycling | A script erroring on every note is recycled three times then given up, with its errors in the editor's log and `session.status`, no hanging note, and the other parts playing on |
| 4 | Lanes and knobs | A script's lane mapped to a test instrument's gain moves it at the exact sample, as in RFC 0002's test; renamed knobs appear in the generated editor |
| 5 | Scale and chord | A Performance key and a part override saved and reopened; a script reading them; a chord change reaching it in the same block |
| 6 | The phrase message | A script sends a phrase to the arpeggiator, which plays it from the stated time (needs RFC 0032 steps 1 and 2) |
| 7 | Library and packages | A script imported, indexed, picked, exported in a library archive and installed on another data folder; a Sound playing with its library item deleted |
| 8 | Budgets and reliability | On the target, a busy reference script's cost at the 99.9th percentile; the arena declared and admitted; generational and incremental GC compared |

## Open questions

- Lua 5.5 (proposed) or 5.4, which has reached its last release.
- The default and largest arena, the budget's share of the deadline, and the GC step size, from
  step 8's measurements.
- Shared code between scripts (a `require` limited to scripts in the library) or not.
- Whether the host's chord recognition and the arpeggiator's (for style tracks) should be one
  shared table.
- Whether a script may keep data across a swap or recycle (a table the plugin saves), or always
  starts fresh.
- Whether scripts get MIDI 2.0 and per-note expressions in version 1, or later.
- Whether controller mappings should move from JavaScript to Lua later, for one language. Mixxx
  compatibility says no for now.

## Ruled out

- **A custom language,** as GPScript, KSP and JSFX have: years of work for a language nobody
  else uses, when a constrained Lua meets the same safety needs.
- **LuaJIT:** the instruction budget does not hold in compiled code, and the JIT adds timing
  jitter and attack surface.
- **JavaScript (QuickJS) in the audio block:** larger, its numbers are doubles (integers only through BigInt), and Lua's
  explicit allocator and stepped GC fit a fixed arena better. QuickJS stays where it is, for
  controller mappings off the audio thread.
- **WebAssembly for user scripts,** for now: users would need a compiler toolchain. It may suit
  compiled plugins later.
- **Scripts in the arpeggiator,** or in any other plugin. Jacob decided scripting is one general
  block that talks to others over notes, SysEx and lanes.
- **Running scripts off the audio thread with lookahead:** it adds latency and loses
  sample-accurate scheduling.
- **The script block in the engine process,** even as a first-party plugin.
- **File, network or OS access** for scripts, whatever their source.

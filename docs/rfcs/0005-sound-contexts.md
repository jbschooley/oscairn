# RFC 0005: Native prepared sound contexts

Status: first instrument implementation. The Basic Synth and its typed/JSON/remote
controls implement this contract. Ordinary CLAP/LV2 instruments also have a host-owned instance pool, documented in the
[control protocol](../control-protocol.md#prepared-sounds); it does not implement this
plugin extension. The Sampler prepares SFZ patches through draft 3. Automatic effect-chain transitions
remain to do; [sample research and implementation scope](../sample-players-and-formats.md)
cover the next asset milestone. Performance persistence is implemented in the
[versioned folder format](../performance-format.md). The desktop sound picker is implemented; native effect contexts
and per-sound audio are specified separately in [RFC 0007](0007-effect-contexts.md).

## Progress

Checked when built and tested; the sections below say how.

- [x] The `oscairn.sound-contexts` contract, implemented by the Basic Synth with typed, JSON and remote controls
- [x] A host-owned instance pool for ordinary CLAP and LV2 instruments
- [x] The Sampler preparing SFZ patches
- [x] Prepared sounds saved in Performances; the desktop sound picker
- [x] Native contexts prepared from the plugin's CLAP presets ([draft 5](#presets)), with the SDK's
      discovery helper, the Basic Synth's factory presets and the presets indexed when plugins are scanned
- [x] Presets listed in the desktop sound picker for every instrument
- [ ] Automatic effect-chain transitions (with [RFC 0007](0007-effect-contexts.md))
- [x] A snapshot's sounds prepared beside the playing ones (`oscairn.sound-snapshots/draft-1`,
      [below](#preparing-a-snapshot-beside)), by the SDK and the Basic Synth, and used across a
      Performance change ([RFC 0021](0021-sounds-performances-and-library.md#native-sound-contexts-across-a-change))
- [x] Ringing out through the outgoing chain: the retiring sounds rendered into a hidden ring-out
      buffer (`oscairn.sound-snapshots/draft-2`, [below](#ringing-out-through-the-outgoing-chain)),
      by the SDK and the Basic Synth, routed through the outgoing Performance's effects and level
- [x] Only one change's tail carries over: at a second change the older tail is choked, fading out
      through its own chain, and the buffer goes to the newer one (`oscairn.sound-snapshots/draft-3`)
- [ ] Parts sharing one instance ([RFC 0021, slice 3](0021-sounds-performances-and-library.md#fewer-instances-reusing-what-is-loaded))

## Contract

`oscairn.sound-contexts/draft-5` separates preparation from selecting the sound for new
notes. Preparation belongs to the single control thread and must be safe concurrently
with rendering. It does not reset, gate or reconfigure the playing plugin. Selection
uses a bounded queue and takes effect at the next plugin processing boundary, before
that call's note and parameter events. It can select one MIDI channel or all channels.
This first version does not schedule a selection at a future absolute sample.

Each prepared context has an instance-local ID, name, independent patch values and DSP
state. IDs are never reused within that plugin instance, even after a slot is reclaimed.
Preparing from numeric values starts with the plugin's defaults and applies the supplied
values; it is not a snapshot of whatever happens to be playing. Draft 3 added an optional
patch text for engines that define one (the Sampler takes SFZ) and a message buffer the
plugin fills with a diagnostic when it rejects a patch. Draft 4 gives the patch a base asset
reference, a library folder whose files the plugin resolves through `oscairn.assets`
([RFC 0009](0009-sound-assets-and-dependencies.md)), instead of an absolute directory. Draft 5
adds preparing from one of the plugin's CLAP presets ([below](#presets)). Unknown/duplicate
parameters and invalid values reject the complete preparation.

A context is pinned by selected channels, queued selections, active voices and any
remaining DSP tail. Discard succeeds only when no such users remain. Selecting an old ID
after discard fails. Queue/capacity exhaustion is reported; the host never steals a
context to make a preparation succeed. The plugin reports independent atomic snapshots
of selected channels, active voices and idleness. These fields are not a transaction;
`discard` performs the authoritative lifetime check.

The ABI is in [sound-contexts.h](../../core/clap-ext/include/oscairn-clap/sound-contexts.h).
CLAP or LV2 support alone does not imply this capability. A plugin without the extension
reports capacity zero. In-place plugin state restore remains a separate, potentially
interrupting operation. The implemented ordinary-plugin path prepares spare instances and
routes note ownership between them rather than claiming in-place restore is seamless.

## Presets

Instruments come with their sounds: the Oscairn Organ's 55 programs, Surge's factory patches,
Dexed's DX7 cartridges. A plugin publishes them through standard CLAP preset discovery, and
Oscairn prepares any of them as an instant-switch sound like any other.

**Discovery is CLAP's.** The plugin's entry offers `clap.preset-discovery-factory/2` (or its
`draft-2` compatibility ID). Each provider declares its locations and file types in `init`:
a location inside the plugin (`CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN`, location null), or a
file or folder (`CLAP_PRESET_DISCOVERY_LOCATION_FILE`). Oscairn asks a plugin location once,
and every file of a declared extension in a folder, recursively (all files when no type is
declared). `get_metadata` reports each preset with `begin_preset(name, load_key)` and the
plugins it is for with `add_plugin_id`; a preset naming no plugin is listed for every plugin
in the library. A container (a plugin location, or a file holding several presets) names
each preset and gives it a load key; a file holding one preset may leave both out, and is
then named after the file. As CLAP says, a preset is identified by its location kind,
location and load key: the triple `clap_plugin_preset_load.from_location` takes. The control
API carries it as a `clap-preset:KIND:LOCATION#LOAD_KEY` URI, parts percent-encoded.

**Preparing a native context from one.** Draft 5 adds an optional `prepare_preset(name,
location_kind, location, load_key, message, capacity)` beside `prepare`, under the same
rules: main thread, concurrent with rendering, may read files, never touches a playing
patch, refuses with a diagnostic, never steals a context. It takes the triple as CLAP gives
it rather than an encoding in `prepare`'s patch text, because:

- the provider's load keys and the loader are then one scheme, the one `preset-load` already
  uses, so a plugin that loads presets in other hosts reuses its parser unchanged;
- patch text is the engine's own language (SFZ for the Sampler) with an asset base; a
  reserved prefix inside it would need escaping rules, could collide with a patch language,
  and would leave the base meaningless;
- appended after `state`, it stays null in a draft-4 table, so an instrument without presets
  (the Sampler) builds unchanged and simply prepares none.

A preset is the whole patch: it starts from the plugin's defaults, and takes no parameter
overrides (edit the prepared sound afterwards). A native instrument without `prepare_preset`
refuses presets ("Plugin cannot prepare sounds from presets") rather than loading one over its
playing sound.

**Plugins without native contexts** keep the instance pool: the preset is loaded with CLAP
`preset-load` into a fresh spare instance, never into the playing one, which is then selected
([control protocol](../control-protocol.md#prepared-sounds)). That is instant to switch but
costs an instance per sound; it is not instant to prepare.

**What a Sound saves.** Nothing new. A Sound or Performance saves the plugin's full snapshot:
for a native instrument every prepared patch and its name (the preset's name), for a pooled
one each instance's state. That snapshot is what plays, so the Sound opens when the preset
file has been deleted or edited, on a device without it, and after the plugin changes its
factory set. A reference to the preset would be a second truth that disagrees after the first
edit, so none is saved, and the Sound format stays at version 6. The context's name says which
preset it came from.

**Indexing.** Listing runs plugin code, so it never runs on the audio thread and always has
limits:

- *On scan*, the isolated scanner indexes presets in a second helper process per plugin binary,
  only for one whose entry offers a discovery factory, after its plugins were found, so a slow
  or crashing index never hides a plugin. It has its own timeout (15 s); its failures are
  reported beside the scan's as `Presets not indexed: <reason>` (`Plugin scan timed out`,
  `Plugin crashed or failed during scan`, …), and the plugin is listed without presets.
- *For a loaded plugin* (`presets.list`), the process serving the call runs the same isolated
  scan helper on the plugin's binary (15 s timeout, crash isolation, and on Linux a 1 GiB
  address-space limit), on a thread of its own: the caller never waits on it. Until the
  listing is in, `presets.list` is an error, `Presets are being listed; ask again`, which the
  desktop's picker shows as its preset problem until it asks again. A preset provider that
  hangs or crashes takes neither the engine's control thread nor the plugin's worker down with
  it. The listing, or its failure, is kept per binary (its path and modification time), so a
  hung provider costs one helper timeout, not one per refresh; a helper that could not be
  started (no pipe, no process) is not kept, and the next call tries again. A failed listing
  is a `presets.list` error saying why (`Presets not listed: Plugin scan timed out`, …). A plugin in
  a worker lists presets only this way: with no helper configured it lists none, saying so.
  Only a plugin hosted in the engine's own process with no helper (a host running plugins
  among its own code) has its provider run in-process, stopping new reads after 10 s; one
  `get_metadata` call there is not bounded. The engine and the desktop's in-process mode use
  the `oscairn-plugin-scan` beside them.
- *Limits:* 4096 presets per plugin library; 64 locations and 64 file types per provider;
  10,000 files per folder and 16 levels of subfolders; names up to 255 bytes, locations and
  load keys up to 4096. A preset over a size limit is skipped; at the count or time limit the
  index stops. Either is noted (`Presets: stopped at 4096`, `Presets: stopped after 10 s`,
  `Presets: provider <id> did not initialize`) in the scan's failures.
- *Rescans:* nothing is watched. Each scan indexes again; a loaded plugin's listing is kept
  per binary (above), so a preset file added or removed shows once the binary changes or the
  engine restarts.

**For a plugin author** the SDK does the protocol: `preset-discovery-plugin.hpp` implements
the factory and a provider from two functions (where the presets are, and the presets in one
location), and `sound-contexts-plugin.hpp`'s `Engine::preparePreset` implements
`prepare_preset` from one hook, `readPreset(kind, location, loadKey, patch, error)`: read
preset X into my Patch. [Plugin porting](../plugin-porting.md#presets) lists what each kind of
plugin provides.

## Preparing a snapshot beside

Opening a Performance whose instrument already plays should not load that instrument again: its
sounds are prepared in the playing instance, beside the playing ones, and selected at the cue
([RFC 0021](0021-sounds-performances-and-library.md#native-sound-contexts-across-a-change)). The
Performance holds the instrument's exclusive snapshot, in the plugin's own format, so only the
plugin can read it. A second, optional extension, `oscairn.sound-snapshots/draft-1`, has the one
call that needs, in [sound-contexts.h](../../core/clap-ext/include/oscairn-clap/sound-contexts.h):

```c
bool prepare(const clap_plugin_t*, const clap_istream_t* snapshot, uint32_t* selected,
             char* message, uint32_t message_capacity);
```

- `snapshot` is a whole snapshot as the plugin's sound-contexts `state->save` writes it.
- It runs as `prepare` does: on the main thread, concurrently with rendering, may read files, and
  never touches a playing sound, a selection or a voice. It is not exclusive like `state->load`.
- It validates the whole snapshot first, then prepares every sound in it, the snapshot's Default
  included, as a new context with a fresh ID. All or nothing: with too few free contexts, or
  anything invalid, it adds none, writes why into `message`, and returns false. It never steals.
- `selected` (16 entries) receives, for each MIDI channel, the new ID that channel selects in the
  snapshot. It selects nothing; the host selects them when it wants (at the cue).

It is a separate extension, not a draft 6 of `oscairn.sound-contexts`, so draft-5 instruments
built before it keep working unchanged; they are taken over whole instead (their snapshot loaded
into a free instance). A plugin offers it only beside `oscairn.sound-contexts` with its `state`.
The host retires the outgoing contexts afterwards with `discard`, which succeeds once each is idle.

**In the SDK**, `sound-contexts-plugin.hpp`'s `Engine::prepareSnapshot` implements the call from
the same `readPatch` the snapshot load uses, then `buildPreset` for each context, as for a preset.
`readPatch` must therefore be safe on the main thread while rendering (it fills a patch that is not
yet published). An instrument adds one table, `{Engine::prepareSnapshot}`, and returns it from
`get_extension` for `OSCAIRN_EXT_SOUND_SNAPSHOTS`. To be fast, `buildPreset` should share what the
contexts already hold: a sampler finding a sample another context loaded uses it instead of
loading it again.

The Default can never be discarded, so after a switch it stays, unselected, with the first sound
it had; a snapshot of that instance saves it, and one sound more than a fresh load. Capacity counts
it: the Basic Synth's eight slots hold the Default and seven others, so a one-sound node prepared
beside needs two free.

## Ringing out through the outgoing chain

A switched instance has one output and one place in the graph, so without more the outgoing
sounds' tails play through the incoming Performance's effects and part level. Draft 3 of
`oscairn.sound-snapshots` adds two calls, optional together, that give the plugin a hidden
ring-out buffer, which the host routes through the outgoing chain, and let the host cut an older
tail when a second change comes:

```c
static const char OSCAIRN_EXT_SOUND_SNAPSHOTS[] = "oscairn.sound-snapshots/draft-3";
typedef struct oscairn_plugin_sound_snapshots {
    bool (CLAP_ABI *prepare)(...);                       /* as above */
    void (CLAP_ABI *ring_out)(const clap_plugin_t*, const clap_audio_buffer_t* buffer); /* optional */
    bool (CLAP_ABI *choke_retiring)(const clap_plugin_t*); /* with ring_out */
} oscairn_plugin_sound_snapshots_t;
```

**How the buffer is passed.** The host calls `ring_out` on the audio thread immediately before
every `process()` call, with a buffer of four `data32` channels of that call's `frames_count`,
already zeroed, or with null when there is no ring-out for that call. The pointer
is valid only until that `process()` returns; a `process()` not preceded by a call has none. The
plugin only stores the pointer: nothing allocates, locks or blocks. It is not a CLAP audio port:
the plugin's ports, the host's Routing and Mixer, and saved Performances never show it. The host
gives a buffer only while it routes it somewhere; otherwise the plugin renders as before.
`ring_out` and `choke_retiring` may both be null (one without the other is refused), and drafts 1
and 2 are gone (formats are replaced outright): a plugin built against either must rebuild against
draft 3, and without `ring_out` its tails play through the incoming chain as before.

**What goes into it.** A context is *retiring* while no MIDI channel selects it and it still has
voices or a tail. During a `process()` given a buffer, the plugin renders every retiring
context's main-port output into channels 0 and 1, adding to them, instead of into its main output
port, and a retiring context `choke_retiring` is fading into channels 2 and 3 (below); everything
else, and retiring contexts' other ports, as usual. Selections apply at the start of a
`process()` call, before its notes, so the outgoing sound becomes retiring at sample 0 of the
block whose selection leaves it unselected: the move from main to ring-out is sample-accurate,
at a block boundary.

**Without a click.** The host makes the ring-out route reach exactly what the instance's main
output reached in the outgoing plan: the same effect instances, carried over and ringing out as
in any seamless change, and the same gains (the part's level frozen as it was at the cue). The
selections are queued just before the new plan is published, so they apply in the plan's first
block, where the ring-out route already exists; or in the old plan's last block, where there is
no buffer and the main output still feeds the outgoing chain, and the next block moves it to the
ring-out buffer, into the same chain. Either way the tail's samples reach the same inputs without
a gap or a step.

**In a worker** the shared block holds four more output channels after the plugin's, sized when
the worker is set up for a plugin that offers `ring_out`, and a per-block flag saying whether the
engine wants them; the worker's host then calls `ring_out` with them, as in the engine. Nothing
else crosses: the buffer is part of the block's audio, like the outputs.

**Cost.** Memory: four channels of the maximum block in the plugin host's scratch, for every
instance whose plugin offers `ring_out` (16 KB at 1024 frames), four in a worker's shared block,
and four in the plan for a node with a ring-out route. CPU: while a tail rings, the outgoing chain's
effects run, as in a seamless change between Performances that share nothing, where the whole
outgoing instance runs as well; the instrument renders no more voices than before. The bench
figures are in [RFC 0021](0021-sounds-performances-and-library.md#native-sound-contexts-across-a-change).

**Overlapping changes: only one tail carries over.** Decided by the owner. At a second change
while the first change's tail still rings, the older tail is cut with a short fade and the buffer
goes to the second change's outgoing sounds, so every tail that remains plays through its own
Performance's chain. The host calls `choke_retiring` on the main thread just before it queues the
incoming selections. It is queued like `select`: at that `process()` boundary, before those
selections, the plugin chokes every context retiring then (the first change's sounds, and the
Default if it holds a tail of theirs), with the fade a CLAP choke gets (the SDK's 3 ms), and renders
them into channels 2 and 3 until they end; the selections then leave the second change's sounds
retiring, in channels 0 and 1. The host routes channels 2 and 3 where channels 0 and 1 went before
the change, through the first chain, so the fade is heard where the tail was, without a jump or a
click; once those sounds are discarded it removes that route. It returns false when nothing could
be queued (the selection queue is full); the older tail then moves to the second chain, as before
draft 3. A third change within the fade sends the rest of the fade through the second chain.

**Limits, for plugin authors** (none needs code in the plugin beyond the SDK's; all are heard by
its users):
- **One tail carries over:** a second change while a first change's tail still rings cuts that
  tail with the choke fade (3 ms in the SDK), through its own chain; only the second change's
  outgoing sounds ring on, through theirs. A long tail, a held pad or a delay's feedback in the
  instrument, ends at the second cue.
- **Main output only:** only a retiring sound's main output port rings out through the outgoing
  chain; its other output ports keep playing through the instance's ports, which follow the
  incoming chain.
- **30 seconds:** an outgoing effect fed only by a ring-out route goes once 30 seconds pass with
  no note through it, so a note held through the change for longer loses the outgoing effects and
  its tail moves back to the main output, through the incoming chain.

**In the SDK**, `Engine` keeps the buffer `ring_out` gives (`Engine::ringOut`, cleared when
`process()` returns), and `Engine::retiring(context)` returns the two channels a context renders
its main output into, or null: `ringOut` for a retiring context, `ringOut + 2` for one
`Engine::chokeRetiring` is fading. `chokeRetiring` queues the request in the selection queue;
applied, it starts the choke fade (`chokeLeft`/`chokeLength`) on every voice of a retiring context
and marks the context until its last voice ends. An instrument's `render` adds a context's main
output into the channels `retiring` returns instead of its main port. The table becomes
`{Engine::prepareSnapshot, Engine::setRingOut, Engine::chokeRetiring}`.

## Basic Synth and Sampler

Both instruments use one shared implementation of this contract,
[sound-contexts-plugin.hpp](../../core/clap-ext/include/oscairn-clap/sound-contexts-plugin.hpp); the rules below apply to both,
except parameters, which the Sampler does not have. A Sampler note outside its region's
key or velocity range starts no voice and reports `NOTE_END` immediately.

The Basic Synth has eight context slots including a protected Default context (ID 0),
a shared pool of 16 voices, and a 32-entry selection queue. Context capacity is fixed
for an instance. Preparation uses preallocated slots; rendering and selection allocate
nothing and never wait for the control thread. At 16 active voices, the existing oldest
voice stealing policy applies across the instrument. Context preparation itself never
steals a voice.

A voice keeps its owning context, key, channel and CLAP note ID through a sound change.
Explicit note IDs release or express their matching voices. CLAP `-1` fields always mean
wildcards, including the note ID: a key-specific off with ID `-1` releases all matching
CLAP voices. Only raw MIDI repeated presses use oldest-held-first release. A CLAP note-on
with zero velocity remains a note-on. The graph assigns destination IDs per route so
identical source IDs do not collide, and the synth emits `NOTE_END` when voices finish or
are stolen. Key/channel pressure without note IDs applies to all matching voices,
including outgoing contexts, because that input cannot distinguish individual presses.
Sustain follows the MIDI channel across switches; CC64 reaches the plugin unchanged.
Disconnecting a native route chokes its owned voices, including sustained voices, then
restores the highest CC64 value remembered by remaining routes on that port/channel.
Other routes' held notes survive; channel-wide pedal changes can release their sustained
tails if no remaining route owns a down pedal. CC120 stops the addressed channel; overflow recovery resets the affected
destination and clears sustain. Release envelopes finish before
an outgoing context becomes idle.

Ordinary parameter events edit the contexts currently selected for their channel (all
selected contexts for a global event). Unselected outgoing patches retain their values.
Queued selection runs before parameter events at the same processing boundary, so a
parameter edit in that call affects the newly selected sound. Global selection emits
parameter-value feedback for the selected patch. Per-channel parts have distinct values;
the ordinary selected-patch parameter view is not a complete editor for all parts.

Each context sums and saturates its voices independently; the context outputs then sum.
This preserves the sound of each patch while several patches play. Default single-context
rendering retains its previous behavior.

## Control and verification

The control API exposes `sounds.get`, `sounds.prepare`, `sounds.select`, and
`sounds.discard`; see the [wire contract](../control-protocol.md#prepared-sounds).
They operate immediately on plugin context state, independently of staged graph edits.
Selection acknowledgement means queued, not already applied; query the selected-channel
mask to observe application. Closing a control client does not discard contexts.

Tests compare held voices and simultaneous sounds with independent reference instances,
including exact-sample note-ID releases, expression, parameter edits, repeated keys,
sustain, independent channels, queue exhaustion, stale IDs and safe slot reuse. The same
transition timeline renders identical samples in 37- and 512-frame blocks. An allocation
guard covers rendering; concurrent prepare/select/discard tests run under ThreadSanitizer.
JSON and real socket tests cover the same public operations.

## Exclusive snapshots

Draft 2 adds an optional `state` interface for a full native context snapshot. Its save/load
callbacks require rendering and parameter flush to be excluded and processing stopped;
this is deliberately not advertised as concurrent `CLAP_EXT_STATE`. Save must include all
accepted queued selections. Load validates before replacing patches, reconstructs IDs and
excludes voices/tails. Oscairn uses it in preference to ordinary CLAP state, which may describe only one patch. A native engine
without a complete snapshot interface cannot be saved as a complete Performance.

Basic Synth's `oscairn-synth-2` state stores eight slots (occupancy, quoted name, ten
parameter values) followed by 16 selected slot indices. Version 1 (eight values, before the pitch
bend range and mod wheel) is refused, not converted. Numbers use the classic locale
and round-trip precision. Flush applies accepted selections before queued patch edits.

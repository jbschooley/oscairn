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
- [ ] Presets listed in the desktop sound picker for every instrument
- [ ] Automatic effect-chain transitions (with [RFC 0007](0007-effect-contexts.md))
- [ ] Sounds prepared in a playing instance across a Performance change, and parts sharing one
      instance ([RFC 0021, slices 2 and 3](0021-sounds-performances-and-library.md#fewer-instances-reusing-what-is-loaded))

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
- *For a loaded plugin* (`presets.list`), its own process indexes: in the engine that is the
  plugin's worker, whose control requests time out after 30 s; the index stops starting new
  `get_metadata` calls after 10 s, so a large library returns what was read in time instead of
  failing the worker.
- *Limits:* 4096 presets per plugin library; 64 locations and 64 file types per provider;
  10,000 files per folder and 16 levels of subfolders; names up to 255 bytes, locations and
  load keys up to 4096. A preset over a size limit is skipped; at the count or time limit the
  index stops. Either is noted (`Presets: stopped at 4096`, `Presets: stopped after 10 s`,
  `Presets: provider <id> did not initialize`) in the scan's failures.
- *Rescans:* nothing is watched or cached. Each scan indexes again, and each `presets.list`
  reads the plugin's locations again, so a preset file added or removed shows at the next
  listing.

**For a plugin author** the SDK does the protocol: `preset-discovery-plugin.hpp` implements
the factory and a provider from two functions (where the presets are, and the presets in one
location), and `sound-contexts-plugin.hpp`'s `Engine::preparePreset` implements
`prepare_preset` from one hook, `readPreset(kind, location, loadKey, patch, error)`: read
preset X into my Patch. [Plugin porting](../plugin-porting.md#presets) lists what each kind of
plugin provides.

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

# RFC 0005: Native prepared sound contexts

Status: first instrument implementation. The Basic Synth and its typed/JSON/remote
controls implement this contract. Ordinary CLAP/LV2 instruments also have a host-owned instance pool, documented in the
[control protocol](../control-protocol.md#prepared-sounds); it does not implement this
plugin extension. Effect-context transitions, asset-bearing native patches, project
persistence remain to do. The desktop sound picker is implemented; native effect contexts
and per-sound audio are specified separately in [RFC 0007](0007-effect-contexts.md).

## Contract

`oscairn.sound-contexts/draft` separates preparation from selecting the sound for new
notes. Preparation belongs to the single control thread and must be safe concurrently
with rendering. It does not reset, gate or reconfigure the playing plugin. Selection
uses a bounded queue and takes effect at the next plugin processing boundary, before
that call's note and parameter events. It can select one MIDI channel or all channels.
This first version does not schedule a selection at a future absolute sample.

Each prepared context has an instance-local ID, name, independent patch values and DSP
state. IDs are never reused within that plugin instance, even after a slot is reclaimed.
Preparing from numeric values starts with the plugin's defaults and applies the supplied
values; it is not a snapshot of whatever happens to be playing. Unknown/duplicate
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
interrupting operation. The future ordinary-plugin path will prepare spare instances and
route note ownership between them rather than claim in-place restore is seamless.

## Basic Synth

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

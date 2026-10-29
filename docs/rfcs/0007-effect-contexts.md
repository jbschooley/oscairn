# RFC 0007: Independent effects on shared native engines

Status: first implementation complete. Delay provides independently scheduled contexts
sharing one native engine; Basic Synth provides eight sound buses in addition to Main.
Typed/JSON/remote control, state, resource telemetry and reference-graph tests are implemented.
Performance persistence is implemented in the [versioned JSON format](../performance-format.md).
Automatic Sound-to-chain management remains planned.

## Musician-facing behavior

A Sound can have several insert effects. Different Sounds can use the same effect type
with different settings, including two parallel versions on one instrument. Several Sounds
can also send to one shared effect with one setting/state. A sound change preserves old
voices, their effect routing and their outgoing effect tails while new notes use the new
sound and chain. Reusing an engine is an implementation detail, not something the musician
must arrange in a complicated rack.

Yamaha's [MONTAGE M effect documentation](https://manual.yamaha.com/mi/synth/montage_m/en/om01basicoperation0100.html)
describes per-Part insertion effects, shared system effects and master effects. That is
useful workflow precedent, not evidence about Yamaha's internal allocation or DSP sharing.
Separately, [local ESP 3.0.1 inspection](../yamaha-esp-effects-research.md) found central
routing buffers alongside block-owned arrays and algorithm instances. This supports the
shared-infrastructure/independent-state distinction below, but does not measure savings
or establish the hardware implementation.

## Logical effects and shared processing resources

Keep each independently configured effect as a separately addressable logical graph node.
A native engine may provide several contexts behind those nodes, sharing code, read-only
tables/assets and compatible backend resources. Each context owns its mutable parameters,
signal history and tail. Two contexts can run in series, in parallel, or on different
instruments. Grouping them into one graph vertex must not introduce false cycles or force
all contexts to run at the same point in the dependency order. Schedule logical contexts
according to their actual audio dependencies; batch only independent work where safe.

A shared send is different: several sources sum into one context. It has one set of
settings and one combined tail. It must never be silently substituted for independent
contexts merely because their current settings happen to match. Nonlinear effects and
later automation can make that substitution audibly wrong.

The native extension/backend contract must cover preparation, per-context processing,
parameter/state access, resource accounting and authoritative retirement. Do not reuse
instrument MIDI-channel selection as an effect routing mechanism. Ordinary CLAP/LV2
plugins keep separate instances unless they explicitly offer a suitable native capability.
The same logical UI and saved graph must work with that fallback.

## Per-sound audio and transitions

Basic Synth parameter 8 selects Main (0) or sound bus 1–8 for new notes. Each voice
captures its bus at note-on and retains it through sustain/release. Ports are permanent
buses, not reusable context IDs: preparing another Sound on a bus deliberately joins that
bus's processing chain. Routed voices are excluded from Main. Give simultaneous independent
chains different buses; the host cannot separate sounds after they have been summed.

A voice retains its sound context and output ownership until it ends, including sustain
and release. New-note selection does not move held voices onto the new chain. Keep the
outgoing chain fed while its voices remain; then feed silence until its effects report
completion. Shared sends remain alive while any contributing route needs them. Define
explicit tail-cut behavior for deliberate removal, separate from seamless transitions.

Prepare buffers, patches and routing off the audio thread. Publish validated plans at an
audio boundary. Running/retiring plans pin referenced contexts; context IDs are generation
safe and stale routes cannot address a reused slot. Capacity exhaustion preserves the
working sound and reports the shortage. There is no allocation, deletion, lock wait or
unbounded preparation in rendering. Preserve the existing main stereo workflow when no
per-context output is requested, without accidentally sending both its mixed and separate
versions to the same destination.

## Resource costs and hardware

Pooling does not promise constant RAM or CPU as more effects become active. Delay lines,
reverb history, compressor envelopes and similar mutable state generally remain separate.
Native code is already shared between ordinary instances in one process. Further savings
come from shared immutable data, less host/instance overhead and algorithm-specific
batching, and must be measured rather than assumed.

For example, the current Delay allocates two float buffers of `2 * sampleRate + 1`
samples: 768,008 bytes at 48 kHz, about 750 KiB per independent history. Two independent
delays still need about twice that history even when they belong to one engine. A future
convolution reverb may share immutable impulse-response data while retaining separate
input histories. Unprepared slots must not allocate full delay/reverb buffers.

CPU, DSP and FPGA backends use the resource/capability contract in
[RFC 0004](0004-hardware-services.md). Context capacity, per-context/shared memory, supported
algorithms, latency and required routing must be known during preparation. A device may
have fixed hardware effect slots; exceeding them must be explicit. Do not change a
musician's independent processing into a shared send to fit a budget.

## Implemented contract and validation

The draft extension [effect-contexts.h](../../core/clap-ext/include/oscairn-clap/effect-contexts.h)
creates child CLAP interfaces with normal parameter/state/port/lifecycle APIs. Each is one
logical graph node, retains the module and engine independently, and can run before or
after siblings according to graph dependencies. Graph plan ownership delays destruction
until rendering is finished. Node IDs are not recycled to address another context.

`effects.create` creates a default patch on the source engine; `effects.resources` reports
shared/context bytes, capacity, prepared count and tail activity. Delay permits 16 contexts
including its originating interface, allocates rings only for prepared contexts, and
keeps all surviving contexts intact when preparation fails. Context creation and destruction
use the control thread. Current scheduling is serial on one audio thread. Removal is an
explicit tail cut; to preserve a tail, retain its node and output route, stop its input,
and wait for `tailActive: false` before removal/commit. A disconnected input does not stop
processing the remaining node. This is not yet an automatic chain-retirement manager.

Delay patch parameters are beats, feedback and wet level. CLAP state stores parameters,
not running history. Its original dry-plus-wet gain law and tempo glide remain. Histories
below 1e-12 are zeroed to bound tail completion and avoid denormals. Standard hosts can
use a single Delay without understanding this extension.

The `effect-contexts` test compares independent and shared serial/parallel graphs, a summed
send, and two sound buses feeding different delays against separate instrument/effect
chains. It also covers exact automation, sustain/release, tails, capacity exhaustion,
parent removal, state round trips and concurrent preparation. Render calls use AllocationGuard.
The socket and JSON tests cover creation, resource readback and independent controls.

On the arm64 release build at 48 kHz, reported plugin-owned memory is 768,248 bytes for
one Delay, 1,536,472 for two shared contexts, and 1,536,496 for two separate engines.
Only 24 bytes are saved: this algorithm has no large immutable assets to share. Host
queues/scratch, shared_ptr control blocks and allocator overhead are excluded. One local
benchmark rendered 508,000 context-frames in about 3.3 ms in either configuration; this
is wall time, not a hardware-independent speedup claim. The test prints fresh measurements.

## Acceptance sequence and remaining integration

1. Give native Delay independently configured contexts with per-context tempo division,
   feedback and wet/dry values. Preserve current default audio. Expose each context as a
   logical effect with explicit ports and parameter/state access through typed/JSON/remote
   control; engine grouping does not constrain graph ordering.
2. Compare two contexts in one engine against two reference instances: separate instruments,
   one source split into parallel effects, and serial effects. Verify isolation, a shared
   send's sum, independent automation, and audible overlapping tails.
3. Add Basic Synth per-sound output ownership and combine old/new sound contexts with
   separate effect chains. Compare held-note/sustain/release transitions against independent
   reference graphs in uneven blocks. Verify disconnected/retired plans cannot reuse a
   live context and failed preparation preserves the prior audio.
4. Measure actual shared/per-context RAM and CPU for one versus several contexts. Test idle
   contexts, exhaustion, concurrent preparation/retirement and zero audio-thread allocation
   under sanitizers. Add an audible desktop example with clear chain/settings readback.
5. Persist the logical Sounds/chains and provenance in the Performance save/reopen milestone.
   Runtime IDs and physical DSP slots are reconstructed on preparation, not saved as identity.

All five checks are implemented for explicit routes and prepared sounds; automatic
Sound/chain setup and retirement remain future work.
A package manager, general rack editor and hardware acceleration are not prerequisites.

Performance persistence now records shared engine relationships and each context’s state
separately; see [the Performance format](../performance-format.md). Histories/tails are
excluded, and successful reopen starts a fresh stopped graph.

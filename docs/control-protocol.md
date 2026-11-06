# Control protocol: implemented subset

`oscairn::JsonRpc` adapts the existing `Control` interface to
[JSON-RPC 2.0](https://www.jsonrpc.org/specification). Link `oscairn_control_json`; call
`dispatch(string_view)` on the control thread with a complete UTF-8 JSON document.
The return value is one compact JSON document, or `nullopt` when no response is due.
The adapter performs no I/O, creates no threads and never runs on the audio thread.
`LocalServer` serves this adapter over a Unix-domain socket. `RemoteControl` implements
the same typed `Control` interface for a desktop connected to `oscairn-engine`.

The [JSON Schema](control-protocol.schema.json) describes the implemented request and
response shapes. [RFC 0003](rfcs/0003-control-api.md) describes the larger planned API.
This subset is protocol version 1. Clients must check `session.hello` capabilities before
using optional methods. Graph editing, parameter coalescing, gestures and subscriptions are
implemented, as are host/device status and helper connections. The desktop now manages
helper startup, reattachment and explicit crash recovery by default.

## Audio routing

Capability `graph.audio-routing` adds explicit channel routes to protocol 1. A connection
uses node IDs/names plus **port IDs** from `graph.get`, not port-array positions. External
capture/playback endpoints are the numeric `input` and `output` IDs in that result
(`4294967294` and `4294967295`). The top-level `audioInputs` and `audioOutputs` describe
the external ports; plugin ports remain inside each node. Audio arrays are flattened in
port-list order internally, while the control API preserves each port's ID and channels.

For example, route a plugin's stereo output port 30 to channels 2/3 of external port 0:

```json
{"jsonrpc":"2.0","id":1,"method":"graph.connect","params":{"kind":"audio","from":"deckB","fromPort":30,"fromChannel":0,"to":4294967295,"toPort":0,"toChannel":2,"channels":2,"gain":1}}
```

Use actual advertised IDs; the numbers above are illustrative. Explicit routes require
all four fields `fromPort`, `fromChannel`, `toPort`, `toChannel`; `channels` defaults to 1
and `gain` to 1. Each range must fit wholly within its selected port. Gain must be finite
and within -16..16. Multiple connections sum, with no hidden normalization or limiting.
For a stereo-to-mono average, connect each source channel separately to the same target
channel with gain 0.5. For mono-to-stereo, explicitly connect the source to both destination
channels. Unconnected channels are silent. Extra plugin outputs remain independent;
there is no automatic fold into master. Missing ports and invalid ranges reject the edit.

Audio routes have one identity per source/destination channel pair. Ranges expand into
these pairs, so overlapping range and single-channel connections cannot duplicate a
signal. Repeating a connection updates its gain; connections from different source
channels still sum. `graph.get` always returns explicit single-channel routes (`channels: 1`).
`graph.disconnect` removes the selected pairs regardless of gain; omit `gain` when
disconnecting. Gain changes are staged until `graph.commit` and do not yet ramp.

Node-only `graph.connect` is a main-bus convenience operation using the same route
collection. CLAP uses its declared main port (or its only port). LV2 mono/stereo plugins
use their one/two separate audio ports; larger ambiguous LV2 layouts require explicit
port/channel routes. External endpoints use their first configured port. Matching channels
connect in order, a mono source is duplicated into a stereo destination, and otherwise
only the shared channel count connects. Additional buses never fold into the main bus.
Node-only `graph.disconnect` removes **all** audio routes between the two nodes.

All routing edits take effect at commit, which validates dependency cycles and builds
buffers/routes outside the audio callback. A failed commit preserves the live plan.
External audio layouts are supplied when constructing Graph/Session and remain fixed for
that session. Limits are 256 input and 256 output channels per node or external endpoint
set, and 4096 source/destination channel pairs per graph. Format-adapter limits also apply: CLAP currently
allows 16 ports and 64 channels per direction. Existing 64-KiB wire-message limits still apply
(no pagination yet); large topologies may require the C++ API until paged snapshots exist.
Shared-host CLAP instances lock their loaded audio port IDs, order and channel counts.
A later structural rescan fails that instance under its render gate and preserves the
original port metadata; reload/reconnect the node to adopt a new layout. Names and flags
may still refresh. This prevents old cue routes from following a changed port index.
The stereo `process` overload remains a convenience for stereo layouts; other layouts
use the overload taking input/output arrays and channel counts. Missing capture channels
are silent; missing required output buffers fail and silence supplied outputs.

`session.status.audioInputs` and `.audioOutputs` list logical channels in order, each as
`{"deviceChannel":4,"name":"Output 5","available":true}`. `audioReady` reports whether
the current device/rate/channel layout can run. The JUCE adapter reports unavailable
selected channels and does not redirect them. Layout/device changes require restarting
the helper; UI configuration and runtime device switching remain future work. Older
servers lack these fields/capability; the current RemoteControl retains their stereo
snapshot defaults but does not silently downgrade explicit routing requests.

## Requests and responses

Requests use named `params` objects. Unknown or duplicate method-parameter fields are
rejected. `params` can be omitted for `session.hello`, `session.status`, `transport.get`, `graph.get` and `graph.commit`; `{}` also works.
Positional arrays receive invalid-params errors. Extra envelope fields are ignored, but
duplicate envelope fields make the request invalid.

IDs can be strings, numbers or null. Numeric ID tokens are echoed without converting them
to floating point. Omit `id` for a notification: valid notification envelopes receive no
response, including when their method or parameters fail. Invalid envelopes receive an
error with a null ID. Notifications therefore cannot confirm queue acceptance; use
requests for commands whose outcome matters.

A nonempty JSON-RPC batch executes in input order and returns responses in that order,
omitting notifications. An all-notification batch returns no document. A batch is not a
transaction: a failing member does not undo preceding members. Nested batches are invalid
request members.

| Method | `params` | `result` |
| --- | --- | --- |
| `session.hello` | omitted or `{}` | `protocolVersion`, `server`, supported methods and optional features in `capabilities`, `limits` |
| `engine.attach`, `engine.release` | `{}` | `{"accepted":true}`. A UI attaches when it connects to a managed engine and releases it when it quits normally; the managed engine stops once every attached client has released it. A client that disconnects while attached (a crash) keeps the engine running until a client attaches again, or the last attached client still connected releases it; clients that never attach (background jobs) do not count. Engines started without `--managed` ignore both |
| `session.status` | omitted or `{}` | `audioDevice`, `midiInputs` (each `{"source","name","kind","port"}`, below), `midiOutputs` (the same shape; `source` is the output's own number, from 1), `sampleRate`, `bufferSize`, `cpuLoad`, `deadlineMisses`, `graphEventDrops`, `midiEventDrops`, `audioReady`, mapped `audioInputs`/`audioOutputs`, `failures`: each plugin that has failed and is silent (`node`, `name`, `reason`), `notices`: what the last Performance load, Sound or import passed over without failing (an optional asset missing), as text, and `workers`: each plugin in its own worker process with the CPU time it has used in seconds, restarts included, its memory in bytes, and the microseconds its last block took inside the worker and the most any block has (`node`, `name`, `cpu`, `memory`, `block`, `blockMax`) |
| `state.info` | `{"node":"synth"}` | `{"supported":true}` when opaque state export is available |
| `state.save` | `{"node":"synth","directory":"/path/to/new-state"}` | `{"saved":true}` after publishing a new state directory |
| `window.info` | `{"node":"synth"}` | `{"available":true}` when the plugin has a window of its own (CLAP's GUI extension) that this engine can show |
| `window.show` | `node`, `show`, optional `title`: opens the plugin's own window titled `title`, or titles and raises it if open; `show` false closes it. The process hosting the plugin shows it (its worker, as a window of its own); it closes when the node goes away ([RFC 0019](rfcs/0019-adaptive-ui.md#plugin-windows)). `-32005` when the plugin cannot open it | `{"shown":true}` or `{"shown":false}` |
| `presets.list` | `{"node":"synth"}` | `{"presets":[{"uri":"urn:example:preset","name":"Warm"}]}` |
| `properties.list` | `{"node":"sampler"}` | `properties` entries: `uri`, `name`, `type`, `readable`, `writable` |
| `properties.get` | `{"node":"sampler","uri":"…"}` | `{"value":null}` or a typed plugin-confirmed value |
| `properties.request` | `{"node":"sampler","uri":"…"}` | `{"accepted":true}` when a readable property's refresh is queued |
| `properties.set` | `{"node":"sampler","uri":"…","value":{"type":"path","value":"/samples/piano.wav"}}` | `{"accepted":true}` when a writable property's change is queued |
| `params.list` | `{"node":"synth"}` | numeric `node`, array `parameters` of `id`, `name`, `minimum`, `maximum`, `defaultValue`, `stepped`, CLAP `flags` and `module`, its CLAP module path (`"Oscillator 1/Pitch"`, or empty), by which editors group it; `widget` (`auto`, `slider`, `knob`, `buttons`, `switch`, `list` or `range`) `priority` (1 first, 0 none) and `group` (parameters of one module sharing a nonzero group are drawn joined) from the plugin's `oscairn.ui` hints, kept only where they fit the parameter, and for a range `rangeHigh`, its high parameter; `role` (`cutoff`, `attack`, `mix`... as the role banks name them) when the plugin declares one |
| `params.get` | `{"values":[{"node":"synth","param":3}]}` | `{"values":[{"node":1,"param":3,"value":0.5}]}` |
| `params.set` | `{"values":[{"node":"synth","param":3,"value":0.7}]}` | same shape as `params.get`, with accepted plain values |
| `params.text` | `{"node":"synth","param":3,"value":0.5}` | `{"text":"50%"}` |
| `transport.get` | omitted or `{}` | `playing`, `tempo`, `beat`, `clockSource`, `clockOutputs` |
| `transport.set` | one or more of `playing` (boolean), `tempo` (1..1000), `clockSource` (`"internal"` or `"midi"`), `clockOutputs` (up to 16 device rules choosing MIDI outputs, as for a part's `devices`, replacing the last; empty, the default, sends no clock) | `{"accepted":true}` |
| `params.gesture` | `node`, `param`, `begin` (boolean) | `{"accepted":true}` |
| `subscribe`, `unsubscribe` | `topics` (1..7 of `params.changed`, `transport`, `graph.changed`, `status`, `record`, `controllers`, `controls.changed`), optional `allChanges` boolean | `{"accepted":true}` |
| `graph.get` | omitted or `{}` | staged topology, names/plugin metadata/ports, each node's `enabled` and `ringing` (switched off and still sounding out), connections, mappings, parts, wires (each with `enabled`), `revision`, `staged`, numeric `input`/`output` endpoints |
| `graph.role` | `node`, `role`: one of `insertion-a`, `insertion-b`, `part-eq`, `reverb`, `variation`, `master-effect`, `master-eq`, `arpeggiator` ([RFC 0016's node roles](rfcs/0016-effects-suite.md#where-the-effects-go)), or `null` for none; `graph.get` gives each node's `role` (or `null`). An edit, saved with the node | `{"accepted":true}`; an unknown role or node is refused |
| `graph.enable` | `node`, `enabled`: switches a plugin off or on, as scenes do. Off, it takes no new notes and no sound: notes it holds play on until released, its sound rings out and then stops (`ringing` until then). In a part's chain, what reached it passes around it, so an effect off is bypassed. An edit, staged until commit | `{"accepted":true}` |
| `graph.add` | `path`, optional `name` and `pluginId` | numeric `node` |
| `graph.remove` | `node`, optional `userWires` (`remove`, the default, or `reconnect`) | `{"accepted":true}` |
| `graph.connect`, `graph.disconnect` | `kind` (`audio` or `notes`), `from`, `to`; optional explicit audio route fields below | `{"accepted":true}` |
| `graph.map` | `from`, `lane`, `to`, `param` | `{"accepted":true}` |
| `graph.unmap` | `from`, `lane` | `{"accepted":true}` |
| `graph.commit` | omitted or `{}` | new `revision` |
| `parts.add` | optional `name`, `input` (`{"kind":"notes"\|"audio"\|"none","port":N}`, default notes) and `output` port | numeric `part` |
| `parts.remove` | `part` | `{"accepted":true}` |
| `parts.insert` | `part`, `index`, `node`; optional `split` and `lane` for a lane's chain | `{"accepted":true}` |
| `parts.removeNode` | `part`, `node`, wherever it is in the part | `{"accepted":true}` |
| `parts.move` | `part`, `from`, `to` (chain positions); optional `split` and `lane` | `{"accepted":true}` |
| `parts.addSplit` | `part`, `index`; optional `split` and `lane` to split inside a lane; optional `kind` (`copy`, `leftRight`, `midSide`, `bands`) with its `splitter` and `joiner` nodes | numeric `split` |
| `parts.removeSplit` | `part`, `split`: its first lane's chain takes its place; other lanes' nodes leave the part | `{"accepted":true}` |
| `parts.addLane` | `part`, `split` | numeric `lane` |
| `parts.removeLane` | `part`, `split`, `lane`: its nodes leave the part; the last lane removes the split | `{"accepted":true}` |
| `parts.lane` | `part`, `split`, `lane`; any of `zones`, `level`, `pan`, `mute`, `solo` (others keep their values) | `{"accepted":true}` |
| `parts.input` | `part`, `input` (as for `parts.add`, with optional `zones`, `devices` and `maxNotes`, notes at once from 0 (no limit) to 128) | `{"accepted":true}` |
| `parts.midiOut` | `part`, `midiOut`: `devices` (rules choosing MIDI outputs, as for inputs; none, the default, sends nothing) and `channel` (1–16; 0, the default, keeps each message's own) | `{"accepted":true}`: the part's MIDI goes to those outputs as well ([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md#step-1-status)) |
| `harmony.get` | omitted or `{}` | the Performance's `key`, each part's own (`parts`: `{"part","key"}`, `key` null when it follows the Performance's), and the named `scales`. A key is `root` (0 = C .. 11 = B), `scale` (its pitch classes in semitones above the root, from 0) and `name` (the scale's, when it is one of the pad editor's; else null) ([RFC 0033](rfcs/0033-script-block.md#the-parts-scale-and-chord)) |
| `harmony.set` | `key`: `root` and `scale`, a name from `scales` or pitch classes including 0; with `part`, that part's own key, or null to follow the Performance's again | `{"accepted":true}`; an edit, staged until commit. Plugins in a part read its key and the chord held on it through `oscairn.harmony` |
| `parts.output` | `part`, and `bus` (play through that bus) or optional `port` (a graph output) | `{"accepted":true}` |
| `parts.send` | `part`, `bus`, optional `level` (without one, the send is removed) | `{"accepted":true}` |
| `parts.level` | `part`, `level`, `mute`: its fader; sends are post-fader | `{"accepted":true}`: a live gain ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#instant-controls)): heard from the next block, ramped across it, with no plan compiled (`graph.commit` after it compiles none unless other edits are staged); a fader whose wires cannot be live stages an edit as before |
| `parts.name` | `part`, `name` | `{"accepted":true}` |
| `parts.pan` | `part`, `pan` (-1 left .. 1 right): where it plays; its sends keep their own | `{"accepted":true}` |
| `master.set` | `level` (0..2, a gain) and `pan` (-1 left .. 1 right): the Performance's level and pan on its main output, after every part, bus and user wire playing there; `graph.get` gives them as `master`, and each scene its own | `{"accepted":true}`; a live gain like `parts.level`'s; an edit, undone with `edit.undo`, stored by scenes with the mix |
| `parts.solo` | `part`, `solo`: while any part is soloed, only soloed parts, the parts feeding them and the buses they feed are heard | `{"accepted":true}` |
| `edit.undo`, `edit.redo` | omitted or `{}`: undoes the last edit, or redoes the last undone | `undo`, `redo`: what each would now do (empty: nothing) |
| `edit.history` | omitted or `{}` | `undo`, `redo` labels, as above |
| `parts.meters` | omitted or `{}` | `meters`: each part's and bus's recent peak after its fader and pan (linear), held and falling: `peak`, and `channels` with one value per channel of its sound (its last plugin's main audio output as wired to where it plays: one for mono, two for stereo, more for surround) |
| `wires.meters` | omitted or `{}` | `meters`: each audio wire's recent peak after its level and pan (linear), held and falling: `wire`, `peak`, and `channels` with one value per channel its source's port sends. A muted wire shows 0 |
| `wires.connect` | `kind`, `from`, `to`; for audio `fromPort`, `toPort`, optional `channels` pairs; optional `level`, `pan` | numeric `wire` |
| `wires.disconnect` | `wire` | `{"accepted":true}` |
| `wires.enable` | `wire`, `enabled`: switches a wire off or on. Off, a note wire takes no new notes (held ones are still released); an audio wire is silent. An edit, staged until commit | `{"accepted":true}` |
| `wires.mix` | `wire`, `level`, `pan`, `mute`, `solo` | `{"accepted":true}` |
| `wires.zones` | `wire` (a note wire), `zones` | `{"accepted":true}` |
| `scenes.add` | optional `name`: captures all of it as it is now (below) | numeric `scene` |
| `scenes.store` | `scene`: captures it into that scene again | `{"accepted":true}` |
| `scenes.select` | `scene`: applies the kinds in its scope at once and commits staged graph edits; plugins switched off ring out | `{"accepted":true}` |
| `scenes.remove`, `scenes.name` | `scene` (and `name`); removing one takes it from every mapping's `offIn` | `{"accepted":true}` |
| `scenes.scope` | `scene`, `scope`: the kinds selecting it applies, of `enabled`, `mutes`, `mix`, `parameters`, `mappings`; the rest stay as they are | `{"accepted":true}` |
| `scenes.transition` | `scene`, optional `glide` (milliseconds, at most 60000; 0 or absent: at once), `curve` (`linear`, `ease` or `equal-power`) and `launch` (`now`, `beat` or `bar`): how selecting it moves there (below) | `{"accepted":true}` |
| `scenes.untouched` | `parameters`: the `{node, param}` scenes never touch (they touch every other automatable, writable parameter); one taken back gets its current value in every scene | `{"accepted":true}` |
| `controls.add` | optional `name` and `part`, the part that owns it (RFC 0021; 0 or absent: the Performance). A part's control targets only its part's plugins and its other controls, and goes with it into a Sound and when it is removed | numeric `control` |
| `controls.remove`, `controls.name` | `control` (and `name`) | `{"accepted":true}` |
| `controls.targets` | `control`, `targets`: each `kind` (`parameter` with `node`, `param`; `partLevel` or `partMute` with `part`; `control` with `control`; `morph` with `from` and `to`, two scenes: the control moves between them, as below), optional `start`, `end` (0..1, end below start reverses), `curve` (`linear`, `exponential`, `logarithmic`, `s-curve`, `{"kind":"db","min":M,"max":X}` with −144 ≤ M < X ≤ 0, or `{"kind":"power","exponent":E}` with 0.1 ≤ E ≤ 10, as [RFC 0011](rfcs/0011-performance-ui-and-editors.md#the-control-tree) defines them; `graph.get` gives them the same way), `relative`, and `offIn` (the scenes it is off in) | `{"accepted":true}` |
| `controls.set` | `control`, `value` (0..1): sets its targets, and theirs | `{"accepted":true}` |
| `pads.get` | omitted or `{}` | the pad map: `bank` and eight `banks` of 16 pads, each `note`, `channel` (-1 follows the player), `velocity` (0 as played), `pressure` (`none`, `poly`, `channel`) |
| `pads.set` | `map` (as `pads.get` returns it) | `{"accepted":true}` |
| `pads.play` | `pad` (0..15, from the bottom-left), `on`, optional `velocity` and `channel` (the player's) | `{"accepted":true}` |
| `pads.pressure` | `pad`, `pressure` (0..1) | `{"accepted":true}` |
| `assignments.get` | omitted or `{}` | `{"assignments":[...]}`: the Performance's physical-control overrides ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#physical-controls)), each `control` (its logical identity: `knob:N`, `fader:N`, `button:N` (assignable switches `button:1` up, scene and mute buttons from `button:101`), `wheel:N`, `strip:N`, `jog:N`, `pedal:N`, `footswitch:N`, `aftertouch:N` or `breath:N`), `kind` (`parameter`, `control`, `partLevel`, `partMute`, `scene`), `node` and `param` for a parameter, `id` (the control, part or scene) otherwise, and `part` (0, or the part whose Sound's own assignment it is) |
| `assignments.set` | `control`, `kind` (as above, or `none` to remove its override), `node` and `param`, or `id`; optional `part`: that part's Sound's own assignment, to one of its plugins' parameters or its controls (the mod wheel's target) | `{"accepted":true}`; an unknown control or target, or a part's assignment to anything not its own, is refused (-32003). An edit: undone with `edit.undo`, saved with the Performance |
| `notes.send` | `on` (boolean), `key` (0..127), optional `velocity` (0..1), optional `channel` (0..15) | `{"accepted":true}` |
| `notes.panic` | `{}` | `{"accepted":true}`: all notes off. Every note played from the keys, MIDI inputs and `notes.send` is released at the next block, and a Performance still ringing out after a change ends at once ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)) |
| `midi.ignore` | `inputs`: up to 64 MIDI input names, as `session.status` lists them (may be empty) | `{"accepted":true}`: the engine leaves out every message from those inputs, replacing the last list, until it exits. For a controller's port that duplicates another's ([RFC 0024](rfcs/0024-device-profiles.md#controllers)); a name not connected now applies when it appears |
| `controllers.set` | `groups`: up to 64 control groups (below), the device's then each connected controller's; `ports`: up to 64 `{input, device, port}`, each MIDI input (by name, as `session.status` lists it) that is a controller profile's port (`device` its profile's id, `port` from 1) | `{"accepted":true}`: replaces the last set. The engine maps every message from a bound input through RFC 0011's [controller-mapping service](rfcs/0011-performance-ui-and-editors.md#instant-controls) on its MIDI input thread; a name not connected now applies when it appears. Not saved: the app sends it again when the engine restarts |
| `controllers.view` | optional `part` (the part the knobs follow; 0 the Performance's controls), `screen` (the editor page shown: `name` and `params`, assignment targets as `assignments.get` writes them; empty `params`: Play's knob row) and `positions` (up to 64 `{group, position}`: a group's page, row or mode, by its place in `groups`) | `{"accepted":true}`: what the screen shows, which the controls follow; groups not listed keep their positions |
| `controllers.get` | omitted or `{}` | `positions` (each group's page, row or mode, in `groups` order), `touched` (the control moved last, `knob:3`; empty: none yet) and `touches` (how many moves so far, so each shows, for learn) |

A control group in `controllers.set` (RFC 0024's [control groups](rfcs/0024-device-profiles.md#control-groups), as
the app resolves them from profiles): `role` (`notes`, `screen`, `assign`, `fixed`), `kind`, `count`,
optional `rows` (`{name, bank}`, bank one of `assign`, `filter`, `ampEG`, `eq`, `levels`, `effects`),
`modes`, `functions`, `scenes`, `paged`, `switches`, and `midi` for a controller's: `device` (its
profile's id), `port`, `channel`, `channels` (one per control), `message` (`cc`, `note`,
`pitchBend`, `pressure`), `numbers` (a CC or note per control, or per control per row),
`encodings` (one per control: `absolute`, `twosComplement`, `binaryOffset`, `signMagnitude` or
`steps`), `steps` (one `[increment, decrement]` per control), `latch` and `feedback`.

`graph.add` accepts a CLAP module or, in an LV2-enabled engine, a `.lv2` bundle directory.
For LV2, `pluginId` is the plugin URI and is required when the bundle has multiple plugins.
Numeric LV2 parameter IDs are control-port indices.
Unsupported required features/ports fail loading with a descriptive error. See
[LV2 hosting](../core/lv2-host/README.md) for limits.

Node addresses accept a name or an unsigned 32-bit numeric ID. Parameter IDs are unsigned
32-bit numbers. All controls use finite double values. Note channel defaults to 0;
velocity defaults to 1 for note-on and 0 for note-off. A note-on with zero velocity becomes
a note-off, as with typed `Control`. UI notes enter at the next block boundary. Physical
MIDI, including pressure, keeps its separate sample-timed route.

Parameter lists contain 1..128 distinct addresses. All addresses, writable flags and values
are validated before any change is submitted. Plugin ranges clamp values and stepped
parameters truncate fractional values, matching the host. Acceptance means retained by the
session, not yet rendered. `params.get` reports the latest retained value when one is
pending, otherwise the host's cached value.

The session coalesces consecutive values for each parameter until `Session::service()`.
The host calls this on the control thread, about every 5 ms in the desktop app, independently
of UI repainting. Each tick submits at most one value per parameter. Accepted values and
gesture boundaries that a plugin queue refuses remain pending for later ticks. A failed
parameter enqueue does not trigger the host's emergency note reset.

`params.gesture` appends begin/end commands. Values never coalesce across a gesture boundary;
the final value precedes its end. Clients are responsible for pairing gestures; they are
not yet associated with per-client ownership or undo transactions. The desktop's parameter
sliders send these boundaries. Screen edits and single parameter writes do not require a
gesture. Delivery assumes the host keeps servicing/rendering a functioning plugin. Removing
a node cancels its pending commands; a parameter removed or made read-only by a plugin
rescan cancels its pending commands and requests client resynchronization.

The session retains at most 4,096 commands (values and gesture boundaries), shared across
parameters. Replacing an already pending tail value consumes no additional slot. If the
store fills partway through `params.set`, error `-32005` includes
`data: {"accepted":N,"index":N}`. The first N entries were accepted; the entry at zero-based
index N and all subsequent entries were not submitted. Accepted entries are not rolled
back. Retry unaccepted entries after control ticks drain the store. `params.gesture` also
returns `-32005` if its boundary cannot be retained. Use requests to detect refusal.
`notes.send` retains its separate bounded queue and emergency release policy.

## Graph edits

Graph operations stage changes on the single control thread; they share one staging area
across clients. `graph.get` describes that staging area, not the currently rendered plan.
`revision` counts successful commits, and `staged` indicates edits since the last commit.
Even an unchanged commit produces a new revision. A successful commit publishes a complete
plan for the audio thread to adopt at a block boundary; it does not wait for adoption.

`graph.add` loads a native CLAP plugin in the engine process. Its optional name must be unique
when nonempty; unnamed nodes use their numeric IDs. IDs are not reused. Audio ports include
IDs, names, channel counts and flags; note ports are currently represented by counts, matching
the graph's whole-node routing. The `input` endpoint accepts external notes and `output`
collects audio. Connections use node names or numeric IDs; the endpoints use the numeric
IDs returned by `graph.get`.

A cycle fails with `-32003`, leaves the live plan/revision unchanged and retains the staged
edits for correction. Disconnect the offending edge and commit again. There is no rollback
method or per-client edit isolation yet. Removing a node prunes its wires and mappings (with
`userWires: "reconnect"`, its user sources are first wired to its user destinations), takes it
out of any part's chain, forgets its name and cancels pending controls. The active plan keeps its plugin alive until
a later successful commit takes effect; its main-thread callbacks continue to be serviced
until the last audio plan using it retires. Parameter changes are independent of graph staging.

## Parts and wires

The graph's audio routes and note connections are compiled from wires
([RFC 0011](rfcs/0011-performance-ui-and-editors.md#step-1-status)). `graph.get` lists them
under `wires`: each has an `id`, `kind`, `owner` (`part` with its `part` ID, or `user`), `from`
and `to` node IDs, audio `fromPort`/`toPort`, explicit `channels` pairs or none, the source's
Mix `level`, `pan`, `mute` and `solo`, and whether it is `connected`. `connections` still
report the compiled per-channel routes.

- `graph.connect` and `graph.disconnect` make and remove user wires; `wires.connect` makes
  one between two ports with the channel rules (equal counts one to one, mono to stereo
  copied with pan, stereo to mono summed at −3 dB) or explicit `[from, to]` channel pairs,
  refusing other widths without pairs (`-32003`).
- The wires into one input port form its Mix: `wires.mix` sets a source's level, pan (for a
  stereo destination), mute and solo. Solo silences the unsoloed sources; mute silences a
  source even when soloed.
- `parts` lists each part's `id`, `name`, `input`, `chain` of node IDs and `output` port.
  The part methods rebuild only that part's own wires: input into the first node, node to
  node along main ports and notes, last node to the output. User wires are never changed
  by them.
- **Shared buses** are parts with input kind `bus`: fed by other parts, with their own chain
  and output. A part plays through one bus (`bus`, else its `output` port) and sends copies
  to any (`sends`, each with a `level`); its send wires carry `send`, the bus's ID. Setting a
  send wire's Mix level sets the send's. A part's `level` and `mute` are its fader: they set
  its wire to where it plays (`fader: true`) and scale its sends, which are post-fader. A wire
  through a bus with no box also carries that bus's level, pan and mute: `wires.mix` on it moves
  the part's level and pan by what was asked of the wire, and its mute only when the wire's own
  changes, so the bus keeps its share. Two ways from a part to one place (an empty bus's output,
  and its send to another empty bus that plays there too) are one wire at their summed level. Edits that would loop a bus into itself are refused;
  removing a bus returns its parts to the main output and ends their sends to it.
- **Zones** decide which notes a note wire passes and how they change: each is
  `{"keys":[low,high],"velocities":[low,high],"channels":mask,"transpose":n,"curve":x,"fixedVelocity":v}`
  (velocities 0..1; the channel mask has a bit per MIDI channel; velocity out = in^curve,
  or `fixedVelocity` above 0). Every field is optional in requests, defaulting to all keys,
  velocities and channels, no transpose, curve 1 and no fixed velocity. A note passes if any
  zone accepts it, and each accepting zone plays its own copy, so two zones layer. Up to
  eight per wire; a part's input `zones` apply to its wire into the first node. Held notes
  keep their releases when zones change.
- **Splits** ([RFC 0011 step 3](rfcs/0011-performance-ui-and-editors.md#step-3-status))
  divide a part's notes and sound into lanes that join again. A part's `chain` lists node
  IDs and `{"split":id}` entries; its `splits` give each split's `id`, `kind`, `splitter`
  and `joiner` nodes (0 for none) and `lanes`, each with its own `chain`, `zones`, `level`,
  `pan`, `mute` and `solo`. Every lane gets the notes, through its zones (after the part's
  own). With `copy`, every lane gets a copy of the sound; the others divide it with an
  Oscairn Split node (`org.oscairn.split`, added first and given as `splitter`), lane k
  taking its output k: `leftRight` and `midSide` have two lanes, joined again by an Oscairn
  Join node (`org.oscairn.join`, the `joiner`), and `bands` two to four, the band count
  following `parts.addLane` and `parts.removeLane`, with crossovers as the Split node's
  parameters. The split sets its nodes' modes, keeps them out of chains, refuses to remove
  them alone (`graph.remove`), and removes them with itself. The lanes' ends sum into
  whatever follows the split (or the Join node), or where the part plays, at the lane's
  level and pan. Solo silences the split's other lanes. A lane's chain may hold another
  split. Wires joining from a lane carry its `split` and `lane`, and take their Mix from it:
  `wires.mix` refuses them. A bus may split too: a send or a part playing through it enters
  each lane of a copy split at the lane's Mix, and those wires carry the bus's `split` and
  `lane`; with the other kinds it enters the Split node.
- **Undo** (`edit.*`): edits to parts, wires, splits, Mix settings, scenes, controls and pads,
  nodes added, removed or replaced, parameter mappings, and parameter values set with
  `params.set` can be undone and redone, up to 100 steps. Everything up to a `graph.commit`
  is one step, as is a change that needs no commit; repeated changes to the same thing within
  1.5 seconds (a fader drag, a knob turn) are one step. Undo changes only what the step
  changed, so a scene switched since keeps its levels. A removed plugin is held by the
  history and comes back with its state and prepared sounds. Playing is not an edit:
  notes, transport, `scenes.select`, `controls.set`, sound selection and preparation.
  Opening a Performance starts a new history.
- **Devices** ([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md#choosing-devices-selectors))
  choose which MIDI inputs play a part, whatever channel they send on. A part's input
  `devices` is a list of rules, `{"kind","port","pattern","match"}`; a note or message
  plays the part if any rule matches the input it came from, and an empty list (the
  default) takes every input. Kinds: `builtIn` (the panel, its keys and pads, and notes
  sent over this protocol), `external`, `all`, `port` (DIN or TRS port number `port`),
  `usb`, `usbNamed` (a USB device whose name matches `pattern`) and `network` (every
  network session, or those matching `pattern`). `match` is `equals`, `contains` (the
  default) or `regex` (ECMAScript); names match ignoring case. Up to 16 rules; an invalid
  pattern is refused. The server resolves rules against `session.status`'s `midiInputs`
  whenever that list changes, applying the result at once unless edits are staged. A
  part whose rules match no input is silent. Each MIDI input has a `source` number (1–63;
  0 is built in), a `name`, a `kind` (`builtIn`, `port`, `usb` or `network`) and its
  `port` number. The desktop app reports every input as `usb`; ports and network
  sessions need the device's own input table.
- **MIDI out** ([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md#step-1-status)):
  a part's `midiOut` sends its MIDI to hardware outputs as well as to its instruments. Its
  `devices` are rules as above, matched against `session.status`'s `midiOutputs` (numbered
  from 1, apart from the inputs) and resolved again whenever that list changes; an empty
  list, the default, sends nothing. `channel` 1–16 puts every message on that channel; 0
  keeps each one's own. What goes is what the part's instruments are played with: its
  notes through its zones and the controllers that reach them, or a sequencer's or MIDI
  effect's notes when one plays them; a part with no instrument sends what leaves its
  chain (with an empty chain, its own input). Each message leaves at its sample, timed to
  sound with the audio it was rendered beside. Removing the output, or a plugin, sends
  note offs for the notes it held there. `graph.get` shows each part's `midiOut`.
- **Key** ([RFC 0033](rfcs/0033-script-block.md#the-parts-scale-and-chord)): `graph.get`
  shows each part's own `key`, or null where it follows the Performance's (`harmony.get`).
- A wire a plugin replacement could not map stays with `connected: false`; port 4294967294
  is a main port awaiting one, 4294967295 none.

All of these stage edits like the other graph methods; `graph.commit` publishes them.

**Controls** form RFC 0011's control tree: `graph.get` lists them (`controls`, each with
its `value` and `targets`). Several controls on one parameter, part level or mute add up
([RFC 0011's summing rule](rfcs/0011-performance-ui-and-editors.md#the-control-tree)): each
target adds an offset to the destination's `base`, the same on all its targets (a relative
target its point in its range less a half, an absolute one its point less the base), clamped
to the range; `base` is the host's and is ignored in `controls.targets`. Setting a control sets its
targets as parameter events or part faders; a control target is set in turn, the one moved last
setting it. Assignments
that would loop are refused. Scenes store control values (`controls` in each scene).

**Scenes** are states of the Performance for sections of a song (RFC 0011's full model).
`graph.get` lists them (`scenes`), `sceneUntouched` (the parameters scenes never touch) and
`scene` (the one last selected or stored, or 0). A scene holds its `scope` (the kinds selecting
it applies) and, as stored: `off`, the plugins switched off, and `offWires`, the wires; `parts`,
each part's `level`, `pan`, `mute`, `solo`, `sends` and its splits' `lanes` Mix rows; `wires`,
user wires' Mix rows; `values` for every parameter scenes touch; and `controls`, each control's
value. Wires are named by what they join (`kind`, `part` (0 for a user wire), `from`, `to`,
`fromPort`, `toPort`, `send`, `split`, `lane`, `fader`), as a part's wires are rebuilt under new
IDs. Which control mappings are off in a scene is on each target (`offIn`, scene IDs): the
mappings off in the scene last selected that applies mappings are skipped. Kinds: `enabled`
(plugins and wires on or off), `mutes` (parts' mute and solo), `mix` (the rest of the parts' and
lanes' faders, sends and user wires' Mix rows), `parameters` (plugin parameters and controls'
values), `mappings`. Selecting a scene switches plugins off seamlessly (held notes and tails
ring out), changes route gains and sends only the parameter values that differ, so held notes
and tails go on.

Each scene also has a transition (`glide`, `curve`, `launch`). With a glide, levels, pans,
sends, Mix rows, control values and smooth parameters move there over that many milliseconds
along the curve (`ease` eases in and out; `equal-power` moves levels by their power, so a fade
does not dip); what cannot glide (plugins and wires on or off, solos, stepped parameters)
changes at the start; a part, lane or user wire muted on one side fades from or to silence.
Moving a part's fader while it glides stops the glide there. With `launch` `beat` or `bar`,
selecting it while the transport plays waits for the next beat or bar (4 beats), checked at the
engine's service tick (about 5 ms). A control target of kind `morph` moves between two scenes by
hand: the state at the control's value, what cannot glide switching at half way.

## Notifications

Create one `JsonRpc` adapter per client, sharing the same `Control`. The host polls each
adapter's `notifications(nowNanoseconds)` on the control thread with a monotonic timestamp;
the returned strings are complete JSON-RPC notifications. No subscription means no messages.
Subscriptions begin at the current change sequence, without replaying earlier history;
request initial parameter/graph snapshots explicitly. Transport and status send their current state on
the first poll. Re-subscribing does not discard already unread changes.

- `params.changed`: `params.changes` contains `sequence`, `node`, `param`, `kind` (`value`,
  `begin`, `end`), `source` (`control` or `plugin`), and `value` for value changes. Every
  accepted control value is recorded, including a client's own writes. Plugin-emitted
  parameter/gesture events and cached values changed by rescans are also recorded.
- Default parameter notifications are sent at most 60 times per second per client. Within
  each interval the latest value per parameter is retained, preserving gesture boundaries.
  Several gestures in one interval can therefore include several final values. `allChanges:
  true` on a parameter subscription returns every retained change without throttling or
  coalescing. This is a control observation stream, not a sample-timestamped automation file.
- `transport` carries the `transport.get` shape when it changes, at most 60 times per second.
- `controllers` carries the `controllers.get` shape when it changes (a control moved, a
  row button, a page), at most 60 times per second.
- `controls.changed` carries the values controls and faders play, only those that changed
  since the last one: `controls` (each `{control, value}`), `parts` (each `{part, level,
  mute}`) and, when it changed, `master` (`{level, pan}`); everything on the first after
  subscribing. At most 60 times per second, the interval counted from the last one sent, so
  a move after a pause goes out at the next poll. A part's level or the master moved live
  ([RFC 0011](rfcs/0011-performance-ui-and-editors.md#instant-controls): `parts.level`,
  `master.set`, a control or a controller) reaches clients only this way: it is no
  `graph.changed`, so a client keeping values up to date needs this topic, and reads no graph
  for a move. A mute changed live is a `graph.changed` as well (the views' wires show it).
- `status` carries the `session.status` shape when it changes, at most 4 times per second.
  `cpuLoad` is the device callback load as a fraction (1 means the entire block budget),
  not whole-machine CPU usage. `deadlineMisses` sums plugin deadline misses for nodes
  currently in the staging graph; it is not an audio-device underrun counter and may
  decrease when nodes are removed. Without device I/O, names are empty and the session
  reports its configured sample rate and maximum block size. `graphEventDrops` counts
  destination events discarded by graph merge or note-ownership overflow. If a destination
  exceeds its 512-note budget (its block's beat and harmony events have their own room), the
  graph discards that destination's entire burst and
  requests format-specific panic/reset, including sustain recovery. This can cut voices
  from other sources on that destination, but prevents a lost note-off leaving stuck notes.
  `midiEventDrops` counts physical MIDI FIFO rejections and note/pressure events discarded
  during recovery from FIFO or 128-event callback-buffer overflow. Recovery discards the
  burst and panics graph-input destinations. The next block accepts new notes normally.
- `graph.changed` carries the latest committed `revision`. Several unseen commits can be
  represented by one notification; fetch `graph.get` to inspect the staging area.
- `session.resync` with `topic: "params.changed"` signals a gap: the bounded 4,096-entry
  history wrapped, plugin control output overflowed, or pending commands were invalidated
  by a rescan. Refresh graph/parameter snapshots. `allChanges` cannot promise lossless
  recording if the client falls behind; gaps are explicit.
- The C++ `LocalClient` keeps at most 128 unread notifications. When a busy client fills
  that queue, it discards its queued notifications and inserts a local `session.resync` with
  `topic: "all"` instead of disconnecting; replies are never discarded. `RemoteControl`
  then rereads the graph, parameters, transport and status.
- `LocalClient::send` writes a request without waiting for its reply; `wait` takes that
  reply later, and `call` is the two together. The server answers a client's requests in
  order, so a request sent later is never applied before one sent earlier. A reply nobody
  waits for comes out of `takeNotifications` among the notifications, in the order it
  arrived, and counts only against the byte bound.

Each client's cursor is independent; reading one client's notifications never consumes
another's. A stopped/slow client cannot grow the history indefinitely. Subscriptions may
be changed or removed at any time.

`transport.set` validates all fields before storing them. They are independent atomic
controls, not an atomic transaction with the audio thread. `transport.get` returns the
last rendered block's playing/tempo/beat plus the requested clock source. Its fields are
individual telemetry readings, not a coherent snapshot. Commands become visible in
rendered telemetry after processing; internal play/tempo commands are retained while
following MIDI clock and take effect when switching back to internal clock.

**Clock out** ([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md#step-2-status)):
`clockOutputs` chooses MIDI outputs by device rules, matched against `session.status`'s
`midiOutputs` and resolved again as outputs come and go. While the transport plays, they get
MIDI clock, 24 a beat, each at its sample; Start when it starts from the top, else Song
Position and Continue; Stop when it stops; a jump while playing is Stop, Song Position and
Continue. Nothing is sent while stopped. Clock follows the transport, internal or external.
The outputs are the device's cabling: not saved with a Performance.

Example:

```json
{"jsonrpc":"2.0","id":7,"method":"params.set","params":{"values":[{"node":"synth","param":3,"value":0.7}]}}
```

```json
{"jsonrpc":"2.0","id":7,"result":{"values":[{"node":1,"param":3,"value":0.7}]}}
```

## Errors and limits

| Code | Meaning |
| --- | --- |
| -32700 | Invalid JSON or UTF-8 |
| -32600 | Invalid request envelope, scalar batch member or empty batch |
| -32601 | Method not implemented |
| -32602 | Invalid method parameters, including read-only values |
| -32603 | Unexpected control/encoding failure |
| -32001 | Unknown node |
| -32002 | Unknown parameter |
| -32003 | Invalid graph change, including a cycle |
| -32004 | Plugin failed to load |
| -32005 | Pending command store or UI note queue refused a command |
| -32006 | Message, nesting or batch limit exceeded |
| -32007 | Recording refused; `data.reason` says why ([Recording](#recording)) |

Input is limited to 65,536 bytes, 32 nested arrays/objects and 64 requests per batch.
The adapter checks byte/depth limits before parsing and batch size before dispatching any
member. These limit errors have a null ID and no requests are executed. Duplicate JSON
members are rejected at the envelope or method-parameter level. Numeric range, unique
addresses, duplicate members, byte limits and nesting limits require checks beyond JSON
Schema validation. The socket transport enforces the byte limit while receiving, before
accumulating a whole line.

Parsing uses a vendored, unmodified [sheredom/json.h](https://github.com/sheredom/json.h)
header under the Unlicense, with its source snapshot and SHA-256 recorded in
[upstream.json](../core/control/third_party/sheredom/upstream.json). The adapter additionally
checks input UTF-8, raw control characters and nesting depth. The build needs no JSON
package or network download, and JSON types do not appear in the public `Control` API.

## Validation

`control-json-audio` compares JSON-driven and typed-control audio sample for sample under
uneven block sizes, parameter changes and note releases, with a render allocation guard.
`control-json-protocol` uses Python's independent JSON decoder to check requests, replies,
IDs, errors, notifications, batches, limits and queue refusal/recovery. Python is optional
for the build; CI environments include it. `control-flow` additionally checks retained final
values, ordered gestures, independent subscribers, explicit history gaps and staged graph edits without interrupting held notes.
These tests are included in the existing ThreadSanitizer job.

## Local transport and client

Link `oscairn_control_socket` on Linux or macOS. `LocalServer` and `LocalClient` use
Unix-domain `SOCK_STREAM`, with one compact JSON document followed by LF. Escaped newlines
inside strings are fine; pretty-printed documents containing literal LF are not frames.
CR before LF is accepted as JSON whitespace. Partial reads/writes and several documents
per read are supported. Each server connection has its own `JsonRpc` subscriptions.
There is no WebSocket, gRPC or TCP listener.

The socket path must be absolute and fit the platform's `sockaddr_un`. Its parent must
be a real directory owned by the current user with no group/other permissions (0700 is
recommended). The endpoint is 0600. The server refuses any existing path, including a
stale socket. Stop the owning process and remove a stale endpoint explicitly before
restarting. Orderly exit unlinks only the socket inode created by this server. This is
a same-user control interface, including native plugin loading; it is not a sandbox for
untrusted clients. `adopt(fd)` takes ownership of an already connected stream, allowing
socket activation and anonymous-stream tests without a pathname listener.

The control thread calls `LocalServer::poll(monotonicNanoseconds)` regularly; the method
never waits for socket readiness. There are at most 8 clients, 16 documents dispatched
per client per tick, roughly 64 KiB read/written per client per tick, and 1 MiB queued
output per client. A document may contain up to 64 batch members. Method execution
(including plugin loading and graph commits) remains synchronous on the control thread.
Oversized frames, I/O failures and full output queues disconnect that client. Malformed
JSON within a bounded frame receives an RPC error and does not discard later frames.
A write-half-close drains complete requests and responses before closing. Each `poll()`
dispatches every client's requests first, then sends each client its notifications, so what
one client changes reaches the others in the same poll.

`LocalServer::watch(wake)` answers requests as they arrive rather than at the next poll: a
thread of the server's own waits on the listener and the clients (and on room to write a reply
a full socket held back) and calls `wake` when one is ready, once, then waits for the next
`poll()` before it looks again, so nothing spins while the control thread catches up. `wake`
must get the control thread to call `poll()` (it is called on the watcher's thread); requests
left over a poll's share call it again at once. The engine's main thread polls when woken and
after each service tick (about 5 ms), which sends what changed then; it has no socket timer of
its own. `engine-pump` serves a session exactly so, without an audio device: requests are
answered in about 0.03 ms (median; 0.1 ms worst) rather than 3 ms (6 ms worst) on the 5 ms
timer the engine used before, a change another client made is heard in under 0.1 ms, and
the process stays at about 1% CPU idle.

`LocalClient::call` waits for its matching response while retaining interleaved
notifications. The default deadline is 1 second. Timeout, malformed response, mismatched
ID or disconnection closes the connection. A mutation may already have applied when
its response is lost: refresh state after reconnecting, never replay it automatically.
IDs continue increasing across reconnects. The client bounds received messages to 128
queued documents and 1 MiB; overflow disconnects rather than silently losing updates.
`poll()` receives without waiting; `takeNotifications()` drains retained notifications.
Call these APIs from one UI/control thread, never from audio or MIDI callbacks.

`RemoteControl` checks protocol version 1, subscribes, and reads graph, parameter,
transport and host-status snapshots, sending the requests after the hello at once. Most getters use that cache; plugin text formatting
and mutations use bounded synchronous RPC calls, apart from the live edits below. Call `service()` regularly to update
snapshots and deliver changes. Its change sequence is local to that adapter's lifetime,
not the server's sequence. Graph commits and explicit history gaps trigger a snapshot
refresh. A refresh is `graph.get`, then every node's `params.list`, then the values
(`params.get`), with several requests out at once (at most 16, fewer while the longest
`params.list` reply seen would overfill the server's 1 MiB queue), so it takes a few of the
server's ticks rather than one per node. One that `service()` starts (a `graph.changed`, a
gap) waits on nothing: `service()` takes its replies as they come and returns true once the
cache shows it, keeping the values pushed while it was out (`controls.changed`,
`params.changed`) and the live edits answered meanwhile (`parts.level`, `master.set`, unless
refused or pushed since) over the older ones it read. An edit's own refresh waits, so `graphState()` is current when the edit
returns. Snapshots span several requests and are not atomic across concurrent clients;
interleaved notifications are retained to bring the cache up to date. After a gap the
transport, status and controller snapshots are read the same way, without waiting. The
`fetch...` methods (meters, wire meters, undo names, assignments, takes, the open
Performance, the audition, a plugin window's availability, the pad map, a parameter's text for
a value, one out per parameter) send a read without waiting,
unless the same read is already out, and `service()` hands the reply to a callback;
`forgetFetches()` drops those out, so a read asked afterwards reflects what came before it.
`setParameter` is sent without waiting too: it checks the address and that the parameter is
writable at once (throwing as the engine would), shows the value in the cache clamped and, if
stepped, truncated, and returns true; the newest value sent for a parameter stays over the
engine's `params.changed` until its reply, which gives the value the engine kept. A refusal
(the pending command store full) comes through `takeError()`, and the graph and values are
read again. Recreate the adapter on reconnect. No commands are implicitly retried.

The desktop's `--connect SOCKET` mode uses this adapter, polls every 16 ms, and retries
failed connections about once a second. It displays disconnection status and refreshes
snapshots when reconnected. Its views' polls and the graph read after an engine change go
without waiting and arrive at its timer; edits and other calls still run on the UI thread
and may pause the UI until their deadline if the engine is unresponsive. Audio and physical MIDI remain in
the helper. See [host instructions](../hosts/README.md) for launching the two processes.
The normal desktop launch now starts or reconnects to a managed helper. Closing the UI
leaves the helper alive. A stopped managed helper enables a Restart engine button; this
starts a fresh demo session, without restoring or replaying the old session. Process
locks serialize startup and guard stale-socket cleanup. See the host instructions for
paths, timeouts and failure handling. `--in-process` remains an explicit alternative.

`control-local-stream` uses real anonymous Unix streams to exercise framing, partial
messages, backpressure, multiple clients, subscriptions, typed remote controls, uncertain
outcomes and reconnects. It also kills a separate controller process with SIGKILL, verifies
rendering continues without allocations, then reconnects and controls the existing state.
`control-local-path-policy` checks path validation, private-directory enforcement and
existing-file/symlink preservation without creating a listener. `control-local-path`
covers bind/connect, permissions, safe endpoint cleanup and a controller process
crashing/reconnecting through a named socket. It verifies ordinary file writes before
binding and skips with code 77 only for `LocalServer::BindError` with EPERM/EACCES; errors
in directory access, chmod, listen or later operations fail normally. CI sets
`OSCAIRN_REQUIRE_SOCKET_BIND=1` so that denial fails the test. These tests do not replace
a live-device listening test or a crash/reopen test of the actual desktop GUI.

## Prepared sounds

`sounds.get`, `sounds.prepare`, `sounds.select` and `sounds.discard` operate on the control
thread, independently of staged graph edits. `sounds.get.mode` identifies the mechanism:
`native` for a plugin implementing [RFC 0005](rfcs/0005-sound-contexts.md), `instances` for
an ordinary CLAP/LV2 instrument managed by the graph, or `unsupported` (capacity zero).
These operations never restore state into the outgoing playing instance.

| Method | Parameters | Result |
| --- | --- | --- |
| `sounds.get` | `node` | `mode`, `capacity`, `contexts` |
| `sounds.prepare` | `node`, `name`, optional `parameters: [{id, value}, ...]`, `state`, `preset`, `sfz`, `directory` | `context` ID |
| `sounds.select` | `node`, `context`, optional `channel` (-1 for all, or 0..15) | `accepted: true` |
| `sounds.discard` | `node`, `context`, optional `cutTail` (default false) | `accepted: true` |

Each context reports `id`, `name`, `selectedChannels` (16-bit mask), `activeVoices` and
`idle`. Fields are independent telemetry snapshots. Selection takes effect at a processing
boundary before that block's notes. Held notes and per-note expression retain their owner;
sustain follows the channel across switches. `accepted` means queued, not applied: poll
`sounds.get` for selection masks. There is no context notification topic yet.

`sfz` goes only to native engines that define patch text, together with the asset `library`
holding its files and the patch's `directory` inside that library (empty for the root): the
Oscairn Sampler takes an SFZ patch whose `sample=` paths are relative to that folder and may
use `../` without leaving the library. The engine resolves and reads those files on its
control thread; a rejected patch returns `-32005` with the plugin's diagnostic, such as
`line 2: unsupported opcode pan`. The whole request must fit the 64 KiB message limit.

### Sound assets

Sound assets are files that sounds use without copying them into a Performance
([RFC 0009](rfcs/0009-sound-assets-and-dependencies.md)). The engine's store lives in
`OSCAIRN_DATA_HOME/assets`, else `~/Library/Application Support/Oscairn/assets` (macOS) or
`$XDG_DATA_HOME/oscairn/assets`/`~/.local/share/oscairn/assets` (Linux).

| Method | Parameters | Result |
| --- | --- | --- |
| `assets.register` | `folder`: absolute path on the engine's filesystem | `library`, `directory`: the library containing the folder (registering the folder as a new library when none does) and the folder's path in it |
| `assets.import` | `file`: absolute path | `library` (`user`), `path`, `sha256`: a copy in the user library |
| `assets.libraries` | none | `libraries: [{id, name, path, files}]` |

Registering hashes every file in the folder once, so it can take seconds for a large library;
`RemoteControl` waits up to 10 minutes. Later lookups re-hash only files whose size or
modification time changed. A reference with a known hash is also satisfied by identical bytes
in any other library. Failures return `-32005` with the reason.

A parameter patch starts from plugin defaults plus up to 128 numeric parameters. Omitted/empty
parameters select defaults. Names have at most 63 UTF-8 bytes (hosted names must be nonempty).
Unknown, duplicate, nonfinite, out-of-range or read-only parameters reject preparation.
For `instances`, `state` is an existing state-bundle directory on the **engine's** filesystem;
`preset` is a URI from `presets.list` (LV2, or a `clap-preset:` URI loaded with CLAP
`preset-load`). At most one may be nonempty. The host loads it on a fresh
instance and applies numeric overrides afterward. It rejects changes to port layout,
note dialects or parameter metadata. Failed preparation leaves existing sounds unchanged.
Native contexts accept numeric, SFZ or preset patches, never state. A native instrument with
[RFC 0005 draft 5](rfcs/0005-sound-contexts.md#presets)'s `prepare_preset` prepares a
`clap-preset:` URI from `presets.list` as the whole patch: `parameters`, `sfz` or `state` beside
it are invalid (`-32602`), and so is a preset for a native instrument without it. A preset the
plugin refuses returns `-32005` with its diagnostic, such as `Preset not prepared: no such
preset`. `session.hello` lists the `sounds.presets` capability where native instruments take presets. Preparation is synchronous on the control thread and may take time for samples;
it never blocks the audio thread, but other control requests wait for it to finish.

The instance pool supports instruments with 1..16 note input ports, audio output, no audio
input and no note/MIDI output. Effects and event-producing instruments are not pooled.
Eight slots include protected Default ID 0. Multiple channels can select one slot or
select independent slots. A global parameter edit affects unique currently requested
selected instances; an explicit per-note edit follows note ownership. Parameter getters,
text and property methods show the channel-0 selection. Control-thread parameter writes
target the selection requested when they are issued, even if audio has not adopted it yet;
new selection requests can supersede an unapplied request. Direct graph events use the
selection applied at their processing boundary. Keep these distinctions in part editors.

Outgoing instances continue rendering and summing their outputs, including release and
internal effect tails. No silence threshold or timer proves an arbitrary plugin tail ended.
For hosted MIDI instruments, `activeVoices` counts owned held/sustained notes; it cannot
measure private DSP voices. CLAP notes remain owned until `NOTE_END`. Processed hosted
instances stay `idle: false`, even if their current output is silent. `discard` without
`cutTail` only frees never-processed, unselected and unqueued hosted slots. `cutTail: true`
also permits previously processed slots after their note ownership reaches zero; it can cut remaining
audio tails. It cannot override selected, pending or note-owned pins. Native plugins keep
their own stricter idle/tail contract regardless of `cutTail`. ID 0 is protected in both
implementations. Disposal occurs on the control thread after audio releases its old plan.

Budget CPU and memory for retained instances; the pool does not share plugin-private
sample memory or stop outgoing DSP based on quiet output. It sums without limiting or
crossfading. MIDI-only plugins retain MIDI 1.0 limits: a targeted choke can require channel
all-sound-off and end other notes in that same instance/channel.

Preparation returns `-32005` for capacity exhaustion or state/preset failure. Selection or
discard returns `-32005` for unknown/busy contexts or exhausted native selection queues.
Invalid arguments or unsupported contexts return `-32602`. Context IDs are never reused
within a node's lifetime and become invalid when it is unloaded. `sounds.get` is telemetry,
not a reservation: always handle a subsequent refused discard.

```json
{"jsonrpc":"2.0","id":1,"method":"sounds.prepare","params":{"node":"synth","name":"Dark","parameters":[{"id":1,"value":0.1},{"id":2,"value":0.6}]}}
{"jsonrpc":"2.0","id":2,"method":"sounds.select","params":{"node":"synth","context":1}}
```

Use the actual returned ID, rather than assuming 1. For a pooled instrument, preparation
from a saved state is `{"node":"instrument","name":"Saved","state":"/absolute/state-bundle"}`.
The native plugin extension and hosted instance pool are separate mechanisms with the
same control entry points. State export, preset enumeration, complete project/pool
persistence, effect transitions and a desktop sound picker remain follow-up work.

## State export, presets and patch properties

These methods are available through typed `Control`, `Session`, JSON-RPC and `RemoteControl`.
Paths refer to the **engine's filesystem**. `state.save` creates a new portable directory;
it refuses an existing destination and includes pending parameter edits, or reports failure
if they cannot be delivered. It exports the channel-0 selected instance for pooled
instruments. This is one plugin snapshot, not project/pool persistence. `state.info` may be
false even when native numeric sound contexts work (currently Basic Synth). Preset enumeration
lists LV2 bundle presets and the presets a CLAP plugin publishes through preset discovery, as
`clap-preset:KIND:LOCATION#LOAD_KEY` URIs (parts percent-encoded), within the limits in
[RFC 0005](rfcs/0005-sound-contexts.md#presets) (at most 4096); each call reads the plugin's
locations again. An empty list does not imply a plugin cannot load state.
Use `sounds.prepare` with `state` or `preset`, then `sounds.select`, for eligible instruments.
State export does not add an in-place restore command for effects.

Property values carry their exact type: `bool`, signed 32-bit `int`, signed 64-bit `long`,
`float`, `double`, `string` or `path`. A `long` uses a decimal **string**, for example
`{"type":"long","value":"9223372036854775807"}`, so JavaScript clients do not round it.
`int` uses an integer JSON token; floating values must be finite and representable. Strings
and paths reject NUL and exceedance of 4096 UTF-8 bytes; the plugin adapter's bounded atom
packet can impose a smaller limit. A path is distinct from a string and must satisfy the
adapter's existing asset checks. `unknown` is metadata only and cannot be sent as a value.

`properties.get` returns only the last plugin-confirmed value, or null before confirmation;
it does not send a request or claim an accepted write succeeded. `properties.request` sends
an asynchronous refresh only for a declared readable property. A plugin may publish a
writable property's value without declaring it readable. Poll `properties.get` after normal
engine service to observe confirmation; no property subscription exists yet. Unknown URIs,
malformed typed values and unsupported access receive `-32602`; a rejected write/refresh
(type mismatch, size, capability, asset or full queue) receives `-32005`.

State export/preparation can perform filesystem I/O and background-work draining on the
control thread. `RemoteControl` waits up to 30 seconds for `state.save` and `sounds.prepare`;
other calls retain their normal timeout. A timeout is an **unknown outcome**, not cancellation:
inspect the destination/context inventory after reconnecting instead of blindly retrying.
An LV2 state export may briefly silence that instance while taking its snapshot; preparing
a spare sound avoids restoring over a held instance. These operations are for preparation,
not sample-timed performance changes.

### Validated plugin replacement

`graph.replace` takes `{node, path, pluginId, preset?}` and returns `{revision}`.
`pluginId` is required and nonempty (CLAP ID or LV2 URI); `path` is the engine-local
installation location, not portable identity. An optional LV2 preset URI is loaded on
an isolated candidate before adoption. No separate `graph.commit` is required.

Replacement refuses staged graph edits or parameter mappings touching the node. It loads
and activates the candidate, moves the node's wires to its ports in the same role (main to
main, other ports in order) and compiles them with the channel rules into one published
plan. Wires that no longer fit stay, with `connected: false`, instead of the replacement
being refused; a main-port wire reconnects when a later replacement has a main port.
Any preparation failure leaves the playing graph, node identity and revision unchanged.
Success retains the node ID/name and its wires, clears old pending parameter commands, and
publishes `graph.changed` so clients rebuild metadata.

This intentionally stops the replaced node's current voices/tails and discards its prepared
sounds; use `sounds.select` for seamless sound changes within an engine. Native plugin
code still runs in the audio-engine process after loading: scanner isolation protects
scanning, not a plugin that crashes or hangs during instantiation/processing.

## Native effect contexts

`effects.create` accepts `source` (existing node name/ID) and optional unique `name`,
returning a new numeric `node`. It stages a separately addressable logical effect sharing
that native engine. Use `graph.connect` and `graph.commit` normally; parameters, state and
serial/parallel ordering use the ordinary node APIs. New contexts start with defaults,
not a copy of the source patch. Unsupported plugins and exhausted engines return -32004.

`effects.resources` accepts `node` and returns `null` for unsupported plugins, or
`{sharedBytes, contextBytes, capacity, prepared, tailActive}`. Shared bytes count once per
engine; context bytes count per logical effect. These plugin-owned figures exclude host
and allocator overhead. `tailActive` is an independent telemetry snapshot, not permission
to free a context still referenced by a render plan. Delay supports 16 contexts; preparation
failure leaves existing audio intact. Removing the originating node does not remove siblings.

To preserve a Delay tail, keep its output routed, disconnect its input after old notes
finish, and continue rendering until `tailActive` is false. Then remove/commit. Removing
a node intentionally cuts its tail. Graph ownership defers destruction until prior plans
retire. Complete Performance persistence and automatic chain retirement remain planned.

For Basic Synth, prepare patches with parameter 8 = output bus (0 = Main, 1–8 = separate
stereo ports with matching IDs). Voices capture the bus at note-on through sustain/release;
changing sounds or Output bus cannot move held notes. Routed voices are excluded from Main.
Buses are permanent destinations, not sound-context IDs; assigning two sounds to one bus
intentionally combines them before the effects. Allocate different buses for different chains.

## Performance persistence

`performance.save` and `performance.load` take `{"directory":"/path/to/performance"}`;
returns are `{"saved":true}` and `{"loaded":true}`. Both are advertised by `session.hello`.
Files use the [Performance version 8 format/schema](performance-format.md). Saving refuses
existing directories and staged graph edits. Loading validates a complete replacement;
a load failure leaves the playing graph intact. Successful publication emits a graph resync,
invalidates old node/context references, and starts stopped at beat zero on adoption. The
change is seamless: the previous Performance's plugins ring out (held notes until released,
tails until idle) and are hidden from `graph.get` meanwhile.
`performance.prepare` takes the same `{"directory":...}` and returns `{"prepared":true}`: the
Performance is read and its plugins loaded with their state while the open one plays, so the
next `performance.load` (or `library.load`) of that folder only publishes it. It returns
`{"prepared":false}` without loading anything when it would not fit the budget beside what plays
now (budgets below); its load is then a cut. One Performance is prepared at a time: preparing
another, loading a different one, or an empty `directory` lets it go; preparing the one already
prepared, unchanged since, keeps it and returns as it did. A folder saved over
since it was prepared is read again at the load. Preparing takes as long as a load and blocks
other control calls meanwhile; only the load gets faster.
The current note/tail audio is not saved. Resource/state/format failures use `-32005`;
malformed RPC arguments use `-32602`. These control-thread calls can take time; remote
wrappers allow 30 seconds, and callers must reread the graph after a timeout before retrying
because a disconnected request is not cancellation. The app runs them on its control worker.

## Recording

[RFC 0020](rfcs/0020-recording.md) steps 1 to 3 and step 4's control API. The engine records taps into a take: one
file per tap, all starting and ending on the same sample, in a folder of its own under the
data folder's `recordings/` (`OSCAIRN_DATA_HOME/recordings`, beside `assets`), named by its
local start time and the Performance or the given name: `2026-11-01 21-04-17 Open Road/`.

- `record.start` takes optional `taps` and `name`, and returns `{take}`. Each tap is
  `{kind}` with:
  - `output`, `port`: a graph output port, as heard; `input`, `port`: a graph input port;
  - `part`, `part`, optional `fader` (`post`, the default, or `pre`): the part's sound summed
    from the wires to where it plays, after or before its level, pan, mute and solo, in that
    destination's channels;
  - `node`, `node`, optional `port`: one of a plugin's audio output ports (its main one when
    `port` is absent and it has no port 0);
  - `notes`: the events entering the graph; with `part`, those the part's first plugins
    receive; with `node`, those a plugin sends.

  Without `taps`, Main (the first output port) and the notes entering the graph. Audio taps
  are 32-bit float WAV files named after the port, part or plugin (`Main.wav`, `Lead.wav`,
  `Lead pre-fader.wav`); note taps are tracks of one Standard MIDI File, `Notes.mid` (type 1,
  960 ticks per quarter, track 0 the tempo map). Optional `from`: `"now"` (the default: the next
  block), `{"secondsAgo":s}` (0 to 3600) or `{"bar":"next"}` / `{"bar":"last"}` (the transport's
  bar line, exact with the internal clock; with the transport stopped, now); optional `until`:
  `"stop"` (the default), `{"bars":n}` or `{"seconds":s}`; optional `keepGoing`. A start in the
  past reads the retroactive buffer (`record.retro`) and ends now unless `keepGoing`; when the
  buffer holds less, the take starts where it still holds it, less up to 2 seconds. One take
  records until stopped at a time; a take with an end (a capture, a length) records beside it.
  Refusals are `-32007` with `data.reason`: `unknownTap`, `retro` (a start in the past on a tap
  the buffer does not hold), `busy` (a take is recording) or `drive` (the folder could not be
  made).
- `record.retro` takes `seconds` (0 to 3600; 0 turns it off) and optional `taps` (as for
  `record.start`; without them, Main and the notes entering the graph) and returns
  `{"accepted":true}`: those taps kept in memory for the last `seconds`, never written unless a
  take asks. Its memory is committed memory in the budget (RFC 0025); past the budget it is
  `-32007` `memory` and the buffer there is kept; `busy` while a take recording reads it.
- `record.stop` takes `{take}` and returns `{"accepted":true}`; the take ends at the next block
  and its files close after. `-32602` for a take that is not recording.
- `record.takes` (`{}`) returns `{takes}`, this engine run's takes and the closed ones found in
  `recordings/` (numbered as found), oldest first by folder name, each
  `{take,state,folder,files,start,frames,sampleRate,overruns,droppedNotes,behind,playing,error}`.
  `state` is `waiting` (for its start), `recording`, `finishing`, `done` or `failed` (a found
  take left open is `failed`, `not closed`); `behind` the writer more than half a ring behind;
  `playing` being played back; `start` the first sample on the engine's
  timeline; `frames` those written to each file so far; `overruns` each span the writer fell a
  whole ring behind, `{start,frames}` from the take's start, written as silence so the files
  stay aligned; `droppedNotes` the events lapped in the notes ring or that have no MIDI
  form; `error` why a take failed (`diskFull` when the drive came within 64 MiB of full).
- `record.get` (`{}`) returns `{recording,retro,takes}`: the take recording, 0 for none, the
  retroactive buffer's `{seconds}`, and the same takes.
- `record.delete` (`{take}`), `record.rename` (`{take,name}`: its folder renamed, keeping the time
  it starts with) and `record.play` (`{take}`: its first WAV file played into the outputs once,
  after the taps so no take records it; 0 stops) return `{"accepted":true}` for a closed take.
  `-32602` for no such take or an empty name, `-32007` `busy` while it is written, `memory` when a
  playback does not fit the budget.
- The `record` topic sends `record` with `{takes}`, at most 10 times a second: the takes being
  written and each whose state changed since the last, in the `record.takes` shape.

Each take folder also holds `take.json`: its sample rate, start, frames, tempo and beat at the
start, files, overruns, dropped notes and the first note's exact sample. Written when the take
starts, with `state` `recording`, and again when it closes. The typed methods are
`Control::recordStart`, `recordStop`, `recordTakes`, `recordRetro`, `recordRetroSeconds`,
`recordDelete`, `recordRename` and `recordPlay`.

## Library

The library ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)) is the Performances
and Sounds in the data folder's `Performances/` and `Sounds/` (`OSCAIRN_DATA_HOME`, else
Application Support on macOS, else `XDG_DATA_HOME/oscairn` or `~/.local/share/oscairn`). Each
file's `info` block is the truth; `library/index.json` caches what was read, by each file's
modification time, so a rescan reads only changed files and deleting the cache loses nothing.
Favorites and recently used (at most 50) are the user's, in `library/user.json`. Files the
library cannot read, and hidden folders, are skipped; of two files with one ID the first found
is listed.

- `performance.info` (`{}`) returns the open Performance's `{id,name,description,tags,color}`
  (`color` is `#rrggbb` or empty); the ID is empty until the first save.
  `performance.setInfo` takes `{name,description,tags}` and an optional `color` (empty when
  absent; the ID is kept) and returns the same. These are not undoable edits.
- `library.query` takes optional `text` (case-insensitive, over name, description and tags),
  `tags` (all must match), `favorites`, `recent`, `kind` (`performance` or `sound`; both when
  absent), `source` (`mine` for the user's own items, or an installed library's ID), `id` (that
  item only) and `limit` (1–1000, default 200). It returns `{items,tags,total}`: items with `id`,
  `kind`, `name`, `description`, `path`, `tags`, `favorite`, `recent` (1 is the most recent,
  0 none) and `color`, sorted by name or, with `recent`, most recent first; `tags` counts each
  tag over every match but those the user hid, and `total` counts matches before the limit;
  `tagColors` lists the colors the user gave tags (`{tag,color}`) and `hiddenTags` every hidden one.
- `library.tag` takes `{tag}` and optional `hidden` and `color` (`#rrggbb`; absent or empty for
  none) and sets both: a hidden tag leaves the tag list but still matches a search and a chosen
  tag filter. These are the user's, in `library/user.json` beside favorites, never in an item.
- `library.favorite` takes `{id,favorite}`; `library.rescan` (`{}`) returns `{"items":n}`.
- `library.edit` takes `{id,name,description,tags}` and an optional `color`, as
  `performance.setInfo` does, and writes them into the item's file; a locked library's items
  refuse. If the open Performance is that item, its info changes too.
- `library.copy` takes `{id}` and returns `{"id":…}`: a new item of the user's own with the same
  contents under a new ID, named "<name> copy" when copying one of theirs. A locked library's
  item keeps its name, and its copy records where it came from, as installing it would.
- `library.save` takes an optional `name` and `new`, and returns `{"id":…}`. Save (`new`
  false or absent) writes a Performance opened from the library over its folder (saved
  beside it, then swapped), renamed when `name` is given; Save As New (`new: true`) gives it
  a new ID. Either way, one not in the library goes to a new folder named after it.
  `library.load` takes `{id}`. Both mark the item recently used.

### Sounds

A Sound is a part saved without its seat ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)):
its chain, splits and lanes, its plugins with their state, the wires with both ends inside it,
note-to-parameter mappings inside it, and the controls it owns. Not its input, output, sends,
fader or name in the Performance, nor wires to or from the rest of the graph.

- `parts.saveSound` takes `{part}` and optional `name` and `new`, and returns
  `{id,revision,left}`: the Sound's library ID, the revision written, and a description of
  each user wire that crosses the part's edge and was not saved. Save (`new` false or absent)
  writes the next revision of the Sound the part came from, over its folder, renamed when
  `name` is given; Save As New (`new: true`), or a part from no Sound or one no longer in the
  library, makes a new Sound in a new folder, named `name` or after the part. The part then
  comes from what was saved, unedited. Saving is not an edit and is refused while graph edits
  are staged.
- `parts.loadSound` takes `{part,id}`: the part's chain is replaced with the Sound's, and the
  part takes the Sound's name. The part keeps its input, output, sends, fader and solo. Its
  old plugins go, with every wire, mapping and scene entry on them. Its own
  controls are replaced by the Sound's; a Performance control that targeted one of them moves
  to the Sound's control of the same name, else that target is removed. It returns
  `{"accepted":true,"flagged":[…]}`, describing each wire and control target removed. Notes
  playing in the part are cut: loading a Sound is editing (seamless changes are for scenes and
  Performances, [RFC 0021](rfcs/0021-sounds-performances-and-library.md#seamless-switching)).
  It is one step to undo, and refused while graph edits are staged. Loading it again when the library has a newer revision is Update from library.
- Each part in `graph.get` has `from`: `{sound,revision,name,modified}` or `null`.
  `modified` turns true when an undoable step changes the part's chain, splits, plugins, the
  wires and mappings inside it or its own controls, or a parameter of one of its plugins (as
  a graph change, so clients refresh); undoing a model step clears it again. Preset and state
  loads, which are not undoable steps yet, do not set it. Its seat (input, output, sends,
  fader) is not the Sound's and does not. `library.query` items carry their `revision` and content `hash` (as recorded at their last save).
- `performance.contents` takes `{directory}` and returns `{parts,scenes}`: each part of that
  Performance folder with its `id` there, `name` and `bus`, and each scene's `id` and `name`.
  `performance.import` takes `{directory}` and optional `parts` and `scenes` (IDs from
  `performance.contents`; both absent or empty for all of it; otherwise at least one part,
  and one scene when it has any) and returns `{parts,notes}`: the new parts' IDs, and each rename or remapped port. What comes,
  and how scenes and names are handled, is in
  [RFC 0021](rfcs/0021-sounds-performances-and-library.md#importing-a-performance). One step
  to undo; refused while graph edits are staged.
- `budget.set` takes `{cpu}` and optional `memory`: the device's budget ([RFC 0025](rfcs/0025-resource-budgets.md)),
  `cpu` the share of the audio thread's time the plugins may take (0 checks nothing), `memory` in
  bytes (0 or absent: not checked). `budget.estimate` takes an optional `{directory}` (absent: the
  open Performance) and returns `{cpu,memory,fits,seamless,budget,plugins}`, each plugin with
  `name`, `plugin`, `cpu`, `memory` and `measured`. For the open Performance, `fits` counts it
  alone and `seamless` adds the last Performance still ringing out. With a budget, opening a Performance over it,
  or adding a plugin that would take the open one over it, fails saying what does not fit; one
  that fits only alone opens as a cut, without the last ringing out. A plugin whose
  `oscairn.memory` declaration exceeds what the memory budget leaves is refused before it activates.
  `budget.set` also takes optional memory classes in bytes ([RFC 0025](rfcs/0025-resource-budgets.md#memory-reserved-committed-and-opportunistic)):
  `device`, `system` (the OS, the engine's base and the shell), `floor` (the Performances' floor)
  and `margin`; with `device` the memory ceiling is the committed share (device less `system`,
  the apps' reservations and the margin), and `budget.estimate`'s `budget.memory` reports it.
  With them, a Performance that cannot be prepared beside the open one is still prepared, as
  opportunistic data, when memory allows (`performance.prepare` still returns `{"prepared":false}`).
- `memory.alwaysAvailable` takes `{apps:[{app,reserve}]}`: the apps reserved whether they run or
  not; refused (-32005) when the apps added do not fit beside the floor. `memory.launch` takes
  `{app,reserve}` and optional `dropNext`, asked before an app starts, and returns
  `{admitted,offerNext,reason}`: refused with what closing would free, never closing a running
  app or letting committed memory go; `offerNext` when only the committed next entry stands in
  the way, and `dropNext` then lets it go and admits. `memory.closed` takes `{app}`.
  `memory.state` (`{}`) returns `{device,reserved,share,floor,margin,committed,opportunistic,
  running,alwaysAvailable,nextPrepared,nextCommitted}`. Apps run in the desktop app today, so a
  reservation is counted, not enforced per app.
- `performance.save` and `library.save` both keep each part's `from`.
- `performance.export` takes `{file}` and optional `editedSounds`, and returns
  `{"exported":true,"notes":[...]}`: one [archive](library-archive.md) holding the open
  Performance, every sample its plugins use, every build there is of each CLAP plugin they use,
  and the library Sound of each part still that Sound (with `editedSounds`, also each edited
  part's). `notes` says what was left out: an LV2 plugin (installed on each device, never carried), or one not on this device. It refuses
  a file that exists. Each plugin build carries its publishers' signatures, from `<bundle>.sig`
  beside it or in the store (one that is not over that build is left out and noted). Optional
  `sign`, `{key,items,archive}`, signs with the signing key named (`keys.list`): each item
  with `items`, the whole archive with `archive` ([signatures](library-archive.md#signatures)).
  Nothing is signed unless asked.
- `audition.start` takes `{id}` and, for a Sound, the `part` to try it in; `audition.stop` takes
  an optional `keep`; both return `{"accepted":true}`. `audition.get` (`{}`) returns `{id,kind,part}`
  (`id` empty when none). A Sound plays in the part's place, its chain set aside; a Performance
  opens in place of the open one, which stays loaded and silent while its held notes and tails
  ring out. Stopping puts back what was there, the same plugins with their state, and the tried
  plugins ring out; `keep` makes the tried item the change (a Sound's load is then one step to
  undo). Edits made meanwhile go with the tried item. Starting another stops the one before;
  opening a Performance keeps it. With a budget set, a Performance that would not fit beside the
  open one is refused.
- `library.export` takes `{id,file}` and optional `editedSounds` and `sign`, and returns the same: a
  library item as it is saved, without opening it. A Performance comes with its parts' Sounds
  by the same rule (from what its file records), a Sound alone; with every sample and plugin they
use.

### Libraries

Libraries come as [archives](library-archive.md): a library, or an exported Performance.

- `library.contents` takes `{file}` and returns `{id,name,kind,items,code,signer}`, each item's `kind`,
  `id` and `name`. `code` is each plugin build it would add to the store (not one already
  there), and `signer` the whole archive's, each `{kind,name,version,platform,status,key,keyName}`
  ([RFC 0006](rfcs/0006-packages-and-portability.md#signatures)): `kind` `plugin` (`app` once
  archives carry apps) or `archive`; `status` `trusted` (signed by a key the trust store trusts
  for it: `plugins`, `apps`, or `content` for the archive), `unknown` (signed by no such
  key), `unsigned` or `revoked`; `key` the deciding key's ID (empty when unsigned) and `keyName`
  its trust store name. An archive with a signature that does not verify is refused.
- `library.install` takes `{file}` and optional `asLibrary`, `items` (IDs; absent or empty
  for all) and `acceptUnsignedCode`, and returns `{added,skipped,notes,code}`. When a plugin
  build the items use, not already in the store, is unsigned or by an unknown key, the install is
  refused, naming each, unless `acceptUnsignedCode` is true: the user saw `library.contents`'
  `code` and said yes. A build signed by a revoked key is never installed (noted; what uses it
  opens with a stand-in where it is not installed otherwise). Sounds, Performances and samples
  install whether signed or not. `code` is the signer of each build it judged. As a library its items are locked and keep
  their IDs, and installing again adds only what is missing; otherwise they are copied into the
  user's own storage under new IDs, and installing again skips copies still as imported. The
  samples they use go into the user's sample library, and the builds of the plugins they use,
  every platform's, into the plugin store; `notes` names any sample that is in neither the
  archive nor the device, a plugin with no build for this device, and a different build of one
  already installed (kept).
- `library.libraries` (`{}`) returns `{libraries}`, each `{id,name,items}`.
- `plugins.list` (`{}`) returns `{plugins}`: the plugin store ([RFC 0006](rfcs/0006-packages-and-portability.md)),
  each `{format,id,version,name,platforms,bytes,usedBy,open}`: the platforms it has builds for,
  their size on disk, the names of the library items that use it, and whether the open
  Performance plays this device's build from the store.
- `plugins.remove` takes `{format,id,version}` and optional `platform` (absent or empty: every
  build) and returns `{"accepted":true}`. A `platform` is one of those `plugins.addBuild` takes.
  It is refused while the open Performance plays the build; items that use it open with a stand-in until it is installed again.
- `plugins.addBuild` takes `{format,id,version,platform,path}`: another platform's build (one
  of `linux-arm64`, `linux-x86_64`, `macos-arm64`, `macos-x86_64`, `windows-x86_64`, not this
  device's) from a file or bundle here, into the store so exports carry it, replacing that
  platform's build. CLAP only. It cannot be loaded here; the device it is for checks its ID and
  version when it loads it.
- `trust.list` (`{}`) returns `{keys}`: the trust store ([RFC 0006](rfcs/0006-packages-and-portability.md#signatures)),
  `trust.json` in the data folder, each `{id,key,name,scope,revoked}`: the key's ID (the first
  16 hex digits of the SHA-256 of its 32 bytes, its fingerprint), its Ed25519 public key in hex,
  its name, what it is trusted for (any of `plugins`, `apps` and `content`) and whether it is
  revoked. `trust.set` takes `{key,name,scope}` and optional `revoked`, adding the key or
  replacing its entry; `trust.remove` takes `{id}`. Both return `{"accepted":true}`.
- `keys.list` (`{}`) returns `{keys}`, the signing keys in the data folder's `Keys/`, each
  `{name,id,key}`: its name, ID and public key; a private key is never sent. `keys.make` takes
  `{name}` (letters, digits, `.`, `-` and `_`, at most 64, not starting with `.`) and
  returns the new key's `{name,id,key}`; its private key is written to `Keys/<name>.key`,
  readable only by the user. A name already used is refused.
- `library.uninstall` takes `{library}`; `library.delete` takes `{id}`, any item's.
- `library.query` items carry `library`: the installed library holding them, locked, or empty
  for the user's own. Saving over a locked item, a Performance by `library.save` or a part's
  Sound by `parts.saveSound`, makes a new one.
- `parts.fromSound` takes `{id}` and returns `{part}`: a new part listening to every key,
  playing to the first output, with the Sound loaded.

The typed methods are `Control::performanceInfo`, `setPerformanceInfo`, `libraryQuery`,
`libraryFavorite`, `libraryRescan`, `savePerformanceToLibrary`,
`loadPerformanceFromLibrary`, `savePartAsSound`, `loadSound`, `partFromSound`,
`performanceContents`, `importPerformance`, `exportPerformance`, `archiveContents`,
`installArchive`, `libraries`, `uninstallLibrary` and `deleteLibraryItem`. Errors use
`-32005`; malformed arguments `-32602`.

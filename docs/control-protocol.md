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
| `session.status` | omitted or `{}` | `audioDevice`, `midiInputs`, `sampleRate`, `bufferSize`, `cpuLoad`, `deadlineMisses`, `graphEventDrops`, `midiEventDrops`, `audioReady`, mapped `audioInputs`/`audioOutputs` |
| `state.info` | `{"node":"synth"}` | `{"supported":true}` when opaque state export is available |
| `state.save` | `{"node":"synth","directory":"/path/to/new-state"}` | `{"saved":true}` after publishing a new state directory |
| `presets.list` | `{"node":"synth"}` | `{"presets":[{"uri":"urn:example:preset","name":"Warm"}]}` |
| `properties.list` | `{"node":"sampler"}` | `properties` entries: `uri`, `name`, `type`, `readable`, `writable` |
| `properties.get` | `{"node":"sampler","uri":"…"}` | `{"value":null}` or a typed plugin-confirmed value |
| `properties.request` | `{"node":"sampler","uri":"…"}` | `{"accepted":true}` when a readable property's refresh is queued |
| `properties.set` | `{"node":"sampler","uri":"…","value":{"type":"path","value":"/samples/piano.wav"}}` | `{"accepted":true}` when a writable property's change is queued |
| `params.list` | `{"node":"synth"}` | numeric `node`, array `parameters` of `id`, `name`, `minimum`, `maximum`, `defaultValue`, `stepped`, CLAP `flags` |
| `params.get` | `{"values":[{"node":"synth","param":3}]}` | `{"values":[{"node":1,"param":3,"value":0.5}]}` |
| `params.set` | `{"values":[{"node":"synth","param":3,"value":0.7}]}` | same shape as `params.get`, with accepted plain values |
| `params.text` | `{"node":"synth","param":3,"value":0.5}` | `{"text":"50%"}` |
| `transport.get` | omitted or `{}` | `playing`, `tempo`, `beat`, `clockSource` |
| `transport.set` | one or more of `playing` (boolean), `tempo` (1..1000), `clockSource` (`"internal"` or `"midi"`) | `{"accepted":true}` |
| `params.gesture` | `node`, `param`, `begin` (boolean) | `{"accepted":true}` |
| `subscribe`, `unsubscribe` | `topics` (1..4 of `params.changed`, `transport`, `graph.changed`, `status`), optional `allChanges` boolean | `{"accepted":true}` |
| `graph.get` | omitted or `{}` | staged topology, names/plugin metadata/ports, connections, mappings, parts, wires, `revision`, `staged`, numeric `input`/`output` endpoints |
| `graph.add` | `path`, optional `name` and `pluginId` | numeric `node` |
| `graph.remove` | `node`, optional `userWires` (`remove`, the default, or `reconnect`) | `{"accepted":true}` |
| `graph.connect`, `graph.disconnect` | `kind` (`audio` or `notes`), `from`, `to`; optional explicit audio route fields below | `{"accepted":true}` |
| `graph.map` | `from`, `lane`, `to`, `param` | `{"accepted":true}` |
| `graph.unmap` | `from`, `lane` | `{"accepted":true}` |
| `graph.commit` | omitted or `{}` | new `revision` |
| `parts.add` | optional `name`, `input` (`{"kind":"notes"\|"audio"\|"none","port":N}`, default notes) and `output` port | numeric `part` |
| `parts.remove` | `part` | `{"accepted":true}` |
| `parts.insert` | `part`, `index`, `node` | `{"accepted":true}` |
| `parts.removeNode` | `part`, `node` | `{"accepted":true}` |
| `parts.move` | `part`, `from`, `to` (chain positions) | `{"accepted":true}` |
| `wires.connect` | `kind`, `from`, `to`; for audio `fromPort`, `toPort`, optional `channels` pairs; optional `level`, `pan` | numeric `wire` |
| `wires.disconnect` | `wire` | `{"accepted":true}` |
| `wires.mix` | `wire`, `level`, `pan`, `mute`, `solo` | `{"accepted":true}` |
| `notes.send` | `on` (boolean), `key` (0..127), optional `velocity` (0..1), optional `channel` (0..15) | `{"accepted":true}` |

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
- A wire a plugin replacement could not map stays with `connected: false`; port 4294967294
  is a main port awaiting one, 4294967295 none.

All of these stage edits like the other graph methods; `graph.commit` publishes them.

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
- `status` carries the `session.status` shape when it changes, at most 60 times per second.
  `cpuLoad` is the device callback load as a fraction (1 means the entire block budget),
  not whole-machine CPU usage. `deadlineMisses` sums plugin deadline misses for nodes
  currently in the staging graph; it is not an audio-device underrun counter and may
  decrease when nodes are removed. Without device I/O, names are empty and the session
  reports its configured sample rate and maximum block size. `graphEventDrops` counts
  destination events discarded by graph merge or note-ownership overflow. If a destination
  exceeds its 512-event budget, the graph discards that destination's entire burst and
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
- The C++ `LocalClient` keeps at most 128 unread messages. When a busy client fills that
  queue, it discards its queued notifications and inserts a local `session.resync` with
  `topic: "all"` instead of disconnecting; replies are never discarded. `RemoteControl`
  then rereads the graph, parameters, transport and status.

Each client's cursor is independent; reading one client's notifications never consumes
another's. A stopped/slow client cannot grow the history indefinitely. Subscriptions may
be changed or removed at any time.

`transport.set` validates all fields before storing them. They are independent atomic
controls, not an atomic transaction with the audio thread. `transport.get` returns the
last rendered block's playing/tempo/beat plus the requested clock source. Its fields are
individual telemetry readings, not a coherent snapshot. Commands become visible in
rendered telemetry after processing; internal play/tempo commands are retained while
following MIDI clock and take effect when switching back to internal clock.

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
A write-half-close drains complete requests and responses before closing.

`LocalClient::call` waits for its matching response while retaining interleaved
notifications. The default deadline is 1 second. Timeout, malformed response, mismatched
ID or disconnection closes the connection. A mutation may already have applied when
its response is lost: refresh state after reconnecting, never replay it automatically.
IDs continue increasing across reconnects. The client bounds received messages to 128
queued documents and 1 MiB; overflow disconnects rather than silently losing updates.
`poll()` receives without waiting; `takeNotifications()` drains retained notifications.
Call these APIs from one UI/control thread, never from audio or MIDI callbacks.

`RemoteControl` checks protocol version 1, subscribes, and reads graph, parameter,
transport and host-status snapshots. Most getters use that cache; plugin text formatting
and mutations use bounded synchronous RPC calls. Call `service()` regularly to update
snapshots and deliver changes. Its change sequence is local to that adapter's lifetime,
not the server's sequence. Graph commits and explicit history gaps trigger a snapshot
refresh. Snapshots span several requests and are not atomic across concurrent clients;
interleaved notifications are retained to bring the cache up to date. Recreate the
adapter on reconnect. No commands are implicitly retried.

The desktop's `--connect SOCKET` mode uses this adapter, polls every 16 ms, and retries
failed connections about once a second. It displays disconnection status and refreshes
snapshots when reconnected. Calls currently run on the UI thread and may pause the UI
until their deadline if the engine is unresponsive. Audio and physical MIDI remain in
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
`preset` is an LV2 preset URI. At most one may be nonempty. The host loads it on a fresh
instance and applies numeric overrides afterward. It rejects changes to port layout,
note dialects or parameter metadata. Failed preparation leaves existing sounds unchanged.
Native contexts currently accept numeric patches only; nonempty state/preset arguments
are invalid. Preparation is synchronous on the control thread and may take time for samples;
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
currently exposes LV2 bundle presets; an empty list does not imply a plugin cannot load state.
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
Files use the [Performance version 2 format/schema](performance-format.md). Saving refuses
existing directories and staged graph edits. Loading validates a complete replacement;
a load failure leaves the playing graph intact. Successful publication emits a graph resync,
invalidates old node/context references, and starts stopped at beat zero on adoption.
The current note/tail audio is not saved. Resource/state/format failures use `-32005`;
malformed RPC arguments use `-32602`. These control-thread calls can take time; remote
wrappers allow 30 seconds, and callers must reread the graph after a timeout before retrying
because a disconnected request is not cancellation. The app runs them on its control worker.

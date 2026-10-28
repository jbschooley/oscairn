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
| `session.hello` | omitted or `{}` | `protocolVersion`, `server`, supported method names in `capabilities`, `limits` |
| `session.status` | omitted or `{}` | `audioDevice`, `midiInputs`, `sampleRate`, `bufferSize`, `cpuLoad`, `deadlineMisses` |
| `params.list` | `{"node":"synth"}` | numeric `node`, array `parameters` of `id`, `name`, `minimum`, `maximum`, `defaultValue`, `stepped`, CLAP `flags` |
| `params.get` | `{"values":[{"node":"synth","param":3}]}` | `{"values":[{"node":1,"param":3,"value":0.5}]}` |
| `params.set` | `{"values":[{"node":"synth","param":3,"value":0.7}]}` | same shape as `params.get`, with accepted plain values |
| `params.text` | `{"node":"synth","param":3,"value":0.5}` | `{"text":"50%"}` |
| `transport.get` | omitted or `{}` | `playing`, `tempo`, `beat`, `clockSource` |
| `transport.set` | one or more of `playing` (boolean), `tempo` (1..1000), `clockSource` (`"internal"` or `"midi"`) | `{"accepted":true}` |
| `params.gesture` | `node`, `param`, `begin` (boolean) | `{"accepted":true}` |
| `subscribe`, `unsubscribe` | `topics` (1..4 of `params.changed`, `transport`, `graph.changed`, `status`), optional `allChanges` boolean | `{"accepted":true}` |
| `graph.get` | omitted or `{}` | staged topology, names/plugin metadata/ports, connections, mappings, `revision`, `staged`, numeric `input`/`output` endpoints |
| `graph.add` | `path`, optional `name` and `pluginId` | numeric `node` |
| `graph.remove` | `node` | `{"accepted":true}` |
| `graph.connect`, `graph.disconnect` | `kind` (`audio` or `notes`), `from`, `to` | `{"accepted":true}` |
| `graph.map` | `from`, `lane`, `to`, `param` | `{"accepted":true}` |
| `graph.unmap` | `from`, `lane` | `{"accepted":true}` |
| `graph.commit` | omitted or `{}` | new `revision` |
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
method or per-client edit isolation yet. Removing a node prunes its connections and mappings,
forgets its name and cancels pending controls. The active plan keeps its plugin alive until
a later successful commit takes effect. Parameter changes are independent of graph staging.

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
  reports its configured sample rate and maximum block size.
- `graph.changed` carries the latest committed `revision`. Several unseen commits can be
  represented by one notification; fetch `graph.get` to inspect the staging area.
- `session.resync` with `topic: "params.changed"` signals a gap: the bounded 4,096-entry
  history wrapped, plugin control output overflowed, or pending commands were invalidated
  by a rescan. Refresh graph/parameter snapshots. `allChanges` cannot promise lossless
  recording if the client falls behind; gaps are explicit.

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

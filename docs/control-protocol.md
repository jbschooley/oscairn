# Control protocol: implemented subset

`oscairn::JsonRpc` adapts the existing `Control` interface to
[JSON-RPC 2.0](https://www.jsonrpc.org/specification). Link `oscairn_control_json`; call
`dispatch(string_view)` on the control thread with a complete UTF-8 JSON document.
The return value is one compact JSON document, or `nullopt` when no response is due.
The adapter performs no I/O, creates no threads and never runs on the audio thread.
The desktop still uses typed `Control`; sockets and the engine helper are pending.

The [JSON Schema](control-protocol.schema.json) describes the implemented request and
response shapes. [RFC 0003](rfcs/0003-control-api.md) describes the larger planned API.
This subset is protocol version 1. Clients must check `session.hello` capabilities before
using optional methods. Graph editing, subscriptions, gestures and parameter coalescing
are not advertised or implemented yet.

## Requests and responses

Requests use named `params` objects. Unknown or duplicate method-parameter fields are
rejected. `params` can be omitted for `session.hello` and `transport.get`; `{}` also works.
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
| `params.list` | `{"node":"synth"}` | numeric `node`, array `parameters` of `id`, `name`, `minimum`, `maximum`, `defaultValue`, `stepped`, CLAP `flags` |
| `params.get` | `{"values":[{"node":"synth","param":3}]}` | `{"values":[{"node":1,"param":3,"value":0.5}]}` |
| `params.set` | `{"values":[{"node":"synth","param":3,"value":0.7}]}` | same shape as `params.get`, with accepted plain values |
| `params.text` | `{"node":"synth","param":3,"value":0.5}` | `{"text":"50%"}` |
| `transport.get` | omitted or `{}` | `playing`, `tempo`, `beat`, `clockSource` |
| `transport.set` | one or more of `playing` (boolean), `tempo` (1..1000), `clockSource` (`"internal"` or `"midi"`) | `{"accepted":true}` |
| `notes.send` | `on` (boolean), `key` (0..127), optional `velocity` (0..1), optional `channel` (0..15) | `{"accepted":true}` |

Node addresses accept a name or an unsigned 32-bit numeric ID. Parameter IDs are unsigned
32-bit numbers. All controls use finite double values. Note channel defaults to 0;
velocity defaults to 1 for note-on and 0 for note-off. A note-on with zero velocity becomes
a note-off, as with typed `Control`. UI notes enter at the next block boundary. Physical
MIDI, including pressure, keeps its separate sample-timed route.

Parameter lists contain 1..128 distinct addresses. All addresses, writable flags and values
are validated before any change is submitted. Plugin ranges clamp values and stepped
parameters use the host's existing rounding. Acceptance means queued, not yet rendered.
There is no coalescing or guaranteed final-value retry in this slice.

When a plugin queue fills partway through `params.set`, error `-32005` includes
`data: {"accepted":N,"index":N}`. The first N entries were accepted; the entry at zero-based
index N and all subsequent entries were not submitted. Accepted entries are not rolled
back. The client can retry the remaining entries after audio drains the queues. Separate
nodes have separate queues. `notes.send` queue overflow also returns `-32005`; the session
schedules its existing emergency release, which can affect other sources on shared
instrument destinations.

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
| -32005 | Queue refused a command; see partial acceptance above |
| -32006 | Message, nesting or batch limit exceeded |

Input is limited to 65,536 bytes, 32 nested arrays/objects and 64 requests per batch.
The adapter checks byte/depth limits before parsing and batch size before dispatching any
member. These limit errors have a null ID and no requests are executed. Duplicate JSON
members are rejected at the envelope or method-parameter level. Numeric range, unique
addresses, duplicate members, byte limits and nesting limits require checks beyond JSON
Schema validation. Future socket code must enforce the byte limit while receiving, before
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
for the build; CI environments include it. Both tests are included in the existing
ThreadSanitizer job.

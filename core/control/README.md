# Control and session

The first implementation slices of [RFC 0003](../../docs/rfcs/0003-control-api.md), without
JUCE, device ownership or threads created by the library. Optional JSON-RPC and Unix
socket libraries provide the local control transport.

`Session` owns a graph, transport, bounded UI note queue and transport telemetry. A host
constructs and connects its graph, then renders stereo blocks with `Session::process`.
The caller supplies buffers, block-relative CLAP events and, when following MIDI clock,
absolute sample timestamps for clock messages from the preceding interval. It owns the
device, timestamp conversion, block splitting and mono-output adaptation.

`Control` exposes node-name lookup, parameter metadata/values/text, graph editing, gestures,
change history, transport commands/status, host status and UI notes. The desktop `Engine`
delegates to `Session::control()` or cached `RemoteControl` when connected to a helper.
Physical MIDI bypasses the control interface. Node IDs and parameter IDs are numeric;
`nodeId(name)` resolves an optional unique UI-facing name. Unnamed nodes use numeric IDs.

## Threads and failures

- Construct, configure, control, service and destroy on one main/control thread. Stop audio
  processing before destroying the session. Graph operations stage
  add/remove/connect/disconnect/map/unmap edits; commit publishes the plan. Failed commits leave audio unchanged and retain staging for correction.
- One audio thread calls `process`. It does not read the control thread's node map or call
  a wall clock. Scratch and event storage are allocated before rendering.
- UI notes use a single-producer/single-consumer queue. Each block drains a bounded
  snapshot, followed by direct input events. Both are ordered by sample time in the graph;
  UI notes precede direct input at equal timestamps. UI velocities are plain 0..1 values,
  without an intermediate MIDI 7-bit conversion.
- Invalid control addresses/values throw `std::invalid_argument`. Tempo accepts 1..1000 BPM.
  Parameter values follow the plugin host's range clamping and stepped-value conversion.
  A false `setParameter` result means the bounded pending store refused the command.
  Accepted consecutive values coalesce until `service()`; refused plugin enqueues are
  retained and retried. Gesture boundaries preserve ordering and their final values.
- UI note overflow returns false and increments `droppedNotes()`. On the next block the
  queued burst is discarded and a wildcard note-off is sent to graph-input destinations.
  This emergency release can affect other sources sharing those destinations. Full note
  ownership and device-output fail-safe policies belong to the later routing work.
- `transportState()` is last-rendered-block telemetry; its fields are individually atomic,
  not a coherent transaction. The host publishes device names, rate, buffer size and
  callback load through `setHostStatus`; `sessionStatus()` also sums plugin deadline misses.

The existing physical-MIDI queue and its capacity limits remain in the shared `hosts/audio` adapter;
this extraction does not implement RFC 0004's hardware scheduling or change the app's
next-block policy for incoming notes and pressure.

## Validation and next work

`control-session` renders the sequencer/synth/delay graph through both the session and the
existing Graph/Transport interface, checking identical samples under uneven block sizes,
parameter changes, tempo changes, UI notes and channel/poly pressure. It also covers MIDI
clock start/stop/source switching, validation, note-queue overflow/recovery and concurrent
control/rendering. Session render calls use the host tests' C++ allocation guard. CI's
ThreadSanitizer job includes this test.

`oscairn_control_json` adds `JsonRpc`, a transport-independent JSON-RPC adapter for the
controls and graph edits, with batches, subscriptions and explicit backpressure errors. Its
[wire contract and schema](../../docs/control-protocol.md) describe the implemented subset.
Two more tests check JSON-driven audio against typed controls and validate protocol behaviour
with Python's independent decoder. JSON is never parsed on the audio thread.

`control-flow` checks coalescing under plugin-queue pressure, final-value/gesture ordering,
held-note continuity, independent subscribers, history-gap reporting and staged graph edits.
The desktop services the session on its own 5 ms timer and sends slider gesture boundaries.

`oscairn_control_socket` adds a bounded `LocalServer`, `LocalClient` and cached
`RemoteControl`, all single-threaded and separate from rendering. Socket tests cover
framing, slow peers, notifications, reconnects and killing a controller process while
audio continues. Pathname-listener tests require socket-binding permission; CI requires
them. `oscairn-engine` owns device I/O and serves the socket; the desktop accepts
`--connect SOCKET`. See [host instructions](../../hosts/README.md).

The desktop now starts or reconnects to a managed helper by default; `hosts/process`
handles lifecycle outside the core. Named-socket tests and silent live-device KICK GUI
crash/reopen/recovery tests pass on macOS. Audible MIDI testing remains separate. Graph staging remains shared across clients, and gestures
have no per-client ownership/undo implementation yet.

# Control and session

The first implementation slices of [RFC 0003](../../docs/rfcs/0003-control-api.md), without
JUCE, device ownership, threads created by the library or sockets.

`Session` owns a graph, transport, bounded UI note queue and transport telemetry. A host
constructs and connects its graph, then renders stereo blocks with `Session::process`.
The caller supplies buffers, block-relative CLAP events and, when following MIDI clock,
absolute sample timestamps for clock messages from the preceding interval. It owns the
device, timestamp conversion, block splitting and mono-output adaptation.

`Control` exposes node-name lookup, parameter metadata/values/text, transport commands and
status, and UI notes. The desktop `Engine` delegates these operations to `Session::control()`.
Physical MIDI bypasses the control interface. Node IDs and parameter IDs are numeric;
`nodeId(name)` resolves the optional UI-facing name (names are required by this initial
graph-construction API).

## Threads and failures

- Construct, configure, control, service and destroy on one main/control thread. Stop audio
  processing before destroying the session. Graph setup currently has add/connect/map/commit;
  the full transactional edit API is pending.
- One audio thread calls `process`. It does not read the control thread's node map or call
  a wall clock. Scratch and event storage are allocated before rendering.
- UI notes use a single-producer/single-consumer queue. Each block drains a bounded
  snapshot, followed by direct input events. Both are ordered by sample time in the graph;
  UI notes precede direct input at equal timestamps. UI velocities are plain 0..1 values,
  without an intermediate MIDI 7-bit conversion.
- Invalid control addresses/values throw `std::invalid_argument`. Tempo accepts 1..1000 BPM.
  Parameter values follow the plugin host's range clamping and stepped-value conversion.
  A false `setParameter` result means the plugin queue refused the command. Coalescing and
  guaranteed final-value retry are not implemented yet.
- UI note overflow returns false and increments `droppedNotes()`. On the next block the
  queued burst is discarded and a wildcard note-off is sent to graph-input destinations.
  This emergency release can affect other sources sharing those destinations. Full note
  ownership and device-output fail-safe policies belong to the later routing work.
- `transportState()` is last-rendered-block telemetry; its fields are individually atomic,
  not a coherent transaction. Hardware status and device lists remain in the host.

The existing physical-MIDI queue and its capacity limits remain in the desktop adapter;
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
existing controls, with batch/notification handling and queue-refusal errors. Its
[wire contract and schema](../../docs/control-protocol.md) describe the implemented subset.
Two more tests check JSON-driven audio against typed controls and validate protocol behaviour
with Python's independent decoder. JSON is never parsed on the audio thread.

Next: complete graph operations, coalescing/notifications and gesture semantics, then
sockets and the engine helper process.
No network listener or separate engine process exists in this slice.

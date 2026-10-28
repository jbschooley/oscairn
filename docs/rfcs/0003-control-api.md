# RFC 0003: Control API

**Status:** proposed

## Problem

Today the desktop app's UI calls the engine directly, in one process. The
[architecture](../architecture.md) needs the UI in its own process, talking to the core
through a control API:

- a UI crash must not stop the audio;
- a device UI may be built with another toolkit (the JUCE app needs X11; a device UI may
  draw straight to the display), without touching the engine;
- community workflows, scripts, test tools and a ported DAW should drive the core through
  the same API as the first-party UI;
- the platform as a DAW plugin needs the same split, with the plugin's editor as the UI.

The API is outside the audio path. Timing-critical work stays in plugins in the graph.

## Decision

### One API, two ways to reach it

The API is a C++ interface in the core, `oscairn::Control`, with no JUCE and no I/O. Its
wire form is **JSON-RPC 2.0** messages. A client reaches it either:

- **in process**, by calling `Control` directly (the desktop app in one-process mode, the
  DAW plugin, tests), or
- **over a local socket**: a Unix domain socket, one JSON message per line, served by the
  engine process (devices, and the desktop app when the UI and engine are separate).

Both carry exactly the same methods and notifications, so a UI does not care which it is
using, and moving it to its own process changes only how it connects.

Why JSON-RPC over a Unix socket:

- **Small and dependency-light**, which matters on a device: a JSON parser (vendored,
  header-only, MIT) and POSIX sockets. gRPC, as Sushi uses, brings protobuf, its own
  runtime and code generation.
- **Request and response with errors**, plus notifications, which a UI needs. OSC has no
  replies or errors and usually runs over UDP; it can be added later as a bridge for
  hardware controllers and other software that speak it.
- **Any language**, readable on the wire, and testable with `socat`. The bandwidth (knob
  moves, a playhead at up to 60 updates a second) is far below what JSON handles.
- **Local only.** The socket is created with user-only permissions
  (`$XDG_RUNTIME_DIR/oscairn/control.sock`; `/run/oscairn/control.sock` on a device). There
  is no network listener. Remote access (a browser UI, a phone) is a later RFC, with
  authentication, probably as WebSocket carrying the same messages.

### Threads

`Control` runs on the engine's control thread, which also runs plugin main-thread work
(`service()`, freeing retired graph plans). It never blocks the audio thread: parameter
changes go through the host's lock-free event queue, graph edits through the graph's plan
swap, transport changes through atomics, and status comes back through atomics and
lock-free queues. The socket server runs on that same thread with `poll()`, so requests are
handled in order and need no locking against each other.

This follows the lesson of norns 3.0, which merged its separate control and audio
processes: inter-process communication belongs at the UI boundary, not between control
logic and DSP.

### Addressing and values

- **Nodes** have a numeric id, stable for the life of the graph, and an optional unique
  name given when the node is added (`"synth"`). Methods take either.
- **Parameters** are addressed by node and CLAP parameter id. Values are the plugin's own
  plain values (as in CLAP); `params.list` gives each parameter's range, default and flags,
  and `params.text` formats a value the way the plugin does.
- **Time** is in beats on the transport (`song_pos_beats` as a number), so a UI computes
  steps or bars itself.

### Methods (version 1)

| Method | Does |
| --- | --- |
| `session.hello` | Protocol version, server name and capabilities; the first call a client makes |
| `session.status` | Audio device, sample rate, buffer size, MIDI inputs, load and deadline misses |
| `graph.get` | Nodes (id, name, plugin, ports), connections and parameter-target mappings |
| `graph.add`, `graph.remove` | Load a plugin as a node; remove a node |
| `graph.connect`, `graph.disconnect` | Audio or note connections |
| `graph.map`, `graph.unmap` | Route a plugin's `oscairn.param-target` lane to a parameter |
| `params.list` | A node's parameters |
| `params.get`, `params.set` | Read or set values (several at once) |
| `params.gesture` | Begin or end a gesture (a knob being held), for automation and undo |
| `params.text` | Format a value as the plugin does |
| `transport.get`, `transport.set` | Playing, tempo, clock source (internal or MIDI clock) |
| `notes.send` | Note on or off from the UI (pads, on-screen keys), into the graph's note input |
| `subscribe`, `unsubscribe` | Choose which notifications to receive |

Graph edits are applied together: `graph.add`, `connect` and `map` collect changes, which
take effect at the next `graph.commit`, so a UI can build or change a graph without the
audio hearing it half-made. A commit that would make a cycle fails and changes nothing.

### Notifications

The server sends notifications only for what a client subscribed to:

| Notification | When |
| --- | --- |
| `transport` | Playing, tempo or position changed, at most 60 times a second while playing |
| `params.changed` | A parameter changed, from any client or from the plugin itself |
| `graph.changed` | A commit changed the graph |
| `status` | The audio device, MIDI inputs or load changed |

A client's own `params.set` is echoed to it too, so every client follows the same stream
and none needs special cases.

### Versioning

`session.hello` returns the protocol version (`1`) and a list of capabilities. Adding
methods, fields or notifications is not a new version; clients ignore what they do not
know. Removing or changing the meaning of something is a new major version. Method names
are namespaced (`graph.`, `params.`), and a future `project.` namespace (saving, loading)
comes with the file-format RFC.

### Errors

Standard JSON-RPC codes for malformed requests, plus: unknown node (-32001), unknown
parameter (-32002), invalid graph change, such as a cycle (-32003), and plugin failed to
load (-32004). Messages say what was wrong in words.

### Processes

- **On a device**, the engine runs as a service (`oscairn-engine`) that owns the audio and
  MIDI devices, the graph and the plugins, and serves the socket. The UI is another
  service and a client; if it crashes or is restarted, the audio carries on.
- **On the desktop**, the app runs the engine in its own process by default (one process,
  in-process `Control`), or connects to a running engine (`--connect`).
- **In a DAW**, the plugin is the engine, and its editor uses `Control` in process.

The engine's audio and MIDI I/O goes through the hardware-services interface (its own RFC).
The first version of `oscairn-engine` uses JUCE's audio and MIDI device classes without
its GUI modules, as the app does today; a direct ALSA backend can replace them on devices.
JUCE stays out of `core/`.

## What this rules out

- UI code in the engine process on devices.
- Clients reaching into the engine's memory, or calls that block the audio thread.
- A second, private API for the first-party UI.

## Plan

1. `core/control`: `Control` and the engine's session (graph, transport, note input, the
   audio processing now in the app's `Engine`), with no JUCE. Tests drive `Control`
   through JSON and render audio, so they are deterministic.
2. The socket server and a small client library, with tests over a real socket.
3. `hosts/engine`: `oscairn-engine`.
4. The desktop app uses only `Control`: in process by default, `--connect` for a socket.
5. The OS image runs the engine and the UI as separate services, with a test that kills
   the UI and checks the audio carries on.

## Open questions

- Batching and coalescing parameter updates when a client sends faster than it needs to
  (a knob dragged quickly): coalesce per parameter on the server, or leave it to clients.
- Whether plugin GUIs (CLAP GUI extension) are shown by the UI process (embedding a window
  from another process) or by the engine; on devices, probably neither at first, with the
  host's generated parameter pages instead.
- Remote access: WebSocket, authentication, and which methods a remote client may call.

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
  replies or errors and usually runs over UDP (see below for OSC as a second way in).
- **Any language**, readable on the wire, and testable with `socat`. A small message costs
  tens of microseconds to send, encode and decode; a UI frame is 16 ms, and the API never
  touches the audio thread, so a parameter change waits for the next audio block (1 to
  12 ms) however it arrived.
- **Protobuf was considered.** Its binary encoding is smaller and faster to decode, but
  the gain is microseconds on messages a UI sends a few hundred times a second, at the
  cost of a library, generated code and unreadable messages. Its real benefit, a typed
  schema that generates clients, comes instead from publishing the message types as a JSON
  Schema. If measurements ever call for it, a binary encoding of the same messages (CBOR
  or MessagePack) can be negotiated in `session.hello`.
- **Local only.** The socket is created with user-only permissions
  (`$XDG_RUNTIME_DIR/oscairn/control.sock`; `/run/oscairn/control.sock` on a device). There
  is no network listener; see Remote access below.

### Streams

Large continuous data (level meters, oscilloscopes, waveform overviews, spectrum) does not
go through request and notification messages. A client asks for a stream over the API and
gets a handle to a shared-memory ring buffer the engine writes and the UI reads, with no
encoding at all. This is a later addition; version 1 has none.

### OSC, a second way in

An OSC bridge, off until enabled, translates OSC messages into `Control` calls, for
controllers and software that speak OSC (TouchOSC, Open Stage Control, Max, Pure Data,
SuperCollider): parameters (`/node/synth/param/1 0.62`), transport (`/transport/play`)
and notes. It listens on UDP on the network, so it is opt-in, and it offers only those
methods, not graph edits.

### Remote access

Off by default, and not part of version 1. Syncing libraries between a device and the
desktop app or DAW plugin needs a network or USB-network connection, but not the whole
control API: a narrow sync service (exchange library indexes, transfer missing files by
hash), available only to a computer paired with the device, for example by confirming a
code shown on its screen. Remote editing from a computer would use that paired
connection. Both are decided in the library and sync RFC; the message layer here does not
depend on how messages are carried, and methods are grouped by namespace so permissions
can be granted per group.

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

### Parameter update rate

A fast knob drag, an OSC fader or a script can send hundreds of changes a second. Each
would go into the plugin's event queue and be echoed to every client; a full queue drops
events, and a dropped final value leaves the parameter where the control is not. So the
server coalesces in both directions:

- **Incoming:** the latest value per parameter is passed to the plugin once per control
  tick (about every 5 ms); intermediate values are dropped. Gesture begin and end are always
  delivered, and the final value of a gesture always arrives. `params.set` takes many
  values in one message.
- **Outgoing:** `params.changed` reaches each client at most 60 times a second per
  parameter. A client that records every change (automation, motion recording) subscribes
  to all changes instead.

MIDI controllers do not go through the control API: their events enter the graph at their
exact sample, so played performance is never coalesced.

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
- **On the desktop**, the app starts its own engine as a helper process by default and
  connects to it over the socket, so a UI crash does not stop the audio, the UI can be
  restarted while it plays, and the desktop runs the same split as a device. The app
  restarts the helper if it exits, and checks that the versions match. `--in-process` runs
  the engine inside the app (debugging, tests); `--connect` attaches to an engine that is
  already running.
- **In a DAW**, the plugin is the engine, and its editor uses `Control` in process. The
  DAW gives the plugin a window to draw into, and the audio callback must not cross
  processes every block, so both stay inside the plugin.

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
4. The desktop app uses only `Control`, starting `oscairn-engine` as its helper by default,
   with `--in-process` and `--connect`.
5. The OS image runs the engine and the UI as separate services, with a test that kills
   the UI and checks the audio carries on.
6. The OSC bridge.

## Plugin GUIs

A plugin's own GUI (the CLAP GUI extension) is created by the plugin, inside the process
that loaded it: the engine. On a device the engine has no window system and must not carry
GUI code, and most plugin GUIs are built for a mouse and a large screen. So:

- **Devices:** no native plugin GUIs. Every plugin gets generated pages from `params.list`,
  and pages of controls from CLAP's remote-controls extension where the plugin offers them
  (eight controls a page, which suit hardware knobs).
- **Desktop:** native plugin GUIs open as separate windows from the engine helper.
- **DAW:** embedded in the plugin's editor as usual.

How third-party plugins get their own look on devices (layouts and themes described in
data, custom views in a sandbox) is its own RFC.

## Open questions

- The shared-memory stream format, and how a client finds and maps a stream.
- The OSC address scheme in full.

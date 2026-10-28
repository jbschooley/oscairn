# Hosts

Programs built on the core: a file renderer, a live audio/MIDI engine and a desktop UI.

- `render/`: `oscairn-render`, the headless file renderer used in CI.
- `audio/`: shared JUCE 9 audio/MIDI adapter. It opens the default audio output and all
  MIDI inputs, converts arrival timestamps, and services the JUCE-free core `Session`.
  The demo graph is the sequencer and pads playing the basic synth through the delay.
  MIDI notes, polyphonic/channel pressure and optional MIDI clock enter here.
- `engine/`: `oscairn-engine`, a console process that owns that audio/MIDI adapter and a
  local JSON-RPC server. No JUCE GUI modules are linked. Closing a connected controller
  does not close the audio engine or reset its transport/held notes.
- `standalone/`: `Oscairn`, the desktop UI and panel layouts. Its `Engine` facade uses
  typed `Control`, either in process (the default) or through cached `RemoteControl`
  with `--connect SOCKET`. The separate helper must already be running for initial
  connection. After a connection fails, the UI retries and refreshes snapshots; it
  never replays commands with uncertain outcomes.

## Build and connect

`-DOSCAIRN_STANDALONE=ON` builds both the desktop and helper. To build only the live
engine, use `-DOSCAIRN_ENGINE=ON`. Both options default to OFF. JUCE is downloaded for
these hosts, while core and renderer builds need no JUCE. Linux and macOS are supported.

For example, from the repository root on macOS:

```sh
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOSCAIRN_STANDALONE=ON
cmake --build build-app
mkdir -p "$HOME/.oscairn"
chmod 700 "$HOME/.oscairn"
build-app/hosts/engine/oscairn_engine_host_artefacts/RelWithDebInfo/oscairn-engine --socket "$HOME/.oscairn/control.sock"
```

Keep that process running. In another terminal, start the UI:

```sh
open build-app/hosts/standalone/oscairn_standalone_artefacts/RelWithDebInfo/Oscairn.app --args --connect "$HOME/.oscairn/control.sock" --layout kick
```

Quit any existing Oscairn UI first so `open` starts it with the new arguments. On Linux,
run the corresponding executable in `oscairn_standalone_artefacts/RelWithDebInfo/` directly.
Both hosts accept `--plugins DIR`; connected UI mode uses the helper's plugins.
The normal demo launcher still runs in-process mode; `--in-process` also selects it
explicitly and cannot be combined with `--connect`.

The socket parent must be private and owned by the user; existing endpoints are never
replaced automatically. SIGINT/SIGTERM stops the helper and removes its own socket.
After an engine crash, verify it has stopped before removing its stale endpoint and
starting a new helper. Reconnecting a UI preserves a running engine's state; restarting
an engine starts the demo graph afresh. Session persistence, helper auto-launch/restart,
and the live GUI crash/reopen listening test remain pending.

See the [wire contract](../docs/control-protocol.md) for framing, bounds, status fields
and timeout semantics. The socket carries JSON-RPC; no WebSocket or gRPC is involved.

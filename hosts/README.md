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
- `process/`: desktop helper startup and lifetime locks, independent of JUCE. A
  launcher attaches to a running engine or starts the bundled helper. Its destruction
  leaves that helper alive. Concurrent launches share one engine per plugin directory.
- `standalone/`: `Oscairn`, the desktop UI and panel layouts. It starts or reconnects
  to a helper by default through cached `RemoteControl`. `--in-process` runs audio in
  the UI process; `--connect SOCKET` attaches to an externally managed engine. After
  disconnection the UI retries and refreshes snapshots, without replaying commands.
  If its managed engine stops, a banner offers **Restart engine**, which opens a fresh
  demo session. Startup failures offer Retry/Quit; snapshot commands fail immediately.

## Build and connect

### Audio buffer size

The shared `AudioEngine` opens the default device through JUCE without requesting a
buffer size or sample rate. JUCE chooses a supported size using its audio backend's
device default. The actual values are printed on startup and exposed by
`session.status` as `bufferSize` (frames) and `sampleRate` (Hz). The macOS test device
was observed at 512 frames / 48 kHz; that is not a forced setting for every device.

The engine's internal `maxFrames = 512` is a separate processing/allocation ceiling.
Device callbacks larger than 512 are split into smaller processing calls; callbacks
below 512 stay smaller. Splitting does not reduce the device's buffering latency.
At 48 kHz, 512 frames span about 10.7 ms, excluding other device/driver latency.

Neither the UI nor the control API currently changes audio device settings. Selecting
a different buffer requires host configuration code using JUCE's `AudioDeviceSetup`
and `setAudioDeviceSetup`, with device-supported sizes. Persisting that choice and
exposing it to users remain follow-up work. The graph currently stays at its startup
sample rate; a later device rate change deliberately silences playback until restart.

Buildroot Linux on hardware uses this same engine through ALSA, so it currently also
uses the backend/device default. No separate hardware buffer-size policy is wired in
yet. A board profile can request and validate a different supported size at startup;
it should be chosen using measurements on the actual codec/driver under load.
The QEMU dummy sound card cannot establish real-hardware latency or reliable settings.

The offline renderer is independent: it defaults to 512 frames at 48 kHz and accepts
`--block` and `--rate`. LV2 Atom buffer capacities are a separate message-memory policy.

### Launching

`-DOSCAIRN_STANDALONE=ON` builds both the desktop and helper. To build only the live
engine, use `-DOSCAIRN_ENGINE=ON`. Both options default to OFF. JUCE is downloaded for
these hosts, while core and renderer builds need no JUCE. Linux and macOS are supported.

Opening the built app normally starts the helper automatically. The macOS bundle
contains the helper beside the UI executable and the demo plugins in `plugins/`.
On Linux, installed hosts use the sibling `oscairn-engine` and `../lib/clap` plugins.
`--plugins DIR` selects a separate managed session for that canonical plugin directory.

For explicit, externally managed operation, from the repository root on macOS:

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
The normal demo launcher now uses managed-helper mode. `--in-process` selects single-process
operation explicitly and cannot be combined with `--connect`.

The socket parent must be private and owned by the user. Explicit `--socket` servers
never replace existing endpoints. SIGINT/SIGTERM stops the helper and removes its own socket.
After an engine crash, verify it has stopped before removing its stale endpoint and
starting a new helper. Reconnecting a UI preserves a running engine's state; restarting
an engine starts the demo graph afresh. Session persistence remains pending. A silent live-device
smoke test on macOS now verifies the actual KICK UI can be killed and reopened while
the same engine keeps processing; an audible listening check remains separate.

See the [wire contract](../docs/control-protocol.md) for framing, bounds, status fields
and timeout semantics. The socket carries JSON-RPC; no WebSocket or gRPC is involved.

## Diagnosing a listener failure

`bind Unix socket PATH: Operation not permitted` means the OS refused the bind syscall.
Sandbox policies can deny local Unix sockets even in a writable, user-owned 0700
directory; a Unix socket needs no internet connection. Changing directory permissions
does not grant a process permission to bind. The current development execution sandbox
exhibits this restriction. This requires a permitted runtime or a runtime policy change,
not a different RPC protocol or weaker endpoint permissions.

Run the automated checks in a runtime that permits local socket listeners:

```sh
OSCAIRN_REQUIRE_SOCKET_BIND=1 ctest --test-dir build -R '^control-local' --output-on-failure
```

This mode fails if binding is denied; it cannot report a skipped listener test as a pass.
Without that variable, only a permission denial from the bind syscall can skip the
named-socket test. Path validation and file preservation run in a separate test even
when listener creation is unavailable. The named-socket test includes a controller
process crash and reconnect, without using an audio device; the live GUI/listening test
is still separate.


## Live desktop regression test (opt-in)

With a desktop session and audio output available, this test starts its own helper and
KICK UI, mutes the synth, kills its UI process, and verifies that the same helper keeps
advancing before reopening the UI at 143 BPM. It saves before/after PNGs and logs, then
stops only the processes it started and checks orderly socket cleanup. It is not part
of default CTest because it needs live devices and a desktop.

```sh
python3 hosts/engine/tests/desktop_lifecycle.py \
  build-app/hosts/engine/oscairn_engine_host_artefacts/RelWithDebInfo/oscairn-engine \
  build-app/hosts/standalone/oscairn_standalone_artefacts/RelWithDebInfo/Oscairn.app/Contents/MacOS/Oscairn
```

The first live run exposed a macOS console-loop issue: `runDispatchLoop()` returned
immediately without an `NSApplication`. The helper now pumps `runDispatchLoopUntil()`
in bounded intervals, keeping JUCE timers and native device events active without a
GUI application. Screenshots also check that reopened tempo controls display the engine's
current tempo. The smoke test is silent; it does not establish perceptual audio quality
or test physical MIDI inputs.


## Managed desktop lifecycle

Managed sessions use `engine-<hash>.sock` in a private per-user runtime directory:
`$XDG_RUNTIME_DIR/oscairn` on Linux when available, otherwise `/tmp/oscairn-<uid>`.
`OSCAIRN_RUNTIME_DIR` overrides the directory for isolated sessions/tests. Existing
runtime directories must already be private and user-owned; symlinks are refused.
The hash identifies the canonical plugin directory, not an audio engine backend.

The `.launch` file serializes launchers; the `.lock` file is held by the helper for its
whole lifetime. Under that lock, `--managed` removes an abandoned socket only after
connection refusal and an inode recheck. Regular files, symlinks and reachable servers
are preserved. Lock/log files must be private regular files and are not unlinked while
in use. The PID written to `.lock` is diagnostic; locking, not a stored PID, establishes
liveness. Output goes to `.sock.log`. The helper has its own process group and redirected
standard streams, so the UI's exit does not own its lifetime.

Startup waits up to five seconds for a listener. If a helper launched by this UI never
becomes ready, the launcher terminates and reaps that specific child (force-stopping it
if it does not respond). It never stops an existing helper it merely tried to attach to.
A macOS permission prompt may delay first startup; accept the system prompt and use
Retry if startup timed out. There is no background crash/relaunch loop. Disconnected
controls are disabled. A managed helper's death enables Restart engine; it loads the
fresh demo with transport stopped. Unsaved engine state is not restored. External
`--connect` sessions reconnect but leave lifecycle management to their owner.

Closing a managed UI leaves its engine and audio device open. Reopening attaches to
that session, including its existing transport and parameter state. Stop playback before
closing if you want silence. Engine shutdown is currently external (SIGTERM); a UI
shutdown command and session save/restore remain separate work.

`engine-process-policy` checks ownership locks, refusal to replace unrelated files and
bounded cleanup of an unresponsive child. `engine-process-lifecycle` checks concurrent
launch, UI detachment, reuse, stale-socket recovery and fresh state after an engine crash
using a device-free helper. CI requires socket permission for this test.

The live managed test exercises the bundled desktop without `--connect`:

```sh
python3 hosts/engine/tests/managed_lifecycle.py \
  build-app/hosts/standalone/oscairn_standalone_artefacts/RelWithDebInfo/Oscairn.app/Contents/MacOS/Oscairn
```

Add `--interactive-recovery` to exercise the on-screen Restart engine button instead of
reopening another UI. This mode waits up to 60 seconds for the button click. Both modes
use a private test runtime directory, mute the synth before playback, save screenshots,
and stop only the processes they started. Automatic startup, reattachment and the actual
recovery button were verified on macOS with the Multi-Output Device at 48 kHz/512 samples.
Separately, Jacob verified SMK37 notes, pressure response and sequencer PLAY/STOP in the
default managed desktop's KICK layout on October 28, 2026.

## LV2-enabled hosts

Add `-DOSCAIRN_LV2=ON` when configuring either the renderer or the desktop/helper. They
use the same graph adapter. `oscairn-render` accepts `bundle.lv2#plugin-uri` alongside
CLAP paths; `graph.add` accepts a bundle path plus the URI in `pluginId`. See
[LV2 hosting](../core/lv2-host/README.md). The desktop layout and default CLAP demo are
unchanged; plugin browsing and selection UI remain pending. LV2-enabled macOS builds
currently link to the installed Lilv libraries; packaging those dependencies for another
Mac is separate from the existing helper/plugin copying step.

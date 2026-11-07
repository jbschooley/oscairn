# RFC 0022: Apps that open each other

Status: in progress, 2026-10-31. Step 1 is built in the desktop app: the registry
(`hosts/standalone/Apps.h`), the Library answering `oscairn.pick.performance` and
`oscairn.pick.sound` and the Setlist taking `oscairn.send.performance`, each flow checked by
`desktop-shell`. One app
per action for now (the first to offer it); manifests, the control API transport and the
chooser are not built. [What an app is](#what-an-app-is) was decided 2026-11-06: native
programs in their own processes, contained like plugin workers; the build order below follows
from it. Builds on the separation of apps from the sound
engine ([architecture](../architecture.md#two-kinds-of-thing-plugins-and-the-ui)), the
control API ([RFC 0003](0003-control-api.md)), the library
([RFC 0021](0021-sounds-performances-and-library.md)) and setlists
([RFC 0012](0012-sync-external-midi-and-multi-device.md)).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. In-process actions: the registry; Library picks (Performances and Sounds), Setlist takes Performances
- [ ] 2. Apps as processes: started and supervised by the shell, actions over the control API, manifests
- [ ] 3. Choice and defaults
- [ ] 4. Permissions and limits enforced per app
- [ ] 5. Apps in packages: signed builds, install, update, removal, data folders
- [ ] 6. Sandboxing on Linux and macOS, and a display path that keeps apps apart on the device
- [ ] 7. The built-in apps moved out of the desktop process, in order
- [ ] 8. Third-party apps: a sequencer interface, then a ported DAW

## Problem

Apps are separate components: the Groovebox, a Setlist app, the Library, the sound editor.
Musicians move between them all the time:

- from the Library, playing straight from the list (like the MODX's Category Search), add a
  Performance to the set for Friday;
- in the Setlist app, fill a slot by choosing a Performance from the Library;
- in the Groovebox, choose a part's Sound from the Library;
- from a part in the sound editor, open the Groovebox's pattern for it.

If each app links to the others, none can be replaced, and a third party's setlist or library
app is a second-class citizen. If none do, the musician copies names by hand.

## Outcome

Apps offer **actions** to each other by name, and ask for them without knowing which app
answers:

- **Pick** something and get it back: "pick a Performance", "pick a Sound for this part".
- **Send** something to whoever takes it: "add this Performance to a setlist".
- **Open** something: "open this Performance in the sound editor", "open this part's pattern".

An action that no installed app offers is not shown. When two apps offer it, the musician
chooses once, and may change the default in settings. Oscairn's own apps use the same route
as anyone else's.

## What an app is

Decided 2026-11-06. The owner, asked what an app is:

> "Performance and Library and Groovebox are apps. Eventually we will need a settings app too.
> I think the launcher is an app too. But ported DAWs, other groovebox or sequencer interfaces
> are apps."

So an app is a whole program with its own UI:

- **Oscairn's own:** Performance (the Play view and the sound editor), the Library, the Setlist,
  the Groovebox, Recordings, the launcher ([RFC 0030](0030-system-shell-launcher-and-navigation.md)),
  the Store ([RFC 0031](0031-package-repository-and-distribution.md#the-store-app)) and a
  Settings app to come;
- **third parties':** ported DAWs, and other groovebox and sequencer interfaces.

A ported DAW is native code, so apps are native programs, each in its own process, reaching the
engine through the control API. Oscairn's own apps follow the same model and get nothing a third
party's could not. This replaces the architecture's "one first-party UI, with Live and Groovebox
as screens inside it" ([two kinds of thing](../architecture.md#two-kinds-of-thing-plugins-and-the-ui)):
timing still belongs in event plugins, and the UI is now several apps under one shell.

### The model

- **An app is a native executable per platform:** a macOS build for the desktop, a Linux ARM64
  build for the devices (and others as RFC 0006's [platform builds](0006-packages-and-portability.md#platform-builds-and-hardware)
  require). It is written in anything that can speak the control protocol; the C++ client
  (`LocalClient`, and the desktop's `Engine` facade for JUCE apps) is what Oscairn's own use.
- **The shell starts it** (Home's tile, an action, a product button), after the engine admits
  its memory reservation (RFC 0025's `memory.launch`, which the shell already asks). The shell
  is the supervisor, decided 2026-11-07 so apps survive an engine restart: it already starts the
  engine and reconnects when it drops, so it outlives the engine; it applies the limits the
  engine's admission grants (the cgroup subtree apps go into is delegated to it,
  [RFC 0025](0025-resource-budgets.md#linux)). The shell stays the system UI ([RFC 0030](0030-system-shell-launcher-and-navigation.md#the-shell-and-the-launcher)):
  it decides which app is in front and where it draws, and is not itself an app.
- **Its connection is handed to it:** the shell asks the engine for a connection with the app's
  grants; the engine makes a socket pair, adopts one end as a client with them
  (`LocalServer::adopt` exists for this), and passes the other to the shell, which gives it to
  the app as an inherited descriptor (`OSCAIRN_CONTROL_FD`) at start, and over a Unix socket
  (`SCM_RIGHTS`) after an engine restart, when the app's library reconnects and resyncs. The app never sees the user's
  control socket, so the engine always knows which app is on which connection.
- **Its UI on the desktop** is its own windows, as any program's. The shell's Apps menu and
  Home window start it and bring it forward; a pick opens as the answering app's own window over
  the asker, placed where the shell says. Nothing from another process is embedded.
- **Its UI on the device** is one full-screen surface the shell gives it: the screen below the
  top bar, at the profile's size class (RFC 0019). Sheets for picks are a second surface the
  shell places over the asker. Today the image runs one JUCE process on X11
  ([os/README.md](../../os/README.md)); X11 lets every client read the others' input and
  pixels, so it cannot hold untrusted apps apart. The proposal: the shell becomes a small
  Wayland compositor that gives each app its surface, and an app that needs X11 (JUCE's Linux
  windowing is X11 only, run on Wayland through Xwayland) gets an X server of its own. This is
  RFC 0028's open question about leaving X11, now with a reason; until it is settled, the
  device runs only apps signed by the image's keys.
- **The device's controls and MIDI come through the engine,** never from the devices directly.
  The engine already owns the MIDI inputs and maps the panel and connected controllers
  (`controllers.set`, RFC 0011's controller-mapping service). The app in front gets the
  `controllers` topic and sets `controllers.view` for what its screen shows, so the knobs follow
  the screen ([RFC 0030](0030-system-shell-launcher-and-navigation.md#the-rules)); an app behind
  gets none unless it holds `background`. Buttons the profile binds to the shell (`oscairn.home`,
  `oscairn.back`, `oscairn.panic`) never reach an app. An app with `midi` subscribes to the
  engine's MIDI input and sends to its outputs through the engine, timed by the engine's
  transport ([RFC 0012](0012-sync-external-midi-and-multi-device.md)).
- **Its documents go in the library** where they are musical (a pattern, a song, a setlist), as
  item types ([RFC 0023](0023-item-types.md)), so they back up, sync and travel without the
  app. Its own data folder (below) is for its private state.

### The manifest

`app.json` in each app's folder, read at install (step 5) or from the folder (step 2), so the
host knows an app without starting it:

```json
{
  "format": "oscairn.app",
  "version": 1,
  "id": "com.example.stepper",
  "appVersion": "1.2.0",
  "name": "Stepper",
  "names": { "de": "Stepper" },
  "icon": "icon.svg",
  "catalog": "catalogs/",
  "requires": { "oscairn": "1.4", "protocol": 1 },
  "builds": {
    "macos-arm64": "Stepper.app/Contents/MacOS/Stepper",
    "linux-arm64": "bin/stepper"
  },
  "surfaces": ["desktop", "panel"],
  "offers": [{ "action": "oscairn.open.pattern", "name": "Open pattern", "icon": "pattern.svg" }],
  "asks": ["oscairn.pick.sound"],
  "home": false,
  "cards": [{ "id": "pattern", "title": "Pattern", "values": ["bpm", "step"], "actions": ["play", "stop"] }],
  "memory": { "reserve": 80, "max": 200 },
  "cpu": { "audio": 0 },
  "permissions": ["play", "edit", "library", "controls", "midi"]
}
```

- **Identity and version:** `id` (reverse-DNS, under the publisher's ID, RFC 0031) and
  `appVersion`; `requires` names the oldest system and the protocol version it speaks.
- **Actions:** `offers`, the actions it answers with a name and icon for menus (this RFC);
  `asks`, those it uses, so the install can say when nothing installed answers one.
- **Cards and launchers:** `cards`, described not drawn, and `home` for a launcher
  ([RFC 0030](0030-system-shell-launcher-and-navigation.md#manifests-and-profiles)); `surfaces`, where
  it draws.
- **Reservations:** `memory.reserve` and `memory.max` in MiB
  ([RFC 0025](0025-resource-budgets.md#declarations)). `cpu.audio` is the share of an audio
  block's deadline its audio node takes, for a DAW (0 for none, below); an app's UI is not in
  the audio budget.
- **Permissions:** what it may use of the control API ([below](#permissions)).
- **Platforms and builds:** `builds`, each platform's executable within the app's folder; a
  platform not listed installs as unavailable, as RFC 0006 says of plugins.
- **Name, icon and translations:** `name` in American English, `names` per locale, the icon as
  plain SVG paths the theme recolors (RFC 0030), and `catalog`, its message catalogs
  ([RFC 0029](0029-internationalization.md#catalogs)).

### Permissions

An app is a client like a paired one ([RFC 0026](0026-network-control-and-web-ui.md#permissions)),
with its permissions from its manifest instead of from pairing. RFC 0026's four, and six for
what only apps do:

| Permission | Gives | For example |
| --- | --- | --- |
| `play` | RFC 0026's Play: notes, transport, scenes, controls, setlist movement | a setlist app |
| `edit` | RFC 0026's Edit: the graph, parts, parameters, scenes, undo | a sound editor, a DAW |
| `library` | RFC 0026's Library: the library, Performances, assets | the Library, a setlist app |
| `system` | RFC 0026's System: updates, network, pairing, profiles, trust, installing code | Settings, the Store |
| `controls` | The device's and controllers' knobs, buttons and pads while in front | the Groovebox |
| `midi` | MIDI in from the engine's inputs and out to its outputs | a sequencer, a DAW |
| `audio` | An audio node in the graph (below) | a ported DAW |
| `background` | `controls`, `midi` and `audio` while not in front | a sequencer played from pads while the Library is open |
| `network` | Network sockets in its sandbox | the Store, a cloud library |
| `files` | The library's files read-only, and drives through the shell's file picker | a DAW, a sample browser |

- **Every app** may read the session's state it needs to draw (`session.hello`, `session.status`,
  the `apps` topic for its own requests) and its own data folder; a setlist app asks for
  `play` and `library` and nothing else.
- **Granted at install,** listed in the install prompt in plain words. `system`, `network` and
  `background` are asked one by one; an update that adds a permission asks again before it
  installs, as Flathub holds a build whose permissions changed
  ([RFC 0031](0031-package-repository-and-distribution.md#what-others-do)).
- **Enforced by the engine** per connection: a method outside the grants is refused with an
  error naming the permission (RFC 0026), and a topic outside them is not subscribed.
- **No file paths from apps:** methods that take a path (loading a plugin bundle, importing a
  file) take from an app only store identities and paths its sandbox grants. The local socket
  today is "a same-user control interface, including native plugin loading; it is not a sandbox
  for untrusted clients" ([control protocol](../control-protocol.md#local-transport-and-client)):
  an app's connection is the untrusted kind.

### Containment

Third-party app code is untrusted. A crash, a hang, a leak or a flood of requests from an app
must never reach the sound, the engine or another app. The shell supervises apps as the engine
does plugin workers ([RFC 0010](0010-plugin-worker-isolation.md#failure-handling)), off the audio
path:

- **Crash:** seen by the process exiting or its socket closing. The shell shows a banner
  ("Stepper stopped", with Details); the app in front is restarted once at once and brought back,
  restoring its own state from its data folder. Restarts are bounded as workers' are (3 a
  minute), after which it stays closed with **Open again**. An app behind is restarted when next
  opened, or at once when it is always-available.
- **Hang:** the engine pings each app every second (`apps.ping`, answered by the client library
  on the app's main thread). The app in front that has not answered for 5 seconds gets the shell's
  "Stepper is not responding" with **Wait** and **Close**, as Android's ANR does after 5 seconds
  without input handled; an app behind that has not answered for 10 seconds is killed. A pick
  waiting on a closed app gets a cancel. (Timings proposed, to be tuned.)
- **Memory:** on Linux the app's cgroup `apps/<app>` holds `memory.min` at its reservation and
  `memory.max` at its declared maximum ([RFC 0025](0025-resource-budgets.md#linux)), with an
  `RLIMIT_AS` backstop as for workers; on macOS, where no limit is enforceable by the kernel
  (RFC 0010 step 6), the supervisor reads its footprint every 100 ms. Over its maximum it is
  stopped and reported like a crash, and its measured peak raises its reservation at the next
  start, as RFC 0025 says.
- **CPU:** on Linux its cgroup runs at a lower `cpu.weight` than the shell, on cores apart from
  those reserved for audio ([RFC 0027](0027-reliability-levels.md)); apps never get real-time
  priority, except an audio node's thread, which the engine wakes as it wakes a worker's. Its
  CPU time is reported in `session.status` as workers' is.
- **stderr and stdout** go to a pipe the supervisor reads: the last 64 KiB kept per app and
  appended to `Logs/apps/<id>.log` in the data folder, rotated at 1 MB, and shown in a crash
  banner's Details.
- **The engine never waits on an app.** The local server already bounds each client (16
  documents and about 64 KiB a tick, 1 MiB of queued output) and disconnects one whose queue is
  full; an app disconnected so is treated as crashed. Added for apps: a per-app budget for
  expensive calls (plugin loading, imports, graph commits, library writes), slowed with an error
  past it and disconnected when it keeps on; and the server's limit of 8 clients raised to count
  apps apart from other clients. Methods that run long on the control thread (synchronous
  plugin loading, preparation) stall other clients' controls though never the audio; they move
  off the control thread before third-party apps are offered.
- **Validation as for any untrusted client:** every request is already checked (sizes, nesting,
  ranges, IDs); an app's answers to actions are checked against what the action gives back
  before the asker sees them (above, [Trust](#third-party-apps)).

**Sandboxing,** mirroring RFC 0010's plan for workers:

- **Linux:** the app runs as an unprivileged user that is not in the `audio` or `input` groups,
  so it cannot open sound cards, MIDI ports or input devices; with `no_new_privs`, Landlock
  granting read-only its own install folder and the system's libraries, read-write its data
  folder, read-only the library's folders with `files`, and nothing else (the control socket's
  folder included); and a seccomp filter refusing `ptrace`, mounts and network sockets without
  `network`. Namespaces (as bubblewrap, which Flatpak uses) where the image's kernel allows
  them, not relied on.
- **macOS:** the shell starts the app under a sandbox profile of its own, as RFC 0010 plans
  for workers, with the same grants. Which mechanism, and whether it holds for a GUI app
  started this way, is checked in step 6. The App Sandbox's entitlements are chosen by whoever
  signs the app, so Oscairn cannot impose them. Until step 6, permissions on the desktop hold
  against an honest app but not a hostile one, which could open the user's control socket
  itself; the install prompt says so.

### Packaging

An app is a kind of package ([RFC 0006](0006-packages-and-portability.md), [RFC 0031](0031-package-repository-and-distribution.md)):

- **In an archive,** each build under `Apps/<id>@<version>/<platform>/`, beside the plugin
  builds, listed in the manifest with its digest, every file hashed. Each carries RFC 0006's
  [app signature](0006-packages-and-portability.md#signatures) (`oscairn-app-1`), judged at
  install against keys trusted for `apps`; unsigned or unknown asks, and a product profile may
  refuse it.
- **Installed** into `OSCAIRN_DATA_HOME/Apps/<id>@<version>/<platform>/`, read-only to the app.
  A device takes only its own platform's build. Nothing runs at install (RFC 0031 rules out
  install scripts).
- **Updated** as RFC 0031 says of apps: only once the app is closed, never while playing, the
  previous version kept until the new one has started once and restored if it does not. Its data
  folder is not rolled back, so an app migrates its data only after starting cleanly.
- **Removed** only when closed, through Settings' Installed content (RFC 0006). Its library
  items are the musician's and stay; its data folder stays unless **Remove its data too** is
  chosen.
- **Its data folder:** `OSCAIRN_DATA_HOME/AppData/<id>/`, apart from the install, kept across
  versions, on the user data partition on a device (RFC 0028), and in backups. Logs under it.

### The built-in apps

- **They become ordinary apps,** each its own executable with a manifest, signed by the project's
  key, shipped in the system image's read-only partition (RFC 0028). They ask for permissions
  like any app: Settings and the Store hold `system`, the Library `library`, and so on. Their
  placeholder reservations (`Profile::appMemory`) become manifest figures, measured per profile.
- **They already reach the engine only through the control API:** the desktop app runs the
  engine as a separate helper by default ([hosts](../../hosts/README.md)). What holds them in
  one process today is the action registry (`Apps.h`), the windows and the shell's top bar, and
  shared models (`SoundPickerModel`, the setlist), which split cleanly.
- **What stays in the desktop process:** the shell (the top bar, overlays, the switcher, quick
  settings, the fallback app list, engine recovery), which is system UI. The apps leave it in
  this order:
  1. **Setlist** (step 2's gate): small, and exercises both a pick and a send across processes.
  2. **Library:** the picks as sheets over another process (RFC 0030 step 3).
  3. **Groovebox,** with `controls` and `midi`: proves the device's pads through the engine.
  4. **Recordings.**
  5. **The launcher,** with RFC 0030 step 2: cards from other processes.
  6. **Settings,** built as its own app from the start; the desktop's Settings dialog stays in
     the shell until then.
  7. **Performance** last: the Play view and the sound editor are the most tied into the shell
     (`DesktopShell`, `PlayView`), always-available, and what a stage depends on.
- **The launcher is not special.** It has no privilege; any app with `home: true` may be one,
  and the chosen launcher is in the always-available set. It need not always run: a product
  whose profile names one app as Home runs none (RFC 0030), and while a launcher is down the
  shell draws its own list. If the chosen launcher fails its restarts, the shell uses the image's
  launcher until the next start and says why, so a broken third-party launcher cannot leave a
  device without Home.
- **Nothing a third party could not have,** as RFC 0030 says of the launcher: no private
  methods, no private actions, no priority in choosing (above).

### Ported DAWs

Porting a DAW means its UI becomes an app. Its audio has three possible homes:

1. **A client of the engine:** its tracks and plugins become the engine's graph (parts, nodes,
   wires through the control API), and its timeline plays through an event plugin in the graph,
   as the architecture puts anything timed. Everything the engine gives works: workers for each
   plugin, budgets, panic, scenes, recording. But only a DAW written for Oscairn, or rewritten,
   fits: an existing DAW's engine would be thrown away.
2. **An audio node:** the DAW keeps its own engine and plugin hosting, and the engine gives it
   one node in the graph, with RFC 0010's shared-memory hand-off: per block the engine writes
   the node's MIDI, transport and audio in, wakes the DAW's audio thread, waits within the
   node's share of the deadline and reads its audio out. A late block is silence and repeated
   ones mute the node, as for a worker. The DAW's output goes through the engine's mix, so
   levels, panic and recording still work, and its declared `cpu.audio` is admitted against the
   audio budget. Its plugins are contained only as one unit (the DAW's process), and are outside
   the plugin store and its signatures unless the DAW loads them from there.
3. **The audio device itself:** the DAW opens the sound card and the engine stops. Ruled out:
   the instrument would stop being one, and panic and the watchdog would not reach it.

**Recommendation:** 2 for ported DAWs, since porting an existing DAW is what the owner named and
its engine is most of it; 1 for DAWs and sequencers written for Oscairn, which needs nothing new
beyond this RFC. Step 8 proves a sequencer interface on 1 first (an app over the engine's own
sequencer plugin), then a DAW on 2. **A question for the owner** (below): whether a DAW
hosting its own plugins, outside per-plugin isolation and the store, is acceptable on a device.

### Prior art

| System | What runs on it | Taken here |
| --- | --- | --- |
| Ableton Push 3, standalone | Live itself; Ableton's devices, but no third-party VST or AU plugins ([Synthtopia](https://www.synthtopia.com/content/2023/05/23/top-5-ableton-push-3-questions-answered/), [Tape Op](https://tapeop.com/reviews/gear/157/push-3)); most Max for Live devices ([Ableton](https://help.ableton.com/hc/fr/articles/8483166334748)) | A closed device runs one vendor's DAW; Oscairn's DAW is one app among others |
| Ableton Move | Sold as a standalone instrument, with no way to add software described ([Ableton](https://www.ableton.com/en/shop/move)); the community's Move Everything runs beside the stock software, unsupported ([Synth Anatomy](https://synthanatomy.com/2026/02/move-everything-turns-ableton-move-into-an-open-platform-with-more-synths-fx-and-more.html)) | Musicians want third-party code even on a closed device |
| Akai MPC, standalone | Third-party plugins cannot be installed ([Akai](https://support.akaipro.com/en/support/solutions/articles/69000876025-akai-mpc-series-how-to-use-3rd-party-vst-s-with-your-mpc)); Native Instruments' Play Series instruments run since the 3.5 update ([inMusic](https://support.inmusicstore.com/en/support/solutions/articles/69000864875-mpc-edition-native-instruments-play-series-plugins-setup-guide-faq)) | A vendor's chosen partners only; Oscairn's model is open |
| monome norns | Scripts: Lua for the hardware and UI, SuperCollider engines for sound, kept in `~/dust/code` ([reference](https://monome.org/docs/norns/reference/), [study](https://monome.org/docs/norns/study-0/)) | The same split: the app has the UI and controls, the sound is elsewhere |
| Bela | User projects in C/C++, Pure Data, SuperCollider or Csound, edited in a browser IDE, on Xenomai ([wiki](https://github.com/BelaPlatform/bela/wiki/Bela)) | Native code as the unit; no containment, as a single-user development board |
| Flatpak | By default no network, no host files and no device nodes; extra access declared in the manifest's `finish-args`; portals for access granted case by case ([docs](https://docs.flatpak.org/en/latest/sandbox-permissions.html)) | Permissions in the manifest; a file picker as the way to a file |
| Android | An app that has not handled input for 5 seconds gets "not responding" ([ANR](https://developer.android.com/topic/performance/vitals/anr)); replaceable launchers (RFC 0030) | The hang rule |

Elektron's instruments and Zynthian's apps were not checked here.

## Actions

An action has a name, what it takes and what it gives back. Names are reverse-domain strings,
like CLAP's extensions, so third parties can add their own without collisions.

| Action | Takes | Gives back | Offered by |
| --- | --- | --- | --- |
| `oscairn.pick.performance` | optional tags to start with | a library ID | Library |
| `oscairn.pick.sound` | optional part, tags | a library ID | Library |
| `oscairn.send.performance` | a library ID | done or canceled | Setlist |
| `oscairn.open.performance` | a library ID | | Sound editor |
| `oscairn.open.part` | a part | | Sound editor |
| `oscairn.open.pattern` | a part or sequencer node | | Groovebox |

What is passed is always a reference the engine already knows (a library ID, a part, a node),
never the content: apps share the one Performance through the control API, so an action only
has to say *which*.

## How it works

- **An app's manifest** lists the actions it offers, with a name and an icon for menus. It is
  part of the app's package ([RFC 0006](0006-packages-and-portability.md)), read at install,
  so the host knows the offers without starting the app. It also declares the app's memory, a
  reservation and a maximum (proposed: [RFC 0025](0025-resource-budgets.md#declarations)). A
  running app keeps its reservation in front or behind, and a small always-available set
  (by default `oscairn.performance` and `oscairn.library`, a setting) is reserved even when
  closed.
  Launching any other app is admitted only if its reservation fits; installed apps that are not
  running reserve nothing.
- **The host** (the shell on the desktop, the system UI on a device) keeps the list, decides
  who answers, starts or brings forward that app, and passes the request and the answer back.
  Apps never call each other directly.
- **A pick opens over the asking app:** a sheet on the device, a dialog or a pane on the
  desktop, in the answering app's own view at the asking app's size class (RFC 0019). The asker
  waits for an answer or a cancel, and is never blocked: a pick has no time limit, and the
  asker may be closed meanwhile, in which case the answer is dropped.
- **A send** is fire and forget with an acknowledgement, so "Add to setlist" can say "Added to
  Friday".
- **Transport:** requests go through the engine's control API as messages addressed to an app
  (`apps.request`, `apps.answer` and an `apps` notification topic), because every app already
  holds a control connection and the engine outlives any one app. The engine relays; it does
  not interpret actions.

## Third-party apps

- A third-party setlist or library app offers the same actions and is listed beside Oscairn's.
  The musician picks a default per action; nothing gives Oscairn's own apps priority.
- **A third-party library** may keep its own content (a sample browser, a cloud library). Its
  picks still give back a library ID, so what it hands over must be in the Oscairn library:
  it imports or registers the item first (RFC 0021's index, RFC 0009's registered folders), so
  saving and portability keep working without that app.
- **Providers without a whole app:** a package may add *content* to the picker (a pack's
  Sounds and tags) without offering actions; that is RFC 0021's package content, not this RFC.
- **Trust:** an app answers only actions it declared; an answer is checked against what the
  action gives back (a library ID that exists) before the asker sees it.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. In-process actions | The registry and requests inside the desktop app; Library pick and Setlist send between the built-in apps | From the Setlist app, fill a slot through the Library's pick; from the Library, send a Performance to the setlist |
| 2. Apps as processes | The shell starting an app from its manifest with a connection the engine grants; `apps.request`, `apps.answer`, the `apps` topic; supervision: restart, the hang rule, the memory limit, stderr captured; the Setlist app as its own executable | The same two flows with the Setlist app in a separate process; the Setlist app killed, hung and leaking memory in turn: each time sound goes on, the shell says what happened, the app comes back, its stderr is in its log |
| 3. Choice and defaults | Two apps offering one action; the chooser and its setting | A second setlist app answers once chosen, and the choice is kept |
| 4. Permissions and limits | Grants per connection from the manifest; rate limits; file paths refused to apps | A test app granted Library only is refused `graph` and `params` with the permission named; one flooding requests is slowed, then disconnected, while another client's controls keep their timing |
| 5. Packages | App builds in archives and repositories, signed; install, update and removal; the data folder | An unsigned app asks before install; an update applies only once the app is closed, and one that fails to start is rolled back; removal keeps its data unless asked |
| 6. Sandboxing | Landlock, seccomp and devices denied on Linux; the sandbox profile on macOS; the device's display path | A test app cannot open the control socket's path, `/dev/snd`, another app's data folder or the network without `network`; on the device it cannot read another app's input or pixels |
| 7. Built-in apps out | Library, Groovebox, Recordings, the launcher, Settings, then Performance, each its own executable ([order](#the-built-in-apps)) | Each gate of RFC 0030's steps 1 and 2 passes with that app in its own process; the desktop process is the shell alone |
| 8. Third-party apps | A sequencer interface over the engine's sequencer plugin, then a ported DAW on the audio node, both packaged | Installed from a repository on the device, played from its pads, killed mid-song with the Performance still sounding |

Step 2 needs nothing from packages: the manifest is read from the app's folder. Step 5 needs
RFC 0006's signatures (built) and RFC 0031's step 1. Third-party apps are offered on a device
only after steps 4 and 6; on the desktop after step 4, with the sandbox's absence stated until
step 6.

## Open questions

- Whether an answer can return several items (pick three Sounds for a split).
- How a device without a touch screen chooses between two apps.
- Whether actions belong in the control API's capability list for clients that are not apps.

Decided by the owner, 2026-11-07:

- **A ported DAW's audio:** an audio node in the engine's graph, the DAW keeping its own engine and
  plugins (the recommendation). Still open below: whether a DAW's own plugins may run on a device.
- **The device's display:** the Wayland shell comes first. Third-party apps on devices wait for it,
  rather than running in the meantime as signed-only. It covers **plugin windows from workers as
  well as apps** (2026-11-07): a worker showing a plugin's own GUI (CLAP's GUI extension) gets a
  surface from the shell as an app does, natively where the plugin speaks Wayland and through an X
  server of the worker's own for X11 plugins (JUCE's on Linux), so a plugin's own interface (a
  skeuomorphic one, or an AWM2-style element editor) is shown on devices too, contained in its
  worker ([RFC 0019](0019-adaptive-ui.md#plugin-windows)).
- **Platforms:** no platform is an absolute requirement for an app. Building for every platform
  is encouraged, and Oscairn should make CI for that easy to set up (a template, as the package
  repository's is). First-party apps keep RFC 0006's requirement of macOS and Linux ARM64.
- **A developer mode,** on the desktop and on devices when turned on explicitly: an app started
  by hand connects with full access through the user's socket, skipping packaging and
  permissions.
- **A ported DAW's own plugins** may run on a device too, with no restriction beyond the app's
  own containment (its memory and CPU reservation, the hang rule, recycling).
- **Unsigned apps on the KICK** are refused by default, as unsigned plugin builds are, unless the
  user explicitly allows one.
- **Apps survive an engine restart:** the shell supervises apps (start, limits, restart, stderr),
  not the engine, asks the restarted engine for each app's connection and passes it over
  (`SCM_RIGHTS`); the app library reconnects and resyncs, as the desktop's own connection
  already does.
- **Performance moves out of the shell's process last,** after Setlist, Library, Groovebox,
  Recordings, the launcher and Settings.
- **The hang timings** are 5 seconds for the app in front ("not responding", Wait or Close) and 10
  for one behind (killed), to be revisited from measurements on the device.
- **Built-in apps update from the repository too,** between images, as ordinary signed apps; the
  image's version stays as the fallback when an update does not start.
- **Background control** is a permission: an app behind may keep the pads and knobs when the user
  grants it `background` (a sequencer driving pads while another app is shown); without it the
  controls follow the screen.

## Ruled out

- **Apps linking to each other directly:** none could be replaced.
- **Passing content between apps:** the Performance is shared through the engine; actions pass
  references.
- **A preferred vendor:** Oscairn's apps answer only by being the default the musician keeps.
- **Apps as modules loaded into the shell** (libraries, or plugins with screens): nothing could
  contain a crash or a leak, and a DAW is not a plugin.
- **Web apps only:** ported DAWs are native code.
- **Apps opening sound cards, MIDI ports or input devices themselves:** the engine owns them, so
  the knobs follow the screen, panic reaches everything and an app cannot stop the sound.
- **Untrusted apps sharing one X server:** each could read the others' input and pixels.
- **A ported DAW taking the audio device from the engine.**

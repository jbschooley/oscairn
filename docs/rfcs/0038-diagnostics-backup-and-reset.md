# RFC 0038: Diagnostics, backup and reset

Status: proposed, 2026-11-07. Required for every product Oscairn runs on; the desktop takes
the parts that apply to it. Builds on reliability levels ([RFC 0027](0027-reliability-levels.md)),
plugin worker failures ([RFC 0010](0010-plugin-worker-isolation.md#failure-handling)), apps and
their supervision ([RFC 0022](0022-apps-and-requests.md#containment)), resource budgets
([RFC 0025](0025-resource-budgets.md)), the system image and its factory reset
([RFC 0028](0028-system-image-updates-boot-and-power.md)), the shell's dialogs and banners
([RFC 0030](0030-system-shell-launcher-and-navigation.md)), network control and the web UI
([RFC 0026](0026-network-control-and-web-ui.md)), the library and its export
([RFC 0021](0021-sounds-performances-and-library.md), the [archive format](../library-archive.md)),
packages and the trust store ([RFC 0006](0006-packages-and-portability.md#signatures),
[RFC 0031](0031-package-repository-and-distribution.md)), device profiles
([RFC 0024](0024-device-profiles.md)) and internationalization
([RFC 0029](0029-internationalization.md)). Time handling is decided in RFC 0035 (being
written); this RFC stamps everything with monotonic, boot-relative time and leaves wall time
to it.

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Logs in RAM: every process's output captured by its supervisor, one line format, levels, a cap per source
- [ ] 2. Bounded persistence: warnings and errors kept across a reboot or a power cut, the last boots kept, flash writes measured
- [ ] 3. Crash reports: a minidump for any crashing process, symbolized off the device from a symbol store per release
- [ ] 4. The diagnostics screen: versions, storage, recent problems and the deadline-miss history
- [ ] 5. The support file: one action, reviewed before it is written, saved to USB, the computer or the web UI
- [ ] 6. Backup and restore of the whole device, onto the same device, another model or the desktop
- [ ] 7. Secrets in a backup: keys, pairings and network credentials, encrypted with a passphrase
- [ ] 8. Factory reset: settings only, or everything, with an erase as thorough as the hardware allows

## Problem

When something goes wrong on an instrument, nobody can find out why. Each process writes
free-form lines to wherever its stderr happens to point, with no level, no time and often no
name of the process; nothing is bounded; a crash leaves only "signal 6". A user cannot hand a
developer anything useful, cannot move their setup to a new instrument, and cannot wipe one
before selling it. RFC 0028 names a factory reset in one paragraph and RFC 0022 says app logs
are kept, but nothing ties logs, crashes, backups and reset into one design that respects the
flash, the audio path and the user's privacy.

## Outcome

- Every process's output lands in one place in RAM, in one format, bounded per source, and the
  part worth keeping survives a reboot or a power cut without wearing out the flash.
- A crash in the engine, a worker, the shell or an app leaves a small report that a developer
  can turn into function names, with no private data beyond what the user is shown.
- One action makes a **support file** the user reviews, then saves or sends. Nothing leaves the
  device on its own.
- One action backs up the whole device; the backup restores onto the same device, a new one, a
  different model or the desktop, and back.
- Factory reset erases what it says and keeps what it says, and says how thorough the erase is.
- Settings › About shows the versions, the storage, recent problems and the audio deadline
  misses.

## Current state

What the code does today (2026-11-07):

| Process | Where its output goes | Format | Bounded |
| --- | --- | --- | --- |
| Desktop app (`hosts/standalone`) | Its own stderr: a terminal, or nothing when started from the Finder. On the device, `oscairn-session` appends it to `/var/log/oscairn.log` | Free text, mostly `oscairn: …`, no level or time | No; never rotated |
| Engine helper (`hosts/engine`) | `EngineProcess` opens `<runtime dir>/engine-<hash>.sock.log` (append, private) and points the helper's stdout and stderr at it. The runtime dir is `OSCAIRN_RUNTIME_DIR`, `$XDG_RUNTIME_DIR/oscairn` on Linux, or `/tmp/oscairn-<uid>` | `oscairn-engine: …` and `oscairn: …` | No; never rotated |
| Plugin workers | `WorkerHost` passes only the socket (fd 3) and leaves fd 2 alone, so a worker's stderr is the engine's: its lines and its plugin's land in the engine's log, not marked with the worker or plugin | Whatever the plugin prints | No |
| X server | `/var/log/oscairn-xorg.log` | Xorg's | No |
| Apps | Designed (RFC 0022: last 64 KiB per app, appended to `Logs/apps/<id>.log`, rotated at 1 MB), not built: RFC 0022 step 2 is open | | |
| Kernel | Its ring buffer (`dmesg`) only | | Yes, the kernel's |

- **Failures:** a worker's crash, hang or slowness is detected and named (RFC 0010 step 3:
  "Plugin worker crashed (signal 6)") and its restarts counted; `session.status` lists failed
  plugins. Nothing records a stack, and an engine crash leaves nothing but the desktop
  reconnecting.
- **Deadline misses:** each plugin host counts blocks whose `process` took longer than the
  block's time (`Statistics::deadlineMisses` in `core/clap-host`, and the worker's own count in
  `WorkerHost`); `session.status`'s `deadlineMisses` is their sum over the nodes in the graph at
  that moment. It has no history or times and falls when a node is removed. The audio device's
  own underruns and overruns are `audioUnderruns` beside it (RFC 0035), since the device started.
- **Measurements** (RFC 0025) are kept in `measurements.json` in the data folder.
- **Backup:** none. The library exports items as `.olib` archives with their samples and plugin
  builds; the [architecture](../architecture.md) promises "one bundle format, three scopes: a
  selection, a user export, and a full backup", and RFC 0006 speaks of full backups as a
  retention choice, but nothing gathers settings, the trust store, keys or app data.
- **Factory reset:** RFC 0028 step 3, not built.
- Not checked here: whether the image runs BusyBox's `syslogd` and `klogd`, and whether its
  `/var/log` is on the root file system or in RAM. Step 1 settles it on the QEMU image.

## Logging

### Options

| | How | For | Against |
| --- | --- | --- | --- |
| A. The system logger | BusyBox `syslogd`, which can keep its log in a shared-memory ring read with `logread` (`-C`), or rotate a file by size (`-s`, `-b`) ([BusyBox](https://busybox.net/downloads/BusyBox.html)) | Already in BusyBox; the kernel's messages through `klogd` | One ring for every source, so a chatty plugin pushes the engine's lines out; processes must call `syslog()`, and a plugin's stderr does not |
| B. systemd's journal | Structured, rate-limited, size-capped | Mature | The image uses BusyBox init; taking systemd for logs alone is a large change to boot (RFC 0028's boot targets) |
| C. Supervisors capture, Oscairn keeps | Each supervisor already owns its children's start: the session starts the shell and the engine, the engine its workers (RFC 0010), the shell its apps (RFC 0022). Each reads its children's stderr from a pipe and writes their lines into a ring per source in RAM | A cap per source; every line marked with who wrote it, including a third-party plugin that only prints; the same code on the desktop and the device | Code of our own (small: a pipe reader and a ring) |

**Recommended: C,** with the kernel's messages read from its ring buffer when needed rather
than copied continuously. If the image later moves to systemd for other reasons, the rings
can feed the journal instead; nothing above the supervisors changes.

### Where lines go

- **In RAM:** a folder on a RAM file system (`/run/oscairn/log/` on the device; the runtime
  folder on the desktop), one ring file per source: `engine`, `shell`, `session`,
  `worker-<plugin id>`, `app-<id>`. Each ring is a fixed size (proposed: 256 KiB for the engine
  and shell, 64 KiB for each worker and app, as RFC 0022 gives apps) and wraps; the total is
  capped by the profile (proposed 4 MiB) and counted in the system's share of memory
  (RFC 0025).
- **Rate:** a source writing more than a set rate (proposed 200 lines a second) has the rest
  dropped and one line saying how many. A flooding plugin fills only its own ring.
- **The audio path never logs** (RFC 0027). The audio thread counts (misses, drops, overruns) in
  atomics it already has; the engine's service tick turns changes into lines.
- **Workers:** the engine gives each worker a pipe as its stderr, read off the audio and
  control threads, so a plugin's prints are marked with that plugin. A worker that stops reading
  cannot stall: the pipe is non-blocking on the engine's side and a full pipe drops.
- **Desktop:** the same rings, and bounded persistence (below) writes to `Logs/` in the data
  folder. The engine helper's `.sock.log` becomes its ring; the desktop app's own stderr is
  captured by itself into the `shell` ring.

### The line

```
+12345.678 W engine[812]: plugin com.example.organ@2.1.0 missed 3 blocks in 1 s
```

- **Time:** seconds since boot from the monotonic clock, to the millisecond. The ring's first
  line names the boot (a random boot ID, made at start) and the Oscairn and image versions.
  When the device learns the wall time (per RFC 0035), the session writes one line pairing
  monotonic and wall time, so a reader can convert the lines after it, and before it within
  that boot; a device that never learns the time shows boot-relative times only. No time is
  guessed (RFC 0030 rules out a clock from a guess).
- **Level:** `E` error, `W` warning, `I` info, `D` debug. Debug is off unless turned on in the
  diagnostics screen, and turns itself off at the next boot.
- **Source** and process ID, then the message, in American English. Logs are for developers and
  are not translated (RFC 0029 keeps protocol and formats in English); what the user reads
  about a problem comes from an event with a code, below.
- **Existing `oscairn: …` lines** keep working: a line without a level is `I`, or `E` when the
  process's own code marks it so. Moving each `fprintf(stderr, …)` to a level is done as the
  code is touched, not in one sweep.

### Problems as events

Some lines matter to the user: a plugin failed, the engine restarted, memory was short, a
package was refused. Those are also **events**: a code (`plugin.failed`, `engine.restarted`,
`audio.deadlineMissed`, `app.crashed`, `memory.short`, `storage.low`), the boot ID and time,
and parameters. They feed the shell's banners (RFC 0030), the diagnostics screen's recent
problems, and are drawn through the catalogs (RFC 0029). The engine and shell keep the last 200
in RAM; they persist with the warnings below.

### Kept across a reboot

Instruments are often switched off at the wall, so a clean shutdown cannot be relied on.

- **Warnings, errors and events** are appended to `Logs/system.log` on the data partition,
  batched, at most once a minute while there are any, and at once for a crash. They are rare
  in normal use, so the writes are small. Rotated at 1 MiB, two kept.
- **A crash or watchdog restart** also saves the last 64 KiB of the failed process's ring and
  the engine's next to its crash report (below).
- **A clean shutdown or update** saves every ring's tail to `Logs/boots/<boot id>/`; the last
  three boots are kept.
- **Kernel panics:** where the board has RAM that survives a reset, the kernel's ramoops keeps
  its panic and oops logs across it ([kernel docs](https://docs.kernel.org/admin-guide/ramoops.html)),
  configured by a reserved-memory node in the device tree; the session moves them into
  `Logs/boots/` at the next start. A board without it loses a panic's log.
- **Budget:** `Logs/` is capped at 8 MiB on the device (proposed), oldest first out. Bytes
  written to flash per hour of play are measured in step 2 and stated per product.

## Crash reports

### Options

| | What it keeps | For | Against |
| --- | --- | --- | --- |
| A. Core dumps | All of the process's memory | Everything a developer could want | Hundreds of megabytes for an engine holding locked samples (RFC 0025), slow to write, full of the user's data; never on the device |
| B. Minidumps | Each thread's registers and stack, the loaded modules with their build IDs, the signal | Small; a standard format with tools to symbolize; [Crashpad](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md) writes them from a handler process apart from the crashed one, on macOS and Linux among others ([status](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/status.md)) | Stacks can hold fragments of data (a name being drawn, a path) |
| C. A backtrace from a signal handler | Return addresses and modules | Tiny, no dependency | Unreliable from inside a process that has just corrupted itself; plugin code built without frame pointers unwinds badly |

**Recommended: B,** written outside the crashed process, in one of two ways chosen by step 3's
measurements:

1. **A system collector** for every process: the kernel runs a program named in `core_pattern`
   when it starts with `|`, giving it the core on its standard input
   ([kernel docs](https://docs.kernel.org/admin-guide/sysctl/kernel.html)). `oscairn-crash` reads
   the stream, keeps only the threads' stacks, registers and the module list, writes a minidump
   and discards the rest. One mechanism covers the engine, workers, the shell, apps and the X
   server with no code in any of them.
2. **An in-process client** (Crashpad's) for Oscairn's own processes, if the collector makes
   an engine restart too slow: the kernel streams the whole core before the process is gone,
   and the watchdog's restart time (RFC 0027) waits on it.

The gate decides: an engine crash with a large sample set loaded restarts within its stated
time with crash capture on. The collector is tried first because it is one mechanism and
catches processes we do not build.

- **Bounded:** the collector runs at low priority on cores apart from audio, with a time limit
  (proposed 5 s, after which the report is marked incomplete), a memory limit, and its own
  stderr into the `session` ring. It reads untrusted input (a core of an untrusted app) and
  checks every size it reads.
- **Kept:** `Logs/crashes/<boot id>-<source>-<n>/`: the minidump, a `crash.json` (process,
  plugin or app ID and version, signal, boot-relative time, uptime, reliability level, profile,
  the open Performance's plugin list by ID and version, not its name), and the ring tails.
  The last 10 crashes, at most 10 MiB.
- **Desktop:** on macOS the system already writes a report for a crashing process; the desktop
  links to it rather than installing a handler of its own (where macOS keeps them is to be
  checked in step 3). On Linux desktops, the system's own `core_pattern` belongs to the
  distribution, so the in-process client is the only option there.

### Symbols

- Every release build splits its debug information from its binaries, keyed by the GNU build
  ID on Linux and the UUID on macOS. CI stores them per release in a symbol store, kept for as
  long as any device may run that release. Images and desktop builds ship stripped; no symbols
  on the device.
- Symbolizing happens off the device, from a support file. Third-party plugins' and apps'
  frames show as module and offset; their publishers symbolize them with their own symbols.
- Where the store is hosted (beside the package repository, RFC 0031, or as release artifacts)
  follows the repository's hosting.

### Privacy

- No heap, no samples, no Performance or Sound documents, no keys. Stacks are kept, so a report
  can hold fragments of what was on them; the review (below) says so in plain words.
- Paths under the user's home folder are written as `~/…` in reports and logs (a desktop's
  `/Users/<name>/` names the user).
- Reports stay on the device. They leave only in a support file the user makes.

## The support file

One action, **Create support file**, in Settings › About (and from a crash banner's Details):

1. **Choose what goes in**, with each part's size, all on by default except where noted:

   | Part | What |
   | --- | --- |
   | System | Oscairn and image versions, the A/B slot, kernel, profile ID and version, reliability level, uptime, storage and memory use |
   | Logs | The rings now, `system.log`, the last boots' tails |
   | Crashes | The reports above |
   | The open Performance's plugins | Each plugin's format, ID, version, build digest and signer state; not the Performance |
   | Resources | `session.status` (load, workers' CPU and memory, failures), the deadline-miss history, `measurements.json` |
   | Installed packages | IDs, versions and sources |
   | The open Performance itself | Off by default: its document, so a developer can reproduce; no samples |

2. **Review:** each file can be opened and read on the screen (or in the web UI) before
   anything is written. The screen says what may be in them: names of Performances and Sounds
   in log lines, fragments of memory in crash reports.
3. **Save:** to a USB drive, through a paired computer (RFC 0026, System permission), or as a
   download in the web UI. On the desktop, a file dialog.

- **Format:** a zip, `oscairn-support-<device name>-<boot id>.zip`, with a `support.json`
  listing every part and file. It is not a library archive: the library reader refuses it, by
  design.
- **Never in it:** `Keys/`, private keys of any kind, paired clients' keys, the device's TLS key,
  network passwords, samples.
- **No upload.** The user attaches the file to an issue or an email. Whether a product may offer
  to send it, and where to, is the owner's question below; until decided, there is no endpoint
  and no code that sends.

## Backup and restore

### What counts

| What | Where today | In a backup | Notes |
| --- | --- | --- | --- |
| The user's own Performances, Sounds, setlists, takes | `Performances/`, `Sounds/`, and the item types' folders | Yes, as the library exports them | Revisions and IDs kept, so a restore merges like an install |
| Samples | The sample library, by hash (RFC 0009) | Yes, by hash, once | The largest part. Samples an installed package provides can be left to it (below) |
| Installed libraries and packages | `Libraries/<id>/`, `Plugins/`, `Apps/` | **A list by default:** ID, version, digest, source. **Whole** when asked (a complete offline backup), or for a package with no known source | RFC 0006's three states: available in this backup, downloadable, no known source; shown before writing |
| Settings | Settings files in the data folder (and on Linux desktops the theme folder under `XDG_CONFIG_HOME`) | Yes | Profile-specific ones marked with the profile they came from |
| Favorites, recent, hidden tags | The library cache's `user.json` | Yes | The index (`index.json`) is a cache, rebuilt; left out |
| Measurements | `measurements.json` | Yes | Restored only onto the same profile: they measure this hardware (RFC 0025) |
| App data | `AppData/<id>/` (RFC 0022) | Yes, per app, with the app's version | An app may name a cache folder that is left out; it migrates its own data on first start, as after an update |
| Trust store | `trust.json` | The user's entries | The product's own entries come from the image (RFC 0006) and are not restored over a newer image's |
| Repositories | `repositories.json` | Yes | |
| Signing keys | `Keys/` | **Only encrypted** | Without a passphrase they are left out and the backup says so |
| Paired clients, the device's TLS identity | RFC 0026, not built | **Only encrypted** | Whether a restore takes over the old device's identity is an open question |
| Network credentials | Not built | **Only encrypted** | |
| Logs, crashes, caches | | No | |

### The format

Options:

- **A. A new kind of library archive,** `backup`, in the [archive format](../library-archive.md):
  the same zip, manifest, per-file SHA-256, path rules and limits, with roots added for this
  kind (`Libraries/`, `Apps/`, `AppData/`, `Settings/`, and `secrets.bin`), and a `packages`
  list of what is referenced but not included.
- **B. A format of its own.**

**Recommended: A.** The reader's safety rules, hashing and the folder form are already built
and tested; a backup's `Performances/`, `Sounds/`, `Assets/` and `Plugins/` read exactly as an
export's, so the desktop can open a device's backup and import items from it with the reader it
has. The folder form ([RFC 0031](0031-package-repository-and-distribution.md#installing-in-little-space))
suits USB: files are named by hash, so a second backup to the same folder writes only what
changed. The archive limits (8 GiB in all, 2 GiB an entry) are too small for some sample
collections; the `backup` kind needs larger ones or the folder form for large backups (an open
question).

### Secrets

- Left out unless the user gives a passphrase. With one, keys, pairings and network credentials
  go into one entry, `secrets.bin`: a key derived from the passphrase with Argon2 and the
  contents sealed with ChaCha20-Poly1305, both in Monocypher, which Oscairn already vendors
  for signatures ([manual](https://monocypher.org/manual/): `crypto_argon2`,
  `crypto_aead_lock`). The Argon2 parameters and a random salt are in the entry's header.
- Only the secrets are encrypted, so the rest stays readable by the ordinary library reader.
  Whether the whole backup should be encryptable (a lost USB drive holds the user's music) is
  an open question.
- A forgotten passphrase loses the secrets, not the backup; the screen says so when it is set.
- A wrong passphrase refuses the secrets before anything is written and offers to restore the
  rest without them.

### Restoring

The restore reads the whole backup first (every hash, as an install does), then shows what
will be restored, what will be fetched, and what is missing, and asks.

Order: settings that fit this device, the trust store, packages (from the backup, else from the
configured sources, a USB repository or a paired computer, RFC 0031), library items, app data
once its app is installed, then secrets.

- **The same device, or a new one of the same model:** everything, settings included.
- **Onto a device that has data:** items merge as an install into the user's storage does (IDs
  and revisions reconciled, divergent work kept, nothing overwritten); settings are replaced
  only when confirmed.
- **A different model** (another profile, RFC 0024): profile-specific settings (button and
  control mappings, layouts, the block size, measurements) are not applied and are listed;
  plugin builds and apps are taken for this platform from the backup or fetched; a Performance
  whose plugin has no build here opens with a stand-in, as an import does today.
- **Desktop to device and back:** the same archive both ways. A desktop's backup carries
  macOS builds (and a device's, if its store has them); the device installs only its own
  platform's (RFC 0006). Desktop-only settings (windows, audio device names) do not apply on a
  device, and the reverse.
- **Where:** a USB drive; a paired computer with System and Library permissions (RFC 0026),
  which can keep a device's backups; the web UI as a download and an upload.
- Never while playing (RFC 0030's stage safety).

## Factory reset

RFC 0028 already says: restore the factory partition's settings and library, ask whether to
keep the user's data, a second confirmation to clear it. This RFC fills in the two choices:

| | Reset settings | Erase everything |
| --- | --- | --- |
| System image (A/B) | Kept, the running one | Kept, the running one |
| Factory library and settings | Restored from the factory partition | Restored from the factory partition |
| Settings, network, pairings, the chosen launcher | Back to factory | Erased |
| The user's library and samples | Kept | Erased |
| Installed packages and apps | Kept | Erased |
| App data | Kept | Erased |
| Trust store | The user's entries kept | Back to the image's |
| Keys | Kept | Erased |
| The device's TLS identity | Kept | Made anew (paired clients pair again) |
| Logs and crashes | Kept | Erased |

- **The system image stays** the one running: going back to the version the device shipped
  with would bring back what later images revoked (RFC 0031).
- **Packages go with "Erase everything":** they can include paid content tied to the owner
  (RFC 0031), and an erased device should carry nothing of its last owner. Whether a third
  choice keeps them is an open question.
- **Confirmation** (RFC 0030's dialogs): "Erase everything on <device name>?", with what goes
  listed and **Back up first** offered; then a second step held for three seconds (a press
  and hold, on a button or the screen), so it works on a device without a keyboard. Refused
  while playing. From a paired client it needs the System permission and a press on the device
  itself.
- **When the UI cannot start:** a product needs a way to reset from the boot (a key held at
  power-on into a recovery screen). Which key is the product's (an open question per profile).

### Secure erase

Deleting files, or overwriting them, does not reliably remove data from flash: the storage's
controller spreads writes across blocks and keeps old copies until it reuses them, so a file
overwritten in place may still be readable from the chips. Discard ("trim") tells the
controller blocks are free; it does not promise they are erased. eMMC has erase commands of its
own, whose support and behavior vary by part, and SD cards fewer. So:

- **Every product:** erase removes the files, reformats the data partition and discards it.
  The screen calls this "erased", not "securely erased".
- **Products that encrypt the data partition** with a random key (Linux's dm-crypt) erase by
  destroying the key, which makes what is left unreadable whatever the controller kept. This is
  as strong as where the key lives: in the SoC's or a secure element's key storage it is strong;
  in a file on the same flash it has the same problem in a much smaller form. Such products may
  say "securely erased".
- Encrypting the data partition costs CPU on sample streaming and time at boot; it is the
  product's choice in its profile, measured before it is turned on (an open question).

## The diagnostics screen

**Settings › About** (in the Settings app, RFC 0022; the same in the web UI, RFC 0026; System
permission for the actions):

- **Versions:** Oscairn, the image and its slot, the kernel, the profile and its version, the
  reliability level, the device name and serial where the profile has one.
- **Storage:** the data partition's use by kind (library, samples, packages, apps and their
  data, logs and crashes, backups kept on the device), and free space. A warning event below a
  floor (proposed 5%).
- **Memory and load:** the budget view RFC 0025 already builds.
- **Recent problems:** the events, newest first, each in the user's language with its time: "12
  minutes ago" in this boot, "the boot before" when the wall time of that boot is unknown, a date
  when RFC 0035 has one. A crash's Details shows its log tail.
- **Audio:** the deadline-miss history. The engine keeps, in RAM, a bucket per minute for the
  last 24 hours (1,440 buckets) of the misses of each plugin, from the counts the hosts already
  keep (above), and of blocks the graph missed as a whole, a count the engine does not keep yet;
  read on the service tick, never on the audio thread. Shown as a strip with the plugins that missed most and which Performance was
  open; the audio device's own overruns are added where the backend reports them. The day's
  summary persists with the warnings.
- **Actions:** Create support file, Back up, Restore, Reset; Debug logging until restart.

## Control API

Under the System permission (RFC 0026), names proposed: `system.info`, `system.events`
(and an `events` topic), `system.logs` (a source's ring, from a point), `system.crashes`,
`system.supportFile` (parts in, a file out to a destination), `system.backup`,
`system.restore` (read, then apply), `system.reset`. The Settings app and the web UI use these;
nothing here bypasses the control API.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Logs in RAM | Supervisors capture stderr into rings per source: the session (shell, engine), the engine (each worker), the shell (apps, with RFC 0022 step 2); the line format; levels; rate limits | A worker printing as fast as it can fills only its own ring; the engine's lines are all there; no missed audio block; total RAM within the cap. On the desktop and the QEMU image. The image's existing logger and `/var/log` checked and recorded here |
| 2. Persistence | `system.log` batched; tails on crashes and clean shutdowns; three boots; ramoops where the board has it | Power cut during play (RFC 0028's harness): after it, the warnings up to a minute before the cut are there. Bytes written to flash per hour of play and idle measured and stated |
| 3. Crash reports | The collector, minidumps, `crash.json`, the symbol store in CI | The crashing fixtures in a worker, the engine and an app each leave a report that symbolizes off the device to function names. An engine crash with a large sample set restarts within RFC 0027's stated time with capture on (else the in-process client). A marker string placed in the heap is not in the report |
| 4. The diagnostics screen | Versions, storage, events, the deadline-miss history | The slow fixture's misses appear in the minute they happened, against that plugin; versions match the image |
| 5. The support file | The action, the review, saving to USB, a paired computer and the web UI | Each part turned off is absent; no `Keys/` or key bytes; home paths written as `~`; a marker in a Performance's content absent unless that part is chosen |
| 6. Backup and restore | The `backup` kind, written as an archive or a folder; restore with its plan | Desktop to a fresh data folder: the library hash-identical, settings, app data and the trust store's user entries back, packages fetched from a USB repository. Desktop to the QEMU image: profile-specific settings skipped and listed. A second backup to the same folder writes only what changed |
| 7. Secrets | `secrets.bin` with a passphrase | A wrong passphrase writes nothing; restored keys sign; no key bytes readable in the archive without the passphrase |
| 8. Factory reset | Both choices, the confirmations, the erase | On the QEMU image: after Erase everything, none of the seeded files' contents is found in the data partition's raw image when encrypted, and the result reported as best effort when not; the factory library opens; refused while playing |

## Open questions

Decided by the owner, 2026-11-07: **encrypting a backup is the user's choice** at backup time (the
whole backup, or only its secrets); keys, pairings and credentials are always encrypted when
included.

Decided by the owner, 2026-11-07: **no upload for now.** Crash reports and support files are saved to
USB or a paired computer only; hosting, retention and an opt-in upload are left for later.

- **Telemetry and sending reports (the owner's call):** may a product offer, opt-in only and off
  by default, to upload crash reports or support files? If so, to whom (the project, the
  manufacturer), hosted where, kept how long. RFC 0031 rules out telemetry to the package
  repository; this RFC proposes none until decided.
- **Encrypting the whole backup** instead of only its secrets.
- **Device identity on restore:** whether restoring onto a replacement device takes over the old
  one's TLS identity and pairings (no re-pairing) or always pairs again.
- **Keeping packages through Erase everything** as a third choice.
- **Encrypting the data partition** per product, for an erase that holds, against its cost on
  streaming and boot.
- **Names in logs:** log lines name Performances and Sounds; whether the support file offers to
  replace them, or the review is enough.
- **Large backups:** raise the archive limits for the `backup` kind, or require the folder form
  past them.
- **Automatic backups** to a USB drive or a paired computer, on a schedule or when plugged in.
- **The recovery key** for a reset when the UI cannot start, per profile.
- **Desktop logs:** in the data folder's `Logs/`, or in each platform's own place for logs.
- **RFC 0022's app logs:** it puts them at `Logs/apps/<id>.log` and also says "Logs under it"
  of an app's data folder. This RFC proposes the first for what the shell captures; what an app
  writes itself stays in its data folder. RFC 0022 should be made to agree.

## Ruled out

- **Logging on the audio thread,** in any form.
- **Wall-clock times guessed** when the device does not know the time.
- **Core dumps** kept on the device or put in a support file.
- **Symbols on the device.**
- **Anything leaving the device without the user's action:** no automatic upload, no
  background reporting.
- **A cloud account** for backups (RFC 0026 rules one out for pairing and sync).
- **Private keys in a backup without encryption.**
- **A factory reset that puts back an older system image.**
- **systemd only for its journal:** too large a change to boot for what supervisors already
  see.

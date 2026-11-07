# RFC 0037: Storage and file transfer

Status: proposed, 2026-11-07. Required for every product Oscairn runs on as its own operating
system, and for the desktop where it says so. Builds on sound assets
([RFC 0009](0009-sound-assets-and-dependencies.md)), the library
([RFC 0021](0021-sounds-performances-and-library.md)), packages
([RFC 0006](0006-packages-and-portability.md), [RFC 0031](0031-package-repository-and-distribution.md)
and the [library archive](../library-archive.md)), recording ([RFC 0020](0020-recording.md)), apps
([RFC 0022](0022-apps-and-requests.md)), resource budgets ([RFC 0025](0025-resource-budgets.md)),
network control ([RFC 0026](0026-network-control-and-web-ui.md)) and the system image
([RFC 0028](0028-system-image-updates-boot-and-power.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Drives read-only: FAT32, exFAT, ext4 and NTFS mounted safely without a desktop; drives as
      Library sources; samples and `.olib` files imported from them
- [ ] 2. Writing to drives: exports and recordings, Eject, and a drive pulled midway
- [ ] 3. The file picker: apps get content from a drive as store identities, never paths
- [ ] 4. Storage accounting: what fills the data partition, a floor, admission and cleanup
- [ ] 5. The data partition's mount and wear: write patterns measured and bounded
- [ ] 6. The USB device: class-compliant audio and MIDI, with a network link, to a computer
- [ ] 7. Transfer through the web UI: upload packages and samples, download recordings and exports
- [ ] 8. Desktop and device sync, on RFC 0026's library sync

## Problem

Musicians move files all the time: samples from a computer, a pack bought online, a backup
before a tour, last night's recording to a DAW. On a computer that is the operating system's
job. On an Oscairn device it is Oscairn's: the image boots straight into the app with no desktop
environment, nothing mounts a USB stick, and nothing lets a computer reach the device's files.

Pieces exist, each deciding a little:

- **Packages** install from a folder or an `.olib` read in place on a USB drive or a share, with a
  free-space check and a move into place by renames (RFC 0031 step 1, built in part). Nothing
  mounts the drive for it, and the gates for a pulled drive and the free-space refusal are not
  tested.
- **Recordings** may go to "an `Oscairn/Recordings` folder on a chosen SD card or USB drive" and
  handle a full disk and a pulled card (RFC 0020), but nothing chooses the drive.
- **The system image** puts the user's data on its own journaled partition (RFC 0028), not yet
  built.
- **Apps** may never pass file paths (RFC 0022), and the `files` permission names "drives through
  the shell's file picker", which is not designed.
- **Library sync** with a computer is one line of RFC 0026, step 6, not designed further.
- **Cleanup** of plugins and assets nothing uses is designed in RFC 0006 and not built ("grouped
  cleanup", [library archive](../library-archive.md)).
- **Free space** is not budgeted anywhere. RFC 0025 budgets CPU and memory; its profile field
  `compute.storage` is read by nothing.

## Outcome

- A USB stick or SD card plugged into a device appears in the Library as a source, read-only
  until something needs to write, mounted so nothing on it can run. Pulling it out loses nothing
  that was on the device and, while nothing was writing, nothing on the drive.
- Plugged into a computer, the device is a class-compliant audio interface and MIDI device, and
  also a small network link over which the computer reaches the web UI and sync. Files move
  through the library, by hash, not by exposing a folder.
- The user data partition never fills by surprise: installs, imports and recordings are refused
  before a floor, with what to remove; cleanup shows what is unused and how much it frees.
- Writes to the device's flash are measured and kept small when idle.
- The desktop keeps the same data folder layout, and syncs with a device without either losing
  divergent work.
- An app gets a file the user picked as a store identity, never as a path.

## Current state

What is built, as of 2026-11-07:

- **The data folder** is `OSCAIRN_DATA_HOME`, else `~/Library/Application Support/Oscairn` on
  macOS, else `$XDG_DATA_HOME/oscairn` or `~/.local/share/oscairn` on Linux
  (`dataHome()` in `core/control/src/library.cpp`; `core/plugin-host/src/assets.cpp` the same for
  `assets/`). It holds:

  | Path | What | Written by |
  | --- | --- | --- |
  | `Performances/`, `Sounds/` | The user's items, one folder each | Saving |
  | `Libraries/<id>/` | Installed locked libraries | Installs |
  | `library/` | The library's index (a cache) and the user file (favorites, tags) | Scans, edits |
  | `assets/` | The asset registry `libraries.tsv`, `index/<id>.tsv` per library, and the `user` library of imported samples | Imports, registrations |
  | `Plugins/<format>/<id>@<version>/<platform>/` | The plugin store | Installs, `plugins.addBuild` |
  | `recordings/` | Takes, and the MIDI journal (planned) | The recorder |
  | `Keys/`, `trust.json` | Signing keys (private keys readable by the user only) and the trust store | Settings |
  | `Repositories/`, `repositories.json` | Package indexes kept per repository, and roots' serials seen | Package fetches |
  | `measurements.json` | Plugin measurements (RFC 0025) | The engine, at most every 30 seconds while they change |
  | `.install-*`, `.export-*` | Staging for installs and exports; a download's `.install-<sha256>.partial`, kept to resume | Installs; folders left by a crash are removed after a day |

  Planned: `Apps/` and `AppData/<id>/` (RFC 0022), with `Logs/apps/<id>.log` under it.
- **Atomic saves:** the library writes beside, `fsync`s the file and renames it into place;
  `measurements.json` and the trust store are renamed into place without an `fsync`. Nothing syncs
  the folder after a rename.
- **Installs from drives:** `library.contents`, `library.install`, `packages.list` and
  `packages.install` take an absolute path (or an address) on the engine's file system. An archive
  is read in place; an unpacked package folder is copied file by file and hashed as it is copied;
  before writing, the install checks the data folder's file system has room for every entry and
  the largest once more (`fs::space`, `core/control/src/package.cpp`).
- **Imports:** `assets.import` copies one file into the `user` library by hash; `assets.register`
  indexes a folder in place (RFC 0009). Both take absolute paths, as every control method that
  touches files does today; the local socket is a same-user interface, not a sandbox.
- **The image** (`os/`): one ext4 root file system of 512 MiB, read-write, with the session
  running Oscairn as root and `HOME=/root`, so the data folder is `/root/.local/share/oscairn` on
  the root file system. BusyBox init and eudev; no automounting, no partitions beyond the root,
  no USB gadget configuration. Buildroot's default `fstab` mounts `/tmp` and `/run` as tmpfs
  ([skeleton fstab](https://raw.githubusercontent.com/buildroot/buildroot/master/package/skeleton-init-sysv/skeleton/etc/fstab)).
  The kernel is Buildroot's QEMU `virt` configuration plus `board/qemu-aarch64/linux.fragment`;
  which file systems and USB functions it has was not checked here.
- **Profiles** give `compute.storage` in MiB (16384 for the KICK, the Ignite and the MODX M, as
  estimates) and `midi.usbDevice` and `usbHost`.
- **The web UI and network transport** (RFC 0026) are not built; the control API is a local
  socket only ([hosts](../../hosts/README.md)). Nothing transfers files over the network but
  package fetches.

## Removable media

### File systems

| File system | Where it is met | In mainline Linux | Proposal |
| --- | --- | --- | --- |
| FAT32 | USB sticks, SDHC cards (2 to 32 GB) as sold ([SD Association](https://www.sdcard.org/developers/sd-standard-overview/capacity-sd-sdhc-sdxc-sduc/)), older instruments | `vfat`, long-standing | Read and write |
| exFAT | SDXC and SDUC cards (over 32 GB) as sold ([SD Association](https://www.sdcard.org/developers/sd-standard-overview/capacity-sd-sdhc-sdxc-sduc/)), large sticks | Samsung's driver, merged in Linux 5.7 as a replacement for the staging one added in 5.4 ([Kernel Newbies](https://kernelnewbies.org/Linux_5.7)) | Read and write |
| ext4 | Drives formatted on Linux | Long-standing | Read and write |
| NTFS | Drives formatted on Windows, external disks | `ntfs3`, a read-write driver added in Linux 5.15 ([Kernel Newbies](https://kernelnewbies.org/Linux_5.15)) | **Read-only** |
| HFS+, APFS | Drives formatted on a Mac | APFS is not in mainline | Not supported; the drive is shown as unreadable, with its type |

The image's kernel is 6.18, so all four drivers are available; the kernel configuration must turn
them on (`CONFIG_VFAT_FS`, `CONFIG_EXFAT_FS`, `CONFIG_EXT4_FS`, `CONFIG_NTFS3_FS`, the code pages
and UTF-8 for FAT names, USB mass storage and the MMC host for the board's card slot).

FAT32 holds no file over 4 GB ([Microsoft](https://learn.microsoft.com/en-us/training/modules/explore-windows-client-file-systems/2-examine-file-allocation-table-file-system)):
an export or a recording that would pass it is refused on that drive before it starts, saying
why (RFC 0020's long takes on FAT32 split or go elsewhere: its open question on RF64 or split
files stands).

NTFS is read-only because the use is "a drive from a Windows computer with samples on it"; writing
needs no NTFS when FAT32 and exFAT serve every computer, and a read-only mount cannot damage the
user's drive. Formatting a drive on the device offers exFAT, and FAT32 for a drive of 32 GB or
less on request (for older gear).

### Mounting without a desktop

A desktop environment mounts drives through udisks and a session's policy. The image has neither,
and needs neither: one small, privileged system service, **`oscairn-mounter`**, part of the image,
not of Oscairn's engine or apps.

- **Hot plug** reaches it through eudev: a rule for block devices with a file system runs it (or
  it listens to the kernel's uevents itself). It identifies the file system with `blkid`'s
  information, and refuses a type not in the table.
- **Where:** `/media/<volume UUID>`, never a label, so two sticks named `UNTITLED` do not collide.
  The label is for display.
- **How:** read-only by default, with `nosuid,nodev,noexec,noatime` always, and for FAT and exFAT
  an owner and masks that leave no file executable. `mount(8)` describes the three security
  options as not honoring set-user-ID bits and file capabilities, not interpreting device files,
  and not permitting execution of binaries ([man page](https://man7.org/linux/man-pages/man8/mount.8.html)).
  FAT's `errors=` defaults to remounting read-only on a critical error
  ([kernel documentation](https://docs.kernel.org/filesystems/vfat.html)); kept.
- **Never `sync`:** `mount(8)` warns that it "may cause life-cycle shortening" on flash with
  limited write cycles. Writes are flushed explicitly instead (below).
- **Checking:** before a writable mount, a read-only check (`fsck.fat -n`, `fsck.exfat -n`,
  `e2fsck -n`; NTFS is never mounted writable). A drive with errors mounts read-only and the
  Library says "This drive has errors. Repair it?", which runs the repair only on that yes.
  Whether the check is fast enough on a large card is measured in step 1.
- **Telling Oscairn:** the mounter keeps the list of drives (UUID, label, type, size, free,
  read-only or not, mount point) and announces changes on a socket under `/run`; the engine
  relays them as a `drives` topic. Only the engine and the mounter know mount points; clients see
  drives by UUID (below, [the file picker](#the-file-picker)).
- **Writable on demand:** a job that writes (an export, a recording to the drive, formatting)
  asks the mounter to remount read-write, and the mounter remounts read-only once the job ends
  and its files are flushed. A drive is writable only while something writes.

### Ejecting, and a drive pulled out

- **Eject** is in the top bar's quick settings ([RFC 0030](0030-system-shell-launcher-and-navigation.md))
  while a drive is in, and beside the drive in the Library's sources. It is refused while a job
  writes to the drive, naming it, with **Stop and eject**. Otherwise the engine closes what it has
  open there, the mounter flushes and unmounts, and the shell says "Safe to remove". On a headless
  unit, from the web UI, or the profile's button for it.
- **Pulled while read-only** (the usual case): nothing on the drive is at risk. What was reading
  fails: an install leaves nothing behind (staging removed, as built); a sample being auditioned
  stops; a Sound whose registered folder was on the drive becomes a missing dependency (RFC 0009),
  and RFC 0006's rule holds that an absent drive's pins are kept, not taken as disuse. The mounter
  lazily unmounts the vanished device.
- **Pulled while writing:** every file written to a drive is written under a temporary name
  (`.oscairn-partial-<name>`), flushed (`fsync`) and renamed into place, then the folder flushed;
  a pull leaves at most one partial file, which the next mount of that drive removes. A
  recording to a drive keeps RFC 0020's rule: files that can be closed are closed, the take is
  marked failed with the reason, and the rings still hold the recent audio for another drive.
  The FAT or exFAT structures themselves may be damaged by a pull mid-write; the next mount's
  check catches it (above).
- **The package installer already moves into place by rename** within the data folder, so a
  drive pulled during an install changes nothing installed. RFC 0031's step 1 gate for it is
  this RFC's step 1 gate too, run on the image with a real (emulated) drive.

### Browsing a drive

- **The Library** lists each drive as a source beside the library's own ("SANDISK, 28 GB free"),
  as the architecture's [user data](../architecture.md#user-data) says ("SD cards and USB drives
  appear as sources"). It shows folders and the files Oscairn can use: audio files and patch
  files a plugin accepts (RFC 0009's table), `.olib` archives and unpacked package folders, and
  Performances and Sounds as folders. Other files are hidden unless **Show all files** is on
  (the architecture's opt-in Files view).
- **Audition** a sample from the drive before importing, read from the drive by the engine.
- **Import a sample:** copied into the `user` library by hash (`assets.import`, built). The copy
  survives the drive's removal, which is what a stage needs.
- **Use a folder in place:** register it as a library (RFC 0009's registered folders), recorded
  by the drive's volume UUID and the folder's path on it, so it resolves again when that drive
  returns at any mount point. Offered, not the default, and the Library marks such Sounds "on a
  removable drive"; the Setlist's readiness shows them not ready while the drive is out. A
  sampler streaming from a USB stick during a show is the musician's choice, said plainly.
- **Install an `.olib` or a package folder:** read in place (built), with the install dialog's
  choices and the trust store's rules for code.
- **A package repository on the drive** (RFC 0031's USB subsets) is recognized by its
  `repository.json` and offered as **Install from this drive**.

### Never running anything from a drive

- `noexec` on every removable mount, and no Oscairn code path loads a plugin, app or script from
  one: plugin builds and apps are installed into the data folder, verified, before anything runs
  them (RFC 0006, RFC 0022). Registering a plugin folder or an app folder on a drive is refused.
- `nosuid` and `nodev`, so a crafted drive cannot carry a set-user-ID program or a device node.
- File names from a drive are untrusted input: the engine validates every name it shows or opens
  (control characters, length, `..`), as the archive reader does.
- Drives are parsed by the kernel's file system drivers, which are an attack surface of their own;
  that is accepted for FAT, exFAT and ext4, which musicians need, and kept smaller by not
  offering more types.

## The device and a computer

### What a computer wants

To play through the device and record it; to put samples and packs on it; to take recordings
and backups off it; to edit from a laptop. The first is audio and MIDI; the rest are files.

### Options

**1. USB mass storage (the device as a drive).** The kernel's gadget can export a block device or
a file as a USB drive. Its documentation warns that a backing file "may not be modified by any
other process", because "the host assumes the data does not change without its knowledge"
([kernel](https://docs.kernel.org/usb/mass-storage.html)). So the device must unmount whatever it
exports for as long as the computer has it. Exporting the data partition would stop the library,
the engine's open Performance and recording for the duration: the instrument stops being one,
as Elektron's Octatrack does in its USB disk mode, where the card is handed to the computer and
must be unmounted there before leaving the mode
([manual](https://www.manualsdir.com/manuals/657165/elektron-octatrack.html?page=43)). A computer
yanked without ejecting can also leave the data partition damaged, and the computer would see
raw folders, bypassing hashing and the index (RFC 0021 rescans, so the files would appear, but
nothing would check them). A variant exports a separate small FAT volume as an "inbox" that the
device imports from after the computer ejects it: workable, but it costs a partition, needs the
eject, and does nothing a network link cannot.

**2. MTP.** A file-level protocol, so the device keeps its file system mounted; served in user
space through the kernel's FunctionFS, for example by uMTP-Responder, which Buildroot packages as
`umtprd` ([Buildroot commit](https://lore.kernel.org/all/20191202083933.CB69180DED@busybox.osuosl.org/T/),
[project](https://github.com/viveris/uMTP-Responder/wiki)). macOS has no MTP client of its own:
Google's Android File Transfer filled that gap and was hidden from android.com in 2024
([9to5Google](https://9to5google.com/2024/02/16/android-file-transfer-mac-hidden/)). Windows and
Linux clients were not checked here. MTP still exposes a folder tree, outside the library.

**3. A network link over USB, with the device's own services.** The gadget can add a USB network
function (NCM or ECM) beside audio and MIDI: the computer sees a network interface to the device,
and everything RFC 0026 offers works over it with no Wi-Fi: pairing, the web UI, library sync.
Ableton's Move has a Move Manager through which Sets are uploaded to it, and its release notes
say it "now works on Windows 10 via USB-C without manually installing drivers"
([release notes](https://www.ableton.com/en/release-notes/move-1/)); how it connects over USB was
not checked here.

**4. A network share (SMB) or WebDAV.** The computer mounts the device's folders. Finder connects
to WebDAV at an `http://` address ([Apple](https://support.apple.com/guide/mac-help/mchlp1546/mac));
Windows marked its WebClient (WebDAV) service deprecated in November 2023
([BetaNews](https://betanews.com/2023/11/06/microsoft-is-killing-off-three-windows-services-because-of-security-concerns)).
SMB means running Samba on an instrument (a large server, a second authentication beside
RFC 0026's pairing, and a folder tree writable from outside while the engine reads it). Both give
the computer paths into the data folder, which RFC 0026 rules out in spirit: "the network
carries the control API, nothing else".

**5. Transfer through the web UI.** Files go over RFC 0026's paired, TLS connection with the
Library permission: uploads into the library's own operations (install, import), downloads of
exports and recordings. Same permissions, same pairing, any browser, over Wi-Fi, Ethernet or the
USB link of option 3.

### Comparison

| | Instrument keeps playing | macOS, Windows, Linux | Through the library (hashes, index, trust) | New code on the device | Pairing and permissions |
| --- | --- | --- | --- | --- | --- |
| 1. Mass storage of the data | No | Yes | No | Small | None |
| 1b. Mass storage "inbox" | Yes | Yes | On import | Small, plus a partition | None |
| 2. MTP | Yes | Not macOS | No | A daemon | None |
| 3+5. USB network and the web UI | Yes | To be verified per system (gate) | Yes | The web UI (RFC 0026) | RFC 0026's |
| 4. SMB or WebDAV | Yes | SMB yes; WebDAV deprecated on Windows | No | A server | Separate |

### The composite USB device

Configfs builds a gadget from functions linked into one configuration
([kernel](https://docs.kernel.org/usb/gadget_configfs.html)). The kernel documents `uac2` (with
channel masks, sample rates and sizes, and asynchronous or adaptive sync per direction), `midi`
(ports in and out through the ALSA sequencer), `midi2` (a UMP endpoint with function blocks and a
legacy port), `ncm`, `ecm`, `mass_storage` and `ffs` (FunctionFS, for a user-space function such
as MTP) ([kernel](https://docs.kernel.org/usb/gadget-testing.html)).

**Recommendation:** on a profile with `usbDevice: true`, the device presents **one composite
device: UAC2 audio, MIDI, and NCM networking**.

- **Audio and MIDI** are the architecture's [USB audio and MIDI, both ways](../architecture.md#usb-audio-and-midi-both-ways):
  channels each way from the profile, the clock bridged by adaptive resampling there. This RFC
  only places them in the gadget; their design belongs to the hardware services
  ([RFC 0004](0004-hardware-services.md)). Whether `midi2` replaces `midi` follows MIDI 2.0's
  support in the engine.
- **NCM** gives the computer a link-local network to the device. RFC 0026's network control is
  off by default per interface; the USB interface is one, and a profile may turn it on by default
  there, since a cable is physical consent (an open question). Pairing still applies.
- **No mass storage and no MTP** in the first version (ruled out and open below).
- Files then move by the web UI (option 5) and sync (option 3), over USB, Wi-Fi or Ethernet
  alike.

Whether macOS, Windows and Linux bind NCM with their own drivers, and which of NCM and ECM each
needs, is not verified here; step 6's gate tests it on each.

### Through the web UI

Built on RFC 0026 steps 1, 2 and 5:

- **Upload a package** (`.olib` or a folder): streamed into staging as RFC 0031's streamed installs
  read an archive, hashed as it arrives, then the install dialog on whichever screen uploaded it.
  The upload never lands whole as a file first when it can be read as it streams; where it must
  (a zip's directory is at its end), it is staged and removed after.
- **Upload samples:** each into the `user` library by hash, as `assets.import`; a file the device
  already has is not sent (the browser hashes first and asks).
- **Download:** an export (the device writes the archive into staging and streams it), a take's
  files, a backup.
- **Resumable,** by hash and range, as RFC 0031's downloads resume.
- **Admission:** an upload is refused before it starts if it does not fit (below).
- HTTP on the same TLS server as the WebSocket, authenticated by the paired client's session;
  the Library permission for uploads and downloads, System for a system update (RFC 0028).

### Sync

RFC 0026's library sync, unchanged: a paired computer and device exchange indexes and send what
the other lacks, by hash, with the Library permission. This RFC says what it covers:

| Synced | Not synced |
| --- | --- |
| The user's Performances and Sounds (revisions as RFC 0021 keeps them; divergent edits kept as both, never overwritten, as the architecture's import rules say) | `Keys/` (private keys never leave the machine) |
| Samples they use, by hash | `trust.json`, `repositories.json`, `Repositories/` (each device's own trust and caches) |
| Installed libraries, by ID and version | `measurements.json` (each device's own, RFC 0025) |
| Plugin builds for the other side's platform, from the store, with their signatures | `library/` (the index: a cache, rebuilt) |
| Favorites and tags (RFC 0021: favorites sync, recents do not) | `AppData/` (an app's private state) |
| Recordings, only those a synced Sound uses, unless chosen | `.install-*`, `.export-*` |

Plugin builds and apps arriving by sync are installs: they meet the receiving side's trust store
as from a package. Sync writes nothing the receiver has not got room for (below).

## Internal storage

### Partitions

RFC 0028's layout, with what this RFC adds:

| Partition | File system | Mounted | Holds |
| --- | --- | --- | --- |
| Boot | as the board needs | not | The boot loader's files |
| System A, System B | read-only image (ext4 or SquashFS: RFC 0028's choice) | `/`, read-only | The image |
| Factory | read-only | read-only | The factory library and settings (RFC 0028) |
| User data | ext4 | `/data`, `nosuid,nodev,noatime` | `OSCAIRN_DATA_HOME` (`/data/oscairn`) |

- **The data partition takes the rest of the internal storage,** grown to fill it on first boot,
  so one image fits any size of eMMC.
- **`OSCAIRN_DATA_HOME` is set by the session** to the data partition's folder, instead of
  `/root/.local/share/oscairn`.
- **`noexec` is not set on the data partition:** plugin builds and apps are installed there and
  run from there (RFC 0006, RFC 0022). What protects it is that only verified installs put code
  there, and apps' sandboxes (RFC 0022 step 6) deny execution elsewhere.
- **Logs and temporary files** stay in RAM (tmpfs), bounded (below).
- **An SD card slot** on a product is removable media, never the data partition. A product that
  must keep its library on a card (no eMMC) says so in its profile; not designed here.

### What fills it

| What | Grows with | Bounded by |
| --- | --- | --- |
| Samples in `assets/` (the `user` library, and samples installs bring) | Imports, installs, sync | Admission (below); removal only when nothing uses them |
| Installed libraries, `Libraries/` | Installs | Admission; uninstall |
| Plugin builds, `Plugins/` (every platform's, for carrying on) | Installs, sync | Admission; RFC 0006's cleanup |
| Recordings | Takes, the MIDI journal | RFC 0020's 64 MiB reserve per drive, the journal's `quota`; admission before a take |
| `Performances/`, `Sounds/` | Saving | Small; never refused (below) |
| Apps and their data, `Apps/`, `AppData/` | Installs, the apps | Admission; a per-app limit (below) |
| Package caches: `Repositories/`, `.partial` downloads | Fetches | Kept indexes are small; a partial download is removed after a week or when space is needed |
| Staging, `.install-*` | Installs | Removed at the end of an install, or a day after a crash (built) |

Samples and recordings are most of it; a single large sample library can be gigabytes. A device
with 16 GiB (the profiles' estimate) holds the image, the factory library and perhaps 10 GiB of
the user's.

### Free space

- **`storage.status`** returns the data partition's size and free space, and the size of each row
  above, computed off the control thread and cached; shared bytes (a sample two libraries use)
  counted once, as RFC 0006 requires of removal estimates.
- **A floor** kept free: the larger of 256 MiB and 2% of the partition (proposed, to be measured).
  The floor is for what must never fail: saving the user's work, the trust store, settings and
  the index. ext4's reserved blocks are for root, and the session runs Oscairn as root today, so
  they would not hold Oscairn back; the floor is Oscairn's own.
- **Admission,** as RFC 0025 admits memory: an install, an import, an upload, a sync batch, a
  recording and an app's installation each check, before writing, that they fit above the floor
  (installs already check room for the entries and the largest once more; the floor is added).
  Refused with what does not fit and what could be removed ("needs 1.2 GB; 3.4 GB is unused:
  see Storage").
- **Saving is never refused** for the floor: a Performance is small, and losing an edit is worse
  than eating into the floor. When the floor itself is gone, saving says so before it fails.
- **Recordings** keep RFC 0020's rules (the 64 MiB reserve, a measured drive) and are refused to
  start when the expected length does not fit above the floor.
- **Warnings:** the shell shows a notice when free space falls under twice the floor.
- **Per-app limits:** an app's `AppData/` is measured and an app over a size declared in its
  manifest (a new field beside `memory`) is told, then refused writes. Whether the kernel enforces
  it (ext4 project quotas, to be checked on the image's kernel) or the shell measures it is open.
- **The profile's `compute.storage`** becomes the expected size for estimates on a desktop
  preparing for a device (RFC 0031's fetching for a device); the device itself reads its real
  partition.

### Cleanup

One **Storage** page in Settings, beside RFC 0006's Installed content:

- the rows above with their sizes;
- **Unused content:** RFC 0006's grouped cleanup (plugins and libraries nothing reaches, by its
  reachability rules, with the reclaimable space), and RFC 0009 step 4's garbage collection of
  samples no item, library or recording references, from the same roots;
- **Recordings,** largest and oldest first, deleted by the user only;
- **Plugin builds for other platforms,** which a device keeps only to pass on in exports: removing
  them frees space and the page says exports will then carry only this platform's build;
- **Caches:** partial downloads and repository indexes, removable at once.

Nothing is removed without the user's say, except staging and partial downloads as above. The
grouped prompt after a deletion is RFC 0006's.

### Wear

eMMC and SD cards wear with writes, and small rewrites cost more than their size. Rules:

- **Idle writes near zero.** Nothing writes periodically while nothing changes. Today
  `measurements.json` is rewritten at most every 30 seconds while measurements change; it moves to
  writing on a change of an estimate past a threshold and at shutdown.
- **Logs in RAM:** the session's and engine's logs go to tmpfs, rotated in memory with a bound.
  RFC 0022's per-app logs (`Logs/apps/<id>.log`, rotated at 1 MB) are the exception, kept on the
  data partition for crash reports; they are written only when an app writes to stderr.
  Whether Buildroot's `/var/log` is in RAM on this image was not checked; the image sets it so.
- **Mount options:** `noatime`; ext4's default 5-second commit and ordered data
  ([kernel](https://docs.kernel.org/admin-guide/ext4.html)) kept until measured; no `sync`; no swap
  on flash (RFC 0028's zram is in RAM). TRIM by a periodic `fstrim` rather than `discard`, which
  ext4 leaves off by default (same page); whether the board's eMMC benefits is measured.
- **Writes in large pieces:** recordings already write a second at a time (RFC 0020); installs
  write each entry once into staging and rename, never copy twice.
- **Atomic saves stay,** and gain the folder `fsync` RFC 0028's power-loss promise needs; the
  measurements and trust store gain the file `fsync` the library already has. Each save is then
  a little more than its size, not a rewrite of anything else.
- **RAM-backed temporary files count as memory:** RFC 0020's planned spill of notes since capture
  began goes to the engine's temporary folder, which on the image is tmpfs; it is counted in
  RFC 0025's budget or moved to the data partition with a bound.
- **Measured:** bytes written per hour idle, playing, and recording, from the block device's
  statistics, on every release (step 5's gate).

## The desktop

- **Where the data lives** stays what the code does: `~/Library/Application Support/Oscairn` on
  macOS, `$XDG_DATA_HOME/oscairn` (else `~/.local/share/oscairn`) on Linux, `OSCAIRN_DATA_HOME`
  overriding both. The layout is the same as a device's, so an archive, a sync or a copied folder
  means the same thing on both.
- **Large sample libraries stay where the user keeps them,** registered in place (RFC 0009), not
  copied into Application Support. Sync to a device copies the samples a synced item uses, by
  hash, into the device's `user` library.
- **Removable media** on the desktop are the operating system's: the desktop app imports through
  the system's file dialog, as it does today. The Library may list mounted volumes as sources
  (macOS's `/Volumes`), read through the same import operations.
- **Storage** shows the same page; the floor applies to the data folder's volume.
- **Sync** is the desktop as one more paired client (RFC 0026's open question on the desktop
  moving to the WebSocket transport decides how it connects).

## The file picker

RFC 0022 gives apps store identities, never paths, and names a `files` permission that reaches
"drives through the shell's file picker". The picker:

- **An action, `oscairn.pick.file`,** offered by the Library (an app), asked by any app with
  `files`. It takes the kinds of file wanted (audio, a patch format, MIDI files, a package), and
  whether a folder may be picked. It opens as RFC 0022's picks do, a sheet over the asker, in the
  Library's view of the drives and the library's own samples.
- **It gives back content, never a path:**
  - **a file:** imported into the `user` library by hash, and the asset reference returned
    (`{library: "user", path, sha256}`), so the app holds what any plugin holds;
  - **a folder:** registered as a library (RFC 0009), by volume UUID on a drive, and the library's
    ID returned;
  - **a package:** installed by the Library under the install dialog's rules, and the installed
    items' IDs returned; the asking app never installs code itself.
- **Reading the content** is by reference: `assets.open(reference)` gives the app a read-only file
  descriptor over its local control connection (`SCM_RIGHTS`, as the shell already passes
  connections to apps), never a path. A sandboxed app's Landlock rules (RFC 0022) need not grant
  it any drive or the store's folders for this; the `files` permission's read-only grant of the
  library's folders becomes unnecessary and is dropped.
- **Drives in the control API** are for the Library and Settings, not other apps: `drives.list`
  and the `drives` topic (UUID, label, type, sizes, read-only), `drives.browse` (a folder's
  entries by name and kind, under the drive's UUID and a path relative to its root, which the
  engine resolves and checks stays inside the mount), `drives.eject` and `drives.format`. A path
  relative to a drive is a name to show and to ask about, not something any client can open.
  These need a new `drives` grant held by the Library and Settings, so an app with `files` sees
  a drive only through the picker.
- **Methods that take an absolute path** (`assets.import`, `assets.register`, `library.install`,
  `library.contents`, `packages.*` with a folder) accept a drive's UUID and relative path as well,
  and refuse absolute paths from an app's connection (RFC 0022 step 4); the user's own socket
  and the developer mode keep paths.
- **On the desktop,** until RFC 0022's macOS sandbox, the picker may be the system's file dialog
  shown by the shell, the result handed over the same way, by reference.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Drives read-only | The kernel's file systems and code pages on; `oscairn-mounter` with eudev; read-only mounts with `nosuid,nodev,noexec,noatime`; the `drives` topic and `drives.list`/`browse`; drives as Library sources; audition, import, install and register from a drive | On the QEMU image with an emulated USB drive in each of FAT32, exFAT, ext4 and NTFS: it appears in the Library within 2 seconds; a WAV imports and plays after the drive is removed; an `.olib` installs with free space of only its installed size plus its largest entry, and pulling the drive midway leaves the device as it was (RFC 0031's step 1 gates); an executable and a set-user-ID file on the drive cannot run; a device node on an ext4 drive cannot be opened; an HFS+ drive is shown as unreadable |
| 2. Writing to drives | Writable on demand, the temporary-name rule, Eject, the check before a writable mount, formatting as exFAT or FAT32; exports and RFC 0020's recordings to a drive | An export to a drive pulled midway leaves the drive's other files intact and its partial file removed at the next mount; a drive with errors mounts read-only and is repaired on request; Eject is refused while a take writes there; a 5 GB export to FAT32 is refused before it starts; after a job the drive is read-only again |
| 3. The file picker | `oscairn.pick.file`, `assets.open` by descriptor, the `drives` grant, drive references in path-taking methods, absolute paths refused to apps | A test app with `files` picks a WAV on a USB drive and receives an asset reference whose content it reads by descriptor; it cannot list the drive, open `/media`, or call `drives.browse`; an app without `files` is refused the action with the permission named |
| 4. Storage accounting | `storage.status`, the floor, admission for installs, imports, recordings and uploads, the Storage page, RFC 0006's grouped cleanup and RFC 0009's garbage collection | With the data partition filled to the floor: an install and a take are refused with the reason and the reclaimable space; a save still succeeds; removing unused content frees what the page said, counting shared samples once; nothing referenced is removed |
| 5. The data partition and wear | RFC 0028 step 1's partition mounted as above, `OSCAIRN_DATA_HOME` set by the session, logs in RAM, the folder and file `fsync`s, measurements written on change | Idle for an hour with a Performance loaded, the data partition takes under 1 MiB of writes (proposed bound); playing an hour of a setlist, under 10 MiB; RFC 0028's power-cut tests pass with the folder syncs |
| 6. The USB device | The composite gadget: UAC2, MIDI and NCM, from the profile, on the first real board | Plugged into macOS, Windows and Linux computers with no driver installed: each plays audio through the device, records it, sends and receives MIDI, and reaches the device's address over the USB link; the sound does not stop when the cable is pulled |
| 7. Web UI transfer | Uploads (packages, samples), downloads (exports, takes), resume, admission, on RFC 0026 steps 1, 2 and 5 | From a browser over the USB link: an `.olib` uploads and installs with peak storage of its installed size plus one entry; a sample the device has is not sent again; an upload cut midway resumes; an unpaired browser gets nothing |
| 8. Sync | RFC 0026 step 6 with this RFC's scope | A desktop and a device each edit one Performance offline and sync: both revisions are kept; samples cross once by hash; private keys, trust and measurements stay where they were; a plugin build arriving by sync meets the receiving trust store |

Steps 1 to 4 need nothing from the network. Step 3 needs RFC 0022 step 2 (apps in processes) for
a real cross-process gate, and its step 4 for refusing paths. Step 5 needs RFC 0028 step 1.
Step 6's gate runs on a real board: whether QEMU can stand in for a USB device controller here
was not checked. Steps 7 and 8 need RFC 0026's transport and
pairing.

## Open questions

- **Network control on over USB by default:** may a profile turn on RFC 0026's network control for
  the USB network interface out of the box, since a cable is physical, while Wi-Fi stays off?
- **MTP later:** worth adding for computers without a browser handy, given macOS lacks a client?
- **A mass-storage "inbox":** worth a partition for gear-to-gear use (a computer-less transfer from
  another instrument's workflow), or never?
- **SMB for advanced users,** read-only, for Recordings only? The comparison says no; a studio user
  dragging takes into a DAW may say yes.
- **Recordings on the desktop:** stay in the data folder, or default to a folder in `~/Music`
  that the user can see?
- **Recordings in sync:** all by default, none, or chosen per take?
- **NTFS writable:** ever, or read-only for good?
- **The floor's size** and the idle-write bounds, from measurements on a board.
- **Per-app storage limits:** kernel project quotas, or the shell measuring?
- **Backups to a drive:** the architecture's "full backup" bundle scope has no format yet;
  whether it is a library archive of everything or a format of its own.
- **Products whose only storage is an SD card:** the data partition on a removable card, and what
  happens when it is pulled.

## Ruled out

- **Exporting the data partition as USB mass storage:** the instrument would stop while it is
  connected, and the computer would write past the library's hashes and index.
- **Running anything from removable media,** plugins, apps or scripts, or registering a drive's
  plugin folder.
- **Mounting drives read-write all the time,** or with `sync`.
- **Paths to apps,** drives' mount points included.
- **A desktop environment's automounter** (udisks and its session policy) on the device: a small
  service of the image's own does what is needed.
- **Silent cleanup:** nothing the user installed, imported or recorded is removed without a yes.
- **Writing logs and caches to flash on a timer.**

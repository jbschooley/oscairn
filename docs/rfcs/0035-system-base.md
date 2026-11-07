# RFC 0035: The system base: real-time tuning, security, licenses, the SDK, time and services

Status: proposed, 2026-11-07. Required for every product Oscairn runs on as its own operating
system. Builds on the [Oscairn OS images](../../os/README.md) (Buildroot, the ARM64 QEMU image),
hardware services ([RFC 0004](0004-hardware-services.md)), plugin workers
([RFC 0010](0010-plugin-worker-isolation.md)), apps ([RFC 0022](0022-apps-and-requests.md)),
device profiles ([RFC 0024](0024-device-profiles.md)), resource budgets
([RFC 0025](0025-resource-budgets.md)), network control
([RFC 0026](0026-network-control-and-web-ui.md)), reliability levels
([RFC 0027](0027-reliability-levels.md)), the system image
([RFC 0028](0028-system-image-updates-boot-and-power.md)), the shell
([RFC 0030](0030-system-shell-launcher-and-navigation.md)), and packages and signatures
([RFC 0006](0006-packages-and-portability.md), [RFC 0031](0031-package-repository-and-distribution.md)).
Bluetooth is [RFC 0036](0036-bluetooth.md), which applies this RFC's rule for
[optional hardware](#optional-hardware-built-per-product) to it.

Oscairn is becoming a Linux distribution built around instruments. The RFCs above say what runs
on it; this one covers the base they run on: how the kernel and CPU are set up for audio, how the
image is kept secure and licensed, how third parties build for it, what it does about time on
hardware with no clock, how its processes are started, and how a product leaves out hardware it
does not have.

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Measuring latency: `cyclictest` in a test image, the engine's audio-device underruns in `session.status`
- [ ] 2. A `PREEMPT_RT` kernel on `instrument` and `pass-through` images, with threaded interrupts
- [ ] 3. Audio cores: an isolated cpuset partition, `nohz_full`, interrupts kept off them, the engine and workers on them
- [ ] 4. Frequency and idle states held on the audio cores while anything sounds; the priority table
- [ ] 5. The supervisor: `oscairn-session` replaced by a small program under BusyBox init, engine and shell as siblings
- [ ] 6. Time: the time state, its sources, the saved floor, and each feature's behavior without a known time
- [ ] 7. Security: a CVE report per image, the kernel and Buildroot version policy, security images on a cadence
- [ ] 8. Licenses: `legal-info` per image release, published with the image; a Licenses screen; the written offer
- [ ] 9. The SDK: a sysroot and toolchain per image, a CMake toolchain file, a CI template for every platform
- [ ] 10. Optional hardware built per product: build options, profile consistency, capabilities reported, a test that a feature left out leaves nothing behind

## Problem

The image boots into Oscairn and plays (os/README.md), and RFC 0028 says how it updates, stores
data and survives power loss. Below that, nothing is decided:

- The kernel is low-latency (`CONFIG_PREEMPT`), not real-time, and nothing reserves cores, steers
  interrupts, holds the CPU's frequency or measures the result. RFC 0027 asks for "a real-time
  kernel, cores reserved for audio, locked memory" at the `instrument` level; only the locked
  memory exists.
- No one tracks security problems in the image's packages, and nothing says how often a fixed
  image ships or who signs it (RFC 0028 leaves the signer open).
- The image is mostly GPL and LGPL software. Nothing collects its corresponding source, publishes
  it, or shows the device's licenses to its owner.
- RFC 0006 says device builds use "their actual Oscairn Buildroot SDK", and RFC 0022 wants CI for
  every platform easy to set up. Neither the SDK nor the template exists.
- Instruments like the MONTAGE and MODX keep no time, and Oscairn devices may not either. The
  code assumes the clock is right: a recording's folder is named by the wall clock whatever it
  says.
- One shell script started by BusyBox init runs X, the app and a restart loop. RFC 0022, 0025 and
  0027 now plan an engine, a shell, apps, workers and network services supervised separately.
- A pedal with no Bluetooth or screen would carry the code and libraries for them.

## Outcome

Every product's image is tuned for audio and measured on its hardware, tracked for security and
updated on a schedule, ships the source and notices its licenses require, comes with an SDK that
builds for it, works with no clock and says when it does not know the time, starts its processes
under one supervisor with the restart rules the other RFCs set, and contains only the subsystems
its hardware has.

## The image today

From `os/buildroot/` and `os/scripts/`, 2026-11-07:

| Part | Today |
| --- | --- |
| Buildroot | 2026.08.1 (`os/scripts/container-build.sh`), as an external tree; downloads hash-checked (`BR2_DOWNLOAD_FORCE_CHECK_HASHES`) |
| Toolchain | Buildroot's own: GCC, glibc, C++; tuned for Cortex-A76/A55 (`BR2_cortex_a76_a55`), so ARMv8.2 |
| Kernel | Linux 6.18.7, Buildroot's QEMU `aarch64-virt` config plus `board/qemu-aarch64/linux.fragment`: ALSA, the sequencer, the dummy and virtio sound cards, and `CONFIG_PREEMPT` |
| Init | BusyBox init (no `BR2_INIT_*` set, so Buildroot's default), eudev for device nodes |
| The session | `/etc/init.d/S95oscairn` starts `/usr/libexec/oscairn-session` with `start-stop-daemon`. It raises `RLIMIT_MEMLOCK` to unlimited, starts Xorg, then loops: start `/usr/bin/Oscairn`, wait, restart after 2 seconds. The app starts the engine, which detaches from its process group |
| User | Everything runs as root (`HOME=/root`), so `SCHED_FIFO` and `mlockall` need no limits raised beyond `RLIMIT_MEMLOCK` |
| Real-time threads | Workers' audio threads call `makeRealtime` (`core/plugin-host/src/Worker.cpp`): `SCHED_FIFO` priority 70 on Linux. No code sets CPU affinity. What priority the engine's own audio thread gets on Linux (it is JUCE's ALSA thread) was not checked |
| Memory | Locked at `instrument` and above when `OSCAIRN_RELIABILITY` says so (RFC 0025, built); `oscairn-session` sets the unlimited `RLIMIT_MEMLOCK` they need and the boot test checks it. The cgroup subtree, `OSCAIRN_CGROUP` and zram are not wired in the image |
| Logs | `/var/log/oscairn.log` and `/var/log/oscairn-xorg.log`, appended, never rotated |
| Network, time, TLS | No network services, no NTP, no TLS library (RFC 0031 step 2 notes HTTPS on Linux waits for one) |
| Licenses | The `oscairn` package declares `AGPL-3.0-or-later` (plus ISC for jalv with LV2, OFL-1.1 for the Inter font with the app); `oscairn-juce` declares `AGPL-3.0` |
| Tests | `os/scripts/boot-test.py`: boots in QEMU, checks the app runs, the engine opened audio, the memory limit and a screenshot; `compare-audio.py` checks a recording against an offline render |

There is no image for real hardware yet. Every gate below that needs a device is unverified until
the RK3588 board image or the KICK exists (os/README.md, "Next").

## Real-time tuning

### Where things stand

`CONFIG_PREEMPT` makes most kernel code preemptible but leaves interrupt handlers and spinlocked
sections running at their own pace. The QEMU image's numbers say nothing about latency: QEMU
schedules the guest's CPUs as host threads, as RFC 0010's container measurements found ("the
numbers show the VM's scheduling"). The engine already counts what matters to the music:
`session.status` reports `cpuLoad` (the callback's share of the block) and `deadlineMisses`, but
the latter sums plugin workers' misses and "is not an audio-device underrun counter"
([control protocol](../control-protocol.md)). Nothing counts the ALSA underruns themselves.

### The kernel

`PREEMPT_RT` is in mainline since Linux 6.12: Thomas Gleixner's pull request for 6.12-rc1 enabled
it "on the supported architectures ARM64, RISCV and X86(32/64-bit)"
([pull request](https://lists.openwall.net/linux-kernel/2024/09/20/15),
[Linux 6.12 release notes](https://kernelnewbies.org/Linux_6.12)). The image's 6.18 has it with
no patches. In mainline's `kernel/Kconfig.preempt` (read on the current tree, not 6.18's) it depends on
`CONFIG_EXPERT` and "enforc[es] interrupt threading", replacing spinlocks with preemptible,
priority-inheriting locks.

| Option | For | Against |
| --- | --- | --- |
| `CONFIG_PREEMPT` (today) | Nothing to do; widest driver compatibility | Interrupt handlers and spinlocked sections are not preemptible: worst-case latency unbounded by design |
| `CONFIG_PREEMPT` with `threadirqs` | Interrupts threaded, so their priority can be set | Spinlocked sections still block; a half measure |
| `CONFIG_PREEMPT_RT` | Bounded worst case is its purpose; mainline, so no patch set to carry; threaded interrupts by default | Some drivers behave differently under it (a board's out-of-tree drivers most of all); slightly lower throughput |

**Recommendation:** `PREEMPT_RT` on every `instrument` and `pass-through` image, set in a kernel
fragment shared by device boards (`CONFIG_EXPERT=y`, `CONFIG_PREEMPT_RT=y`, `CONFIG_NO_HZ_FULL=y`),
and on the QEMU image too, so the build and boot are tested even though its latency is not.
Boards whose drivers need a vendor kernel without working RT are not `instrument` products until
they have one.

### Cores for audio

A profile already says how many cores are for audio (`compute.realtimeCores`, RFC 0024). Nothing
puts the engine there or keeps other work off them.

| Option | What it does | Against |
| --- | --- | --- |
| `isolcpus=` (domain) | Removes cores from the scheduler at boot | The kernel's own documentation calls it irreversible and points to cpusets "for a dynamic configuration which can be altered at runtime" ([kernel parameters](https://www.kernel.org/doc/html/latest/admin-guide/kernel-parameters.html)) |
| cgroup v2 cpuset partition, `cpuset.cpus.partition` = `isolated` | A partition root "without load balancing", with exclusive use of its CPUs ([cgroup v2](https://docs.kernel.org/admin-guide/cgroup-v2.html)); changed at run time | The partition file "is not delegatable", so the supervisor (root) sets it, not the engine |
| `nohz_full=` | Stops the tick on a core running one task and offloads its RCU callbacks (kernel parameters) | Boot-time only; needs `CONFIG_NO_HZ_FULL` |
| `irqaffinity=` | The default interrupt affinity: interrupts start on housekeeping cores | Per-device interrupts that the kernel manages ignore it (`isolcpus=managed_irq` covers those, best effort) |

**Recommendation:** all of them except `isolcpus=`, which the cpuset partition replaces:

- The kernel command line for the board sets `nohz_full=` and `irqaffinity=` from the profile.
- The supervisor makes the cgroup subtree RFC 0025 designs with a cpuset: the engine and workers
  in an `isolated` partition on the audio cores; the shell, apps, network services and everything
  else on the rest. The audio interrupt is the exception (below).
- On big.LITTLE parts (the RK3588's four Cortex-A76 and four A55) the audio cores are big ones.
  The profile has to name which cores, not only how many: `realtimeCores` becomes a list
  (an open question, RFC 0024's schema).
- Workers inherit the partition from the engine; a worker's non-audio threads stay with it, which
  is acceptable since they are mostly idle while the audio thread runs.
- At the `desktop` level nothing is isolated; the operating system's own scheduling stands.

### Interrupts

Under `PREEMPT_RT`, interrupt handlers run as kernel threads (`irq/<n>-<name>`) with a priority
like any `SCHED_FIFO` thread. The supervisor, at start:

- moves every interrupt's affinity off the audio cores (`/proc/irq/<n>/smp_affinity_list`), except
  the audio interface's (the I2S or DMA controller, or the USB host controller for a USB
  interface), which goes to an audio core so its wake-up does not cross cores;
- raises the audio interface's interrupt thread above the engine's audio thread, and leaves the
  others at the default.

Which interrupt is the audio interface's is per board, so the board's profile names it.

### Priorities

One table, in the image, used by every process that sets a real-time priority. Values proposed,
to be set from measurements:

| Thread | Policy, priority | Cores |
| --- | --- | --- |
| Audio interface's interrupt thread | `SCHED_FIFO` 90 | audio |
| Engine's audio thread | `SCHED_FIFO` 80 | audio |
| Workers' and graph helpers' audio threads (RFC 0010, 0034) | `SCHED_FIFO` 70 (as `makeRealtime` sets today) | audio |
| Other interrupt threads | `SCHED_FIFO` 50 (the kernel's default) | housekeeping |
| An app's audio node thread (RFC 0022) | as workers, woken by the engine | audio |
| Engine control thread, shell, apps, recorder's writer (RFC 0020) | `SCHED_OTHER` | housekeeping |

RT throttling stays at the kernel's default, "0.05s to be used by SCHED_OTHER" in each second
([sched-rt-group](https://docs.kernel.org/scheduler/sched-rt-group.html)), so a runaway audio
thread cannot lock the device; the watchdog (RFC 0027) restarts the engine instead. Whether it is
then too blunt for an isolated core with nothing else to run is a measurement.

### Frequency and idle states

RFC 0028's power section already says "the cores reserved for audio keep their speed while
anything sounds". How:

- **Frequency:** the `performance` governor on the audio cores' frequency policy while anything
  sounds or the transport runs; `schedutil` otherwise. On parts where a cluster shares one
  policy, the whole cluster follows (unverified for the RK3588's mainline driver).
- **Idle states:** a deep idle state's exit latency lands in the audio deadline. Per core,
  `/sys/devices/system/cpu/cpu<n>/cpuidle/state<k>/disable` turns states off; a state shared by a
  cluster "must be disabled for all of them" to take effect
  ([cpuidle](https://docs.kernel.org/admin-guide/pm/cpuidle.html)). The engine instead opens
  `/dev/cpu_dma_latency`, which holds a latency limit for every core while the file is open, the
  cpuidle document says. The recommendation: per-core `disable` on the audio cores for states
  whose exit latency exceeds a tenth of the block, set by the supervisor, so housekeeping cores
  still sleep. Battery products (RFC 0024's `power`) release both when nothing sounds.

### Memory locking

Done for the engine and workers (RFC 0025's [Locking](0025-resource-budgets.md#locking)); the
image's part is `RLIMIT_MEMLOCK`, which `oscairn-session` already raises. When the supervisor
replaces the script it sets the limit with `setrlimit` for the engine and workers only, not for
apps, which RFC 0025 lets swap to zram. RFC 0028 keeps zram and the cgroup subtree; this RFC adds
nothing to them.

### Measuring

- **`cyclictest`**, in Buildroot's `rt-tests` package (`BR2_PACKAGE_RT_TESTS`, utilities for
  testing a Linux system's real-time behavior, as its help says; its `Config.in` notes `cyclictest` needs a
  non-musl toolchain, which glibc is). It goes in a test image variant, not the product image: one
  thread per audio core at priority 80, for hours, with a load on the housekeeping cores (storage,
  network, the UI redrawing, a sample library streaming) and the gate's figure its maximum.
- **The engine's own counters:** `cpuLoad` and `deadlineMisses` as today, plus a new
  `audioUnderruns` in `session.status`: underruns the ALSA device reported, the counter that says
  a listener heard a click. A device gate needs zero over its run.
- **Both together:** `cyclictest`'s maximum says what the system costs; the engine's counters say
  whether the Performance fits on top. RFC 0025 admission uses the second.

## Security maintenance

### Where things stand

The image is built from pinned versions with checked hashes, and nothing looks at whether they
have known vulnerabilities. Oscairn's own vendored code (`core/control/third_party`: miniz,
Monocypher, sheredom; `core/clap-host/third_party/clap`; `core/lv2-host/third_party/jalv`) and
JUCE are inside the `oscairn` and `oscairn-juce` packages, which carry no CPE identifiers.

### Tracking

Buildroot's `pkg-stats` reports "whether known CVEs (security vulnerabilities) affect the packages
in your current configuration" ([Buildroot manual](https://buildroot.org/downloads/manual/manual.html),
"Details about packages"). The script (`support/scripts/pkg-stats`) does it when given
`--nvd-path`, downloading the NVD database and matching each package by its CPE identifier or
name and version; matches are confirmed or "unsure", and a package's `<PKG>_IGNORE_CVES` lists
CVEs that a patch fixes or that do not apply. It writes `--html` and `--json` reports. The manual
also describes CycloneDX SBOMs "enriched with vulnerability analysis from the NVD database" (the
target that writes them was not checked).

**Recommendation:**

- CI runs `pkg-stats --nvd-path … --json` on every product defconfig weekly and on each release,
  and keeps the JSON beside the release. A new confirmed CVE opens an issue for triage.
- Triage counts what is reachable. Network control is off by default (RFC 0026) and many
  instruments are never online, so a remote hole in a service the image does not enable is low;
  one in the kernel's USB or Bluetooth stack, the TLS library or anything parsing files a user
  imports (archives, samples, MIDI files) is not.
- Oscairn's vendored libraries get a list of their own (name, version, upstream) and a check
  against their upstream advisories, since `pkg-stats` cannot see inside the `oscairn` package.
  CPE IDs on `oscairn-juce` where one exists.
- `<PKG>_IGNORE_CVES` entries need a one-line reason in the package file.

### Versions

- **Kernel:** a longterm (LTS) kernel per image series. kernel.org lists 6.18 as longterm,
  released 2025-11-30, projected end of life December 2028
  ([releases](https://www.kernel.org/category/releases.html)). Each security image takes the
  newest 6.18 point release. The image moves to the next LTS within a year of its release, once
  the real-time gates pass on it again; never to a non-LTS kernel.
- **Buildroot:** 2026.08.1 today, a quarterly release (the manual: "every 3 months, in February,
  May, August and November"). The download page lists 2025.02.x as Buildroot's long-term support
  series, end of life March 2028 ([downloads](https://buildroot.org/download.html)). Products
  in the field are better on an LTS series, which takes security fixes without new package
  versions. The recommendation: development follows quarterly releases; a product's first
  release moves to the newest LTS series and stays on it until its end of life. How long the next
  LTS series is maintained was not checked.

### Cadence and signing

- **Scheduled:** a security image monthly when the month's triage found anything affecting an
  image, through RFC 0028's A/B updates. Offered, never applied on its own and never while
  playing (RFC 0028).
- **Out of band:** within a week for an exploitable, reachable issue (the triage above), and the
  release notes say so, so an owner who keeps network control off can judge.
- **Who signs:** the project's images are signed by a release key that an offline image root key
  signs, the shape RFC 0031 uses for repositories (an offline root, an online signing key,
  rotation by the root). A third party's product's images are signed by its maker's key,
  trusted by its own devices; the project publishes base updates and its security notes, and
  makers rebuild. This answers RFC 0028's open question as a recommendation, for the owner to
  confirm.
- **Revocations travel with images,** as RFC 0006 and 0031 already have them, so a security image
  is also how an offline device learns of revoked keys.

## License compliance

### Where things stand

Oscairn itself is AGPL-3.0-or-later ([RFC 0001](0001-license.md)). Around it, the image is the
Linux kernel and BusyBox (GPL-2.0), glibc (LGPL), GCC's runtime libraries, X.org, ALSA, eudev
and fonts. Nothing collects source or notices. Plugin and app packages carry their own `license`
(an SPDX expression, RFC 0031) and package-level source fields are being built separately; this
RFC does not touch packages.

The `oscairn` package's `LICENSE_FILES` name Oscairn's license, jalv's ISC and the Inter font's
OFL, not the licenses of CLAP, miniz, Monocypher or sheredom vendored in the same tree. Their
notices would be missing from what `legal-info` collects (whether each requires one in a binary
distribution was not checked per library).

### What Buildroot gives

`make legal-info` collects "legally-relevant material in your output directory, under the
legal-info/ subdirectory", with a README listing "warnings about material that Buildroot could
not produce"; it saves no source for packages marked `<PKG>_REDISTRIBUTE = NO`, and "does not
produce some material that you will or may need, such as the toolchain source code" (manual,
"Complying with open source licenses"). The manual says its output must be checked before it is
relied on. Not checked here: whether it archives the source of a `local` site-method package
like `oscairn`, the rootfs overlay and the board scripts, which are part of what builds the image.

### What the licenses ask

From the license texts:

- **GPL-2.0, section 3** (the kernel, BusyBox): accompany the binary with the source, or with "a
  written offer, valid for at least three years, to give any third party … a complete
  machine-readable copy"; and "complete source code means all the source code for all modules it
  contains, plus … the scripts used to control compilation and installation". Offering the
  source from the same place as a download counts as distributing it
  ([GPL-2.0](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html)).
- **AGPL-3.0 and GPL-3.0, section 6:** the same choice in the newer form (object code from a
  designated place, with the Corresponding Source there or on another server with "clear
  directions"), and for a **User Product** (a consumer device) the source "must be accompanied by
  the Installation Information": the "methods, procedures, authorization keys, or other
  information required to install and execute modified versions"
  ([AGPL-3.0](https://www.gnu.org/licenses/agpl-3.0.html)). Oscairn is AGPL, so this applies to the
  Oscairn programs in every device image.

The last point meets RFC 0028's signed images and RFC 0022's "unsigned apps on the KICK are
refused by default": a device that runs only images its maker signed may not satisfy section 6
for the AGPL parts. RFC 0022 already lets a user explicitly allow an unsigned app; images need the
same: an owner-controlled way to trust their own image key (a key added on the device with the
owner's confirmation, as RFC 0006 adds keys). This is a reading of the license text, not legal
advice; it goes to counsel with RFC 0001's open question.

### Recommendation

- **Per image release, publish, don't offer:** the `legal-info` output, the Buildroot tree and
  the external tree at the release's commit, the kernel config, and the toolchain's source, as
  one archive beside the image wherever images are downloaded, named by the image version.
  The image's About screen says where. Images are downloaded (RFC 0028), so source from the same
  place is the simpler route.
- **Devices shipped with an image installed** are distributed without a download, so they also
  carry a written offer (on the device's Licenses screen and in the printed documentation),
  kept for three years after the last unit ships. Which of the two a maker uses for its own
  products is its choice; the project's images use both.
- **On the device:** Settings > About > Licenses lists each component in the image, its license
  and its notice, generated at build time from `legal-info`'s manifest and license files, plus the
  written offer and the source address. Plugins and apps installed later show their own package
  license in the Library (RFC 0031), not here.
- **Fix the `oscairn` package's license list:** every vendored library's license and license file
  in `OSCAIRN_LICENSE` and `OSCAIRN_LICENSE_FILES`, so `legal-info` and the Licenses screen are
  complete.
- **A check in CI** fails a release whose `legal-info` README has warnings that are not on a
  reviewed list.

## The SDK

### Where things stand

The architecture promises "a container image with the same cross-compilers as the OS images, and
a CI template, so anyone can build for every device from any computer"
([building and sharing plugins](../architecture.md#building-and-sharing-plugins)). RFC 0006 says
device builds use "their actual Oscairn Buildroot SDK/profile, not merely the label 'ARM64
Linux'". RFC 0022 decided no platform is mandatory for a third party but CI for every platform
should be easy. The package repository has a template (`tools/package-repository/template/`)
with a CI workflow (`scripts/ci.yml`). Oscairn's own CI builds Linux x86-64 and macOS
(`.github/workflows/ci.yml`); device builds happen only inside the Buildroot container.

### What Buildroot gives

`make sdk` exports "the toolchain and the development files of all selected packages, as an
SDK", a tarball whose user runs `relocate-sdk.sh` after unpacking; `make prepare-sdk` does the
same without the tarball (manual, "Using the generated toolchain outside Buildroot"). Buildroot
also writes a CMake toolchain file, `$(HOST_DIR)/share/buildroot/toolchainfile.cmake`, which its
own CMake packages use (`package/pkg-cmake.mk`); being in the host directory, it is in the SDK
(that it works from a relocated SDK was not tried).

The SDK's compilers run on the machine that built it. `os/build.sh` builds in Docker, which on an
Apple silicon Mac runs ARM64 Linux, so an SDK made there would run on ARM64 Linux hosts, not on
the x86-64 runners most CI uses (likely; not tried).

### Recommendation

- **One SDK per image version and target:** `oscairn-sdk-<runtime>-<image version>-<target>-<host>`,
  built by the release CI on an x86-64 Linux host, and on ARM64 Linux too where runners exist.
  The `runtime` is the profile's (`oscairn-buildroot-2026`, RFC 0024).
- **Targets:** `linux-arm64` first. Its baseline is the oldest core any device has: the KICK's
  Cortex-A53 is ARMv8.0 while the image is tuned for ARMv8.2 (os/README.md), so the SDK compiles
  for ARMv8.0 and a plugin built with it runs on every ARM64 device. A build tuned for a newer
  core is a second build declaring the instruction set (RFC 0006 already has the field).
  `linux-x86-64` only when an x86-64 product image exists; the desktop Linux build is not an
  image target (below).
- **What is in it:** the toolchain, the sysroot with the image's libraries and headers,
  `toolchainfile.cmake`, Oscairn's MIT SDK headers (CLAP extensions and helpers, RFC 0001) and the
  control client library for apps. It is also published as a container image, the "builder" the
  architecture names, for those who would rather not unpack it.
- **The CI template:** `tools/sdk/template/` beside the repository template, a GitHub Actions
  workflow a plugin or app copies, with one job per platform, each optional by deleting it:
  `linux-arm64` (the SDK, cross-compiled on an x86-64 runner), `macos-arm64` (Xcode on a macOS
  runner, with code signing as RFC 0006 keeps it), Linux x86-64 desktop (the runner's own
  compiler), and Windows when Oscairn hosts there. Its last job signs each build and packs a
  plugin package with `oscairn-sign` (the commands the repository template already documents),
  and, if asked, writes the release record for the package repository.
- **Versioning against images:** glibc and libstdc++ are backward compatible, so a build made
  with an older SDK runs on a newer image; the reverse is not promised. Each image states the
  oldest SDK whose builds it runs (in practice the series' first) and refuses builds whose
  declared runtime is newer than its own, through RFC 0006's runtime field, so "needs system
  update" is said before a crash. Within a runtime series (one Buildroot LTS line) glibc's
  version is not expected to change (to check against Buildroot's LTS policy). A new series (a new Buildroot LTS, a new glibc) is a new runtime name;
  the repository builds for both while devices on the old one are supported.
- **Desktop builds** use the platform's own toolchain, not the image SDK; the template's other
  jobs cover them.

## Time

### What instruments do

The MONTAGE and MODX keep no time:

- A reverse-engineering of the MONTAGE's file format says it "lacks an RTC, so to allow lists to be
  date-sorted, it uses an arbitrary 32-bit incrementing counter as a proxy for time stamps"
  ([Montage file structure](https://arachsys.github.io/montage/files/structure)).
- On Yamaha's forum, users (not Yamaha staff) answer that "there's no RTC in MODX/Montage so things
  like file dates are not going to be valid", and that the clock "will revert to the same
  date/time at each power cycle"; a MODX recording showed a creation date of 1/2/2000 on a
  computer ([yamahasynth.com](https://yamahasynth.com/community/modx-series-synthesizers/set-the-date-time-on-the-modx/)).
- RFC 0006 already assumes it ("a MODX keeps none") and gave signatures no expiry for that
  reason; RFC 0030 shows the clock only where the time is known.

No Yamaha manual saying so was found. How other instruments handle it (Korg, Roland, Elektron)
was not researched.

Linux systems without a clock use two known patterns: Debian's `fake-hwclock`, "a simple set of
scripts to save the kernel's current clock periodically" for machines that "don't have a working
realtime clock" ([Debian](https://packages.debian.org/stable/fake-hwclock)), and systemd's clock
epoch, which advances the clock at boot to the latest of its build time and a file timesyncd
updates ([systemd](https://www.freedesktop.org/software/systemd/man/latest/systemd.html)).

### Where things stand in the code

- A recording's folder is named by `std::chrono::system_clock` in local time
  (`Session::recordStart`, `core/control/src/session.cpp`), whatever the clock says: on a device
  with no time it would be named 1970 or wherever the clock started.
- Parameter-info revisions are seeded from the wall clock so "a new engine's revisions never match
  those a client still holds" (`core/control/include/oscairn/session.h`). If the clock is stepped
  back (a correction from the network), a restarted engine could repeat an earlier engine's
  revisions within one boot. A random seed or the boot ID would not depend on the clock.
- Library recents are an ordered list, not times (`core/control/src/library.cpp`), and RFC 0006
  already rules out timestamps for sync ("Do not use timestamps as last-writer-wins"). Both are
  already independent of the clock.
- The profile schema has no clock field yet; RFC 0030 proposes `"clock": "rtc"`, `"network"` or
  `"none"`.

### Recommendation: a decision for the owner

Oscairn works with no clock. It knows whether it knows the time, says so, and never presents a
guess as the time.

**The time state.** The supervisor keeps one state, read by the engine, the shell and apps
through the control API (`system.time`, with a notification when it changes):

- `known`, with its source (`rtc`, `network`, `paired`, `user`) and when it was set; or
- `unknown`, with a **floor**: the time is at least this. The floor is the latest of the image's
  build time and the last time saved on the data partition.

The kernel clock is set to the floor at boot when nothing better exists, so files and TLS see a
plausible, never-earlier-than-true time (a file saved today is stamped no earlier than the last
session, not 2000). Oscairn's own records do not treat it as the time.

**Sources, in order of preference:**

1. **A real-time clock** where the board has one with a battery (`"clock": "rtc"`): read at boot,
   and written back whenever a better source sets the time. An RTC reading before the floor (a flat
   battery) counts as `unknown`.
2. **The network** (NTP), when a network is up. BusyBox has an `ntpd` applet, enough for a first
   step (whether Buildroot's BusyBox configuration enables it was not checked). NTP is unauthenticated; authenticated NTP (NTS, RFC 8915) is a later choice. A
   wrong time does limited harm here because nothing security-critical depends on it (below).
3. **A paired device or the desktop** (RFC 0026): a paired client sends its time when it connects,
   and the device takes it when its own is `unknown`. Devices in one rig (RFC 0012) pass it on. The
   desktop app connected over USB counts: it is the only way "from the USB host" works, since
   USB audio and MIDI carry no wall time that we know of (MIDI Time Code is a position, not a
   date; unverified that no USB class carries one).
4. **The user,** in Settings > Date and time, which is the `user` source.
5. **The saved floor,** fake-hwclock style: the supervisor writes the time to the data partition
   every 10 minutes while it is `known`, and at shutdown. Writing more often wears flash; a power
   cut loses at most the interval.

**Times taken before the time was known.** Within one boot the monotonic clock runs from boot, so
anything stamped while `unknown` records the boot ID and seconds since boot. When the time becomes
known later in the same boot, those stamps are resolved (`known time − (now − then)`) and the
records updated: a recording made before the network came up gets its real date. Across a reboot
they cannot be resolved and stay undated.

**Each feature without a known time:**

| Feature | With the time known | With it unknown |
| --- | --- | --- |
| The clock in the top bar (RFC 0030) | Shown | Not shown, as RFC 0030 says |
| Recordings (RFC 0020) | Folder `2026-10-31 21-04-17 Open Road/`; `bext` origination date and time | Folder named by a per-device take number (`Take 0142 Open Road/`), renamed once resolved in the same boot; `bext` date and time left empty (whether common software accepts empty fields is to be tested); `TimeReference` is 0 for every file of the take, so they still line up; `take.json` records the boot-relative start |
| Library items' dates (RFC 0021) | Created and modified shown | Not shown; "newest first" sorts by a per-library sequence number every save increments (the MONTAGE's counter, kept also when the time is known) |
| File dates on disk and on a USB stick | The time | The floor, which the file system needs; on a computer it reads as the last known time, not as 2000 |
| Logs | Wall time on each line | Boot ID and seconds since boot on every line, wall time added once known; a boot counter orders logs across boots |
| TLS to the package repository (RFC 0031) | Normal checks | Chain and host name checked as usual; validity checked against the floor only: a certificate that expired before the floor is refused (it is expired whatever the time), one not yet valid at the floor is accepted. Package integrity never rested on TLS: blobs are checked by hash against a root signed with Ed25519 |
| TLS for pairing (RFC 0026) | Pinned fingerprints | The same: the device's certificate is pinned, not dated; one made while `unknown` takes the floor as its start |
| Signatures, revocation (RFC 0006, 0031) | No expiry, by design; rollback refused by serial | The same. Only the freezing attack (a stale mirror) goes undetected, as RFC 0031 already says; with the time known the shell shows the root's age |
| Engine revision seeds (`session.h`) | Not by the clock | Not by the clock (the fix above) |

**Devices with an RTC** use it as source 1 and otherwise behave the same, so the code has one path.

The owner decides: whether this is the design; whether the device may set the time from a paired
client without asking; and whether "time unknown" is shown anywhere beyond the missing clock
(Settings > Date and time, at least).

## Init and services

### Where things stand

BusyBox init runs `/etc/init.d/rcS`, which runs the `S??` scripts; `S95oscairn` starts a shell
script that loops on the app with a 2-second pause and no limit. The planned processes and who
supervises them, from the other RFCs:

| Process | Supervised by | Rule |
| --- | --- | --- |
| Engine | a watchdog outside it (RFC 0027) | heartbeat per block; restart and reopen the Performance |
| Workers | the engine (RFC 0010) | three restarts a minute |
| Shell | the watchdog; "sound goes on" (RFC 0025's out-of-memory order) | restart |
| Apps | the shell (RFC 0022, decided) | crash, hang and memory rules |
| Network services (NTP, mDNS, the WebSocket transport, Bluetooth) | not decided | |

The engine and shell must be siblings: apps survive an engine restart because the shell outlives
the engine (RFC 0022), and sound survives a shell restart because the engine outlives the shell.
Today the app starts the engine.

### Options

| Option | For | Against |
| --- | --- | --- |
| BusyBox init and shell scripts (today) | Small, fast, already there | No restart limits, no cgroups, no watchdog; a loop in a script is not supervision |
| BusyBox init, plus an Oscairn supervisor it starts with `respawn` | One small program does what the RFCs specify (cgroup subtree, cpusets, interrupt affinity, limits, heartbeat, the time state, logs); PID 1 restarts the supervisor itself | We write and test it |
| BusyBox's runit applets (`runsvdir`, `sv`; not enabled in the image) | Generic supervision with little code | No cgroups or heartbeat; the Oscairn-specific parts are still needed |
| systemd | Cgroup delegation, restart limits, watchdogs, journald, timesyncd and its clock file, all built | Buildroot's manual says it brings "a fairly big number of large dependencies" and recommends BusyBox init for "most embedded systems"; boot time for a pedal; the Oscairn-specific supervision (reopen the Performance, pass-through switching, the app rules) is still ours |

### Recommendation

BusyBox init stays as PID 1, and `oscairn-session` becomes a small C++ program, the supervisor,
started from `/etc/inittab` with `respawn`:

- it sets up the system once: the cgroup subtree with its cpusets (RFC 0025, above), interrupt
  affinity and priorities, idle states, `RLIMIT_MEMLOCK`, the time state, a hardware watchdog
  device where the board has one, kicked while the engine and shell are healthy;
- it starts the engine and the shell as siblings, and is the RFC 0027 watchdog for the engine
  (RFC 0027 step 2 builds the heartbeat; this RFC only places it) and the shell's restarter;
- it runs the few network services as children, with stderr captured and restarts bounded as
  RFC 0022 bounds apps;
- it captures every child's stderr into rotated logs (`Logs/` in the data folder, RFC 0022's
  size), replacing today's unrotated `/var/log/oscairn.log`;
- in the read-only system of RFC 0028, nothing it writes is on the system partition.

The display server (Xorg today, the Wayland shell later, RFC 0022) is the shell's to start, not
the supervisor's. systemd is reconsidered if the image comes to need several services that
expect it (a D-Bus-based Bluetooth stack and a network manager are the likely ones; RFC 0036
decides Bluetooth's).

## Optional hardware built per product

Products differ in what they have: Bluetooth, Wi-Fi, a touch screen, a display at all, cameras,
a battery, USB gadget mode. Code for hardware a product lacks wastes its storage and memory and
adds to what has to be kept secure. The owner's rule: a build for a device without a feature does
not contain its code, and apps and services that use it handle its absence gracefully.

### Where things stand

The image already works this way for two things: CMake options (`OSCAIRN_STANDALONE`,
`OSCAIRN_ENGINE`, `OSCAIRN_MODULAR`, `OSCAIRN_LV2`) and the Buildroot options that set them
(`BR2_PACKAGE_OSCAIRN_STANDALONE` brings JUCE and X11 only when chosen, `BR2_PACKAGE_OSCAIRN_LV2`
brings Lilv). Profiles describe hardware (`screens`, `network.wifi`, `power.battery`,
`midi.usbDevice`, RFC 0024). `session.hello` returns protocol capabilities, not hardware ones.
None of the subsystems below has Oscairn code yet, so the rule applies as each is built.

### The rule

- **Compiled out and left out.** Each optional subsystem is a CMake option in Oscairn
  (`OSCAIRN_BLUETOOTH`, `OSCAIRN_WIFI`, `OSCAIRN_TOUCH`, `OSCAIRN_DISPLAY`, `OSCAIRN_CAMERA`,
  `OSCAIRN_POWER`, `OSCAIRN_USB_GADGET`) and a Buildroot option in the `oscairn` package's
  `Config.in` that sets it and selects its libraries, kernel options and firmware. Off, the code
  is not built and nothing it needs is in the image.
- **Driven by the defconfig, kept consistent with the profile.** The product's defconfig chooses
  them; a build check compares the defconfig with the profile the image ships (RFC 0024) and fails
  when they disagree (a profile with `wifi: true` in an image without Wi-Fi, or the reverse).
- **Reported at run time.** The engine reports the hardware capabilities it was built with and
  found working in `session.hello` (`hardware`: `bluetooth`, `wifi`, `touch`, `display`, `camera`,
  `battery`, `usbGadget`), and a change (a USB Bluetooth adapter plugged in, on a build that has
  Bluetooth) as a notification. Apps and services check them and degrade gracefully: the feature
  is hidden or disabled with a reason ("This instrument has no Bluetooth"), never an error.
- **Declared by apps.** An app's manifest (RFC 0022) lists the hardware it uses as `required` or
  `optional`. One requiring what a product lacks is shown as unavailable on it, with the reason, as
  a package without the device's platform build is (RFC 0006); one using it optionally runs
  without it.
- **Saved content that names missing hardware** (a Performance routed to Bluetooth MIDI, a
  setting for a camera) opens with that part shown as unavailable and kept, as a missing plugin's
  stand-in keeps its place (RFC 0006), so the same Performance works again on a product that has
  it.
- **Tested.** A build with a feature off contains none of its code or libraries: CI builds the
  `oscairn` package with each option off, and checks the installed files and their dynamic
  dependencies (`readelf -d`) for the feature's libraries and the binaries for its symbols.

[RFC 0036](0036-bluetooth.md) applies this to Bluetooth (MIDI and audio).

## Build order and gates

Gates on a device are **unverified** until a board image exists; on QEMU only the build, boot and
behavior can be checked, not latency.

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Measuring | `rt-tests` in a test image variant; `audioUnderruns` in `session.status`; a script running `cyclictest` and a playing Performance under load and reporting both | QEMU: the script runs and reports (numbers not meaningful). Device: a baseline for the next steps, recorded with the image version |
| 2. `PREEMPT_RT` | The shared kernel fragment; the QEMU image built with it | QEMU: boots, the boot test and `compare-audio.py` pass. Device: `cyclictest` maximum on an audio core under load over 12 hours below a stated figure (proposed: 100 µs, to be set from step 1), lower than step 1's |
| 3. Audio cores | The cpuset partition, `nohz_full`, `irqaffinity`, the audio interrupt on an audio core; `realtimeCores` as a list | QEMU: the engine and workers are in the partition, nothing else is, interrupts are off it (read from `/proc`). Device: zero `audioUnderruns` over 12 hours at the profile's block size with a Performance at the admission limit and the load script on the other cores |
| 4. Frequency, idle, priorities | Governor and idle-state switching; the priority table applied | Device: the same run passes at 64 frames, 48 kHz; on a battery product the audio cores slow down within a minute of silence |
| 5. Supervisor | The C++ supervisor under BusyBox init; engine and shell as siblings; logs rotated; hardware watchdog | QEMU: kill the engine, the shell and the supervisor in turn: each comes back, the others keep running (sound goes on when the shell dies), logs hold each one's stderr; the boot test passes |
| 6. Time | The time state, sources, the saved floor, `system.time`; recordings, library, logs and TLS as the table says; the revision seed fixed | QEMU with no RTC device and no network: a recording is named by take number, no clock is shown, the library sorts newest first; then the time is sent by a paired client: the recording is renamed with its date, the clock appears; reboot: the clock is at least the saved time |
| 7. Security | `pkg-stats` in CI per defconfig; the vendored-library list; the policy written in `os/README.md`; the image signing keys | A known-vulnerable package version put in on purpose is reported; a security image is built, signed and installed through RFC 0028 |
| 8. Licenses | `legal-info` per release, the source archive published, the Licenses screen, the `oscairn` package's complete license list | The archive rebuilds the image from scratch to the same files (where the build is reproducible) or boots to the same version; the Licenses screen lists every package `legal-info` names |
| 9. SDK | `make sdk` per image on an x86-64 host; the template; the builder container | A sample plugin and a sample app built from the template on a fresh GitHub repository, with no Oscairn checkout, install and run on the QEMU image; the same plugin built with the previous image's SDK runs on the new image |
| 10. Optional hardware | The options as each subsystem lands; the profile check; `hardware` in `session.hello`; the leftover check | An image with Wi-Fi off has none of its libraries or firmware and the profile check passes; one with a mismatched profile fails the build; an app requiring Wi-Fi shows as unavailable with the reason |

Steps 2 to 4 need step 1's baseline and a board image. Step 5 comes before RFC 0022 step 2 needs
apps supervised on the device and before RFC 0027 step 2. Step 6 needs no hardware and can come
first. Step 8 comes before any image is distributed outside the project.

## Open questions

Real-time:

- The profile's audio cores as a list of core numbers, and the audio interrupt's name: RFC 0024's
  schema.
- Whether RT throttling stays on for isolated audio cores.
- The `cyclictest` and underrun thresholds per reliability level, from measurements.

Security and licenses:

- Who signs a third party's product's images (RFC 0028's question), and whether the project's
  image keys sign anything but the project's own images.
- Whether the AGPL's Installation Information requirement means every product must let its owner
  trust their own image key, and how that sits with refusing unsigned code by default: counsel,
  with RFC 0001's open question.
- When development moves to a Buildroot LTS series, and how long the next one is maintained.

SDK:

- Whether an x86-64 device image is planned, which would add `linux-x86-64` as an SDK target.
- Whether the builder container is the primary way to get the SDK, or the tarball.

Time (the owner's decision above):

- Whether to adopt the design.
- Whether a paired client may set the time without asking on the device.
- NTP or NTS, and whose servers.

Services:

- Which network services the image runs, and whether any of them pulls in D-Bus or systemd
  (RFC 0036 for Bluetooth).

## Ruled out

- **`isolcpus=` domain isolation:** irreversible at run time; cpuset partitions do the same and
  can change.
- **A vendor kernel without working `PREEMPT_RT` on an `instrument` product.**
- **Showing a guessed time as the time,** or dating files 1970 or 2000 because the clock started
  there.
- **Signatures, revocation or package validity that depend on the date** (RFC 0006 already).
- **Product images carrying code for hardware the product does not have.**
- **An error where a missing feature should be hidden or explained.**
- **The image's licenses handled only on request:** the source is published with each image.

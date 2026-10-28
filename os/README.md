# Oscairn OS images

Linux images that boot straight into Oscairn, built with
[Buildroot](https://buildroot.org) 2026.08.1 as an external tree (`os/buildroot`), so
Buildroot itself stays unmodified and easy to upgrade.

| Image | Defconfig | Status |
| --- | --- | --- |
| QEMU, generic ARM64 machine | `oscairn_qemu_aarch64_defconfig` | Builds and boots; Oscairn starts and plays |

QEMU has no model of the RK3588 or the KICK's i.MX8M Mini, so the QEMU image uses the
generic `virt` machine with a Cortex-A76 CPU (code is built for the RK3588's
Cortex-A76/A55). It tests the toolchain, packaging and startup, not board-specific parts
(display controller, audio codec, panel).

## What is in the image

- GCC 15, glibc, Linux 6.18 (LTS) with a low-latency (`PREEMPT`) kernel, BusyBox init, eudev.
- Oscairn: the core, the CLAP plugins in `/usr/lib/clap`, `oscairn-render`, and the
  desktop app as `/usr/bin/Oscairn`.
- The session (`board/common/session-juce-x11`): at boot, `/etc/init.d/S95oscairn` starts
  `/usr/libexec/oscairn-session`, which starts an X server and then Oscairn full screen,
  and restarts Oscairn if it exits. Its arguments are in `/etc/default/oscairn`
  (`--layout kick-screen --fullscreen --play`). Logs: `/var/log/oscairn.log` and
  `/var/log/oscairn-xorg.log`.
- Sound through ALSA. The QEMU image uses the kernel's dummy sound card, which plays
  silently.

## Changing the UI later

The JUCE app needs X11, and that is confined to two places:

- the `BR2_PACKAGE_OSCAIRN_STANDALONE` option (the JUCE app and its X11, ALSA and font
  dependencies), and
- the `board/common/session-juce-x11` overlay with `board/common/post-build.sh` (the
  session that starts X and the app).

A device UI built another way (for example Qt or Slint drawing directly to the display
through DRM/KMS) is a new package and a new session overlay; the core, plugins, kernel and
the rest of the image do not change. Once the UI is its own process talking to the core
through the control API, the core will run as a separate service that does not depend on
any UI toolkit.

## Building

Needs Docker. Buildroot runs in a Debian container (`os/docker`), and its work (Buildroot
itself, downloads, compiler cache and output) lives in the Docker volume `oscairn-os`,
because Buildroot needs a case-sensitive Linux file system.

```sh
os/build.sh                       # build oscairn_qemu_aarch64_defconfig
os/build.sh test                  # boot it in QEMU and check that Oscairn starts
```

A clean build took 12 minutes on a 16-core Mac (Docker with 8 GB of memory) with the
downloads and the compiler cache already in place; the first build, which also downloads
everything and fills the cache, takes longer (not timed). The image builds from
the working tree: committed files and new files git does not ignore.

`os/build.sh test` boots the image (4 cores, 2 GB, a 1024 x 600 display) and passes when
the session reports Oscairn running and two screenshots 3 seconds apart both show a
picture and differ, which means the screen is drawn and the sequencer is playing.
Results: `build-os/test` (console log and screenshots).

Other Buildroot targets go after the defconfig, for example
`os/build.sh oscairn_qemu_aarch64_defconfig oscairn-rebuild all`.

## Next

- A board image for an RK3588 development board (mainline kernel), with a real audio
  interface and a real-time kernel, and latency measurements.
- The KICK, once units exist: its kernel, display, audio codec, and a driver for the panel.
- Signed updates with A/B system images.

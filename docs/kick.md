# Running Oscairn on a KICK or a KICK emulator

Oscairn has not yet run on a KICK or on any i.MX8M Mini board. This page says what exists for
the KICK today, how to try it on an emulator, and what to expect, as of November 1, 2026.

## What exists

- **A device profile for the KICK** (`--profile kick`, [RFC 0024](rfcs/0024-device-profiles.md)):
  [hosts/standalone/profiles/kick.json](../hosts/standalone/profiles/kick.json). Its values are
  **estimates** (cores, memory, audio, controls, MIDI, network), not measured on a unit. It makes
  the app fill the display with no title or menu bar, show the KICK's screen layout
  (`kick-screen`, 1024 × 600), request 128-frame audio blocks, and offer the Library, Setlist
  and Groovebox apps.
- **Two layouts:** `kick-screen`, the screen alone as the device shows it, and `kick`, the whole
  front panel drawn on a computer screen (with the geometry of chillbeast's KickEM simulator
  board), for trying the KICK's flow on a desktop.
- **A Linux image** ([os/](../os/README.md)) built with Buildroot for QEMU's generic ARM64
  machine. It boots straight into Oscairn full screen under X11. It is **not** built for the
  KICK's board: no i.MX8M Mini kernel or device tree, no display, codec or panel drivers.

## What you see

The app starts at **Home** ([RFC 0030](rfcs/0030-system-shell-launcher-and-navigation.md)): a top
bar and the apps as tiles.

- **Performance:** plays and edits the open Performance. For now it opens the parts editor
  (parts, mixer, routing, controls); a live-controls view for playing comes later.
- **Library:** Performances and Sounds, by source and tags, with audition.
- **Setlist:** songs in order, for the stage.
- **Groovebox:** the step sequencer (two tracks, A/B patterns, per-step locks).

On the `kick-screen` layout (no buttons around the screen), each app's top bar has a Home icon.
On the `kick` layout, the panel's **MAIN** button goes Home, so the screen shows no Home icon;
**BACK** goes Home too for now (every app's top screen is one level down; going back within an
app is the app's own). The demo session plays the step
sequencer through the Basic Synth and the Delay.

## What does not work yet

- **The KICK's physical controls:** no driver reads its pads, encoders or buttons (its panel
  protocol is not implemented). On a device or an emulator, what works is what Linux sees: the
  touchscreen as a pointer, MIDI inputs ALSA lists (every one plays), and a mouse or keyboard. On a computer, the `kick` layout's
  drawn controls work with the mouse.
- **The KICK's audio codec, display and touch controller:** nothing is specific to them; the app
  uses what ALSA and X11 provide. It opens ALSA's default device; the profile asks for
  128-frame blocks, the device may choose another size (the log says which), and the profile's
  sample rate is not applied yet.
- **Live controls in the Performance app, the system shell's full top bar, quick settings and
  the running-apps switcher:** designed in RFC 0030, not built.
- **Measured CPU and memory budgets on the KICK:** the budget comes from the profile's estimates.

## Running it

### On your emulator's own Linux

If the emulator runs an ARM64 Linux with X11 and ALSA, build Oscairn there or cross-compile it,
then start the app with the KICK's profile:

```sh
cmake -S . -B build-app -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOSCAIRN_STANDALONE=ON
ninja -C build-app oscairn_standalone
build-app/hosts/standalone/oscairn_standalone_artefacts/RelWithDebInfo/Oscairn --profile kick
```

The app starts its audio engine (`oscairn-engine`, beside it) and keeps it running if the UI
exits; see [launching](../hosts/README.md#launching). It loads the demo plugins from the build
tree, or from `--plugins DIR`. A native Linux build of the desktop app is untested; the image's
cross-compiled build is the tested Linux path.

### With the Oscairn image

`os/build.sh` builds the QEMU image (needs Docker; see [os/](../os/README.md)). To use the
KICK's profile, set `OSCAIRN_ARGS="--profile kick"` in `/etc/default/oscairn` (in the image's
overlay, `os/buildroot/board/common/session-juce-x11/etc/default/oscairn`, before building).

Two things to change for an emulator of the KICK's hardware, neither tried yet:

- **The CPU:** the image is built for Cortex-A76/A55 (`BR2_cortex_a76_a55` in
  `os/buildroot/configs/oscairn_qemu_aarch64_defconfig`), which is ARMv8.2. The KICK's Cortex-A53
  is ARMv8.0, so those binaries may stop with an illegal instruction there. Set `BR2_cortex_a53=y`
  instead to build for it.
- **The kernel:** the image's kernel is configured for QEMU's `virt` machine. An emulator that
  models the i.MX8M Mini needs a kernel and device tree for that SoC.

## What to report

The session logs to `/var/log/oscairn.log` (the app and engine) and `/var/log/oscairn-xorg.log`.
Useful to know: whether the app reaches Home, the audio block size it reports, whether touch
reaches the app, whether the demo plays without dropouts, and the `budget.estimate` the
[control API](control-protocol.md) gives for the open Performance.

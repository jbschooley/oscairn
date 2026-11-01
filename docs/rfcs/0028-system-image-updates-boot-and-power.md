# RFC 0028: The system image: updates, boot, storage and power

Status: proposed, 2026-11-01. Required for every product Oscairn runs on as its own operating
system. Builds on the [Oscairn OS images](../../os/README.md) (Buildroot, an ARM64 QEMU image that
boots into Oscairn), device profiles ([RFC 0024](0024-device-profiles.md)) and reliability levels
([RFC 0027](0027-reliability-levels.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. A read-only system partition, with the user's data on its own
- [ ] 2. Signed A/B updates, falling back to the previous system if the new one does not start
- [ ] 3. Factory reset that keeps or clears the user's data, as asked
- [ ] 4. Boot time measured and reduced to each product's target
- [ ] 5. Safe power loss: no corrupted data, checked by cutting power in tests
- [ ] 6. Power management for battery products

## Problem

The OS image boots into Oscairn and restarts it if it exits, and its next steps name signed A/B
updates. A product needs more: it must survive an update that goes wrong, a power cut in the
middle of saving, years of use without its system drifting, and, for a pedal, starting in a few
seconds. None of that is decided.

## Outcome

Every product runs the same kind of image: a read-only system that updates as a whole, the user's
data apart, a boot measured against the product's target, and power loss that loses at most what
was being saved.

## Partitions

- **System, A and B:** read-only, one running and one for the next update. Nothing writes to the
  running one.
- **User data:** Performances, Sounds, libraries, samples, settings, paired clients and
  measurements (RFC 0025): the data folder Oscairn already uses (`OSCAIRN_DATA_HOME`), on its own
  partition with a journaling file system.
- **Factory:** the factory library (RFC 0021) and the factory settings, read-only, for reset and
  reinstalling.

## Updates

- An update is a whole system image, signed by its publisher, written to the partition not
  running, then booted once on trial. If Oscairn does not reach a healthy state (the engine
  plays, the UI or web UI answers) the boot loader goes back to the previous system on its own.
- Never during a performance: offered only while nothing plays, and never applied on its own.
- Plugins and content are not part of the system image: they are packages (RFC 0006), updated
  apart, so a system update does not change a sound.
- From a USB stick, from a paired computer (RFC 0026), or downloaded when online.

## Factory reset

Restores the system to the factory partition's settings and reinstalls the factory library.
It asks whether to keep the user's data (the default) or clear it; clearing it is a second
confirmation.

## Boot

Each profile states a boot target: seconds to sound for a pedal, longer for a workstation.
The path to first sound: kernel, the engine and its audio, the last Performance; the UI and the
rest load after. Plugins are prepared in the order the Performance needs them; worker processes
(RFC 0010) start in parallel. Measured on each product, on every release.

## Power loss

Saving is already atomic (written beside, then renamed into place); the data partition's
journal and syncing after each save make it survive a cut. Tests cut power during saves,
installs and updates, and check that what was saved before is intact and that nothing half
written is shown.

## Power management

For battery products (RFC 0024's `power`): plugins that sleep (CLAP sleep) and Performances at
rest let the cores slow down; the screen dims; a warning before the battery runs out leaves time
to save. Never at the cost of a missed audio deadline: the cores reserved for audio keep their
speed while anything sounds.

## Open questions

- The update package format and signing keys: who signs a third party's product's images.
- Whether the UI process moves off X11 to a direct display path for faster boot (the OS README
  already keeps that open).

## Ruled out

- **Updating files in place on a running system.**
- **Updates applied without the user's say**, or while playing.

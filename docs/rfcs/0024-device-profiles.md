# RFC 0024: Device profiles

Status: in progress, 2026-11-01: step 1 built, steps 2 and 3 started ([status](#step-1-status)).
Required for every product Oscairn runs on. Builds on the
capability profile catalog of [RFC 0006](0006-packages-and-portability.md#capability-profile-catalog),
hardware services ([RFC 0004](0004-hardware-services.md)) and the surfaces of
[RFC 0019](0019-adaptive-ui.md); feeds resource budgets ([RFC 0025](0025-resource-budgets.md)),
reliability levels ([RFC 0027](0027-reliability-levels.md)) and the system image
([RFC 0028](0028-system-image-updates-boot-and-power.md)).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. The profile schema, and profiles for the desktop, the QEMU image, the Behringer KICK and the Ignite ([status](#step-1-status))
- [ ] 2. The app reads its profile: surface, layouts, default mappings and app set follow from it
      (surface, layout and apps done; the control groups reach RFC 0011's physical controls,
      which resolve each group's defaults; the panel playing faders, rows and buttons remains)
- [ ] 3. The engine reads it: audio block size, budgets and reliability level follow from it
      (block size and the budget (RFC 0025) done; reliability waits for RFC 0027)
- [ ] 4. Packages and Performances checked against it (with RFC 0006 and RFC 0025)
- [ ] 5. A second product's profile (a desktop module or pedal) built from the same image

## Problem

Oscairn should run on a keyboard workstation, a tabletop module, a groovebox, a pedal, a rack
unit, a drum module, a DJ unit, a Eurorack module, a DAW plugin and a desktop app. Today the
desktop and the KICK are told apart by command-line flags and layout files: which layout, which
screen, which buffer size. Each further product would add flags, and the same facts (what
controls it has, how much CPU, whether it may ever drop audio) would be decided in several
places that can disagree.

## Outcome

A **device profile** describes one product, in one versioned manifest, and every part of
Oscairn that depends on the product reads it instead of being configured separately. RFC 0006's
catalog already holds profiles for portability (which builds and backends fit a device); this RFC
makes that catalog the one place, and says what a profile holds and who reads it.

## First products

The first profiles, and the products every part of this design is checked against:

- **The Behringer KICK**, the instrument Oscairn grew from; its panel is the `kick` layout.
- **The Ignite** (BE 0716-AAF), on the same board family and panel protocol; its panel, with its
  1024 x 600 touchscreen and endless encoders, is the `ignite` layout. It needs external MIDI,
  CV and clock (RFC 0012).
- **The desktop app** on macOS and Linux, and the **QEMU image** (`oscairn_qemu_aarch64_defconfig`).
- **A MODX M-style keyboard** (`modx-m`, estimated), the test case for control groups with
  hardware rows and fader modes.

Their profiles' values (cores, memory, audio interface, controls) are filled from the hardware
and measured on real units, not assumed; the layouts already describe their panels.

## What a profile holds

An illustrative profile (the values are an example, not a real product's):

```json
{
  "format": "oscairn.profile",
  "version": 2,
  "id": "org.example.keys61",
  "name": "Example 61-key instrument",
  "platform": { "os": "linux", "cpu": "arm64", "runtime": "oscairn-buildroot-2026", "board": "rk3588" },
  "compute": { "cores": 8, "realtimeCores": 2, "memory": 8192, "storage": 65536 },
  "audio": { "inputs": 2, "outputs": 4, "rates": [48000], "block": 64, "headphones": true },
  "controls": [
    { "kind": "keys", "count": 61, "role": "notes", "sensing": "channel aftertouch" },
    { "kind": "pads", "count": 16, "role": "notes", "sensing": "poly pressure" },
    { "kind": "knobs", "count": 6, "role": "screen" },
    { "kind": "encoders", "count": 4, "role": "assign", "rows": [
      { "name": "Assign 1", "bank": "assign" }, { "name": "Assign 2", "bank": "assign" },
      { "name": "Tone", "bank": "filter" }, { "name": "EG", "bank": "ampEG" } ] },
    { "kind": "faders", "count": 8, "role": "assign", "modes": ["part", "element", "audio"] },
    { "kind": "knobs", "count": 1, "role": "fixed", "functions": ["volume"] },
    { "kind": "pedal", "count": 2, "role": "assign", "paging": "button" }
  ],
  "screens": [ { "width": 1024, "height": 600, "touch": true, "surface": "panel" } ],
  "midi": { "din": 1, "usbDevice": true, "usbHost": true },
  "cv": { "inputs": 0, "outputs": 0 },
  "network": { "ethernet": false, "wifi": true },
  "power": { "battery": false },
  "reliability": "instrument",
  "apps": ["oscairn.library", "oscairn.setlist", "oscairn.groovebox"]
}
```

- **`platform`:** what builds it needs (RFC 0006 selects packages by it).
- **`compute`:** cores (and how many are reserved for audio), memory and storage: the budget's
  ceiling (RFC 0025). The memory classes proposed there would add the operating system's share
  and a floor for Performances ([RFC 0025](0025-resource-budgets.md#admissions-arithmetic)).
- **`audio`:** its interface, the sample rates and the block size it runs at, headphones (a cue
  bus).
- **`controls`:** every physical control, in groups (below). Default control mappings and the
  panel layout come from these (RFC 0011's physical controls).
- **`screens`:** each screen's size, touch and surface (RFC 0019: desktop, panel, compact, or
  none for a headless unit).
- **`midi`, `cv`, `network`:** what it can connect to; CV outputs mean the CV service of RFC 0004.
- **`power`:** battery or mains, for power management (RFC 0028).
- **`reliability`:** its level (RFC 0027): `desktop`, `instrument` or `pass-through`.
- **`apps`:** the apps installed with it (RFC 0022); the user can add others.

### Control groups

A group is controls of one kind that do one job: `kind` (keys, pads, knobs, encoders, faders,
buttons, wheels, jog, touchstrip, pedal, expression, footswitches), `count`, what they can sense
(`sensing`, free text), and `role`:

| Role | The controls | Group fields |
| --- | --- | --- |
| `notes` | Play notes: keys and pads, and only they | |
| `screen` | Play what the screen shows: the Performance app's knob row, an editor page's top parameters | |
| `assign` | Play banks ([RFC 0011](0011-performance-ui-and-editors.md#physical-controls)) | Knobs: `rows` (each `{name, bank}`, its row button picking it, `bank` one of `assign`, `filter`, `ampEG`, `eq`, `levels`, `effects`) or `paging` (`button` or `screen`), not both. Faders: `modes`, from `part`, `element` and `audio` |
| `fixed` | A dedicated function each | `functions`, one name per control, when known |

Banks are as wide as the group playing them. For example, the KICK's panel has four encoders,
P1 to P4, that play the row selected on screen (`assign`, `paging: screen`); VOLUME, MASTER and
REC GAIN knobs and the large TEMPO wheel (`fixed`); 16 pads (`notes`); 31 buttons and a touch
strip. A MODX M has six display knobs (`screen`), four control knobs whose panel buttons pick
their row (Assign 1, Assign 2, Tone, EG, EQ, Effects: names to be confirmed on a unit), eight
faders in Part, Element and Audio modes, a Super Knob, and Scene and Part buttons.

A profile describes the product, not one unit: the unit reports what it actually has (RFC 0006
separates the catalog's profile, the device's reported inventory and the user's settings), and
where they differ the unit's report wins and the difference is shown.

## Who reads it

| Reader | What follows from the profile |
| --- | --- |
| The image (RFC 0028) | Which session starts, which services run, the boot path, power management |
| The engine | Audio block size, cores for audio, the reliability level's watchdog and bypass, the budget's ceiling |
| The UI (RFC 0019) | The surface and its layouts; whether there is a UI at all, or only the web UI (RFC 0026) |
| Control mappings (RFC 0011) | The default layer for its knobs, faders, pads and pedals |
| Packages (RFC 0006) | Which builds and backends fit; refusing a package with no build for it |
| Admission (RFC 0025) | Whether a Performance fits before it loads |
| Item types (RFC 0023) | Which types it shows by default (a pedal shows impulse responses, not Performances' keyboard zones) |

On the desktop, the profile is the computer's, generated at start: its cores, memory, audio
device and screen, `desktop` reliability, no physical controls unless a controller is connected
(and then the controller's own profile adds them).

## Controllers

A connected controller (a keyboard, a pad controller, a DJ controller) has a profile of its own
with `controls` and `midi` only, matched by its USB identity. Its controls join the device's for
mappings while it is connected. This replaces per-controller special cases.

## Step 1 status

Built 2026-11-01 (`hosts/standalone/Profile.h`, [schema](../profile.schema.json)). Profiles are
strict JSON, as themes are: unknown fields, wrong types and impossible values (more realtime
cores than cores, a block under 16 frames, an unknown surface or reliability) refuse them. Built
in: `qemu-aarch64` (its values are the QEMU test machine's), `kick` and `ignite` (best guesses,
every guessed section listed in `estimated`: the board is taken as the i.MX8M Mini named in the
OS notes, the controls are counted from their panel layouts, the rest is assumed until a unit is
measured). Profile version 2 (2026-11-01) describes controls in groups with roles, replacing
version 1's flat counts: the KICK's are corrected (four assignable encoders paged on screen,
three fixed knobs, the tempo wheel, pads, buttons and a touch strip, where version 1 had eight
encoders), the Ignite's eight sound encoders, 16 step encoders, DATA, ROW and BAR are fixed
functions from its layout, and `modx-m` (all estimated, with an 800 x 480 screen layout of its
own) is added. `desktop-profile` checks them and the groups' refusals. The desktop app makes `desktop` from the computer: its cores, memory and main display,
desktop reliability, every app.

The app takes `--profile NAME|FILE` (the image's `/etc/default/oscairn` passes `qemu-aarch64`,
replacing `--layout kick-screen --fullscreen`, now gone). A panel or compact surface fills the
screen with no title or menu bar; the screen's layout is shown unless `--layout` says otherwise;
the profile's `apps` are the Apps menu; its `audio.block` is the block the audio device is asked
for, passed to the managed engine as `--buffer-frames` (16 to 8192; given on the command line it
wins). `desktop-profile` checks the built-in profiles, the desktop's, nine refusals and the block
option; launching the app with `--profile kick` was checked by its snapshot (the KICK's screen at
1024 x 600). The QEMU image, rebuilt with `--profile qemu-aarch64`, boots and passes its boot
test (`os/build.sh test`: Oscairn running, the screen drawn and changing).

## Open questions

- Whether profiles ship in the image only, or also update from RFC 0006's catalog snapshots.
- How a profile declares controls with displays of their own (encoders with rings, pads with
  colors, motorized faders) and what drives them.
- How fine the compute description must be for admission to be trusted (RFC 0025 measures; the
  profile may only seed it).

## Ruled out

- **Product checks in code** (`if KICK ...`): every difference is in the profile.
- **One profile per unit in the catalog:** units report their own differences locally.

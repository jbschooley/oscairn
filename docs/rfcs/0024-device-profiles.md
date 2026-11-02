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
- [ ] 6. External MIDI controllers: [controller profiles](#controllers) matched by port name,
      their controls joining the device's, each group's [default target](#default-targets)
      (parsing, defaults, resolution, encodings and pickup built; the SMK37 Elite's numbers
      read with `oscairn-midi-monitor`, all but its buttons; Mackie Control ports read and fed
      back, and knob values sent back where a profile says so ([status](#mackie-control));
      choosing between profiles that match one port not built)

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
  "apps": ["oscairn.performance", "oscairn.library", "oscairn.setlist", "oscairn.groovebox"]
}
```

- **`platform`:** what builds it needs (RFC 0006 selects packages by it).
- **`compute`:** cores (and how many are reserved for audio), memory and storage: the budget's
  ceiling (RFC 0025). The memory classes there (RFC 0025 step 6, built 2026-11-01) add two
  optional fields, in MiB like `memory`:
  - `osMemory`: what the operating system and everything beside Oscairn need at their peak
    (kernel, init, drivers, the display server). Measured on the device later; the larger of the
    two counts.
  - `performanceMemory`: the floor for Performances, committed memory that running apps can never
    take. A launch that would leave less is refused
    ([RFC 0025](0025-resource-budgets.md#admissions-arithmetic)).

  Both or neither. Without them the budget stays 80% of `memory`, as built. With them, a profile
  is refused when `osMemory` + `performanceMemory`, plus the always-available apps' declared
  reservations, exceed `memory`. The margin per reliability level is not a profile field yet; its
  values are open, and the app keeps 5% of `memory` at every level meanwhile. Until apps declare
  reservations in manifests, the app knows its own apps' (RFC 0025's illustrative figures:
  Performance 150, Library 120, Setlist 60, Groovebox 200 MiB; not measured). None of the
  built-in profiles sets them yet.
- **`audio`:** its interface, the sample rates and the block size it runs at, headphones (a cue
  bus).
- **`controls`:** every physical control, in groups (below). Default control mappings and the
  panel layout come from these (RFC 0011's physical controls).
- **`screens`:** each screen's size, touch and surface (RFC 0019: desktop, panel, compact, or
  none for a headless unit).
- **`midi`, `cv`, `network`:** what it can connect to; CV outputs mean the CV service of RFC 0004.
- **`power`:** battery or mains, for power management (RFC 0028).
- **`reliability`:** its level (RFC 0027): `desktop`, `instrument` or `pass-through`.
- **`apps`:** the apps installed with it (RFC 0022), by app ID; the user can add others. The
  built-in ones are `oscairn.performance`, `oscairn.library`, `oscairn.setlist` and
  `oscairn.groovebox`; Home offers the profile's, so a device that plays no Performances (a
  recorder, a DJ deck) can leave `oscairn.performance` out.
- **`alwaysAvailable`** (optional, built with RFC 0025 step 6): the app IDs whose memory is
  reserved whether they run or not, so they always open; each must be in `apps`. It is the
  default of a setting the musician can change. Absent: `oscairn.performance` and
  `oscairn.library`, where `apps` lists them.

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

Every group can say what it does by default (below), so a stage keyboard needs no programming.

#### Default targets

| Kind | `role: assign` plays, when nothing is assigned | Field |
| --- | --- | --- |
| knobs, encoders | the banks (rows or pages, as above) | `target`: `banks` (the default) |
| faders | part levels, parts then buses; their `modes` add element and audio levels | `target`: `partLevels` (the default) |
| buttons | part mutes, or scenes (button N selects scene N) | `target`: `partMutes` (the default) or `scenes` |
| wheels, touchstrip, pedal, expression, footswitches, jog | a standard controller each, sent to every part, so each part's plugin does its own thing with it (the mod wheel's vibrato) | `functions`, one per control: `pitchBend`, `modulation` (CC 1), `breath` (CC 2), `foot` (CC 4), `expression` (CC 11), `sustain` (CC 64), `portamento` (CC 65), `sostenuto` (CC 66), `soft` (CC 67) |

`target` is only on assign groups and only with a value its kind allows. `functions` on an
assign group names standard controllers only; a `fixed` group keeps free names (`volume`,
`tempo`). A wheel that is `assign` can be reassigned, by the Performance or by the Sound
([RFC 0011](0011-performance-ui-and-editors.md#physical-controls)); a `fixed` one cannot. A
wheel group's pitch-bend wheel and mod wheel are best two groups of one (they speak different
messages, below), pitch bend first, so the mod wheel is `wheel:2` on every keyboard.

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

An external controller (a USB MIDI keyboard, a pad controller, a DJ controller) has a profile in
the same format, version 2, with three things a device profile does not have:

- **`midi.ports`:** the controller's MIDI input ports, each `{name, index, protocol}`. A port
  matches an input whose name contains `name`, ignoring case (`"SMK-37 Elite -Master"` matches
  that input as macOS names it); `index` (from 1) picks the Nth matching input, in
  the order the system lists them, for a device with several ports under one name (the MODX M's
  port 1 and port 2). `protocol` is `midi` (the default), or `mackie` for a Mackie Control port,
  read by a parser of its own ([below](#mackie-control)); `display: true` says a Mackie
  surface has scribble strips. `ignore: true` names a port that
  carries copies of another's messages (the SMK37 Elite's `-Private` repeats `-Master`): the
  app sends the matching inputs' names to the engine (the control API's `midi.ignore`, sent
  again when the engine restarts), which leaves out every message from them, so no key or pad
  plays twice; an ignored port has no `protocol` and no group's `midi` names it. Matching by name works on every OS
  and through every MIDI driver; USB vendor and product IDs are not visible to JUCE's MIDI
  inputs. A profile with `midi.ports` is a controller's: it holds `format`, `version`, `id`,
  `name`, `controls`, `midi` (only `ports`) and `estimated`, and nothing about a platform,
  screens or reliability.
- **`midi` on each group:** how its controls talk MIDI: `port` (which of `midi.ports`, from 1;
  the first by default, and a `midi` port), `channel` (1 to 16, for the group or as a list, one
  per control, for a group whose controls send on different channels: the SMK37 Elite's knobs 1
  to 6 on channel 6 and 7 and 8 on channel 1) and one of `cc` (a controller
  number per control, 0 to 127), `notes` (a note number per control, for pads and note buttons)
  or `pitchBend: true` (a group of one wheel). With `cc`, `encoding` says how the value is sent,
  for the whole group or as a list, one per control:

  | Encoding | Values | Step |
  | --- | --- | --- |
  | `absolute` (the default) | 0 to 127, the control's position | |
  | `twosComplement` | 1 to 63 up, 127 down to 65 for −1 to −63 | 1 up, 127 down |
  | `binaryOffset` | 64 plus the step: 65 up, 63 down | 65 up, 63 down |
  | `signMagnitude` | bit 6 the sign: 1 to 63 up, 65 to 127 for −1 to −63 | 1 up, 65 down |
  | `{"increment": U, "decrement": D}` | U for one step up, D (another value) for one step down; any other value is no step | U up, D down |

  A step larger than one is the controller's acceleration and moves the value as far. Buttons
  say how they send with `button`: `momentary` (the default: a value of 64 or more, or a note
  on, is a press, and each press toggles a mute or selects a scene) or `latch` (the controller
  keeps the state: 64 or more is on, less is off). An assign group of knobs with hardware `rows`
  whose row switch changes the numbers it sends (the MODX M's [ASSIGN] button: knobs 1 to 4, or
  5 to 8) lists a number per control per row, row after row; a message then also says which row
  the group is on. Two controls of one controller never share a message; where the hardware
  sends one number from two controls (the MODX M's FC2 and Super Knob), the profile gives it to
  one and says so in the other's `sensing`. A keys group needs no `midi` (its notes go to the
  parts, as every input's do, RFC 0012); a controller's group without `midi` is described but
  does nothing yet.
- **Device profiles refuse both:** a device's panel speaks its own protocol, not MIDI.

While a matching port is connected, the controller's groups **join the device's**, after them,
and are numbered on from them by identity: a KICK with an eight-knob controller has `knob:1`
to `knob:4` on its panel and `knob:5` to `knob:12` on the controller. On the desktop, which has
no controls, the controller's are the only ones, so its knobs are `knob:1` to `knob:8`: joining
replaces nothing, and an empty panel needs no special case. Their defaults come from their
groups like any device's: the knobs play the banks, the faders part levels, the wheels pass
pitch bend and modulation to every part. Several controllers join in the order of their
profiles' ids. RFC 0011's controller-mapping service decodes the encodings and does pickup.

**A profile describes one firmware.** What a controller sends is its firmware's choice, so a
unit running custom firmware gets a profile of its own (its own id), beside the stock one.
Today, when two profiles match one input, the first by id takes it (a `--controller FILE`
replaces the built-in of its own id): deterministic, but silent. The intended behavior is that
the musician chooses, in a setting that defaults to the stock firmware's profile; that setting
is not built yet. Matching by the device's own identity (the firmware version in its reply to a
MIDI Identity Request) is a possible later step, where the firmware reports it.

Built in: `smk37-elite`, the owner's Arturia-style SMK37 Elite with its stock firmware (37 keys,
16 pads as eight by two, pitch and mod wheels, eight endless knobs, four faders), read with
`oscairn-midi-monitor` on 2026-11-01 by moving every control. On macOS it shows three inputs,
`SMK-37 Elite -Private`, `-Master` and `-MCP`: `-Private` and `-Master` carry identical copies
of every message, so the profile takes `-Master` for its controls and ignores `-Private`;
`-MCP` sent nothing and is declared Mackie Control from its name, unconfirmed. The keys, pitch
wheel (the full range), mod wheel (CC 1) and faders (CC 68 to 71, absolute) are on channel 1;
knobs 1 to 6 send CC 48 to 53, absolute, on channel 6; knobs 7 and 8 send CC 38 and 37 on
channel 1, a step a message: 69 clockwise and 1 counterclockwise (knob 7 checked turning
clockwise only). The pads are on channel 2, notes 36, 37, 38, 40, 42, 43, 44, 45, 46, 47, 48,
49, 51, 53, 54 and 55, with channel pressure as their aftertouch; the format has no field for a
group's aftertouch, so the pads' `sensing` says so, and the pressure reaches the parts as every
input's does (RFC 0012). No button was pressed, so the profile describes no buttons yet.
`oscairn-midi-monitor` (in `tools/`) prints every message from every input and a table per
control; `--seconds N` listens for that long instead of waiting for Enter.

The second built-in controller is `modx-m-usb`, a MODX M played over USB in DAW Remote's Plugin
mode (the DAW type Cubase, Logic or Live), apart from `modx-m`, the profile for running on that
hardware. From Yamaha's MODX M Data List and Supplementary Manual v3.00: port 1 carries the keys,
pitch bend, the mod wheel (CC 1), aftertouch and every control as a 7-bit absolute CC on the
instrument's MIDI channel (each number settable from 1 to 95): knobs 1 to 8 on CC 17 to 24, four
knobs whose [ASSIGN] button picks 1 to 4 or 5 to 8 (`rows`, a number per control per row); sliders
on CC 70 to 77; display knobs on CC 78 to 83; the Super Knob on CC 95; scene buttons on CC 25 to 31
and 85; Assign 1, Assign 2 and MSEQ Trigger on CC 86, 87 and 89; portamento switch and time on CC 65
and 5; FC1 on CC 11, FC2 on CC 95 (the Super Knob's), the footswitch on CC 88, sustain on CC 64 and
the ribbon on CC 16. Port 2 is DAW Remote as Mackie Control (part select, mute and solo, transport,
the jog dial and the cursors), declared `mackie`; ports 3 (a thru to the 5-pin jacks) and 4 (ESP)
are not used. Knobs 1 to 8 are endless encoders (confirmed by the owner) that still send absolute
7-bit values in Plugin mode, since the MODX keeps a value per knob and sends that: the profile's
group is `encoders` with the `absolute` encoding, and they pick up like pots. Oscairn could send
each knob's value back on its CC, so the MODX's value matches the target's and no pickup is
needed; whether Plugin mode takes CC 17 to 24 as input is unconfirmed. Estimated until a unit
confirms them: the port names and order, the channel, whether the scene buttons latch, and
whether the slider mode buttons change what Plugin mode sends.

### Mackie Control

Built 2026-11-01 (`oscairn/control_surface.h`, used by the app's `Controllers`). A `mackie`
port is read by a parser of its own, with no `midi` per group, since Mackie Control fixes the
messages: transport buttons, the cursors, each strip's select, mute, solo and record arm (notes),
fader touch (notes 104 to 112), the faders (14-bit pitch bend, a channel per strip, the ninth the
master), the V-pots (CC 16 to 23) and the jog wheel (CC 60), both relative with bit 6 the sign.
Oscairn maps them: Play and Stop to the transport; the first eight parts, in order, to the strips,
Select selecting a part for the surface (its own selection, apart from the screen's), Mute and
Solo toggling the part's, the fader its level; the V-pots play the selected part's knob banks a
step a detent (the Performance's controls with none selected); up, down and the jog wheel select
the part before or after, left and right page the V-pots' banks. The engine leaves a Mackie
port's input out, as it does an ignored port's, since its buttons are notes. Back, on the output
matching the port's name and index: the mute, solo, select, Play and Stop LEDs, the faders'
positions (not while a fader is touched; the level goes back on release) and, with `display`, the
parts' names on the scribble strips' upper row (SysEx `F0 00 00 66 14 12`, six characters a
strip); only what changed is sent.

**Values back to knobs.** A group's `midi` may say `feedback: true` (absolute CCs only): the
app then sends each control's target value on its CC and channel (the row's number, for rows),
whenever it changes, to the output matching the port, so an endless knob that keeps a value of
its own, as the MODX M's do, matches its target and needs no pickup. Off unless a profile says
so; no built-in profile does, since whether the MODX M's Plugin mode takes CC 17 to 24 as input
is unconfirmed.

`control-surface` checks every message kind parsed, the mapping to the transport and parts, the
feedback (everything at first, then only changes, a touched fader left alone, names only with a
display) and knob values back (the row's numbers, nothing for a blank knob, a group without
`feedback` or a relative one); `desktop-profile` checks the Mackie ports matched and the new
fields and their refusals. No surface was connected: the SMK37's `-MCP` and the MODX M's port 2
are unconfirmed as Mackie Control, and neither the parsing against real devices, the LEDs, the
faders' feedback nor the knob values back has been seen on hardware. Not yet: banks of eight
beyond the first eight parts, Record, Rewind and Fast Forward (the control API has no seek or
record), the V-pots' LED rings, the lower scribble row (the V-pots' labels), the timecode
display.

## Step 1 status## Step 1 status

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

## Step 6 status

Built 2026-11-01 (`hosts/standalone/Profile.h`, `Controllers.h`; RFC 0011's
`ControllerMapping` in `physical_controls.h`). Profiles take the fields above additively, still
version 2: `target` and `functions` on assign groups, each controller group's `midi`, and
`midi.ports`; the MODX M's wheels are now two assign groups, pitch bend and modulation. A
controller's profile is refused as `--profile`; the app opens every MIDI input matching a
built-in controller or a `--controller FILE`, joins its groups to the device's, and plays its
messages through the mapping service, following the part Play's knobs follow (and Play's page
when the controller's knobs are Play's knob row, as on the desktop); values are set
through the control API like the screen's knobs. `desktop-profile` checks both built-in
controllers, port matching by name and index (Mackie ports left out), and twelve refusals (a
controller's reliability, an unknown protocol, port, encoding or channel, a CC missing or shared,
a CC with a note, latching faders, MIDI on a device's panel, knobs playing scenes, an assign
group's function that is not a standard controller); `control-physical-controls` checks the
resolution and the mapping service (RFC 0011's step 6 status); `midi-monitor` checks the
monitor's lines and table. No controller was connected for any of this: the matching and
playing were checked with names and messages given in tests, not with hardware.

Then (2026-11-01) the SMK37 Elite's real numbers from the monitor, and the format gaps they
showed: a `channel` per control, the `{increment, decrement}` encoding, and ignored duplicate
ports, left out by the engine (`midi.ignore`, `Session` dropping the inputs' messages before
the graph). `desktop-profile` checks the new fields and their refusals (a channel list of the
wrong length, an increment equal to its decrement, a control on an ignored port);
`control-physical-controls` decodes 69 and 1 to +1 and −1 and matches by each control's
channel; `control-parts` checks an ignored input's notes are not delivered; `desktop-shell`
resolves the SMK37's CC 48 on channel 6, CC 38 on channel 1 and CC 70 to knob 1, knob 7 and
fader 3. Still checked without hardware: the profile's numbers are the owner's readings.

Not yet: the SMK37 Elite's buttons and the MODX M's real numbers (the monitor reads them); choosing between profiles that match one port; the mod
wheel still reaching a part as CC 1 when a Sound or the Performance assigns it elsewhere (the
engine passes CC 1 to every part; filtering it per part comes with the engine's input changes);
a controller's mod wheel or pedal on a nonstandard CC passed on as its function; paging a
controller's group that Play does not show, and Play following a row a controller switched;
Mackie Control's rings and banks beyond eight parts ([its status](#mackie-control)).

## Open questions

- Whether profiles ship in the image only, or also update from RFC 0006's catalog snapshots.
- How a profile declares controls with displays of their own (encoders with rings, pads with
  colors, motorized faders) and what drives them.
- How fine the compute description must be for admission to be trusted (RFC 0025 measures; the
  profile may only seed it).

## Ruled out

- **Product checks in code** (`if KICK ...`): every difference is in the profile.
- **One profile per unit in the catalog:** units report their own differences locally.

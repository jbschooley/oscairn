# RFC 0036: Bluetooth MIDI and audio

Status: proposed, 2026-11-07. Builds on hardware services
([RFC 0004](0004-hardware-services.md)), MIDI inputs, outputs and their selectors
([RFC 0012](0012-sync-external-midi-and-multi-device.md#choosing-devices-selectors)), recording
inputs ([RFC 0020](0020-recording.md#monitoring-and-latency-compensation)), apps and the engine's
ownership of devices ([RFC 0022](0022-apps-and-requests.md#the-model)), device profiles
([RFC 0024](0024-device-profiles.md)), network pairing ([RFC 0026](0026-network-control-and-web-ui.md)),
reliability levels ([RFC 0027](0027-reliability-levels.md)), internationalization
([RFC 0029](0029-internationalization.md)) and the shell ([RFC 0030](0030-system-shell-launcher-and-navigation.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Bluetooth as an optional part of the build and the image, reported as capabilities, absent where it is not built or has no radio
- [ ] 2. Bluetooth MIDI on the desktop: Bluetooth endpoints, selectors and profiles, reconnection
- [ ] 3. Bluetooth MIDI on the device: BlueZ, pairing through the engine, the Settings page
- [ ] 4. Bluetooth audio out, as a mirror of Main, with drift correction and the latency shown
- [ ] 5. Bluetooth audio in from a phone, as an input like any other
- [ ] 6. Pairing on products with a small screen or none
- [ ] 7. LE Audio (LC3), where the radio and the stack support it

## Problem

The owner, 2026-11-07:

> Bluetooth "is important for both MIDI and audio. People might want to jam through AirPods even
> with latency, and people might also want to connect their phone as an audio source to play
> along to."

And on products without it:

> "for devices without bluetooth, the build for those devices shouldn't contain the bluetooth
> code because it would be a waste of space there. apps and services that use bluetooth midi or
> audio should gracefully handle the lack of it on instruments that dont have it."

Oscairn has no Bluetooth code, and nothing in the design says how Bluetooth devices fit the
model of inputs and outputs the engine owns. Three uses need it:

- **Bluetooth MIDI:** a wireless keyboard, a pedalboard or an iPad app plays the instrument, or
  the instrument plays them.
- **Headphones and speakers:** practicing or jamming through AirPods, accepting that the sound
  arrives late.
- **A phone as a source:** a backing track or a song from the phone, mixed in to play along to,
  and recordable.

Bluetooth is also a radio stack with daemons, firmware and its own clocks, the kind of thing that
fails in ways a stage instrument must not notice. And most products may not have a radio at all.

## Current state

Read in the code on 2026-11-07:

- **Audio:** the engine (`hosts/audio/AudioEngine.cpp`, shared by the desktop app and the engine
  helper) opens **one** device through JUCE's `AudioDeviceManager` and maps logical channels to
  its physical ones (`ChannelMap`). There is no second output device, no resampling between
  devices, and no input from anything but that device. `outputLatencyMs_` (the device's reported
  output latency plus one buffer) delays MIDI out so it sounds with its block.
- **MIDI:** every input and output JUCE lists is opened (`openMidiInputs`, `openMidiOutputs`) and
  numbered 1 to 63, keeping its number while the app runs, keyed by JUCE's identifier. Every one
  is reported as USB (`MidiEndpoint::Kind::Usb`, marked `shortcut:` in the code); the kinds are
  `BuiltIn`, `Port`, `Usb` and `Network` (`core/control/include/oscairn/parts.h`). An input's
  message is stamped with the host clock on arrival, and notes enter at the start of the next
  block; clock messages keep their offset within the block. A list change (hotplug) reopens all.
- **On macOS** a Bluetooth MIDI device connected in Audio MIDI Setup becomes a CoreMIDI device,
  so JUCE would list it and the engine would open it, as "USB". Not tried with a device.
- **On Linux** JUCE's MIDI is the ALSA sequencer (`juce_Midi_linux.cpp`), so a port BlueZ creates
  there (below) would be listed the same way. The image has no BlueZ: it builds ALSA, X11 and
  Oscairn only (`oscairn_qemu_aarch64_defconfig`).
- **Profiles** (`docs/profile.schema.json`) have `network: { ethernet, wifi }` and nothing for
  Bluetooth. The KICK and Ignite profiles say `wifi: true`, marked estimated.
- **The control API** (`session.hello`) already returns a list of capabilities clients must check
  before using optional features ([control protocol](../control-protocol.md)).

## Outcome

- A Bluetooth MIDI device is a MIDI input and output like any other, of kind Bluetooth, chosen
  by selectors and matched by controller profiles; its timing is as good as its timestamps allow,
  and it comes back by itself when it reconnects.
- Bluetooth headphones and speakers play Main as a **mirror** of the main output, with the delay
  shown and a plain warning that it is for jamming, not tight playing.
- A phone paired as a source is an **audio input** of the engine: a part can play it, a take can
  record it.
- The engine owns all of it, as it owns every device: apps reach it through the control API.
- Bluetooth failing never touches the sound on the main output.
- A product without Bluetooth has none of it in its image, and everything that could use
  Bluetooth says plainly that it is not there, without errors.

## Bluetooth MIDI

### The standard

MIDI over Bluetooth Low Energy (BLE-MIDI) is a GATT service with one characteristic. MIDI
messages travel in notifications and writes, several per packet, each with a 13-bit timestamp in
milliseconds (the top 6 bits in the packet's header, the bottom 7 before each message), wrapping
every 8192 ms. The links below are vendors' guides; the MIDI Association's specification itself
was not read for this RFC, and must be before implementing.

- **Connection interval:** a BLE link exchanges data at connection events, at least 7.5 ms apart.
  Guides ask accessories to request 15 ms or less, Apple suggesting about 11.25 ms, and say the
  central (the computer or instrument) decides
  ([Silicon Labs](https://docs.silabs.com/bluetooth/2.13/bluetooth-code-examples-applications/midi-over-ble),
  [Nordic](https://devzone.nordicsemi.com/nordic/nordic-blog/b/blog/posts/optimizing-ble-midi-with-regards-to-timing-1293631358),
  [Espressif](https://docs.espressif.com/projects/esp-iot-solution/en/latest/bluetooth/ble_midi.html)).
- **Jitter:** messages played between connection events wait for the next one, so arrival times
  jitter by up to a connection interval. The timestamps exist to take that out: a receiver that
  uses them keeps the played intervals at the cost of a constant added delay. PipeWire's option
  for this says exactly that: "Larger values add a constant latency, but reduces timing jitter
  caused by Bluetooth transport" ([WirePlumber](https://pipewire.pages.freedesktop.org/wireplumber/daemon/configuration/bluetooth.html)).
- **Latency:** at least one connection interval on top of the sender's own scan and the
  receiver's block. No figure here is measured; step 2's gate measures one against USB.

### The platforms

| | Central (connects to a keyboard) | Peripheral (a phone or iPad connects to it) | Timestamps on input |
| --- | --- | --- | --- |
| **macOS, CoreMIDI** | Audio MIDI Setup's MIDI Studio, Configure Bluetooth ([Apple](https://support.apple.com/guide/audio-midi-setup/ams33f013765/mac)) | the same window advertises the Mac; Apple's Q&A says the Mac can play either role ([QA1831](https://developer.apple.com/library/ios/qa/qa1831/_index.html)) | handled inside CoreMIDI (not verified how) |
| **Linux, BlueZ's MIDI plugin** | yes: a GATT client that connects to bonded devices by itself (`auto_connect = true`) and makes an ALSA sequencer port named after the device with " Bluetooth" appended (`profiles/midi/midi.c`) | no | **discarded:** parsed, then `/* TODO: update event timestamp here! */`, and events sent "direct" (`profiles/midi/libmidi.c`); outgoing packets are stamped with the time of writing |
| **Linux, BlueALSA** | not documented | `--midi-advertise`, an ALSA sequencer port ([bluealsad(8)](https://github.com/arkq/bluez-alsa/blob/master/doc/bluealsad.8.rst)) | not documented |
| **Linux, PipeWire** | yes (`midi-node.c`, `midi-enum.c`) | yes (`midi-server.c`, "server nodes") | **used:** device timestamps mapped to the system clock, keeping event intervals (`midi-node.c`), with `node.latency-offset-msec` |

BlueZ and PipeWire sources read from their main branches on 2026-11-07; Buildroot 2026.08.1 ships
BlueZ 5.86, BlueALSA 4.3.1 and PipeWire 1.6.6 (below).

A macOS app can also show the system's Bluetooth MIDI panel itself (`CABTLEMIDIWindowController`
in CoreAudioKit; the class name is from memory, not checked against Apple's documentation).

### In the model

- **A new endpoint kind, `Bluetooth`,** beside `BuiltIn`, `Port`, `Usb` and `Network`
  (`MidiEndpoint::Kind`), in `session.status`'s `midiInputs` and `midiOutputs`.
- **Two new selector rules** (RFC 0012's table): **All Bluetooth**, and **Bluetooth named**
  (equals, contains or a regular expression, as USB named). **All external** includes Bluetooth.
  A Performance stores the rules, so a part set to "All Bluetooth" plays from whichever
  Bluetooth keyboard is connected, and a device without Bluetooth shows the part "No device", as
  for any selector matching nothing.
- **Telling Bluetooth apart:** on Linux the engine's Bluetooth service knows which ALSA sequencer
  ports BlueZ made (their client, and the device each belongs to); on macOS, CoreMIDI's driver
  owner property is expected to name Apple's Bluetooth MIDI driver (unverified; step 2 checks it
  with a device). Until then they show as USB, as today.
- **Controller profiles** (RFC 0024) match by name, "contains", so a profile written for a
  controller's USB port name also matches its Bluetooth port (BlueZ's name is the device's
  advertised name plus " Bluetooth"; whether a controller advertises the same name it gives over
  USB is per controller and unverified).
- **Numbers:** a Bluetooth endpoint keeps its source number across a disconnect by the device's
  Bluetooth address, not the sequencer client, which is new on each connection. Learn (RFC 0012)
  records it by name, as for USB.
- **Apps** with the `midi` permission see Bluetooth MIDI through the engine, as all MIDI
  (RFC 0022); nothing opens the radio itself.

### Timing into the block

RFC 0004's rule applies: an input keeps its capture time to graph ingress, translated into a
block offset under an explicit latency policy, and an input without a capture time says so.

- **Each Bluetooth input has a fixed delay**, applied before block placement: events are placed at
  their (reconstructed) capture time plus the delay, at their sample within the block, rather
  than all at the block's start as the desktop does today. The delay defaults to one connection
  interval plus the block, is shown, and can be set per endpoint (0 for "as soon as it arrives").
- **Where the stack gives timestamps** (PipeWire; CoreMIDI if it does), the capture time is the
  timestamp mapped to the engine's clock; jitter goes and only the delay remains.
- **Where it does not** (BlueZ's plugin today), the capture time is the arrival time, marked as
  such, and jitter stays up to a connection interval. Fixing the TODO in BlueZ upstream, so its
  sequencer events carry the times, is the better fix than a parser of our own (an open question).
- **Output:** a part's MIDI out to a Bluetooth endpoint leaves on step 1 of RFC 0012's path, with
  RFC 0012's per-destination latency offset. The stack stamps packets at writing, so sending at
  the due time is all the engine can do.

### Reconnection and loss

- A bonded device reconnects by itself (BlueZ's `auto_connect`; macOS remembers connected
  devices, not verified across restarts). Its endpoint reappears with its number, selectors
  resolve again (RFC 0012's hotplug path), and parts play from it again without edits.
- **On a disconnect,** notes held from that input are released and its pedals lifted, at once:
  RFC 0004 says a lost link must not leave notes held. Notes the engine was sending to it are
  forgotten by the output's note ledger, since no note off can reach it (RFC 0012's ledger sends
  offs when a tap is removed, which is the case of an output still connected).
- A device that drops repeatedly is shown in the banner once, not each time.

## Bluetooth audio out

### Profiles and codecs

- **A2DP** is the music profile. SBC is the codec every device must support (the A2DP
  specification's mandatory codec; not rechecked for this RFC); AAC is what Apple's headphones
  are usually reported to prefer (not verified from Apple's documentation), and aptX, LDAC and
  others exist. Buildroot 2026.08.1 packages `sbc`, `fdk-aac`, `libopenaptx` and `liblc3`, but no
  LDAC library. BlueALSA uses AAC with `fdk-aac`, aptX with `libopenaptx`; PipeWire AAC, LC3 and
  Opus where those are built (the package files below).
- **LE Audio** (the Basic Audio Profile, codec LC3) replaces A2DP on newer headphones. PipeWire
  supports it (`bap_sink` and `bap_source` roles, `bap-codec-lc3.c`); BlueALSA does not ("the
  other BLE audio profiles are not (yet) supported", its README). It needs a controller with LE
  isochronous channels, which is the radio's question, not checked for any target here.
- **HFP and HSP,** the headset profiles, carry a microphone but are made for calls: ruled out.

### Latency

| Measured | Touch to sound | Source |
| --- | --- | --- |
| iPad Pro's own speaker | 83 ms | [Stephen Coyle, 2022](https://stephencoyle.net/airpods-pro-2) |
| AirPods Pro 2 on the same iPad | 126 ms | the same |
| Original AirPods Pro, the same test | 167 ms | the same |

So on Apple's own tuned path the second-generation AirPods Pro added about 43 ms over the
built-in speaker (one tester, one method). No figure for A2DP on Linux with BlueALSA or
PipeWire was found that could be checked; it is expected to be no better, and is **unverified**
until step 4 measures it. An LE Audio figure is not given for the same reason.

That is far over the live-playing budget (the [architecture](../architecture.md#latency) measures
key to analog output in milliseconds), and the UX says so rather than hiding it:

- Choosing a Bluetooth output shows, once and then in its details: "Bluetooth adds about *N* ms.
  Fine for jamming and practice, too late for tight playing." *N* is the delay the stack reports
  for the connected device (BlueALSA and PipeWire both report one through their PCM delay; not
  checked), or "an unknown delay" when it reports none.
- The top bar's Bluetooth mark (RFC 0030) shows the output is on, so nobody plays a gig through
  AirPods by mistake.
- A **tap-along calibration** (play a click, tap with it) refines *N* per device, since what a
  stack reports often is not the real figure, as RFC 0020 found for wired devices.

### The audio path: a mirror, through a bridge

**Recommended: a mirror of Main**, not a replacement for the main output.

- The main interface stays the clock master and the engine's only real-time output. Each block,
  the audio callback copies Main (or the output the musician chose to mirror, such as a cue bus)
  into a preallocated lock-free ring, and does nothing else for Bluetooth.
- A **bridge thread** of the engine, not the audio thread, reads the ring, resamples (below) and
  writes to the Bluetooth sink: an ALSA PCM (`bluealsa:` or `pipewire`, the recommendation below)
  on Linux, a second CoreAudio device on macOS. It may block on the sink; the audio callback never
  waits for it.
- **Mirror or Bluetooth only:** a setting mutes the main interface's outputs while Bluetooth plays
  (silent practice), but the main interface keeps running and clocking the engine; if Bluetooth
  goes, the main outputs come back unmuted only when the musician says so, never with a jump in
  volume.
- **On the desktop**, AirPods can already be the main device (`--output-device`), and JUCE reads
  CoreAudio's reported latency into `outputLatencyMs_`. That stays possible, as it is the
  operating system's choice of device; the mirror is what Oscairn adds.
- **Compensation:** live playing cannot be compensated. What can:
  - MIDI and clock sent out (RFC 0012) are aligned with the main output, not the mirror.
  - A take recording an input while the musician monitors through Bluetooth (RFC 0020) is read
    later by the Bluetooth delay in place of the main output's, since they played along to what
    they heard; the take records which output was monitored.
- The route is a device setting, not part of the Performance: a Performance opened on a device
  without Bluetooth loses nothing.

### Drift and resampling

The main interface's clock, the Bluetooth sink's pacing (the host's clock through the stack, and
the headphones' own clock behind it) and a phone's clock are independent: RFC 0004 says not to
assume two nominal rates stay aligned. The bridge runs an **adaptive resampler** steered by the
ring's fill level, as the architecture already asks of USB audio
([USB audio, both ways](../architecture.md#usb-audio-and-midi-both-ways)): one design for USB
audio, Bluetooth out and Bluetooth in.

- The resampler is band-limited and its ratio moves slowly (a control loop on the ring's fill,
  in parts per million), so drift never clicks and never shifts pitch audibly.
- Ring underrun (the sink stalled) plays silence into the sink and resynchronizes; overrun drops
  the oldest audio. Either is counted and reported, and neither reaches the main output.
- A sink at another rate (A2DP at 44.1 kHz with the engine at 48 kHz) is the same resampler with a
  different nominal ratio.

### The Linux stack

The engine owns ALSA today and must keep owning the main interface directly: that is RFC 0027's
guaranteed path. The choice is what sits between BlueZ and the engine's bridge.

| Option | What it adds | Covers | Real-time impact |
| --- | --- | --- | --- |
| **BlueALSA** (bluez-alsa) | `bluetoothd`, `bluealsad`, D-Bus, GLib, SBC (+ `fdk-aac`, `libopenaptx`); an ALSA PCM plugin | A2DP out and in, BLE-MIDI as a peripheral; with BlueZ's own MIDI plugin, as a central (timestamps lost) | Separate processes; the bridge opens its PCM like any ALSA device; its I/O threads run at the main thread's priority unless `--io-rt-priority` says otherwise |
| **PipeWire, for Bluetooth only** | the above plus PipeWire, WirePlumber (Lua), with WirePlumber's ALSA monitor off so it never opens the main card | A2DP out and in, BLE MIDI as central and peripheral with timestamps, LE Audio, more codecs | More processes and a graph scheduler of its own; the bridge reaches it through the `pipewire` ALSA PCM or a native stream |
| **PipeWire for everything** | the engine plays through PipeWire | all of the above | Puts a daemon in the guaranteed path: ruled out |
| **Our own A2DP endpoint** | Oscairn speaks BlueZ's D-Bus media API and encodes SBC itself | what we write | Rebuilds BlueALSA: ruled out |

**Recommended: BlueALSA first, with BlueZ's MIDI plugin for BLE MIDI;** PipeWire for Bluetooth
only if LE Audio or BLE MIDI timestamps turn out to need it. Reasons:

- It matches the ownership model: Bluetooth audio is just another ALSA PCM for the bridge, and
  BLE MIDI another ALSA sequencer port, which JUCE's Linux MIDI already lists.
- It is the smaller stack, with no session manager deciding device policy beside the engine's.
- The bridge is written against ALSA PCM names, and PipeWire offers an ALSA PCM too, so changing
  stacks later changes the image and the PCM name, not the engine's design.
- What it costs: no LE Audio, and BLE MIDI jitter up to a connection interval until BlueZ's
  timestamp TODO is fixed. Step 3's gate measures whether that jitter matters.

Either way the daemons run as their own users, on cores not reserved for audio (RFC 0027), below
the engine's audio threads' priority, and supervised by init, and the radio's interrupts (UART or
SDIO, USB for a dongle) are kept off the reserved cores.

## Bluetooth audio in: a phone to play along to

- The device takes the **A2DP sink** role (BlueALSA `-p a2dp-sink`; PipeWire's `a2dp_sink`): the
  phone sees it as a speaker and pairs from its own Bluetooth menu.
- The bridge reads the sink's PCM, resamples it to the main interface's clock as for output, and
  writes a ring the audio callback reads into a **graph input port named Bluetooth** (stereo). It
  is an input like the interface's:
  - a part with an audio input plays it (`PartInput::Kind::Audio` names its port), through
    effects or straight to Main;
  - a take records it as a graph input tap (RFC 0020);
  - it has a level, and a meter.
- Its latency does not matter for playing along: the musician plays to what they hear. A take
  aligning it to the bar grid has no grid to align to (the phone has no clock of ours), so takes
  record it as it arrived and say so.
- With no phone connected the port gives silence and its part is badged "No device"; a
  Performance naming it on a device without Bluetooth opens with the port shown unavailable, as
  the architecture says of [missing endpoints](../architecture.md#audio-ports-and-hardware-routing),
  and plays everything else.
- Play and pause from the device (AVRCP) and the song's name are out of scope here.

## Pairing and the UI

### Who talks to the radio

The engine's **Bluetooth service** (a part of the engine host, off the audio thread, built only
with Bluetooth, below) speaks BlueZ's D-Bus API, with every call asynchronous and timed out, so a
hung `bluetoothd` cannot stall the engine's control loop. It owns adapter power, discovery,
pairing, trust and connections, and the bridges. Apps reach it through the control API, as for
every device (RFC 0022): a `bluetooth.` namespace (`bluetooth.devices`, `bluetooth.scan`,
`bluetooth.pair`, `bluetooth.forget`, `bluetooth.connect`, `bluetooth.discoverable`,
`bluetooth.route` for the audio mirror), under RFC 0026's **System** permission, with a
`bluetooth` notification topic. On macOS the desktop leaves pairing to the system (Audio MIDI
Setup and the Bluetooth settings), and the service only lists and routes.

### The Settings app

Settings (RFC 0022, built as its own app) gets a **Bluetooth** page:

- On and off; the paired devices, each with its kind (MIDI, headphones, phone), its state, and
  Forget; **Add device**, which scans and lists what it finds by name.
- For headphones, Use for audio (mirror, or Bluetooth only), the delay and its calibration.
- For MIDI devices, the input delay (above), and the controller profile if one matches (RFC 0024).
- **Let a phone connect:** discoverable for two minutes, so a phone can pair as a source.

The shell (RFC 0030) shows a Bluetooth mark in the top bar's status while a Bluetooth device is
connected, and quick settings gain Bluetooth audio on or off and the device. Every string
(the latency warning, the reasons below) goes through the catalog (RFC 0029).

### Small screens and none

From the profile's surface (RFC 0024, RFC 0019):

- **Panel:** the Settings page as above.
- **Compact:** a Bluetooth entry in the launcher's list: on or off, a list of found devices to
  push the encoder on, Forget on a long push.
- **No screen:** pairing from a client that has one: the web UI or a paired laptop (RFC 0026),
  which draws the same page; or, with nothing paired, a button combination the profile names opens
  a two-minute pairing window, shown by an LED, in which the first device to connect is accepted.

### Security

From the Bluetooth Core Specification's Secure Simple Pairing (not rechecked for this RFC): a
device with a screen and a yes/no can use numeric comparison (both sides show a six-digit code);
one without uses "Just Works", which has no protection against a device in the middle. So:

- **Never discoverable or pairable by default.** Discovery opens only from Settings, a paired
  client, or a physical button press on a headless unit, and closes after two minutes.
- Numeric comparison wherever there is a screen; Just Works only inside a window opened on the
  device itself.
- Paired devices are kept on the user data partition (RFC 0028) and go with a factory reset.
- MIDI from a paired Bluetooth device is trusted as cabling is (RFC 0012's network safety rule):
  it plays, it does not configure.

## Products without Bluetooth

### Not built

A product without a radio carries none of the stack:

- **Oscairn:** a CMake option, `OSCAIRN_BLUETOOTH` (default off; the macOS desktop build turns it
  on), compiles the Bluetooth service, the bridges' Bluetooth sinks and the `bluetooth.` methods.
  Off, none of that code is in the binaries, and nothing links D-Bus or BlueZ. The resampler stays
  either way, since USB audio needs it too.
- **The image:** a Buildroot option in `os/buildroot/package/oscairn/Config.in`,
  `BR2_PACKAGE_OSCAIRN_BLUETOOTH`, passes `-DOSCAIRN_BLUETOOTH=ON` and selects the stack:
  `bluez5_utils` with its audio and MIDI plugins, `bluez-alsa`, `sbc`. The board's defconfig
  selects it, and the radio's firmware (from `linux-firmware`'s Bluetooth options, or the
  vendor's) with it. A defconfig without it builds an image with no `bluetoothd`, no BlueALSA
  plugin and no firmware.
- **The profile** gains an optional `bluetooth` object: `{ "midi": true, "audioOut": true,
  "audioIn": true }`, each what the product supports. Absent means no Bluetooth.
- **Kept consistent:** the defconfig names its profile already (the overlay's
  `/etc/default/oscairn` passes `--profile`); `post-build.sh` reads that profile and fails the
  build when the profile has `bluetooth` and the option is off, or the option is on and the profile
  has none. The desktop's profile is not checked this way (the operating system has the radio).

### Reported, not failing

- **Capabilities:** `session.hello` lists `bluetooth.midi` and `bluetooth.audio` only when they
  are built in and the profile has them. Clients already must check capabilities first; without
  them a `bluetooth.` call is an unknown method.
- **No radio, or radio off:** a build with Bluetooth on hardware where the adapter is missing,
  blocked or failed lists the capabilities, and `session.status` carries
  `bluetooth: { "state": "off" | "noAdapter" | "failed" | "on", "reason" }`, so Settings can say why.
  Without the capability, the field is absent.

### Handled gracefully

- **Apps** check the capability: Settings leaves the Bluetooth page out where it is not built, and
  shows it disabled with the reason ("No Bluetooth adapter", "Bluetooth is off") where it is built
  but unavailable; the MIDI menus leave out the Bluetooth rules; the recording inputs leave out
  the Bluetooth port. None errors.
- **Performances** naming a Bluetooth selector or the Bluetooth input open with it shown as no
  device or unavailable (RFC 0012's handling of a selector matching nothing) and play everything
  else; saving keeps what they named.
- **App manifests** (RFC 0022) say `"bluetooth": { "midi": "required" | "optional",
  "audio": "required" | "optional" }`. A required one is not offered by the Store on, and does not
  install on, a device whose profile lacks it (as RFC 0022 treats a platform without a build); an
  optional one installs and checks the capability.

## Reliability

Bluetooth is never part of the guaranteed path (RFC 0027):

- **The audio callback only copies** into and out of preallocated rings. It never calls the stack,
  takes a lock the bridge holds, or waits.
- **A failed sink or source** (out of range, the daemon crashed, the radio wedged) leaves the main
  output exactly as it was: the mirror goes silent, the input port gives silence, and the banner
  says so. The service reconnects with a bounded backoff.
- **The daemons are separate processes** under init's supervision, with cgroup limits like apps'
  (RFC 0025), off the reserved cores. A crash of `bluetoothd` or `bluealsad` cannot take the engine;
  an engine restart does not drop the radio's links, and the service reattaches.
- **BLE MIDI loss** releases its held notes (above).
- **A `pass-through` product** never routes its pass-through path through Bluetooth, and its
  fallback chain ignores it.
- **Admission** (RFC 0025) counts the bridges' memory and the resampler's CPU when Bluetooth audio
  is on.

## Hardware

| Target | Bluetooth | Evidence |
| --- | --- | --- |
| **Ignite** (i.MX8M Mini on a Variscite VAR-SOM) | **Likely present:** its Linux release names the NXP SD8987 Wi-Fi firmware (`sd8987_uapsta.bin`), and NXP's 88W8987 is a Wi-Fi 5 and Bluetooth 5.2 combo chip. Whether the Bluetooth half is wired and enabled on the Ignite is unverified | [Ignite Linux README](https://github.com/MusicTribeOpenSource/0716-AAF/blob/main/Linux/README.md), [NXP 88W8987](https://www.nxp.com/products/wireless/wi-fi-plus-bluetooth/88w8987-wi-fi-bluetooth-5-combo-solutions-for-wireless:88W8987) |
| **KICK** (i.MX8M Mini) | Unknown; even its Wi-Fi is an estimate in its profile | `kick.json` |
| **VAR-SOM-MX8M-MINI** in general | Offered with a built-in Wi-Fi and Bluetooth/BLE module; Variscite's pages disagree on the Bluetooth version, and a recent kernel patch describes an NXP IW61x module on newer boards | [Variscite](https://www.variscite.com/product/system-on-module-som/cortex-a72/var-som-mx8m-mini-nxp-i-mx8m-mini/), [kernel patch](https://lkml.iu.edu/2602.2/02193.html) |
| **RK3588 board** | Not chosen yet ([roadmap](../roadmap.md)) | |
| **QEMU image** | None; a USB dongle passed through to QEMU is untried | |
| **The desktop** | The computer's; macOS owns it | |

**USB dongles** work wherever there is a USB host port: the kernel's USB Bluetooth driver, plus
firmware for the chip. Buildroot's `linux-firmware` has Bluetooth firmware options for Realtek
(87xx, 88xx), Intel, MediaTek, Qualcomm and others; a dongle is the route for development and for
a product whose board lacks a radio but wants one. Which dongles support LE Audio is not checked.

**Buildroot 2026.08.1** (checked in its package tree on 2026-11-07):

| Package | Version | What it gives |
| --- | --- | --- |
| `bluez5_utils` | 5.86 | `bluetoothd`; options for the audio plugins (A2DP, AVRCP) and the MIDI plugin ("MIDI support via ALSA sequencer", `--enable-midi`); `bluetoothctl` optional; selects D-Bus and GLib |
| `bluez-alsa` | 4.3.1 | `bluealsad` and the ALSA plugin; AAC with `fdk-aac`, aptX with `libopenaptx`, BLE-MIDI when BlueZ's MIDI plugin is on |
| `pipewire`, `wireplumber` | 1.6.6 | Bluetooth with BlueZ, SBC and GLib; AAC, LC3 and Opus codecs when those packages are on |
| `sbc`, `fdk-aac`, `libopenaptx`, `liblc3` | | the codecs (no LDAC library is packaged) |
| `linux-firmware` | | the Bluetooth firmware options above; none named for the SD8987 |

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Optional and reported | `OSCAIRN_BLUETOOTH`, `BR2_PACKAGE_OSCAIRN_BLUETOOTH`, the profile's `bluetooth`, the post-build consistency check, the capabilities and status; Settings, the MIDI menus and recording inputs following them; the `Bluetooth` endpoint kind and selector rules; app manifests' `bluetooth` | A build with the option off has no Bluetooth symbols (`nm -C` on the engine and app finds none of the service's) and links no `libdbus`, `libbluetooth` or GLib through it (`ldd`); the QEMU image without the option has no `bluetoothd`, BlueALSA plugin or Bluetooth firmware; `session.hello` lacks the capabilities and `session.status` the field; a Performance with an "All Bluetooth" part and the Bluetooth input opens and plays its other parts, both shown unavailable; the post-build check fails a profile and defconfig that disagree |
| 2. MIDI on the desktop | Bluetooth endpoints recognized in CoreMIDI, numbers kept by address, notes released on disconnect, the input delay and placement within the block | A BLE MIDI keyboard plays a part chosen by "All Bluetooth" and not one chosen by "All USB"; switched off while a chord is held, nothing hangs; switched on again, it plays the same part without edits; timing against a USB connection measured and stated (latency and jitter) |
| 3. MIDI on the device | BlueZ with its MIDI plugin in the image, the Bluetooth service and `bluetooth.` methods, the Settings page | On a board or a Linux machine with a dongle, a BLE MIDI keyboard pairs from Settings and plays; jitter measured, deciding between living with it, fixing BlueZ's timestamps upstream and PipeWire's MIDI |
| 4. Audio out | The bridge, the resampler, the mirror, Bluetooth only, the delay shown and calibrated, the top bar's mark | A2DP headphones play Main for an hour with no click from drift (the ring's fill stays bounded); taking them out of range and back causes no missed deadline on the main output (counted) and the mirror resumes; the delay measured with a microphone against the main output and compared with what the stack reports |
| 5. Audio in | The A2DP sink role, the Bluetooth input port, its part and take | A phone streams for an hour into a part with no click from drift; a take records it; with the phone gone the part is badged and the rest plays |
| 6. Small screens and none | The compact list, the web UI page, the headless pairing window | A unit with no screen pairs a phone and a MIDI device from the web UI, and a phone by the button window alone; the window closes after two minutes |
| 7. LE Audio | LC3 through PipeWire for Bluetooth only, where the radio supports it | LE Audio headphones play the mirror with less delay than A2DP on the same device, measured |

## Open questions

From the owner, 2026-11-07 (to verify on the hardware): **the KICK and the Ignite are believed to
have Bluetooth; the MODX M is believed not to** ("maybe it's there in hardware but not supported in
software"), so its products build without Bluetooth (RFC 0035's rule).

Decided by the owner, 2026-11-07: **headphones mirror Main by default, as a setting** the user can
change to replace the speakers while they are connected. The Linux stack (BlueALSA or PipeWire
for Bluetooth only) is still open: the owner finds LE Audio and BLE MIDI timestamps useful and
asked how much heavier PipeWire is.

- **BlueALSA or PipeWire** for the device: BlueALSA is recommended for its size and fit; PipeWire
  for Bluetooth only is the path if LE Audio or BLE MIDI timestamps are wanted early.
- **AAC:** `fdk-aac` is licensed by Fraunhofer on its own terms; whether the image may ship it, or
  starts with SBC only (A2DP's mandatory codec), needs a licensing answer.
- **Mirror by default, or the phone's behavior** (Bluetooth headphones replace the speakers the
  moment they connect)? The recommendation is mirror, with Bluetooth only one tap away.
- **The device as a BLE MIDI peripheral** (an iPad app connects to it) in the first version, or
  only as a central?
- **BlueZ's timestamp TODO:** contribute the fix upstream, rather than carrying a parser?
- **A phone in and headphones out at once** on one radio: whether the target radios hold both
  links without dropouts is not known.
- **Which products have Bluetooth:** the Ignite's radio likely does, if wired; the KICK unknown.
- **Bluetooth on `pass-through` products** (pedals, mixers) at all, given the warning it needs?

## Ruled out

- **Bluetooth as the clock master or the only output** on a device: the main interface stays the
  engine's real-time output.
- **A sound server in the guaranteed path:** PipeWire, or anything else, owning the main interface.
- **Apps opening Bluetooth themselves:** the engine owns devices (RFC 0022).
- **The headset profiles (HFP, HSP) for music,** in or out: they are made for calls.
- **Our own A2DP stack or codecs.**
- **Bluetooth code or packages in the image of a product without a radio.**
- **Discoverable or pairable by default.**

Sources checked 2026-11-07.

# RFC 0026: Network control and the web UI

Status: proposed, 2026-11-01. Required for every product Oscairn runs on, and the only UI of a
headless one. Replaces the "Remote access" note of [RFC 0003](0003-control-api.md#remote-access);
builds on the control API and its permissions by namespace, the view models of
[RFC 0019](0019-adaptive-ui.md), apps ([RFC 0022](0022-apps-and-requests.md)) and multi-device
setlists ([RFC 0012](0012-sync-external-midi-and-multi-device.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. A network transport for the control API: TLS, pairing, sessions, off by default
- [ ] 2. Permissions per client by namespace (play, edit, library, system)
- [ ] 3. Several clients at once: one change history, changes pushed to all
- [ ] 4. Discovery on the local network
- [ ] 5. The web UI: the view models drawn in a browser, served by the device
- [ ] 6. Library sync between a device and a computer, by hash

## Problem

The control API ([RFC 0003](0003-control-api.md)) reaches the engine over a local socket only.
A rack unit has no screen; a pedal's screen is too small to build a Performance on; a keyboard
player wants to edit from a laptop or a tablet at the side of the stage; several devices in one
rig need to agree on a setlist. None of that works without the control API on the network, and
putting it there carelessly exposes a live instrument to anyone on the venue's Wi-Fi.

## Outcome

The same control API, over the network, to clients the device's owner has paired, each with the
permissions they were given; and a web UI, served by the device, that draws the same view models
as the desktop and panel, so a phone, tablet or laptop is a screen for any Oscairn product.

## Transport

- **WebSocket over TLS**, carrying the same JSON-RPC messages, notifications and batches as the
  local socket. Browsers can speak it; so can the desktop app, which becomes one more client.
- **Off by default.** Turned on in the device's settings, per network interface.
- **Pairing:** a new client shows a code; the owner confirms it on the device (or, on a headless
  unit, by a button press or from an already paired client). Paired clients keep a key; unpaired
  ones get nothing beyond the pairing exchange.
- **The device's certificate** is made on the device and shown at pairing as a fingerprint, so a
  client pins it; there is no certificate authority to trust.

## Permissions

RFC 0003 groups methods by namespace so permissions can follow. Each paired client gets some of:

| Permission | Namespaces |
| --- | --- |
| Play | `notes`, `transport`, `scenes.select`, `controls.set`, `pads.play`, setlist movement |
| Edit | `graph`, `parts`, `wires`, `params`, `scenes`, `controls`, `pads`, `edit` |
| Library | `library`, `performance`, `assets` |
| System | updates, network settings, pairing, profiles |

A stage tablet might get Play only; the owner's laptop everything. A client asking for a method it
may not use gets an error naming the permission.

## Several clients

- One change history (RFC 0011's undo) for everyone; each step records which client made it.
- Every change is pushed to every client (RFC 0003's notifications), so a laptop and the device's
  own screen stay in step.
- Edits are not locked: the last edit wins, as between the device's screen and a knob today.

## Discovery

Devices announce themselves on the local network (mDNS / DNS-SD) with their name and profile
(RFC 0024), only while network control is on. Clients list them; nothing connects without pairing.

## The web UI

- Served by the device, from its image: no internet, no app store.
- Draws RFC 0019's view models with the device's theme, in the browser's size and input mode
  (touch or mouse), as a fourth surface beside desktop, panel and compact.
- Plugin editors: the generated ones (RFC 0019's descriptions) work as they do on the panel; a
  plugin's own GUI does not travel (it is native code on the device).
- Real-time feedback (meters, positions) over the same connection, at a reduced rate.

## Library sync

A paired computer and device exchange library indexes and send the files the other lacks, by hash
(RFC 0009's assets, RFC 0021's items, RFC 0023's types): the narrow sync service RFC 0003 planned,
now one more use of the paired connection, with the Library permission.

## Open questions

- Whether the desktop app moves to the WebSocket transport too, so there is one way in.
- Pairing on a device with no screen and one button.
- Remote access beyond the local network (through a relay) — not planned; would need its own RFC.

## Ruled out

- **Unauthenticated network control,** even "just for the local network."
- **A separate remote API:** the network carries the control API, nothing else.
- **A cloud account** to pair or sync.

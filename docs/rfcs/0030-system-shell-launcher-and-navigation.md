# RFC 0030: The system shell: launchers, system bars and how screens flow

Status: proposed, 2026-11-01. Required for every product with a screen. Builds on apps and
actions ([RFC 0022](0022-apps-and-requests.md)), surfaces and input modes
([RFC 0019](0019-adaptive-ui.md)), device profiles ([RFC 0024](0024-device-profiles.md)),
reliability levels ([RFC 0027](0027-reliability-levels.md)) and the web UI
([RFC 0026](0026-network-control-and-web-ui.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. The shell: system bars, Home, Back and the running apps, on the Panel surface in the desktop app
- [ ] 2. The launcher as an app answering `oscairn.home`, with a fallback the shell draws itself
- [ ] 3. The flow language's parts: sheets, dialogs, menus, toasts and banners, shared by every app
- [ ] 4. Physical controls: each profile's buttons for Home, Back and the encoder's push
- [ ] 5. A second launcher installed and chosen; a profile that fixes its launcher
- [ ] 6. Compact: the same flow on a small screen with encoders

## Problem

Oscairn has apps (the Library, the Setlist, the sound editor, the Groovebox) but nothing that
ties them into one instrument. On the desktop, the host's windows and an Apps menu do that. On a
device there is no desktop: something has to show what is installed, move between apps, show
the instrument's state (what is open, the tempo, whether MIDI arrives, how loaded the engine
is), and give every app the same way to go back, pick, confirm and report. Without one
language for that, each app invents its own, and a third-party app feels foreign.

## Outcome

- **A shell** that every screen product runs: the system bars, Home, Back, the running apps,
  and the overlays apps share (sheets, dialogs, menus, toasts, banners). It is part of the
  system, small, and always there.
- **A launcher that is an app,** as Android's are: it answers the `oscairn.home` action
  (RFC 0022), so a user or a manufacturer can install another and choose it. Ours is good
  enough that few will want to.
- **One flow language:** the same levels, the same Back, the same overlays and the same
  wording on every app and every surface, so a musician who knows one app knows them all.

## The shell and the launcher

| | The shell | The launcher |
| --- | --- | --- |
| What it is | System UI, part of the image | An app, in a package, replaceable |
| Draws | Status bar, navigation bar, overlays, the app switcher, the fallback app list | The Home screen |
| Owns | Which app is in front, Back and Home, presenting picks (RFC 0022), the volume and panic controls | What Home shows and how apps are arranged |
| If it fails | The watchdog restarts the UI (RFC 0027); sound goes on | The shell shows its own plain list of apps until the launcher is back |

- **Home** brings the launcher forward. Apps keep running behind it with their state; Home
  never closes anything.
- **The launcher is chosen** like any action's answer (RFC 0022 step 3): the first time two
  launchers are installed, the musician picks one, and can change it in Settings. A profile
  names its default launcher and may **fix** it (a product sold as one thing, such as a pedal
  or a kiosk), in which case the choice is not offered.
- **A launcher gets** the installed apps with their names, icons and the actions they offer,
  the recent library items, and the instrument's state, all through the control API, the same
  as any app. It gets nothing a third-party app could not.
- **Desktop:** the computer's own windows are the shell. The Apps menu and a Home window (the
  launcher, in a window) are its Home; status shows in each window's status line. No navigation
  bar is drawn.

## The system bars

### Status bar (top, every screen surface)

One line, the same in every app, at 28 pixels on the Panel:

- **Left:** the open Performance's name and the current scene; a dot when it has unsaved
  changes. Tap: the Performance menu (Save, Save As, Revert, Info).
- **Middle:** the app's own title, or nothing when the app draws its own.
- **Right:** tempo and transport (a running beat dot), MIDI in (a dot that blinks per message),
  the engine's load (RFC 0025; amber over 75%, red past the budget), the clock source when not
  internal, network (RFC 0026) and battery (profiles with one), then the time.
- **Tap the right side:** quick settings as a sheet: output level, audio device and rate, MIDI
  inputs, tempo and clock, theme, brightness, and **All notes off**.

A banner below the bar reports what needs attention and stays until dealt with: a plugin that
failed (RFC 0010), an engine near its budget, a device unplugged.

### Navigation bar (Panel; bottom; hidden where buttons do it)

- **Back** (left), **Home** (middle), **Apps** (right: the running apps, to switch or close).
- **All notes off** at the far right, always, because a stuck note on stage cannot wait for a
  menu. Held for half a second, so a brush does not cut a chord.
- **Hidden when the profile's panel has Home and Back buttons** (RFC 0024 `controls`): the
  KICK's and the Ignite's physical buttons do it, and the screen gets the height back.
- **Never hidden while performing:** no gesture-only navigation. A screen edge swipe can be a
  shortcut, never the only way.

### Compact

No bars. The encoder's push enters, a long push or a Back button goes back, and a held
Home combination (named by the profile) opens the launcher's list. The status shows as the
first line of the launcher's list.

## How screens flow

### Levels

Every screen is at one of four levels, and Back always goes up one:

1. **Home:** the launcher.
2. **App:** an app's top screen, with its tabs (the sound editor's Parts, Part, Mixer, Routing).
3. **Screen:** something chosen in it (one part, one plugin, one setlist).
4. **Detail:** one thing within that (one parameter's curve, one zone).

Back from an app's top screen goes Home. Tabs are not levels: switching tabs is not undone by
Back. Opening another app (an action, RFC 0022) puts that app at its level above the one that
asked, and Back returns to the asker.

### Overlays

| | For | Looks | Ends with |
| --- | --- | --- | --- |
| **Sheet** | A pick from another app; quick settings; a short form | Rises over the lower part of the screen; the app behind stays visible and dimmed | Its action (Choose, Add) at the bottom right, Cancel at the bottom left, or Back |
| **Dialog** | A question that must be answered: discard changes, delete | Centered card, two or three buttons that say what they do ("Delete", not "OK") | One of its buttons; Back is Cancel |
| **Menu** | Everything that can be done with one thing | Opens from it: right click, long press, or its Options button | A choice, or a tap outside |
| **Toast** | It is done ("Saved", "Added to Friday") | A line at the bottom for four seconds, with Undo where it can | Itself |
| **Banner** | Something stays wrong | Under the status bar | Its action, or the problem going away |

### The rules

- **Sound first.** Nothing the UI does waits on audio or stops it; loading shows progress, and
  the instrument keeps playing what it has (seamless changes, RFC 0021).
- **Undo before confirm.** An action that can be undone happens at once with a toast offering
  Undo; only what cannot be undone (delete from the library, factory reset) asks first.
- **One main action per screen,** at the bottom right, the accent color; the rest in its menu.
- **Every action has a text label** in its menu, even when the screen shows only an icon, so
  Compact and screen readers have it (RFC 0019).
- **The knobs follow the screen** (RFC 0019): the parameters shown get the physical knobs, named
  along the screen's edge.
- **Stage safety:** while a Performance plays, nothing opens over it on its own: updates
  (RFC 0028), install prompts and non-urgent banners wait. A stage lock (in quick settings)
  ignores touches except the knobs, Back and All notes off.
- **Motion is short** (150 ms, eased) and never needed to understand anything; it is off with
  the system's reduce-motion setting.
- **Words:** sentence case, verbs on buttons, no "OK", the item's name in what is said
  ("Delete Big Strings?").

## Our launcher

The Home screen of the Panel (1024 × 600), in two parts:

- **The instrument, left:** the open Performance as a large card (its color, name, scene,
  setlist position with Previous and Next), a row of its scenes to switch, and the recent
  Performances and Sounds below, one tap to open.
- **The apps, right:** the installed apps as tiles in a grid, with icons, in the order the
  musician arranges (long press to move or hide); a tile can show a line of state (the
  Setlist's next song, the Recorder's last take).

On the desktop the same launcher is a window; on Compact it is a list: the Performance, then
the apps. A launcher may add **widgets** later; not in the first version.

## Manifests

An app's manifest (RFC 0022) adds:

- `home: true` when it is a launcher (it answers `oscairn.home`);
- its **icon** (an SVG with plain paths, recolored by the theme) and name for the launcher;
- the **surfaces** it draws (desktop, panel, compact, web), so a launcher shows only what runs
  on the product.

A profile (RFC 0024) adds `launcher` (the default app's ID), `launcherFixed`, and which of its
controls are Home, Back and All notes off.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. The shell | Status bar, navigation bar, Home, Back and Apps on the Panel surface in the desktop app (`--profile kick`) | Moving Library → Setlist → pick → Back → Home returns to each screen as it was |
| 2. The launcher app | Ours, answering `oscairn.home`; the shell's own list when it is missing or fails | The launcher killed: the list appears, sound goes on, the launcher comes back |
| 3. Overlays | Sheets, dialogs, menus, toasts and banners as shared components every app uses | The Library's pick as a sheet; Delete's dialog; Save's toast with Undo |
| 4. Physical controls | Profile buttons for Home, Back, All notes off; navigation bar hidden when present | On the KICK's profile the bar is gone and the buttons work |
| 5. Another launcher | A minimal second launcher installed; the chooser; a fixed launcher | Choosing it survives a restart; a fixed profile offers no choice |
| 6. Compact | The flow with an encoder and two buttons | Library → Load → Back → Home with no touch |

## Open questions

- Whether apps in the background may keep drawing (meters in a widget) or only the one in front.
- How many apps may run at once on a small device, and which one goes when memory is short.
- Whether the web UI (RFC 0026) shows the device's launcher or its own.

## Ruled out

- **The launcher in the shell:** neither users nor manufacturers could replace it.
- **Gesture-only navigation:** not reliable with sweaty hands on a dark stage.
- **Apps drawing their own system bars:** the instrument's state would look different in each.

# RFC 0030: The system shell: launchers, the top bar and how screens flow

Status: proposed, 2026-11-01; revised the same day (one top bar, buttons from the profile,
Home for products that are not instruments, the clock only where the time is known). Required
for every product with a screen. Builds on apps and actions ([RFC 0022](0022-apps-and-requests.md)),
surfaces and input modes ([RFC 0019](0019-adaptive-ui.md)), device profiles
([RFC 0024](0024-device-profiles.md)), reliability levels ([RFC 0027](0027-reliability-levels.md))
and the web UI ([RFC 0026](0026-network-control-and-web-ui.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. The shell: the top bar, Home, Back and the running apps, on the Panel surface in the desktop app
      (built 2026-11-01: Home first in every window. The top bar (`TopBar` in `Launcher.cpp`) on
      Home and every app on a panel: Home, Back and the running apps unless the layout binds a
      button to `home` or `back`, where the musician is, the open Performance and its scene (a
      tap: Save, Save As New), the tempo and the engine's load, and All notes off (held half a
      second; `notes.panic`) unless a button is bound to `panic`. Apps on a panel keep running
      with their state; the switcher brings one forward or closes it. Back goes Home for now.
      Not yet: apps putting their own actions in the bar, the clock, MIDI activity, quick
      settings, banners)
- [ ] 2. The launcher as an app answering `oscairn.home`, with home cards, and a fallback the shell draws itself
      (built: a launcher inside the app, `Launcher.cpp`, with the profile's apps as tiles and a
      line of state each; not yet an app of its own, no cards)
- [ ] 3. The flow language's parts: sheets, dialogs, menus, toasts and banners, shared by every app
- [ ] 4. Physical buttons: each profile's buttons opening actions, and the top bar showing only what they lack
- [ ] 5. A second launcher installed and chosen; a profile that fixes its launcher
- [ ] 6. Compact: the same flow on a small screen with encoders

## Problem

Oscairn has apps (the Library, the Setlist, the sound editor, the Groovebox) but nothing that
ties them into one product. On the desktop, the host's windows and an Apps menu do that. On a
device there is no desktop: something has to show what is installed, move between apps, show
the product's state, and give every app the same way to go back, pick, confirm and report.
Products differ: an instrument is about the open Performance, a recorder about its tracks and
takes, a DJ unit about its decks, a pedal about one preset. And their panels differ: the MODX M
has PERFORMANCE, LIVE SET and CATEGORY buttons, a pedal has two footswitches and an encoder, a
tablet has nothing but the screen.

## Outcome

- **A shell** that every screen product runs: one top bar, Home, Back, the running apps, and
  the overlays apps share (sheets, dialogs, menus, toasts, banners). It is part of the system,
  small, and always there.
- **A launcher that is an app,** as Android's are: it answers the `oscairn.home` action
  (RFC 0022), so a user or a manufacturer can install another and choose it. Ours is good
  enough that few will want to.
- **The hardware decides what the screen shows:** a profile maps the product's buttons to
  actions, and the top bar shows on screen only what the panel has no button for.
- **One flow language:** the same levels, the same Back, the same overlays and the same
  wording in every app and on every surface.

## The shell and the launcher

| | The shell | The launcher |
| --- | --- | --- |
| What it is | System UI, part of the image | An app, in a package, replaceable |
| Draws | The top bar, overlays, the app switcher, the fallback app list | The Home screen |
| Owns | Which app is in front, Back and Home, presenting picks (RFC 0022), the product's buttons, the volume and panic controls | What Home shows and how apps are arranged |
| If it fails | The watchdog restarts the UI (RFC 0027); sound goes on | The shell shows its own plain list of apps until the launcher is back |

- **Home** brings the launcher forward. Apps keep running behind it with their state; Home
  never closes anything.
- **The launcher is chosen** like any action's answer (RFC 0022 step 3): the first time two
  launchers are installed, the musician picks one, and can change it in Settings. A profile
  names its default launcher and may **fix** it (a product sold as one thing, such as a pedal),
  in which case no choice is offered.
- **A product with one main app** may have no launcher screen at all: its profile names that
  app as Home (a pedal's preset screen), and the other apps are reached from its menu.
- **A launcher gets** the installed apps with their names, icons and actions, the home cards
  (below), the recent library items and the product's state, all through the control API like
  any app. It gets nothing a third-party app could not.
- **Desktop:** the computer's windows are the shell. The Apps menu and a Home window (the
  launcher, in a window) are its Home; each window's status line carries the status.

## Home: cards and apps

Home shows what the product is for, then the apps. What it is for comes from the apps
themselves, as **home cards**:

- An app may describe one or more **cards** in its manifest: a title, a line or two of state,
  a few values (a time, a BPM, a level), meters, and up to three actions. Cards are
  **described, not drawn**, as plugin editors are (RFC 0019): the launcher draws them in its
  own style, so any launcher can show any app's card, and nothing from another process is
  embedded.
- The **profile names the focus**: which apps' cards lead Home (`focus` in RFC 0024's profile).
  The launcher gives the focus cards the most room and the rest of the apps a grid or a row.
- The launcher arranges by what the cards and the screen allow:

| Product | Focus cards | Home on a 1024 × 600 panel |
| --- | --- | --- |
| Instrument (MODX, KICK) | The open Performance: name, scene, set position, Previous and Next | The card on the left, the apps on the right |
| Recorder (a multitrack box) | The session: transport, time, armed tracks with meters, time left on the card | The card across the top, the apps as a row below |
| DJ unit | One card per deck: track, BPM, position, key | Two decks side by side, the apps as a row below |
| Mixer or stagebox | The scene, and the channels' meters | The card across the top, the apps below |
| Pedal or multi-effects | The preset and its chain | No launcher screen: Home is the preset |

- Recent items (Performances, Sounds, takes, tracks) are a card of the launcher's own, from the
  library (RFC 0021 and RFC 0023's item types), shown where there is room.
- The musician can move and hide tiles and cards (long press, or a tile's menu).

## The top bar

**One bar, at the top, in every app.** No navigation bar and no separate action bar: the
top bar carries navigation, where the musician is, the app's own actions and the status, and the
rest of the screen belongs to the app. On the Panel it is 40 pixels high (its touch targets 44
square, reaching into the content's margin); on the desktop in touch mode the same, in mouse
mode 30.

From left to right:

1. **Home and Back,** as icons, each only where the panel has no button for it (below).
2. **Where:** the app's name and the screen, as a short trail ("Library › Big Strings"); a
   tap on a part of it goes there. On Home, the product's name or nothing.
3. **Context:** the focus card's one-line summary (an instrument: the open Performance and
   scene, with a dot when edited; a recorder: the take's time; a DJ unit: the BPM). A tap opens
   its menu (Save, Save As, Revert, Info for a Performance).
4. **The app's actions:** its main action, then up to two more as icons, then its menu (⋮)
   with everything else. The app supplies them; the bar draws them, so they sit in the same
   place in every app.
5. **Status,** as compact marks: tempo, MIDI in (a dot that blinks per message), the engine's
   load (RFC 0025; amber over 75%, red past the budget), clock source when not internal,
   network (RFC 0026), battery (profiles with one), and the time (below). A tap opens quick
   settings as a sheet: output level, audio device and rate, MIDI, tempo and clock, theme,
   brightness, stage lock and **All notes off**.
6. **All notes off,** as an icon, where the panel has no button for it. Held half a second, so
   a brush does not cut a chord.

What does not fit (a narrow screen, a long title) gives way in this order: the trail shortens
to the screen's name, the app's extra actions move into its menu, status marks collapse into
one that opens quick settings. Home, Back, the main action and All notes off never go.

A **banner** under the bar reports what needs attention and stays until dealt with: a plugin
that failed (RFC 0010), an engine near its budget, a device unplugged.

### The product's buttons

A profile (RFC 0024) names its buttons and the action each opens, so the hardware's buttons
are the shortcuts and the screen does not repeat them:

```json
"buttons": [
  { "control": "performance", "label": "PERFORMANCE (HOME)", "action": "oscairn.home" },
  { "control": "live-set",    "label": "LIVE SET",          "action": "oscairn.open.setlist" },
  { "control": "category",    "label": "CATEGORY",          "action": "oscairn.pick.performance" },
  { "control": "exit",        "label": "EXIT",              "action": "oscairn.back" },
  { "control": "shift+stop",  "label": "SHIFT + STOP",      "action": "oscairn.panic" }
]
```

- An action a button opens is answered by whichever app the musician chose for it
  (RFC 0022): LIVE SET opens a third-party setlist app as readily as ours.
- `oscairn.home`, `oscairn.back` and `oscairn.panic` are the shell's own; the top bar leaves out
  each one a button provides. A panel's on-screen button may stay where the hardware puts
  Home on the screen (the MODX M shows Home in its top bar): `"onScreen": ["home"]` keeps it.
- A button may open a mode of an app (a library pick by category) through an action's
  parameters, as RFC 0022 actions take them.

### The clock

The time is shown only where the device knows it: a clock that keeps time while off (a
battery-backed real-time clock), the time from the network, or the time set by a connected
computer or by the musician since it started. A device that does not know the time shows no
clock and stamps files with no time rather than a wrong one. The profile says what the
hardware has (`"clock": "rtc"`, `"network"` or `"none"`); the shell shows the time only once it
has been set. Whether the MODX M keeps a clock is not known here: saving a file to a USB stick,
powering the instrument off and on, and looking at the file's time on a computer would tell.

### Compact

No bar. The encoder's push enters, a long push or a Back button goes back, and the button the
profile names Home opens the launcher's list. The status is the list's first line.

## How screens flow

### Levels

Every screen is at one of four levels, and Back always goes up one:

1. **Home:** the launcher.
2. **App:** an app's top screen, with its tabs (the sound editor's Parts, Part, Mixer, Routing).
3. **Screen:** something chosen in it (one part, one plugin, one setlist).
4. **Detail:** one thing within that (one parameter's curve, one zone).

Back from an app's top screen goes Home. Tabs are not levels: switching tabs is not undone by
Back. Opening another app (an action, RFC 0022) puts that app above the one that asked, and
Back returns to the asker. A product button that opens an app (LIVE SET) puts it above
wherever the musician was.

### Overlays

| | For | Looks | Ends with |
| --- | --- | --- | --- |
| **Sheet** | A pick from another app; quick settings; a short form | Rises over the lower part of the screen; the app behind stays visible and dimmed; its own top line has Cancel at the left and its action at the right | Its action, Cancel, or Back |
| **Dialog** | A question that must be answered: discard changes, delete | Centered card, two or three buttons that say what they do ("Delete", not "OK") | One of its buttons; Back is Cancel |
| **Menu** | Everything that can be done with one thing | Opens from it: right click, long press, its Options, or the bar's ⋮ for the screen | A choice, or a tap outside |
| **Toast** | It is done ("Saved", "Added to Friday") | A line at the bottom for four seconds, with Undo where it can | Itself |
| **Banner** | Something stays wrong | Under the top bar | Its action, or the problem going away |

### The rules

- **Sound first.** Nothing the UI does waits on audio or stops it; loading shows progress, and
  the product keeps playing what it has (seamless changes, RFC 0021).
- **Undo before confirm.** An action that can be undone happens at once with a toast offering
  Undo; only what cannot be undone (delete from the library, factory reset) asks first.
- **One main action per screen,** in the top bar, in the accent color; the rest in the menu.
- **Every action has a text label** in its menu, even when the bar shows only an icon, so
  Compact and screen readers have it (RFC 0019).
- **The knobs follow the screen** (RFC 0019): the parameters shown get the physical knobs,
  named along the screen's edge.
- **Stage safety:** while the product plays, nothing opens over it on its own: updates
  (RFC 0028), install prompts and non-urgent banners wait. A stage lock (in quick settings)
  ignores touches except the knobs, Back and All notes off.
- **Motion is short** (150 ms, eased) and never needed to understand anything; it is off with
  the system's reduce-motion setting.
- **Words:** sentence case, verbs on buttons, no "OK", the item's name in what is said
  ("Delete Big Strings?").

## Manifests and profiles

An app's manifest (RFC 0022) adds:

- `home: true` when it is a launcher (it answers `oscairn.home`);
- its **icon** (an SVG with plain paths, recolored by the theme) and name;
- its **cards**, described (title, state lines, values, meters, up to three actions);
- the **surfaces** it draws (desktop, panel, compact, web), so a launcher shows only what runs
  on the product.

A profile (RFC 0024) adds:

- `launcher` (the default launcher's ID) and `launcherFixed`, or `home` naming the one app
  that is Home on a product without a launcher screen;
- `focus`: the apps whose cards lead Home;
- `buttons`: each button's control, label and action, and `onScreen` for what stays on screen
  anyway;
- `clock`: `rtc`, `network` or `none`.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. The shell | The top bar, Home, Back and the running apps on the Panel surface in the desktop app (`--profile kick`) | Library → Setlist → pick → Back → Home returns to each screen as it was |
| 2. The launcher app | Ours, answering `oscairn.home`, with the Performance card and a recorder-style card from a test app; the shell's own list when it is missing or fails | The launcher killed: the list appears, sound goes on, the launcher comes back |
| 3. Overlays | Sheets, dialogs, menus, toasts and banners as shared components every app uses | The Library's pick as a sheet; Delete's dialog; Save's toast with Undo |
| 4. Product buttons | `buttons` in profiles; the top bar leaving out what the panel has | A MODX-style profile: LIVE SET opens the Setlist, CATEGORY the Library's pick, no Back on screen |
| 5. Another launcher | A minimal second launcher installed; the chooser; a fixed launcher | Choosing it survives a restart; a fixed profile offers no choice |
| 6. Compact | The flow with an encoder and two buttons | Library → Load → Back → Home with no touch |

## Open questions

- Whether apps in the background may keep updating their cards while another app is in front,
  and how often.
- How many apps may run at once on a small device, and which one goes when memory is short.
  Proposed in [RFC 0025](0025-resource-budgets.md#memory-reserved-committed-and-opportunistic):
  none has to go. Every running app keeps its reservation, and the always-available apps are
  reserved even when closed. Before launching any other app, the shell asks the engine to admit
  its reservation. If it does not fit, the shell refuses the launch with the reason and what
  closing would free; it never closes a running app to make room.
- Whether the web UI (RFC 0026) shows the device's launcher or its own.

## Ruled out

- **The launcher in the shell:** neither users nor manufacturers could replace it.
- **A bottom navigation or action bar:** two bars take height a 600-pixel screen needs, and the
  hardware's buttons already do what one would.
- **Apps drawing other apps' cards, or embedding them:** cards are described, so any launcher
  draws any app's.
- **Gesture-only navigation:** not reliable with sweaty hands on a dark stage.
- **A clock from a guess:** a wrong time on a file is worse than none.

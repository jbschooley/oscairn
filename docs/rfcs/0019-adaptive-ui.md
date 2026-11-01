# RFC 0019: One UI across desktop, touch panel and small screens

Status: in progress, 2026-10-31. Step 1 done; steps 2, 3 and 5 started (see [Progress](#progress)). Builds on the views and plugin editors of
[RFC 0011](0011-performance-ui-and-editors.md), the control API
([RFC 0003](0003-control-api.md)) and the layouts and themes of the
[design language](../design-language.md).

## Progress

Checked when built and tested; the sections below say how.

- [x] 1. Surfaces and view models ([status](#step-1-status))
- [ ] 2. Desktop shell: menu bar, toolbar, parts list, tabs and detached windows done; dockable
      panes with a saved arrangement, the inspector and shortcuts remain
- [ ] 3. Generated editors: parameter pages from module paths, `oscairn.ui` draft hints, panel
      row knobs, the Compact test layout and the gate's checks done (Odin 2 on Desktop, Panel
      and Compact); the Sampler has no parameters to edit yet
- [ ] 4. Plugin windows
- [ ] 5. Rich widgets: envelope, LFO preview, key and velocity map done; waveform, step lane
      and the Modular grid remain
- [ ] 6. Front panels

## Problem

Oscairn runs on very different screens:

- **A desktop or laptop**, with a large screen, a keyboard and a mouse, and sometimes a
  touchscreen. It should show the most information and the most options, and look and work
  like a desktop app, as Gig Performer does: menus, panes, several windows, shortcuts,
  right-click menus, drag and drop, and a plugin's own window when it has one.
- **A device's touch panel**, such as the KICK's 1024 × 600 screen with knobs and buttons
  around it. It must be enough to design sounds without a computer, in a touch interface
  like the MODX's and the Montage M's: tabs, large targets, the knobs following what is on
  screen.
- **A small screen**, such as a guitar pedal's, with a few encoders and perhaps no touch.
  We don't design it now, but nothing we build should stop it.

Today there is one JUCE app. The window is built from a device layout
(`hosts/standalone/layouts`), and the desktop layout is the device screen stretched, with
knobs and pads around it. Its screens are sized for 1024 × 600 and touch. Some of their
state is already kept apart from drawing and tested on its own (`PartsModel`,
`SoundPickerModel`); most is not. Plugin editors are generic knob pages. No plugin window
can be opened yet.

If each plugin and each screen is built once per surface, three surfaces triple the work, and
third-party plugins never reach the small ones. If one UI is stretched across all of them,
the desktop feels like a device and the pedal can't show it at all.

## Outcome

- **Surfaces:** a *surface* is a class of UI: **Desktop**, **Panel** (a device touch screen
  with physical controls) or **Compact** (a small screen with encoders). A layout names its
  surface. The desktop app can open any of them, so each can be tried without the hardware.
- **Shared view models:** every screen's state and actions live in view models that know
  nothing about drawing. Each surface draws them its own way. A change made on one surface
  shows on the others, because all of them go through the control API.
- **Plugin editors described, not drawn:** a plugin describes its editor by meaning: pages,
  groups, parameters with roles and priorities, and a few rich widgets. Each surface draws
  the description to suit it. Every CLAP plugin gets a description generated from its
  parameters; first-party plugins and willing third-party plugins write better ones. This
  is the "description format" RFC 0011 leaves for later.
- **The desktop looks like a desktop app,** with more views and options than the device, and
  an input mode: **Keyboard and mouse**, **Touch** or **Automatic**.
- **Compact is designed for, not built:** descriptions carry enough priority and structure
  that a small-screen renderer can be added later without changing any plugin.

## Surfaces

| | Desktop | Panel | Compact (later) |
| --- | --- | --- | --- |
| Example | Mac or Linux computer | KICK, Ignite, a MODX-style keyboard | Guitar pedal, rack unit |
| Screen | Any size, resizable, several windows | Fixed, about 1024 × 600, touch | About 320 × 240 down to 128 × 64; maybe no touch |
| Input | Keyboard and mouse, or touch (input mode) | Touch, the device's knobs and buttons | A few encoders and buttons |
| At once | Several panes side by side | One main view, with a strip of tabs and quick controls | One list or one parameter |
| Plugin editor | Description, generated pages, or the plugin's own window | Description or generated pages | The description's top-priority parameters, as a list |
| Built by | The desktop app | The desktop app full screen, on the device's Linux image | A renderer to be chosen |

A fourth surface, the **web UI** served by a device to a phone, tablet or laptop, draws the same
view models ([RFC 0026](0026-network-control-and-web-ui.md)); it is the only screen of a
headless product. Which surface a product uses, and its layouts, follow from its device profile
([RFC 0024](0024-device-profiles.md)); text comes from catalogs ([RFC 0029](0029-internationalization.md)).

### Desktop

Gig Performer is the model for how it feels. Not a device screen in a window, but:

- **A menu bar and toolbar,** with every action in a menu and a keyboard shortcut for the
  common ones. Edit already has undo and redo.
- **Panes** for Parts, Part, Mixer, Routing, the plugin editor, the browser and an inspector
  for the selection, which can be docked side by side, tabbed or closed, with the
  arrangement saved per user. Routing and Mixer can be open while the Part page is edited.
- **Several windows:** a plugin editor or the mixer can go in its own window, on a second
  screen.
- **More options than the device:** everything the device has, plus things a small screen
  hides behind pages, such as all of a node's ports in Routing, wire levels in a table, and
  every parameter of a plugin in one scrolling list with search.
- **Desktop habits:** right-click menus, drag and drop (plugins from the browser into a
  chain, wires in Routing), multiple selection, hover help, double-click to edit, scroll
  wheel and fine adjustment with a modifier key.
- **The plugin's own window,** through CLAP's GUI extension, as a floating window, and
  embedded in a pane where the plugin and platform allow it.
- **Front panels, later:** a performance can have panels of knobs, sliders and buttons the
  user places and binds to any parameter, like Gig Performer's panels. The layout format
  already places controls and binds them (`"bind": "synth:1"`), so a front panel is a
  layout saved with the performance, not a new format.

The device views remain available on the desktop: View > Layout opens the KICK, the Ignite or
any layout, for testing and for people who prefer them.

### Input mode

The desktop's input mode is a per-user, per-device preference, like the theme:

| | Keyboard and mouse | Touch |
| --- | --- | --- |
| Control size | The design language's 32–36 pixels, dense | At least 44 pixels, with more spacing |
| More actions | Right-click menu, hover help | Long press, an info button |
| Adjust a value | Drag, scroll wheel, a modifier for fine steps, type a value | Drag with larger travel, a fine-adjust button, the on-screen keyboard to type |
| Lists | Scroll wheel, keyboard focus and arrow keys | Swipe with momentum |

**Automatic** chooses Touch on a computer whose only pointer is a touchscreen, and
otherwise Keyboard and mouse. Both modes keep keyboard shortcuts and multi-touch. The mode
changes size and gestures, not what the panes show: a touch laptop still gets the desktop's
panes and options. Panel uses Touch sizes always.

### Panel

This is the device experience RFC 0011 describes, and the mockups reviewed on 2026-10-30
show:

- one main view at a time, chosen from tabs (Parts, Part, Mixer, Routing, Editor);
- large targets and full-screen editors for one part or one plugin at a time;
- the physical knobs following the parameters on screen, by priority, with their names and
  values shown next to them;
- every action reachable by touch, with no right-click or hover.

### Compact

Not built now. To keep it possible:

- every plugin description gives each parameter a priority, so the most important 4–8 can be
  shown alone and mapped to encoders;
- groups and pages give a menu structure that works as a list, one level at a time;
- every action in a view model has a text label and works without touch or a pointer;
- the system screens a pedal needs (choose a sound, edit its top parameters, save, tuner)
  are small, and are written for that device when it exists, from the same view models.

A test layout in the desktop app at 320 × 240 with four encoders, showing a plugin's
description as Compact would, is the check that this holds (step 3). It is built: View >
Layout > Compact (`layouts/compact.json`), see [Step 1 status](#step-1-status).

## Plugin editor descriptions

A plugin provides a description through an Oscairn CLAP extension, `oscairn.ui`, MIT
licensed like the other extensions
([RFC 0001](0001-license.md)). It is data, not code: the host draws it in the system theme,
on every surface, with the physical controls mapped. Without the extension, the host
generates one.

**Contents:**
- **Pages and groups,** in order, each with a name: "Oscillator", "Filter", "Amp".
- **Parameters,** by CLAP parameter ID, each with:
  - a **priority** (1 is most important), which decides what Panel puts on the knobs first
    and what Compact shows at all;
  - an optional **role** (cutoff, resonance, attack, level, pan …), which lets the host map
    physical controls by default ([RFC 0011](0011-performance-ui-and-editors.md#defaults-so-nothing-must-be-programd))
    and use the right widget;
  - an optional **widget hint**: knob, slider, switch, choice, or a field in a rich widget.
- **Rich widgets,** a small fixed set the host draws on every surface that has the room, each
  bound to parameters or to plugin operations:
  - an envelope (attack, decay, sustain, release, with optional more segments);
  - a keyboard and velocity map (sample zones, splits);
  - a step lane;
  - a module grid ([chillbeast's Modular](../modular-engine-evaluation.md));
  - a waveform with markers (sample start, loop).
- **Operations** for edits that aren't parameter changes, such as loading a sample into a
  zone or moving a module: named actions with arguments, sent through the control API
  ([RFC 0011](0011-performance-ui-and-editors.md#plugin-editors)), never pixels.

**Generated descriptions:** one page per CLAP module path (`clap_param_info.module`), or one
page for a plugin with none; priority in the plugin's parameter order; roles guessed from
names only where unambiguous ("Cutoff", "Resonance", "Attack").

**How surfaces use it:**
- Desktop shows every page at once in a scrolling editor, with the rich widgets large.
- Panel shows one page at a time, rich widgets full width, knobs bound to the page's
  parameters by priority.
- Compact lists the pages, then each page's parameters by priority, one at a time.

## Toolkit

**Desktop and Panel stay on JUCE.** The app is already JUCE, the device image already boots
into it on X11 (`os/buildroot/board/common/session-juce-x11`, so far in QEMU only), and JUCE 9
has multi-touch on Linux through XInput2 ([architecture](../architecture.md#touch)). One
codebase draws both, from the same view models and theme.

**Compact's renderer is chosen when a device needs it.** A small screen on a weaker processor
may not run X11 and JUCE well. Because the descriptions and view models don't depend on JUCE,
it can be something lighter, such as LVGL drawing straight to the display, without changing a
plugin.

**To check first:** JUCE's frame rate and touch latency on the KICK's i.MX8M Mini, under audio
load. If it fails, Panel's renderer changes, and the view models and descriptions do not.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Surfaces and view models | The surface a layout names; the input mode preference; the Parts, Part and Mixer views on view models, drawn for Desktop and Panel | The view models' tests run without drawing; each view renders in Desktop (both input modes) and in Panel at 1024 × 600 |
| 2. Desktop shell | Menu bar and shortcuts, dockable panes with a saved arrangement, the inspector, a second window | Parts, Mixer and an editor open side by side and edit one Performance; the arrangement survives a restart |
| 3. Generated editors and the description | `oscairn.ui` header and generated descriptions; Desktop, Panel and a Compact test layout drawing them; Basic Synth and Sampler descriptions | Basic Synth, Sampler and one third-party plugin editable on Desktop and Panel without a plugin window; the Compact test layout reaches each one's top parameters with four encoders |
| 4. Plugin windows | CLAP GUI on the desktop: floating windows, embedded where supported, and from worker processes | A third-party plugin's window opens, edits and closes without stopping audio, also when its worker restarts |
| 5. Rich widgets | Envelope, key and velocity map, waveform, step lane; then the Modular grid (RFC 0011 step 5) | The Sampler's zones and envelope edited by touch on Panel and by mouse on Desktop |
| 6. Front panels | User-placed controls bound to any parameter, saved with the Performance, on Desktop | A panel of controls for three plugins, built and played without the plugins' editors |

Steps 1–3 come before RFC 0011's step 5, and cover its knob pages.

## Step 1 status

Started 2026-10-31. A layout names its surface (`"surface": "desktop"`), and the desktop
layout is now the Desktop surface (`hosts/standalone/DesktopShell.cpp`):
- a toolbar with the tempo (the clock every plugin follows), the scene and Store, the audio
  device and MIDI inputs, and undo and redo as icons;
- the parts and buses down the left, each with what plays and from where, mute and solo
  marks, a meter per channel of its sound and a mark where its fader sits;
- tabs for the selected part, the Mixer, Routing and Controls. Each tab, or
  any tab from its right-click menu, opens in a window of its own as well.

The Groovebox is an app, not part of the sound editor: it opens from the Apps menu in a window
of its own, with its Play and Stop, which start the sequencer, not the window. A part's
meters have one bar per channel of its sound, its last plugin's main audio output as wired to
where it plays (`parts.meters`' `channels`), after its fader and pan. On the desktop, text
follows a type scale by role and every control shares the theme's corners
([design language](../design-language.md)); the parts views take tighter rows with a mouse.
The input mode (View > Input: Automatic, Keyboard and mouse, Touch) is saved beside the
theme selection; Automatic means keyboard and mouse for now. `desktop-shell` builds a small
Performance, plays a chord, checks the meters move and renders every tab in both input modes
(to PNGs when given a path). The device layouts are unchanged.

On the desktop the Part page's settings are a grid of tiles, each its name above its control,
several to a row, the last in a row widening to fill it; device screens keep one row each.
Themes choose how controls and sections are drawn (format 3: Soft, Classic and Block, the last
with sections joined edge to edge).

Step 3 started: selecting a plugin on the desktop's Part page shows all its parameters as
pages of tiles (`ParameterGrid`), built from their CLAP module paths, which `params.list` now
carries. `desktop-shell` renders the Basic Synth's pages, and those of any plugin given on its
command line (the FM and analog engines were checked: 595 and 294 parameters). A page of more
than 24 parameters gets a tab per second-level module, and a search box finds parameters on
any page by name or module. Step 5's envelope widget is drawn where a module's parameters
are named like an ADSR, a DX7 envelope or an FM-X one, until plugins can describe theirs;
dragging a point sets its time and level. LFOs get a preview of their waveform the same way.
The desktop's Part page edits keys and velocities on a keyboard (`ZoneEditor`): drag either
end of a range or the whole range; split lanes have the keys. The parts model now reads and
sets a part's velocity range.

Step 1's gate met, 2026-10-31. The views read their state from data that has no drawing in
it: `readParts` (the Parts list, Mixer strips and meters), `readRouting` and `partFields` (the
Part page's settings, in order and width), all in `PartsModel` and tested by
`desktop-parts-model` without a window. The Part page draws those fields as one grid of tiles
on every surface, with the keyboard, parameter pages, envelopes and LFOs; the device's own row
layout and key lists are gone, and its Parts view uses the touch sizes. `desktop-shell` renders
every view on the Desktop in both input modes, and `desktop-pad-editor` renders them on the
Panel at 1024 × 600.

Since then: parameters with 2 to 4 values are rows of buttons (Mono | Poly), lists hold up to
64 values (filter types), and a module with a cutoff and resonance gets its filter's response
drawn for the type its value text names.

**From the MODX's screens (2026-10-31), still to build:**
- *Knobs and ranges where a plugin wants them:* done in `oscairn.ui/draft-2`
  (`oscairn-clap/ui.h`), a hint per parameter: knob, slider, buttons, switch, list or range
  (with its high parameter), and a priority. The host reads the hints with the parameters'
  info, keeps each only where it fits, carries them through workers and `params.list`, and
  the pages draw them and order a section's tiles by priority. The Basic Synth gives them.
  `kick-clap-host` checks the host's checks on a fixture's good and bad hints, `worker-host`
  that they cross a worker, `control-json-protocol` the protocol, and `desktop-shell` the
  Basic Synth's knobs and order. A range's two-sided slider is drawn but not yet checked by a
  test; no plugin gives a range yet.
- *Linked tiles:* tiles that belong together (a hint's group, or an envelope's, LFO's or
  filter's parameters recognized by name) are one run, in order (Attack, Decay, Sustain,
  Release), drawn edge to edge in one card with a line between each. A run that does not fit
  what is left of a row starts the next one (the tile before it widening to the row's end); a
  run wider than the screen goes on in rows that touch, its last tile filling the row. The
  grid has as many columns as the width allows, two to six. `desktop-shell` checks the Basic
  Synth's linked LFO, and given the engines, that the analog filter envelope's stages stay
  together at 1,180 and 560 pixels and that no controls overlap on any of their pages.
- *Physical knobs following the screen* (step 6): done for parameter pages. A layout's knob
  bound `row:N` plays the Nth parameter of the line selected on screen; the panel counts them
  and works out the screen's edge they sit beyond. Knobs above or below the screen play a row,
  left to right; knobs beside it play a column, top to bottom. On a parameter page a tap on a
  tile selects its line (the first until then), its tiles take a tint and a border in the
  accent, and a strip along that edge
  names each knob's parameter (and its value, when the strip runs down a side). A device
  without such knobs shows no strip. On device layouts the Parts views now sit over the
  screen, scaled with it, so the panel's knobs stay in reach. The KICK's P1 to P4 and the
  concept keyboard's Mod 1 to 3 are row knobs. `desktop-shell` checks, on a panel with four
  knobs right of its screen, that the first column is theirs, that tapping a tile in the next
  column moves them there, that turning P1 sets its first parameter, and that the line and the
  knobs go with their page and panel; and on a panel with knobs under its screen, that they
  play the first row. These are sound-editing controls: what the knobs do while playing, with
  no page open, is RFC 0011 step 6's defaults. Not yet: rows on the Part page, Mixer and
  Routing; defaults for the knobs when no page is open; endless encoders' relative turns.
- *No fixed effect sends or EQ in the Mixer:* sends go to the Performance's own buses, and an
  EQ is a plugin put wherever it is needed (RFC 0016).

**The Compact test layout (2026-11-01):** a layout's `"surface": "compact"` draws its panel
as a device's, with its screen showing `CompactEditor` instead of the device screen: the
Performance's plugins as a list, then a plugin's pages, then a page's parameters by priority,
four at a time, as a column above each of four encoders (its name, its value as the plugin
writes it, and a bar). Knobs bound `encoder:N` are those encoders: in a list, turning chooses
and pressing encoder 4 opens; on a page each turns its parameter (a value per detent when
stepped, else a hundredth of the range) and pressing encoder 4 shows the next four; pressing
encoder 1 goes back. It draws the same description as Desktop and Panel: the pages and
priority order come from one function (`editorPages`, `byPriority`) that `ParameterGrid` uses
too. `desktop-shell` runs step 3's Compact gate on `layouts/compact.json`, with the encoders
alone: for the Basic Synth, the Sampler and, where `OSCAIRN_THIRD_PARTY_CLAP` names one (the
build sets it to Odin 2 beside the checkout when it is there; the test says when it skips),
a third-party plugin, it chooses the plugin from the list, checks that every page's encoders
hold its top four parameters, turns each and sees the value change, and pages on with encoder
4. Checked with Odin 2 (258 parameters, one page). The Sampler has no parameters yet (it is
edited through its sounds and the Part page's zones), so its part of the gate shows it as
having none. Loading Odin 2 needed a host fix: the CLAP host checked the plugin's functions
before `init`, which plugins built on clap-helpers only fill in during `init`, and then
destroyed it uninitialized, which aborts them.

**Editors for real plugins (2026-11-01):** checked with Odin 2, whose 258 parameters have no
module paths and come in its IDs' order (the host lists parameters by ID). A plugin of more
than 24 parameters with no module paths now gets them from its names' leading words, in name
order (`editorParameters`): "Osc2 Detune" is Detune on the Osc2 page, "Arp Step 6 Transpose"
Transpose on the Arp page's Step 6 tab; a word leading one name only stays in it. Odin 2 gets
26 pages, and its envelopes are recognized and drawn. The page list scrolls when it does not
fit, tabs wrap onto more rows, and a value text that is a bare number with more than three
decimals ("0.0100000", JUCE's default) is shortened ("0.01"). `desktop-shell` checks the
names' modules on a made-up plugin and, for each gate plugin, every page and tab on the
Desktop (1,180 × 820) and on a Panel's screen (1,024 × 600) with no controls overlapping, and
that a slider edits it (Odin 2: 26 pages; 87 of its pages' top parameters turned on Compact).

**Pages that change with a mode (2026-11-01):** a plugin whose mode hides or renames
controls calls CLAP's info rescan. The CLAP host now takes such a rescan in place (names,
modules, the hidden and periodic flags, and the `oscairn.ui` hints), without deactivating the
plugin, where it used to restart it; the session tells clients the graph changed when a
plugin's parameter info does, so remote editors read it again; and the parameter pages and
Compact compare the info twice a second and rebuild, keeping the page shown where it still
exists. `kick-clap-host` checks that the test fixture's mode hides its Toggle by an info
rescan without a restart, and `desktop-shell` that the Desktop's pages and Compact drop the
Toggle in mode 3 and show it again in mode 0. Plugins in worker processes do not pass such
rescans on yet.

Not yet: the Mixer and Routing views' layout is still in `PartsView`, and the device's chain
row, tabs and header are the older ones; Compact's real renderer waits for a device (by
design).

## Open questions

- Whether a device's touch panel ever needs the desktop's panes (a large touchscreen
  keyboard), or whether Panel's one-view model always fits.
- How Desktop and Panel share one editing session when both are open on the same engine
  (also open in RFC 0011).
- Embedding a plugin window from a worker process: X11 can embed another process's window,
  macOS can't embed another process's view, so there it may stay a floating window.
- What Compact needs from the controller-mapping service when it has no touch.
- Following CLAP's own work on remote controls (`clap.remote-controls`), which covers part of
  the priority and paging above; the description should use it rather than duplicate it
  where it is enough.

## Ruled out

- **One UI stretched across every screen:** the desktop would feel like a device, and the
  pedal could not show it.
- **Plugin-drawn editors on devices:** as in RFC 0011, devices draw descriptions.
- **Code in descriptions:** they are data, so a plugin can't break or slow the host's UI.
- **Separate per-surface plugin editors written by each plugin:** third-party plugins would
  not write three.

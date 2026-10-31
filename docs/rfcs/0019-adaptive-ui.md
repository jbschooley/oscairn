# RFC 0019: One UI across desktop, touch panel and small screens

Status: proposed, 2026-10-31. Step 1 started (below). Builds on the views and plugin editors of
[RFC 0011](0011-performance-ui-and-editors.md), the control API
([RFC 0003](0003-control-api.md)) and the layouts and themes of the
[design language](../design-language.md).

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
description as Compact would, is the check that this holds (step 3).

## Plugin editor descriptions

A plugin provides a description through an Oscairn CLAP extension, `oscairn.ui`, MIT
licensed like the other extensions
([RFC 0001](0001-licence.md)). It is data, not code: the host draws it in the system theme,
on every surface, with the physical controls mapped. Without the extension, the host
generates one.

**Contents:**
- **Pages and groups,** in order, each with a name: "Oscillator", "Filter", "Amp".
- **Parameters,** by CLAP parameter ID, each with:
  - a **priority** (1 is most important), which decides what Panel puts on the knobs first
    and what Compact shows at all;
  - an optional **role** (cutoff, resonance, attack, level, pan …), which lets the host map
    physical controls by default ([RFC 0011](0011-performance-ui-and-editors.md#defaults-so-nothing-must-be-programmed))
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
command line (the FM and analog engines were checked: 595 and 294 parameters).

Not yet: the views' state on view models of their own, the parts views redrawn for the
desktop (they are the device's, with the desktop's text), and Panel renders in the same test.

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

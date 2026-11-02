# Oscairn interface and themes

Oscairn uses one visual language for system navigation, transport, plugin browsing,
prepared sounds, generated parameter editors, dialogs and device panel controls.
A device **layout** describes geometry and bindings. A **theme** describes appearance.
Changing either must not change the audio graph, parameters, MIDI assignments or transport.

## Shared design rules

- Use the semantic roles below, never private color constants in a system component.
- Use an 8-pixel spacing unit, 16-pixel outer margins and 30-pixel desktop controls with a
  keyboard and mouse, 44 pixels with touch ([RFC 0019](rfcs/0019-adaptive-ui.md#input-mode)).
  Hardware layouts retain their measured geometry and scale as a unit.
- Use a sans-serif family. On the desktop, text sizes are fixed by role, not by the box they
  sit in: small 12 (13 with touch), body 14 (16), buttons 13 (15), titles 18 (20), times the
  theme's font scale (`textSize` in `Theme.h`). A device's panel scales its text with the
  panel. Fit or truncate labels without moving targets.
- Buttons, dropdowns, text boxes and value boxes share the theme's corner radius and outline.
- Use one corner radius and visible borders/focus rings. Keep a text label for every action.
- Accent marks selection or an action; positive marks active playback; danger marks failures.
  State must also be conveyed by text, position, shape or accessibility state, not color alone.
- Distinguish browsing from applying. **Replace instrument/effect** explicitly changes the
  loaded engine; **Use sound** switches a prepared sound. Replacement ends the old slot's
  notes, tails and prepared sounds. It is not a seamless sound transition.
- Keep failed scans and load errors visible and copyable. Preserve a working graph when
  preparation or compatibility checks fail. Never silently choose a different plugin.

`hosts/standalone/Theme.h/.cpp` is the shared JUCE look-and-feel and semantic palette.
`Panel`, `Screen` and `PluginBrowser` use it. First-party and third-party generated
parameter pages inherit the same styling. A vendor's own native plugin GUI is outside
this theme contract; future declarative plugin pages will use these tokens, and optional
plugin branding must leave system navigation/transport consistent.

## Install and apply

Open **Plugins…** on the panel (or the **Plugins** menu / KICK **BROWSE** button).
The browser's **System theme** selector applies a theme throughout Oscairn.
**Install theme…** asks for a theme folder's `theme.json`, validates the theme and its
fonts, copies the files it names into the user's theme directory, then applies it. The
**Appearance** menu also lists installed themes. Built-in **Oscairn Dark**, **Oscairn
Light** and **Oscairn Classic** are ordinary theme folders shipped with the app: Dark and
Light in the soft style below, Classic in the outlined style the app first had, and **Oscairn
Block** with joined, square sections. An installable example is
[Midnight](../hosts/standalone/theme-examples/midnight/theme.json).

Appearance is a per-user, per-device preference shared by all Oscairn layouts and UI
processes. Other running UI processes pick up a selection within approximately one second;
a reopened UI reads the same selection. The audio engine does not read theme files.
This changes Oscairn system-wide, not the surrounding macOS/Windows/Linux desktop.

Configuration locations:

- macOS: `~/Library/Application Support/Oscairn/`
- Linux: `$XDG_CONFIG_HOME/Oscairn/`, or `~/.config/Oscairn/`
- Windows: the user's application-data directory, under `Oscairn/`
- `OSCAIRN_CONFIG_HOME` overrides the full configuration directory (useful for device images
  and isolated tests).

Installed themes are folders `themes/<id>/`; `appearance.txt` contains the selected ID.
Installation stages a copy and renames it into place; the selection is written through a
temporary file. A different theme cannot overwrite an installed theme ID; give a modified
theme a new ID. If the selected theme is missing or unusable, Oscairn shows an error and
uses the default built-in theme.

### Built-in themes and instrument builders

Built-in themes live beside the app: `Oscairn.app/Contents/MacOS/themes/` on macOS and
`share/oscairn/themes/` under the install prefix on Linux (`/usr/share/oscairn/themes/`
on Oscairn OS). Each is a theme folder in the same format as installed themes, so the
bundled font is loaded exactly like a theme author's. An instrument builder can replace
or add folders there; the file `default` names the theme a new user starts with. Built-in
IDs are reserved: user themes cannot replace them. If no built-in theme is usable, an
emergency theme with fixed colors and the platform's default font keeps text visible. Project exports do not
change a musician's appearance preference. Theme package distribution/sync will use the
future package system; it is not needed for local theme installation.

### The splash screen

While Oscairn starts and connects to its engine it shows a splash screen: the mark, the name
"oscairn" in the theme's bold font and a line of status ("Starting the engine…", or why it
could not start). It stays exactly as long as start-up takes, never longer. On a device's
panel it fills the screen; on the desktop it is a small window in the middle of the screen.
A theme's `splash` fields change it. A manufacturer can therefore brand start-up without
code: a device profile or product image ships a built-in theme carrying its own splash image
and colors, and names it in `default`.

## Icons

Icons are [Lucide](https://lucide.dev)'s (ISC; see [CREDITS](../CREDITS.md#interface-icons)),
kept as SVG files in `hosts/standalone/icons/` and built into the app. `drawIcon` in `Icons.h`
draws one in a color within a box: each file is read once, drawn in black and recolored to the
theme's ink, so an icon follows the theme like text does. `IconButton` is an icon on a button,
with its name as the tooltip; a toggle that is on is tinted with the accent. Only plain paths,
lines, circles and rectangles are used, which JUCE's SVG reader draws faithfully. To add one,
copy the file unchanged from the same Lucide release, list it in the `oscairn_standalone_icons`
target and record its hash in CREDITS. Themes do not carry icons (fonts, data and a splash
image only).

Oscairn's own mark ([branding](../branding/README.md)) is built in the same way, from the
SVGs `branding/logo.mjs` draws: `drawBrandMark` draws it in its colors for a dark or light
ground, where the brand shows (the splash screen), and `drawMonoMark` in one ink, where it
follows the theme like an icon (Home's header). `markName` in `Icons.h` names the mark shown;
the alternate is embedded too, so switching to it is that name alone. The app bundle's icon is
`branding/app-icon-1024.png`.

## Theme format, version 7

A theme is a folder named after its ID, holding `theme.json` ([schema](theme.schema.json))
and the files it names. See the complete Midnight example. Themes contain data, fonts and an
optional splash image only: no scripts, shaders, remote URLs or executable code. Every theme carries its own
fonts, so text never depends on what a device has installed.

| Field | Contract |
| --- | --- |
| `version` | Integer `7` |
| `id` | 1–64 lowercase letters, digits or hyphens; the folder's name; built-in IDs are reserved |
| `name` | Display name, 1–80 characters |
| `font` | `regular` and optional `bold` font files (`.ttf`/`.otf`, ≤ 4 MiB each) and the fonts' `license` text (≤ 64 KiB), as relative paths inside the folder |
| `fontScale` | Optional number, 0.8–1.4; default 1 |
| `radius` | Optional number, 0–16; default 6 |
| `colors` | All nine semantic colors below, opaque `#RRGGBB` |
| `style` | How controls are drawn: one choice for each of the eight below |
| `wires` | The routing views' wire colors, opaque `#RRGGBB`: `audio` and `notes` for enabled wires, `disabled` (muted, or off in a scene), `draining` (taking no new notes while held ones ring out, in a seamless change) and `unconnected` (no longer fits its plugin's ports). Disabled and draining wires are drawn dashed |
| `splash` | Optional: the splash screen (below). `image`, an `.svg` or `.png` file inside the folder (≤ 1 MiB) shown in place of the mark; `background` and `ink`, opaque `#RRGGBB`, the colors behind it and of its name and status line, with at least 4.5:1 contrast. Each is optional; without them the splash shows the built-in mark on the theme's `background` in its `text` color |

| Style | Choices | Draws |
| --- | --- | --- |
| `buttons` | `rounded`, `pill` | Buttons with the theme's radius, or fully round ends (single-line fields round the same way; taller ones use the radius plus 3) |
| `edges` | `none`, `hairline`, `outlined` | Outlines on buttons, fields and cards: none, faint, or the full border color. Keyboard focus is always drawn |
| `depth` | `flat`, `soft` | A small shadow under buttons, slider thumbs and the selected tab |
| `sliders` | `filled`, `line`, `cap` | An accent thumb on a thick track, a thin track with a light round thumb, or with a square cap |
| `tabs` | `folder`, `segmented` | Tabs joined to the view below, or a track with the selected tab lifted out |
| `marks` | `square`, `round` | Mute and solo as squares (rounded up to the theme's radius) or circles |
| `sections` | `separate`, `joined` | Cards, tiles and mixer strips with gaps between them, or edge to edge with one line between each and square corners, which uses small screens best |
| `rows` | `left`, `right`, `dot`, `fill` | Where an item's color goes in a list row (the Library, setlists): a short bar at the left or right end, a dot at the left, or the whole row tinted. A tinted row is tinted only as far as primary text keeps 4.5:1 contrast on it, alternate rows less, with a line between rows so neighbors of one color stay apart |

Choices, not code: the host draws every combination, so styles can be mixed (a soft style with
round marks). Which control is used (a pan knob or a slider) is the surface's, not the theme's.

| Colour | Meaning |
| --- | --- |
| `background` | Window/panel background |
| `surface` | Lists, editors, display surfaces |
| `raised` | Buttons, selected rows and pads |
| `border` | Control boundaries and unlit indicators |
| `text` | Primary text |
| `muted` | Secondary labels |
| `accent` | Selection, editable values and keyboard focus |
| `positive` | Playing/active state |
| `danger` | Errors and unavailable resources |

`theme.json` is limited to 16 KiB. Unknown fields, malformed colors, unsafe IDs,
out-of-range metrics, paths leaving the folder, linked files, missing licenses, files
that are not TrueType/OpenType fonts and splash images JUCE cannot read are rejected before
installation. Only the files a theme names are copied. Fonts and splash images are parsed in
the UI process by the platform's font engine and JUCE's image readers, which are not hardened
against malicious files: install themes only from trusted sources.
Earlier theme versions are no longer read; rename `colours` to `colors` (American spelling
throughout), add `wires` if missing, add `style.rows` (`left` draws rows as version 5 did), and
set `version` to 7 (initial development; version 7 added the optional `splash`). Primary text must have at least 4.5:1 contrast
against background, surface and raised colors. Authors should also check secondary text,
small labels, keyboard focus and selected controls in every supported layout.
Themes deliberately exclude knob assets and other images besides the splash, and custom layout code; richer plugin
pages and assets need separate bounded formats rather than executable theme hooks.

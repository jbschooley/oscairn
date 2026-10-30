# Oscairn interface and themes

Oscairn uses one visual language for system navigation, transport, plugin browsing,
prepared sounds, generated parameter editors, dialogs and device panel controls.
A device **layout** describes geometry and bindings. A **theme** describes appearance.
Changing either must not change the audio graph, parameters, MIDI assignments or transport.

## Shared design rules

- Use the semantic roles below, never private colour constants in a system component.
- Use an 8-pixel spacing unit, 16-pixel outer margins and 32–36-pixel desktop controls.
  Hardware layouts retain their measured geometry and scale as a unit.
- Use a sans-serif family, normal labels around 14–16 pixels and bold section titles.
  Theme font scaling applies to host controls; fit or truncate labels without moving targets.
- Use one corner radius and visible borders/focus rings. Keep a text label for every action.
- Accent marks selection or an action; positive marks active playback; danger marks failures.
  State must also be conveyed by text, position, shape or accessibility state, not colour alone.
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
**Appearance** menu also lists installed themes. Built-in **Oscairn Dark** and **Oscairn
Light** are ordinary theme folders shipped with the app. An installable example is
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
emergency theme with fixed colours and the platform's default font keeps text visible. Project exports do not
change a musician's appearance preference. Theme package distribution/sync will use the
future package system; it is not needed for local theme installation.

## Theme format, version 2

A theme is a folder named after its ID, holding `theme.json` ([schema](theme.schema.json))
and the files it names. See the complete Midnight example. Themes contain data and fonts
only: no scripts, shaders, remote URLs or executable code. Every theme carries its own
fonts, so text never depends on what a device has installed.

| Field | Contract |
| --- | --- |
| `version` | Integer `2` |
| `id` | 1–64 lowercase letters, digits or hyphens; the folder's name; built-in IDs are reserved |
| `name` | Display name, 1–80 characters |
| `font` | `regular` and optional `bold` font files (`.ttf`/`.otf`, ≤ 4 MiB each) and the fonts' `license` text (≤ 64 KiB), as relative paths inside the folder |
| `fontScale` | Optional number, 0.8–1.4; default 1 |
| `radius` | Optional number, 0–16; default 6 |
| `colours` | All nine semantic colours below, opaque `#RRGGBB` |

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

`theme.json` is limited to 16 KiB. Unknown fields, malformed colours, unsafe IDs,
out-of-range metrics, paths leaving the folder, linked files, missing licences and files
that are not TrueType/OpenType fonts are rejected before installation. Only the files a
theme names are copied. Fonts are parsed in the UI process by the platform's font engine,
which is not hardened against malicious files: install themes only from trusted sources.
Version 1 single-file themes are no longer read; recreate them as folders (initial
development). Primary text must have at least 4.5:1 contrast
against background, surface and raised colours. Authors should also check secondary text,
small labels, keyboard focus and selected controls in every supported layout.
Version 1 deliberately excludes image/knob assets and custom layout code; richer plugin
pages and assets need separate bounded formats rather than executable theme hooks.

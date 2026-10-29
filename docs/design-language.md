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
**Install theme…** validates and copies a JSON theme into the user's theme directory,
then applies it. The **Appearance** menu also lists installed themes.
Built-in **Oscairn Dark** and **Oscairn Light** are always available.
An installable example is [Midnight](../hosts/standalone/themes/midnight.json).

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

Installed themes are `themes/<id>.json`; `appearance.txt` contains the selected ID.
Writes use a temporary file followed by replacement. A different file cannot overwrite an
installed theme ID; give a modified theme a new ID. Removing or corrupting the selected
file falls back to the built-in dark theme on the next UI launch. Project exports do not
change a musician's appearance preference. Theme package distribution/sync will use the
future package system; it is not needed for local theme installation.

## Theme format, version 1

See [the schema](theme.schema.json) and the complete Midnight example.
Themes contain data only: no scripts, shaders, remote URLs, executable code or external
assets. System font family names use the platform's font fallback. Fonts are not installed
by importing a theme.

| Field | Contract |
| --- | --- |
| `version` | Integer `1` |
| `id` | 1–64 lowercase letters, digits or hyphens; built-in IDs are reserved |
| `name` | Display name, 1–80 characters |
| `font` | Optional installed font-family name, 1–80 characters |
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

Files are limited to 16 KiB. Unknown fields, malformed colours, unsafe IDs and out-of-range
metrics are rejected before installation. Primary text must have at least 4.5:1 contrast
against background, surface and raised colours. Authors should also check secondary text,
small labels, keyboard focus and selected controls in every supported layout.
Version 1 deliberately excludes image/knob assets and custom layout code; richer plugin
pages and assets need separate bounded formats rather than executable theme hooks.

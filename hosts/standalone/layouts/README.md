# Panel layouts

A layout describes a device's front panel: its size and where each control sits. The
desktop app (`oscairn-standalone`) builds its window from one, so a layout can stand in for
a device that does not exist yet and be tried with the same engine behind it. Built in:

| File | Layout |
| --- | --- |
| `desktop.json` | The desktop app's own window (the Desktop surface): toolbar, parts list, and tabs for the part, Mixer, Routing and Controls; apps such as the Groovebox open from the Apps menu |
| `kick.json` | The KICK's panel, with the geometry of chillbeast's KickEM simulator board (1024 x 600 screen); P1 to P4 play the row selected on screen; most buttons do nothing yet |
| `kick-screen.json` | The KICK's screen alone, 1024 x 600: on the device, the hardware panel supplies the controls |
| `ignite.json` | The Ignite's panel (BE 0716-AAF), from its front-panel photo, with its 1024 x 600 screen; the 16 step encoders edit each step's note, CUTOFF and VOLUME the synth; most buttons do nothing yet |
| `keys49.json` | A concept 49-key keyboard: screen, knobs, pads and keys |
| `compact.json` | A small screen (320 x 240) with four encoders under it, the Compact surface's test layout ([RFC 0019](../../../docs/rfcs/0019-adaptive-ui.md#compact)): the Performance's plugins, their pages and their parameters as lists |

Others can be opened with View > Open Layout File... or `--layout path.json`.

## Format

```json
{
  "name": "Desktop",
  "size": [1280, 800],
  "resize": "stretch",
  "panels": [ {"rect": [870, 80, 370, 470], "label": "SOUND", "anchor": "right top"} ],
  "controls": [
    {"type": "screen", "rect": [40, 80, 800, 469], "anchor": "stretch stretch"},
    {"type": "knob", "rect": [890, 110, 105, 140], "label": "Brightness", "bind": "synth:1", "anchor": "right top"}
  ]
}
```

- `surface` ([RFC 0019](../../../docs/rfcs/0019-adaptive-ui.md)): `panel` (the default), a
  device's front panel drawn from `panels` and `controls`, or `desktop`, the desktop app's own
  window with menus and tabs, which uses neither and may leave `controls` out, or `compact`, a
  panel whose screen shows plugin editors as lists for a few encoders (below). Its controls
  follow the **View > Input** setting: Keyboard and mouse, Touch (larger targets) or Automatic.
- `size`: design units. The window starts with one unit per pixel when it fits. `rect` is
  `[x, y, width, height]`.
- `resize`: `scale` (the default: the window keeps the panel's proportions and the panel
  scales with it; right for a real device's panel), `stretch` (any window shape: the panel
  scales to fit the tighter dimension and the space left over in the other goes to
  elements by their `anchor`) or `fixed`.
- `anchor` (panels and controls, used by `stretch`): a horizontal and a vertical part,
  `"left|right|center|stretch top|bottom|center|stretch"`. `right bottom` keeps the
  element's distance from the right and bottom edges; `stretch stretch` grows it with the
  window. The default is `left top`.
- `panels`: background areas with a label.
- `controls`, by `type`:
  - `screen`: the display (at most one). It is drawn for a 1024 x 600 screen and scales; on a
    compact layout, for 320 x 240.
  - `knob`: `bind` is `node:param` (nodes `sequencer`, `synth`, `delay`; a CLAP parameter
    id), `tempo`, or `control:NAME`: a control of the Performance's control tree by name
    (RFC 0011), driving everything it is set up to, such as a Super Knob.
    `row:N` plays the Nth parameter of the row selected on screen (RFC 0019): tap a tile in a
    plugin's parameter pages and the knobs take that row, left to right, and the screen
    names them along its edge nearest the knobs. With no row selected the knob is idle.
    `encoder:N` (compact layouts only) is the Nth of the screen's encoders: turning it moves
    through a list or turns the parameter above it, and a click without a turn presses it
    (encoder 1 goes back; the last opens what is chosen or shows the next parameters).
    `"mode": "endless"` draws an endless encoder (a ring of LEDs showing the value) instead of a
    pot with end stops (`absolute`, the default).
  - `button`: `bind` is `play`, `stop`, `play-stop`, `sync` (follow MIDI clock), `plugins`
    (the plugin browser), or the shell's `home`, `back` and `panic` (all notes off)
    ([RFC 0030](../../../docs/rfcs/0030-system-shell-launcher-and-navigation.md)). A layout with
    one of those buttons shows no icon for it in the top bar.
  - `pads`: `rows` x `cols` pads, pad 1 at the bottom-left, row by row upwards; each plays
    what the Performance's pad map assigns it (**Pads...** on the screen edits it). With
    `"steps": true` they light with the sequencer's playhead.
  - `keys`: a piano keyboard of `count` keys from `note`.
  - `label`: text; `"align"` is `left` (the default), `center` or `right`.

  A control without `bind` is drawn but does nothing yet. An unknown type or binding stops
  the layout loading, with the reason.

Colours, fonts and control styling come from the system [theme](../../../docs/design-language.md).
Layouts define geometry and bindings; they do not override the selected theme.

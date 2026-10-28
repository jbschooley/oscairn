# Panel layouts

A layout describes a device's front panel: its size and where each control sits. The
desktop app (`oscairn-standalone`) builds its window from one, so a layout can stand in for
a device that does not exist yet and be tried with the same engine behind it. Built in:

| File | Layout |
| --- | --- |
| `desktop.json` | Desktop window that stretches to any shape: large screen, sound knobs, 16 pads, transport |
| `kick.json` | The KICK's panel, with the geometry of chillbeast's KickEM simulator board (1024 x 600 screen); most buttons do nothing yet |
| `kick-screen.json` | The KICK's screen alone, 1024 x 600: on the device, the hardware panel supplies the controls |
| `keys49.json` | A concept 49-key keyboard: screen, knobs, pads and keys |

Others can be opened with View > Open Layout File... or `--layout path.json`.

## Format

```json
{
  "name": "Desktop",
  "size": [1280, 800],
  "background": "#1b1d21",
  "resize": "stretch",
  "panels": [ {"rect": [870, 80, 370, 470], "label": "SOUND", "anchor": "right top"} ],
  "controls": [
    {"type": "screen", "rect": [40, 80, 800, 469], "anchor": "stretch stretch"},
    {"type": "knob", "rect": [890, 110, 105, 140], "label": "Brightness", "bind": "synth:1", "anchor": "right top"}
  ]
}
```

- `size`: design units. The window starts with one unit per pixel when it fits. `rect` is
  `[x, y, width, height]`.
- `resize`: `scale` (the default: the window keeps the panel's proportions and the panel
  scales with it; right for a real device's panel), `stretch` (any window shape: the panel
  scales to fit the tighter dimension and the space left over in the other goes to
  elements by their `anchor`) or `fixed`.
- `anchor` (panels and controls, used by `stretch`): a horizontal and a vertical part,
  `"left|right|centre|stretch top|bottom|centre|stretch"`. `right bottom` keeps the
  element's distance from the right and bottom edges; `stretch stretch` grows it with the
  window. The default is `left top`.
- `panels`: background areas with a label.
- `controls`, by `type`:
  - `screen`: the display (at most one). It is drawn for a 1024 x 600 screen and scales.
  - `knob`: `bind` is `node:param` (nodes `sequencer`, `synth`, `delay`; a CLAP parameter
    id) or `tempo`.
  - `button`: `bind` is `play`, `stop`, `play-stop` or `sync` (follow MIDI clock).
  - `pads`: `rows` x `cols` pads playing notes from `note` upwards, left to right from the
    top; with `"steps": true` they light with the sequencer's playhead.
  - `keys`: a piano keyboard of `count` keys from `note`.
  - `label`: text.

  A control without `bind` is drawn but does nothing yet. An unknown type or binding stops
  the layout loading, with the reason.

# Setlist format, version 1

A setlist is the Setlist app's file, not the engine's: one JSON file per setlist in the data
folder's `Setlists/` (`OSCAIRN_DATA_HOME`, else Application Support on macOS, else
`XDG_DATA_HOME/oscairn` or `~/.local/share/oscairn`), named after the setlist.

```json
{"format": "oscairn.setlist", "version": 1, "name": "Friday",
 "display": {"mode": "grid", "columns": 4, "rows": 3},
 "entries": [{"id": "3f9c0a1b2d4e5f60", "name": "Bass Split", "color": "#e5484d"}]}
```

| Field | Meaning |
| --- | --- |
| `format`, `version` | Exactly `oscairn.setlist` and integer `1`; any other is refused |
| `name` | The setlist's name |
| `display` | How the Setlist app shows it: `mode` is `list` (a list with Previous and Next), `scroll` (a list to scroll and tap) or `grid` (`columns` × `rows` cells, each 1 to 8, with Previous and Next; the page shown is the one holding the entry playing) |
| `entries` | Up to 1000, in playing order: a Performance's library ID ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)), its name when last seen (shown when it is no longer in the library) and `color`: `#rrggbb` to show it in this setlist instead of the Performance's color, or empty |

The file is written whole (a temporary file, then a rename). Multi-device setlists
([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md)) will replace this format.

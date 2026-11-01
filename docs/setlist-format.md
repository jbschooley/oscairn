# Setlist format, version 1

A setlist is the Setlist app's file, not the engine's: one JSON file per setlist in the data
folder's `Setlists/` (`OSCAIRN_DATA_HOME`, else Application Support on macOS, else
`XDG_DATA_HOME/oscairn` or `~/.local/share/oscairn`), named after the setlist.

```json
{"format": "oscairn.setlist", "version": 1, "name": "Friday",
 "entries": [{"id": "3f9c0a1b2d4e5f60", "name": "Bass Split"}]}
```

| Field | Meaning |
| --- | --- |
| `format`, `version` | Exactly `oscairn.setlist` and integer `1`; any other is refused |
| `name` | The setlist's name |
| `entries` | Up to 1000, in playing order: a Performance's library ID ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)) and its name when last seen, shown when it is no longer in the library |

The file is written whole (a temporary file, then a rename). Multi-device setlists
([RFC 0012](rfcs/0012-sync-external-midi-and-multi-device.md)) will replace this format.

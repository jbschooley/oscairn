# Library archive format, version 1

A library, or an exported Performance, is one archive file
([RFC 0021](rfcs/0021-sounds-performances-and-library.md#libraries)): a zip holding

| Entry | What it is |
| --- | --- |
| `library.json` | The manifest (below) |
| `Performances/<folder>/` | Each Performance as the library keeps it: `performance.json` ([format](performance-format.md)) and `nodes/` |
| `Sounds/<folder>/` | Each Sound the same way, `sound.json` and `nodes/` |
| `Assets/<first 16 hex digits of the SHA-256>/<file>` | Each sample the items' plugins use, once |

The manifest:

| Field | Meaning |
| --- | --- |
| `format`, `version` | Exactly `oscairn.library` and integer `1` |
| `id` | The library's ID: its folder's name when installed as a library, and what imported copies record as their `origin` |
| `name` | Its name |
| `kind` | `library`, or `performance` for an exported Performance (its ID and name are the Performance's) |
| `items` | Each item's `kind` (`performance` or `sound`), `id`, `name` and `path`, its folder in the archive |

An export holds the Performance, the library Sound of each part that is still that Sound (the
part unedited since it was loaded or saved, and the library holding that revision), and with
**edited Sounds**, the Sound of each part edited since, as the library has it. Every sample their
plugins list goes in once, by its hash.

## Reading one

An archive is untrusted. Reading extracts it into a new folder of its own, refusing, and leaving
nothing behind:

- an entry outside `library.json`, `Performances/`, `Sounds/` and `Assets/`;
- a path that is absolute, climbs out (`..`), names a drive (`:`), uses a backslash, is hidden
  (a component starting with `.`), has a control character, or is longer than 1024 bytes;
- a link, or an entry twice;
- more than 65536 entries, more than 8 GiB in all, or an entry over 2 GiB, as declared; and a
  file that extracts to another size than it declares;
- a manifest that is not version 1, or names an item whose folder or document is not there, or
  whose document's ID is not the one listed.

## Installing one

- **As a library:** locked, in `Libraries/<id>/` in the data folder with its manifest. Items
  keep their IDs. Installing again copies only the items that are missing. Uninstalling removes
  the folder; deleting an item removes its folder, and installing again brings it back.
- **Into the user's own storage:** each item is copied into `Performances/` or `Sounds/` under a
  new ID and a new revision, its info's `origin` recording the archive's ID, the item's ID there
  and the copy's content hash. A Performance's parts that came from a Sound in the archive point
  at that Sound's copy. Installing again skips an item whose copy is still there with that hash;
  one edited or deleted since is copied again.
- **All of it or chosen items.**
- **Samples** go into the user's sample library (RFC 0009) by their hash either way, so they stay
  when a library is uninstalled. A sample an item needs that is in neither the archive nor the
  device is reported.

Two items with one ID are listed once, the user's own first: so a Performance exported and
installed as a library on the device it came from shows the original, not the locked copy.
Installed into the user's own storage (the default for an export) it is a new copy.

Not yet: a Sound from a locked library exported as that library's item, and plugins in archives
(RFC 0006).

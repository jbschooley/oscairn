# Library archive format, version 3

A library, or an exported Performance, is one archive file
([RFC 0021](rfcs/0021-sounds-performances-and-library.md#libraries)): a zip holding

| Entry | What it is |
| --- | --- |
| `library.json` | The manifest (below) |
| `library.sig` | Optional: the whole archive's signature, `{key, signature}` over `oscairn-archive-1\0` and the manifest's exact bytes ([signatures](#signatures)) |
| `Performances/<folder>/` | Each Performance as the library keeps it: `performance.json` ([format](performance-format.md)) and `nodes/` |
| `Sounds/<folder>/` | Each Sound the same way, `sound.json` and `nodes/` |
| `Assets/<first 16 hex digits of the SHA-256>/<file>` | Each sample the items' plugins use, once |
| `Plugins/<format>/<id>@<version>/<platform>/<bundle>` | Each plugin build the items use ([RFC 0006](rfcs/0006-packages-and-portability.md)), a file or a bundle folder as installed; characters other than letters, digits, `.`, `-`, `@` and `_` in the ID and version are `_` |

The manifest:

| Field | Meaning |
| --- | --- |
| `format`, `version` | Exactly `oscairn.library` and integer `3` |
| `id` | The library's ID: its folder's name when installed as a library, and what imported copies record as their `origin`; 1 to 64 letters, digits, `.`, `-` and `_`, the first one a letter or digit and the last not a `.` |
| `name` | Its name |
| `kind` | `library`, `performance` for an exported Performance or `sound` for an exported Sound (its ID and name are that item's) |
| `items` | Each item's `kind` (`performance` or `sound`), `id` (as plain as the library's: an exported item's ID is its archive's), `name`, `path`, its folder in the archive, and `signatures` (optional) |
| `plugins` | Each plugin's `format`, `id`, `version`, `name` and `builds`: each build's `platform` (such as `macos-arm64` or `linux-arm64`), `path` in the archive, `sha256`, its digest as a [Performance's lock](performance-format.md) gives it, and `signatures` (optional) |
| `files` | Every other entry's SHA-256, by its path |

An export holds the Performance, the library Sound of each part that is still that Sound (the
part unedited since it was loaded or saved, and the library holding that revision), and with
**edited Sounds**, the Sound of each part edited since, as the library has it. Every sample their
plugins list goes in once, by its hash. Each plugin they use goes in with every build there is:
this device's from its `pathHint` while that build still has the document's locked digest, else
from the plugin store, and the store's builds for other platforms (installed from archives, or
added with `plugins.addBuild`). Only CLAP plugins go in: LV2 plugins are installed on each device (many need libraries
outside their bundles), so an LV2 plugin, like one not on this device, is left out, and
the export says which.

## Reading one

An archive is untrusted. Reading extracts it into a new folder of its own, refusing, and leaving
nothing behind:

- an entry outside `library.json`, `Performances/`, `Sounds/`, `Assets/` and `Plugins/`;
- a path that is absolute, climbs out (`..`), names a drive (`:`), uses a backslash, is hidden
  (a component starting with `.`), has a control character, or is longer than 1024 bytes;
- a link, or an entry twice;
- more than 1,048,576 entries or 64 MiB of their names, more than 8 GiB in all, an entry over 2 GiB or a manifest over
  64 MiB, as declared; and a
  file that extracts to another size than it declares;
- a manifest that is not version 3 (there is no reader for version 2), whose ID or an item's
  is not plain (as `id` above says), or names an item whose folder or document is not there, or
  whose document's ID is not the one listed, or a plugin build not under `Plugins/`, not there,
  or for a platform other than `linux-arm64`, `linux-x86_64`, `macos-arm64`, `macos-x86_64` and
  `windows-x86_64`, or whose path is not a `.clap` or `.lv2` bundle (the store finds builds by
  that extension);
- a file whose SHA-256 is not the one `files` lists, a file `files` does not list or one it
  lists that is not there, and a plugin build whose digest is not its own;
- a signature that does not verify, at any level, or a malformed one.

Reading only the manifest (an archive's contents, before installing) checks the archive's and
the builds' signatures; items' signatures are checked when the archive is extracted.

## Signatures

Every signature is optional, and what is signed is the publisher's choice
([RFC 0006](rfcs/0006-packages-and-portability.md#signatures)). Each is `{key, signature}`:
the signer's Ed25519 public key (64 hex digits) and the signature (128), over a message that
says what it signs:

| Where | Signs | Message |
| --- | --- | --- |
| `items[].signatures` | The item: a Sound or a Performance | `oscairn-item-1\0` and its content hash, the library's `hash` (its document without `info`, and its `nodes/` files) |
| `plugins[].builds[].signatures` | A plugin build, by its publisher | `oscairn-build-1\0` and the plugin's format, ID, version, the build's platform and digest, each followed by `\0` |
| `library.sig` | The whole archive | `oscairn-archive-1\0` and `library.json`'s exact bytes, which list every file's SHA-256 and every build's digest |

A key is shown by its ID, the first 16 hex digits of the SHA-256 of its 32 bytes. An export
signs its items, the whole archive, or both, only when the exporter asks, with a key of theirs
from the data folder's `Keys/`. A build's signatures travel with it: an export carries those in
`<bundle>.sig` beside the build it takes (a JSON list of `{key, signature}`, as
`oscairn-sign build` writes it), whether at the plugin's own path or in the store, leaving out
and noting any that is not over that build; an install, and `plugins.addBuild`, keep them in the
store the same way, beside the build. A signature that does not verify is refused like a wrong
hash. Who signed is reported; whether that is
trusted is the installing device's [trust store](rfcs/0006-packages-and-portability.md#signatures).

## As a folder

Planned ([RFC 0031](rfcs/0031-package-repository-and-distribution.md#installing-in-little-space)),
not built. A package can also be an unpacked folder: the same tree as inside the zip, with
`library.json` and every file it lists by hash. It is a second form of this format, read by the
same rules, except that hidden files a desktop adds are skipped. Each file is hashed as it is
copied in. Archives on a USB drive or a share are to be read in place and installed entry by
entry, so an install needs room for the installed files plus one entry, not for the archive and
its contents at once.

## Installing one

- **As a library:** locked, in `Libraries/<id>/` in the data folder with its manifest. Items
  keep their IDs. Installing again copies only the items that are missing. Uninstalling removes
  the folder; deleting an item removes its folder, and installing again brings it back. An ID
  that differs from an installed library's only in letter case names the same folder on a
  case-insensitive file system (macOS's and Windows's by default), so there it is refused: two
  libraries never share a folder.
- **Into the user's own storage:** each item is copied into `Performances/` or `Sounds/` under a
  new ID and a new revision, its info's `origin` recording the archive's ID, the item's ID there
  and the copy's content hash. A Performance's parts that came from a Sound in the archive point
  at that Sound's copy. Installing again skips an item whose copy is still there with that hash;
  one edited or deleted since is copied again.
- **All of it or chosen items.**
- **Samples** go into the user's sample library (RFC 0009) by their hash either way, so they stay
  when a library is uninstalled. A sample an item needs that is in neither the archive nor the
  device is reported.
- **Plugins:** every build of each plugin the installed items use goes into the plugin store,
  each whole or not at all, so this device can pass the others on in its own exports (a desktop
  carrying a device's builds). A Performance whose plugin is not at its `pathHint` finds this
  device's build there. A build already there with the same digest is left; a different one is
  kept and reported; a plugin with no build for this device is reported.
- **Signatures** ([RFC 0006](rfcs/0006-packages-and-portability.md#signatures)): before anything
  is installed, each plugin build that would go into the store is judged against the device's
  trust store. One unsigned or signed by an unknown key refuses the install, listing them, until
  the user says yes (`acceptUnsignedCode`); one signed by a revoked key is never installed, and
  is noted. Sounds, Performances and samples install whether signed or not.

## The plugin store

`Plugins/<format>/<id>@<version>/` in the data folder (characters other than letters, digits,
`.`, `-`, `@` and `_` made `_`) holds `plugin.json`, `{format, id, version, name}`, as the folder
name cannot say them exactly, and a folder per platform with its build: `macos-arm64/Odin2.clap`.
`plugins.list` shows each plugin with the library items that use it; removing one is refused
while the open Performance plays it, and what used it opens with a stand-in until it is back.

Two items with one ID are listed once, the user's own first: so a Performance exported and
installed as a library on the device it came from shows the original, not the locked copy.
Installed into the user's own storage (the default for an export) it is a new copy.

Not yet: a Sound from a locked library exported as that library's item, and grouped cleanup of plugins nothing uses (RFC 0006).

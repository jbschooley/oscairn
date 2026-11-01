# RFC 0023: Item types

Status: proposed, 2026-11-01. Builds on the library
([RFC 0021](0021-sounds-performances-and-library.md)), sound assets
([RFC 0009](0009-sound-assets-and-dependencies.md)), packages
([RFC 0006](0006-packages-and-portability.md)) and apps that open each other
([RFC 0022](0022-apps-and-requests.md)).

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. Built-in types: today's Performances, Sounds and samples described as types, with no change in behavior
- [ ] 2. Queries and requests by type: `library.query` by `type`, `oscairn.pick.<type>` actions, archives by type
- [ ] 3. Installed types: declared by packages, for documents with an info block and for folders indexed in place
- [ ] 4. Extractors: sandboxed metadata readers for files with no info block (audio, plugin presets)
- [ ] 5. A database index, for libraries of tens of thousands of items
- [ ] 6. The DJ types: tracks, crates and track analysis

## Problem

The library knows two kinds of item, Performances and Sounds, by name in its code. Samples
live in another store with its own index (RFC 0009), and the requests apps make of each other
name kinds by hand (`oscairn.pick.performance`, `oscairn.pick.sound`). Every new kind would
repeat all of it: where its files go, how they are found, searched, tagged, favorited,
packed into archives, installed and picked.

New kinds are coming, and not only Oscairn's own:

- wavetables and single-cycle waveforms for the engines;
- patterns and songs for sequencers, patches for the Modular instrument;
- setlists, themes and panel layouts;
- for a DJ deck: tracks, crates and playlists, and each track's analysis (BPM, key,
  beatgrid, cue points).

Musicians should be able to install a different picker, and an app should be able to show a
picker of its own inside its window, without either knowing how the items are stored.

## Outcome

An **item type** says what a kind of item is, so the library can find, index, search, pack
and install items of any type the same way, and apps can ask for items by type. A type is
declared, not programmed: a manifest. Code enters only where a file has to be read to know
what it holds, and then out of process.

A type has three parts, kept apart:

1. **The manifest**, declarative: what the type is called, how its items are recognized,
   how they are stored and identified, which fields describe them, and where they go in an
   archive.
2. **The indexer**, which reads an item into the index: built in for documents with an info
   block; an extractor (below) for anything else.
3. **Handlers**, the apps that pick, open or edit items of the type, offering RFC 0022
   actions for it. The type never says what opening an item does: loading a Performance is
   the engine's business, playing a track the DJ deck's.

## The manifest

```json
{
  "format": "oscairn.type",
  "version": 1,
  "id": "oscairn.sound",
  "name": "Sound",
  "plural": "Sounds",
  "recognize": { "folder": ".osound", "document": "sound.json", "format": "oscairn.sound" },
  "storage": "managed",
  "folder": "Sounds",
  "identity": "id",
  "fields": [
    { "id": "engines", "name": "Engine", "facet": true }
  ]
}
```

- **`id`**: reverse-DNS, unique; a package may declare only IDs under its own name, and
  `oscairn.` is Oscairn's.
- **`recognize`**: how an item is known: a folder with this suffix holding a document of
  this `format`, or a file with one of these `extensions` (and, optionally, magic bytes).
- **`storage`**, one of three models (below).
- **`folder`**: under the data folder, for managed types; the type's folder in archives.
- **`identity`**: what makes two items the same (below).
- **`fields`**: what the index keeps beyond name, tags, color and description, and which of
  them are search filters (`facet`) or sort keys.

Types depend on nothing but their manifest. A type whose package is uninstalled stays known
for the items already on the device: they are listed, read-only, until removed.

## Three ways of storing items

| Model | Who arranges the files | Identity | Today | Examples |
| --- | --- | --- | --- | --- |
| **Managed** | Oscairn: one folder per item under the type's folder, with IDs and revisions | an ID in the item | Performances, Sounds | patterns, patches, setlists, crates |
| **By content** | Oscairn: stored once by hash, shared by everything that uses them | the SHA-256 of the content | samples (RFC 0009) | wavetables, waveforms |
| **In place** | The user: their own folders, which Oscairn indexes and never moves or changes | the content, or what the type names | sample folders used in place (RFC 0009) | DJ tracks |

**In place** answers the DJ question: a DJ keeps their music as they arranged it, in their
folders or on a USB stick, and the folder tree is one way of browsing among search, tags and
crates. The index holds what Oscairn learned about a track (its analysis, cue points, tags,
favorites), keyed by its identity, so nothing is written into the user's files unless they
ask, and the analysis follows the track when it is moved or copied to another device.

**Identity** depends on the type:

- `id`: an ID in the item's info block (managed documents).
- `content`: the SHA-256 of the whole file (samples).
- `payload`: the SHA-256 of what a file holds without its tags, for files whose tags people
  edit (audio tracks: the audio frames, not the ID3 or Vorbis comments), read by the
  extractor.
- `path`: the path alone, for types where nothing better exists; it does not survive a move.

## Indexers and extractors

Documents with an info block (RFC 0021) are indexed by the library itself, as today: name,
tags, color, description and the type's fields from the document. Files without one (audio,
a plugin's presets, a third party's patch format) need code to read: an **extractor**,
declared by the type's package. It reads untrusted files, so it runs as a separate process,
like plugin scanning: one file at a time, with a timeout, a memory limit, captured stderr, and
replacement when it dies. It answers with the fields and the identity; it never writes.
Analysis that takes long (a track's beatgrid) runs in the background and fills the index in
as it goes.

## Queries, requests and pickers

- `library.query` takes `type` (one or more) where it takes `kind` today, and returns each
  type's facets.
- RFC 0022's requests are named by type: `oscairn.pick.<type id>`, `oscairn.send.<type id>`;
  the Library answers picks for every type it indexes. A **replacement picker** is an app
  answering `oscairn.pick.<type>` in its own window.
- **A picker inside an app's window** is a component from the SDK, over the query API: the
  standard list, search and filters, which the app embeds and styles with the theme. Apps
  are separate processes, so one app's picker is not drawn inside another's window.
- Archives ([format](../library-archive.md)) hold one folder per type, named by the type's
  `folder`; installing one installs items of every type the device knows, and keeps those of
  types it does not know until the type is installed.

## Scale

The index today is one JSON file. A DJ library can hold 50,000 tracks across several
folders and drives. Step 5 moves the index into a database (SQLite, in the data folder, still
a cache: deleting it loses nothing that is not also in the items' files or the user file),
scans incrementally and watches folders indexed in place. Analysis and user data for items in
place (cue points, tags, favorites) are not a cache: they live in the user file, keyed by
identity.

## Trust

- A type's `folder` is a plain name inside the data folder; a manifest cannot name paths.
- Folders indexed in place are chosen by the user, one by one, and listed where they can
  remove them; the scan reads only those.
- Extractors are untrusted code reading untrusted files: separate processes, with limits,
  as above. A type installed without one indexes its items by name and path only.
- Two packages declaring one type ID: the first installed keeps it; the second is refused,
  and says so.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Built-in types | Performance, Sound and sample as manifests in the app; the library and the sample store read them instead of naming kinds | Every library, archive and asset test passes unchanged |
| 2. By type | `library.query` by `type`; `oscairn.pick.<type>`; archives by type folder | The Library answers a pick for a type added in a test, with no change to its code |
| 3. Installed types | Manifests from packages; managed and in-place types with info blocks | A package declaring a pattern type installs, its items appear, are picked and packed, and uninstalling it leaves them listed read-only |
| 4. Extractors | Out-of-process extractors with limits | A test extractor that hangs or crashes on one file is replaced and the scan goes on; its fields appear for the rest |
| 5. Database index | SQLite cache, incremental scans, watched folders | 50,000 items indexed in place: a rescan with nothing changed reads no file; a search answers within 50 ms |
| 6. DJ types | Track (in place, payload identity, extractor), crate (managed), analysis in the user file | A track moved to another folder keeps its cue points; retagging it does not change its identity |

## Open questions

- Whether Oscairn's own types (Performance, Sound) ship as manifests that users can see,
  or stay built in and only third-party types are manifests.
- How a type's fields are typed (numbers, enumerations, durations) for range filters and
  sorting.
- Whether themes and panel layouts become types, installable and pickable like the rest, or
  stay the settings they are.
- How an item of one type refers to items of another (a crate to its tracks, a Performance
  to its samples) so packing and deleting follow the references.

## Ruled out

- **Types that run code to open items.** Opening stays with the apps that handle the type;
  a type that could run code on open would be a plugin by another name.
- **Extractors in the library's process.** They read untrusted files with third-party code.
- **Writing analysis into users' files by default.** Tags and cue points written into a DJ's
  MP3s change files they did not ask to change, and can upset the other software they use
  them with; writing them out is an explicit export.

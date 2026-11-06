# RFC 0021: Sounds, Performances and the library

Status: in progress, 2026-10-31. Started with Performances: format version 3 with the info
block (id, name, description, tags; no provenance or control owners yet), and a library of the
Performances in the user's folder with the index as a cache, the user file (favorites and
recents), query, rescan and save and load by ID (`library.*`, `performance.info`/`setInfo`,
see [control-protocol.md](../control-protocol.md#library)). The desktop app has a Library
app (Performances and Sounds; search, tags, favorites, recent; a click opens a
Performance, New part adds a Sound), a Part menu (Load Sound into Part through the Library's
pick, Save Part as Sound) and a Setlist app
([setlist-format.md](../setlist-format.md)), and Performance > Save and Save As New. Step
1's Sound documents are built (`parts.saveSound`, `parts.loadSound`, `parts.fromSound`,
control owners, and provenance: `from`, revisions, Save and Save As New, Update from
library), with these differences from the text below: a Sound holds a `part` object rather
than one entry in `parts`; a crossing wire is removed and flagged on load rather than kept
unconnected; `modified` is set by edits as they happen rather than by comparing with the
library, so preset and state loads do not set it yet; there is no author or suggested input yet, so a new part from a
Sound listens to every key; and
`parts.loadSound` applies at once, returning what it removed, with no `confirm` round. Not
yet: editing tags, tag suggestions, packages, `LibraryModel` and the step 3 pane with its
inspector and drag to part. Builds on parts, splits, buses, wire ownership,
scenes and controls ([RFC 0011](0011-performance-ui-and-editors.md)), prepared sounds
([RFC 0005](0005-sound-contexts.md), [RFC 0007](0007-effect-contexts.md)), sound assets
([RFC 0009](0009-sound-assets-and-dependencies.md)), packages ([RFC 0006](0006-packages-and-portability.md))
and the three surfaces ([RFC 0019](0019-adaptive-ui.md)).

## Progress

Checked when built and tested; the sections below say how.

- [x] Performance format 3 with the info block (name, description, tags, color)
- [x] The library of Performances: cache, favorites, recents, query, Save and Save As New
- [x] 1. Sound documents: save a part, load into a part or a new part, control owners
- [x] 1. Provenance: each part records the Sound and revision it came from and whether it was
      edited since; revisions with parents for Sounds and Performances; Save and Save As New
      for Sounds; Update from library; a deleted Sound breaks nothing
- [x] Content hashes for Sounds and Performances, in the info block and the index
- [ ] Authors in the info block, and `modified` set by preset and state loads
- [x] Libraries as archive files: items locked, Save As New from them, deleting items,
      uninstalling, and reinstalling to restore what is missing ([design](#libraries))
- [ ] Factory content as a preloaded library (there is no factory content yet)
- [x] Importing a library as a library or into your own storage, all of it or chosen items;
      importing again skips copies still unchanged (by content hash)
- [x] The desktop's Install, Export, Delete and Uninstall (Performance menu; the Library app)
- [x] Hidden tags and tag colors (`library.tag`; right-click a tag in the Library app)
- [x] Editing an item's name, description, tags and color, copying an item into your own, and
      showing only your own or one library's (`library.edit`, `library.copy`, `source`)
- [x] The Library app's left column (the library: everything, your own, each installed library;
      then the tags), as rows or pills, compact rows, and every item's actions on one menu (a right
      click, a long press, or Options), exporting an item without opening it among them
      (`library.export`)
- [x] Exporting a Performance keeps its parts' references to their Sounds
- [x] Exporting with every asset, the Sounds of unedited parts, and optionally those of edited
      parts ([design](#exporting-a-performance))
- [ ] 2. Library index and tags: index, user file, Sounds and tag editing done; suggestions,
      packages and `LibraryModel` remain
- [ ] 3. Desktop pane: the Library app and Part menu done; the dockable pane, inspector,
      drag to part and keyboard remain
- [x] Importing a Performance, all of it or chosen parts (with what they need) and scenes ([design](#importing-a-performance))
- [ ] 4. Plugin presets
- [x] Engine support for seamless changes: draining note connections, retiring nodes, plugin
      sleep ([status](#seamless-switching)); loading a Sound cuts, by decision
- [x] Seamless Performance changes: the outgoing Performance rings out beside the new one ([status](#seamless-switching))
- [x] Pooled instruments restore a snapshot in the instances they have ([design](#free-instances-across-a-change-and-instances-kept-warm))
- [x] Free instances taken over across a change ([design](#free-instances-across-a-change-and-instances-kept-warm))
- [x] Instances kept warm once rung out, bounded and evicted under pressure ([design](#free-instances-across-a-change-and-instances-kept-warm))
- [x] The middle rule: a playing instance taken over only where keeping it warm would not fit ([design](#free-instances-across-a-change-and-instances-kept-warm))
- [x] A failed load puts back the state of the playing instances it took over ([design](#free-instances-across-a-change-and-instances-kept-warm))
- [x] Audition of a Sound in a part and of a Performance in place of the open one, put back as
      it was or kept, from the Library app ([status](#audition))
- [ ] 5. Seamless loading into a part (part transitions on RFC 0007's chain preparation)
- [ ] 6. Panel
- [ ] 7. Compact test layout

## Problem

We planned a Performance as a collection of Sounds. The architecture says so:

> **Patch** (engine state) → **Sound** (event chain + engine + insert effects) →
> **Performance** (Sounds, zones, routing, scenes) → **Setlist**.

and, for the groovebox, "**Project** → patterns and song, with Sounds as track instruments".

Since then the Performance has grown a richer shape. A part has a chain with splits and
lanes, sends to shared buses, and its own level, pan, mute and solo. Routing lets a musician
draw user wires anywhere, including between parts. Scenes store part faders and chosen
parameters. Controls form a tree whose targets are any parameter, part or control. So the
line between "a Sound" and "a piece of a Performance" is no longer obvious.

The word is also used in three different ways today:

- the architecture's Sound: event chain, engine and insert effects;
- RFC 0009's table: "A plugin identity plus its state plus the asset references that state
  uses";
- the control protocol's `sounds.*` methods: a prepared context inside one plugin
  (RFC 0005) or one instance in a prepared pool.

There is also no library. The architecture plans one:

> **Library (default):** browse Sounds, Performances, Setlists, Projects and Samples by
> tag and search. Saving just saves.

> **Files first, index second.** Everything a user owns is a plain file or folder in a
> documented format. The core keeps an index (stable IDs, revisions, dependencies, content
> hashes), rebuilt by scanning, so files added from a computer appear in the library.

What exists is narrower. `SoundPickerModel` finds the node named `synth`, offers five
hard-coded Basic Synth patches ("Warm", "Bright" …), the presets `presets.list` returns for
other instruments, or a `.wav`/`.sfz` import for the Sampler, and prepares, selects and
discards contexts on that one node. `PluginBrowser` scans plugins, replaces the instrument or
effect slot, and reloads it with a chosen preset through `graph.replace`, which cuts the
slot's notes and tails. The desktop's **Performance** menu saves and opens whole
Performance folders. Nothing has a name beyond its folder, a tag, an ID or a revision.

## Outcome

- **A Sound is a saved part.** It holds the part's chain, splits and lanes, every plugin's
  state and assets, the wires inside the part, and the part's own controls. It does not hold
  the part's place in a Performance: where it listens, where it plays, its fader, its sends.
- **A Performance keeps its own copy of every part.** It records which Sound and revision a
  part came from, but never follows the library. It opens without the library.
- **Plugin presets stay what they are:** the state of one plugin. The picker shows them, and
  loading one into a part makes a one-plugin Sound.
- **Tags instead of a category tree.** Any number of free tags, all alike: there are no
  factory tags and user tags, only tags. Facets the host works out itself (engine, kind,
  seamless) sit beside them, never stored as tags. Filters combine with AND.
- **Your own Sounds, and libraries.** Sounds and Performances you save are yours to edit,
  save over and delete. A **library** (a sound pack, or a device's factory content) is
  imported as a whole and locked: its items load and play like any other, and **Save As**
  makes your own copy, but they are never saved over, and neither are their samples.
  Factory content is a library preloaded on the device; devices may come with different ones.
- **One library view model, drawn three ways:** a pane beside the parts on the Desktop, a
  full-screen tab with the knobs on the Panel, and a short list on Compact.
- **Loading a Sound into a part is seamless when it can be,** and says so before it
  happens when it cannot.

## What a Sound is

Three candidates. Each is judged by what happens when it is dropped into a Performance.

### (a) A preset for one plugin

The simplest. It is exactly what CLAP preset discovery and `preset-load` already model,
and what LV2 presets are.

- **Dropped into a part:** loads into one plugin. Fine for "a Rhodes on the FM engine".
- **Wires out of the part:** none involved. The plugin's wires stay.
- **Scenes and controls:** unaffected, because the node stays. Its parameters change under
  them, which can make a scene's stored values meaningless for the new patch.
- **What it cannot hold:** "FM synth → compressor", the architecture's own first example.
  Nor a layered piano and pad on one key, a dry/wet lane, or an arpeggiator in front of the
  synth. The musician would save four presets and rebuild the chain by hand every time.

That is a Patch in the architecture's chain, not a Sound. Keep it, under its own name.

### (b) A saved part

A part is already the unit the musician builds, mutes, solos and moves: an input, a chain,
an output. Saving it is the natural "save this sound".

- **Dropped into a part:** the target part's chain is replaced by the Sound's chain. The
  part keeps its seat: input and zone, output or bus, level, pan, mute, solo and sends.
- **Dropped as a new part:** the part takes the Sound's chain and its suggested input and
  zone, and plays to Main.
- **Wires out of the part:** shared buses and sends belong to the Performance, so they are
  never in a Sound (see below). User wires that cross the part's edge cannot travel; the
  save dialog lists them.
- **Scenes and controls:** the Sound brings its own controls (Assign 1–8, macros), which is
  what RFC 0011 already promises: "The sound's own assignments, saved with the sound by its
  author … travel with the sound to another performance or device." Scene parameters are
  Performance data and stay behind.

### (c) Any subgraph

The most general. A musician selects nodes in Routing and saves them.

- **Dropped into a Performance:** there is no defined input or output. The host must ask
  where each loose end connects, or guess. RFC 0011 rules out guessing for channels; it
  would be worse for whole wires.
- **Wires out:** every crossing wire is a question at load time.
- **Scenes and controls:** the same questions, for each node.
- **What it adds over (b):** saving two parts and a bus together. That is a small
  Performance, and it can already be saved as one.

### Recommendation: (b)

A Sound is a saved part. It matches the architecture's definition (event chain + engine +
insert effects), it has one input and one output, so dropping it in needs no questions, and
it is the thing the views already let a musician build. A one-plugin preset is the smallest
Sound. A larger arrangement is a Performance.

RFC 0009's table row ("A plugin identity plus its state plus the asset references that state
uses") describes one node's saved state. This RFC proposes renaming that row **node state**
and using **Sound** only as defined here. The `sounds.*` control methods are discussed under
Open questions.

### What a Sound holds

| In the Sound | Not in the Sound |
| --- | --- |
| The chain, its splits and lanes, with each lane's zones and Mix | Input kind and device selectors of the part it came from |
| Every node: plugin identity, version, state, prepared contexts, asset references | Output port or bus; sends and their levels |
| Wires with both ends inside the part, of both owners | Wires with one end outside the part |
| Controls owned by the part (below) | Performance controls, including a Super Knob that drives the part's controls |
| A suggested input: kind and zone, used only for **New part from sound** (decided) | Level, pan, mute, solo |
| Name, tags, description, author, ID, revision, parent revision | Scenes, scene parameters, pads, tempo |
| Libraries the nodes use (RFC 0009) | |

Lane zones are inside the chain, so a key-split Sound (bass below C3, piano above) keeps its
split. The part's own zone is its seat in the Performance, so loading that Sound into a part
limited to C4 and up keeps the part's zone, and only the upper lane sounds.

### Controls owned by a part

Controls are Performance-level today: a name, a value and targets (`controls` in the
Performance format). A Sound needs the controls that belong to it to come along.

A control gains an optional **owner**, like a wire: a part, or the Performance. A control is
owned by a part when every one of its targets is inside that part (a node's parameter, or
another control the part owns). Part-owned controls are saved in the Sound and replaced with
the chain. Performance controls stay, and may target a part's controls.

When a Sound is loaded into a part:

- the part's old controls go, and the Sound's arrive;
- a Performance control targeting one of the old controls is retargeted to the new control
  with the same name ("Assign 3" to "Assign 3"), else its target is kept and flagged as
  unconnected, the way RFC 0011 keeps a wire it cannot remap;
- the confirmation names every retargeted and flagged control before it is applied.

### Scenes

A scene stores all of RFC 0011's full model: plugins and wires off, each part's mute, solo and
mix, parameter values and control values. Loading a Sound into a part:

- keeps the part's mute and mix in every scene, because the part stays;
- removes what scenes hold of the nodes that left (values, plugins and wires off, user wires'
  Mix rows), and the parameters set aside on them;
- keeps scene values for Performance controls, and for part controls that kept their name.

Whether a scene may choose a different Sound for a part is an open question.

### Wires that leave the part

- **Sends and outputs to shared buses** are the part's seat, not its sound. Loading a Sound
  keeps the target part's sends. A new part from a Sound has none.
- **User wires inside the part** are saved, attached to their nodes and ports, as RFC 0011
  attaches them.
- **User wires crossing the part's edge** (a sidechain from the kick part, the pad wired into
  another part's vocoder) are not saved. **Save as Sound** lists them first: "2 wires
  leave this part and won't be in the Sound."
- **Loading over a part with crossing user wires:** the nodes they attach to are removed.
  As when a plugin is replaced, a crossing wire is kept, unconnected and flagged, rather than
  refusing the load or dropping the wire. The confirmation lists them. If the new chain has a
  node in the same position with a port of the same role, the wire moves to it, as
  `graph.replace` moves wires today.
- A node fed by several parts (an implicit shared row in RFC 0011) is outside every part, so
  it is never in a Sound.

## What a Performance is

A Performance is the whole graph: its parts, their seats, buses, user wires, scenes,
controls, pads, transport and libraries. That is what `performance.json` saves today, and it
stays the unit of saving and opening.

**It keeps its own copies.** Each part is stored complete, as now. A part loaded from the
library also records where it came from:

```json
"from": {"sound": "5f0c…", "revision": "8a21…", "name": "Glass Rhodes", "modified": true}
```

Reasons for copies over links:

- **Portability.** The architecture's portable-sounds rules and RFC 0006 make a Performance
  carry everything needed to play it, with an exact dependency lock. A link to a library item
  that is missing on the other device would make the Performance unplayable.
- **Rehearsed work stays put.** RFC 0006: "rehearsed Setlists keep their pinned revisions
  until updated", and the packages section: "saved work keeps the version it was made with
  until moved on deliberately". Editing a Sound in the library must not change a gig.

The provenance makes the useful parts of linking explicit actions:

- **Update from library:** when the library has a newer revision of the Sound, the part
  offers it, with the same confirmation as loading.
- **Save part as Sound:** saves a new revision of the Sound it came from or a new Sound.
- `modified` is worked out by comparing the part with the revision it came from.

**One data model, no second format.** The Performance format says: "Save, reopen, import and
export use the same data model." A Sound is saved with the same objects: `nodes`, `wires`,
one entry in `parts`, part-owned `controls` and `libraries`, plus the info block, in a
folder with the same `nodes/` layout. Only `format` differs (`oscairn.sound`), and the reader
requires exactly one part with no seat fields. The Performance reader and the Sound reader
are one reader.

**Format changes replace version 2.** Adding the info block, provenance and control owners
makes the Performance format version 3. Under the
[initial-development policy](../developer-guide.md#initial-development-compatibility-policy)
there is no version 2 reader afterwards; fixtures are recreated, and version 2 documents are
refused with a clear message, as version 1 is today.

The info block, the same in both:

| Field | Meaning |
| --- | --- |
| `id` | Stable ID, made once at first save. A copy saved as new gets a new one |
| `revision`, `parent` | This revision's ID and the one it was saved from, as RFC 0006 asks |
| `name` | Display name, up to 63 UTF-8 bytes like prepared-sound names |
| `tags` | Up to 32 tags (below) |
| `description`, `author` | Optional text |

### Importing a Performance

Built 2026-10-31 (`performance.import`, Performance > Import Performance). A Performance can
import another, all of it or chosen parts and scenes, adding them to the open one in one step
to undo. A partial import takes at least one part and, when the Performance has scenes, at
least one scene: the scene says how what comes is set (today its parts' levels and mutes,
all it holds of what comes: plugins and wires off, mutes, the mix, parameter values,
controls; mappings come with the scenes they are off in).

- **Chosen parts bring what they need:** their plugins with their state, the buses they play
  through or send to (and those buses' own), the wires and note-to-parameter mappings among
  what comes, the controls they own, and the Performance's controls whose every target comes
  too. Plugins no chosen part uses stay behind. Importing all of it brings everything, loose
  plugins included.
- **Every part and bus** arrives as a new part with its seat (input and zones, device
  selectors, output, fader, pan, mute, solo) and where it came from. An output or audio
  input this device lacks becomes the first one, with a note.
- **Names:** a scene, control or plugin whose name is taken gets the next number: "Verse"
  becomes "Verse 2", "Scene 3" becomes "Scene 4". Each rename is reported.
- **Scenes:** all of them with all of it, else those chosen, added beside the open
  Performance's. A chosen scene holds only what comes: its states for imported parts,
  plugins and controls. The open Performance's scenes keep the arriving parts, scene
  parameters and controls as they arrive; the arriving scenes keep the open Performance's as
  they are.
- Pads, tempo, transport and the imported info block stay with the open Performance.
- Notes held in the open Performance's parts carry on through an import (`control-import`).

## Plugin presets

The host already reads presets two ways:

- **CLAP:** a plugin's presets, indexed by the isolated scan (or re-read through the scan
  helper for a loaded plugin; RFC 0005), reach `presets.list` as `clap-preset:` URIs, and
  `loadPreset` loads one through `preset-load`.
- **LV2:** bundle presets, through the same method.

A preset is one plugin's state. In the picker:

- **In the library list**, presets appear as items of kind **Preset**, with their plugin's
  engine. Loading one into a part replaces the chain with that one plugin, as a one-plugin
  Sound. **Save as Sound** saves it to the library as a Sound.
- **On a selected box in the Part view**, the picker can narrow to **Presets for this
  plugin**. Loading one changes only that node's state. Where the plugin supports prepared
  sounds, this goes through `sounds.prepare` with the preset and `sounds.select`, so held
  notes keep the old patch; otherwise it is a state load and says it will interrupt.
- **Tags from presets:** CLAP preset metadata carries `add_feature` strings ("bass", "pad",
  "acid" … in CLAP's own examples), flags (factory, user, favorite), creators and a
  sound pack. The host's discovery receiver ignores all of these today (empty callbacks in
  `core/clap-host/src/Host.cpp`). Step 4 reads them: features become tags, flags set the
  favorite, creators the author. Its factory and user flags are not used: a preset
  is a preset wherever it came from.

Presets are listed from the plugins, not copied into the library. The index records them so
they can be searched with everything else.

## Tags

### What the MODX does

Yamaha's MONTAGE M manual describes a Performance Category Search with a Category filter
("Select one from Main and Sub"), an Attribute filter (combinations of attributes, some
matched exactly, such as AWM2, FM-X and AN-X), a bank filter and a keyword search
([Yamaha](https://manual.yamaha.com/mi/synth/montage_m/en/om01basicoperation0070.html)).
We have not checked the MODX's own manual page; we expect it to be similar, unverified.
Users on Yamaha's forum report that a Performance's category is taken from Part 1's main
and sub category, and that users cannot create categories of their own
([forum](https://yamahasynth.com/community/montage-series-synthesizers/setting-categorysubcategory-for-performance/)).
That is anecdotal.

So each item sits in one place in a fixed two-level tree. A layered piano and pad must be
filed as one or the other.

### Oscairn's tags

- **Free tags.** Any short word or phrase. Stored lowercase, trimmed, up to 32 UTF-8 bytes,
  up to 32 per item. Every tag is the same kind of thing, whoever added it: one preloaded on
  a device, one a pack came with, one the musician typed.
- **Suggestions** come from the tags already in the library, most used first, so a new
  device suggests the tags of its preloaded content and the list grows with use. There is no
  separate vocabulary fixed in the host or added by packages.
- **Facets the host works out**, never stored as tags:
  - *Engine:* the plugins a Sound uses (Basic Synth, Sampler, FM engine, analog engine,
    Delay …), from its nodes;
  - *Kind:* Sound, Performance, Preset;
  - *Seamless:* whether every plugin in it supports prepared switching, so it can load
    while notes are held.

### Where tags are stored

- **In the file,** in the info block. The file is the truth: "Files first, index second."
- **In the index,** as a copy for searching. The index is a cache, rebuilt by scanning, like
  the asset store's `index/` files. Deleting it loses nothing.
- **Changing an item's tags changes its file.** A locked library's items keep the tags they
  came with.
- **What the tag list shows is yours:** you can hide tags you do not want in the tag list and
  give a tag a color, used on its chip and in lists. These are settings in the user file, not
  changes to any item, so they work the same on locked items. A hidden tag still matches a
  search typed in full.
- **Favorites and recently used** are the user's, per device, in the same user file.
  Favorites sync; recents do not. They never go in an item's file, so a favorite does not
  change a Sound's revision.

### Libraries

- **A library is a set of Sounds and Performances, with the samples they use, imported as a
  whole:** a sound pack, a library exported from another device, or a device's factory
  content, preloaded. It has a name and keeps its items' fixed IDs, so the same library has
  the same IDs on every device.
- **Its items are locked.** They are listed, searched, auditioned and loaded like any other,
  and a part loaded from one records where it came from as usual. Nothing saves over a
  library item or its samples: **Save** on a part from a locked Sound is **Save As New**,
  which makes the musician's own Sound (with a new ID) and points the part at it. Its tags
  are the ones it came with; hiding and colors still apply.
- **Deleting and uninstalling.** Items of a library can be deleted, from the library view or
  by deleting their files, and a whole library can be uninstalled, the factory library
  included. Performances keep working: their parts are copies.
- **Reinstalling restores it.** Reinstalling a library puts back whatever of it is missing,
  however it went, and touches nothing the musician made: their edits were Save As copies,
  outside the library. Reinstalling twice changes nothing. Restoring factory content is
  reinstalling the factory library.
- **Choosing how and what to import.** Importing a library asks:
  - **as a library**, locked, or **into your own Sounds and Performances**, where its items
    are the musician's own, to edit, save over and delete. Imported that way they get new IDs,
    as Save As New would, so installing the same library later as a library does not clash
    with them; Performances imported with them point at those copies.
  - **Importing again adds no duplicates.** Each item imported into your own storage records
    where it came from (the library and the item's ID there) and a hash of its contents as
    imported: its document and its plugins' saved state. Importing the same library again
    skips an item whose copy is still there with that hash. One that was edited since (its
    hash differs) or deleted is imported again as a new copy; an edit is never overwritten.
    Samples are already stored once by their hash (RFC 0009), however many items use them.
  - **all of it, or chosen items**: some Sounds, some Performances. An item brings what it
    needs (its plugins and samples), as RFC 0006's
    [choosing what to import](0006-packages-and-portability.md#choosing-what-to-import) says
    for any package.
- **Imported, not linked:** a library is installed into the library folders (RFC 0006's
  package store when it exists), so it plays without the medium it came from.
- CLAP preset features become tags as above.

Built 2026-11-01 ([archive format](../library-archive.md); `library.contents`,
`library.install`, `library.libraries`, `library.uninstall`, `library.delete`): a library is
one archive file (a zip, decided 2026-11-01, with a single-file form easier to share than a
folder), read as untrusted input. Installed as a library its items are locked under its ID in
`Libraries/`, Save over one is Save As New, deleting an item or uninstalling works and
installing again restores only what is missing. Installed into the user's own storage its
items get new IDs and an `origin` in their info (the archive's ID, the item's ID there, the
copy's content hash), and installing again skips copies still as imported. All of it or chosen
items; the samples go into the user's sample library by hash either way, so they stay when a
library goes. On the desktop: **Export Performance...** (asking whether edited parts' Sounds
go too) and **Install Library...** (choosing locked or the user's own, and which items) in the
Performance menu; **Delete** and **Libraries...** (install, uninstall) in the Library app, where
a locked item says so. `control-archives` checks export and install across three data folders;
`control-package` checks unsafe archives refused. Not yet: the factory library itself (there is
no factory content yet), plugins in archives (RFC 0006), and a chosen Performance bringing the
Sounds it needs when they are not chosen.


### Exporting a Performance

An exported Performance carries its parts complete, as copies, whatever they came from: many
are edited from their Sounds or made from scratch and have no Sound saved apart, and they need
none. Each part also keeps where it came from, if it came from a Sound, as a reference
(`from`).

- **Samples and other assets always go.** Every sample, waveform and other asset the
  Performance's plugins use is in the export, so it plays on a device that has none of them.
  The device stores each once by its hash (RFC 0009), skipping those it already has.
- **A part's Sound goes with it when the part is still that Sound:** unedited since it was
  loaded or saved, with the content hash the library has for that revision. It costs no
  more than the part itself and keeps the link working on the other device.
- **A part edited since its Sound** may also bring that Sound, as an option, off by
  default: the Performance has the edited part, and the Sound is the earlier version.
- Importing the Performance adds the Sounds it brings that the device does not have; one
  whose content hash is already there is not added again. A part whose Sound did not come,
  and is not on the device, plays the same; Save and Update from library treat its Sound
  as missing.
- A Sound from a locked library comes as that library's item, locked, with its library's name
  and ID.

Built 2026-11-01 (`performance.export`, the [archive format](../library-archive.md)): the export
is one archive file holding the Performance, every sample, and the Sounds as above. Installing
it is installing a library (below), into the user's own storage by default; a part's `from`
then points at its Sound's copy. Not yet: a Sound from a locked library exported as that
library's item.

### Filtering

- **Within a facet and across facets, chosen tags combine with AND.** Choosing *bass* and
  *dark* shows items tagged both. Choosing two engines shows items using both.
- **Search** matches name, tags, description, author and engine names, case-insensitive,
  and combines with the chips by AND. Name matches rank first, then tag matches.
- **Each chip shows its count** under the current filter, and chips with no matches are
  dimmed, not hidden, so the facet does not jump around.
- **Favorites** and **Recent** are shortcuts above the facets. Recent lists the last 50
  items loaded on this device, newest first.
- **Sort:** name, recently used, or recently saved.

## The picker

One view model, `LibraryModel`, in the same style as `PartsModel`: state and actions with no
drawing, tested without a window. It holds the query, the results with their facets and
counts, the selection, the target (a part, a new part, or one node for presets) and the
seamless check for the selected item against that target.

### Actions

| Action | What it does |
| --- | --- |
| **Load into part** | Replaces the target part's chain and part controls with the Sound's, keeping the part's seat. Confirms first when it would flag wires or controls or cut notes |
| **New part from sound** | Adds a part with the Sound's chain, suggested input and zone, playing to Main |
| **Audition** | Plays the selected item without changing the Performance (below) |
| **Open performance** | For a Performance: replaces the whole Performance, after asking about unsaved changes; seamless (held notes and tails ring out); it is `performance.load` |
| **Load preset** | With a node as target: changes only that plugin's state |
| **Save as Sound** | From a part: saves it to the library, listing what stays behind |
| Favorite, Edit tags | Change the user file or the item's own file |

### Audition

Audition tries an item without keeping it, and without losing what it replaces.

- **A Sound** plays in the target part's place: the part's chain is set aside, silent, and the
  Sound's chain takes its notes and plays to the same place, so the musician hears the
  candidate in place.
- **A Performance** opens in place of the open one (decided by Jacob, 2026-11-01), which stays
  loaded and silent; its held notes and tails ring out as in any Performance change.
- **Stop** puts back what was there: the same plugins, with their state and prepared sounds,
  not copies loaded again, and the undo history as it was. The tried plugins ring out with
  their tails. **Keep** makes the tried item the change; a Sound's load is then one step to
  undo. Edits made while auditioning go with the tried item. Choosing another item stops the
  last; opening a Performance keeps it.
- With a budget set ([RFC 0025](0025-resource-budgets.md)), a Performance that would not fit
  beside the open one is refused, saying so; a Sound's plugins pass the same check as adding
  them, while the part's own chain is still loaded.

Built 2026-11-01 (`core/control/src/audition.cpp`; `audition.start`, `audition.stop`,
`audition.get`). What is set aside is held the way the undo history holds a removed plugin
(the model before the audition, holding the plugins), so putting it back is the undo
machinery's own. Earlier designs used a hidden audition part beside the target; trying the
item in the part's place gives the same sound with nothing hidden in the Performance.
In the Library app, **Audition** (or Space, or the item's menu) tries the selected item, a
Sound in the part the Part view shows; while it plays, choosing another row tries that one, the
row says Auditioning, and the button reads **Stop**, with **Keep** beside it. Picking, canceling
or closing the picker stops it.

`control-parts` checks a Sound put back and kept (and its load undone), and a Performance
whose held note plays on through the audition and back, put back and kept;
`control-library` checks the methods; `desktop-shell` auditions from the Library app and stops.

Where audio goes when the device has a cue output is an open question.

### Seamless switching

The architecture asks for this:

> Scene and sound changes should keep held notes and effect tails rather than rebuild the
> graph and cut audio. … The host reports when a transition cannot be seamless.

Decided 2026-10-31: seamless switching is for **scene and Performance changes**, which happen
while playing. **Loading a Sound into a part** (Load into part, New part from sound, Update
from library) is editing, and replaces the part's chain at once, cutting its notes and tails.

What the engine has for it, tested in `note-routing`:

- **Draining note connections** (`Graph::setDraining`): a connection that takes no new note
  while the notes it holds still get their releases, expressions and pedal.
- **Retiring nodes** (`Graph::setRetiring`, `Graph::idle`): the audio thread counts the notes
  still sounding through a node, how long its output has been below -100 dB, and whether the
  plugin said it is asleep (`CLAP_PROCESS_SLEEP`, which hosts now report), so the control
  thread knows when it may go.

A scene that switches a block off (RFC 0011's full scene model, built) uses them: the block's
notes drain and its tail rings out while new notes play what is on. A note held while importing
a Performance plays on (`control-import`).

**Performance changes** are seamless, built 2026-11-01: opening a Performance (from a setlist,
the library or `performance.load`) carries the outgoing one's plugins into the new graph under
their IDs (`Graph::carryOver`), retiring. Their sound to the outputs and to each other stays; the
note connections into them drain through the same note routes, so a note held across the change
plays on until released, pedal included; nothing else reaches them, and new notes play the new
Performance. They are hidden from it (`graph.get`, saving, undo, node IDs) and each goes once
idle, as a scene's: asleep or three seconds of silence, or 30 seconds without a note. Notes
queued at the moment of the change are no longer dropped, so a release reaches the old plugins.
Changing again while one still rings carries those too. A change that would not fit with both
Performances in memory and CPU opens as a cut instead, and the desktop says so after opening
it: admission, in [RFC 0025](0025-resource-budgets.md#status). `control-parts` checks a held note
through the change, an edit to the new Performance leaving the old one ringing, a new note
reaching only the new one and the old one going; `control-performance` checks a held note
through the change and, once it has rung out, output identical to a fresh load.

Measured 2026-11-01 (`control-setlist-change`, Release, Apple Silicon, in-process plugins): two
Performances of eight parts each (four Samplers with their own 16 MiB sample, four Basic Synths,
each part through its own Delay context: 16 plugin instances), changed back and forth with a note
held. The cue takes 36–63 ms (median 39 ms) until the next Performance is published, all of it
instantiating plugins and loading their state and samples; the change adds about 80 MiB of
resident memory, and 32 to 48 instances live until the old ones have rung out. The same bench
found every change keeping the plugins it retired alive for good (about 80 MiB more each time);
fixed in `Graph::publishReplacement`, and the bench checks memory no longer grows with each change.

**The next one prepared ahead**, built 2026-11-01: while one Performance plays, the next is read
and its plugins created with their saved state and sounds, off the audio thread, in a
replacement graph that waits (`preparePerformance`, `performance.prepare`); its plugins get
their main-thread callbacks meanwhile. Opening that folder then only carries the playing one
over and publishes. One is prepared at a time; preparing another, opening a different one or a
folder saved over since lets it go and the open reads as before. Plugins added while it waits
take IDs after its own. Admission decides first: a Performance that would not fit beside what
plays now is not prepared, and opens as a cut when cued, as before; the Setlist app says so. The
Setlist app prepares the entry after the one it opens; going to it while it is still being
prepared waits for it, going anywhere else lets it go. Preparing holds the engine's control
thread as long as a load does, so on-screen controls wait meanwhile.

Measured with the same bench: the cue takes 0.5–1.0 ms (median 0.6 ms) instead of 39 ms;
preparing takes what the load took, while the last one plays. Memory is the same, both
Performances at once, but from the preparation on rather than from the cue. `control-performance`
checks the prepared change (a held note through it, a plugin added while it waited, output
identical to a fresh load once the old one has rung out), its physical controls' overrides
carried as by a direct load, and a folder saved over after
preparing read again; `control-parts` checks admission (not prepared when both would not fit,
then a cut; prepared and ringing out when they would); `control-performance-format` the RPC;
`desktop-shell` the Setlist preparing its next entry over the app's engine connection without an
error, and going to it.

### Fewer instances: reusing what is loaded

Preparing ahead moves the load off the cue but still holds two Performances' instances while
the old one rings out, and each part has its own instance even when several use one engine
with native contexts. Engines with native contexts can do with fewer: the Basic Synth and the
Sampler hold several sounds in one instance ([RFC 0005](0005-sound-contexts.md); the Sampler
keeps audio that several sounds use once), the Delay several effects
([RFC 0007](0007-effect-contexts.md)). Three slices, each on its own:

1. **Effect contexts across a change.** Reading the next Performance, a group of nodes sharing
   one engine (its saved `engine` key) whose plugin (format, ID, version, path) is an engine
   of the playing Performance that offers effect contexts becomes new contexts of that engine
   instead of a new instance, each given its saved state; the engine is left alone, and the
   old Performance's contexts ring out on it as before. The whole group goes into one engine,
   so the saved grouping holds: when the engine has too few free contexts for the group
   (`effectResources`: capacity less prepared), it spills, and the group gets an instance of
   its own as today. A context that outlives the engine's first node keeps the engine; saving
   names the engine by the open Performance's first node in it, never one ringing out.
   Output after the change matches a fresh load, since contexts are independent.
2. **Sound contexts across a change.** The next Performance's instrument whose plugin is one
   playing, with native sound contexts, has its sounds prepared in the playing instance
   instead of a new one: the node is kept, at the cue its channels select the new sounds,
   held notes keep theirs until released, and the old sounds are discarded once idle (the
   contract's own lifetime rule). This needs the next Performance's sounds out of its saved
   snapshot without loading another instance: a contract call that prepares a snapshot's
   contexts beside the live ones, not replacing them, with IDs of the instance's choosing; and
   per-sound output buses where the two Performances' chains after the instrument differ (the
   Basic Synth's eight; the Sampler has none yet), else that instrument loads as today. It
   spills the same way, to a new instance, when the free slots (Basic Synth: seven beside
   Default) are too few. Where the Sampler's sounds use the same samples, the change holds
   them once instead of twice.
3. **Parts sharing one engine.** Within a Performance, parts whose instruments are the same
   native-context plugin share one instance: each part its own sound, MIDI channel and output
   bus, its chain starting at that bus. A node then belongs to several parts, which the part
   model and the saved format (a part's instrument as engine plus sound) must say; a format
   change replaces the old one outright. A Performance needing more sounds or buses than an
   instance has spills to a second instance, and so on.

Slice 1 is built first: it needs no contract change and makes the cross-graph sharing that
slices 2 and 3 also need. Slices 2 and 3 are not built.

Slice 1 built 2026-11-01, for every Performance opened or prepared (`Graph::addEffectContext`
from another graph; `Session::read`). Admission still counts the next Performance's plugins at
full cost. `control-performance` checks the Delay contexts of a Performance opened over itself
going into the playing engine (four contexts on it while the old two ring out, two once they
have gone), a save made meanwhile naming the engine by the new contexts and reopening shared,
output identical to a fresh load once the old ones have gone, and an engine with too few free
contexts spilling the group to a new instance. The saving is the engine instance: for the Delay
little memory in the engine's process (RFC 0007 measured 24 bytes between two contexts and two
engines), more when plugins run in workers, where each instance is a process.

#### Free instances across a change, and instances kept warm

Proposed 2026-11-05. Measured first: opening "Where Is My Mind Pno" (20 nodes: 4 sfizz, 4
arpeggiators, 12 EQs) in workers loads 24 instances, one worker process each, and every later
change loads all-new instances again even when the next Performance uses the same plugins; only
the EQs went into the playing engines (slice 1). Each pooled instrument (sfizz here) loaded
twice: the pool restoring its snapshot loaded a second instance for its first sound and
discarded the one it was made with. Per sfizz node about 23 ms start a worker and load the
plugin, 23 ms again for the second instance, then 35 to 460 ms for its state, its samples.

A fourth slice, for every plugin, with or without contexts: **a node of the next Performance
takes over a loaded instance that is free, loading its state into it, instead of loading a new
one.** Free instances come from two places, taken in this order:

1. **Instances kept warm.** Once an instance of an outgoing Performance has rung out it is
   kept loaded, out of the graph, so it costs no CPU, instead of being destroyed. An
   instance whose state is already the node's, byte for byte, goes first (switching back),
   then the least recently used.
2. **The playing Performance's free instances**, and those of the one before still ringing out,
   when a Performance is loaded at the cue, only where keeping them warm would not fit (the
   middle rule, below). Not while one is prepared ahead: it is
   read while the outgoing one plays, so its instances are not free then; a prepared Performance
   takes only instances kept warm. Nor while auditioning a Performance: stopping puts back the
   instances it set aside, as they were.

**The matching rule.** A node may take over an instance when all of these hold:

- The node is its own engine's only node (its saved `engine` is itself and no other node names
  it); groups of nodes sharing an engine go by slice 1.
- The instance is a plugin instance of its own: not an effect context, no contexts made from it,
  not a stand-in, not failed.
- The same plugin: format, ID and version, at the path the node would load (its saved path when
  that exists, else the store's build of that version), **and the same build**: the bundle's
  digest now is the digest it had when the instance was loaded. A plugin rebuilt since is never
  reused.
- The same placement: in a worker when the placement policy puts that plugin in a worker now,
  in the engine when it does not.
- The same ports: after its state loads, its audio and note ports must be the node's saved ones,
  the check a fresh load gets. One that differs is let go and the node loads a fresh instance.
- Free. An instance kept warm is. A playing one is free when no note sounds through it and it is
  asleep or has been silent (below -100 dB) for three seconds, the ring-out rule without its
  30-second limit, so nothing still sounding is cut; and when every node sending it audio or notes
  is free too, so a held note's chain stays whole: a synth holding a note keeps its effects.

**Keep or take over: the middle rule.** Decided 2026-11-05 (Jacob): a change keeps the outgoing
Performance's free instances warm and gives the incoming nodes instances kept warm or fresh ones;
it takes over a playing instance only where keeping it warm would not fit. So switching back and
forth between two Performances finds each one's instances holding its state, at the cost of both
Performances' memory and a slower first switch. Precisely, when a Performance is read at the cue:

- **A cut** (the two would not fit together, [RFC 0025](0025-resource-budgets.md)) keeps nothing
  warm, so it may take over any of the outgoing instances, as before.
- **Otherwise** the free playing instances are counted in node order. One is kept, not offered to
  the incoming nodes, while, with those kept before it:
  - they are at most 32, the warm limit. The instances kept warm already are older and go first
    (least recently used), so they do not count;
  - with memory classes, their memory fits the room admission leaves opportunistic data once the
    change has happened: the device's memory less the system's share, the running apps'
    reservations, the margin, recording's committed bytes, the incoming Performance at its
    estimate (full cost, as admission counts it) and the outgoing nodes not free (they still ring
    out; engines with effect contexts are counted here too). An instance's memory is counted as eviction counts
    the instances kept warm: a worker's resident memory; one in the engine's process counts
    none (a shortcut of the eviction, not of this rule). Without memory classes only the warm
    limit applies.
- **A kept instance holding a node's state already** (the same Performance opened again) is
  still taken over by that node: keeping it would only hold a second copy of a state the
  incoming Performance holds.
- Every other kept instance rings out with the outgoing Performance at the cue, at once since it
  is silent, and joins the instances kept warm. The rest are offered after the instances kept
  warm, as before.

Since a seamless change was admitted with both Performances together, keeping fits whenever the
estimates hold; the memory test decides only when an instance's resident memory has outgrown its
estimate.

**Halfway through a switch.** The choice is made once, before the read, from the figures
admission used to call the change seamless; it is not made again as instances load. Between the
read and the cue memory holds the outgoing Performance, what still rings out, the incoming one's
fresh instances and the instances kept warm before: the first three are what seamless admission
checked, the incoming one at full cost, so the rule adds nothing to the peak that admission did
not count. The instances kept warm before are opportunistic and are not let go before the read,
since the incoming Performance may take them. A plugin refused for memory halfway fails the load,
as any refusal does: the room it is checked against counts the incoming Performance alone, so
taking over a playing instance instead would not make room. If memory is tighter than the
estimates said, the end of the change corrects it: admission's eviction runs right after the
change and again as each outgoing instance rings out and is kept, letting instances kept warm go,
least recently used first, until they fit, and memory pressure lets all of them go. Switching back
is then slower, never a failure.

**How it is taken over.** An instance kept warm gets the node's state when the Performance is
read; nothing plays it. A playing instance is claimed when the Performance is read and added to
the replacement graph under a new node ID; its state is loaded last, after everything else in the
document has been read and checked, so a document that fails leaves it as it was. At the cue it
leaves the outgoing graph instead of being carried over: nothing rings out on it, since nothing
did. If its state does not load, the node loads a fresh instance instead. A note played on that
part between the check and the cue reaches the instance as before and is released at the cue (the
graph releases the notes of routes that go); a note played during the load on a part whose
instance is taken over does not ring out. A failure in the last step itself (a plugin refusing the
state even in a fresh instance) fails the load with the outgoing Performance playing on, and
leaves it sounding exactly as before: each playing instance's own state is saved just before it is
given the new one (decided 2026-11-05, Jacob; one state save per instance taken over), and put
back when the load fails, into every instance given a state so far, the one refusing included.
An instance let go for a fresh one gets its own state back too, since it plays on in the outgoing
Performance; one whose own state cannot be saved is not taken over but let go for a fresh one.
One that already held the node's state was only reset, which changes no state, so it is not
saved. An instance kept warm is not saved either: nothing plays it, and a failed load leaves it
marked as holding no known state, so it is never mistaken for one holding its old state.

**Never a different sound.** A fresh instance is activated, deactivated, given the state and
activated again. A reused one is deactivated, given the state and activated, as every state load
does. One whose state already equals the node's is not given it again but reset (CLAP `reset`:
buffers, voices, envelopes, modulation), so switching back costs no state load and no sample
load. Plugins must make these sound alike; [the porting guide](../plugin-porting.md) says so. A
render comparison, the same notes into a Session that switched and one that opened the same
Performance fresh, checks every sample (`control-performance-switch`, in the engine and in
workers).

**Pools and contexts.** A pooled instrument is taken over whole: restoring a snapshot into a pool
keeps its plugins, slot by slot, and loads only the slots it lacks, so a fresh pool no longer loads
a second instance for its first sound (built 2026-11-05; `prepared-instances` counts the instances
a restored pool loads, one per sound, and fails without). A native-context engine that is free gets the node's
snapshot like any other plugin (its exclusive snapshot replaces its sounds), which is as fast as
preparing them beside; slice 2, preparing the next sounds in an instance still sounding, still
needs its contract call and is not built.

**Kept warm, and bounded.** An instance that has rung out is kept only when it could be taken
over (the rule above, without the ports); at most 32, the least recently used let go first. With
memory classes they are opportunistic data ([RFC 0025](0025-resource-budgets.md#the-order-of-eviction),
order 2): let go when admission needs the room, on memory pressure before anything else, and
counted in what the device holds. A change that opens as a cut keeps none, since it cut for want of
room. Admission still counts the next Performance's plugins at full cost, as before.

`Session::reusedInstances()` counts nodes opened in an instance already loaded (slice 1's contexts
included) and `pluginInstancesLoaded()` the instances loaded, for the bench.

Taking over the playing Performance's free instances built 2026-11-05 (`Session::freeInstances`,
`Session::read`; `Graph::silent`, now counted for every node, and `Graph::reload`).
`control-performance-switch` (and `-workers`) builds two Performances of
four parts on the same plugins (two Basic Synths, two plain CLAP instruments in the pool, each
through a Delay of its own) and one of four Samplers, and checks: every instance of the silent
outgoing one taken over (8 of 8, none loaded; the Delays as slice 1's contexts), output identical
sample for sample to the same Performance opened fresh, in both directions; a note held on one part
through the change keeping that part's instrument, which rings out, while the rest are taken over;
nothing taken where nothing is shared; and a document that fails after its plugins are loaded
leaving every parameter of what plays unchanged. Without the change the counts fail; with the
taken-over instances not given their state, the render comparison fails at block 0.
`control-parts` auditions a Performance over a silent one opened from its file and checks the one
put back plays its own sound; taking over during an audition fails it.

Measured the same day, Release, M3 Max at a load average of 30 to 55 from other builds, so the
figures are ranges over runs (milliseconds from `loadPerformance` until published):

| Change | Before | After |
| --- | --- | --- |
| Bench, workers, A to B, both silent | 31–39 ms, 6 instances loaded | 10–33 ms, 0 loaded, 8 reused |
| Bench, engine, A to B | 2.8–4.1 ms, 6 loaded | 2.1–3.4 ms, 0 loaded |
| "Where Is My Mind Pno" cold, workers | 420–680 ms, 24 loaded | 340–720 ms, 20 loaded |
| It to "NKC Christmas" (median of 5), workers | 650–780 ms, 16 loaded | 360–800 ms, 2–3 loaded, 16 reused |
| "NKC Christmas" back to it (median of 5), workers | 425–460 ms, 12 loaded | 180–310 ms, 0 loaded, 20 reused |
| The same, plugins in the engine | 170 ms, 12 loaded | 160–195 ms, 0 loaded |

What is left is the state itself: sfizz loading its samples takes 35 to 460 ms per instance,
whether the instance is new or taken over. Taking over saves the worker start and plugin load,
about 23 ms per instance, and the memory of a second Performance's instances during the change.

Instances kept warm built the same day (`Session::keepWarm`, from `ringOut`; `Graph::release`;
`PluginHost::reset`, a request of its own in a worker). An instance that holds the node's state
already is found by the snapshot it last took (folder and document time) and confirmed by saving
its snapshot and comparing it with the node's, file for file; it is then reset, not given the
state. The warm ones get their main-thread callbacks; `memoryState` counts them as opportunistic;
admission's eviction lets them go least recently used first, and memory pressure lets them all go
first. `control-performance-switch` checks a Performance's instruments kept warm once rung out
(Delays, engines with contexts, are not), taken over on the way back with all four holding their
state (`keptStates`) and output identical to a fresh load, and memory pressure letting them go.
Measured as above: "Sick tines" back to "Where Is My Mind Pno" with its sfizz instances kept warm
takes 41–58 ms in workers (25 ms in the engine) instead of 390–455 ms; its 4 sfizz instances held
their state, the arpeggiators were given theirs.

Taking over and keeping warm pulled against each other: a switch A to B took A's instances, so B
back to A found none holding A's state and loaded it again. The middle rule (above) built the
same day (`Session::readTaking` marks the playing instances to keep, `Free::keep`; `read` offers
them only to a node whose state they hold). `control-performance-switch` now checks: A to B keeps
A's four instruments warm and loads B's (4 loaded, the Delays contexts of A's); B back to A and A
to B again take the instances kept warm, all four holding their state, nothing loaded, output
identical to a fresh load; a cut takes over all 8 free playing instances; a note held through a
change keeps every instance of A, the sounding one included; and past the warm limit, 34 parts
with one holding a note, 32 of the 33 free instances are kept, one is taken over and the
sounding one is not. Without the rule the counts fail. The memory test itself is not exercised: with a seamless
change admitted, keeping fits unless a worker's resident memory has outgrown its estimate.

Measured with the bench on "Where Is My Mind Pno" (A), "NKC Christmas" (B) and "Sick tines" (C),
Release, M3 Max at a load average of 35 to 85 from other work, so ranges over two or three runs;
memory is the resident memory of the bench process and its workers (`ps`), once A is open and
again after five rounds of A to B and back:

| | Before | After |
| --- | --- | --- |
| A to B, the first, workers | 420–1120 ms, 3 loaded, 16 reused | 1100–1320 ms, 11 loaded, 8 reused |
| The same, engine | 320–460 ms | 460–480 ms |
| B to A, the first, workers | 185–355 ms, 0 loaded, none holding their state | 42–89 ms, 0 loaded, 4 holding theirs |
| A to B again (median of 4), workers | 340–840 ms, 1 loaded | 67–100 ms, 1 loaded, 5 holding their state |
| B to A again (median of 4), workers | 185–365 ms | 50–88 ms |
| The same two, engine | 295–345 ms and 160–185 ms | 36–54 ms and 36–43 ms |
| Memory, A open, workers | 517–534 MiB | 598–610 MiB |
| Memory after switching, workers | 1167–1267 MiB, 2 kept warm | 888–898 MiB, 10 kept warm |
| The same two, engine | 214–241 and 537–883 MiB | 329–345 and 495–516 MiB |

Repeated switches between the two take about 50 ms, as expected, and the first takes longer, its
instruments loaded fresh. Memory after switching is lower than before, though both
Performances' instruments are now held. Before, every switch gave the sfizz instances another
state; that their resident memory grew with each would explain it, but it was not checked.

Saving each playing instance's state before it is taken over built the same day (`read`'s
`saved`, put back on failure). `control-performance-switch` makes a load fail in that last step:
a cut takes over A's four instruments, and B's fourth instrument's snapshot is unreadable, so it
is refused in the taken instance and in a fresh one after the first three took B's state. A then
plays the same notes as A opened fresh, sample for sample; without the change it differs from
block 3.

**Load preset** on a node is seamless where `sounds.prepare` accepts it (native contexts and
pooled instruments) and an interrupting state load otherwise, as today.

### Desktop

A **Library** pane, dockable like the other RFC 0019 panes, beside the parts list.

- Top: a search box, the kind switch (Sounds, Performances, Presets), and Favorites and
  Recent.
- Facet chips under it, one row per facet, wrapping; More tags collapsed.
- Results as a list (name, tags, engine marks, favorite star) or a grid of cards. The
  list is the default; the grid suits browsing a pack.
- An inspector for the selection: description, author, every tag, the engines, the
  controls it brings, whether it loads seamlessly into the target part, and the actions.
- **Drag** a result onto a part row: load into that part. Onto the space under the parts:
  new part. Onto a box in the Part view (presets): load into that plugin. The drop target
  highlights and names the action before release.
- Keyboard: arrows move, Return loads into the selected part, Shift-Return makes a new
  part, Space toggles audition, Cmd-F (Ctrl-F) focuses search. Right-click a result for all
  actions.

### Panel

A full-screen **Library** tab at 1024 × 600, touch sizes throughout.

- The header names the target: "Into Part 2 · Pad". Tapping it picks another part or New
  part.
- Left column: Favorites, Recent, then the facets as large chips, scrolling.
- Middle: results as large rows with name, three tags and engine marks.
- Right or bottom: the selected item's tags and the three actions, at least 44 pixels high.
- **The knobs follow the screen,** as on parameter pages: on the KICK's P1–P4, P1 moves
  through the results, P2 steps the Type facet, P3 steps Character, P4 steps Engine. The
  strip along the screen's edge names them. A press on P1, where the knob has one, loads.
- Search opens the on-screen keyboard. It is there, but the tags and knobs are the main path.

### Compact

Not built now (RFC 0019). The view model must allow:

- one list at a time: **Recent, Favorites, Tags, Engines, All**;
- turning the encoder moves, pressing enters or loads into the current part, a long press
  or second button auditions;
- Tags lists the tags, most used first, then matching items: AND by entering
  one tag, then another, from a "narrow further" row;
- no free-text search, no drag, no grid.

### What is saved where

| What | Where | Kind |
| --- | --- | --- |
| A Sound | `Sounds/<name>.osound/` in the user's library folder: `sound.json` and `nodes/` | User file |
| A Performance | `Performances/<name>.oscairn/`, as today, version 3 | User file |
| Libraries (packs, factory content) | Their own folder per library under the library folders, locked | Imported content |
| Plugin presets | Where the plugin keeps them | The plugin's |
| Index of all of the above | Under `OSCAIRN_DATA_HOME/library/` | Cache, rebuilt by scanning |
| Favorites, recents, hidden tags, tag colors | One user file under `OSCAIRN_DATA_HOME/library/` | User data |
| Samples a Sound uses | Asset libraries (RFC 0009), referenced, never copied | Assets |

The folder names are a convention, as `.oscairn` is today: the reader recognizes the
manifest. Exact folder locations follow the platform paths already used for the asset store.

### Control API

Proposed only; [control-protocol.md](../control-protocol.md) is not changed by this RFC. A new
`library.*` namespace avoids the existing `sounds.*` methods, which mean prepared contexts.

| Method | Parameters | Result |
| --- | --- | --- |
| `library.query` | optional `text`, `tags`, `engines`, `kinds`, `favorites`, `recent`, `sort`, `offset`, `limit` | `items` (id, revision, kind, name, tags, engines, favorite, seamless), `facets` with counts, `total` |
| `library.get` | `id`, optional `revision`, optional `part` to check against | the info block, nodes summary, controls it brings, estimated memory, and for a `part`: seamless or not, wires and controls that would be flagged |
| `library.tags` | none | every tag in the library with its count, for suggestions |
| `library.setTags` | `id`, `tags` | `{"accepted":true}`; writes the item's file |
| `library.favorite` | `id`, `favorite` | `{"accepted":true}` |
| `library.rescan` | none | counts added, changed and removed; emits `library.changed` |
| `parts.loadSound` | `part`, `id`, optional `revision`, `confirm` | without `confirm`: what would change; with it: `{"accepted":true, "seamless":bool}` |
| `parts.fromSound` | `id`, optional `revision` | numeric `part` |
| `parts.saveSound` | `part`, `name`, optional `tags`, `asRevisionOf` | `id`, `revision`, and `left` (crossing wires, Performance controls not saved) |
| `audition.start`, `audition.stop` | `id` and `part`; none | `{"accepted":true}` |
| `performance.load` | also accepts `id` in place of `directory` | as today |

`library.changed` joins the notification topics. Long scans run off the control thread, so
other requests do not wait the way `assets.register` does today.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Sound documents | Performance format version 3 with the info block, provenance and control owners; the Sound document on the same reader; `parts.saveSound`, `parts.loadSound` (cutting), `parts.fromSound` | A part with a split, two plugins, an internal user wire and two part controls, saved as a Sound and loaded into another Performance's part, renders sample for sample as the original part did. Crossing wires are listed and not saved; a Performance control is retargeted by name or flagged; scene parameters on removed nodes are reported. A version 2 document is refused |
| 2. Library index and tags | Scanning user folders and packages, the index as cache, the user file, tag editing and suggestions, `library.*`, `LibraryModel` | Files copied in from a computer appear after a rescan. Deleting the index and rescanning gives the same results and loses no favorite or tag. AND filtering, counts and search are checked in `LibraryModel` tests without a window |
| 3. Desktop pane | The Library pane, inspector, drag to part, new part, keyboard | From the pane alone: find a Sound by two tags, load it into a part, make a new part from another, by mouse and by keyboard. `desktop-shell` renders the pane in both input modes |
| 4. Plugin presets | Presets in the index; CLAP features, flags and creators read; Load preset on a node | The CLAP test fixture's presets appear tagged and load into their node; a native-context plugin's preset loads while a note is held without cutting it |
| 5. Audition and seamless loading | The audition part; part transitions on RFC 0007's automatic chain preparation | A note held across **Load into part** keeps the outgoing sound and its delay tail while new notes play the incoming one, in uneven block sizes, with no allocation on the audio thread. A load that cannot be seamless says so before it happens |
| 6. Panel | The Library tab at 1024 × 600 with knob bindings | `desktop-pad-editor` renders it; a test finds and loads a Sound using only the row knobs and taps |
| 7. Compact test layout | The list form at 320 × 240 with four encoders | Recent, one tag and a load reached with encoders only |

`SoundPickerModel`'s `synth`-only lookup goes when step 3 lands (its hard-coded Basic Synth
patches are now the plugin's own presets, [RFC 0005](0005-sound-contexts.md#presets)); its prepare, select and discard logic becomes the preset and transition path.

## Open questions


- **Scenes choosing Sounds.** Partly answered by the scene model (RFC 0011, "Scenes, the
  full model"): a scene enables and disables blocks and wires, so a part holding two lanes
  switches between them by scene. Whether a scene may also load a library Sound is open.
- **Renaming `sounds.*`.** Those methods address contexts inside one plugin. Renaming them
  `contexts.*` would free the word; the development policy allows it, but it touches the
  protocol, the remote client and tests.
- **Saved chain snippets.** An effect chain without an instrument ("my vocal chain") is a
  part whose input is audio, so it is already a Sound. Whether a fragment to insert into the
  middle of a chain is needed is left until groups exist (RFC 0011 step 7).
- **Audition output** on a device with a cue output: cue, or in place on Main.
- **Translating tags.** Whether a tag can show translated while it is stored as written.
- **Size limits on the device:** how many items the index must search quickly on the KICK's
  processor, and how it stays current. Options: rescan every folder at start (simplest,
  slowest with thousands of items on an SD card); keep the index and recheck only files whose
  times changed (fast start); and watch the folders for changes while running. The proposal
  is the second, with a rescan on request, and targets of a picker that opens in 100 ms and
  filters within 50 ms of a keystroke at 5,000 items. Not measured.
- **Shared effects and loops.** An effect three parts send to is a bus, or a node outside
  every part (RFC 0011's shared row), so no part's Sound holds it; a Performance does. Loops
  are refused by the graph today; feedback needs the delayed return edge the architecture
  plans, which a Sound would save like any other wire inside it.
- **Sends on a new part.** Should **New part from sound** offer to send to an existing bus of
  the same name as the one the part was saved from? This RFC saves no sends at all.

## Ruled out

- **A fixed category tree.** One place per item cannot file a layered sound, and users
  cannot add categories.
- **A Sound as a single-plugin preset only.** It cannot hold the chain the architecture
  defines a Sound to be. Presets stay, as the smallest case.
- **A Sound as any subgraph.** No defined input and output; every load would ask where wires
  go. A larger arrangement is a Performance.
- **Performances linking to library items.** It breaks portability and lets library edits
  change rehearsed work.
- **A separate Sound format.** A Sound uses the Performance data model and reader.
- **The index as the source of truth.** It is a cache; files and the user file are the truth.
- **Favorites in item files.** They would change revisions, and are one musician's, not the
  item's.
- **Tags of different kinds:** user tag overlays on top of an item's tags, a fixed host
  vocabulary. Tags are tags; what the list shows is the musician's setting.
- **Editable factory content restored by merging:** restoring around the musician's edits
  needed per-item revisions and copies. Factory content is a locked library instead.
- **Silent cuts.** Loading never cuts held notes without saying so first.

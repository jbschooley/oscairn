# RFC 0021: Sounds, Performances and the library

Status: in progress, 2026-10-31. Started with Performances: format version 3 with the info
block (id, name, description, tags; no provenance or control owners yet), and a library of the
Performances in the user's folder with the index as a cache, the user file (favourites and
recents), query, rescan and save and load by ID (`library.*`, `performance.info`/`setInfo`,
see [control-protocol.md](../control-protocol.md#library)). The desktop app has a Library
app (Performances and Sounds; search, tags, favourites, recent; a click opens a
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
- [x] The library of Performances: cache, favourites, recents, query, Save and Save As New
- [x] 1. Sound documents: save a part, load into a part or a new part, control owners
- [x] 1. Provenance: each part records the Sound and revision it came from and whether it was
      edited since; revisions with parents for Sounds and Performances; Save and Save As New
      for Sounds; Update from library; a deleted Sound breaks nothing
- [x] Content hashes for Sounds and Performances, in the info block and the index
- [ ] Authors in the info block, and `modified` set by preset and state loads
- [ ] Libraries: items locked, Save As New from them, deleting items, uninstalling, and
      reinstalling to restore what is missing ([design](#libraries)); factory content as a
      preloaded library
- [ ] Importing a library as a library or into your own storage, all of it or chosen items;
      importing again skips copies still unchanged (by content hash)
- [ ] Hidden tags and tag colors
- [x] Exporting a Performance keeps its parts' references to their Sounds
- [ ] Exporting with every asset, the Sounds of unedited parts, and optionally those of edited
      parts ([design](#exporting-a-performance))
- [ ] 2. Library index and tags: index, user file and Sounds done; tag editing, suggestions,
      packages and `LibraryModel` remain
- [ ] 3. Desktop pane: the Library app and Part menu done; the dockable pane, inspector,
      drag to part and keyboard remain
- [x] Importing a Performance, all of it or chosen parts with what they need ([design](#importing-a-performance))
- [ ] 4. Plugin presets
- [ ] 5. Audition and seamless loading
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

A scene stores each part's level and mute, the values of the scene parameters, and control
values. Loading a Sound into a part:

- keeps the part's level and mute in every scene, because the part stays;
- removes scene parameters on nodes that left, and says so;
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
import another, all of it or chosen parts, adding them to the open one in one step to undo:

- **Chosen parts bring what they need:** their plugins with their state, the buses they play
  through or send to (and those buses' own), the wires and note-to-parameter mappings among
  what comes, the controls they own, and the Performance's controls whose every target comes
  too. Importing all of it brings everything, loose plugins included.
- **Every part and bus** arrives as a new part with its seat (input and zones, device
  selectors, output, fader, pan, mute, solo) and where it came from. An output or audio
  input this device lacks becomes the first one, with a note.
- **Names:** a scene, control or plugin whose name is taken gets the next number: "Verse"
  becomes "Verse 2", "Scene 3" becomes "Scene 4". Each rename is reported.
- **Scenes come with all of it,** added beside the open Performance's. The open
  Performance's scenes keep the arriving parts, scene parameters and controls as they
  arrive; the arriving scenes keep the open Performance's as they are. Chosen parts bring no
  scenes: a scene describes a whole Performance.
- Pads, tempo, transport and the imported info block stay with the open Performance.
- It cuts what a graph rebuild cuts; the parts already playing are not replaced.

## Plugin presets

The host already reads presets two ways:

- **CLAP:** `kick_clap::Host::presets()` lists what a plugin's preset discovery factory
  declares, both inside the plugin and in preset files, and `loadPreset` loads one through
  `preset-load`. `presets.list` returns them as `clap-preset:` URIs.
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
  "acid" … in CLAP's own examples), flags (factory, user, favourite), creators and a
  sound pack. The host's discovery receiver ignores all of these today (empty callbacks in
  `core/clap-host/src/Host.cpp`). Step 4 reads them: features become tags, flags set the
  favourite, creators the author. Its factory and user flags are not used: a preset
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
- **Favourites and recently used** are the user's, per device, in the same user file.
  Favourites sync; recents do not. They never go in an item's file, so a favourite does not
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

Today a Performance saved to a folder keeps its parts' references; the export that brings
Sounds and assets along is not built yet (the folder references samples in their libraries,
as RFC 0009 has it for saved Performances).

### Filtering

- **Within a facet and across facets, chosen tags combine with AND.** Choosing *bass* and
  *dark* shows items tagged both. Choosing two engines shows items using both.
- **Search** matches name, tags, description, author and engine names, case-insensitive,
  and combines with the chips by AND. Name matches rank first, then tag matches.
- **Each chip shows its count** under the current filter, and chips with no matches are
  dimmed, not hidden, so the facet does not jump around.
- **Favourites** and **Recent** are shortcuts above the facets. Recent lists the last 50
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
| **Open performance** | For a Performance: replaces the whole Performance, after asking about unsaved changes. Not seamless; it is `performance.load` |
| **Load preset** | With a node as target: changes only that plugin's state |
| **Save as Sound** | From a part: saves it to the library, listing what stays behind |
| Favourite, Edit tags | Change the user file or the item's own file |

### Audition

Audition prepares the item on a hidden **audition part**: outside the Performance, never
saved, fed by the same notes as the target part, and playing to the same place. Auditioning
mutes the target part's own chain while it lasts, so the musician hears the candidate in
place. Leaving audition, or choosing another item, retires the audition chain with its
tails. Audition passes the same memory check as loading
([keeping plugins loaded](../architecture.md#keeping-plugins-loaded)) and says when an item
will not fit.

Where audio goes when the device has a cue output is an open question.

### Seamless switching

The architecture asks for this:

> Scene and sound changes should keep held notes and effect tails rather than rebuild the
> graph and cut audio. … The host reports when a transition cannot be seamless.

**Load into part** becomes a part transition:

1. Prepare the incoming chain off the audio thread: new nodes for plugins not in the part,
   prepared contexts (RFC 0005) or pooled instances where the same plugin is already
   there, and prepared effect contexts or new effect nodes (RFC 0007).
2. At an audio boundary, move the part's note input to the incoming chain. Notes already
   held stay with the outgoing chain, with their note-offs, sustain and expression, as
   RFC 0005's note ownership already does inside one plugin.
3. Feed the outgoing chain silence once its notes end, until its effects report no tail,
   then retire it.

This depends on the automatic chain preparation and retirement that RFC 0007 lists as
planned. Until it exists, **Load into part** builds the new chain and replaces the old one in
one commit, which cuts the part's held notes and tails. The picker says so before loading
("Cuts held notes in Part 2"), using the *Seamless* facet's check. It never claims a seamless
change it cannot make.

**Load preset** on a node is seamless where `sounds.prepare` accepts it (native contexts and
pooled instruments) and an interrupting state load otherwise, as today.

**Open performance** cuts, as `performance.load` documents. Seamless Performance changes
are setlist work, not this RFC.

### Desktop

A **Library** pane, dockable like the other RFC 0019 panes, beside the parts list.

- Top: a search box, the kind switch (Sounds, Performances, Presets), and Favourites and
  Recent.
- Facet chips under it, one row per facet, wrapping; More tags collapsed.
- Results as a list (name, tags, engine marks, favourite star) or a grid of cards. The
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
- Left column: Favourites, Recent, then the facets as large chips, scrolling.
- Middle: results as large rows with name, three tags and engine marks.
- Right or bottom: the selected item's tags and the three actions, at least 44 pixels high.
- **The knobs follow the screen,** as on parameter pages: on the KICK's P1–P4, P1 moves
  through the results, P2 steps the Type facet, P3 steps Character, P4 steps Engine. The
  strip along the screen's edge names them. A press on P1, where the knob has one, loads.
- Search opens the on-screen keyboard. It is there, but the tags and knobs are the main path.

### Compact

Not built now (RFC 0019). The view model must allow:

- one list at a time: **Recent, Favourites, Tags, Engines, All**;
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
| Favourites, recents, hidden tags, tag colors | One user file under `OSCAIRN_DATA_HOME/library/` | User data |
| Samples a Sound uses | Asset libraries (RFC 0009), referenced, never copied | Assets |

The folder names are a convention, as `.oscairn` is today: the reader recognises the
manifest. Exact folder locations follow the platform paths already used for the asset store.

### Control API

Proposed only; [control-protocol.md](../control-protocol.md) is not changed by this RFC. A new
`library.*` namespace avoids the existing `sounds.*` methods, which mean prepared contexts.

| Method | Parameters | Result |
| --- | --- | --- |
| `library.query` | optional `text`, `tags`, `engines`, `kinds`, `favourites`, `recent`, `sort`, `offset`, `limit` | `items` (id, revision, kind, name, tags, engines, favourite, seamless), `facets` with counts, `total` |
| `library.get` | `id`, optional `revision`, optional `part` to check against | the info block, nodes summary, controls it brings, estimated memory, and for a `part`: seamless or not, wires and controls that would be flagged |
| `library.tags` | none | every tag in the library with its count, for suggestions |
| `library.setTags` | `id`, `tags` | `{"accepted":true}`; writes the item's file |
| `library.favourite` | `id`, `favourite` | `{"accepted":true}` |
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
| 2. Library index and tags | Scanning user folders and packages, the index as cache, the user file, tag editing and suggestions, `library.*`, `LibraryModel` | Files copied in from a computer appear after a rescan. Deleting the index and rescanning gives the same results and loses no favourite or tag. AND filtering, counts and search are checked in `LibraryModel` tests without a window |
| 3. Desktop pane | The Library pane, inspector, drag to part, new part, keyboard | From the pane alone: find a Sound by two tags, load it into a part, make a new part from another, by mouse and by keyboard. `desktop-shell` renders the pane in both input modes |
| 4. Plugin presets | Presets in the index; CLAP features, flags and creators read; Load preset on a node | The CLAP test fixture's presets appear tagged and load into their node; a native-context plugin's preset loads while a note is held without cutting it |
| 5. Audition and seamless loading | The audition part; part transitions on RFC 0007's automatic chain preparation | A note held across **Load into part** keeps the outgoing sound and its delay tail while new notes play the incoming one, in uneven block sizes, with no allocation on the audio thread. A load that cannot be seamless says so before it happens |
| 6. Panel | The Library tab at 1024 × 600 with knob bindings | `desktop-pad-editor` renders it; a test finds and loads a Sound using only the row knobs and taps |
| 7. Compact test layout | The list form at 320 × 240 with four encoders | Recent, one tag and a load reached with encoders only |

`SoundPickerModel`'s hard-coded Basic Synth patches and its `synth`-only lookup go when step
3 lands; its prepare, select and discard logic becomes the preset and transition path.

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
- **Favourites in item files.** They would change revisions, and are one musician's, not the
  item's.
- **Tags of different kinds:** user tag overlays on top of an item's tags, a fixed host
  vocabulary. Tags are tags; what the list shows is the musician's setting.
- **Editable factory content restored by merging:** restoring around the musician's edits
  needed per-item revisions and copies. Factory content is a locked library instead.
- **Silent cuts.** Loading never cuts held notes without saying so first.

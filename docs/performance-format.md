# Performance format, version 5

A Performance saves the graph, all prepared sounds and their MIDI-channel assignments,
plugin settings/state, its wires and parts ([RFC 0011](rfcs/0011-performance-ui-and-editors.md)),
parameter mappings, sequencer parameters, tempo and clock source. **Save, reopen, import and export use the same data
model.** The machine-readable contract is [performance.schema.json](performance.schema.json),
using JSON Schema draft 2020-12. JSON matches our existing control and theme contracts,
has explicit types and broad tooling, and needs no YAML implicit typing/aliases or XML
object mapping. Plugin-owned state remains opaque; it need not be JSON.

The current implementation saves a folder. Move/copy the entire folder to export/import it;
open it through the desktop **Performance → Open Performance…** menu. **Save Performance As…**
writes a new `.oscairn` folder and refuses to overwrite an existing one. The suffix is
a desktop naming convention; the reader recognizes the manifest, not the folder name. Preparation and file I/O use
the separate engine's control thread, outside the UI and audio threads. These menu actions
require the separate engine (the default); `--in-process` is a development mode.

## Folder and manifest

```text
My Performance/
  performance.json
  nodes/
    node-1/
      snapshot
      state/                 # when the plugin provides complete state
        plugin.id            # CLAP identity
        state.clap           # opaque CLAP/native snapshot
    node-2/
      snapshot               # parameters, or a prepared-instance pool index
      0/state/...            # each prepared hosted instrument has its own snapshot
      1/state/...
```

LV2 state directories contain `state.ttl` and materialized assets. CLAP state is preserved
as supplied by the plugin; the host cannot discover external sample paths hidden inside
an arbitrary CLAP state blob. A plugin whose state is not self-contained may still require
its original sample library. Plugins that declare their files through
`oscairn.asset-dependencies`, such as the Oscairn Sampler, store asset references instead
([RFC 0009](rfcs/0009-sound-assets-and-dependencies.md)): the files stay in their libraries.
Package-managed dependency resolution remains planned in [RFC 0006](rfcs/0006-packages-and-portability.md).

Required top-level fields:

| Field | Meaning |
| --- | --- |
| `format`, `version` | Exactly `oscairn.performance` and integer `5` |
| `info` | What the library shows ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)): `id` (made at the first save, kept by every later one), `name` (the folder's name when none is set), `description`, up to 32 free `tags`, `color` (`#rrggbb`, or empty for none), and `revision` (made at every save) and `parent` (the revision it was saved over; empty for the first), and `hash`: SHA-256 of the contents, over the document without its info block followed by each file under `nodes/` by path with its own SHA-256, in path order. Names, tags and revisions are not contents. Equal hashes mean equal contents; the ID stays the identifier. `origin`: for an item imported into the user's own storage from a [library archive](library-archive.md), the archive's ID, the item's ID there and this copy's content hash as imported; `null` otherwise |
| `transport` | Tempo and `internal` or `midi` clock source |
| `audioInputs`, `audioOutputs` | Required logical graph ports: ID, channels, flags |
| `nodes` | Local key, display name, plugin identity/version, path hint, shared engine key, expected audio/note ports, and `enabled` (false: switched off, as scenes do; it takes no notes and no sound, and a part passes what reaches it around it) |
| `wires` | Audio and note wires: owner (`part`, with its part ID, or `user`), source and destination node and audio port, explicit channel pairs or none (the channel rules), the source's level, pan, mute and solo in the destination's Mix, a note wire's zones, and `enabled` (false: switched off; a note wire takes no new notes, an audio wire is silent) |
| `parts` | ID, name, input (`notes` with its zones, device rules (selectors, resolved against the devices of the machine that opens it) and `maxNotes`, `audio` with a graph input port, `none`, or `bus` for a shared bus), the chain of node keys and `{"split":id}` entries, its `splits` (kind, the keys of its Split and Join nodes or null, and each lane with its chain, zones, level, pan, mute and solo), the graph output port or the `bus` it plays through, its `sends` to buses with their levels, and its `level`, `pan`, `mute` and `solo` |
| `parts[].from` | Where the part came from (RFC 0021): `{sound, revision, name, modified}`, the library Sound and revision loaded into it, its name then, and whether the part has been edited since; `null` when none. Kept wherever the Performance is saved, to the library or to a folder (an export). The part never needs the Sound: it is a copy. Planned: an export brings every asset and the Sounds of unedited parts, and optionally of edited ones ([RFC 0021](rfcs/0021-sounds-performances-and-library.md#exporting-a-performance)) |
| `mappings` | Source node/lane to destination node/parameter |
| `scenes`, `sceneUntouched`, `scene` | Each scene (RFC 0011's full model): `id`, `name`, `scope` (the kinds selecting it applies: `enabled`, `mutes`, `mix`, `parameters`, `mappings`), its transition (`glide` in milliseconds, `curve`: `linear`, `ease` or `equal-power`, `launch`: `now`, `beat` or `bar`), and as stored: `off` (plugins switched off, by node key) and `offWires` (wires switched off, each by `kind`, `part` (0 for a user wire), `from`, `to`, `fromPort`, `toPort`, `send`, `split`, `lane`, `fader`), `parts` (each part's `level`, `pan`, `mute`, `solo`, `sends` and its splits' `lanes` Mix rows), `wires` (user wires' Mix rows, named the same way), `values` (every parameter scenes touch) and `controls` (each control's value); the parameters scenes never touch; and the scene last selected |
| `controls` | The control tree: each control's `part` (the part that owns it, or 0 for the Performance), name, value and targets (kind, node key and parameter, part, control, or for a `morph` the `from` and `to` scenes; start, end, curve, relative, base, and `offIn`: the scenes the mapping is off in, empty in a Sound) |
| `lock` | The plugin builds it was saved with ([RFC 0006](rfcs/0006-packages-and-portability.md)): `plugins`, each `{format, id, version, platform, sha256}`: per platform (such as `macos-arm64`), the build's digest, a file's SHA-256 or for a bundle the SHA-256 of each file's path and SHA-256 in path order, hidden files left out. Saving writes this platform's digest for each plugin that loaded, and keeps the entries the loaded documents had for other platforms and for stand-ins. An [archive](library-archive.md) bundles a plugin's build from its `pathHint` only while that build still has the locked digest, else from the plugin store |
| `pads` | The pad map: the selected bank and eight banks of 16 pads, each with its note, MIDI channel (-1 follows the player), velocity (0 as played) and pressure mode |

`node-1` etc. are document-local keys, not runtime node IDs. `input` and `output` name
graph endpoints. Runtime IDs are assigned again on load; controls must refresh their
node/context references after `graph.changed`. A node's `engine` references itself or a
root node for shared native effect contexts. Roots are restored first, then siblings with
independent state. If the original root interface was removed before saving, a surviving
sibling becomes the saved root. No history buffers or pointers cross this boundary.

The graph's channel routes and note connections are compiled from the wires on load, as
during editing. A wire port of 4294967294 or 4294967295 marks an end a plugin replacement
could not map (a main port awaiting one, or none); such a wire is kept and loads
unconnected.

Plugin identity is `(format, id)`, plus the observed version as state provenance.
`pathHint` locates an installed binary/bundle and is verified against that identity and
version; the plugin store (`OSCAIRN_DATA_HOME/Plugins/<format>/<id>@<version>/<bundle>`, RFC 0006,
where installing an archive puts the builds it carries) is tried next. It is **not** a package ID or portability guarantee. There are no silent upgrades
or untested state conversions: a plugin that is not here, or only in another version, or that
fails to load, opens as a **stand-in** in its place (RFC 0006's unavailable-node preservation).
The stand-in keeps the saved identity, ports and the parameters the snapshot and the document
name, so parts, wires, scenes and controls keep it; it plays silence, is reported among the
session's `failures` saying why, and writes its saved state back byte for byte on save, so
installing the plugin later restores it. A document that is malformed is still refused.

## Snapshot rules

- Native Basic Synth stores every prepared patch, name and all 16 channel selections,
  including accepted selections still queued at save time. Voices, pedal state and pitch
  modulation in progress are excluded. IDs are reconstructed, not persisted.
- Each node lists the sound assets its plugin declares as `assets: [{library, path, sha256}]`,
  and the top-level `libraries: [{id, name, fingerprint}]` names the libraries they come from.
  Before any plugin loads, every reference must resolve: by library and path, or by identical
  content in any registered library. Otherwise loading fails, preserving the live graph,
  with a message per sound and library such as `Piano: 641 of 641 files from library
  'Salamander' unavailable, e.g. …`. Moving the Performance folder does not move the samples;
  registering the same folder on another device is enough.
- The Sampler's `oscairn-sampler-2` snapshot stores every prepared SFZ patch (normalized) with
  its library, folder and the sample's SHA-256.
- Hosted instrument pools snapshot every prepared instance and the selected slot per
  channel. Restoring a pool checks every instance against the plugin's layout/parameters.
- Plugins with state support use their full state interface. LV2 asset links produced by
  saving are copied into the folder, so moving it does not depend on the original files.
- A plugin without a state interface saves its writable parameter values. This captures
  our parameter-only sequencer/MIDI effects, but cannot recover private state a third-party
  plugin does not expose. This fallback supports at most 2048 writable parameters and checks
  IDs, bounds and stepped values; unsupported snapshots fail instead of truncating.
- Samples, plugin state and each prepared context are captured sequentially. This is a
  patch/Performance snapshot, not a globally sample-synchronous capture of automation.
  Native context snapshots and non-concurrent state APIs briefly exclude plugin rendering;
  a racing callback may emit silence for that node. Do not promise seamless live saving.

The internal `snapshot` file starts with `state 1`, `parameters 1`, or `pool 1`.
`state` refers to a `state/` bundle. `parameters` contains one `id value` pair per writable
parameter, using locale-independent round-trip doubles. `pool` contains eight occupancy
flags with quoted names for occupied slots, then 16 selected slot indices; occupied slots
have recursive snapshots in directories `0` through `7`. These are host-owned records,
separate from the plugin's own state version. During initial development, incompatible
changes replace the format outright; development saves/fixtures are recreated, not migrated.
Unknown versions are rejected, preserving the current session and original folder. See the
[compatibility policy](developer-guide.md#initial-development-compatibility-policy).

## Sounds

A Sound ([RFC 0021](rfcs/0021-sounds-performances-and-library.md)) is a part saved without its
seat, in a folder with the same layout: `sound.json` and `nodes/`. It uses the same objects
and limits as a Performance, with `format` `oscairn.sound` and version 5:

| Field | Meaning |
| --- | --- |
| `format`, `version`, `info`, `libraries`, `lock` | As in a Performance |
| `nodes` | The part's plugins only, keyed `node-1` onwards; none may share an engine with a plugin outside the part |
| `wires` | The wires with both ends among those nodes: user wires, and the part's own wires for their Mix (the part's wires are rebuilt from its chain on load, taking those settings) |
| `part` | `chain` and `splits`, as in a Performance's part; no ID, name, input, output, fader or sends |
| `mappings` | Those with both ends among the nodes |
| `controls` | The controls the part owns, as in a Performance |

No `transport`, ports, scenes, pads or other parts. A Sound is loaded into a part of the open
Performance (`parts.loadSound`), never opened on its own. A Performance never reads the Sounds
its parts came from: each part is a full copy, so deleting a Sound from the library changes
no Performance, and only Save and Update from library look for it. There is no schema file for Sounds
yet; the reader checks every field.

## Publication and failure behavior

Saving requires committed graph edits and drained Session parameter commands. A sibling
staging directory is assembled and checked before rename publishes the final folder.
Exceptions remove staging data and leave existing saves intact. This provides atomic
visibility for normal filesystem failures; it is not an fsync/power-loss durability promise.
An abruptly killed process can leave a hidden `.oscairn-state-*` staging directory.

Loading parses and validates the complete document, creates new plugin instances, restores
all their state/assets, checks ports and compiles routes/cycle constraints before publication.
Missing plugins/assets, incompatible versions, invalid state or insufficient resources fail
without modifying the playing graph or the original Performance folder. A failed load reports
the failure instead of opening a partial graph with disabled placeholder nodes; that richer
missing-dependency workflow remains planned.

A successful load replaces the graph at an audio-block boundary. It starts stopped at beat
zero. It is seamless ([RFC 0021](rfcs/0021-sounds-performances-and-library.md#seamless-switching)):
the previous Performance's plugins come along, hidden, and ring out (held notes play on until
released, tails decay), each going once idle; notes queued before the change still play. The engine's absolute sample
clock keeps advancing, preserving the hardware MIDI timestamp domain. New node IDs avoid
stale controls addressing unrelated plugins. Old plans/plugins are reclaimed on the control
thread. Output-device settings and physical channel maps are host preferences, not saved
Performance data. Logical port layouts must match exactly; cue audio is never silently
folded into the master output.

The reader rejects duplicate/unknown fields, invalid references, duplicate part IDs or chain
entries, part wires of unknown parts, cycles,
path-traversal node keys, nonfinite numbers, missing state and symlinks/special files in the
bundle. Limits: 4 MiB manifest, nesting depth 32, strings 4096 UTF-8 bytes, 256 nodes,
4096 wires and mappings each, 256 parts, 256 audio channels, 16 note ports,
65536 filesystem entries and 16 GiB aggregate assets. Plugin state has its own bounded
reader. JSON Schema describes document structure; the engine additionally checks graph,
plugin, asset and real-time constraints that a schema cannot express. Load only trusted
Performance/plugin content: in-process plugin code is not sandboxed by JSON validation.

## Control API and checks

`performance.save` and `performance.load` each take `{"directory":"/absolute/path"}`.
They return `{"saved":true}` and `{"loaded":true}` respectively, or an error with the
failing plugin/reason. Load acknowledgement means publication, not proof the audio callback
has adopted it. The typed methods are `Control::savePerformance` / `loadPerformance`.
See [control-protocol.md](control-protocol.md).

`control-parts` saves and reloads parts, wires of both owners, Mix settings and a flagged
wire, comparing the model and rendered audio, and checks a version 4 document is refused.
`control-performance` checks sample-identical audio against a separately configured graph,
prepared native/hosted sounds, shared delay state, sequencer edits, load failures, queued
selections and concurrent publication with an audio allocation guard.
`control-performance-format` uses separate processes and a moved folder, exercises all eight
native contexts and malformed documents, and validates generated JSON against the schema
when Python `jsonschema` is available. Native socket tests exercise both remote methods.
The upstream LV2 sampler test checks copied sample assets and missing-asset isolation.

# Performance format, version 1

A Performance saves the graph, all prepared sounds and their MIDI-channel assignments,
plugin settings/state, explicit audio and note routes, parameter mappings, sequencer
parameters, tempo and clock source. **Save, reopen, import and export use the same data
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
its original sample library. Native asset-bearing patches and package-managed dependency
resolution remain planned in [RFC 0006](rfcs/0006-packages-and-portability.md).

Required top-level fields:

| Field | Meaning |
| --- | --- |
| `format`, `version` | Exactly `oscairn.performance` and integer `1` |
| `transport` | Tempo and `internal` or `midi` clock source |
| `audioInputs`, `audioOutputs` | Required logical graph ports: ID, channels, flags |
| `nodes` | Local key, display name, plugin identity/version, path hint, shared engine key, expected audio/note ports |
| `audio` | Exact source/destination node, port and channel, count and gain |
| `notes` | Source/destination note routes |
| `mappings` | Source node/lane to destination node/parameter |

`node-1` etc. are document-local keys, not runtime node IDs. `input` and `output` name
graph endpoints. Runtime IDs are assigned again on load; controls must refresh their
node/context references after `graph.changed`. A node's `engine` references itself or a
root node for shared native effect contexts. Roots are restored first, then siblings with
independent state. If the original root interface was removed before saving, a surviving
sibling becomes the saved root. No history buffers or pointers cross this boundary.

Plugin identity is `(format, id)`, plus the observed version as state provenance.
`pathHint` currently locates an installed binary/bundle and is verified against that
identity/version. It is **not** a package ID or portability guarantee. Version mismatches
fail explicitly; there are no silent upgrades or untested state conversions. Version 1
has no package manifest, dependency lockfile, binary payload, resolver, archive reader or
cross-format conversion. Those can wrap/reference this Performance data later without
inventing a second save format.

## Snapshot rules

- Native Basic Synth stores every prepared patch, name and all 16 channel selections,
  including accepted selections still queued at save time. Voices, pedal state and pitch
  modulation in progress are excluded. IDs are reconstructed, not persisted.
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

## Publication and failure behavior

Saving requires committed graph edits and drained Session parameter commands. A sibling
staging directory is assembled and checked before rename publishes the final folder.
Exceptions remove staging data and leave existing saves intact. This provides atomic
visibility for normal filesystem failures; it is not an fsync/power-loss durability promise.
An abruptly killed process can leave a hidden `.oscairn-state-*` staging directory.

Loading parses and validates the complete document, creates new plugin instances, restores
all their state/assets, checks ports and compiles routes/cycle constraints before publication.
Missing plugins/assets, incompatible versions, invalid state or insufficient resources fail
without modifying the playing graph or the original Performance folder. Version 1 reports
the failure instead of opening a partial graph with disabled placeholder nodes; that richer
missing-dependency workflow remains planned.

A successful load replaces the graph at an audio-block boundary. It starts stopped at beat
zero, discards pre-load queued UI notes, and cuts the previous Performance's held notes and
effect tails. It is not a seamless Performance transition. The engine's absolute sample
clock keeps advancing, preserving the hardware MIDI timestamp domain. New node IDs avoid
stale controls addressing unrelated plugins. Old plans/plugins are reclaimed on the control
thread. Output-device settings and physical channel maps are host preferences, not saved
Performance data. Logical port layouts must match exactly; cue audio is never silently
folded into the master output.

The reader rejects duplicate/unknown fields, invalid references, duplicate routes, cycles,
path-traversal node keys, nonfinite numbers, missing state and symlinks/special files in the
bundle. Limits: 4 MiB manifest, nesting depth 32, strings 4096 UTF-8 bytes, 256 nodes,
4096 audio channel routes/note routes/mappings each, 256 audio channels, 16 note ports,
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

`control-performance` checks sample-identical audio against a separately configured graph,
prepared native/hosted sounds, shared delay state, sequencer edits, load failures, queued
selections and concurrent publication with an audio allocation guard.
`control-performance-format` uses separate processes and a moved folder, exercises all eight
native contexts and malformed documents, and validates generated JSON against the schema
when Python `jsonschema` is available. Native socket tests exercise both remote methods.
The upstream LV2 sampler test checks copied sample assets and missing-asset isolation.

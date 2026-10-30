# RFC 0009: Sound assets and dependencies

Status: steps 1–2 implemented 2026-10-30 (see [Implementation status](#implementation-status));
steps 3–4 planned. This is the concrete asset contract that [RFC 0006](0006-packages-and-portability.md)
leaves as "the planned host asset API" and "assets must be declared by the plugin".

## Outcome

A sound depends on plugins, as today, and also on **files those plugins read**: samples,
waveforms, impulse responses, tunings, models. Every plugin declares and resolves those
files the same way. A Performance lists them as dependencies, never copies them into its
own folder, and opens only when all of them are present with the expected content. A
library used by many sounds or many plugins is stored once.

## Terms

| Term | Meaning |
| --- | --- |
| **Asset** | One immutable file, identified by the SHA-256 of its bytes. Never edited in place. |
| **Library** | A named, versioned collection of assets with a manifest: ID, version, title, authors, license/attribution, redistribution terms, and each file's path, size and hash. A library is a kind of RFC 0006 package. |
| **Asset reference** | `{library, version, path, sha256}`. The path is relative to the library root. Stored in plugin state and in the Performance manifest. Never an absolute path. |
| **Sound** | A plugin identity plus its state plus the asset references that state uses. |
| **Dependencies of a Performance** | Its plugins plus the union of its sounds' asset references. |

Asset identity is the file's bytes; library identity is provenance. Two libraries that ship
identical bytes share storage but keep separate manifests, licenses and authorship.

## Where files live

Nothing is copied into a Performance. Files come from one of three places, all managed by
the host, not by any plugin:

1. **Installed libraries** in the RFC 0006 shared store: downloaded packs, factory content,
   libraries brought along by an imported Performance. Each byte-identical file is stored once.
2. **Registered folders**: a folder the musician already has, such as a downloaded piano,
   added like a plugin folder and referenced in place without copying. The host indexes it
   into a library manifest (hashes computed once, at registration). Moving or deleting the
   folder makes it a missing dependency, the same as uninstalling a plugin.
3. **The user library**: new files created by sounds (recordings, resampled audio, edited
   wavetables, converted Yamaha user waveforms) or single files imported on their own.
   Edits create a new asset; the old one remains while anything references it.

Export writes a package containing each required library once, subject to its
redistribution terms (RFC 0006). A library that may not be redistributed, such as
Yamaha factory content or a commercial pack, is exported as a reference the destination
must already satisfy, and the export reports it.

## Files used by several plugins

Assets belong to the host and are not tied to any plugin. Our Sampler, sfizz, a drum
machine and a granular plugin can all reference the same library, and its files are on
disk once. Which plugins can use a file is a plugin capability (accepted media types and
patch formats), not something the library records.

Disk is shared; memory is not automatically shared. Each plugin decodes into its own
memory unless a later host service hands out shared, read-only decoded buffers. Today the
Sampler shares decoded audio only between its own contexts.

Plugin-specific derived data, such as decoded PCM, wavetable mipmaps, compiled patches or
neural-model caches, is a disposable cache keyed by asset hash plus plugin ID and version.
It is never an asset, never exported, and can always be rebuilt.

## Plugin contract

Two new Oscairn extensions, alongside `oscairn.sound-contexts`:

- **Host extension `oscairn.assets`**, called by the plugin on its main thread:
  - `resolve(reference) → local path`: a verified, readable file path, or failure with a reason.
  - `import(path) → reference`: register a file the musician picked into a registered
    folder or the user library.
  - `create(bytes or temporary file) → reference`: store a new file the plugin produced.
- **Plugin extension `oscairn.asset-dependencies`**: list the asset references used by a
  prepared context or by the whole state, so the host can write the manifest, check
  availability before loading, package exports and keep referenced files from being collected.

Plugin state stores asset references only. Preparation receives references, not a
directory: `prepare(patch, base reference)` replaces draft 3's `directory`, and relative
paths in the patch resolve within that library.

Other plugin formats use the same store:

- **LV2:** the host's `LV2_State_Map_Path`/`Make_Path` map abstract paths to library
  references, so existing LV2 plugins participate without changes (the adapter already
  collects mapped paths today).
- **CLAP `resource-directory` draft:** an adapter for third-party CLAP plugins that
  implement it. The shared directory is a store view; files they collect become user-library assets.
- **Ordinary CLAP state:** opaque. The host cannot discover its files, so the sound is
  marked "dependencies undeclared" and export follows RFC 0006's rules for unknown assets.

## Loading and verification

Before building a graph, the host resolves every asset reference of every sound. If any
are missing or changed, nothing is replaced. The report names the sound, library and files
(for example, "Piano: Salamander Grand Piano V3 not installed (641 files)"). Later, RFC 0006
unavailable-node placeholders can open the rest of the Performance.

Hashing a multi-gigabyte library on every load is too slow, so:
- the host hashes at install, registration or creation time and keeps an index of
  (path, size, modification time) → SHA-256;
- loading checks the index and re-hashes only files whose size or time changed;
- a full verification is an explicit maintenance operation.

## Which plugins provide or need files

| Kind | Files | Shared across plugins? |
| --- | --- | --- |
| Samplers, ROMplers, drum machines | WAV/FLAC samples, SFZ, SF2/SF3, Hydrogen kits, DecentSampler | Yes: SFZ/WAV libraries play in several samplers |
| Workstation conversion (MODX M) | User waveforms, arpeggios, curves, microtunings, motion sequences | Yamaha already stores these as shared library items that Performances reference |
| Wavetable synths | `.wav`/`.wt` wavetables | Often (Surge XT, Vital-style tables) |
| Convolution reverbs, cabinet simulators | Impulse responses | Yes: IR packs are used by many convolvers |
| Amp/neural models | `.nam`, AIDA-X models, RAVE/neural audio models | Yes: several hosts load the same model files |
| Tuning | Scala `.scl`/`.kbm`, MTS-ESP | Yes: one tuning for every instrument in a Performance |
| Phrase and arpeggio players | MIDI files, grooves, pattern libraries | Often |
| Granular synths, loopers | Audio files and recordings | Sometimes |
| Script/patch hosts | Faust, Pure Data, Lua, JSFX sources | Rarely |

## Patch file extensions

The extension says which player can open the file:

- **`.sfz`**: a patch that uses only standard SFZ opcodes, including everything in the
  current `oscairn-sfz-baseline-0` profile. Other SFZ players can open it, and it stays
  `.sfz` on export.
- **`.osfz`**: a patch that requires at least one Oscairn extension (RFC 0008 step 4).
  Ordinary SFZ players silently ignore unknown opcodes, so a distinct extension keeps them
  from playing such a patch wrongly. Exporting to plain SFZ is a deliberate, reported
  conversion, never a rename.

Neither file contains library IDs or absolute paths: `sample=` stays relative to the
library root, and the binding to a library lives in the sound's asset references. A
`.sfz` file therefore remains a valid SFZ file inside its library.

## Changes to what exists

- **Sampler snapshot:** today it stores an absolute directory and a 64-bit FNV hash of the
  decoded audio. Replace these with asset references (SHA-256 of file bytes). Keep the
  in-memory FNV hash only for deduplicating decoded audio within one instance.
- **`prepare`:** `directory` becomes a base asset reference.
- **Desktop import:** importing a `.wav`/`.sfz` file registers its folder (or adds the file to
  the user library) before preparing.
- **Performance manifest:** gains `dependencies.assets` (library ID, version and referenced
  files per node). The format version changes, and development saves are recreated
  rather than migrated.

## Build order

1. Store index, library manifest and registered folders; hashing with an index; host-side
   `resolve`/`import` behind the control API.
2. The `oscairn.assets` and `oscairn.asset-dependencies` extensions, used by the Sampler;
   manifest dependency list; transactional preflight with a precise missing-asset report.
3. LV2 path mapping onto the store; `create` for user-library files (first use: Yamaha
   user waveform conversion).
4. Export and import of libraries as RFC 0006 packages, with redistribution checks and
   garbage collection from explicit roots.

## Decisions on identity, overlap and memory

Decided by Jacob, 2026-10-29.

**Registered folders get a generated ID and a content fingerprint.** The fingerprint is a
hash over the folder's file hashes. When a Performance needs a library whose ID is not
present, the host looks for a registered or installed library with the referenced file
hashes and offers it as a match, which the musician confirms. The same piano registered
separately on two devices therefore resolves, without guessing from names. A name or
version found in the folder is used only for display.

**References resolve by content hash first.** Any library holding a file with the referenced
SHA-256 satisfies the reference; the library ID and path are provenance, used in reports
and for licensing and export. The same bytes may appear in several libraries, for example
the user library and a registered folder. Single-file import copies into the user library,
so later cleaning up the original folder cannot break a sound. A registered folder is used
in place and never moved or deleted by Oscairn.

**No shared decoded-audio service for now.** Disk storage is shared; each plugin decodes into
its own memory, and the Sampler shares decoded audio only between its own contexts.
Streaming large libraries (RFC 0008 step 3) reduces memory per plugin more than sharing
would, and a cross-plugin service would face differing sample formats, cross-plugin
lifetime tracking, worker-process isolation, and plugins that would not use it. Revisit
only if a measured Performance on target hardware runs out of memory because different
plugins load the same library.

## Implementation status

Steps 1 and 2 are implemented and tested (`assets`, `sampler`, `desktop-sound-picker`):

- [`oscairn::assets::Store`](../../core/plugin-host/include/oscairn/assets.h) keeps
  `libraries.tsv` (ID, fingerprint, path, name) and one `index/<id>.tsv` per library (size,
  modification time, SHA-256, path) under the data directory given in the
  [control protocol](../control-protocol.md#sound-assets). Registering hashes every regular
  file once (at most 200,000; links skipped). Lookups re-hash only changed files, check paths
  stay inside the library (no `..`, links or escapes), then fall back to identical content in
  any library. Registry changes re-read the file under an exclusive `flock`, so engines in
  several processes keep each other's registrations; indexes are caches and may be rebuilt.
- Single-file imports go to the `user` library as `<first 16 hex digits of SHA-256>/<name>`.
- The host extension `oscairn.assets/draft-1` and plugin extension
  `oscairn.asset-dependencies/draft-1` are in [`oscairn-clap/assets.h`](../../core/clap-ext/include/oscairn-clap/assets.h);
  `oscairn.sound-contexts/draft-4` passes the patch's base reference. The Oscairn Sampler
  uses both and no longer reads directories itself.
- Control: `assets.register`, `assets.import`, `assets.libraries`, and `sounds.prepare` with
  `library`/`directory`. The desktop picker copies an imported WAV into the user library and
  registers an imported SFZ's folder.
- Performances record `libraries` and per-node `assets`, and check every reference before any
  plugin loads ([format](../performance-format.md#snapshot-rules)). A test reopens a saved
  Performance in a fresh store: it is refused, naming the library, until the folder is
  registered under a new ID, and then plays identically.

Not done: LV2 path mapping and `create` (step 3), packages, export and garbage collection (step
4), a desktop screen for managing libraries, and fingerprint matching of whole libraries; files
resolve individually by content, which already covers a folder registered again elsewhere.
Registration runs on the engine's control thread and blocks other control requests while a
large folder is hashed.

## Ruled out

- Copying assets into each Performance: too much storage.
- Absolute or plugin-private paths in saved state.
- Identity by file name or path alone: an edited file must not reuse stale data.
- CLAP `resource-directory` as the primary mechanism: it is per-instance and copy-based.
  It is kept only as an adapter for plugins that implement it.

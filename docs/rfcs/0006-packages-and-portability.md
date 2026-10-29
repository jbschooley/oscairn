# RFC 0006: Packages and cross-device portability

Status: proposed. This is the packaging and transfer contract to build toward, not an
implemented installer or project format. It refines [Portable sounds](../architecture.md#portable-sounds)
and [Packages](../architecture.md#packages-sound-packs-libraries-and-plugins).

## Outcome

A musician exports a Performance or Setlist, imports it once on another Oscairn system,
and gets its sounds without running an installer for every managed plugin. The transfer
includes or resolves the plugins, their shared libraries, presets and sample assets.
Offline transfer works when the export includes everything required by the chosen target.
The import reports exactly what will play before activation. Unsupported dependencies keep
their state and place in the graph; nothing is silently replaced or forgotten.

This guarantee applies to a **complete, compatible managed dependency set**, not every
third-party plugin someone can load. Conventional plugins with vendor installers, licensing
services, undeclared files or no target build can be used as external dependencies, with
those limits visible before export and on the destination.

Today Oscairn loads CLAP and optional LV2 by path/format identity. It exports individual
plugin state; the LV2 adapter collects assets registered through state path mapping, while
the CLAP adapter saves an opaque stream and plugin ID. Neither proves that all third-party
assets have been found or that state works on another OS. There is no package resolver,
complete Performance/Setlist persistence, AU/VST3 adapter or cross-format state converter.
No changes to the audio graph are required for this proposal: package resolution and state
preparation belong to library/control services, before a ready graph is published.

## Oscairn-native and external plugins

**Oscairn-native** is a portability/conformance contract, not a new binary format, a synonym
for CLAP, or a restriction to first-party authors. A community or commercial developer can
supply a native engine. Required capabilities are declared package dependencies, stable
engine/control identities, a versioned portable state/asset contract, tested builds for each
advertised destination profile, and ordinary plugin operation outside Oscairn. Native
instruments also implement the prepared/multitimbral sound contract; format wrappers may
expose only what their host supports. Native effects/event plugins have their own applicable
contracts, not an instrument-only voice interface.

The product UI distinguishes **Oscairn-native** from **External**, and separately reports
readiness for a selected destination. Native is not a promise of a binary for every possible
machine or unlimited resources. A native plugin with no suitable build/assets, an unsupported
host capability or too little device capacity is unavailable there too. The current
`sounds.get.mode: "native"` reports only the sound-context extension; it is not proof of
package portability and must not be reused as that badge.

External/non-native plugins have no default portability guarantee. They work on a destination
only when a compatible same-format installation is present (or explicitly installed there)
and can restore the saved state/assets, or when an approved tested conversion has a usable
destination implementation. Packaging an external binary does not promote it to native.
Otherwise disable the unavailable node, show the reason, preserve its state/routes, and mark
the affected performance not ready. Do not silently bypass the node. Other complete songs
remain usable; a user can deliberately save a reduced/substituted version of the affected one.

## Project manifest, lockfile and shared store

Adopt the developer experience of a package manifest plus lockfile: a project declares direct
packages and version requirements, and Oscairn resolves their transitive dependencies. The
user normally manages this through the UI, with readable files and equivalent tooling for
advanced use. No JavaScript runtime, npm registry dependency or package install scripts are
required. Exact filenames/CLI syntax belong to the schema/tooling milestone.

- **Manifest:** requested packages/version constraints and external plugin requirements.
  Native package dependencies and external plugin requirements are separate categories;
  external requirements are not automatically treated as retrievable native packages.
  The library derives this from the full saved graph, including inactive scenes and parts.
- **Lockfile:** exact resolved dependency graph, immutable release/artifact hashes, target
  variants, state contracts and validated conversion choices. Reopening/installing from a
  lock never chooses a newer release. Manifest/lock mismatch requires an explicit resolve
  or update operation; it does not silently rewrite a performance on open.
- **Project-local dependency view:** each project resolves only its own locked dependencies
  through a small index or materialized layout; audio loading never searches for an arbitrary
  globally installed version. Distinct project versions can coexist.
- **Shared durable store:** identical package files/versions across projects refer to stored
  content, using links/clones where suitable. This is installed content pinned by references,
  not a disposable download cache. Removing a download cache must not break an offline rig.
  Optional fetched archives/cache entries are separate and can be discarded when their
  verified installed contents remain available.
- **Online install:** reuse verified local content, acquire only missing target artifacts
  from configured sources, then stage and publish the locked dependency set. Record selected
  package sources/publishers; do not search unrelated catalogs for a same-named replacement.
- **Offline install:** resolve the same lock using the local store plus an imported complete
  archive, removable-media package repository or paired computer's prepared transfer. Missing
  content is reported precisely; no downgrade or best-effort newer-version substitution.

This combines npm-style declared dependencies and locked installs with a shared installed-file
store closer to [pnpm's layout](https://pnpm.io/faq). The
[npm cache](https://docs.npmjs.com/cli/commands/npm-cache/) is documented as disposable, while
[`npm ci`](https://docs.npmjs.com/cli/commands/npm-ci/) illustrates installing a frozen lock.
Oscairn's store pins and offline-completeness checks are essential additions for live devices.
The resolution process stays entirely off the real-time thread.

## Separate four identities

| Identity | Purpose |
| --- | --- |
| Engine/product identity | Publisher-scoped logical instrument/effect, independent of format, filename and CPU |
| Format identity | Actual CLAP descriptor ID, LV2 URI, VST3 class ID or AU component identity used to instantiate it |
| Package identity | Publisher, package ID, release version and immutable manifest/content digest |
| State contract | Versioned representation the engine understands, plus the originating format/build and any tested migration |

A package can contain several plugins and several platform builds. A product can have
several format identities, but a publisher's association is not proof that their states
are interchangeable. Unknown third-party products retain format-qualified identities;
Oscairn must not invent equivalence from names, filenames or parameter counts.

Saved documents record logical references and an exact dependency lock, not installation
paths. Discovery resolves those references to local paths. The lock records concrete package
releases/digests, format IDs, state revisions and chosen target artifacts. Build variants for
other destinations come from the same authenticated release manifest, or an explicitly
approved migration. A compatible version range aids resolution but never silently upgrades
an existing locked performance. Missing exact releases are reported rather than replaced by
whatever version happens to be installed. Unmanaged plugins retain observed identity/version
and available fingerprint information; weak identification is reported as such.

## One import, explicit completeness

The normal library file is lightweight: graph/state/metadata plus asset and dependency
references. Copying that file alone is not the same as collecting its dependencies.
The UI offers **Export for another device** on Sounds, Performances, Setlists and Projects:

1. Collect the transitive closure: all songs, inactive scenes, layers, effects, MIDI
   processors, prepared sounds, plugin runtime dependencies and required assets. It must
   not collect only the currently sounding patch.
2. Select a known destination profile, or select multiple target profiles for an offline
   travel bundle. A paired device can report its installed hashes and runtime capabilities.
   An unknown destination cannot be promised universal compatibility.
3. Resolve exact builds, state compatibility and permissions to include files. Show total
   size, reusable installed content, material that must be acquired separately, and any
   missing/unsupported dependency. Fetching from a catalog is optional; no downloads occur
   on the audio path or unexpectedly during performance activation.
4. Export the existing bundle model with the document, lock, state, permitted packages and
   assets. A complete offline bundle contains the target dependency closure beyond the
   declared platform baseline. A references-only export is labelled incomplete for offline
   use unless the destination already has everything. An unavailable download is not an
   included dependency. A multi-target bundle includes each selected available binary build;
   it does not synthesize new ones.
5. Import shows one consolidated plan for added code/content and unresolved items. Stage,
   verify, resolve and scan away from live processing; publish atomically after success.
   No per-plugin installer dialogs or package-provided installation scripts for managed
   packages. Cancellation/failure preserves existing installations and the live performance.
   A document may be imported with unresolved placeholders without installing unavailable
   code; that successful data import does not make it ready to play.
6. Preflight the actual destination: state restoration, ports and host extensions, required
   assets, device I/O mappings, sample rate/block constraints and CPU/RAM/storage budgets.
   Only a ready performance is eligible for activation. Installing successfully is not a
   real-time capacity guarantee.

Default catalog packages work from their files without activation services. External
commercial plugins remain subject to their vendor's installation/activation and transfer
terms; Oscairn does not remove these requirements. Distributable managed packages can
travel in the bundle. Restricted packages can be references or be acquired through a
permitted channel; an export cannot claim completeness when they are absent. A paired
computer can assemble packages for an offline device; a network account is not mandatory.

Built-ins satisfy dependencies only when the destination's versioned platform profile
actually supplies compatible releases. Device manufacturers' extras are ordinary packages,
not universal built-ins. Permit their inclusion when available and permitted; otherwise
report the gap. Do not categorically omit source-device built-ins that the destination lacks.
The baseline profile promises defined plugin/state capabilities on its supported targets,
not unlimited polyphony or arbitrary graph size.

## Self-contained packages without duplicating everything

"Self-contained" means the package's complete runtime requirements are declared and
resolvable from its dependency closure plus a versioned OS/runtime baseline. It does not
mean that every plugin archive physically contains its own copies of every dependency.

For example, a developer's organ and rotary effect can both depend on `vendor.dsp-runtime`
version 2 and `vendor.cabinet-data` version 1. The relevant target build of the runtime and
the architecture-independent cabinet data install once; both plugins refer to them. A new
runtime major version can coexist with version 2 without breaking saved performances.
Factory sample libraries and user content follow the same model.

- Store immutable files by cryptographic content hash. Deduplicate identical bytes across
  packages and exports; keep manifests, authorship, licenses and package boundaries even
  when the bytes coincide. Different OS/CPU builds have different bytes and occupy storage.
- Shared **code** is an explicit versioned runtime dependency, built for the destination
  ABI. Sharing a source repository alone does not deduplicate code statically compiled
  differently into multiple binaries. The host does not extract common instructions from
  arbitrary plugins. Small static runtimes remain a reasonable developer tradeoff.
- Materialize normal plugin/bundle layouts from stored files, using safe immutable links,
  filesystem clones or copies as supported. Preserve native signed bundle structure,
  permissions and required metadata; do not rewrite signed binaries to change paths.
  Deduplication is an optimization, not a reason to invalidate a signature or load a
  different dependency. Physical storage savings depend on filesystem/layout constraints.
- Resolve libraries within a pinned package/runtime scope, using declared loader paths and
  versioned ABI names. Do not install arbitrary replacements in global system directories
  or rely on global `PATH`/library-path edits. Reject unresolved transitive libraries. When
  two required library versions cannot safely coexist in one loader namespace, isolate
  instances in separate workers or reject that combination until isolation is available.
- Distinguish storage from RAM: OS mappings can share immutable code pages, and cooperating
  plugins can share immutable samples through the planned host asset API. Independently
  allocated or decoded samples, voices, delay lines and other mutable DSP state are private.
  No blanket "loaded once" memory promise for arbitrary plugins or across worker processes.
- Pins come from installed packages, saved documents, backups as configured, prepared
  setlists and live instances. Garbage collection must not remove a referenced release.
  Updates install side by side; rollback selects the old lock. Garbage collection is an
  explicit storage operation, never work on the audio callback.

Assets must be declared/exported by the plugin or integration. For an opaque third-party
sampler, copying its preset blob does not discover every file it might open. Include the
whole required library if its working subset cannot be established and inclusion is
permitted, or mark the export unresolved. Do not label factory-library references "small
and complete" unless those libraries are also included or confirmed at the destination.

## Platform builds and hardware

A code artifact declares OS, CPU architecture and required instruction set, runtime/ABI
profile and minimum supported versions, plugin format, required host features, and relevant
DSP/audio limits. Optional GUI resources/dependencies are separate where the plugin permits
headless operation. Generic host-generated controls cannot rescue DSP that itself requires
an unavailable desktop service. Resource estimates are input to measured preflight, not a
substitute for it.

KICK/Ignite targets use their actual Oscairn Buildroot SDK/profile, not merely the label
"ARM64 Linux". Linux desktop plugins may require an incompatible libc/C++ runtime or
unavailable libraries. macOS ARM64 and Linux ARM64 are different targets despite sharing
an instruction set. Windows x64/ARM64 also need matching host-process ABIs. Native builds
are selected from packages; runtime translation, Wine, arbitrary binary recompilation and
running a macOS AU on Linux are outside this guarantee.

First-party portable engines must provide a CPU implementation for each supported Oscairn
profile and an architecture-independent state/asset contract. Optional DSP/core backends
use the same logical engine and validated state as described in
[RFC 0004](0004-hardware-services.md). Backend-specific state needs a tested migration;
lack of an accelerator must not silently remove a feature or change a sound. Resource or
quality reductions require an explicit user policy. Source and build recipes help maintain
ports but do not guarantee a missing binary can be produced at import time.

| Transfer | Resolution |
| --- | --- |
| Linux KICK/Ignite CLAP sound → Mac or Windows | Select that engine's destination build, restore compatible state and resolve the same assets; refuse if any is unavailable |
| Linux LV2 sound → Mac or Windows with that LV2 available | Keep LV2 and its plugin URI; select the target build and verify state/assets/host features; Windows hosting itself is still planned |
| Linux LV2 sound → only the vendor's VST3 is installed | Requires an explicit tested LV2→VST3 state and control/port migration; not automatic |
| Mac AU sound → Windows or Linux hardware | Use a declared compatible non-AU implementation plus tested migration, or preserve an unavailable-plugin placeholder |
| VST3 on hardware → VST3 on desktop | Requires a hardware-compatible Linux build and host adapter first, then the target desktop build, matching class identity and compatible state/assets |
| Same OS/CPU, another laptop | Managed dependencies arrive/reuse installed artifacts in one import; external vendor installations may still be necessary |

LV2 is already an optional Buildroot host feature. VST3 and AU hosting remain planned.
LV2 and VST3 can both be packaged as managed dependencies. Their bundle formats alone do
not prove the binary's external library closure or state portability. A Windows/Mac VST3
installation does not supply a Linux hardware binary, and an LV2 instance is not the same
format identity as the vendor's VST3 instance.

## State portability and format migration

Prefer the same engine, format and compatible release on the destination. A plugin save/load
API or stable ID is necessary but not sufficient evidence of portability. The transfer
record retains the original state bytes, originating format/build, schema/version when
known, referenced assets and the host-side graph/control mappings.

For managed first-party engines, define a canonical versioned state representation that
all supported CPU builds use. Alternate format adapters may share it only if tested; no
new wrapper format is required merely to package a CLAP or LV2 plugin. For external plugins,
use their native opaque state unless the vendor or an audited integration provides a
conversion contract. Do not reverse-engineer a universal parameter-only approximation:
parameters can omit modulation, sample maps, tuning and other private state.

A migration must name exact source/destination identities and supported versions, translate
state, asset references, parameter IDs/units, automation/macros, audio/note ports and expression
capabilities, and pass restore/render comparisons. A format change can lose a required
feature even when the preset loads. Report such a conversion as lossy and require an explicit
alternative, rather than marking it equivalent. Pre-approved, validated mappings may resolve
automatically with the choice recorded in the destination lock; otherwise review is required.
Never choose by display name, and never pick an arbitrary newer release.

Migration creates a destination revision without destroying the original. Keep provenance
and the original state so the project can return to its source platform. If an imported
alternative is edited, cached states for other formats are stale; they must be regenerated
by a supported conversion or marked unavailable. Reusing an old AU snapshot after edits to
its VST3 replacement would silently undo the musician's work. Round-trip conversion is a
separate tested capability, not implied by one-way import support.

## Native plugins in DAWs and standalone applications

Native plugins must not require the Oscairn application or a running Oscairn background
engine just to make sound in another host. There are three distinct delivery paths:

| Use | Planned delivery |
| --- | --- |
| One native instrument/effect/event processor in another DAW | Ordinary CLAP and VST3 builds for supported Windows/macOS/Linux targets; AU builds for macOS hosts needing AU |
| A complete Oscairn graph/Performance inside a DAW | Oscairn itself as a standard plugin, preserving routing, sound contexts and dependency lock within its saved state |
| Use without a DAW | Oscairn standalone; optionally an individual plugin with a small standalone audio/MIDI shell |

First-party distribution targets CLAP plus VST3 on supported desktop profiles and AU on
macOS, with reusable SDK packaging for third-party native authors. Compatibility is per
host/format/plugin category, not a promise that every DAW accepts every format or can route
MIDI effects. Other formats, including AAX-only hosts, require separate work and are not
implicitly covered. Hardware builds use the required engine/DSP artifact without bringing
unneeded desktop-format wrappers or UI dependencies.

Prefer a common engine/state implementation with thin format adapters. Evaluate existing
[clap-wrapper](https://github.com/free-audio/clap-wrapper) before writing our own bridges;
it provides CLAP-to-VST3/AU and standalone paths. Its existence is not proof that every
Oscairn extension, UI, bus arrangement or note feature survives conversion. No wrapper is
adopted or integrated by this proposal. Preserve upstream credits if used.

Oscairn extensions are optional to the external host. Ordinary notes, audio, parameters,
state and supported transport must work without those extensions. Host-specific features
such as parameter-target routing, prepared sound selection and hardware services require
explicit support or remain unavailable; a wrapper must not pretend to provide them. An
engine must use its declared CPU backend outside specialized hardware. Asset access needs a
read-only package-backed implementation supplied by the plugin/SDK when the DAW does not
provide Oscairn's asset service. The generic/generated editor or packaged UI must function
without assuming the Oscairn app's panel or its private control socket exists.

One package-manager operation can also install/publish the requested DAW formats in their
standard discovery locations, preserving signatures and OS registration requirements.
Those exports reference or carry the same pinned engine/runtime/assets. Use immutable
links/clones where supported, with valid copies where necessary; exact zero duplication is
not promised for every signed bundle or filesystem. An external DAW cannot be expected to
understand Oscairn's project-local package view or perform its dependency resolution.

State saved by a native format adapter must retain canonical sound state, engine/state
version and explicit asset requirements; preserve port/parameter identity across compatible
updates. A compatible adapter may dispatch to a pinned engine version. If an adapter cannot
load old state safely, retaining the old package in the store alone is insufficient: keep
an actually loadable compatible adapter or report the conflict. Generic DAWs often identify
plugins by format ID, not package hash, so installing several versions with the same format
ID is not a solution by itself. Never silently use the newest engine for an old DAW session.
Published DAW exports pin their runtime versions until explicitly retired; do not rely on
finding every saved project owned by another DAW before garbage collection.

The Oscairn container can embed its complete internal dependency lock and export its assets.
A native plugin loaded directly into another DAW cannot automatically collect that DAW's
whole session or unrelated third-party plugins. Supply native sound/package export for its
own dependency set, with missing dependencies reported before it processes audio. A DAW's
normal project file does not automatically become a complete Oscairn offline bundle.
Installation/download/upgrade tasks happen outside the audio callback, never as a hidden
consequence of pressing Play.

Before claiming DAW support, test direct native plugins and the Oscairn container separately:
scan/load, editor lifecycle, save/reopen on a clean destination, offline render, transport,
automation, audio buses, note/expression and MIDI-output behavior where supported, samples,
old-state/version restoration and missing-dependency failures. Include a host with no
Oscairn extensions and platform-specific standard-format hosts. The current reference
plugins are CLAP modules; VST3/AU exports, container and individual standalone packaging are
not implemented or DAW-certified by this document.

## Missing plugins and playable alternatives

The saved graph remains intact with unavailable-node placeholders preserving identity,
state, routes, automation and dependencies. The preflight report distinguishes a missing
package from no target build, incompatible runtime, unsupported format/host feature,
unconvertible state, unavailable asset, external activation, and insufficient resources.
Opening/editing the document is allowed; the affected performance is not reported ready.

Do not silently bypass an unavailable effect, reroute a cue bus or replace an instrument
with a different synth. A user can deliberately save a device-specific substitute and
retain the original. Future optional rendered stems or multisampled versions can be
prepared on a working source machine and exported as explicitly labelled alternatives:
a stem cannot replace an interactive instrument, and sampling does not preserve arbitrary
modulation or MIDI behavior. Those alternatives are not the first package milestone.

## Installation boundary

Import is a data operation until native code is approved for installation/loading. Show
publisher/provenance, dependencies, target builds and permissions in one plan. Verify both
hashes and configured publisher trust; a hash alone does not establish authorship. Preserve
OS code-signing and registration requirements. Do not silently execute vendor installers,
post-install scripts, plugin scanners in the UI process, or privilege escalation.

Managed CLAP/LV2/future VST3 libraries use Oscairn-owned discovery roots and private writable
per-instance/user state. Existing OS installations may satisfy compatible external
references without being moved or modified. AU hosting respects Apple's component/app
extension registration model; an AUv3 extension inside its containing app cannot simply
be repackaged as a generic Linux/Windows library. Such distribution requirements can keep
an AU an external dependency even on macOS.

Untrusted archives need path/link traversal checks, declared size/count limits, duplicate
path rejection, signature verification where required and crash-contained scanning. Runtime
plugin isolation is separate planned work; currently a loaded plugin can still crash the
engine process. Installation and package loading must not claim sandbox guarantees before
those workers exist.

## Implementation sequence and completion checks

1. **Before discovery/project persistence becomes permanent:** carry format-qualified IDs,
   package/artifact references and state provenance through discovery/library design. Keep
   path-based development loading, without baking absolute paths into the saved document
   identity. Define the first target runtime profiles, native/external capability labels,
   project manifest/lock and shared-store resolution schema with fixtures.
2. **First vertical slice:** managed CLAP packages, one explicit shared runtime and sample
   dependency, exact version locks, side-by-side immutable installation, offline export/import
   and unavailable-node preservation. Demonstrate a setlist on a clean second supported
   machine with no plugin-specific installers. A local package directory is enough; no
   online catalog required. Test cache removal without losing pinned offline installations
   and two projects sharing a release while another pins an older one. Avoid inventing
   another plugin ABI.
3. **Cross-platform proof:** first-party engine/state tests on supported macOS, Windows and
   Buildroot Linux builds as those hosts exist. Test Linux ARM64 through available QEMU
   coverage initially and real hardware when available; test a clean runtime image without
   undeclared desktop libraries. Restore every saved part/scene; compare renders with
   explicit tolerances, event behavior and resource measurements. Do not promise bit-identical
   floating-point DSP across CPUs without evidence.
4. **Broaden integrations:** LV2 packaging/state assets, existing-install discovery and
   audited third-party manifests; VST3 and AU only alongside their host adapters. Add one
   verified cross-format migration and a negative AU-only case before advertising it.
5. **DAW delivery proof:** one native engine as direct CLAP/VST3/AU where supported,
   outside the Oscairn app, using the same state/assets; then the Oscairn graph container
   and optional standalone shell. Validate old DAW-session restore against the shared
   store without assuming the DAW understands package locks.
6. **Distribution/UI polish:** catalog downloads, paired-device deltas, storage cleanup,
   migration tools and optional rendered alternatives use the same manifest and lock.

Required tests include clean/offline import of an entire setlist; one-copy shared resources;
conflicting runtime versions; missing transitive libraries; wrong CPU/runtime/features;
interrupted/corrupt import; old-project restore after update; unavailable-plugin re-export;
samples moved from their original paths; stale alternate-format state after edits; and a
smaller device refusing an over-budget performance before activation. For unknown asset or
state portability, the honest result is an unresolved dependency, not a green checkmark.

## Format references

These establish format constraints; the package/resolution policies above are Oscairn's
proposal, not features supplied automatically by these standards.

- [CLAP state extension](https://github.com/free-audio/clap/blob/main/include/clap/ext/state.h):
  plugin-defined save/load streams, including non-parameter state.
- [LV2 bundle layout](https://lv2plug.in/pages/bundle-definition.html) and
  [filesystem standard](https://lv2plug.in/pages/filesystem-hierarchy-standard.html):
  bundles and URI identities, with platform-specific as well as portable files.
- [LV2 state flags](https://lv2plug.in/c/html/group__state.html): copying POD data is not
  equivalent to an architecture-independent representation; assets require path handling.
- [VST3 bundle structure](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Plugin%2BFormat.html)
  and [preset structure](https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical%2BDocumentation/Locations%2BFormat/Preset%2BFormat.html):
  platform binaries and class-addressed state; a merged bundle can carry multiple builds.
- [Apple Audio Components](https://developer.apple.com/documentation/audiotoolbox/audio-components)
  and [AU hosting](https://developer.apple.com/documentation/audiotoolbox/incorporating-audio-effects-and-instruments):
  Apple component registration and AU app-extension hosting.

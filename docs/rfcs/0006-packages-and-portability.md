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
Setlist/library persistence, AU/VST3 adapter or cross-format state converter.
[Performance version 1 folders](../performance-format.md) now persist the graph, prepared
sounds and shared effect state using installed plugins; they are not managed packages.
No changes to the audio graph are required for this proposal: package resolution and state
preparation belong to library/control services, before a ready graph is published.
Sample-instrument interoperability and MODX M migration are planned in
[RFC 0008](0008-sfz-native-samples-and-conversion.md). Its format adapters and loss reports
complement this package/dependency contract; importing a Yamaha or SFZ file does not by
itself establish a complete, playable target dependency set.

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

## Default export targets and selective installation

The normal **Export for another device** preset prioritizes offline Oscairn hardware:
include the required native-plugin builds and runtime dependencies for the exporter's
versioned default set of supported Oscairn Buildroot Linux profiles, plus shared content,
state and assets. Do not include macOS/Windows builds merely because export runs on a
Mac/PC. This policy applies to exported packages/libraries as well as Sounds, Performances
and Setlists with collected dependencies; it does not restrict what a publisher offers in
its full release or what a user deliberately includes in a full backup.

The preset names its exact CPU/runtime and relevant hardware-service/backend profiles.
"Linux" alone is not a target, and one ARM64 binary is not assumed compatible with every
Buildroot release. The default profile
set is versioned with the exporter/platform tooling; the export records the profiles it
actually covers. No build is invented when a package lacks a default target: report that
profile's unresolved dependency and do not mark it complete for offline use. An export
explicitly addressed to a named/paired device can select only that device's profile.

Advanced export controls can add macOS, Windows or other supported profiles, narrow the
Linux targets, include several destinations for offline travel, or produce a references-only
export. Show size and per-target offline readiness before writing the archive. Optional
online acquisition can assemble missing Linux builds on the source computer; if that is
unavailable, the result must state which dependencies still need to be supplied.

On import, select only the destination-compatible artifacts and required shared dependencies
for the installed store. A multi-target archive must not cause unrelated platform binaries
to consume the hardware's permanent plugin storage. Keep the authenticated release metadata,
artifact hashes and lock information needed to resolve other targets later, without keeping
their payloads. Select complete target artifacts; do not strip members from signed bundles
or mutate their signatures. Full-archive retention is a separate explicit backup/cache choice;
the importer does not delete or rewrite the user's source archive on USB or disk.

If a destination needs a build absent from the archive, resolve in this order:

1. Reuse the exact compatible release already installed or available in the verified local
   store. Check transitive runtimes/assets and state compatibility too.
2. When online acquisition is enabled, obtain missing artifacts for the locked release from
   its configured authenticated source as part of the normal import transaction. Download
   only that destination's requirements, not the publisher's entire multi-platform release.
3. If offline, no compatible build exists, retrieval fails, or a vendor installation is
   required, report the exact missing packages/targets and how they can be supplied. Allow
   the document/library to import with unavailable nodes, preserve state and warn that the
   affected performance is not ready. A paired computer or USB repository can supply the
   same locked artifacts later.

Downloading another platform build does not authorize a version upgrade, make an external
plugin native, or establish cross-format state compatibility. External plugins retain their
installation/conversion rules. Neither download nor installation occurs on the audio thread
or as a surprise when a performer starts playback. A Linux-default export may therefore be
complete offline for its advertised hardware profiles while requiring downloads on a Mac/PC;
that distinction is part of the export/import status, not an error in the default policy.

## Repairing missing dependencies on offline instruments

Offline means the instrument cannot fetch files itself, not that removed packages are
irrecoverable. Importing any trusted bundle containing the exact required target artifacts
and dependencies can satisfy existing unavailable Sounds, Performances, Setlists and apps.
Reuse the normal verified store/import transaction, then recheck affected dependents; do not
require reimporting their documents or overwrite newer musical edits with an older bundle's
state. A dependency-only repair is distinct from choosing to import/update musical documents.
Successful repair restores eligibility after validation, not automatic live activation.

Offer **Repair missing content** from unavailable items and Installed content. The musician
can repair selected items or all known missing dependencies on that device through:

1. **An existing package or backup:** inspect local/removable-media bundles and reuse exact
   matching artifacts, including shared libraries/assets and selected hardware backends.
2. **A request carried to a computer:** export a small **Missing content request** to USB.
   It records schema/request identity, target profile revision and relevant device capabilities,
   selected document/lock revisions, exact package/artifact/asset identities and hashes,
   dependency metadata and configured source identities. It need not include recordings,
   patch state or private song names. Include the locked dependency graph so the computer
   can collect transitive requirements, not merely the named top-level plugins.
3. **A USB-connected computer:** where a compatible data connection and companion transport
   exist, let the desktop read the request/inventory, acquire missing content and send the
   repair bundle back. An audio/MIDI-only USB connection does not imply file-transfer support.
   Use the same request, resolution and import operations as the USB-drive workflow; the
   computer is the downloader, so the instrument itself needs no Internet connection.

The desktop opens the request and offers **Collect for instrument**. First reuse its own
verified store/backups, then obtain exact artifacts from configured authenticated sources
when enabled. Resolve for the instrument, not the computer's OS. Produce a normal portable
repair bundle containing the missing dependency closure plus a fulfilled/unresolved report;
no separate package format or installer is needed beyond the request/report metadata.
Existing target content may be omitted using the request's inventory, but record that
assumption. A complete offline backup can instead include the full selected closure.

On return, the instrument verifies hashes, provenance, target compatibility and current
dependencies before staging installation. Recompute remaining requirements because the
library, firmware or installed content may have changed since the request was made. Show
partial completion honestly and allow a refreshed request; never upgrade versions or fill
gaps with different sounds. Interrupted transfers can be retried, and repeated imports are
idempotent. Existing working performances remain intact throughout repair. Request files
are data, not authority to execute commands, add arbitrary sources or run vendor installers;
the normal import/trust and external-plugin rules still apply.

Removal should explain these repair routes and offer export/backup before reclaiming files.
A verified retained copy on a computer or removable media provides recovery even when a
publisher later removes a download; deleting an instrument's copy does not delete that backup.
Distinguish **available in a verified backup**, **currently downloadable** and **no known
source**. Revalidate sources when repairing. No workflow can retrieve an exact artifact or
user asset that no longer exists in any accessible source; report that specific gap rather
than treating offline operation itself as the problem.

## Preparing for several performance devices

The musician prepares a Sound, Performance or Setlist for named instruments, not backend
artifacts. A library can remember several devices and assign all or selected items to each.
One shared musical revision can target several devices; explicit device variations can use
different sounds, layers or quality settings without overwriting the shared original. Device
assignments reference exact document/variation revisions and target locks. They do not copy
and silently fork every sound. Shared Sound edits offer an impact preview and an explicit
update of dependent Performances; rehearsed Setlists keep their pinned revisions until updated.

Show a content-by-device readiness matrix, with actionable reasons: missing download,
unsupported engine/mode, unresolved controls/outputs, insufficient capacity or stale checks.
The browser can filter for the selected devices (all selected versus any selected), while
keeping unavailable sounds discoverable. Collect only each destination's assigned items and
resolved dependencies. A multi-device archive can share common assets; each import installs
only its assigned target subset. Readiness is per device and revision, never one universal
green checkmark.

Audition locally only when a compatible software implementation can reproduce the saved mode.
When the hardware is connected and its integration supports it, the computer can edit while
the instrument produces the audio. Hardware-only sounds offline remain editable where
supported, with audition unavailable. Label approximate previews and recordings explicitly;
a recording cannot demonstrate parameter edits it has not rendered.

Distinguish **ready to transfer** (complete files for a declared target), **validated on this
device** (restoration, mappings and measured capacity checked) and the musician's rehearsal
status. Preparation checks every scene and setlist transition, including held notes/tails,
resource overlap and expected loading time. Checks become stale when relevant content,
packages, device configuration or firmware changes. No resource estimate guarantees every
possible live performance. Stage transfers, keep the previous working revision, and activate
an update deliberately outside an ongoing performance. Offline USB transfer remains first-class.

### Capability profile catalog

Maintain a versioned, declarative instrument-profile catalog in Git, with reviewed changes,
schema validation and test evidence. Publish authenticated immutable catalog snapshots for
clients; devices need neither Git nor an account, and can import/cache snapshots offline.
Git history provides review/provenance, not proof that a capability works. Profiles distinguish
documented, measured/verified, experimental and unknown claims, including test configuration
and evidence date. A named instrument is not declared supported merely by adding its profile.

Separate three things: the catalog's model/board/firmware/runtime profile, the actual device's
reported installed services/resources, and private user settings such as names, mappings,
library inventory and measured performance. Do not publish private device inventories to the
catalog. A profile includes audio/MIDI/control capabilities, backend requirements, runtime
baseline and measured limits where known. An instance may differ through firmware, hardware
revision or modifications; unresolved differences stay unverified rather than enabling
hardware optimistically. Resource availability still needs runtime admission.

Record profile ID/revision/digest with target plans and validation results. Catalog updates
do not silently change locked sounds or activate new backends; relevant changes invalidate
checks and offer revalidation. A connected device supplies its current inventory; an offline
device can export it via USB. Cached inventory shows its age and cannot certify current free
storage or resources. Local experimental profiles are allowed but retain that status.

### Editing on one instrument and transferring to another

For the hypothetical Montage-to-KICK workflow (neither Yamaha chip access nor that backend
is established), save the edited Performance and Sounds as new revisions with stable IDs,
parent revision IDs, complete current state, new assets and originating backend/state contract.
An engine's common portable state remains authoritative only if the backend can faithfully
export it; otherwise retain hardware-specific opaque state and declare its limited scope.
Do not flatten private state into a guessed list of parameter values.

Transfer directly or through the computer, preserving revision ancestry. Identical imports
are idempotent; a descendant can be offered as an update. Concurrent computer/instrument
edits keep both revisions and ask which to use or save as a variation. Do not use timestamps
as last-writer-wins, automatically merge opaque plugin states, or silently propagate deletion
to other devices. Git is for the public profile catalog, not a required musician-facing sync UI.

Preparing the edited revision for KICK resolves its exact compatible backend, packages and
assets, checks controller/output mappings and measures capacity. Equivalent CPU state can
restore when it fits. A Montage-only mode stays unavailable unless a tested conversion or
explicit user-selected KICK variation exists. Preserve the source revision and name any
sound/feature loss. Derived alternate-backend states and prior device checks become stale
after edits; regenerate from the edited state or report unavailable, never reuse an older
snapshot that discards the edit. Editing a device-specific variation does not overwrite its
parent or siblings. Approved updates advance the relevant assignments/setlists explicitly.

## One import, explicit completeness

The normal library file is lightweight: graph/state/metadata plus asset and dependency
references. Copying that file alone is not the same as collecting its dependencies.
The UI offers **Export for another device** on Sounds, Performances, Setlists and Projects:

1. Collect the transitive closure: all songs, inactive scenes, layers, effects, MIDI
   processors, prepared sounds, plugin runtime dependencies and required assets. It must
   not collect only the currently sounding patch.
2. Start with the default Oscairn Buildroot Linux target preset described above. Use the
   explicit destination profile for a named device, or advanced settings for other/multiple
   targets. A paired device can report installed hashes and runtime capabilities. Record
   actual covered profiles; an unspecified destination is not universally compatible.
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
5. Import selects destination artifacts, reuses installed dependencies and resolves missing
   locked builds by download when enabled, otherwise reporting required installations. Show
   one consolidated plan for added code/content and unresolved items. Stage, verify, resolve
   and scan away from live processing; publish atomically after success.
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
  plugins can share immutable samples through the planned host asset API ([RFC 0009](0009-sound-assets-and-dependencies.md)). Independently
  allocated or decoded samples, voices, delay lines and other mutable DSP state are private.
  No blanket "loaded once" memory promise for arbitrary plugins or across worker processes.
- Pins come from explicitly retained packages, saved documents/revisions, backups as
  configured, prepared setlists and live instances. Dependency-only installs are reachable
  through these roots, not permanent roots merely because they were once installed.
  Garbage collection must not remove a referenced release.
  Updates install side by side; rollback selects the old lock. Garbage collection is an
  explicit storage operation, never work on the audio callback.

Assets must be declared/exported by the plugin or integration; [RFC 0009](0009-sound-assets-and-dependencies.md) defines how. For an opaque third-party
sampler, copying its preset blob does not discover every file it might open. Include the
whole required library if its working subset cannot be established and inclusion is
permitted, or mark the export unresolved. Do not label factory-library references "small
and complete" unless those libraries are also included or confirmed at the destination.

## Installed content and removal

Provide **Settings > Installed content** per device for managed plugins, apps and libraries:

- Show versions, publisher, installation reason, status, disk usage and actually reclaimable
  space. Deduplicated/shared bytes must not be counted twice in a batch removal estimate.
- Show **Used by** paths through Sounds, Performances, Setlists, Projects, app dependencies
  and retained revisions; also allow inspection from a document to its dependencies.
- Import/export selected packages with their required dependency closure and target options;
  export reports unavailable artifacts and external transfer restrictions as usual.
- Offer **Keep installed**, reinstall/**Repair missing content**, remove selected items and
  **Remove all unused**, with a preview of the exact versions/content affected. Retention and version-update policy are
  distinct: keeping an engine for future use does not request an upgrade.
- Hide implementation libraries in the default list, with **Show shared dependencies** to
  reveal who installed/requires them. Manually installed apps/plugins default to retained;
  dependency-only installs become cleanup candidates after their last root disappears.

Deleting a Setlist removes that document's references, not independently saved Performances
or Sounds. Removing a song from a Setlist is not deleting the song from the library. Deleting
a Sound/Performance referenced elsewhere shows affected documents and requires an explicit
choice; keep their captured state/revisions or unavailable references, never silently erase
dependent musical work. The subsequent package-cleanup decision is a separate operation.

After a deletion removes the final retention root, default to one grouped prompt offering
**Keep installed** or **Remove unused content**, naming candidates, reclaimable space and
offline reinstall implications. Cancelling keeps packages. Per-device policy offers **Ask
before removing unused content** (default) or **Keep installed without asking**; either still
allows deliberate cleanup in Settings. Keeping particular content pins its dependency closure
until the user releases that retention. Avoid repeated prompts for already retained content.
No automatic uninstall policy is needed for the first version.

Compute unused content by reachability from the device's complete known reference graph:
saved and retained revisions (including retained trash/undo), all scenes and variations,
device preparations, manually retained apps/plugins, configured backups, published DAW
exports, active instances and in-progress transactions. Explain which root prevents removal.
Offline/unmounted registered libraries keep their last known pins until explicitly forgotten;
do not infer disuse because a drive is missing. Arbitrary unregistered DAW projects/USB files
cannot be enumerated: expose that scope and allow Keep installed or export before cleanup.

Deliberately removing a used managed package is distinct from garbage collection. Preview
every known affected document/app, warn that dependents become unavailable until the exact
compatible version and dependencies are restored, and require confirmation. Preserve states,
locks, assets owned by the user and revision metadata; invalidate readiness and disable the
affected dependents without silent bypass. Permit removal only after active/prepared users
and transactions have safely released it; never unlink live resources mid-performance.
Protected platform components are managed by system updates, not this removal action.
Unmanaged vendor installations can be inventoried but use their supported uninstall path.

Separate package binaries/immutable distributed content from user recordings, presets and
mutable app data. Uninstalling an app/plugin does not delete user data. Shared dependencies
still referenced by other retained packages remain installed. Recompute reachability before
committing a removal to handle concurrent saves/imports; journal/stage metadata changes for
crash recovery. Cleanup is control/storage work, not audio-callback work. Missing packages
retain reinstall identity and offer the [offline repair workflow](#repairing-missing-dependencies-on-offline-instruments);
show known backup/download sources rather than assuming offline content cannot be restored.

### Package-manager prior art and reuse

Borrow established behavior before designing new machinery:

| Prior art | Useful Oscairn behavior | Boundary |
| --- | --- | --- |
| [npm explain](https://docs.npmjs.com/cli/v11/commands/npm-explain/) and [Yarn why](https://yarnpkg.com/cli/why) | Explain every dependency path in Used by | Roots are musical documents/apps/revisions, not only JS projects |
| [pnpm store](https://pnpm.io/cli/store) | Shared immutable content and explicit unused-store cleanup | An offline instrument cannot assume pruned releases can be downloaded again |
| [Homebrew autoremove](https://docs.brew.sh/Manpage#autoremove---dry-run) | Distinguish requested installations from orphaned dependencies; preview removals | Never remove versions still required by saved performances |
| [Nix garbage collection](https://nix.dev/manual/nix/2.35/package-management/garbage-collection.html) | Reachability from retained roots/generations and rollback retention | Oscairn additionally tracks live instances and prepared musical revisions |

These are researched patterns, not adopted implementations. At the resolver milestone,
evaluate reusable version-resolution, content-store, archive and transaction components
against our target/ABI/state contracts, offline behavior, footprint and supported platforms.
Reuse a suitable maintained component if it is smaller than owning that machinery. Do not
wrap an entire JS/system package manager merely to obtain dependency traversal; keep Node,
Ruby, build recipes and arbitrary install scripts out of the device runtime unless separately
justified. Credit upstream authors and preserve applicable notices for any borrowed code;
review the exact component and its dependencies before adoption. No upstream code is copied
by this RFC. Source documentation checked 2026-10-29.

### Implementation candidates and decision timing

Do not assume Oscairn needs a new package manager from scratch. Evaluate existing foundations
before choosing a permanent package format or implementing a general dependency solver:

| Candidate | Reusable machinery | Questions to prove |
| --- | --- | --- |
| [Mamba/libmamba](https://mamba.readthedocs.io/en/latest/user_guide/micromamba.html) | C++ package tooling without requiring Python; resolution, downloads, cache and environment installation | Fit with Buildroot, exact per-project versions, hardware variants, signed plugin bundles and script-free installation |
| [Nix](https://nix.dev/manual/nix/stable/command-ref/nix-store/export.html) | Immutable store, retained versions/roots and offline dependency-closure transfer | Store integration, ordinary DAW discovery/signatures, embedded footprint and native Windows delivery; currently documented platforms are Linux/macOS |
| [libsolv](https://github.com/openSUSE/libsolv) | Embeddable dependency solving used by Mamba and other package systems | Leaves downloading, verified installation, storage and recovery to other components; use directly if a fuller foundation costs more to adapt |

Mamba is the first fuller candidate to evaluate, alongside Nix as a reference for durable
retention. Its [installation behavior](https://mamba.readthedocs.io/en/latest/advanced_usage/detailed_operations.html)
includes file sharing, possible prefix rewriting and package scripts; those last two must
not violate Oscairn's signed-bundle and managed-install contracts. Nix's
[supported-platform list](https://nix.dev/manual/nix/stable/installation/supported-platforms.html)
does not establish native Windows support. None is adopted; general Linux support alone
does not establish compatibility with our Buildroot runtime.

At the package milestone, compare a small working scenario: two plugins sharing one
dependency, two retained versions, an offline repair bundle, and removal that preserves
another Performance's dependencies. Check signed macOS bundles, native Windows, the actual
Buildroot runtime, interrupted installation and resource footprint. Musical revision roots,
state/backend compatibility, live resource admission and the musician-facing UI remain
Oscairn responsibilities whichever foundation wins. Prefer existing machinery where the
complete integration is smaller and maintainable; do not select by feature lists alone.

Defer this prototype until the basic sound selection, discovery and save/reopen workflow is
usable. Carry stable identities/state provenance now, without making that workflow depend
on a catalog, a full package resolver or all the future multi-device features.

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

## Backend variants and device-specific dependencies

OS/CPU builds are one selection dimension; synthesis backend capabilities are another.
A plugin release must describe **alternative execution plans**, not make every CPU, FPGA
and sound-chip implementation a mandatory dependency. Keep the common engine identity,
state schema, control definitions and shared assets separate from backend-specific payloads.
Use the same manifest/lock mechanism rather than introducing a second installer.

A candidate plan records its backend identity/version, target runtime, required service ABI,
board/chip/firmware/FPGA-region compatibility, implemented sound modes, state compatibility,
resource and latency constraints, and the exact artifact/dependency hashes to install.
It can name an engine-wide backend or an explicitly supported combination of kernels.
Selecting one plan pulls its transitive dependencies; unselected alternatives remain
metadata only. An install must fail honestly if a selected plan's artifacts/services are
missing, rather than pulling an unrelated implementation that happens to be available.

For a hypothetical `vendor.organ` release:

- **CPU device:** common data plus the matching CPU DSP artifact.
- **Supported FPGA instrument:** common data, its host control adapter and the exact board/
  region-compatible FPGA artifact; no dedicated-sound-chip adapter or unrelated bitstreams.
- **Supported sound-chip instrument:** common data and its host adapter/mappings, plus any
  legitimately supplied required firmware; no FPGA payload. Existing ROM/firmware/banks
  are validated device capabilities when supplied by the instrument, not duplicate downloads.

Every selected plan still includes whatever CPU host/control code it needs. A portable
engine's published CPU reference backend is not necessarily installed on every hardware
instrument. Retain/install it only when selected or needed for declared offline rendering
or a prepared fallback. CPU fallback also requires enough live capacity and valid sound/state
compatibility; its availability in a catalog is not a recovery guarantee.

The portable document records engine, musical mode/quality and any mandatory capabilities.
Keep that separate from a **per-target resolved execution plan** in the dependency lock,
which pins actual backend/artifact versions and records the capability profile used for
selection. Device/session policy may prefer an equivalent implementation, but importing to
another device creates/revalidates its target plan instead of rewriting the musical patch
or reusing physical device handles. Hardware-enhanced modes with no equivalent CPU version
remain explicitly hardware-required, as defined in
[RFC 0004](0004-hardware-services.md#backend-packages-and-sound-modes).

Export profiles therefore include relevant board/services/accelerator capabilities in
addition to CPU and runtime. The default Buildroot export covers only its advertised
profiles: include each selected profile's resolved backend dependency set, deduplicating
common files. A named-device export includes its selected plan and any explicitly requested
fallbacks. It does not include every FPGA/DSP image merely because the archive is for Linux.
A generic CPU-only profile cannot be labelled ready for a hardware-required Sound or a
configuration whose CPU budget is insufficient. Unknown hardware capacity remains unverified.

Import uses actual device service inventory, supported modes and measured capacity to select
or validate a compatible plan, installs only that plan's dependencies, and applies the usual
exact-release download/offline-missing rules. A package manager may install several candidate
plans when explicitly requested for offline use, but the resource manager still must reserve
the chosen one before performance activation. Profile matches and successful file installation
do not prove an accelerator is currently available. Firmware/bitstream loading belongs to
hardware services, outside the audio callback, with ownership and recovery rules.

Tests must prove that two devices with the same Linux/CPU ABI but different accelerators
install different backend payloads, neither retains the other's code, and shared assets
remain shared. Also test wrong board/shell/firmware rejection; opaque or inaccessible chip
services; absent ROM banks; an insufficient CPU fallback; incompatible sound modes; and two
plugins competing for one resource. First exercise these rules with simulated capabilities;
real hardware and complete latency/state tests are required before claiming support.

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
   project manifest/lock and shared-store resolution schema with fixtures. Include document
   revision ancestry, per-device assignments/variations, versioned capability profiles and
   dependency/retention roots before those identities become persistent.
2. **First vertical slice:** managed CLAP packages, one explicit shared runtime and sample
   dependency, exact version locks, side-by-side immutable installation, offline export/import
   and unavailable-node preservation. Demonstrate a setlist on a clean second supported
   machine with no plugin-specific installers. A local package directory is enough; no
   online catalog required. Test cache removal without losing pinned offline installations
   and two projects sharing a release while another pins an older one. Include basic Used by,
   grouped cleanup prompts, per-device retention settings and safe removal/reinstall; deletion
   safety is part of this slice, not later UI polish. Prove dependency-only repair from an
   existing bundle and an exported missing-content request fulfilled from a computer's local
   repository, preserving newer document edits. Avoid inventing another plugin ABI.
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
6. **Distribution/UI polish:** published profile/package catalogs, multi-device preparation
   matrix, paired-device deltas, advanced storage views, migration tools and optional rendered
   alternatives use the same manifest and lock. Add online acquisition for offline requests
   and USB companion repair using the same bundle/import path. Prove revision-aware round
   trips before advertising automatic device synchronization.

Also test multiple devices using shared and distinct revisions; offline/stale/modified device
profiles; conflicting edits and repeated imports; hardware-only mode transfer and stale
converted state; removal of a setlist without deleting saved sounds; both cleanup policies;
manually retained and shared dependencies; retained history and offline-library pins; batch
reclaimable sizes; deliberate used-package removal/reinstall with intact state; active-resource
refusal; and interrupted/concurrent cleanup versus import/save. Unknown external usage must
not be presented as proven unused. Repair tests cover transitive missing dependencies,
instrument-target artifacts collected on another OS, shared-store reuse, newer document edits,
stale device/inventory requests, partial/unavailable downloads, tampered bundles, interrupted
USB transfers and repeated imports. Test backup recovery without a catalog and revalidation
before readiness; neither repair completion nor reconnecting a computer activates a live change.

Required tests include Linux-default export on a Mac/PC without desktop binaries; advanced
multi-target export that installs only destination artifacts on hardware; exact-release
fetch on a desktop importing a Linux-only archive; offline/download-failure warnings that
preserve state; explicit missing-build reports for default profiles; and preservation of
signed target bundles. Also test clean/offline import of an entire setlist; one-copy shared resources;
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

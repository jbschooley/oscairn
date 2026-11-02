# RFC 0031: The package repository and distribution

Status: proposed, 2026-11-01. Fills in step 6 of
[RFC 0006](0006-packages-and-portability.md) ("Catalogs, multi-device preparation and
distribution polish") for packages. Builds on RFC 0006's packages, locks, offline repair and
[signatures](0006-packages-and-portability.md#signatures), the
[library archive](../library-archive.md), the library
([RFC 0021](0021-sounds-performances-and-library.md)), apps
([RFC 0022](0022-apps-and-requests.md)), item types ([RFC 0023](0023-item-types.md)), device
profiles ([RFC 0024](0024-device-profiles.md)), resource estimates
([RFC 0025](0025-resource-budgets.md)), paired devices and library sync
([RFC 0026](0026-network-control-and-web-ui.md)), system images
([RFC 0028](0028-system-image-updates-boot-and-power.md)), translations
([RFC 0029](0029-internationalization.md)), the system shell
([RFC 0030](0030-system-shell-launcher-and-navigation.md)) and sample assets
([RFC 0009](0009-sound-assets-and-dependencies.md)).

Revised 2026-11-01 with Jacob's input: [ideas from npm, pnpm and yarn](#ideas-from-npm-pnpm-and-yarn),
[installing in little space](#installing-in-little-space) (unpacked package folders and
streamed installs), [decisions](#decided-2026-11-01) on paid content, large libraries and
manufacturers' repositories, and [research and a proposal](#publisher-trust-and-review) on
publisher trust and review. Those two stay open until Jacob confirms the proposal.

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. The repository format; installing from a folder or USB drive, from an unpacked package
      folder, and streamed
- [ ] 2. Fetching over HTTPS: refresh, incremental index updates, mirrors, resumable downloads
- [ ] 3. Publishing: the `oscairn publish` command, checks, immutable versions and yanking
- [ ] 4. Revocation lists and the takedown process
- [ ] 5. Discovery: Get more in the Library, the Store app, compatibility filters
- [ ] 6. Updates: packages, Performances' locks moved on explicitly, apps
- [ ] 7. Offline and multi-device: fetching for a device, USB subsets, a paired computer as a source
- [ ] 8. The official repository goes public: hosting, mirrors, policy

## Problem

Musicians and publishers want one place to share plugins, Sounds, Performances, sample
libraries, themes, apps and translations, and Oscairn should find and install them there.

RFC 0006 has the pieces around this: packages and their identity, locks that never move on
their own, configured sources, offline repositories on USB or a paired computer, and
signatures. It leaves the repository itself open: what file a device downloads to learn what
exists, how a publisher puts a package there, who hosts it, how updates and search work, what
happens when something must be taken down, and how an instrument that never sees the internet
gets any of it.

## Outcome

- A **repository** is a set of static files: a small signed root file, a signed index and the
  package files, named by their hashes. Any static web host or CDN can serve it, anyone can
  mirror it, and the same layout works as a folder on a disk or a USB drive.
- A device can use **several repositories**: the official one, a manufacturer's, a
  publisher's own, and the user's.
- A **publisher** runs one command to check, sign and submit a release. Every package in a
  repository is signed by its publisher. Versions never change once published; a bad one is
  yanked, not overwritten.
- Musicians **browse and search** in the Library and in a Store app, filtered by what runs on
  their device. Updates never change a Performance's sound without the musician saying so.
- An instrument with no internet gets the same packages from a USB drive or a paired computer,
  checking the same signatures.
- The first version needs **no server-side code**: static hosting, a CDN, a Git repository for
  submissions and a CI job.

## Prior art

| System | What Oscairn borrows | What it does not |
| --- | --- | --- |
| [Debian/APT](https://wiki.debian.org/SecureApt) | A small signed root (`InRelease`) naming the index files by hash; untrusted mirrors; any HTTP server | Its dependency solver and maintainer scripts |
| [The Update Framework](https://theupdateframework.io) | An offline root key delegating to an online index key; refusing an older index (rollback) | Its expiry dates: instruments may have no clock |
| [Flathub](https://docs.flathub.org/docs/for-app-authors/submission) | Submissions as pull requests to a Git repository, built and checked in CI; publisher IDs verified by domain | Its runtime model and server-side build farm |
| [F-Droid](https://f-droid.org/docs/Reproducible_Builds/) | A signed index; reproducible builds checked against the publisher's; the index usable from a USB drive or a nearby device | Building every package from source on the project's machines |
| [npm](https://docs.npmjs.com/policies/unpublish) and [crates.io](https://doc.rust-lang.org/cargo/commands/cargo-yank.html) | Versions immutable once published; yanking hides a version from new installs while locks still fetch it | A live registry API and accounts to install |
| [VCV Rack Library](https://library.vcvrack.com) | One place for free and paid plugins of one host, per-platform builds, a plugin manifest checked on submission | A login required to install, and a central server |
| Bitwig and Arturia preset stores | Browsing sounds by type and tag inside the instrument, with audio previews | Accounts and closed formats |
| [MOD Audio's plugin store](https://mod.audio) | Plugins for a hardware device, filtered to what builds for it, installed from a computer or the device | A store that only its own server can serve |

The links are pointers for later reading; this RFC adopts none of their code.

## Ideas from npm, pnpm and yarn

Jacob asked on 2026-11-01 which ideas from the JavaScript package managers apply. Already
borrowed:

- **pnpm's content-addressed store:** RFC 0006's
  [shared store](0006-packages-and-portability.md#project-manifest-lockfile-and-shared-store).
- **`npm ci`:** installing exactly what the lock names, never a newer version (RFC 0006).
- **Immutable versions and yanking,** from npm and crates.io ([below](#versions-and-yanking)).

Considered:

| Idea | Decision | Why |
| --- | --- | --- |
| [Scoped names](https://docs.npmjs.com/cli/v11/using-npm/scope) (`@publisher/package`) | Not adopted | Package IDs are already reverse-DNS under the publisher's ID (`com.example.organ`), which is the same namespacing. Scopes fixed npm's flat namespace; Oscairn never had one |
| [Dist-tags](https://docs.npmjs.com/cli/v11/commands/npm-dist-tag) as release channels | Adopted, two channels | A version is published to `stable` (the default) or `beta`. The index lists each version's channel. A device offers beta versions of a package only when the musician turns them on for it. Locks name versions, never channels, so a channel never moves a Performance |
| [`npm deprecate`](https://docs.npmjs.com/cli/v11/commands/npm-deprecate) | Adopted | A publisher can mark a package, or a range of its versions, deprecated with a message and an optional replacement package ID. It stays installable and is shown with the message. Unlike yanking, it is advice. Nothing is replaced on its own |
| [npm provenance](https://docs.npmjs.com/generating-provenance-statements) (Sigstore attestations) | Not adopted on devices | It is a second signature system with its own services and trust root, which devices would have to carry and update, and npm supports it only from GitHub Actions and GitLab CI on cloud-hosted runners. Oscairn's Ed25519 signatures already say who. The goal, linking a build to its source, is met by the reproducibility check: the manifest names the source and builder image, CI rebuilds, and the index says whether it matched. A desktop tool may show a Sigstore bundle a publisher adds to the details; it grants no trust |
| [Integrity hashes in the lock](https://docs.npmjs.com/cli/v11/configuring-npm/package-lock-json#packages) (Subresource Integrity) | Already adopted | A Performance's lock records each build's digest, and a package's identity is its manifest's hash (RFC 0006). Any source, a mirror, a USB drive or a friend, is checked against them. Oscairn writes SHA-256 in hex everywhere, not SRI's `sha512-` base64 strings |
| [Yarn's offline mirror and zero-installs](https://yarnpkg.com/features/caching) | The mirror, yes; zero-installs, no | The offline mirror is a project's packages kept beside it so it installs with no registry. A USB repository (a signed root, index and the blobs needed) is the same idea, checked the same way. Zero-installs runs code straight from the committed cache. An instrument does not play from a removable drive, which can be pulled mid-show, so it always installs into its own storage. It can install by reading the drive in place ([below](#installing-in-little-space)) |
| [pnpm's hard-linked store](https://pnpm.io/motivation) | Already adopted, within one filesystem | RFC 0006's store keeps each file once, by hash, and links or clones it where it is used. Samples are stored once by hash already (RFC 0009). Links stay inside the data folder's filesystem: never to a removable drive or a share, which may be gone at the next start |
| [`engines`](https://docs.npmjs.com/cli/v11/configuring-npm/package-json#engines) | Already adopted as `requires.oscairn` | Both name the host version a package needs. npm's is advisory unless `engine-strict` is set. Oscairn's is enforced: a package needing a newer system is shown with "needs system update" and not installed. Platform builds and estimates cover what `os` and `cpu` cover in npm |
| [Workspaces](https://docs.npmjs.com/cli/v11/using-npm/workspaces) | Adopted for publishing | A publisher with many packages keeps them in one folder with a list of members. `oscairn publish` checks them together, publishes each changed one as its own version, and submits them in one pull request. The dependency check accepts a dependency submitted in the same pull request. Devices see only separate packages |

## A repository

### What is in it

```
repository.json        the root: who this is, its keys, mirrors, and the current index
repository.sig         the root's signature
index/<sha256>.json.gz     the full index, named by its hash
index/<sha256>.delta.gz    changes from one index to the next
details/<sha256>.json      a package's long description, screenshots and previews
revocations/<sha256>.json  the revocation list
blobs/<ab>/<sha256>        package files: manifests, items, samples, plugin builds
```

- **Everything but the root is named by its SHA-256** and never changes. A CDN can cache it
  forever, and a mirror copies it once.
- **The root is the only file that changes.** It is small (a few kilobytes) and names the
  current index, the deltas, the revocation list and the mirrors by hash and size.
- **Blobs are kept by the first two hex digits of their hash** (`<ab>`), so no folder or
  GitHub release holds too many: a blob is at the root's blob base, then `<ab>/<sha256>`.
- **Compressed with deflate (gzip),** which the vendored miniz already reads. No new library.

### Package files

A release is published as a library archive ([format](../library-archive.md)), signed as
RFC 0006 says. The repository stores it in pieces, each named by its hash:

- the archive's manifest (`library.json`) and its signature (`library.sig`);
- each item folder, as one zip;
- each sample, as itself, so a sample shared by several packages or kept between versions is
  stored and downloaded once (RFC 0009 stores samples by hash already);
- each plugin build, as one zip of its bundle, with the build's own digest and signatures as
  the manifest lists them.

A device fetches the manifest, checks its signature, and then fetches only what it needs: the
items chosen ([RFC 0006](0006-packages-and-portability.md#choosing-what-to-import)), the
samples it does not have, and its own platform's builds. It can put the pieces back together as
an ordinary `.olib` archive, byte for byte the archive's entries, to pass on by USB.

A package can name a **blob base** of its own: an HTTPS address the publisher hosts, where its
large files are. Their hashes are in the signed manifest, so they need no trust in that host.
This keeps a 20 GB piano off the project's bandwidth.

### Several repositories

- A device keeps a list: the repositories its image ships (a product names them in its
  profile, RFC 0024's new `repositories`), and those the user adds.
- **A manufacturer may make its own repository the default, or hide the official one,** but it
  is heavily discouraged ([decided](#decided-2026-11-01)). The profile field that hides it,
  `repositories.hideOfficial`, is documented as discouraged in the profile schema and in the
  product documentation. Hidden means off by default, never removed: Settings always keeps the
  official repository one switch away, and no profile can take that switch out.
- **Adding one** takes its address and its root key's fingerprint, typed, scanned from a QR
  code, or read from a small `.orepo` file. Nothing is added by a package or a web page.
- **Each installed package records its repository.** Updates and repair come from there, as
  RFC 0006 requires: Oscairn never searches other repositories for a package of the same name.
- **The same package ID in two repositories** is one package only when its publisher key is
  the same. Otherwise both are listed as different, with their repositories named.
- **A folder or USB drive** is a repository with the same layout. It may hold a whole
  repository, or another repository's signed root and index with only some blobs; a package
  whose blobs are not there is shown as "not on this drive".
- **Your own repository:** a desktop can make one from its own exports, signed with its device
  key (RFC 0006's "your own devices"), for a band or a studio to share.

## Installing in little space

An instrument may not have room, in storage or in memory, for a zipped package and its
unzipped contents at once (Jacob, 2026-11-01). So no install needs the whole archive in the
instrument's storage or memory. **Peak storage is the installed size plus one entry,** not
twice the package.

### Unpacked package folders

A package can also come as a **folder**: the same tree as inside the archive, with
`library.json`, `library.sig` and every file the manifest lists by hash. It might be on a USB
drive or a desktop's share. This is a second form of the
[library archive format](../library-archive.md), not a new format:

- It is checked by the same rules as an archive: the manifest's signature, every path rule,
  the limits, and every file's hash. A file the manifest does not list is refused, as in an
  archive. Hidden files and folders (a name starting with `.`, such as `.DS_Store`) are skipped,
  since desktops add them.
- It is not the same as a folder repository ([above](#several-repositories)), which holds blobs
  named by hash. A folder package is what a person gets by unzipping an `.olib`.
- Each file is hashed **as it is copied** into place, never checked first and copied after: a
  share or a drive can change between the two. A move instead of a copy is offered only within
  one filesystem, and the moved file is hashed before it is used.

### Streamed installs

Every install writes entry by entry:

1. Read the manifest and its signature, and check them. Check free space for the installed
   size (the index's `installed` figure, or the sizes the archive or folder gives) plus the
   largest entry, and stop before writing anything if it does not fit.
2. For each entry needed, read or fetch it, decompress it as it arrives, hash it as it is
   written into a staging folder in the data folder, and compare. Memory use is a fixed buffer,
   not the entry's size.
3. When an item, a sample or a plugin build is whole and verified, move it into place by a
   rename in the same filesystem. Nothing is published half-written.
4. On a bad hash, a pulled drive or a dropped connection, remove what this install staged and
   leave what was there before, as reading an archive does now.

Sources:

- **From a repository,** this is the per-blob fetch ([package files](#package-files)): one blob
  at a time, each resumable. A blob that is itself a zip (an item folder, a plugin build) is
  unzipped as it streams, so it is never stored zipped beside its contents.
- **An `.olib` on a USB drive or a share** is read in place, without copying it to the
  instrument first. A zip's directory is at its end, so a seekable file is enough to find and
  decompress each entry straight into staging.
- **A folder package** is copied file by file, as above.
- **An `.olib` already in the instrument's own storage** costs its own size as well. The
  device can install from it, but offers to remove it after; downloads never land as a whole
  archive.

Today, reading an archive extracts every entry into a new folder before installing from it
([format](../library-archive.md#reading-one)). Streaming replaces that for packages.

## The index

### What it holds

The index lists every package and version, with what a device needs to decide whether to show
it, whether it fits, and what to download. It does not hold long descriptions or media; those
are in `details/`, fetched when a package's page opens.

```json
{
  "format": "oscairn.repository-index",
  "version": 1,
  "repository": "org.oscairn.main",
  "serial": 1842,
  "packages": [
    {
      "id": "com.example.organ",
      "name": "Example Organ",
      "summary": "A tonewheel organ and rotary speaker",
      "publisher": { "id": "com.example", "name": "Example Audio", "key": "3f9a1c0d2e4b5a67" },
      "license": "GPL-3.0-or-later",
      "tags": ["organ", "keys"],
      "details": "sha256-…",
      "versions": [
        {
          "version": "2.1.0",
          "manifest": { "sha256": "…", "size": 18342 },
          "packageSignature": true,
          "requires": { "oscairn": "1.4", "packages": [{ "id": "com.example.dsp-runtime", "version": "2" }] },
          "types": { "oscairn.sound": 64, "oscairn.performance": 12, "clap": 2 },
          "platforms": {
            "linux-arm64": { "size": 9821004, "installed": 24117248 },
            "macos-arm64": { "size": 11003920, "installed": 27262976 }
          },
          "common": { "size": 412881, "installed": 1048576 },
          "estimates": { "org.example.keys61": { "memory": 96, "cpu": 0.12 } },
          "yanked": null
        }
      ]
    }
  ]
}
```

- **Identity:** the package ID (reverse-DNS, under its publisher's ID) and version, and the
  manifest's hash, which is the identity a lock records (RFC 0006's package identity).
- **`types`:** how many items of each type the package holds, by RFC 0023 type ID, and its
  plugins and apps by format, so a search by type needs no download.
- **`platforms`:** download and installed size per platform, apart from what every platform
  shares (`common`). A device shows its own platform's figures only.
- **`estimates`:** memory and CPU per device profile where measured (RFC 0025 step 5), from
  the publisher or the repository's checks. They decide "fits this device" before anything is
  downloaded; admission still measures on the device.
- **`license`:** an SPDX expression, or `LicenseRef-` and the package's own license file for
  anything else.
- **`publisher.key`:** the publisher's key fingerprint. The manifest must be signed by it.
- **Names and summaries** are in American English; translations of them may be added per
  locale (`names`, `summaries`, keyed by locale, RFC 0029).

### Size

The index must be small enough for an instrument to download over a slow link, store and
search:

- one record per package, a few hundred bytes compressed per version;
- the target is under 2 MB compressed for 5,000 packages with their versions;
- the device reads it into the library's database (RFC 0023 step 5), so search is local and
  works offline;
- if it grows past that, versions older than the newest few move into a per-package history
  file named in the record, fetched when an older version is asked for (a lock asking for it,
  or "show all versions").

### Incremental updates

- Each change to the repository makes a new index with a higher **serial**.
- The root names the full index and the deltas from recent serials to the current one. A
  delta lists the package records that changed, each whole.
- A device that has serial 1830 fetches the root, then the delta from 1830 if the root lists
  one, else the full index. A typical refresh is the root and a few kilobytes.

### Signatures and freshness

- **The root is signed** by the repository's **index key**. The root also lists the
  repository's **root key**, which signs the index key and is kept offline. A stolen index key
  is replaced by a new one signed by the root key, and the old one is revoked (as TUF does).
  Ed25519, as in [RFC 0006](0006-packages-and-portability.md#signatures), over
  `oscairn-index-1\0` and the signed file's exact bytes: `repository.json` for the root (its
  signatures in `repository.sig`, a list of `{key, signature}`), and the index key's 64 hex
  digits and a newline for the index key. `oscairn-sign file` signs either and
  `oscairn-sign verify-file` checks it.
- **The root's fields:** `format` (`oscairn.repository`), `version` (1), `repository`, `name`,
  `serial`, `rootKey`, `indexKey` (`{key, signature}`, the root key's signature over it),
  `index` and `revocations` (each `{path, sha256, size}`), `blobBase` and `mirrors`. Until step
  1 stores a release in pieces, each version in the index also names its whole archive
  (`archive`, `{sha256, size}`), a blob like any other.
- Everything else is checked by its hash in the root.
- **Rollback:** a device refuses a root with a lower serial than one it has seen, so a mirror
  cannot hand it an older index with a version that has since been revoked.
- **Freezing:** a stale mirror can keep serving an old, valid root. Without a clock a device
  cannot tell. It fetches the root from the repository's own address when it can and blobs
  from mirrors; with a clock it shows the root's age.

## Publishing

### The command

`oscairn publish`, part of the `oscairn` tool ([architecture](../architecture.md#building-and-sharing-plugins)):

1. Packs the release into a library archive, or takes one already made.
2. Runs the checks below locally, so a publisher sees failures before submitting.
3. Signs: each plugin build and app (a build signature) and the archive (the package
   signature), with the publisher's key (`oscairn-sign`, RFC 0006). Items may carry their own
   signatures too. Signing stays on the publisher's machine; the key never leaves it.
4. Submits.

**Publishing to a repository requires a package signature.** That, and signing each component,
are the owner's decisions of 2026-11-01, written into
[RFC 0006's signatures](0006-packages-and-portability.md#signatures); this RFC does not restate
them. An unsigned archive can still be shared by hand and installed under RFC 0006's policy.

### Submitting, with no server

For the first version, the official repository takes submissions as **pull requests** to a Git
repository of submission records:

- `packages/<package>/<version>.json` holds where the signed archive is (a URL, often the
  publisher's own release page, or the archive beside it) and its hash, with the index's
  summary, license, tags, channel and claimed platforms.
- **A publisher's first submission** also adds `publishers/<publisher>.json`: the publisher's
  ID, name and key. A reverse-DNS publisher ID is verified by a file at
  `https://<domain>/.well-known/oscairn-publisher.json` naming the key, as Flathub verifies
  app IDs. Later submissions must be signed by that key or one it signed.
- **CI** fetches the files, checks every hash against the signed manifest, runs the checks, and
  posts the results on the pull request.
- **On merge**, CI copies the blobs into storage (unless the package uses its own blob base),
  writes the new index and root, signs the root with the index key, and uploads. Merges are
  serialized, so serials never collide.
- A template for the repository and its CI is kept in
  [tools/package-repository/template](../../tools/package-repository/template/README.md)
  ([hosting](#hosting-and-cost)).

A third party running its own repository uses the same tool: `oscairn repo build` turns a
folder of signed archives into a repository, ready to upload anywhere.

### Checks

| Check | What it proves |
| --- | --- |
| Schema | The manifest, item documents and type manifests are valid (RFC 0023) |
| Archive safety | Every rule for reading an untrusted archive ([format](../library-archive.md#reading-one)) |
| Signatures | The package signature and every build signature verify with the publisher's key |
| Publisher | Every release of a package names the same publisher, and no publisher's ID is another's followed by a `.` and more: otherwise the owner of `io.github` could publish a newer `io.github.alice.synth`, signed with its own key, and take the package over |
| Platforms | Each platform the package claims has its build; a first-party package has one for every supported profile ([RFC 0006](0006-packages-and-portability.md#platform-builds-and-hardware)) |
| Loading | Each plugin scans, instantiates, saves and restores state and processes silence in a sandboxed worker, per platform (Linux ARM64 under QEMU), with a timeout and memory limit; the measurements become estimates |
| Validation | The developer kit's conformance and realtime-safety tests, as warnings at first |
| Dependencies | Every required package and version is in this repository |
| Scanning | Files against a malware scanner's signatures (limits below) |
| Reproducibility | Where a package names its source and builder image, CI rebuilds it and compares, and the index says whether it matched (F-Droid's model; the builder is reproducible already) |

**Review:** data-only updates from a known publisher are merged once the checks pass. A new
publisher, and any package newly bringing plugin or app code, also needs a maintainer's review
([decided](#decided-2026-11-01)).

### Versions and yanking

- **A version is immutable.** Once published, the same package ID and version always mean the
  same manifest hash. A fix is a new version.
- **Yanking** marks a version with a reason. It is not offered for new installs or updates, and
  is shown as yanked where installed, but a lock that names it still fetches it, so a
  Performance that uses it keeps working. A publisher can yank their own versions.
- **Removal** of files happens only through a takedown (below), never as a publisher's routine
  act, as npm restricts unpublishing.

## Discovery

### In the Library

Each type's list in the Library app has **Get more**: the repositories' packages holding items
of that type that are not installed, searched and filtered like the rest, from the local index.
Opening one shows its page from the Store app's action (below).

### The Store app

- An app (RFC 0022), `oscairn.store`, in a package, replaceable like any other. It offers
  `oscairn.open.package` (open a package's page) and `oscairn.install.package`, which the
  Library, Settings' Installed content and the missing-content repair flow use.
- A tile on Home, and a card when there are updates (RFC 0030).
- Browse by type (plugins, Sounds, Performances, sample libraries, apps, themes,
  translations), by tag and by publisher; search over names, summaries and tags.
- A package's page: its details, audio previews and screenshots (fetched on demand), its
  versions, size on this device, license, publisher and fingerprint, signatures, whether its
  build was reproduced, and what it needs.
- **Install** shows RFC 0006's one import plan: what will be installed, from whom, its size,
  and what runs code.

### Compatibility

Filtered by the device's profile (RFC 0024) by default, with **Show all** beside it:

- a build for the device's `platform`, or no code at all;
- the system version the package requires (below);
- estimates within the device's budget (RFC 0025);
- for apps, a surface the device has (RFC 0030's `surfaces`);
- for items, a type the device knows or that the package brings.

What does not fit is shown grayed with the reason, never hidden without saying so.

### Control API

A `packages` namespace: `packages.repositories`, `packages.addRepository`,
`packages.removeRepository`, `packages.refresh`, `packages.search`, `packages.get`,
`packages.install` and `packages.updates`, with notifications for progress. Searching and
installing content need the Library permission (RFC 0026); adding a repository or installing
plugin and app code needs System.

## Updates

- **Refreshing** the index is a network fetch, off the audio path, by hand or on a schedule the
  musician sets (off by default on instruments). It never installs anything.
- **Plugins and content** install side by side with the version in use (RFC 0006). A
  Performance's lock never moves on its own. Updating one is explicit: **Use version 2.1** on
  a Performance, or **Update Performances using this** from the package, listing each one with
  what changes. Rehearsed Setlists keep their pinned revisions.
- **Libraries:** a new version of a locked library replaces its items with their new
  revisions; Performances keep working, since their parts are copies (RFC 0021). Items imported
  into the musician's own storage are untouched.
- **Apps, themes and translations** are not locked by documents. An update replaces the
  version installed, keeping the previous one until the new one has started once, and
  applies only when the app is closed. Per device: ask (the default), or update automatically.
  Never while playing.
- **System images** ([RFC 0028](0028-system-image-updates-boot-and-power.md)) are not packages.
  They have their own signing and are offered apart, though they may be hosted beside a
  repository. A system update does not change installed packages. A package declares the
  oldest system it runs on (`requires.oscairn`); one needing a newer system is shown with
  "needs system update". System images also carry trust store entries and revocations, so an
  offline device learns them with its next image (RFC 0006).

## Offline and several devices

- **A desktop fetches for a device.** Choose a device (paired, or from an inventory it exported,
  RFC 0006), browse with that device's profile as the filter, and queue packages. They are
  delivered the next time the two are paired, or written to a USB drive. The desktop downloads
  only that device's platform builds.
- **USB:** the drive carries the repository's signed root and index and only the queued
  blobs. The device checks the root's signature and every hash, as from the network.
  RFC 0006's missing-content request is fulfilled the same way.
- **A paired computer as a source** (RFC 0026): with the Library permission, a device fetches
  the index and blobs through the paired connection, from the computer's cache or through the
  computer's internet. The computer is a mirror: the device still checks every signature
  and hash itself.
- **Device to device:** one paired instrument can serve another its cached blobs the same way.

## Trust and safety

### Keys

| Key | Held by | Signs | Means |
| --- | --- | --- | --- |
| Repository root key | The repository's maintainers, offline | The index key | This is the repository |
| Repository index key | CI | The root file | This repository lists these packages, versions and publishers |
| Publisher key | The publisher | Packages, plugin builds, apps, items | The publisher made this |
| Device key | Each Oscairn install | Your own exports and repository | One of your devices made this |

A repository cannot forge a publisher's package: the publisher's signature would not verify.
A publisher cannot appear in a repository without it. RFC 0006's trust store gains one purpose,
`repositories`, beside `plugins` and `content`. A repository's listing counts as trust in its
publishers' keys for content, not for code: a new code publisher is approved once on the device
([decided](#decided-2026-11-01)).

### Takedown and revocation

- **The revocation list** is signed by the repository's index key and named in the root:
  revoked publisher keys, revoked build digests and revoked package versions, each with a
  reason. A device applies it on every refresh: a revoked build stays on disk but does not
  load, and what used it opens with a stand-in saying why (RFC 0006). The official list also
  travels in system images.
- **Takedown for safety** (malware, a build that harms devices): the version is revoked and its
  blobs removed.
- **Takedown for rights** (a copyright claim): the blobs are removed from the repository and
  its mirrors and the index marks the version removed. Copies already installed are not
  revoked: that is a legal matter between the parties, not a safety one.
- A published policy says who decides, how to report, and how a publisher appeals. Every
  takedown is a commit in the submissions repository, so the record is public.

### What scanning can do

A malware scanner finds known malware. It cannot prove native code safe. A plugin is
arbitrary code, and a determined author can hide what it does. What protects a device is
knowing who made a build (signatures), being able to stop it everywhere (revocation), and
running it isolated in a worker (RFC 0010).

### What a repository cannot promise

- That code is safe, or bug-free: a trusted publisher's bug is still a bug.
- That a package sounds or performs as described beyond the checks it passed.
- That the publisher has the rights to what they publish: that rests on their declaration and
  takedown.
- That a package stays available forever. Locks fetch yanked versions, but removed files are
  gone; keep backups (RFC 0006's repair routes).
- That a device without a clock sees the newest index.

### Privacy

No account to browse, download or install. Requests carry no device ID. The static host sees
addresses and files, as any web server does; the project publishes download counts only in
aggregate, if at all.

## Publisher trust and review

Whether a repository's listing counts as trust in its publishers' keys for plugins, and how
much human review a package with code gets, were decided from this comparison of what other
repositories do ([decided](#decided-2026-11-01)). Sources were read on
2026-11-01.

### What others do

Confirmed from each source:

| Repository | Publisher identity | Review of a first submission | Review of updates |
| --- | --- | --- | --- |
| [Flathub](https://docs.flathub.org/docs/for-app-authors/submission) | [Verified](https://docs.flathub.org/docs/for-app-authors/verification) by a token at a well-known URL or a DNS record on the app ID's domain, or by owning the repository for `io.github.` and similar IDs | Reviewed by volunteer reviewers on the pull request; permissions are reviewed separately | [Maintainers](https://docs.flathub.org/docs/for-app-authors/maintenance) test and merge their own updates. A change of permissions must be justified, and a build can be held until a moderator approves it |
| [F-Droid](https://f-droid.org/docs/Inclusion_Policy/) | None needed: it builds every app from public source itself | Contributors review apps by hand, including for non-free dependencies and tracking; undisclosed anti-features are rejected | Built from source by F-Droid, as for the first |
| [Debian](https://www.debian.org/doc/manuals/developers-reference/pkgs.en.html) | Uploads must be signed by a key in the Debian Developers or Debian Maintainers keyring | A new package is placed in the archive by hand, after the archive administrators review it | Updates to existing packages in `unstable` are processed automatically |
| [VS Code Marketplace](https://code.visualstudio.com/docs/configure/extensions/extension-runtime-security) | A publisher can earn a verified badge by proving domain ownership | A malware scan of every package, with several antivirus engines | The same scan of every update. VS Code itself asks the user to trust a third-party publisher on the first install of one of its extensions; later ones from that publisher install without asking |
| [npm](https://docs.npmjs.com/threats-and-mitigations) | An account; [provenance](https://docs.npmjs.com/generating-provenance-statements) is optional | No review before publishing. Names close to popular packages are blocked | Scanning for known malicious content; reported packages are removed after publishing |
| [Apple App Store](https://developer.apple.com/distribute/app-review/) | Not covered by the source | Reviewed | Every update is reviewed |
| [Google Play](https://support.google.com/googleplay/android-developer/answer/9859751) | Not covered by the source | Reviewed | Updates are reviewed and published "as soon as possible"; some apps and accounts get extended reviews of up to 7 days |
| [Homebrew casks](https://docs.brew.sh/Acceptable-Casks) | Not covered by the source | Judged against the acceptance policy; macOS apps must pass Gatekeeper's checks without it being bypassed | Malware and removal are decided case by case, with evidence; Homebrew does not certify that cask apps are safe |
| [VCV Library](https://github.com/VCVRack/library) | None stated | An open-source plugin is requested in an issue with its source URL; a closed-source one by email. A maintainer handles each request. No formal code review is described; plugins must follow VCV's ethics guidelines and not harm the user's computer or privacy | The developer comments with the new version and commit hash, and a maintainer updates the build |

Inferred, not stated by the sources:

- VCV appears to build open-source plugins itself from the commit given. Its
  [licensing page](https://vcvrack.com/manual/PluginLicensing) says "a source package which we
  will build for you", but in its section on commercial plugins.
- Homebrew cask changes arrive as pull requests that maintainers merge, as for the rest of
  Homebrew. The acceptance page does not describe the review step.
- The pattern among the free repositories is human review at the door (a new package or a new
  publisher) and automation after it, with a second human look when an update asks for more
  power (Flathub's permission changes). Only the commercial stores, with paid staff, review
  every update.
- Domain verification is the common way to give a publisher a badge (Flathub, VS Code). This
  RFC's `.well-known` check already does it.

### Proposal

Accepted by Jacob on 2026-11-01 ([decided](#decided-2026-11-01)).

- **A repository's listing counts as trust for content,** not for code. Sounds, Performances and
  samples from a listed publisher install as signed by a known key. Content installs anyway
  under RFC 0006's policy; the listing changes what is shown, not what is allowed.
- **For code, trust on first use, then pinned.** The first plugin or app from a publisher new to
  the device asks once, naming the publisher, its fingerprint and its repository, as VS Code
  does. Yes adds that key to the trust store for `plugins`. Later builds from that publisher
  install without asking. Only that key is accepted for that publisher: a build under the same
  publisher ID with another key is refused unless the old key signed the new one (RFC 0006's
  key replacement). A product may skip the question for its own manufacturer's repository by
  shipping its publishers' keys in the image's trust store, as RFC 0006 already allows.
- **Human review** of a publisher's first submission, and whenever a package newly adds code: a
  first plugin or app, or a plugin ID the package did not have before. Everything else, code
  updates from a known publisher included, is merged on the automated checks. This is Flathub's
  shape, with "adds code" in place of "adds permissions". Revocation is the backstop for what
  review misses.
- **A verified publisher badge** where the publisher ID's domain names its key at
  `.well-known/oscairn-publisher.json`, shown on the package's page and in the install
  question. A badge says who, not that the code is safe.

## Hosting and cost

The design needs only static storage, a CDN, a Git repository and CI. No server-side code: no
upload API, no accounts, no database. Nothing depends on one provider, because the signed root
names the blob base and every file is checked by its hash.

### GitHub first

The owner's decision of 2026-11-01 ([decided](#decided-2026-11-01)): the official repository
starts on GitHub, with no server.

| Part | Where | What it holds |
| --- | --- | --- |
| Submissions | The `oscairn/packages` repository; a pull request is a submission | `publishers/`, `packages/`, `revoked.json`, `repository-config.json` and the scripts |
| Checks and publishing | GitHub Actions: `check.yml` on pull requests, `publish.yml` on each merge to `main` | Builds `oscairn-sign` at a pinned commit, checks, builds the index, signs the root |
| The signed root and index | GitHub Pages | `repository.json`, `repository.sig`, `index/`, `revocations/` |
| Package files | GitHub Releases assets: a release `blobs-<ab>` per first two hex digits, each asset named by its SHA-256 | The blob base is `https://github.com/oscairn/packages/releases/download/blobs-`, so a blob is at `blobs-<ab>/<sha256>` under it |

The template for the repository, its workflows and its index builder are in
[tools/package-repository/template](../../tools/package-repository/template/README.md).
Release downloads redirect to another GitHub host, so devices follow redirects; the hash is
what they trust, not the host.

GitHub's limits, as its documentation gives them (read 2026-11-01):

- **Pages:** a published site may be no larger than 1 GB, and the page has given a soft
  bandwidth limit of 100 GB a month; check the
  [current Pages limits](https://docs.github.com/en/pages/getting-started-with-github-pages/github-pages-limits)
  rather than treating that figure as fixed. Pages is not for running an online business,
  e-commerce or SaaS, and for a site over quota GitHub suggests a CDN in front, Releases, or
  another host. The index is small (the target is under 2 MB) and fits. Paid content, if ever
  added ([decided](#decided-2026-11-01)), could not be sold through Pages.
- **Releases:** each file must be under 2 GiB, with no limit on a release's total size or
  bandwidth, and at most 1000 assets in a release
  ([About releases](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases),
  [About large files on GitHub](https://docs.github.com/en/repositories/working-with-files/managing-large-files/about-large-files-on-github)).
  Users report failures near 2 GiB, so the index builder refuses an archive over 1,900 MiB. A
  file in the repository itself must be under 100 MiB, so larger archives are linked from a
  submission, not committed.
- **Acceptable use:** bandwidth limits vary by feature, and usage significantly above similar
  users' can lead GitHub to throttle file hosting or suspend the account until it drops
  ([GitHub Acceptable Use Policies](https://docs.github.com/en/site-policy/acceptable-use-policies/github-acceptable-use-policies),
  "Excessive Bandwidth Use"). That covers package files on Releases, not only Pages. The
  publish job reports total release downloads in its summary, so the project sees growth
  coming.
- **Malware:** the same policies forbid using GitHub to deliver malware. A malicious plugin
  that slips past the checks could get the repository's releases or the account disabled. That
  is one more reason for [revocation and fast takedown](#takedown-and-revocation), and for
  keeping the blob base movable.

### Moving package files to object storage

Package files move to object storage behind a CDN under a custom domain, such as Cloudflare
R2, when the first of these comes:

- a package file of 2 GiB or more that the official repository hosts (a large sample library);
- Pages bandwidth near its soft limit, or release downloads growing toward what GitHub's
  acceptable use allows;
- more blobs than the releases hold comfortably (256 releases of 1000 assets), once releases
  are stored in pieces.

[R2](https://developers.cloudflare.com/r2/pricing/) charges no egress fees. Its free tier is
10 GB-month of storage, 1 million Class A and 10 million Class B operations a month; beyond it,
$0.015 per GB-month, and $4.50 and $0.36 per million Class A and Class B operations. Moving is a
copy and one field: copy every blob to `blobs/<ab>/<sha256>` under the new domain, set
`blobBase` in `repository-config.json`, and merge. The next signed root names the new base.
Devices change nothing, because every file is checked by its hash. The root and index may stay
on Pages or move too.

### What costs what

| Part | On GitHub | After the move |
| --- | --- | --- |
| Submissions and CI | Free for a public repository | The same |
| Root and index | Pages, free within its limits | The same, or the CDN |
| Package files | Releases, free, within GitHub's acceptable use | R2: storage past 10 GB-month and operations past the free tier; no egress fees |
| A custom domain | Optional | A domain's yearly fee |

Most bandwidth is sample libraries. The official repository may host a few; the rest are
hosted by their publishers through blob bases ([decided](#decided-2026-11-01)). Sample
deduplication and per-platform downloads keep the rest down.

### Mirrors

Anyone can mirror the repository by copying the static tree: the root, its signature, `index/`,
`revocations/` and the blobs, with rsync, a download of the Pages site and the releases, or a
CDN's own copy. The root lists mirrors and devices try them in turn. Mirrors need no trust: a
device checks the root's signature and every file's hash, and refuses a root with a lower
serial than one it has seen ([freshness](#signatures-and-freshness)).

### What the project sets up

1. **The repository:** create `oscairn/packages` in the GitHub organization and copy in the
   template. Protect `main`: pull requests only, the Check workflow required, no force pushes
   (the serial is the number of commits on `main`).
2. **The pinned tools:** set `OSCAIRN_REPOSITORY` and `OSCAIRN_COMMIT` in both workflows to the
   Oscairn repository and a commit of it that has `oscairn-sign file`.
3. **The Pages site:** Settings, Pages, Source: GitHub Actions. A custom domain is optional.
4. **The blob base:** nothing to create; the publish job makes each `blobs-<ab>` release on
   first use, and `repository-config.json`'s `blobBase` names them.
5. **The keys and the secret,** below.

### Keys

- **The root key** is made on an offline machine and stays offline with the maintainers, with a
  backup. It signs only the index key. Devices trust the repository by its fingerprint.
- **The index key** is made the same way and signed by the root key (`oscairn-sign file
  root.key index-key.pub index-key.sig`, the file holding its 64 hex digits and a newline). It
  is stored as the `OSCAIRN_INDEX_KEY` Actions secret, and the local copy deleted. The root
  key, the index key and its signature go in `repository-config.json`. The publish job writes
  the secret to a file only it can read, signs the root, deletes the file, and checks that the
  signer is the index key the config names.
- **Rotation:** a new index key is made and signed by the root key the same way, the secret and
  the config are replaced, and the old key goes in `revoked.json`. A stolen index key is
  rotated at once. Replacing the root key means devices trust a new fingerprint, through a
  system image's trust store or by the user. Key replacement for publishers is not built yet,
  so the checks refuse a changed publisher key.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Format and folders | `repository.json`, index, deltas and blobs; `oscairn repo build`; installing from a folder repository, a folder package and an `.olib` read in place; streamed installs | A device with no network installs a package from a USB folder; a changed blob, a root with a bad signature and a root with a lower serial are each refused; a folder package with a changed file is refused and leaves nothing behind; an `.olib` on USB installs with free space of only its installed size plus its largest entry; pulling the drive midway leaves the device as it was |
| 2. HTTPS | Refresh with deltas, mirrors, resumable downloads, per-platform fetch | A refresh with one changed package downloads the root and one delta; a mirror serving a tampered blob is refused and the next mirror used; an interrupted download resumes |
| 3. Publishing | `oscairn publish`, the submissions repository, CI checks, immutable versions, yanking | A signed release published through a pull request installs on a device; an unsigned one, a re-used version and a missing claimed platform build each fail in CI; a yanked version is not offered but its lock still fetches it |
| 4. Revocation and takedown | Revocation lists, the takedown policy | A build revoked in the list stops loading on refresh and its Performance opens with a stand-in; the same from a system image on an offline device |
| 5. Discovery | Get more, the Store app and its actions, compatibility filters, `packages.*` | A search by type on an instrument answers from the local index with no network; a package without a build for the device is grayed with the reason |
| 6. Updates | Update lists, explicit lock updates, app updates | Installing a new version leaves every Performance's lock as it was until **Use version** is chosen; an app update applies only once the app is closed |
| 7. Offline and multi-device | Fetch for a device, USB subsets, a paired computer as source | A desktop queues packages for a KICK profile and writes a USB drive with only its builds; the device installs them; the same through a paired connection |
| 8. Public | Hosting, CDN, mirrors, the policy, the root key ceremony | The official repository serves from two mirrors, and a device survives one going away |

Steps 1 and 3 need RFC 0006's signatures built. Step 5 needs RFC 0022 step 2 (apps in their own
processes) for the Store app, and RFC 0023 step 2 for types in queries. Step 4 comes before
step 8: nothing public without a way to stop it.

## Decided 2026-11-01

Jacob's decisions on six of the open questions:

- **Publisher trust:** a repository's listing counts as trust for content, not for code. Code is
  approved once per publisher on the device, then pinned to its key, as the
  [proposal](#proposal) describes.
- **Review:** human review of a publisher's first submission and whenever a package newly adds
  code; automated checks after that; a verified publisher badge for publishers who prove their
  domain ([proposal](#proposal)).

- **Paid content:** the official repository does not include paid content, for now. If it is
  ever added, paid packages must be marked as paid in the index, and search must offer a filter
  that excludes them. The filter's default is decided then.
- **Large sample libraries:** the official repository may host a few. Others are hosted by their
  publishers through the blob base ([package files](#package-files)).
- **A manufacturer replacing or hiding the official repository:** allowed, but heavily
  discouraged. The official repository stays one switch away in Settings, and the
  documentation and the profile schema flag hiding it as discouraged
  ([several repositories](#several-repositories)).
- **Hosting:** GitHub first, with no server. Submissions are pull requests to
  `oscairn/packages`; GitHub Actions checks them and, on merge, builds and signs the index with
  the index key from Actions secrets, while the root key stays offline. The signed index is on
  GitHub Pages and package files are Releases assets. Package files move to object storage
  behind a CDN (Cloudflare R2) under a custom domain when files over 2 GiB or bandwidth require
  it; devices do not change ([hosting](#hosting-and-cost)).

## Open questions

- **Built-in apps:** do apps that ship in the system image also update from the repository
  between images, or only with the image?
- **Publisher IDs:** verified by domain (reverse-DNS), or names the project hands out?
- **Reproducible builds:** reported only, or required of open-source packages in the official
  repository?
- **Popularity:** no download counts, or aggregate counts from the CDN's logs to sort by?

## Ruled out

- **Server-side code in the first version:** accounts, upload APIs, ratings and reviews all
  need one. Static files and CI cover publishing and installing.
- **An account to browse or install.**
- **Unsigned packages in a repository.**
- **Changing a published version,** even by its publisher.
- **Updates that move a Performance's lock,** or that install while playing.
- **Install scripts:** packages are files; nothing runs on install (RFC 0006).
- **Looking in other repositories for a missing package of the same name.**
- **The repository's signature standing in for the publisher's:** they say different things.
- **Telemetry** from devices to the repository.

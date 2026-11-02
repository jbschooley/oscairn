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

## Progress

Checked when built and tested; the sections below say how.

- [ ] 1. The repository format, and installing from a folder or USB drive
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

## A repository

### What is in it

```
repository.json        the root: who this is, its keys, mirrors, and the current index
repository.sig         the root's signature
index/<sha256>.json.gz     the full index, named by its hash
index/<sha256>.delta.gz    changes from one index to the next
details/<sha256>.json      a package's long description, screenshots and previews
revocations/<sha256>.json  the revocation list
blobs/<sha256>             package files: manifests, items, samples, plugin builds
```

- **Everything but the root is named by its SHA-256** and never changes. A CDN can cache it
  forever, and a mirror copies it once.
- **The root is the only file that changes.** It is small (a few kilobytes) and names the
  current index, the deltas, the revocation list and the mirrors by hash and size.
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
  Ed25519, as in [RFC 0006](0006-packages-and-portability.md#signatures).
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

- `submissions/<publisher>/<package>/<version>.json` holds the signed manifest and where the
  files are (a URL per blob, often the publisher's own release page, or a blob base).
- **A publisher's first submission** also adds `publishers/<publisher>.json`: the publisher's
  ID, name and key. A reverse-DNS publisher ID is verified by a file at
  `https://<domain>/.well-known/oscairn-publisher.json` naming the key, as Flathub verifies
  app IDs. Later submissions must be signed by that key or one it signed.
- **CI** fetches the files, checks every hash against the signed manifest, runs the checks, and
  posts the results on the pull request.
- **On merge**, CI copies the blobs into storage (unless the package uses its own blob base),
  writes the new index and root, signs the root with the index key, and uploads. Merges are
  serialized, so serials never collide.

A third party running its own repository uses the same tool: `oscairn repo build` turns a
folder of signed archives into a repository, ready to upload anywhere.

### Checks

| Check | What it proves |
| --- | --- |
| Schema | The manifest, item documents and type manifests are valid (RFC 0023) |
| Archive safety | Every rule for reading an untrusted archive ([format](../library-archive.md#reading-one)) |
| Signatures | The package signature and every build signature verify with the publisher's key |
| Platforms | Each platform the package claims has its build; a first-party package has one for every supported profile ([RFC 0006](0006-packages-and-portability.md#platform-builds-and-hardware)) |
| Loading | Each plugin scans, instantiates, saves and restores state and processes silence in a sandboxed worker, per platform (Linux ARM64 under QEMU), with a timeout and memory limit; the measurements become estimates |
| Validation | The developer kit's conformance and realtime-safety tests, as warnings at first |
| Dependencies | Every required package and version is in this repository |
| Scanning | Files against a malware scanner's signatures (limits below) |
| Reproducibility | Where a package names its source and builder image, CI rebuilds it and compares, and the index says whether it matched (F-Droid's model; the builder is reproducible already) |

**Review:** data-only updates from a known publisher are merged once the checks pass. A new
publisher, and any package bringing plugin or app code, also needs a maintainer's review. How
much review is an open question.

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
`repositories`, beside `plugins` and `content`. Whether a repository's listing also counts as
trust in its publishers' keys for plugins is an open question.

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

## Hosting and cost

What the project runs for the first version:

- **Static storage** for blobs, indexes and roots, behind a **CDN**. Any object store and CDN
  will do; nothing depends on one provider.
- **A Git repository** for submissions, publishers and policy, and **CI** to check, build the
  index and sign it. The index key lives in CI's secrets; the root key stays offline with the
  maintainers.
- **Mirrors:** anyone can copy the static tree (rsync, or the CDN's own copy). The root lists
  mirrors; devices try them in turn. Mirrors need no trust.

No server-side code: no upload API, no accounts, no database. Costs are storage and outgoing
bandwidth, most of it sample libraries. Blob bases let publishers host large libraries
themselves; sample deduplication and per-platform downloads keep the rest down.

## Build order and gates

| Step | Deliverable | Gate |
| --- | --- | --- |
| 1. Format and folders | `repository.json`, index, deltas and blobs; `oscairn repo build`; installing from a folder | A device with no network installs a package from a USB folder; a changed blob, a root with a bad signature and a root with a lower serial are each refused |
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

## Open questions

- **Publisher trust:** does a repository's listing count as trust in its publishers' keys for
  plugins, or does the musician approve each new plugin publisher once? A product may want the
  first; a desktop user may want the second.
- **Review:** a maintainer's review for every package with code, for every publisher's first
  submission only, or automated checks alone after the first?
- **Paid content:** list commercial packages with a link to the vendor (their licensing stays
  theirs), or sell through the repository (which needs a server)?
- **Large libraries:** does the official repository host big sample libraries, or ask their
  publishers to host them with blob bases?
- **Products:** may a manufacturer make its own repository the default on its product, and hide
  or remove the official one?
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

# Oscairn packages

Submissions to Oscairn's official package repository
([RFC 0031](https://github.com/jbschooley/oscairn/blob/main/docs/rfcs/0031-package-repository-and-distribution.md)).
A pull request is a submission. GitHub Actions checks it, and on merge builds and signs the index,
puts package files in this repository's releases and the signed index on GitHub Pages. There is
no server.

## Submitting a package

You need `oscairn-sign`, built with Oscairn (`cmake --build build --target oscairn-sign`).

1. **Make a key**, once, and keep it safe. It never leaves your machine:

   ```sh
   oscairn-sign key ~/oscairn/publisher.key
   ```

   It prints your key's ID (its fingerprint) and public key.

2. **Sign each plugin build**, then the whole archive. A repository requires both:

   ```sh
   oscairn-sign build ~/oscairn/publisher.key clap com.example.organ 2.1.0 linux-arm64 Organ.clap
   # export the library archive (it carries Organ.clap.sig), then:
   oscairn-sign archive ~/oscairn/publisher.key com.example.organ-2.1.0.olib
   oscairn-sign verify com.example.organ-2.1.0.olib
   ```

3. **Open a pull request** adding:

   - on your first submission, `publishers/<your publisher ID>.json` ([below](#publishers)), and
     `https://<your domain>/.well-known/oscairn-publisher.json` naming your public key;
   - `packages/<package ID>/<version>.json` ([below](#packages)), with a link to the archive
     (your own release page, for example), or the archive itself beside it as
     `packages/<package ID>/<version>.olib` if it is under 100 MiB, which is the largest file
     GitHub accepts in a repository.

The checks report on the pull request. A maintainer reviews a publisher's first submission and
any package that newly brings plugin or app code; other submissions are merged once the checks
pass.

**A version never changes once merged.** Publish a fix as a new version. To withdraw one, set its
`yanked` to a reason: it is no longer offered, but Performances that use it still get it.

## The layout

### Publishers

`publishers/com.example.json`, one per publisher, named by its ID:

```json
{
  "format": "oscairn.repository-publisher",
  "version": 1,
  "id": "com.example",
  "name": "Example Audio",
  "key": "<your public key, 64 hex digits>",
  "verification": "https://example.com/.well-known/oscairn-publisher.json"
}
```

A publisher's key cannot be changed by a pull request yet.

### Packages

`packages/com.example.organ/2.1.0.json`, one per version. The package ID is under the
publisher's ID:

```json
{
  "format": "oscairn.repository-release",
  "version": 1,
  "id": "com.example.organ",
  "release": "2.1.0",
  "publisher": "com.example",
  "summary": "A tonewheel organ and rotary speaker",
  "license": "GPL-3.0-or-later",
  "tags": ["organ", "keys"],
  "channel": "stable",
  "platforms": ["linux-arm64", "macos-arm64"],
  "requires": { "oscairn": "1.4" },
  "archive": { "url": "https://example.com/releases/organ-2.1.0.olib", "sha256": "<64 hex digits>", "size": 21534720 },
  "yanked": null
}
```

- `archive`: an `https://` `url`, or a `path` naming the archive beside this file; its SHA-256
  and size in bytes. Under 1,900 MiB, since release assets must be under 2 GiB.
- `platforms`: each platform the package has plugin builds for; every plugin must have a build
  for each. Leave it out for a package with no plugins.
- `channel`: `stable` (the default) or `beta`.
- `license`: an SPDX expression.

### Revocations

`revoked.json` lists revoked publisher keys, plugin build digests and package versions, each
with a `reason`. Maintainers change it, following the takedown policy.

## For maintainers

### What the checks do

`.github/workflows/check.yml` builds `oscairn-sign` at a pinned commit of Oscairn and runs
`scripts/build-index.py --check` from the base branch: every record's schema, each archive's
hash, archive safety and every signature through `oscairn-sign verify`, the archive and every
build signed by the publisher's key, each claimed platform's build, that a package keeps the
publisher of its other releases and that publisher IDs do not nest, and no published version or
publisher key changed. Each check RFC 0031 lists that is not built yet is its own step that says
so with a warning. Changes outside `publishers/`, `packages/` and `revoked.json` need a
maintainer's review: a pull request can change the workflows it runs under, so `CODEOWNERS`
requires a maintainer for `.github/`, `scripts/`, `revoked.json`, `repository-config.json` and
any `.gitattributes`, and publish re-checks that no published release or publisher key changed
since the commit tagged `published`, whatever the pull request's own check said. It reads that
commit's records as raw blobs, so no `.gitattributes` can hide or rewrite them, and fails when
the tag is missing once any release or publisher is recorded: if the tag is lost, put it back on
the last published commit. `CODEOWNERS` holds only while `main`'s branch protection requires
review from Code Owners.

### Keys

- **The root key** is made offline and stays offline with the maintainers. It signs only the
  index key.
- **The index key** is in the `OSCAIRN_INDEX_KEY` Actions secret. It signs the root
  (`repository.json`) on every publish.

Making them, on an offline machine:

```sh
oscairn-sign key root.key            # prints the root key's ID and public key
oscairn-sign key index.key           # the same for the index key
printf '%s\n' <index public key> > index-key.pub
oscairn-sign file root.key index-key.pub index-key.sig
```

Put the root public key in `repository-config.json` as `rootKey`, and the index public key and
the signature from `index-key.sig` as `indexKey`. Put the whole of `index.key` in the
`OSCAIRN_INDEX_KEY` secret, then delete that copy. **Replacing the index key** is the same with a
new `index.key`; add the old one to `revoked.json`'s `keys`.

### Setting up

See RFC 0031, "Hosting and cost". Protect `main`: pull requests only, with review from Code
Owners required, the Check workflow required, and no force pushes.

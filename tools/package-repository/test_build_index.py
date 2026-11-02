#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The package repository template's index builder (RFC 0031) on a sample tree.

usage: test_build_index.py <oscairn-sign>

Makes throwaway keys, a signed archive with one plugin build, a publisher and a release, builds
the index, signs and checks the root with oscairn-sign, and checks that a bad release, a changed
release and a tampered root are each refused. Keys live only in a temporary folder.
"""
import gzip
import hashlib
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
BUILDER = HERE / "template" / "scripts" / "build-index.py"
failures = 0


def check(ok, what):
    global failures
    print(("ok   " if ok else "FAIL ") + what)
    failures += not ok


def run(*command):
    return subprocess.run([str(c) for c in command], capture_output=True, text=True, timeout=300)


def main(tool):
    temp = Path(tempfile.mkdtemp(prefix="oscairn-repo-test-"))
    try:
        keys = temp / "keys"
        made = {}
        for name in ("publisher", "root", "index"):
            out = run(tool, "key", keys / f"{name}.key")
            assert out.returncode == 0, out.stderr
            _, key_id, public = out.stdout.split()
            made[name] = (key_id, public)

        # A signed archive: one CLAP build for linux-arm64, signed by the publisher, and the whole.
        def archive(path, signed=True):
            build = temp / "Organ.clap"
            build.write_bytes(b"not really a plugin")
            Path(str(build) + ".sig").unlink(missing_ok=True)
            out = run(tool, "build", keys / "publisher.key", "clap", "com.example.organ", "2.1.0", "linux-arm64", build)
            assert out.returncode == 0, out.stderr
            digest = out.stdout.split()[5]
            inside = "Plugins/clap/com.example.organ@2.1.0/linux-arm64/Organ.clap"
            manifest = {
                "format": "oscairn.library", "version": 3, "id": "com.example.organ", "name": "Example Organ", "kind": "library",
                "items": [],
                "plugins": [{"format": "clap", "id": "com.example.organ", "version": "2.1.0", "name": "Example Organ", "builds": [
                    {"platform": "linux-arm64", "path": inside, "sha256": digest,
                     "signatures": json.loads(Path(str(build) + ".sig").read_text()) if signed else []}]}],
                "files": {inside: hashlib.sha256(build.read_bytes()).hexdigest()},
            }
            with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
                z.writestr("library.json", json.dumps(manifest))
                z.write(build, inside)
            if signed:
                out = run(tool, "archive", keys / "publisher.key", path)
                assert out.returncode == 0, out.stderr
            return path

        # The submissions repository, from the template.
        repo = temp / "packages-repo"
        shutil.copytree(HERE / "template", repo)
        signature = temp / "index-key.sig"
        (temp / "index-key.pub").write_text(made["index"][1] + "\n")
        out = run(tool, "file", keys / "root.key", temp / "index-key.pub", signature)
        check(out.returncode == 0, "the root key signs the index key: " + out.stderr.strip())
        config = json.loads((repo / "repository-config.json").read_text())
        config.update(rootKey=made["root"][1], indexKey={"key": made["index"][1], "signature": json.loads(signature.read_text())[0]["signature"]})
        (repo / "repository-config.json").write_text(json.dumps(config))
        (repo / "publishers" / "com.example.json").write_text(json.dumps({
            "format": "oscairn.repository-publisher", "version": 1, "id": "com.example", "name": "Example Audio",
            "key": made["publisher"][1], "verification": "https://example.com/.well-known/oscairn-publisher.json"}))
        release_dir = repo / "packages" / "com.example.organ"
        release_dir.mkdir()
        olib = archive(release_dir / "2.1.0.olib")
        data = olib.read_bytes()
        release = {
            "format": "oscairn.repository-release", "version": 1, "id": "com.example.organ", "release": "2.1.0",
            "publisher": "com.example", "summary": "A tonewheel organ", "license": "GPL-3.0-or-later", "tags": ["organ"],
            "platforms": ["linux-arm64"],
            "archive": {"path": "2.1.0.olib", "sha256": hashlib.sha256(data).hexdigest(), "size": len(data)}, "yanked": None}
        (release_dir / "2.1.0.json").write_text(json.dumps(release))

        def build(out_dir, *extra):
            return run(sys.executable, "-I", BUILDER, "--repo", repo, "--oscairn-sign", tool, "--cache", temp / "cache", "--out", out_dir, "--serial", "7", *extra)

        # A good build.
        site = temp / "site"
        out = build(site)
        check(out.returncode == 0, "a good tree builds: " + out.stderr.strip())
        root = json.loads((site / "repository.json").read_text())
        gz = (site / root["index"]["path"]).read_bytes()
        check(hashlib.sha256(gz).hexdigest() == root["index"]["sha256"] and len(gz) == root["index"]["size"] and root["index"]["path"] == f"index/{root['index']['sha256']}.json.gz",
              "the root names the index by its hash and size")
        index = json.loads(gzip.decompress(gz))
        package = index["packages"][0]
        version = package["versions"][0]
        with zipfile.ZipFile(olib) as z:
            manifest = z.read("library.json")
        check(index["format"] == "oscairn.repository-index" and index["serial"] == 7 and root["serial"] == 7, "the index and root carry the serial")
        check(package["id"] == "com.example.organ" and package["name"] == "Example Organ" and package["publisher"]["key"] == made["publisher"][0],
              "the package, its name from the manifest and its publisher's fingerprint")
        check(version["manifest"] == {"sha256": hashlib.sha256(manifest).hexdigest(), "size": len(manifest)}, "the version names its manifest by hash")
        check(version["types"] == {"clap": 1} and set(version["platforms"]) == {"linux-arm64"} and version["platforms"]["linux-arm64"]["installed"] == len(b"not really a plugin"),
              "types and per-platform sizes: " + json.dumps(version["platforms"]))
        rev = (site / root["revocations"]["path"]).read_bytes()
        check(hashlib.sha256(rev).hexdigest() == root["revocations"]["sha256"] and json.loads(rev)["format"] == "oscairn.repository-revocations", "the revocation list, by hash")
        blob = release["archive"]["sha256"]
        check((site / "blobs.txt").read_text() == blob + "\n" and (temp / "cache" / blob).read_bytes() == data, "the archive is a blob in the cache by its hash")
        check(root["indexKey"]["key"] == made["index"][1] and root["rootKey"] == made["root"][1], "the root carries the root key and the delegated index key")
        check(build(temp / "again").returncode == 0 and (temp / "again" / "repository.json").read_bytes() == (site / "repository.json").read_bytes(), "the build is reproducible")

        # The root, signed by the index key and checked; a changed root is refused.
        out = run(tool, "file", keys / "index.key", site / "repository.json", site / "repository.sig")
        check(out.returncode == 0, "oscairn-sign file signs the root")
        out = run(tool, "verify-file", site / "repository.json", site / "repository.sig")
        check(out.returncode == 0 and out.stdout.split()[1] == made["index"][0], "oscairn-sign verify-file says the index key signed it")
        (site / "repository.json").write_bytes((site / "repository.json").read_bytes().replace(b'"serial":7', b'"serial":9'))
        check(run(tool, "verify-file", site / "repository.json", site / "repository.sig").returncode != 0, "a changed root is refused")

        # A delegation signed by the wrong key is refused, and --check does not need keys.
        bad = dict(config, rootKey=made["publisher"][1])
        (repo / "repository-config.json").write_text(json.dumps(bad))
        out = build(temp / "bad-delegation")
        check(out.returncode != 0 and "index key does not verify" in out.stderr, "an index key not signed by the root key is refused")
        (repo / "repository-config.json").write_text(json.dumps(dict(config, rootKey="", indexKey={})))
        check(build(temp / "nokeys").returncode != 0, "a build to publish needs the keys")
        check(build(temp / "nokeys-check", "--check").returncode == 0, "a pull request's check does not")
        (repo / "repository-config.json").write_text(json.dumps(config))

        # A published release never changes, but for yanked.
        previous = temp / "previous"
        shutil.copytree(repo, previous)
        (release_dir / "2.1.0.json").write_text(json.dumps(dict(release, yanked="breaks presets")))
        check(build(temp / "yanked", "--previous", previous).returncode == 0, "yanking a release is allowed")
        (release_dir / "2.1.0.json").write_text(json.dumps(dict(release, summary="Changed")))
        out = build(temp / "changed", "--previous", previous)
        check(out.returncode != 0 and "a published release changed" in out.stderr, "changing a published release is refused")
        (release_dir / "2.1.0.json").write_text(json.dumps(release))

        # Releases each refused: a missing claimed platform, a wrong hash, an unsigned archive.
        def refused(changed, expect, what, olib_bytes=None):
            (release_dir / "2.1.0.json").write_text(json.dumps(changed))
            if olib_bytes is not None:
                olib.write_bytes(olib_bytes)
            shutil.rmtree(temp / "cache", ignore_errors=True)
            out = build(temp / "refused")
            check(out.returncode != 0 and expect in out.stderr, what + ": " + out.stderr.strip())
            (release_dir / "2.1.0.json").write_text(json.dumps(release))
            olib.write_bytes(data)

        refused(dict(release, platforms=["linux-arm64", "macos-arm64"]), "claims macos-arm64", "a claimed platform without its build is refused")
        refused(dict(release, archive=dict(release["archive"], sha256="0" * 64)), "not the size and SHA-256", "an archive with another hash is refused")
        unsigned = archive(temp / "unsigned.olib", signed=False).read_bytes()
        refused(dict(release, archive=dict(release["archive"], sha256=hashlib.sha256(unsigned).hexdigest(), size=len(unsigned))),
                "not signed by its publisher's key", "an unsigned archive is refused", unsigned)
        refused(dict(release, publisher="org.other"), "no publishers/org.other.json", "a release from an unknown publisher is refused")
        refused(dict(release, archive=dict(release["archive"], size=2**31)), "under 2 GiB", "an archive too large for a release asset is refused")
    finally:
        shutil.rmtree(temp, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(Path(sys.argv[1]).resolve()))

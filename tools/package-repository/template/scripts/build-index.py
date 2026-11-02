#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Builds an Oscairn repository (RFC 0031) from packages/, publishers/ and revoked.json.

Checks every release on the way: the records' schema, each archive's hash, archive safety and
every signature (through `oscairn-sign verify`), the publisher's key, and the platform builds
each release claims. With --previous, also that no published release changed. Writes the
repository's static tree to --out:

    repository.json            the root, unsigned: sign it with `oscairn-sign file`
    index/<sha256>.json.gz     the index
    revocations/<sha256>.json  the revocation list
    blobs.txt                  the SHA-256 of every archive, each in --cache under that name

Standard library only. Exits 1 and lists every problem when anything is wrong.
"""
import argparse
import gzip
import hashlib
import json
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path

MAX_ARCHIVE = 1900 * 2**20  # GitHub release assets must be under 2 GiB; headroom below it
MAX_RECORD = 1 << 20
PLATFORMS = {"linux-arm64", "linux-x86_64", "macos-arm64", "macos-x86_64", "windows-x86_64"}
ID = re.compile(r"^[A-Za-z0-9_-]+(\.[A-Za-z0-9_-]+)+$")  # reverse-DNS
VERSION = re.compile(r"^[A-Za-z0-9][A-Za-z0-9.+_-]{0,63}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")
HEX128 = re.compile(r"^[0-9a-f]{128}$")


class Problems(list):
    def add(self, where, what):
        self.append(f"{where}: {what}")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def key_id(key):
    return sha256(bytes.fromhex(key))[:16]


def load(path, problems):
    try:
        if path.stat().st_size > MAX_RECORD:
            raise ValueError("over 1 MiB")
        value = json.loads(path.read_bytes())
        if not isinstance(value, dict):
            raise ValueError("not a JSON object")
        return value
    except (OSError, ValueError) as e:
        problems.add(path, f"cannot be read: {e}")
        return None


def text(record, field, where, problems, pattern=None):
    value = record.get(field)
    if not isinstance(value, str) or not value or (pattern and not pattern.match(value)):
        problems.add(where, f"`{field}` is missing or malformed")
        return None
    return value


def check_format(record, name, where, problems):
    if record.get("format") != name or record.get("version") != 1:
        problems.add(where, f"not format {name}, version 1")
        return False
    return True


def read_publishers(root, problems):
    publishers = {}
    for path in sorted((root / "publishers").glob("*.json")):
        p = load(path, problems)
        if p is None or not check_format(p, "oscairn.repository-publisher", path, problems):
            continue
        pid = text(p, "id", path, problems, ID)
        text(p, "name", path, problems)
        key = text(p, "key", path, problems, HEX64)
        verification = text(p, "verification", path, problems)
        if verification and not verification.startswith("https://"):
            problems.add(path, "`verification` is not an https:// address")
        if pid and pid != path.stem:
            problems.add(path, f"its file is not named {pid}.json")
        if pid and key:
            publishers[pid] = p
    return publishers


def fetch(record, record_path, cache, problems):
    """The archive in --cache under its SHA-256, copied or downloaded and checked, or None."""
    archive = record.get("archive")
    if not isinstance(archive, dict):
        problems.add(record_path, "`archive` is missing")
        return None
    digest = text(archive, "sha256", record_path, problems, HEX64)
    size = archive.get("size")
    if not isinstance(size, int) or not 0 < size <= MAX_ARCHIVE:
        problems.add(record_path, f"`archive.size` is missing or over {MAX_ARCHIVE} bytes (GitHub release assets must be under 2 GiB)")
        return None
    if not digest:
        return None
    target = cache / digest
    if target.is_file() and sha256_file(target) == digest:
        return target
    partial = cache / (digest + ".partial")
    try:
        if "path" in archive:
            name = archive["path"]
            if not isinstance(name, str) or "/" in name or "\\" in name or name.startswith("."):
                problems.add(record_path, "`archive.path` must name a file beside the record")
                return None
            shutil.copyfile(record_path.parent / name, partial)
        elif isinstance(archive.get("url"), str) and archive["url"].startswith("https://"):
            with urllib.request.urlopen(archive["url"], timeout=60) as response, open(partial, "wb") as out:
                received = 0
                for chunk in iter(lambda: response.read(1 << 20), b""):
                    received += len(chunk)
                    if received > size:
                        raise ValueError("larger than `archive.size`")
                    out.write(chunk)
        else:
            problems.add(record_path, "`archive` needs a `path` beside the record or an https:// `url`")
            return None
        if partial.stat().st_size != size or sha256_file(partial) != digest:
            raise ValueError("not the size and SHA-256 the record gives")
        partial.replace(target)
        return target
    except (OSError, ValueError) as e:
        partial.unlink(missing_ok=True)
        problems.add(record_path, f"the archive cannot be fetched: {e}")
        return None


def verify(tool, archive, where, problems):
    """`oscairn-sign verify`: archive safety, every hash and every signature. Its report, or None."""
    try:
        run = subprocess.run([tool, "verify", str(archive)], capture_output=True, text=True, timeout=900)
    except (OSError, subprocess.TimeoutExpired) as e:
        problems.add(where, f"oscairn-sign verify did not run: {e}")
        return None
    if run.returncode != 0:
        problems.add(where, f"the archive is refused: {run.stderr.strip()}")
        return None
    report = {"archive": None, "builds": []}
    for line in run.stdout.splitlines():
        words = line.split()
        if words[:1] == ["archive"] and len(words) == 3:
            report["archive"] = (words[1], words[2])
        elif words[:1] == ["build"] and len(words) == 6:
            report["builds"].append((words[1], words[2], words[3], words[4], words[5].split(",")))
    return report


def facts(archive):
    """What the index says of an archive: its manifest's hash and size, types and sizes."""
    with zipfile.ZipFile(archive) as z:  # only after oscairn-sign verify accepted it
        manifest_bytes = z.read("library.json")
        manifest = json.loads(manifest_bytes)
        build_paths = {b["path"]: b["platform"] for p in manifest["plugins"] for b in p["builds"]}
        platforms, common = {}, {"size": 0, "installed": 0}
        for info in z.infolist():
            platform = next((pl for path, pl in build_paths.items() if info.filename == path or info.filename.startswith(path + "/")), None)
            sizes = platforms.setdefault(platform, {"size": 0, "installed": 0}) if platform else common
            sizes["size"] += info.compress_size
            sizes["installed"] += info.file_size
    types = {}
    for item in manifest["items"]:
        kind = "oscairn." + item["kind"]
        types[kind] = types.get(kind, 0) + 1
    for plugin in manifest["plugins"]:
        types[plugin["format"]] = types.get(plugin["format"], 0) + 1
    return manifest, {"sha256": sha256(manifest_bytes), "size": len(manifest_bytes)}, types, platforms, common


def version_key(version):
    return [(0, int(part), "") if part.isdigit() else (1, 0, part) for part in re.split(r"[.+-]", version)]


def read_release(path, root, publishers, tool, cache, problems):
    r = load(path, problems)
    if r is None or not check_format(r, "oscairn.repository-release", path, problems):
        return None
    before = len(problems)
    pid = text(r, "id", path, problems, ID)
    release = text(r, "release", path, problems, VERSION)
    publisher = text(r, "publisher", path, problems, ID)
    text(r, "summary", path, problems)
    text(r, "license", path, problems)
    if (pid and path.parent.name != pid) or (release and path.stem != release):
        problems.add(path, "not at packages/<id>/<release>.json")
    if r.get("channel", "stable") not in ("stable", "beta"):
        problems.add(path, "`channel` is not stable or beta")
    if not isinstance(r.get("tags", []), list) or not all(isinstance(t, str) for t in r.get("tags", [])):
        problems.add(path, "`tags` is not a list of words")
    if r.get("yanked") is not None and not isinstance(r["yanked"], str):
        problems.add(path, "`yanked` is not null or a reason")
    if not isinstance(r.get("requires", {}), dict):
        problems.add(path, "`requires` is not an object")
    claimed = r.get("platforms", [])
    if not isinstance(claimed, list) or not set(claimed) <= PLATFORMS:
        problems.add(path, f"`platforms` names a platform not in {sorted(PLATFORMS)}")
        claimed = []
    if publisher and publisher not in publishers:
        problems.add(path, f"no publishers/{publisher}.json")
    elif pid and publisher and not pid.startswith(publisher + "."):
        problems.add(path, f"the package ID is not under its publisher's ID {publisher}")
    if len(problems) > before:
        return None

    archive = fetch(r, path, cache, problems)
    report = archive and verify(tool, archive, path, problems)
    if not report:
        return None
    signer = key_id(publishers[publisher]["key"])
    if report["archive"] is None or report["archive"][0] != pid:
        problems.add(path, f"the archive's ID is not {pid}")
    elif report["archive"][1] != signer:
        problems.add(path, f"the archive is not signed by its publisher's key {signer} (it is {report['archive'][1]})")
    for fmt, plugin, version, platform, signers in report["builds"]:
        if signer not in signers:
            problems.add(path, f"the {platform} build of {fmt} {plugin} {version} is not signed by its publisher's key {signer}")
    plugins = {(b[0], b[1], b[2]) for b in report["builds"]}
    for plugin in sorted(plugins):
        have = {b[3] for b in report["builds"] if b[:3] == plugin}
        for platform in claimed:
            if platform not in have:
                problems.add(path, f"claims {platform} but {plugin[0]} {plugin[1]} {plugin[2]} has no build for it")
    if claimed and not plugins:
        problems.add(path, "claims platforms but holds no plugin builds")
    if len(problems) > before:
        return None

    contents, manifest, types, platforms, common = facts(archive)
    r["name"] = contents["name"]
    return r, {
        "version": release,
        "channel": r.get("channel", "stable"),
        "manifest": manifest,
        # shortcut: the whole archive is one blob until RFC 0031 step 1 stores it in pieces.
        "archive": {"sha256": r["archive"]["sha256"], "size": r["archive"]["size"]},
        "packageSignature": True,
        "requires": r.get("requires", {}),
        "types": types,
        "platforms": platforms,
        "common": common,
        "estimates": {},
        "yanked": r.get("yanked"),
    }


def unchanged(previous, root, problems):
    """Published releases never change, but for `yanked`; a publisher's key never changes."""
    for old_path in sorted((previous / "packages").glob("*/*.json")):
        new_path = root / "packages" / old_path.relative_to(previous / "packages")
        old = load(old_path, problems)
        new = load(new_path, problems) if new_path.is_file() else None
        if new is None:
            problems.add(new_path, "a published release was removed; yank it instead")
        elif old is not None and {k: v for k, v in old.items() if k != "yanked"} != {k: v for k, v in new.items() if k != "yanked"}:
            problems.add(new_path, "a published release changed; publish a new version instead")
    for old_path in sorted((previous / "publishers").glob("*.json")):
        new_path = root / "publishers" / old_path.name
        old, new = load(old_path, problems), load(new_path, problems) if new_path.is_file() else None
        if old and (new is None or old.get("key") != new.get("key")):
            problems.add(new_path, "a publisher's key changed or was removed; key replacement is not built yet")


def check_delegation(config, tool, problems):
    """The root key's signature over the index key: oscairn-index-1\\0, the key's hex and a newline."""
    root_key, index = config.get("rootKey", ""), config.get("indexKey", {})
    if not (HEX64.match(root_key) and isinstance(index, dict) and HEX64.match(index.get("key", "")) and HEX128.match(index.get("signature", ""))):
        problems.add("repository-config.json", "`rootKey` and `indexKey` are not set (see README.md, Keys)")
        return
    with tempfile.TemporaryDirectory() as temp:
        key_file, sig_file = Path(temp) / "index-key.pub", Path(temp) / "index-key.sig"
        key_file.write_text(index["key"] + "\n")
        sig_file.write_text(json.dumps([{"key": root_key, "signature": index["signature"]}]))
        run = subprocess.run([tool, "verify-file", str(key_file), str(sig_file)], capture_output=True, text=True, timeout=60)
        if run.returncode != 0:
            problems.add("repository-config.json", f"the root key's signature over the index key does not verify: {run.stderr.strip()}")


def write_named(out, folder, data, suffix):
    digest = sha256(data)
    (out / folder).mkdir(parents=True, exist_ok=True)
    (out / folder / (digest + suffix)).write_bytes(data)
    return {"path": f"{folder}/{digest}{suffix}", "sha256": digest, "size": len(data)}


def dumps(value):
    return json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", type=Path, default=Path("."), help="the submissions repository (default: here)")
    ap.add_argument("--oscairn-sign", required=True, help="the oscairn-sign tool")
    ap.add_argument("--cache", type=Path, required=True, help="where archives are kept by SHA-256")
    ap.add_argument("--out", type=Path, required=True, help="where the repository's tree is written")
    ap.add_argument("--serial", type=int, required=True, help="the index's serial; higher than any before")
    ap.add_argument("--previous", type=Path, help="the repository before this change, to check nothing published changed")
    ap.add_argument("--check", action="store_true", help="for a pull request: the root and index keys need not be set")
    args = ap.parse_args()
    root, problems = args.repo, Problems()
    args.cache.mkdir(parents=True, exist_ok=True)

    config = load(root / "repository-config.json", problems) or {}
    text(config, "repository", "repository-config.json", problems, ID)
    blob_base = text(config, "blobBase", "repository-config.json", problems)
    if blob_base and not blob_base.startswith("https://"):
        problems.add("repository-config.json", "`blobBase` is not an https:// address")
    if not args.check or config.get("rootKey"):
        check_delegation(config, args.oscairn_sign, problems)
    if args.previous:
        unchanged(args.previous, root, problems)

    publishers = read_publishers(root, problems)
    packages = {}
    for path in sorted((root / "packages").glob("*/*.json")):
        got = read_release(path, root, publishers, args.oscairn_sign, args.cache, problems)
        if got:
            packages.setdefault(got[0]["id"], []).append(got)

    revoked = load(root / "revoked.json", problems)
    if revoked is not None and check_format(revoked, "oscairn.repository-revocations", "revoked.json", problems):
        for field in ("keys", "builds", "versions"):
            if not isinstance(revoked.get(field), list) or not all(isinstance(e, dict) and isinstance(e.get("reason"), str) for e in revoked[field]):
                problems.add("revoked.json", f"`{field}` is not a list of entries, each with a `reason`")

    if problems:
        print("\n".join(problems), file=sys.stderr)
        return 1

    index_packages, blobs = [], set()
    for pid in sorted(packages):
        releases = sorted(packages[pid], key=lambda g: version_key(g[0]["release"]))
        newest = releases[-1][0]
        publisher = publishers[newest["publisher"]]
        index_packages.append({
            "id": pid,
            "name": newest["name"],
            "summary": newest["summary"],
            "publisher": {"id": publisher["id"], "name": publisher["name"], "key": key_id(publisher["key"])},
            "license": newest["license"],
            "tags": newest.get("tags", []),
            "versions": [v for _, v in releases],
        })
        blobs.update(v["archive"]["sha256"] for _, v in releases)
    index = {"format": "oscairn.repository-index", "version": 1, "repository": config["repository"], "serial": args.serial, "packages": index_packages}

    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    root_file = {
        "format": "oscairn.repository",
        "version": 1,
        "repository": config["repository"],
        "name": config.get("name", config["repository"]),
        "serial": args.serial,
        "rootKey": config.get("rootKey", ""),
        "indexKey": config.get("indexKey", {}),
        "index": write_named(out, "index", gzip.compress(dumps(index), mtime=0), ".json.gz"),
        "revocations": write_named(out, "revocations", dumps(revoked), ".json"),
        "blobBase": blob_base,
        "mirrors": config.get("mirrors", []),
    }
    (out / "repository.json").write_bytes(dumps(root_file) + b"\n")
    (out / "blobs.txt").write_text("".join(b + "\n" for b in sorted(blobs)))
    print(f"serial {args.serial}: {len(index_packages)} packages, {sum(len(p['versions']) for p in index_packages)} versions, {len(blobs)} blobs")
    return 0


if __name__ == "__main__":
    sys.exit(main())

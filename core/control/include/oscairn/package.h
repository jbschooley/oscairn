// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace oscairn
{
    /// A library or an exported Performance as one archive file (RFC 0021, RFC 0006): a zip
    /// holding library.json, the manifest, its items' folders under Performances/ and Sounds/, as
    /// the library keeps them, the samples they use under Assets/, each named by its SHA-256, and
    /// the plugins they use under Plugins/, a build per platform. Every file's SHA-256 is in the
    /// manifest and checked when it is extracted.
    struct Package
    {
        // An item: kind "performance" or "sound", its ID and name, and its folder in the archive.
        struct Item { std::string kind, id, name, path; };
        // A plugin's build for one platform (platform()): its folder or file in the archive and
        // its digest (bundleDigest).
        struct Build { std::string platform, path, sha256; };
        struct Plugin { std::string format, id, version, name; std::vector<Build> builds; };
        std::string id, name;
        std::string kind = "library"; // "library", or "performance" for an exported Performance
        std::vector<Item> items;
        std::vector<Plugin> plugins;
        std::map<std::string, std::string> files; // each file's SHA-256 by its path; filled by writePackage
    };

    // The largest manifest read, in an archive or an installed library. A library of thousands of
    // items lists every file's hash (a converted MONTAGE M library's passes 4 MiB); bounded all
    // the same, as archives are untrusted.
    constexpr uint64_t maxManifestBytes = 64ull << 20;

    // This build's platform: its operating system and CPU, such as "macos-arm64" or "linux-arm64".
    std::string platform();
    // A platform plugin builds are kept for: linux-arm64, linux-x86_64, macos-arm64, macos-x86_64
    // or windows-x86_64. It names a folder in the plugin store, so nothing else is accepted.
    bool knownPlatform(const std::string& name);
    // A plugin build's digest: a file's SHA-256, or for a bundle (a folder), the SHA-256 of each
    // file's path and SHA-256 in path order. Hidden files (a component starting with '.') are not
    // part of a build and are left out, as archives leave them out.
    std::string bundleDigest(const std::filesystem::path& build);

    // The manifest, library.json, as the archive and an installed library hold it.
    std::string manifestJson(const Package& package);

    // Writes `root` (a folder holding Performances/, Sounds/, Assets/ and Plugins/) and the manifest
    // into a new archive, every file's SHA-256 in it. Refuses an archive that exists; written
    // beside it and moved in once complete.
    void writePackage(const std::filesystem::path& archive, const Package& package, const std::filesystem::path& root);

    // Extracts an archive into `into`, a folder that must not exist yet, and returns its manifest.
    // Untrusted input: refuses entries outside library.json, Performances/, Sounds/, Assets/ and
    // Plugins/, paths that climb out or are absolute, links, duplicates, more than 1,048,576 entries,
    // more than 8 GiB in all, an entry over 2 GiB or a manifest over maxManifestBytes (as declared; each file extracted must be the
    // size it declares), a file whose SHA-256 is not the manifest's or that the manifest does not
    // list, a plugin build whose digest is not its own, and a manifest whose items or builds are
    // not in the archive or not for a knownPlatform. On refusal `into` is removed. With `into` empty, nothing is extracted:
    // the same checks but the hashes, and the manifest.
    Package readPackage(const std::filesystem::path& archive, const std::filesystem::path& into);
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/signing.h"
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
    /// manifest and checked when it is extracted. Items, plugin builds and the whole archive can
    /// each be signed (signing.h); every signature there must verify.
    struct Package
    {
        // An item: kind "performance" or "sound", its ID and name, its folder in the archive, and
        // signatures over its content hash (signing::itemMessage).
        struct Item { std::string kind, id, name, path; std::vector<signing::Signature> signatures = {}; };
        // A plugin's build for one platform (platform()): its folder or file in the archive, its
        // digest (bundleDigest) and its publishers' signatures (signing::buildMessage).
        struct Build { std::string platform, path, sha256; std::vector<signing::Signature> signatures = {}; };
        struct Plugin { std::string format, id, version, name; std::vector<Build> builds; };
        std::string id, name;
        // "library"; "performance" or "sound" for an exported item; "plugin" for a plugin package,
        // which holds plugin builds and no items; "asset" for an asset package, which holds an
        // asset library and nothing else. Apps are to be another kind, with their own list.
        std::string kind = "library";
        std::vector<Item> items;
        std::vector<Plugin> plugins;
        // Its license (an SPDX expression) and where its source is: an https:// URL and the exact
        // commit or tag. A plugin package needs a license, and one under the GPL, LGPL or AGPL
        // needs the source and its license texts and notices under Licenses/ (their §6).
        std::string license, sourceUrl, sourceRevision;
        // An asset package (kind "asset", RFC 0009): an asset library whose ID is the package's,
        // its version, and each file under Assets/ by its path there, SHA-256 and size.
        struct AssetFile { std::string path, sha256; uint64_t size = 0; };
        std::string assetVersion;
        std::vector<AssetFile> assets;
        // A plugin package's asset libraries, each by ID and the least version it needs.
        struct Dependency { std::string id, version; };
        std::vector<Dependency> assetDependencies;
        std::map<std::string, std::string> files; // each file's SHA-256 by its path; filled by writePackage
        signing::Signature signature;              // library.sig, the whole archive's over the manifest; an empty key for none
        std::string manifestSha256;                // the manifest's, as read: a package's identity (RFC 0006)
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
    // A plugin's folder in the plugin store and in archives, "<id>@<version>" with characters other
    // than letters, digits, '.', '-', '@' and '_' made '_'.
    std::string pluginFolderName(const std::string& id, const std::string& version);
    // This platform's build folder of each plugin in the store (Plugins/<format>/<id>@<version>/<platform>
    // in the data folder), where a plugin scan finds what archives installed.
    std::vector<std::filesystem::path> pluginStoreFolders();
    // A plugin build's digest: a file's SHA-256, or for a bundle (a folder), the SHA-256 of each
    // file's path and SHA-256 in path order. Hidden files (a component starting with '.') are not
    // part of a build and are left out, as archives leave them out.
    std::string bundleDigest(const std::filesystem::path& build);

    // What a plugin build loads from outside the platform's system libraries and the build itself,
    // one line per problem ("<file in the build>: <library or rpath> (why)"); empty when nothing.
    // Reads each Mach-O (thin or fat) and ELF file in the build directly, whatever this platform
    // is. macOS: every LC_LOAD_DYLIB, weak, reexport, lazy and upward library under /usr/lib/ or
    // /System/, or in the bundle through @loader_path or @rpath (an @rpath library found through
    // a system rpath counts as the system's), and every rpath a system folder or @loader_path;
    // @executable_path is the host's folder, so it is outside. Linux: every DT_NEEDED one of the
    // libraries every Oscairn image carries (docs/library-archive.md), or found in the bundle
    // through an $ORIGIN rpath, and no rpath but $ORIGIN's. A file that does not parse is a
    // problem; other files (resources, Windows DLLs) are not checked. Untrusted input: every read
    // is bounds-checked.
    std::vector<std::string> outsideLibraries(const std::filesystem::path& build);

    // The manifest, library.json, as the archive and an installed library hold it.
    std::string manifestJson(const Package& package);

    // Writes `root` (a folder holding Performances/, Sounds/, Assets/, Plugins/ and Licenses/) and the manifest
    // into a new archive, every file's SHA-256 in it; with `key`, library.sig, signing the whole
    // archive. Refuses an archive that exists; written beside it and moved in once complete.
    void writePackage(const std::filesystem::path& archive, const Package& package, const std::filesystem::path& root, const signing::Key* key = nullptr);
    // Signs an archive whole: rewrites it with library.sig by `key`, replacing any there. The
    // archive is read and checked first, so one whose hashes or signatures do not hold is refused.
    void signPackage(const std::filesystem::path& archive, const signing::Key& key);

    // Extracts an archive into `into`, a folder that must not exist yet, and returns its manifest.
    // The archive is a zip, read in place, or an unpacked package folder (the same tree; hidden
    // files in it are skipped), read by the same rules. Nothing is written unless `into`'s file
    // system has room for every entry and the largest once more.
    // Untrusted input: refuses entries outside library.json, Performances/, Sounds/, Assets/,
    // Plugins/ and Licenses/, an asset package with items or plugins, without a license or a
    // version, with no files, a file listed twice or not plain, not in the archive, not the size
    // or SHA-256 it lists, or a file under Assets/ it does not list; asset dependencies anywhere
    // but in a plugin package, or not plain; a plugin package without a license, or under the GPL, LGPL or AGPL
    // without its source (an https:// URL and a commit or tag) and a file under Licenses/, paths that climb out or are absolute, links, duplicates, more than 1,048,576 entries
    // or 64 MiB of names, more than 8 GiB in all, an entry over 2 GiB or a manifest over maxManifestBytes (as declared; each file extracted must be the
    // size it declares), a file whose SHA-256 is not the manifest's or that the manifest does not
    // list, a plugin build whose digest is not its own, and a manifest whose ID or an item's is
    // not a plain folder name (1 to 64 letters, digits, `.`, `-` and `_`, starting with a letter
    // or digit and not ending in `.`), or whose items or builds are
    // not in the archive, not for a knownPlatform, or whose build is not a .clap or .lv2 bundle,
    // a plugin listed twice (several plugins may list one bundle's path: a bundle holding them all),
    // and a signature that does not verify (each
    // build's over its listed digest, library.sig over the manifest, each item's over its content
    // hash). On refusal `into` is removed. With `into` empty, nothing is extracted: the same
    // checks but the hashes and the items' signatures (listed, unchecked), and the manifest.
    Package readPackage(const std::filesystem::path& archive, const std::filesystem::path& into);
}

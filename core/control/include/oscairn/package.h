// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace oscairn
{
    /// A library or an exported Performance as one archive file (RFC 0021): a zip holding
    /// library.json, the manifest, and its items' folders under Performances/ and Sounds/, as the
    /// library keeps them, with the samples they use under Assets/, each named by its SHA-256.
    struct Package
    {
        // An item: kind "performance" or "sound", its ID and name, and its folder in the archive.
        struct Item { std::string kind, id, name, path; };
        std::string id, name;
        std::string kind = "library"; // "library", or "performance" for an exported Performance
        std::vector<Item> items;
    };

    // The manifest, library.json, as the archive and an installed library hold it.
    std::string manifestJson(const Package& package);

    // Writes `root` (a folder holding Performances/, Sounds/ and Assets/) and the manifest into a
    // new archive. Refuses an archive that exists; written beside it and moved in once complete.
    void writePackage(const std::filesystem::path& archive, const Package& package, const std::filesystem::path& root);

    // Extracts an archive into `into`, a folder that must not exist yet, and returns its manifest.
    // Untrusted input: refuses entries outside library.json, Performances/, Sounds/ and Assets/,
    // paths that climb out or are absolute, links, duplicates, more than 65536 entries, more than
    // 8 GiB in all or an entry over 2 GiB (as declared; each file extracted must be the size it
    // declares), and a manifest whose items are not in the archive. On refusal `into` is removed.
    // With `into` empty, nothing is extracted: the same checks, and the manifest.
    Package readPackage(const std::filesystem::path& archive, const std::filesystem::path& into);
}

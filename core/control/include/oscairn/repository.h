// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// A package repository as a device reads it (RFC 0031): repository.json, the root, signed by the
// index key in repository.sig; the index key, signed by the root key; the index, named in the
// root by its hash; and package files (blobs), named by theirs. Every byte is untrusted until it
// verifies, and every refusal says why.
namespace oscairn::repository
{
    // Where a repository's files come from: a folder (a disk, a USB drive or a share), or HTTPS.
    class Source
    {
    public:
        virtual ~Source() = default;
        // A small file whole, refused past maxBytes.
        virtual std::string read(const std::string& path, uint64_t maxBytes) = 0;
        // A blob of `size` bytes into the new file `to`; its hash is checked by the caller.
        virtual void fetch(const std::string& path, uint64_t size, const std::filesystem::path& to) = 0;
        // The file itself, where it can be read in place (a folder): no copy is made first.
        virtual std::filesystem::path local(const std::string&) { return {}; }
    };
    // A repository in a folder: the same tree a web host serves, blobs under blobs/<ab>/.
    std::unique_ptr<Source> folder(const std::filesystem::path& root);

    struct Version
    {
        std::string version, channel, yanked, needs;    // needs: requires.oscairn, the oldest Oscairn it runs on, empty for any
        std::string manifestSha256, archiveSha256;
        uint64_t archiveSize = 0;
        bool code = false;                              // it brings plugin builds
        std::vector<std::string> platforms;             // those it has builds for
        uint64_t size = 0, installed = 0;               // on this platform: download, and installed
    };
    struct Package { std::string id, name, summary, license, publisherId, publisherName, publisherKey; std::vector<std::string> tags; std::vector<Version> versions; };
    struct Index { std::string repository, name, rootKey, indexKey, blobBase; uint64_t serial = 0; std::vector<std::string> mirrors; std::vector<Package> packages; };

    // Reads and checks the root, the index key and the index. `trusted` holds the root keys this
    // device trusts for repositories and `revoked` the keys it has revoked (public keys, hex).
    // rootKey is set to the root's key as soon as it is read, so a refusal for an untrusted one
    // can offer it to the user.
    Index open(Source& source, const std::vector<std::string>& trusted, const std::vector<std::string>& revoked, std::string& rootKey);

    // Why this device cannot install a version, or empty: it needs a newer system than `system`,
    // or brings code with no build for `platform`.
    std::string incompatible(const Version& v, const std::string& system, const std::string& platform);
    // Dotted versions compared part by part, numbers as numbers: is a older than b?
    bool older(const std::string& a, const std::string& b);
    // A blob's path in a repository: blobs/<first two hex digits>/<sha256>.
    std::string blobPath(const std::string& sha256);
    // A gzip file's contents, refused past maxBytes or when its CRC or length does not hold.
    std::string gunzip(std::string_view gz, uint64_t maxBytes);
}

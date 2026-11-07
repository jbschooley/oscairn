// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
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
    // Where a repository's files come from: a folder (a disk, a USB drive or a share), or the web.
    class Source
    {
    public:
        virtual ~Source() = default;
        // A small file whole, refused past maxBytes.
        virtual std::string read(const std::string& path, uint64_t maxBytes) = 0;
        // A blob of `size` bytes and this SHA-256 into the file `to`, resuming what an interrupted
        // fetch left there; one that does not verify is refused and removed.
        virtual void fetch(const std::string& sha256, uint64_t size, const std::filesystem::path& to) = 0;
        // The file itself, where it can be read in place (a folder): no copy is made first.
        virtual std::filesystem::path local(const std::string&) { return {}; }
        // Where else the root says its files are, once it verifies: mirrors (copies of the whole
        // tree) and the blob base (blobs at <blobBase><ab>/<sha256>).
        virtual void places(const std::vector<std::string>&, const std::string&) {}
    };
    // A repository in a folder: the same tree a web host serves, blobs under blobs/<ab>/.
    std::unique_ptr<Source> folder(const std::filesystem::path& root);

    // An HTTP(S) GET of `url` from byte `from` (a Range request when not 0), following
    // redirects; the body goes to `sink` as it arrives, with the status, and it returns false to
    // stop. Returns the status (200, or 206 for a range), or throws when the server cannot be
    // reached. The core links no TLS: the host provides this (hosts/audio, through JUCE).
    using Transport = std::function<int(const std::string& url, uint64_t from, const std::function<bool(int status, const char* data, size_t size)>& sink)>;
    void setTransport(Transport transport);
    // A repository on the web at `base` (ending in '/'): the root and index from there, the index
    // from the mirrors when it is not; blobs from the mirrors in turn, then the blob base, then
    // the base itself, each checked by its hash, the next tried when one does not verify. A
    // download cut short is resumed with a Range request. `cancel`, when set, stops it.
    std::unique_ptr<Source> web(const std::string& base, const std::atomic<bool>* cancel = nullptr);

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
    // can offer it to the user. With a `cache` folder, the index last read from this repository
    // is kept there: used again while the root's serial is the same, and brought up to date by
    // the delta the root names from its serial, else fetched whole (RFC 0031).
    Index open(Source& source, const std::vector<std::string>& trusted, const std::vector<std::string>& revoked, std::string& rootKey,
               const std::filesystem::path& cache = {});

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

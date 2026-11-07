// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <kick_clap/Host.h>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// Sound assets as dependencies (docs/rfcs/0009-sound-assets-and-dependencies.md): files that
// plugins read, identified by SHA-256, found in registered folders or the user library and
// never copied into a Performance. Control thread only; a mutex guards against misuse.
namespace oscairn::assets
{
    using Ref = kick::clap::AssetReference; // path is relative to the library
    struct Library { std::string id, name, path, fingerprint; size_t files = 0; };

    std::string sha256File(const std::filesystem::path& file);
    std::string sha256(std::string_view bytes);
    // Dotted versions compared part by part, numbers as numbers: is a older than b?
    bool older(const std::string& a, const std::string& b);

    class Store final : public kick::clap::AssetResolver
    {
    public:
        // root holds the registry, file indexes and the user library. Empty: OSCAIRN_DATA_HOME,
        // else the platform's per-user data directory, followed by "assets".
        explicit Store(std::filesystem::path root = {});
        // Registers a folder used in place (re-registering rescans it) and returns its ID: `id`
        // when given (an installed asset library's), else the folder's earlier one or a new one.
        std::string registerFolder(const std::filesystem::path& folder, const std::string& id = {});
        // Asset libraries from asset packages (RFC 0009): each file kept once by its SHA-256 in
        // store/<sha256> under root, and a read-only tree of hard links to them (copies where a
        // link cannot be made) at <root's parent>/AssetLibraries/<id>/<version>/<path>, the data
        // folder's, registered under the library's ID. One version per library: installing
        // another builds the new tree beside the old, moves it into place and removes the old.
        // Each `from` file (already checked by its caller) is moved into the store when the store
        // lacks it. The id and version must be plain names, each path a safe relative path.
        struct LibraryFile { std::string path, sha256; std::filesystem::path from; };
        void installLibrary(const std::string& id, const std::string& version, const std::string& name, const std::vector<LibraryFile>& files);
        // The installed version of a library from an asset package, empty when none is.
        std::string libraryVersion(const std::string& id) const;
        // Its tree when a version at least minVersion (empty: any) is installed, else empty.
        std::string libraryRoot(const std::string& id, const std::string& minVersion) const override;
        // Copies one file into the user library; the original can then be moved or deleted.
        Ref import(const std::filesystem::path& file);
        std::vector<Library> libraries() const;
        // A registered library containing this absolute directory, and the directory's path in it.
        bool locate(const std::filesystem::path& directory, std::string& library, std::string& relative) const;
        // The local file for ref, verified. An empty sha256 accepts the current file and is filled
        // in; a given sha256 must match, and may be satisfied by identical bytes in any library.
        std::filesystem::path resolve(Ref& ref) const;
        std::string resolve(const std::string& library, const std::string& path, std::string& sha256) override;
        const std::filesystem::path& root() const;
    private:
        struct Entry { uintmax_t size; int64_t modified; std::string sha256; };
        using Index = std::map<std::string, Entry>; // relative path → entry
        mutable std::filesystem::path root_;
        mutable std::mutex mutex_;
        mutable std::map<std::string, Library> libraries_;
        mutable std::map<std::string, Index> indexes_;
        mutable bool loaded_ = false;
        void load() const;
        void save() const;
        Index& index(const std::string& id) const;
        void saveIndex(const std::string& id) const;
        std::string hashOf(const std::string& id, const std::filesystem::path& base, const std::string& relative) const;
        Library& user() const;
    };
}

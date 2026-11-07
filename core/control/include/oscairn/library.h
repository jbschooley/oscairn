// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace oscairn
{
    /// The library (RFC 0021): the Performances and Sounds in the user's library folders, found by scanning,
    /// with their info blocks (name, tags, description) read from each file. Files first: the
    /// index under library/ is a cache of what was read, kept by each file's time so a rescan
    /// reads only what changed, and deleting it loses nothing. Favorites and recently used are
    /// the user's own, in library/user.json, never in the items' files. Control thread.
    class Library
    {
    public:
        // dataHome: OSCAIRN_DATA_HOME's folder (empty: that, else the platform's); the library
        // folders are its Performances/ and Sounds/, the cache and user file its library/.
        explicit Library(std::filesystem::path dataHome = {});
        const std::filesystem::path& performances() const { return performances_; }
        // Installed libraries (RFC 0021): each in its own folder here, named by its ID, with its
        // manifest (library.json) and its items' folders. Their items are locked.
        const std::filesystem::path& libraries() const { return libraries_; }
        std::vector<Control::InstalledLibrary> installed() const;
        uint32_t rescan();
        Control::LibraryResult query(const Control::LibraryQuery&) const;
        const Control::LibraryItem* find(const std::string& id) const;
        const std::vector<Control::LibraryItem>& items() const { return items_; }
        void setFavorite(const std::string& id, bool favorite);
        void used(const std::string& id); // most recent first; at most 50 kept
        void setTag(const std::string& tag, bool hidden, const std::string& color);
        // A folder for a new Performance or Sound (kind) named `name`, its name made safe and unique.
        std::filesystem::path newFolder(const std::string& name, const std::string& kind) const;

    private:
        void readUser();
        void writeUser() const;
        std::filesystem::path performances_, sounds_, libraries_, cache_;
        std::vector<Control::InstalledLibrary> installed_;
        std::vector<Control::LibraryItem> items_;
        std::vector<std::string> favorites_, recent_, hiddenTags_;
        std::map<std::string, std::string> tagColors_;
    };

    // The platform's folder for Oscairn's user data: OSCAIRN_DATA_HOME, else Application Support
    // on macOS, else XDG_DATA_HOME or ~/.local/share.
    std::filesystem::path dataHome();
    // Whether the wall clock is known to be right (RFC 0035's time state): OSCAIRN_TIME says
    // "known" or "unknown" where a supervisor keeps the state; otherwise the clock is trusted, as
    // on a desktop, unless it reads before the floor.
    bool timeKnown();
    // "#rrggbb" in hex digits, or empty: a Performance's color.
    bool validColor(const std::string& color);
    // A Sound's or Performance's content hash: SHA-256 over its document without the info block,
    // then each file of its plugins' saved state (nodes/) by path with its own SHA-256, in path
    // order. Names, tags and revisions are not contents; plugins' state is. The second form is
    // what the writer uses, from the document text it is about to write without its info.
    std::string contentHash(const std::filesystem::path& file);
    std::string contentHash(std::string_view documentWithoutInfo, const std::filesystem::path& nodes);
    // The info block of a performance.json or sound.json (format names which); throws on a bad one.
    Control::PerformanceInfo readInfo(const std::filesystem::path& file, const std::string& format);
    // An info block as the documents hold it.
    std::string infoJson(const Control::PerformanceInfo& info);
    // Replaces a document's info block with this one, keeping the rest byte for byte.
    void writeInfo(const std::filesystem::path& file, const Control::PerformanceInfo& info);
    // Writes a whole file through a temporary beside it, moved in once complete: a reader sees the
    // old file or the new one, never half of either.
    void writeAtomically(const std::filesystem::path& file, const std::string& text);
}

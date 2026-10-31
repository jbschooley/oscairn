// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <filesystem>
#include <string>
#include <vector>

namespace oscairn
{
    /// The library (RFC 0021): the Performances in the user's library folder, found by scanning,
    /// with their info blocks (name, tags, description) read from each file. Files first: the
    /// index under library/ is a cache of what was read, kept by each file's time so a rescan
    /// reads only what changed, and deleting it loses nothing. Favourites and recently used are
    /// the user's own, in library/user.json, never in the items' files. Control thread.
    class Library
    {
    public:
        // dataHome: OSCAIRN_DATA_HOME's folder (empty: that, else the platform's); the library
        // folder is its Performances/, the cache and user file its library/.
        explicit Library(std::filesystem::path dataHome = {});
        const std::filesystem::path& performances() const { return performances_; }
        uint32_t rescan();
        Control::LibraryResult query(const Control::LibraryQuery&) const;
        const Control::LibraryItem* find(const std::string& id) const;
        void setFavourite(const std::string& id, bool favourite);
        void used(const std::string& id); // most recent first; at most 50 kept
        // A folder for a new Performance named `name` (its name made safe, made unique).
        std::filesystem::path newFolder(const std::string& name) const;

    private:
        void readUser();
        void writeUser() const;
        std::filesystem::path performances_, cache_;
        std::vector<Control::LibraryItem> items_;
        std::vector<std::string> favourites_, recent_;
    };

    // The platform's folder for Oscairn's user data: OSCAIRN_DATA_HOME, else Application Support
    // on macOS, else XDG_DATA_HOME or ~/.local/share.
    std::filesystem::path dataHome();
    // A Performance's info block as the library and the format read it; throws on a bad one.
    Control::PerformanceInfo readPerformanceInfo(const std::filesystem::path& folder);
}

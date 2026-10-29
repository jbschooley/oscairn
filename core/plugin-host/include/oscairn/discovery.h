// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <kick_clap/Discovery.h>
namespace oscairn {
// Identity is (format, descriptor.id). The path is only the current installation location.
std::vector<std::filesystem::path> pluginSearchPaths();
kick::clap::ScanResult scanPlugins(const std::vector<std::filesystem::path>& roots,const std::filesystem::path& helper,const std::atomic<bool>* cancelled=nullptr);
kick::clap::ScanResult inspectPlugin(const std::filesystem::path& path); // scanner process only
}

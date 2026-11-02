// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Host.h"
#include <atomic>
#include <filesystem>
#include <map>

namespace kick::clap {
struct ScanFailure { std::string path, message; };
struct ScanResult { std::vector<Descriptor> plugins; std::vector<ScanFailure> failures; };
std::vector<std::filesystem::path> defaultSearchPaths();
std::vector<std::filesystem::path> findBinaries(const std::vector<std::filesystem::path>& roots, const std::string& extension = ".clap");
// Loads native code. Only call in the scanner worker or with a trusted fixture. With presets,
// also indexes each plugin's presets (indexPresets).
ScanResult inspect(const std::filesystem::path& binary, bool presets = false);
// Each binary is inspected by a fresh helper process with timeout/crash isolation; one whose
// library offers preset discovery has its presets indexed by a second helper process with its
// own timeout (presetTimeoutMilliseconds), so a failed index leaves its plugins listed.
ScanResult scan(const std::vector<std::filesystem::path>& roots,
                const std::filesystem::path& scannerExecutable,
                uint32_t timeoutMilliseconds = 5000,
                const std::atomic<bool>* canceled = nullptr, const std::string& extension = ".clap",
                uint32_t presetTimeoutMilliseconds = 15000);
std::string encodeScan(const ScanResult&);
bool decodeScan(const std::string&, ScanResult&);

// CLAP preset discovery (RFC 0005, Presets). The presets a plugin library offers for each of
// pluginIds: inside the plugin, and files of the declared types in the declared places, each
// under the plugins it names (one naming none is listed for all). Runs plugin code: call it off
// the audio thread. Stops at maxPresets, and starts no new read after budgetMilliseconds;
// either, and a provider that does not initialize, is noted in problems.
struct PresetIndex { std::map<std::string, std::vector<Preset>> presets; std::vector<std::string> problems; };
constexpr size_t maxPresets = 4096, maxPresetNameBytes = 255, maxPresetLocationBytes = 4096;
PresetIndex indexPresets(const clap_plugin_entry_t* entry, const std::vector<std::string>& pluginIds, uint32_t budgetMilliseconds = 10000);
} // namespace kick::clap

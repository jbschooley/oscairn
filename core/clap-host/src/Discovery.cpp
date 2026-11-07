// SPDX-License-Identifier: AGPL-3.0-or-later
#include <kick_clap/Discovery.h>
#include "Internal.h"
#include <clap/factory/preset-discovery.h>
#include <oscairn-clap/assets.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <set>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace kick::clap {
namespace fs = std::filesystem;
namespace {
constexpr size_t maxWireBytes = 4 * 1024 * 1024;
constexpr const char* magic = "KICKCLAP1\n";
void number(std::string& out, uint32_t n) { for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((n >> (8 * i)) & 255)); }
void field(std::string& out, const std::string& s) { number(out, static_cast<uint32_t>(s.size())); out += s; }
struct Reader {
    const std::string& source;
    size_t position;
    uint32_t number() {
        if (source.size() - position < 4) throw std::runtime_error("Truncated scanner output");
        uint32_t n = 0;
        for (int i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<uint8_t>(source[position++])) << (8 * i);
        return n;
    }
    std::string field() {
        const auto n = number();
        if (n > maxWireBytes || n > source.size() - position) throw std::runtime_error("Invalid scanner field");
        auto s = source.substr(position, n); position += n; return s;
    }
};
}
std::vector<fs::path> defaultSearchPaths() {
    std::vector<fs::path> paths;
    if (const auto* custom = std::getenv("CLAP_PATH")) {
        std::string list(custom);
#ifdef _WIN32
        const char separator = ';';
#else
        const char separator = ':';
#endif
        for (size_t begin = 0, end; begin < list.size(); begin = end + 1) {
            end = list.find(separator, begin); if (end == std::string::npos) end = list.size();
            if (end > begin) paths.emplace_back(list.substr(begin, end - begin));
        }
    }
    if (const auto* home = std::getenv("HOME")) {
        paths.emplace_back(fs::path(home) / ".clap");
#ifdef __APPLE__
        paths.emplace_back(fs::path(home) / "Library/Audio/Plug-Ins/CLAP");
#endif
    }
#ifdef __APPLE__
    paths.emplace_back("/Library/Audio/Plug-Ins/CLAP");
#elif defined(_WIN32)
    if (const auto* common = std::getenv("COMMONPROGRAMFILES")) paths.emplace_back(fs::path(common) / "CLAP");
#else
    paths.emplace_back("/usr/lib/clap"); paths.emplace_back("/usr/local/lib/clap");
    paths.emplace_back("/opt/kick/plugins/clap");
#endif
    return paths;
}
std::vector<fs::path> findBinaries(const std::vector<fs::path>& roots, const std::string& extension) {
    std::set<fs::path> found;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::exists(root, ec)) continue;
        if (root.extension() == extension) { found.insert(fs::canonical(root, ec)); continue; }
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it.depth() > 8) { it.disable_recursion_pending(); continue; }
            if (it->path().extension() == extension) {
                auto path = fs::canonical(it->path(), ec);
                if (!ec) found.insert(path);
                if (it->is_directory(ec)) it.disable_recursion_pending();
            }
            if (found.size() >= 8192) break;
        }
    }
    found.erase(fs::path{});
    return {found.begin(), found.end()};
}
ScanResult inspect(const fs::path& binary, bool presets, const AssetResolver* assets) {
    ScanResult result;
    try {
        auto library = detail::openLibrary(binary.string());
        const bool discovery = library->entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID) || library->entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT);
        const auto* factory = library->factory();
        if (!factory) throw std::runtime_error("No plugin factory");
        const auto count = factory->get_plugin_count(factory);
        if (!count || count > 4096) throw std::runtime_error("Invalid factory descriptor count");
        std::set<std::string> ids;
        for (uint32_t i = 0; i < count; ++i) {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (!d || !d->id || !d->id[0] || !d->name || !d->name[0] || !clap_version_is_compatible(d->clap_version))
                throw std::runtime_error("Invalid or incompatible plugin descriptor");
            if (!ids.insert(d->id).second) throw std::runtime_error("Duplicate plugin ID in factory");
            result.plugins.push_back(detail::describe(library->path, *d));
            result.plugins.back().presetDiscovery = discovery;
        }
        if (presets && discovery) {
            std::vector<std::string> plugins;
            for (const auto& p : result.plugins) plugins.push_back(p.id);
            auto index = indexPresets(library->entry, plugins, 10000, assets);
            for (auto& p : result.plugins) p.presets = std::move(index.presets[p.id]);
            for (auto& problem : index.problems) result.failures.push_back({binary.string(), std::move(problem)});
        }
    } catch (const std::exception& e) { result.plugins.clear(); result.failures.push_back({binary.string(), e.what()}); }
      catch (...) { result.plugins.clear(); result.failures.push_back({binary.string(), "Plugin threw during discovery"}); }
    return result;
}
std::string encodeScan(const ScanResult& result) {
    std::string out = magic;
    number(out, static_cast<uint32_t>(result.plugins.size()));
    for (const auto& p : result.plugins) {
        field(out, p.path); field(out, p.id); field(out, p.name); field(out, p.vendor); field(out, p.version); field(out, p.description);
        number(out, static_cast<uint32_t>(p.features.size()));
        for (const auto& f : p.features) field(out, f);
        number(out, p.presetDiscovery); number(out, static_cast<uint32_t>(p.presets.size()));
        for (const auto& preset : p.presets) { field(out, preset.name); number(out, preset.kind); field(out, preset.location); field(out, preset.loadKey); }
    }
    number(out, static_cast<uint32_t>(result.failures.size()));
    for (const auto& f : result.failures) { field(out, f.path); field(out, f.message); }
    if (out.size() > maxWireBytes) throw std::runtime_error("Scanner result exceeds size limit");
    return out;
}
bool decodeScan(const std::string& source, ScanResult& result) {
    if (source.size() > maxWireBytes) return false;
    const auto start = source.rfind(magic);
    if (start == std::string::npos) return false;
    ScanResult decoded;
    try {
        Reader reader{source, start + std::strlen(magic)};
        auto count = reader.number(); if (count > 4096) return false;
        for (uint32_t i = 0; i < count; ++i) {
            Descriptor p;
            p.path = reader.field(); p.id = reader.field(); p.name = reader.field(); p.vendor = reader.field();
            p.version = reader.field(); p.description = reader.field();
            auto features = reader.number(); if (features > 128) return false;
            for (uint32_t j = 0; j < features; ++j) p.features.push_back(reader.field());
            const auto discovery = reader.number(), presets = reader.number();
            if (discovery > 1 || presets > maxPresets) return false;
            p.presetDiscovery = discovery;
            for (uint32_t j = 0; j < presets; ++j) {
                Preset preset; preset.name = reader.field(); preset.kind = reader.number(); preset.location = reader.field(); preset.loadKey = reader.field();
                if (preset.kind > CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || preset.name.empty() || preset.name.size() > maxPresetNameBytes ||
                    preset.location.size() > maxPresetLocationBytes || preset.loadKey.size() > maxPresetLocationBytes) return false;
                p.presets.push_back(std::move(preset));
            }
            if (p.path.empty() || p.id.empty() || p.name.empty()) return false;
            decoded.plugins.push_back(std::move(p));
        }
        count = reader.number(); if (count > 8192) return false;
        for (uint32_t i = 0; i < count; ++i) decoded.failures.push_back({reader.field(), reader.field()});
        if (reader.position != source.size()) return false;
        result = std::move(decoded); return true;
    } catch (...) { return false; }
}
#ifndef _WIN32
// Runs the scanner helper on one binary (flag: --worker, or --presets to index presets too) with
// a timeout, crash isolation and the last of its stderr; false with the reason in failure, which
// starts "Cannot " only when the helper never started (scanPluginPresets asks again after those).
static bool runHelper(const fs::path& helper, const char* flag, const std::string& binary, uint32_t timeoutMs, const std::atomic<bool>* canceled,
               ScanResult& result, std::string& failure) {
    int pipes[2], errors[2];
    if (::pipe(pipes) != 0) { failure = "Cannot create scanner pipe"; return false; }
    if (::pipe(errors) != 0) { ::close(pipes[0]); ::close(pipes[1]); failure = "Cannot create scanner pipe"; return false; }
    for (int fd : {pipes[0], pipes[1], errors[0], errors[1]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipes[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipes[0]);
    posix_spawn_file_actions_addclose(&actions, pipes[1]);
    posix_spawn_file_actions_adddup2(&actions, errors[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, errors[0]);
    posix_spawn_file_actions_addclose(&actions, errors[1]);
    posix_spawnattr_t attributes; posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    auto program = helper.string(); std::string mode = flag, path = binary;
    char* arguments[]{program.data(), mode.data(), path.data(), nullptr};
    pid_t pid = 0;
    const auto spawned = posix_spawn(&pid, program.c_str(), &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes); posix_spawn_file_actions_destroy(&actions);
    ::close(pipes[1]); ::close(errors[1]);
    if (spawned != 0) { ::close(pipes[0]); ::close(errors[0]); failure = "Cannot launch kick-clap-scan helper"; return false; }
    ::fcntl(pipes[0], F_SETFL, ::fcntl(pipes[0], F_GETFL) | O_NONBLOCK);
    ::fcntl(errors[0], F_SETFL, ::fcntl(errors[0], F_GETFL) | O_NONBLOCK);
    std::string stderrTail; // last 2 KiB the plugin wrote to stderr, for the failure message
    auto drainErrors = [&] {
        char bytes[4096]; ssize_t n;
        while ((n = ::read(errors[0], bytes, sizeof(bytes))) > 0) {
            stderrTail.append(bytes, static_cast<size_t>(n));
            if (stderrTail.size() > 2048) stderrTail.erase(0, stderrTail.size() - 2048);
        }
    };
    std::string wire; int status = 0; bool done = false, timedOut = false, oversized = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done) {
        char bytes[4096]; ssize_t n;
        while ((n = ::read(pipes[0], bytes, sizeof(bytes))) > 0) {
            if (wire.size() + static_cast<size_t>(n) > maxWireBytes) { oversized = true; break; }
            wire.append(bytes, static_cast<size_t>(n));
        }
        drainErrors();
        const auto waited = ::waitpid(pid, &status, WNOHANG);
        if (waited == pid) { done = true; break; }
        if (waited < 0 && errno != EINTR) { done = true; status = -1; break; }
        if (oversized || std::chrono::steady_clock::now() >= deadline || (canceled && canceled->load())) {
            timedOut = !oversized;
            ::kill(-pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            done = true; break;
        }
        pollfd fd{pipes[0], POLLIN, 0}; ::poll(&fd, 1, 10);
    }
    char tail[4096]; ssize_t n;
    while (!oversized && (n = ::read(pipes[0], tail, sizeof(tail))) > 0) {
        if (wire.size() + static_cast<size_t>(n) > maxWireBytes) { oversized = true; break; }
        wire.append(tail, static_cast<size_t>(n));
    }
    ::close(pipes[0]);
    drainErrors(); ::close(errors[0]);
    while (!stderrTail.empty() && std::isspace(static_cast<unsigned char>(stderrTail.back()))) stderrTail.pop_back();
    for (auto& c : stderrTail) if (c == '\n' || c == '\r') c = ' ';
    const auto detail = stderrTail.empty() ? std::string() : ": " + stderrTail;
    if (timedOut) failure = (canceled && canceled->load() ? "Scan canceled" : "Plugin scan timed out") + detail;
    else if (oversized) failure = "Plugin scanner output exceeded size limit" + detail;
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) failure = "Plugin crashed or failed during scan" + detail;
    else if (!decodeScan(wire, result)) failure = "Invalid scanner response" + detail;
    else return true;
    return false;
}
bool indexPresetsWithHelper(const fs::path& helper, const fs::path& binary, uint32_t timeoutMs, ScanResult& result, std::string& failure) {
    return runHelper(helper, "--presets", binary.string(), timeoutMs ? timeoutMs : 1, nullptr, result, failure);
}
#else
bool indexPresetsWithHelper(const fs::path&, const fs::path&, uint32_t, ScanResult&, std::string& failure) {
    failure = "Isolated preset indexing currently supports Linux and macOS";
    return false;
}
#endif
ScanResult scan(const std::vector<fs::path>& roots, const fs::path& helper, uint32_t timeoutMs, const std::atomic<bool>* canceled, const std::string& extension, uint32_t presetTimeoutMs) {
    ScanResult result;
    if (timeoutMs == 0 || timeoutMs > 60000 || presetTimeoutMs == 0 || presetTimeoutMs > 60000) { result.failures.push_back({helper.string(), "Invalid scanner timeout"}); return result; }
#ifdef _WIN32
    (void)roots; (void)canceled; (void)presetTimeoutMs;
    result.failures.push_back({helper.string(), "Isolated scanning currently supports Linux and macOS"});
#else
    for (const auto& path : findBinaries(roots,extension)) {
        if (canceled && canceled->load()) break;
        const auto binary = path.string();
        ScanResult single; std::string failure;
        if (!runHelper(helper, "--worker", binary, timeoutMs, canceled, single, failure)) { result.failures.push_back({binary, failure}); continue; }
        // Presets in a second process, so a slow or failing index leaves the plugins listed.
        if (std::any_of(single.plugins.begin(), single.plugins.end(), [](const Descriptor& d) { return d.presetDiscovery; })) {
            ScanResult indexed;
            if (!runHelper(helper, "--presets", binary, presetTimeoutMs, canceled, indexed, failure)) single.failures.push_back({binary, "Presets not indexed: " + failure});
            else {
                for (auto& p : single.plugins)
                    for (auto& q : indexed.plugins) if (q.id == p.id) p.presets = std::move(q.presets);
                for (auto& f : indexed.failures) if (f.message.rfind("Presets: ", 0) == 0) single.failures.push_back(std::move(f));
            }
        }
        result.plugins.insert(result.plugins.end(), single.plugins.begin(), single.plugins.end());
        result.failures.insert(result.failures.end(), single.failures.begin(), single.failures.end());
    }
#endif
    return result;
}

namespace {
// What a preset provider declares while it initializes, and the presets it then reports.
struct Indexing {
    const std::vector<std::string>* plugins = nullptr;
    PresetIndex* index = nullptr;
    const AssetResolver* assets = nullptr; // oscairn.assets' library_root, for providers' locations
    std::vector<std::string> extensions;
    struct Location { uint32_t kind; std::string path; };
    std::vector<Location> locations;
    uint32_t kind = 0; std::string location;
    size_t count = 0; bool stopped = false;
    bool open = false; std::vector<std::string> named; Preset current;
    void finish() {
        if (!open) return;
        open = false; bool kept = false;
        for (const auto& id : *plugins)
            if (named.empty() || std::find(named.begin(), named.end(), id) != named.end()) { index->presets[id].push_back(current); kept = true; }
        count += kept;
    }
};
Indexing& of(const clap_preset_discovery_indexer_t* i) { return *static_cast<Indexing*>(i->indexer_data); }
Indexing& of(const clap_preset_discovery_metadata_receiver_t* r) { return *static_cast<Indexing*>(r->receiver_data); }
}

PresetIndex indexPresets(const clap_plugin_entry_t* entry, const std::vector<std::string>& plugins, uint32_t budgetMs, const AssetResolver* assets) {
    PresetIndex index;
    if (!entry || !entry->get_factory) return index;
    auto* factory = static_cast<const clap_preset_discovery_factory_t*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID));
    if (!factory) factory = static_cast<const clap_preset_discovery_factory_t*>(entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT));
    if (!factory || !factory->count || !factory->get_descriptor || !factory->create) return index;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
    Indexing data; data.plugins = &plugins; data.index = &index; data.assets = assets;
    // oscairn.assets from the indexer: library_root only (resolve needs a plugin instance's host).
    static const oscairn_host_assets_t libraries{nullptr, [](const void* owner, const char* library, const char* minVersion, char* path, uint32_t capacity) {
        const auto* i = static_cast<const clap_preset_discovery_indexer_t*>(owner);
        return i && library && path && capacity && of(i).assets && detail::writeLibraryRoot(*of(i).assets, library, minVersion, path, capacity); }};
    clap_preset_discovery_indexer_t indexer{CLAP_VERSION, "Oscairn", "Oscairn", "https://github.com/oscairn/oscairn", "1", &data,
        [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_filetype_t* type) {
            if (!type || of(i).extensions.size() >= 64) return false;
            of(i).extensions.emplace_back(type->file_extension ? type->file_extension : ""); return true; },
        [](const clap_preset_discovery_indexer_t* i, const clap_preset_discovery_location_t* l) {
            if (!l || l->kind > CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN || (l->kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) != (l->location != nullptr) ||
                (l->location && std::strlen(l->location) > maxPresetLocationBytes) || of(i).locations.size() >= 64) return false;
            of(i).locations.push_back({l->kind, l->location ? l->location : ""}); return true; },
        [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) { return true; },
        [](const clap_preset_discovery_indexer_t* i, const char* id) -> const void* {
            return id && of(i).assets && !std::strcmp(id, OSCAIRN_EXT_ASSETS) ? &libraries : nullptr; }}; // not draft 1, whose resolve this has not
    clap_preset_discovery_metadata_receiver_t receiver{&data,
        [](const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {},
        [](const clap_preset_discovery_metadata_receiver_t* r, const char* name, const char* key) {
            auto& d = of(r); d.finish();
            if (d.stopped) return false;
            if (d.count >= maxPresets) { d.stopped = true; d.index->problems.push_back("Presets: stopped at " + std::to_string(maxPresets)); return false; }
            // A container names its presets and gives each a load key; a file that is one preset may leave both out.
            std::string title = name ? name : "";
            if (title.empty() && d.kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE) title = fs::path(d.location).stem().string();
            const std::string loadKey = key ? key : "";
            d.named.clear(); d.current = {title, d.kind, d.location, loadKey};
            d.open = !title.empty() && title.size() <= maxPresetNameBytes && loadKey.size() <= maxPresetLocationBytes &&
                     (d.kind == CLAP_PRESET_DISCOVERY_LOCATION_FILE || !loadKey.empty());
            return true; },
        [](const clap_preset_discovery_metadata_receiver_t* r, const clap_universal_plugin_id_t* id) {
            if (!id || of(r).named.size() >= 256) return;
            // Another format's plugin is not ours, yet the preset is named: it is not listed for all.
            of(r).named.emplace_back(id->abi && id->id && !std::strcmp(id->abi, "clap") ? id->id : "");
        },
        [](const clap_preset_discovery_metadata_receiver_t*, const char*) {},
        [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {},
        [](const clap_preset_discovery_metadata_receiver_t*, const char*) {},
        [](const clap_preset_discovery_metadata_receiver_t*, const char*) {},
        [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {},
        [](const clap_preset_discovery_metadata_receiver_t*, const char*) {},
        [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {}};
    auto matches = [&](const fs::path& file) {
        if (data.extensions.empty()) return true;
        auto ext = file.extension().string(); if (!ext.empty()) ext.erase(0, 1);
        return std::any_of(data.extensions.begin(), data.extensions.end(), [&](const std::string& e) { return e.empty() || e == ext; });
    };
    auto read = [&](const clap_preset_discovery_provider_t* provider, uint32_t kind, const std::string& location) {
        if (data.stopped) return;
        if (std::chrono::steady_clock::now() >= deadline) {
            data.stopped = true; index.problems.push_back("Presets: stopped after " + std::to_string(budgetMs / 1000) + " s"); return;
        }
        if (location.size() > maxPresetLocationBytes) return;
        data.kind = kind; data.location = location;
        provider->get_metadata(provider, kind, kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? nullptr : location.c_str(), &receiver);
        data.finish();
    };
    for (uint32_t p = 0, providers = std::min<uint32_t>(factory->count(factory), 64); p < providers && !data.stopped; ++p) {
        const auto* d = factory->get_descriptor(factory, p);
        if (!d || !d->id) continue;
        const auto* provider = factory->create(factory, &indexer, d->id);
        if (!provider) { index.problems.push_back(std::string("Presets: provider ") + d->id + " was not created"); continue; }
        try {
            data.locations.clear(); data.extensions.clear();
            if (!provider->init || !provider->init(provider) || !provider->get_metadata)
                index.problems.push_back(std::string("Presets: provider ") + d->id + " did not initialize");
            else
                for (const auto& location : data.locations) {
                    if (location.kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) { read(provider, location.kind, {}); continue; }
                    std::error_code error;
                    if (fs::is_regular_file(location.path, error)) { read(provider, location.kind, location.path); continue; }
                    // A folder: its preset files, at most 10,000 of them, 16 folders deep.
                    size_t files = 0;
                    for (auto it = fs::recursive_directory_iterator(location.path, fs::directory_options::skip_permission_denied, error);
                         !error && it != fs::recursive_directory_iterator() && files < 10000 && !data.stopped; it.increment(error)) {
                        if (it.depth() >= 16) it.disable_recursion_pending();
                        if (it->is_regular_file(error) && matches(it->path())) { ++files; read(provider, location.kind, it->path().string()); }
                    }
                }
        } catch (...) { index.problems.push_back(std::string("Presets: provider ") + d->id + " threw"); }
        data.open = false;
        if (provider->destroy) provider->destroy(provider);
    }
    return index;
}
} // namespace kick::clap

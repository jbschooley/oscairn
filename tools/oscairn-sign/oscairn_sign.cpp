// SPDX-License-Identifier: AGPL-3.0-or-later
//
// oscairn-sign: package signatures (RFC 0006) for plugin authors and publishers. Makes a key,
// signs a plugin build (a .sig beside it, which an export carries), packs builds into a plugin
// package, signs an archive whole, and
// checks an archive's signatures, and signs and checks a repository's files (RFC 0031). A private
// key is never printed.
#include "oscairn/package.h"
#include "oscairn/signing.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <random>
#include <string>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int usage()
    {
        std::fputs("usage:\n"
                   "  oscairn-sign key <key-file>\n"
                   "      make a key, readable only by you; prints its ID and public key\n"
                   "  oscairn-sign build <key-file> <format> <id> <version> <platform> <build>\n"
                   "      sign a plugin build, adding the signature to <build>.sig beside it and replacing\n"
                   "      this key's others there; <id> is a comma-separated list for a bundle holding\n"
                   "      several plugins, each signed\n"
                   "  oscairn-sign archive <key-file> <archive.olib>\n"
                   "      sign an archive whole (library.sig), replacing any signature over it\n"
                   "  oscairn-sign plugin-package [--key <key-file>] [--plugin <id> <name>]... <archive.olib> <format> <id> <version> <name> <platform>=<build>...\n"
                   "      pack plugin builds, each with the signatures in <build>.sig beside it, into a\n"
                   "      plugin package (an archive with no items) with that ID and name; with --key,\n"
                   "      signed whole. A bundle holding several plugins names each with --plugin (else\n"
                   "      its one plugin is <id>, named <name>); <build>.sig signs each of them\n"
                   "  oscairn-sign verify [--into <folder>] <archive.olib>\n"
                   "      check every signature and hash in an archive and say who signed what;\n"
                   "      <folder>, which must not exist yet, is where it is unpacked to check (default: a\n"
                   "      folder in the OS temp folder; pick one on the file system with room for it)\n"
                   "  oscairn-sign file <key-file> <file> <signature-file>\n"
                   "      sign a repository's file (its root, or its index key), writing <signature-file>\n"
                   "  oscairn-sign verify-file <file> <signature-file>\n"
                   "      check a repository file's signatures and say who signed it\n", stderr);
        return 2;
    }
    std::string signers(const std::vector<signing::Signature>& list)
    {
        if(list.empty()) return "unsigned";
        std::string out;
        for(const auto& s : list) out += (out.empty() ? "" : ",") + signing::keyId(s.key);
        return out;
    }
    // A repository's file, whole: a root or an index key is a few kilobytes.
    std::string readFile(const fs::path& file)
    {
        if(!fs::is_regular_file(file) || fs::file_size(file) > 64 * 1024 * 1024) throw std::runtime_error("no file, or one over 64 MiB: " + file.string());
        std::ifstream in(file, std::ios::binary);
        return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    }
}

int main(int argc, char** argv)
{
    const std::vector<std::string> a(argv + 1, argv + argc);
    try
    {
        if(a.size() == 2 && a[0] == "key")
        {
            const auto k = signing::makeKey(a[1]);
            std::printf("key %s %s\n", signing::keyId(k.publicKey).c_str(), k.publicKey.c_str());
            return 0;
        }
        if(a.size() == 7 && a[0] == "build")
        {
            const auto k = signing::loadKey(a[1]);
            if(!knownPlatform(a[5])) throw std::runtime_error("unknown platform " + a[5] + "; one of linux-arm64, linux-x86_64, macos-arm64, macos-x86_64, windows-x86_64");
            const fs::path build = a[6];
            if(!fs::exists(build)) throw std::runtime_error("no build " + a[6]);
            const auto digest = bundleDigest(build);
            const fs::path sig = build.string() + ".sig";
            auto list = signing::readSignatures(sig);
            std::erase_if(list, [&](const auto& s) { return s.key == k.publicKey; }); // this key's again, over this build
            // Several IDs, comma-separated: each plugin a bundle holds, signed together so none drops another's.
            std::vector<std::string> ids;
            for(size_t at = 0; at <= a[3].size();) { const auto end = std::min(a[3].find(',', at), a[3].size()); ids.push_back(a[3].substr(at, end - at)); at = end + 1; }
            for(const auto& id : ids) if(id.empty()) return usage();
            for(const auto& id : ids) list.push_back({k.publicKey, signing::sign(k, signing::buildMessage(a[2], id, a[4], a[5], digest))});
            signing::writeSignatures(sig, list);
            for(const auto& id : ids) std::printf("signed %s %s %s %s %s by %s\n", a[2].c_str(), id.c_str(), a[4].c_str(), a[5].c_str(), digest.c_str(), signing::keyId(k.publicKey).c_str());
            return 0;
        }
        if(a.size() == 3 && a[0] == "archive")
        {
            const auto k = signing::loadKey(a[1]);
            signPackage(a[2], k);
            std::printf("signed %s by %s\n", a[2].c_str(), signing::keyId(k.publicKey).c_str());
            return 0;
        }
        if(a.size() >= 7 && a[0] == "plugin-package")
        {
            std::vector<std::string> rest(a.begin() + 1, a.end());
            std::unique_ptr<signing::Key> key;
            std::vector<std::pair<std::string, std::string>> listed; // --plugin: each plugin the bundle holds, by ID and name
            while(!rest.empty() && rest[0].rfind("--", 0) == 0)
            {
                if(rest[0] == "--key" && rest.size() > 1) { key = std::make_unique<signing::Key>(signing::loadKey(rest[1])); rest.erase(rest.begin(), rest.begin() + 2); }
                else if(rest[0] == "--plugin" && rest.size() > 2) { listed.push_back({rest[1], rest[2]}); rest.erase(rest.begin(), rest.begin() + 3); }
                else return usage();
            }
            if(rest.size() < 6) return usage();
            Package p;
            p.kind = "plugin"; p.id = rest[2]; p.name = rest[4];
            if(listed.empty()) listed.push_back({rest[2], rest[4]});
            std::string ids;
            for(const auto& [id, name] : listed)
            {
                if(std::any_of(p.plugins.begin(), p.plugins.end(), [&](const auto& q) { return q.id == id; })) throw std::runtime_error("plugin " + id + " is listed twice");
                p.plugins.push_back({rest[1], id, rest[3], name, {}});
                ids += (ids.empty() ? "" : ", ") + id;
            }
            const auto format = rest[1], version = rest[3];
            std::random_device random;
            const auto staged = fs::temp_directory_path() / ("oscairn-pack-" + std::to_string(random()) + std::to_string(random()));
            struct Gone { fs::path p; ~Gone() { std::error_code e; fs::remove_all(p, e); } } gone{staged};
            for(size_t i = 5; i < rest.size(); ++i)
            {
                const auto equals = rest[i].find('=');
                if(equals == std::string::npos) return usage();
                const auto forPlatform = rest[i].substr(0, equals);
                const fs::path build = rest[i].substr(equals + 1);
                if(!knownPlatform(forPlatform)) throw std::runtime_error("unknown platform " + forPlatform);
                if(!fs::exists(build)) throw std::runtime_error("no build " + build.string());
                const auto digest = bundleDigest(build);
                // The bundle is packed once for every plugin it holds; each signature goes to the plugin it is over.
                std::vector<std::vector<signing::Signature>> signatures(p.plugins.size());
                for(const auto& s : signing::readSignatures(build.string() + ".sig"))
                {
                    bool over = false;
                    for(size_t k = 0; k < p.plugins.size(); ++k)
                        if(signing::verify(s, signing::buildMessage(format, p.plugins[k].id, version, forPlatform, digest))) { signatures[k].push_back(s); over = true; }
                    if(!over) throw std::runtime_error("a signature in " + build.string() + ".sig is not over this build of " + ids + " (a preset-discovery provider is not a plugin, and is not signed)");
                }
                const auto inside = fs::path("Plugins") / format / pluginFolderName(p.plugins[0].id, version) / forPlatform / build.filename();
                fs::create_directories((staged / inside).parent_path());
                fs::copy(build, staged / inside, fs::copy_options::recursive);
                for(size_t k = 0; k < p.plugins.size(); ++k) p.plugins[k].builds.push_back({forPlatform, inside.generic_string(), digest, signatures[k]});
            }
            writePackage(rest[0], p, staged, key.get());
            try { readPackage(rest[0], {}); } catch(...) { fs::remove(rest[0]); throw; } // its ID, format and bundles as a reader takes them
            std::printf("packed %s %s %s with %zu builds%s\n", format.c_str(), ids.c_str(), version.c_str(), p.plugins[0].builds.size(),
                        key ? (", signed by " + signing::keyId(key->publicKey)).c_str() : "");
            return 0;
        }
        if(a.size() >= 2 && a[0] == "verify" && (a.size() == 2 || (a.size() == 4 && a[1] == "--into")))
        {
            std::random_device random;
            const auto into = a.size() == 4 ? fs::path(a[2]) : fs::temp_directory_path() / ("oscairn-verify-" + std::to_string(random()) + std::to_string(random()));
            const auto p = readPackage(a.back(), into); // throws on any signature or hash that does not hold
            std::error_code ignored; fs::remove_all(into, ignored);
            std::printf("archive %s %s\n", p.id.c_str(), p.signature.key.empty() ? "unsigned" : signing::keyId(p.signature.key).c_str());
            for(const auto& i : p.items) std::printf("item %s %s %s\n", i.kind.c_str(), i.id.c_str(), signers(i.signatures).c_str());
            for(const auto& plugin : p.plugins)
                for(const auto& b : plugin.builds) std::printf("build %s %s %s %s %s\n", plugin.format.c_str(), plugin.id.c_str(), plugin.version.c_str(), b.platform.c_str(), signers(b.signatures).c_str());
            return 0;
        }
        if(a.size() == 4 && a[0] == "file")
        {
            const auto k = signing::loadKey(a[1]);
            signing::writeSignatures(a[3], {{k.publicKey, signing::sign(k, signing::indexMessage(readFile(a[2])))}});
            std::printf("signed %s by %s\n", a[2].c_str(), signing::keyId(k.publicKey).c_str());
            return 0;
        }
        if(a.size() == 3 && a[0] == "verify-file")
        {
            const auto message = signing::indexMessage(readFile(a[1]));
            const auto list = signing::readSignatures(a[2]);
            if(list.empty()) throw std::runtime_error("no signature in " + a[2]);
            for(const auto& s : list)
                if(!signing::verify(s, message)) throw std::runtime_error("a signature by " + signing::keyId(s.key) + " does not verify");
            for(const auto& s : list) std::printf("file %s %s\n", signing::keyId(s.key).c_str(), s.key.c_str());
            return 0;
        }
        return usage();
    }
    catch(const std::exception& e) { std::fprintf(stderr, "oscairn-sign: %s\n", e.what()); return 1; }
}

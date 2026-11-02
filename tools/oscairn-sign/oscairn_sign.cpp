// SPDX-License-Identifier: AGPL-3.0-or-later
//
// oscairn-sign: package signatures (RFC 0006) for plugin authors and publishers. Makes a key,
// signs a plugin build (a .sig beside it, which an export carries), signs an archive whole, and
// checks an archive's signatures. A private key is never printed.
#include "oscairn/package.h"
#include "oscairn/signing.h"
#include <cstdio>
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
                   "      sign a plugin build, adding the signature to <build>.sig beside it\n"
                   "  oscairn-sign archive <key-file> <archive.olib>\n"
                   "      sign an archive whole (library.sig), replacing any signature over it\n"
                   "  oscairn-sign verify <archive.olib>\n"
                   "      check every signature and hash in an archive and say who signed what\n", stderr);
        return 2;
    }
    std::string signers(const std::vector<signing::Signature>& list)
    {
        if(list.empty()) return "unsigned";
        std::string out;
        for(const auto& s : list) out += (out.empty() ? "" : ",") + signing::keyId(s.key);
        return out;
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
            list.push_back({k.publicKey, signing::sign(k, signing::buildMessage(a[2], a[3], a[4], a[5], digest))});
            signing::writeSignatures(sig, list);
            std::printf("signed %s %s %s %s %s by %s\n", a[2].c_str(), a[3].c_str(), a[4].c_str(), a[5].c_str(), digest.c_str(), signing::keyId(k.publicKey).c_str());
            return 0;
        }
        if(a.size() == 3 && a[0] == "archive")
        {
            const auto k = signing::loadKey(a[1]);
            signPackage(a[2], k);
            std::printf("signed %s by %s\n", a[2].c_str(), signing::keyId(k.publicKey).c_str());
            return 0;
        }
        if(a.size() == 2 && a[0] == "verify")
        {
            std::random_device random;
            const auto into = fs::temp_directory_path() / ("oscairn-verify-" + std::to_string(random()) + std::to_string(random()));
            const auto p = readPackage(a[1], into); // throws on any signature or hash that does not hold
            std::error_code ignored; fs::remove_all(into, ignored);
            std::printf("archive %s %s\n", p.id.c_str(), p.signature.key.empty() ? "unsigned" : signing::keyId(p.signature.key).c_str());
            for(const auto& i : p.items) std::printf("item %s %s %s\n", i.kind.c_str(), i.id.c_str(), signers(i.signatures).c_str());
            for(const auto& plugin : p.plugins)
                for(const auto& b : plugin.builds) std::printf("build %s %s %s %s %s\n", plugin.format.c_str(), plugin.id.c_str(), plugin.version.c_str(), b.platform.c_str(), signers(b.signatures).c_str());
            return 0;
        }
        return usage();
    }
    catch(const std::exception& e) { std::fprintf(stderr, "oscairn-sign: %s\n", e.what()); return 1; }
}

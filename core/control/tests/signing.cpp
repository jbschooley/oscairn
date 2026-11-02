// SPDX-License-Identifier: AGPL-3.0-or-later
//
// RFC 0006's signatures: Ed25519 as RFC 8032 defines it, key IDs, key files only the user can
// read, and messages that keep one purpose's signature from passing for another's.
#include "oscairn/signing.h"
#include "oscairn/assets.h"
#include <cstdio>
#include <fstream>
#include <random>
#include <sys/stat.h>

using namespace oscairn;
namespace fs = std::filesystem;

namespace
{
    int failures = 0;
    void check(bool ok, const std::string& what) { if(!ok) { std::fprintf(stderr, "FAIL: %s\n", what.c_str()); ++failures; } }
    template<class F> bool throws(F f) { try { f(); } catch(const std::exception&) { return true; } return false; }
}

int main()
{
    std::random_device random;
    const auto root = fs::temp_directory_path() / ("oscairn-signing-" + std::to_string(random()));
    fs::create_directories(root);
    try
    {
        // RFC 8032, section 7.1, test 1: this seed, its public key, and its signature of nothing.
        const auto vector = root / "rfc8032.key";
        std::ofstream(vector) << "oscairn-key-1\n9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60\n";
        check(throws([&] { signing::loadKey(vector); }), "a key others can read is refused");
        fs::permissions(vector, fs::perms::owner_read | fs::perms::owner_write);
        const auto k = signing::loadKey(vector);
        check(k.publicKey == "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "the RFC 8032 public key");
        check(signing::sign(k, "") == "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
              "the RFC 8032 signature: Ed25519, not Monocypher's BLAKE2b EdDSA");
        check(signing::keyId(k.publicKey) == assets::sha256(std::string("\xd7\x5a\x98\x01\x82\xb1\x0a\xb7\xd5\x4b\xfe\xd3\xc9\x64\x07\x3a\x0e\xe1\x72\xf3\xda\xa6\x23\x25\xaf\x02\x1a\x68\xf7\x07\x51\x1a", 32)).substr(0, 16),
              "a key's ID is the first 16 hex digits of the SHA-256 of its bytes");
        check(signing::keyId("xyz").empty(), "a malformed key has no ID");

        // A new key: only the user can read it, and it cannot be made over another.
        const auto made = signing::makeKey(root / "Keys/mine.key");
        struct stat st{};
        check(::stat((root / "Keys/mine.key").c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "a new key file is readable only by the user");
        check(throws([&] { signing::makeKey(root / "Keys/mine.key"); }), "a key is not made over another");
        check(signing::loadKey(root / "Keys/mine.key").publicKey == made.publicKey && made.publicKey != k.publicKey, "a key loads as it was made");

        // Signatures verify for their message and key only, and each purpose's message is its own.
        const auto build = signing::buildMessage("clap", "com.example.synth", "1.0", "macos-arm64", std::string(64, 'a'));
        const signing::Signature s{made.publicKey, signing::sign(made, build)};
        check(signing::verify(s, build), "a build signature verifies");
        check(!signing::verify(s, signing::buildMessage("clap", "com.example.synth", "1.1", "macos-arm64", std::string(64, 'a'))), "not for another version");
        check(!signing::verify({k.publicKey, s.signature}, build), "not under another key");
        check(build.compare(0, 16, std::string("oscairn-build-1", 16)) == 0 && signing::archiveMessage("{}") == std::string("oscairn-archive-1\0{}", 20) &&
              signing::itemMessage("ab") == std::string("oscairn-item-1\0ab", 17) &&
              signing::indexMessage("{}") == std::string("oscairn-index-1\0{}", 18), "messages are domain-separated");
        const signing::Signature item{made.publicKey, signing::sign(made, signing::itemMessage(std::string(64, 'a')))};
        check(!signing::verify(item, signing::archiveMessage(std::string(64, 'a'))), "an item signature does not pass as an archive's");
        const signing::Signature repositoryFile{made.publicKey, signing::sign(made, signing::indexMessage("{}"))};
        check(!signing::verify(repositoryFile, signing::archiveMessage("{}")), "a repository file's signature does not pass as an archive's");

        // .sig files round-trip; a malformed one is refused.
        signing::writeSignatures(root / "Synth.clap.sig", {s, item});
        const auto read = signing::readSignatures(root / "Synth.clap.sig");
        check(read.size() == 2 && read[0].key == s.key && read[1].signature == item.signature, "a .sig file round-trips");
        check(signing::readSignatures(root / "none.sig").empty(), "no .sig file is no signatures");
        std::ofstream(root / "bad.sig") << R"([{"key":"00","signature":"11"}])";
        check(throws([&] { signing::readSignatures(root / "bad.sig"); }), "a malformed .sig file is refused");
    }
    catch(const std::exception& e) { check(false, e.what()); }
    std::error_code ignored; fs::remove_all(root, ignored);
    if(failures) return 1;
    std::puts("signing: ok");
    return 0;
}

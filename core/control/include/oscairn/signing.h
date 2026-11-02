// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

struct json_value_s;

// Package signatures (RFC 0006): Ed25519 (Monocypher), each message domain-separated by its
// purpose, so a signature over one kind of thing is never taken for another.
namespace oscairn::signing
{
    // A signature as archives and .sig files carry it: the signer's public key and the signature,
    // in lowercase hex (64 and 128 digits).
    struct Signature { std::string key, signature; };

    // A private key, loaded from its file. Never written anywhere but its own file.
    struct Key
    {
        std::string publicKey; // hex
        std::array<uint8_t, 64> secret{};
        Key() = default;
        Key(const Key&) = default;
        Key& operator=(const Key&) = default;
        ~Key();
    };

    // A key's ID, shown to people as its fingerprint: the first 16 hex digits of the SHA-256 of
    // its 32 bytes. Empty for a malformed key.
    std::string keyId(const std::string& publicKey);

    // A new key in `file`, which must not exist, readable and writable only by the user.
    Key makeKey(const std::filesystem::path& file);
    // A key from its file; refused when the file can be read by anyone but the user.
    Key loadKey(const std::filesystem::path& file);

    std::string sign(const Key& key, std::string_view message);
    bool verify(const Signature& s, std::string_view message);

    // The messages signed. A build: oscairn-build-1\0 and format, ID, version, platform and
    // digest, each NUL-terminated. An archive: oscairn-archive-1\0 and the manifest's exact bytes.
    // An item: oscairn-item-1\0 and its content hash (library.h's contentHash).
    std::string buildMessage(const std::string& format, const std::string& id, const std::string& version, const std::string& platform, const std::string& digest);
    std::string archiveMessage(std::string_view manifest);
    std::string itemMessage(const std::string& contentHash);

    // {"key":...,"signature":...}, and a list of them as a .sig file beside a plugin build holds
    // it. Reading a missing file gives none; a malformed one is refused.
    std::string json(const Signature& s);
    std::vector<Signature> readSignatures(const std::filesystem::path& file);
    void writeSignatures(const std::filesystem::path& file, const std::vector<Signature>& list);
    // The signatures in a parsed JSON array (none for null); refuses malformed entries.
    std::vector<Signature> signatures(const json_value_s* list);
    Signature signatureFrom(const json_value_s* object);
}

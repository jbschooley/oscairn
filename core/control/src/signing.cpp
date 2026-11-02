// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/signing.h"
#include "JsonInput.h"
#include "monocypher/monocypher-ed25519.h"
#include "monocypher/monocypher.h"
#include "oscairn/assets.h"
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace oscairn::signing
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr char header[] = "oscairn-key-1\n"; // a key file: this line, then the 32-byte seed in hex

        std::string hex(const uint8_t* bytes, size_t n)
        {
            constexpr char digits[] = "0123456789abcdef";
            std::string out;
            for(size_t i = 0; i < n; ++i) { out += digits[bytes[i] >> 4]; out += digits[bytes[i] & 15]; }
            return out;
        }
        // Exactly n bytes from 2n lowercase hex digits, else false.
        bool unhex(const std::string& text, uint8_t* out, size_t n)
        {
            if(text.size() != 2 * n) return false;
            auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
            for(size_t i = 0; i < n; ++i)
            {
                const int hi = digit(text[2 * i]), lo = digit(text[2 * i + 1]);
                if(hi < 0 || lo < 0) return false;
                out[i] = uint8_t(hi << 4 | lo);
            }
            return true;
        }
        Key fromSeed(uint8_t seed[32])
        {
            Key k;
            uint8_t pub[32];
            crypto_ed25519_key_pair(k.secret.data(), pub, seed); // wipes the seed
            k.publicKey = hex(pub, 32);
            return k;
        }
        std::string text(const json_value_s* v)
        {
            if(!v || v->type != json_type_string) throw std::runtime_error("a signature without its key or signature");
            const auto* s = static_cast<const json_string_s*>(v->payload);
            return {s->string, s->string_size};
        }
    }

    Key::~Key() { crypto_wipe(secret.data(), secret.size()); }

    std::string keyId(const std::string& publicKey)
    {
        uint8_t raw[32];
        if(!unhex(publicKey, raw, 32)) return {};
        return assets::sha256({reinterpret_cast<const char*>(raw), 32}).substr(0, 16);
    }

    Key makeKey(const fs::path& file)
    {
        uint8_t seed[32];
        {
            std::ifstream random("/dev/urandom", std::ios::binary);
            if(!random.read(reinterpret_cast<char*>(seed), 32)) throw std::runtime_error("no source of randomness for a key");
        }
        const std::string line = hex(seed, 32);
        auto key = fromSeed(seed);
        const std::string contents = header + line + '\n';
        if(!file.parent_path().empty()) fs::create_directories(file.parent_path());
        const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if(fd < 0) throw std::runtime_error("cannot make the key " + file.string() + " (it may exist already)");
        const bool written = ::write(fd, contents.data(), contents.size()) == ssize_t(contents.size());
        ::close(fd);
        std::string wipe = contents; crypto_wipe(wipe.data(), wipe.size());
        if(!written) { fs::remove(file); throw std::runtime_error("cannot write the key " + file.string()); }
        return key;
    }

    Key loadKey(const fs::path& file)
    {
        struct stat st{};
        if(::stat(file.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) throw std::runtime_error("no key " + file.string());
        if(st.st_mode & 077) throw std::runtime_error("the key " + file.string() + " can be read by others; make it readable only by you (chmod 600)");
        std::string contents(256, '\0');
        {
            std::ifstream in(file, std::ios::binary);
            in.read(contents.data(), std::streamsize(contents.size()));
            contents.resize(size_t(in.gcount()));
        }
        uint8_t seed[32];
        const auto prefix = std::string(header);
        const bool ok = contents.size() >= prefix.size() + 64 && contents.compare(0, prefix.size(), prefix) == 0 && unhex(contents.substr(prefix.size(), 64), seed, 32);
        crypto_wipe(contents.data(), contents.size());
        if(!ok) throw std::runtime_error("not an Oscairn key: " + file.string());
        return fromSeed(seed);
    }

    std::string sign(const Key& key, std::string_view message)
    {
        uint8_t signature[64];
        crypto_ed25519_sign(signature, key.secret.data(), reinterpret_cast<const uint8_t*>(message.data()), message.size());
        return hex(signature, 64);
    }

    bool verify(const Signature& s, std::string_view message)
    {
        uint8_t pub[32], signature[64];
        if(!unhex(s.key, pub, 32) || !unhex(s.signature, signature, 64)) return false;
        return crypto_ed25519_check(signature, pub, reinterpret_cast<const uint8_t*>(message.data()), message.size()) == 0;
    }

    std::string buildMessage(const std::string& format, const std::string& id, const std::string& version, const std::string& platform, const std::string& digest)
    {
        std::string m("oscairn-build-1", 16);
        for(const auto* part : {&format, &id, &version, &platform, &digest}) { m += *part; m += '\0'; }
        return m;
    }
    std::string archiveMessage(std::string_view manifest) { return std::string("oscairn-archive-1", 18) + std::string(manifest); }
    std::string itemMessage(const std::string& contentHash) { return std::string("oscairn-item-1", 15) + contentHash; }

    std::string json(const Signature& s) { return "{\"key\":" + detail::quote(s.key) + ",\"signature\":" + detail::quote(s.signature) + '}'; }

    Signature signatureFrom(const json_value_s* object)
    {
        Signature s{text(detail::member(object, "key")), text(detail::member(object, "signature"))};
        if(keyId(s.key).empty() || s.signature.size() != 128) throw std::runtime_error("a malformed signature");
        return s;
    }

    std::vector<Signature> signatures(const json_value_s* list)
    {
        std::vector<Signature> out;
        if(!list) return out;
        if(list->type != json_type_array) throw std::runtime_error("signatures that are not a list");
        size_t n = 0;
        for(auto* e = static_cast<const json_array_s*>(list->payload)->start; e; e = e->next)
        {
            if(++n > 64) throw std::runtime_error("too many signatures");
            out.push_back(signatureFrom(e->value));
        }
        return out;
    }

    std::vector<Signature> readSignatures(const fs::path& file)
    {
        std::error_code e;
        if(!fs::is_regular_file(file, e)) return {};
        if(fs::file_size(file) > 64 * 1024) throw std::runtime_error("a signature file too large: " + file.string());
        std::ifstream in(file, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if(!detail::jsonPreflight(bytes, 4).empty()) throw std::runtime_error("not a signature file: " + file.string());
        const std::unique_ptr<json_value_s, decltype(&std::free)> doc{json_parse(bytes.data(), bytes.size()), &std::free};
        if(!doc) throw std::runtime_error("not a signature file: " + file.string());
        return signatures(doc.get());
    }

    void writeSignatures(const fs::path& file, const std::vector<Signature>& list)
    {
        std::string out = "[";
        for(const auto& s : list) out += (out.size() > 1 ? "," : "") + json(s);
        out += "]\n";
        const auto partial = file.parent_path() / ("." + file.filename().string() + ".partial");
        {
            std::ofstream o(partial, std::ios::binary | std::ios::trunc);
            o << out;
            if(!o) throw std::runtime_error("cannot write " + file.string());
        }
        fs::rename(partial, file);
    }
}

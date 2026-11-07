// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/repository.h"
#include "oscairn/assets.h"
#include "oscairn/package.h"
#include "oscairn/signing.h"
#include "sheredom/json.h"
#include "JsonInput.h"
#include "miniz/miniz.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace oscairn::repository
{
    namespace
    {
        namespace fs = std::filesystem;
        using V = const json_value_s*;
        constexpr uint64_t maxRoot = 1 << 20, maxSignatures = 64 << 10, maxIndexGz = 64ull << 20, maxIndex = 256ull << 20, maxBlob = 8ull << 30;
        [[noreturn]] void bad(const std::string& why) { throw std::runtime_error("not a usable repository: " + why); }
        using Doc = std::unique_ptr<json_value_s, decltype(&std::free)>;
        Doc parse(const std::string& bytes, const char* what)
        {
            if(!detail::jsonPreflight(bytes, 16).empty()) bad(std::string(what) + " is not valid JSON");
            Doc d{json_parse(bytes.data(), bytes.size()), &std::free};
            if(!d || d->type != json_type_object) bad(std::string(what) + " is not a JSON object");
            return d;
        }
        std::string text(V v, const char* what, bool optional = false)
        {
            if((!v || v->type == json_type_null) && optional) return {};
            if(!v || v->type != json_type_string) bad(std::string("`") + what + "` is missing or not text");
            const auto* s = static_cast<const json_string_s*>(v->payload);
            if(s->string_size > 4096) bad(std::string("`") + what + "` is too long");
            return {s->string, s->string_size};
        }
        uint64_t number(V v, const char* what)
        {
            if(!v || v->type != json_type_number) bad(std::string("`") + what + "` is missing or not a number");
            const auto* n = static_cast<const json_number_s*>(v->payload);
            const std::string digits(n->number, n->number_size);
            if(digits.empty() || digits.size() > 19 || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) bad(std::string("`") + what + "` is not a whole number");
            return std::stoull(digits);
        }
        bool hex(const std::string& s, size_t digits) { return s.size() == digits && std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }); }
        template<class F> void each(V v, F f) { if(v && v->type == json_type_array) for(auto* e = static_cast<const json_array_s*>(v->payload)->start; e; e = e->next) f(e->value); }
        template<class F> void members(V v, F f) { if(v && v->type == json_type_object) for(auto* e = static_cast<const json_object_s*>(v->payload)->start; e; e = e->next) f(std::string(e->name->string, e->name->string_size), e->value); }

        class Folder final : public Source
        {
        public:
            explicit Folder(fs::path root) : root_(std::move(root)) {}
            std::string read(const std::string& path, uint64_t maxBytes) override
            {
                const auto file = local(path);
                std::error_code e;
                const auto size = fs::file_size(file, e);
                if(e) bad("no " + path + " in " + root_.string());
                if(size > maxBytes) bad(path + " is too large");
                std::string bytes(size_t(size), '\0');
                std::ifstream in(file, std::ios::binary);
                if(!in.read(bytes.data(), std::streamsize(bytes.size()))) bad(path + " cannot be read");
                return bytes;
            }
            void fetch(const std::string& sha256, uint64_t size, const fs::path& to) override
            {
                const auto file = local(blobPath(sha256));
                std::error_code e;
                if(fs::file_size(file, e) != size || e) bad("the package file is not on this drive, or not the size the index gives: " + blobPath(sha256));
                fs::copy_file(file, to, fs::copy_options::overwrite_existing);
                if(assets::sha256File(to) != sha256) { fs::remove(to, e); bad("the package file is not the one the index names: " + blobPath(sha256)); }
            }
            fs::path local(const std::string& path) override
            {
                // Paths come from the root or are made here from hashes; nothing climbs out.
                if(path.empty() || path.find("..") != std::string::npos || path.front() == '/' || path.find('\\') != std::string::npos) bad("an unsafe path: " + path);
                return root_ / path;
            }

        private:
            fs::path root_;
        };

        std::mutex transportMutex;
        Transport transport; // set by the host; guarded by transportMutex
        bool webAddress(const std::string& url) { return url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0; }

        class Web final : public Source
        {
        public:
            Web(std::string base, const std::atomic<bool>* cancel) : base_(std::move(base)), cancel_(cancel) {}
            // From the base; an index or delta (named by its hash) from each mirror in turn too.
            std::string read(const std::string& path, uint64_t maxBytes) override
            {
                std::vector<std::string> from{base_ + path};
                if(path.rfind("index/", 0) == 0) for(const auto& m : mirrors_) from.push_back(m + path);
                std::string why;
                for(const auto& url : from)
                {
                    std::string bytes;
                    try
                    {
                        bool tooLarge = false;
                        const auto status = get(url, 0, [&](int answer, const char* data, size_t n) {
                            if(answer != 200) return false;
                            if(bytes.size() + n > maxBytes) { tooLarge = true; return false; }
                            bytes.append(data, n); return true;
                        });
                        if(tooLarge) { why = path + " is too large at " + url; continue; }
                        if(status == 200) return bytes;
                        why = url + " answered " + std::to_string(status);
                    }
                    catch(const std::runtime_error& e) { why = e.what(); }
                }
                bad("cannot fetch " + path + ": " + why);
            }
            void fetch(const std::string& sha256, uint64_t size, const fs::path& to) override
            {
                const auto tail = sha256.substr(0, 2) + "/" + sha256;
                std::vector<std::string> from;
                for(const auto& m : mirrors_) from.push_back(m + "blobs/" + tail);
                if(webAddress(blobBase_)) from.push_back(blobBase_ + tail);
                from.push_back(base_ + "blobs/" + tail);
                std::string why;
                for(const auto& url : from)
                {
                    try
                    {
                        // Resumed from what is there already: the hash says whether the whole is right.
                        std::error_code e;
                        uint64_t have = fs::exists(to, e) ? fs::file_size(to, e) : 0;
                        if(have > size) { fs::remove(to, e); have = 0; }
                        std::ofstream out(to, std::ios::binary | std::ios::app);
                        bool first = true;
                        const auto status = have == size ? 206 : get(url, have, [&](int answer, const char* data, size_t n) {
                            if(answer != 200 && answer != 206) return false;
                            // A 200 to a range is the whole file again: what was there goes first.
                            if(first && answer == 200 && have) { out.close(); out.open(to, std::ios::binary | std::ios::trunc); have = 0; }
                            first = false;
                            if(have + n > size) return false;
                            out.write(data, std::streamsize(n)); have += n;
                            return bool(out);
                        });
                        out.close();
                        if(status != 200 && status != 206) { why = url + " answered " + std::to_string(status); continue; }
                        if(have < size) { why = "the download from " + url + " was cut short; installing again resumes it"; continue; }
                        if(assets::sha256File(to) == sha256) return;
                        fs::remove(to, e); // a mirror's bad copy: refused, and the next one tried
                        why = url + " served a file that is not the one the index names";
                    }
                    catch(const std::runtime_error& e) { why = e.what(); }
                    if(cancel_ && *cancel_) break;
                }
                bad("cannot fetch the package file " + sha256 + ": " + why);
            }
            void places(const std::vector<std::string>& mirrors, const std::string& blobBase) override
            {
                for(auto m : mirrors) if(webAddress(m)) { if(m.back() != '/') m += '/'; mirrors_.push_back(m); }
                blobBase_ = blobBase;
            }

        private:
            // One GET through the host's transport, stopped when canceled.
            int get(const std::string& url, uint64_t from, const std::function<bool(int, const char*, size_t)>& sink)
            {
                Transport t;
                { std::lock_guard lock(transportMutex); t = transport; }
                if(!t) throw std::runtime_error("this build cannot fetch from the web");
                return t(url, from, [&](int status, const char* data, size_t n) { return !(cancel_ && *cancel_) && sink(status, data, n); });
            }
            std::string base_, blobBase_;
            std::vector<std::string> mirrors_;
            const std::atomic<bool>* cancel_;
        };
    }

    void setTransport(Transport t) { std::lock_guard lock(transportMutex); transport = std::move(t); }

    std::unique_ptr<Source> web(const std::string& base, const std::atomic<bool>* cancel)
    {
        if(!webAddress(base) || base.size() > 4096) throw std::invalid_argument("a repository on the web is an https:// or http:// address");
        return std::make_unique<Web>(base.back() == '/' ? base : base + "/", cancel);
    }

    std::unique_ptr<Source> folder(const fs::path& root)
    {
        if(!fs::is_regular_file(root / "repository.json")) throw std::runtime_error("not a repository: no repository.json in " + root.string());
        return std::make_unique<Folder>(root);
    }

    std::string blobPath(const std::string& sha256) { return "blobs/" + sha256.substr(0, 2) + "/" + sha256; }

    bool older(const std::string& a, const std::string& b)
    {
        auto parts = [](const std::string& v) {
            std::vector<std::string> out; std::string part;
            for(const char c : v + ".") if(c == '.' || c == '-' || c == '+') { out.push_back(part); part.clear(); } else part += c;
            return out;
        };
        const auto x = parts(a), y = parts(b);
        for(size_t i = 0; i < std::max(x.size(), y.size()); ++i)
        {
            const auto p = i < x.size() ? x[i] : "0", q = i < y.size() ? y[i] : "0";
            const bool pn = !p.empty() && std::all_of(p.begin(), p.end(), ::isdigit), qn = !q.empty() && std::all_of(q.begin(), q.end(), ::isdigit);
            if(pn && qn)
            {
                const auto pv = p.find_first_not_of('0') == std::string::npos ? std::string("0") : p.substr(p.find_first_not_of('0'));
                const auto qv = q.find_first_not_of('0') == std::string::npos ? std::string("0") : q.substr(q.find_first_not_of('0'));
                if(pv.size() != qv.size()) return pv.size() < qv.size();
                if(pv != qv) return pv < qv;
            }
            else if(pn != qn) return pn; // a number before a word, as the index builder orders them
            else if(p != q) return p < q;
        }
        return false;
    }

    std::string incompatible(const Version& v, const std::string& system, const std::string& platform)
    {
        if(!v.needs.empty() && older(system, v.needs)) return "it needs a system update: Oscairn " + v.needs + " or later";
        if(v.code && std::find(v.platforms.begin(), v.platforms.end(), platform) == v.platforms.end()) return "it has no build for this device (" + platform + ")";
        return {};
    }

    std::string gunzip(std::string_view gz, uint64_t maxBytes)
    {
        const auto* b = reinterpret_cast<const uint8_t*>(gz.data());
        if(gz.size() < 18 || b[0] != 0x1f || b[1] != 0x8b || b[2] != 8) bad("the index is not gzip");
        const uint8_t flags = b[3];
        size_t at = 10;
        if(flags & 4) { if(at + 2 > gz.size()) bad("the index is cut short"); at += 2 + size_t(b[at] | (b[at + 1] << 8)); }
        for(const uint8_t text : {uint8_t(8), uint8_t(16)}) if(flags & text) { while(at < gz.size() && b[at]) ++at; ++at; }
        if(flags & 2) at += 2;
        if(at + 8 > gz.size()) bad("the index is cut short");
        tinfl_decompressor inflator; tinfl_init(&inflator);
        std::vector<uint8_t> dictionary(TINFL_LZ_DICT_SIZE);
        size_t offset = 0, left = gz.size() - 8 - at;
        const uint8_t* next = b + at;
        std::string out;
        for(;;)
        {
            size_t in = left, made = TINFL_LZ_DICT_SIZE - offset;
            const auto status = tinfl_decompress(&inflator, next, &in, dictionary.data(), dictionary.data() + offset, &made, 0);
            next += in; left -= in;
            out.append(reinterpret_cast<const char*>(dictionary.data() + offset), made);
            if(out.size() > maxBytes) bad("the index is too large");
            offset = (offset + made) & (TINFL_LZ_DICT_SIZE - 1);
            if(status == TINFL_STATUS_DONE) break;
            if(status != TINFL_STATUS_HAS_MORE_OUTPUT) bad("the index is damaged");
        }
        const auto* trailer = next;
        if(size_t(trailer - b) + 8 > gz.size()) bad("the index is cut short");
        auto word = [&](size_t i) { return uint32_t(trailer[i]) | uint32_t(trailer[i + 1]) << 8 | uint32_t(trailer[i + 2]) << 16 | uint32_t(trailer[i + 3]) << 24; };
        if(word(0) != uint32_t(mz_crc32(MZ_CRC32_INIT, reinterpret_cast<const uint8_t*>(out.data()), out.size())) || word(4) != uint32_t(out.size())) bad("the index is damaged");
        return out;
    }

    Index open(Source& source, const std::vector<std::string>& trusted, const std::vector<std::string>& revoked, std::string& rootKey, const fs::path& cache)
    {
        const auto rootBytes = source.read("repository.json", maxRoot);
        const auto root = parse(rootBytes, "repository.json");
        V r = root.get();
        if(text(detail::member(r, "format"), "format") != "oscairn.repository" || number(detail::member(r, "version"), "version") != 1) bad("repository.json is not format oscairn.repository, version 1");
        Index index;
        index.repository = text(detail::member(r, "repository"), "repository");
        index.name = text(detail::member(r, "name"), "name");
        index.serial = number(detail::member(r, "serial"), "serial");
        index.rootKey = rootKey = text(detail::member(r, "rootKey"), "rootKey");
        const V indexKey = detail::member(r, "indexKey");
        index.indexKey = text(detail::member(indexKey, "key"), "indexKey.key");
        const signing::Signature delegation{index.rootKey, text(detail::member(indexKey, "signature"), "indexKey.signature")};
        if(!hex(index.rootKey, 64) || !hex(index.indexKey, 64) || !hex(delegation.signature, 128)) bad("its keys are malformed");
        index.blobBase = text(detail::member(r, "blobBase"), "blobBase", true);
        each(detail::member(r, "mirrors"), [&](V m) { index.mirrors.push_back(text(m, "mirrors")); });

        // Trust: the root key is one this device trusts for repositories, it signed the index
        // key, and the index key signed this root. A revoked key is never accepted.
        const auto fingerprint = signing::keyId(index.rootKey);
        for(const auto* key : {&index.rootKey, &index.indexKey})
            if(std::find(revoked.begin(), revoked.end(), *key) != revoked.end()) bad("it is signed by a revoked key (" + signing::keyId(*key) + ")");
        if(std::find(trusted.begin(), trusted.end(), index.rootKey) == trusted.end())
            throw std::runtime_error("not a repository this device trusts: its root key is " + fingerprint + "; trust that key for repositories to use it");
        if(!signing::verify(delegation, signing::indexMessage(index.indexKey + "\n"))) bad("its index key is not signed by its root key");
        const auto signatureBytes = source.read("repository.sig", maxSignatures);
        const auto signatureDoc = Doc{json_parse(signatureBytes.data(), signatureBytes.size()), &std::free};
        if(!detail::jsonPreflight(signatureBytes, 4).empty() || !signatureDoc || signatureDoc->type != json_type_array) bad("repository.sig is not a list of signatures");
        std::vector<signing::Signature> signatures;
        try { signatures = signing::signatures(signatureDoc.get()); } catch(const std::runtime_error& e) { bad(std::string("repository.sig: ") + e.what()); }
        bool byIndexKey = false;
        for(const auto& s : signatures)
        {
            if(!signing::verify(s, signing::indexMessage(rootBytes))) bad("its root's signature does not verify (" + signing::keyId(s.key) + ")");
            byIndexKey = byIndexKey || s.key == index.indexKey;
        }
        if(!byIndexKey) bad("its root is not signed by its index key");

        source.places(index.mirrors, index.blobBase);
        // A file the root names by its hash and size: index/<sha256><suffix>.
        auto named = [&](V entry, const char* suffix) {
            const auto path = text(detail::member(entry, "path"), "path"), sha = text(detail::member(entry, "sha256"), "sha256");
            const auto size = number(detail::member(entry, "size"), "size");
            if(!hex(sha, 64) || path != "index/" + sha + suffix || size > maxIndexGz) bad("its root names its index files wrongly");
            const auto gz = source.read(path, size);
            if(gz.size() != size || assets::sha256(gz) != sha) bad(path + " is not the one its root names (changed or damaged)");
            return gunzip(gz, maxIndex);
        };
        // The index this device read last from this repository, if any: used again at the same
        // serial, or brought up to date by the delta the root names from its serial.
        const bool plain = !index.repository.empty() && index.repository.size() <= 64 && index.repository.front() != '.' &&
                           std::all_of(index.repository.begin(), index.repository.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_'; });
        const auto cached = cache.empty() || !plain ? fs::path() : cache / (index.repository + ".index.json");
        std::string text_;
        if(!cached.empty())
            try
            {
                std::error_code e;
                if(fs::is_regular_file(cached, e) && fs::file_size(cached, e) <= maxIndex)
                {
                    std::ifstream in(cached, std::ios::binary);
                    std::string before((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    const auto old = parse(before, "the kept index");
                    const auto serial = number(detail::member(old.get(), "serial"), "serial");
                    if(text(detail::member(old.get(), "repository"), "repository") != index.repository) {}
                    else if(serial == index.serial) text_ = std::move(before);
                    else
                    {
                        V delta = nullptr;
                        each(detail::member(r, "deltas"), [&](V x) { if(detail::member(x, "from") && number(detail::member(x, "from"), "from") == serial) delta = x; });
                        if(delta)
                        {
                            const auto change = parse(named(delta, ".delta.gz"), "the delta");
                            V c = change.get();
                            if(text(detail::member(c, "format"), "format") != "oscairn.repository-delta" || number(detail::member(c, "version"), "version") != 1 ||
                               text(detail::member(c, "repository"), "repository") != index.repository || number(detail::member(c, "from"), "from") != serial ||
                               number(detail::member(c, "serial"), "serial") != index.serial)
                                bad("the delta is not the root's");
                            // Each package record the delta holds replaces the kept one; those removed go.
                            std::map<std::string, std::string> records;
                            auto record = [&](V p) {
                                void* json = json_write_minified(p, nullptr);
                                if(!json) bad("an index record cannot be kept");
                                records[text(detail::member(p, "id"), "id")] = static_cast<const char*>(json);
                                std::free(json);
                            };
                            each(detail::member(old.get(), "packages"), record);
                            each(detail::member(c, "packages"), record);
                            each(detail::member(c, "removed"), [&](V id) { records.erase(text(id, "removed")); });
                            text_ = "{\"format\":\"oscairn.repository-index\",\"version\":1,\"repository\":" + detail::quote(index.repository) + ",\"serial\":" + std::to_string(index.serial) + ",\"packages\":[";
                            bool first = true;
                            for(const auto& [id, json] : records) { text_ += (first ? "" : ",") + json; first = false; }
                            text_ += "]}";
                        }
                    }
                }
            }
            catch(const std::runtime_error&) { text_.clear(); } // a kept index or delta that does not hold: the whole index instead
        if(text_.empty()) text_ = named(detail::member(r, "index"), ".json.gz");
        const auto doc = parse(text_, "the index");
        V d = doc.get();
        if(text(detail::member(d, "format"), "format") != "oscairn.repository-index" || number(detail::member(d, "version"), "version") != 1) bad("the index is not format oscairn.repository-index, version 1");
        if(text(detail::member(d, "repository"), "repository") != index.repository || number(detail::member(d, "serial"), "serial") != index.serial) bad("the index is not the root's");
        each(detail::member(d, "packages"), [&](V p) {
            Package package;
            package.id = text(detail::member(p, "id"), "id"); package.name = text(detail::member(p, "name"), "name");
            package.summary = text(detail::member(p, "summary"), "summary", true); package.license = text(detail::member(p, "license"), "license", true);
            each(detail::member(p, "tags"), [&](V t) { package.tags.push_back(text(t, "tags")); });
            const V publisher = detail::member(p, "publisher");
            package.publisherId = text(detail::member(publisher, "id"), "publisher.id"); package.publisherName = text(detail::member(publisher, "name"), "publisher.name");
            package.publisherKey = text(detail::member(publisher, "key"), "publisher.key");
            if(!hex(package.publisherKey, 16)) bad("a publisher's key is malformed: " + package.id);
            each(detail::member(p, "versions"), [&](V v) {
                Version version;
                version.version = text(detail::member(v, "version"), "version"); version.channel = text(detail::member(v, "channel"), "channel", true);
                version.yanked = text(detail::member(v, "yanked"), "yanked", true);
                version.needs = text(detail::member(detail::member(v, "requires"), "oscairn"), "requires.oscairn", true);
                version.manifestSha256 = text(detail::member(detail::member(v, "manifest"), "sha256"), "manifest.sha256");
                const V archive = detail::member(v, "archive");
                version.archiveSha256 = text(detail::member(archive, "sha256"), "archive.sha256");
                version.archiveSize = number(detail::member(archive, "size"), "archive.size");
                if(!hex(version.manifestSha256, 64) || !hex(version.archiveSha256, 64) || version.archiveSize > maxBlob) bad("a version's files are named wrongly: " + package.id + " " + version.version);
                members(detail::member(v, "types"), [&](const std::string& type, V n) { if((type == "clap" || type == "lv2") && number(n, "types")) version.code = true; });
                auto sizes = [&](V s) { version.size += number(detail::member(s, "size"), "size"); version.installed += number(detail::member(s, "installed"), "installed"); };
                members(detail::member(v, "platforms"), [&](const std::string& name, V s) { version.platforms.push_back(name); if(name == platform()) sizes(s); });
                if(const V common = detail::member(v, "common")) sizes(common);
                package.versions.push_back(std::move(version));
            });
            index.packages.push_back(std::move(package));
        });
        // Kept for the next refresh once it has all been read.
        if(!cached.empty())
        {
            std::error_code e;
            fs::create_directories(cache, e);
            const auto partial = cache / ("." + index.repository + ".partial");
            { std::ofstream out(partial, std::ios::binary | std::ios::trunc); out << text_; }
            fs::rename(partial, cached, e);
        }
        return index;
    }
}

// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/assets.h"
#include "../../plugin-host/src/StateFiles.h"
#include <fstream>
#include <iostream>
using namespace oscairn;
namespace fs = std::filesystem;
void check(bool ok, const std::string& why) { if(!ok) throw std::runtime_error(why); }
void write(const fs::path& file, const std::string& text) { fs::create_directories(file.parent_path()); std::ofstream(file, std::ios::binary) << text; }
std::string failure(assets::Store& store, assets::Ref ref) { try { store.resolve(ref); } catch(const std::exception& e) { return e.what(); } return "resolved"; }

int main() { try {
    // FIPS 180-4 / NIST example vectors.
    check(assets::sha256(std::string_view("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of empty input");
    check(assets::sha256(std::string_view("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256 of abc");
    check(assets::sha256(std::string_view("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", "SHA-256 of the 448-bit message");
    check(assets::sha256(std::string(1000000, 'a')) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", "SHA-256 of a million a");

    state_files::TemporaryDirectory temp;
    const auto piano = temp.path / "Grand piano \xC3\xBC", other = temp.path / "Other copy";
    write(piano / "piano.sfz", "<region> sample=samples/C4.wav\n"); write(piano / "samples/C4.wav", "C4 audio"); write(temp.path / "outside.wav", "outside");
    fs::create_symlink(temp.path / "outside.wav", piano / "samples/escape.wav");
    assets::Store store(temp.path / "store");
    const auto id = store.registerFolder(piano);
    check(store.registerFolder(piano) == id, "re-registering a folder keeps its ID");
    auto libraries = store.libraries();
    check(libraries.size() == 1 && libraries[0].name == "Grand piano \xC3\xBC" && libraries[0].files == 2 && libraries[0].fingerprint.size() == 64, "registered folder indexed without its links");
    std::string library, relative;
    check(store.locate(piano / "samples", library, relative) && library == id && relative == "samples", "locate a directory inside a library");
    check(!store.locate(temp.path, library, relative), "directories outside libraries are not located");

    assets::Ref ref{id, "samples/C4.wav", ""};
    check(store.resolve(ref) == fs::canonical(piano / "samples/C4.wav") && ref.sha256 == assets::sha256(std::string_view("C4 audio")), "resolve fills in the content hash");
    const auto saved = ref;
    check(failure(store, {id, "../outside.wav", ""}).find("invalid asset path") != std::string::npos, "paths cannot leave the library");
    check(failure(store, {id, "samples/escape.wav", ""}).find("outside library") != std::string::npos, "links cannot leave the library");
    check(failure(store, {id, "samples/missing.wav", ""}).find("not found in library") != std::string::npos, "missing file reported");
    check(failure(store, {"0123456789abcdef", "x.wav", ""}).find("is not registered") != std::string::npos, "unknown library reported");

    // Identical bytes in another library satisfy a reference with a known hash.
    write(other / "C4 copy.wav", "C4 audio"); store.registerFolder(other);
    fs::remove(piano / "samples/C4.wav");
    { auto moved = saved; check(store.resolve(moved) == fs::canonical(other / "C4 copy.wav"), "content hash finds the same file in another library"); }
    write(piano / "samples/C4.wav", "edited audio"); fs::remove(other / "C4 copy.wav");
    check(failure(store, saved).find("changed since it was saved") != std::string::npos, "an edited file does not satisfy the saved hash");

    // A single imported file lives in the user library and survives deleting the original.
    write(temp.path / "Downloads/take 1.wav", "take one");
    const auto imported = store.import(temp.path / "Downloads/take 1.wav");
    fs::remove_all(temp.path / "Downloads");
    { auto again = imported; check(store.resolve(again).filename() == "take 1.wav" && again.library == "user", "imported file resolves after the original is deleted"); }

    // Registrations and indexes persist for a new store on the same root.
    assets::Store reopened(temp.path / "store");
    check(reopened.libraries().size() == 3, "libraries persist");
    { auto again = imported; reopened.resolve(again); auto piece = assets::Ref{id, "piano.sfz", ""}; reopened.resolve(piece); check(piece.sha256.size() == 64, "reopened store resolves"); }
    // Two stores on one root (two engine processes) keep each other's registrations.
    {
        assets::Store first(temp.path / "shared"), second(temp.path / "shared");
        write(temp.path / "A/a.wav", "a"); write(temp.path / "B/b.wav", "b");
        first.libraries(); second.libraries(); // both have read the (empty) registry
        const auto a = first.registerFolder(temp.path / "A"), b = second.registerFolder(temp.path / "B");
        const auto all = assets::Store(temp.path / "shared").libraries();
        check(all.size() == 2 && first.libraries().size() == 2, "registrations from separate stores are both kept");
        auto ref = assets::Ref{a, "a.wav", ""}; second.resolve(ref); check(ref.sha256.size() == 64, "a library registered by another store resolves");
        (void)b;
    }
    std::cout << "ok: SHA-256 vectors, registered folders, content-hash resolution, path safety, import and persistence\n";
} catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }

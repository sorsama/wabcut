// P.2 (E5): Keyboard Shortcuts > Save As a Premiere key set (.kys) - the exporter writes what the importer reads, so a
// set saved from Wabcut opens in Premiere (and in Wabcut) with the same keys.

#include <wabcut/keymap/KysImport.h>

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>

using namespace wabcut;
using namespace wabcut::keymap;

namespace {

const CommandOverride* findOverride(const KysImport& import, std::string_view command) {
    const auto it = std::ranges::find(import.overrides, CommandId(command), &CommandOverride::command);
    return it == import.overrides.end() ? nullptr : &*it;
}

} // namespace

TEST_CASE("Kys virtual keys encode as they decode", "[keymap][kys][export]") {
    for (const std::uint32_t code : {1u, 2u, 3u, 4u, 7u, 18u, 35u, 36u, 37u, 38u, 39u, 42u, 43u, 44u, 45u}) {
        const auto chord = decodeKysVirtualKey(code);
        REQUIRE(chord);
        CHECK(encodeKysVirtualKey(*chord) == code);
    }
    CHECK(encodeKysVirtualKey(KeyChord{Key_K}) == (0x80000000u | 'K'));
    CHECK(encodeKysVirtualKey(KeyChord{Key_Semicolon}) == (0x80000000u | ';'));
    CHECK(encodeKysVirtualKey(KeyChord{Key_5, Modifier::Keypad}) == (0xC0000000u | '5'));
    CHECK_FALSE(encodeKysVirtualKey(KeyChord{Key_Escape}).has_value()); // no verified code
}

TEST_CASE("A saved key set reads back with the same shortcuts", "[keymap][kys][export]") {
    for (const Platform platform : {Platform::Windows, Platform::MacOS}) {
        INFO(platformId(platform));
        auto keymap = KeymapManager::withDefaults(platform);
        REQUIRE(keymap);
        // A user binding: Razor on X, and Ripple Delete without a key.
        REQUIRE(keymap->setChords("tool.razor", {KeyChord{Key_X}}));
        REQUIRE(keymap->setChords("edit.rippleDelete", {}));
        const KysExport exported = exportKys(*keymap);
        CHECK(exported.commands > 100);
        CHECK(exported.xml.find(platform == Platform::MacOS ? "<platform>macintosh</platform>"
                                                             : "<platform>windows</platform>") != std::string::npos);
        const auto read = parseKys(exported.xml);
        REQUIRE(read);
        CHECK(read->platform == (platform == Platform::MacOS ? Platform::MacOS : Platform::Windows));
        const CommandOverride* razor = findOverride(*read, "tool.razor");
        REQUIRE(razor);
        CHECK(razor->chords == std::vector<KeyChord>{KeyChord{Key_X}});
        const CommandOverride* ripple = findOverride(*read, "edit.rippleDelete");
        REQUIRE(ripple);
        CHECK(ripple->chords.empty());
        // Every command with a Premiere identity and keys the format can carry comes back with them.
        for (const CommandInfo& info : keymap->registry().all()) {
            if (info.kysName.empty()) {
                continue;
            }
            std::vector<KeyChord> expected;
            for (const KeyChord& c : keymap->chordsFor(info.id)) {
                if (encodeKysVirtualKey(c)) {
                    expected.push_back(c);
                }
            }
            const CommandOverride* o = findOverride(*read, info.id);
            INFO(info.id);
            REQUIRE(o);
            if (expected.size() == keymap->chordsFor(info.id).size()) { // none skipped
                std::vector<KeyChord> got = o->chords;
                std::ranges::sort(got);
                std::ranges::sort(expected);
                CHECK(got == expected);
            }
        }
        // Applied to a fresh keymap of the platform, it gives the same keys.
        auto fresh = KeymapManager::withDefaults(platform);
        REQUIRE(fresh);
        REQUIRE(applyKysImport(*fresh, *read));
        CHECK(fresh->chordsFor("tool.razor") == std::vector<KeyChord>{KeyChord{Key_X}});
    }
}

TEST_CASE("saveKys writes the key set to a file", "[keymap][kys][export]") {
    const auto keymap = KeymapManager::withDefaults(Platform::Windows);
    REQUIRE(keymap);
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "wabcut-test-export.kys";
    const auto saved = saveKys(*keymap, file);
    REQUIRE(saved);
    const auto read = importKys(file);
    REQUIRE(read);
    CHECK_FALSE(read->overrides.empty());
    std::error_code ec;
    std::filesystem::remove(file, ec);
    CHECK_FALSE(saveKys(*keymap, std::filesystem::path("Z:/no/such/folder/x.kys")));
}

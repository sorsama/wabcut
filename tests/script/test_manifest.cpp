// Extension manifests and folders (C.12): untrusted input, bounded and checked field by field.

#include "ScriptTestUtil.h"

#include <wabcut/script/Extension.h>
#include <wabcut/script/Manifest.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::script;
using namespace wabcut::script::test;

namespace {

Json good() {
    Json m = manifest("com.example.beat-markers", "Beat Markers", {"Read", "EditSequence"});
    m["description"] = "Adds markers.\nAt every beat.";
    m["scripts"] = Json::array({"lib/beats.js"});
    m["commands"] = Json::array({{{"id", "addBeatMarkers"}, {"title", "Add Markers at Every Beat"}}});
    return m;
}

/// Why the extension in `folder` does not load ("" when it does).
std::string loadError(const fs::path& folder) {
    auto bundle = loadExtension(folder, ExtensionScope::User);
    return bundle ? std::string() : bundle.error().message;
}

/// The issues of `m` as "field: message" text (empty when it parses).
std::string issues(const Json& m, const std::optional<Version>& running = {}) {
    auto parsed = parseManifest(m.dump(), running);
    return parsed ? std::string() : parsed.error().summary();
}

} // namespace

TEST_CASE("A manifest names the extension; what it asks for and where its code starts", "[script][manifest]") {
    auto parsed = parseManifest(good().dump(), parseVersion("0.1.0"));
    REQUIRE(parsed.has_value());
    CHECK(parsed->id == "com.example.beat-markers");
    CHECK(parsed->name == "Beat Markers");
    CHECK(toString(parsed->version) == "1.0.0");
    CHECK(parsed->licence == "MIT");
    CHECK(parsed->capabilities == std::vector{command::Capability::Read, command::Capability::EditSequence});
    CHECK(parsed->requested().has(command::Capability::EditSequence));
    CHECK_FALSE(parsed->requested().has(command::Capability::EditProject));
    CHECK(parsed->files() == std::vector<std::string>{"lib/beats.js", "main.js"});
    REQUIRE(parsed->commands.size() == 1);
    CHECK(parsed->commands[0].title == "Add Markers at Every Beat");
    CHECK(parsed->warnings.empty());
    // Written back in the file's format, it reads the same.
    auto again = parseManifest(toJson(*parsed).dump());
    REQUIRE(again.has_value());
    CHECK(toJson(*again) == toJson(*parsed));

    // "license" is read too; unknown keys are ignored with a warning (a newer manifest still loads).
    Json m = good();
    m.erase("licence");
    m["license"] = "Apache-2.0";
    m["homepage"] = "https://example.com";
    auto us = parseManifest(m.dump());
    REQUIRE(us.has_value());
    CHECK(us->licence == "Apache-2.0");
    REQUIRE(us->warnings.size() == 1);
    CHECK(us->warnings[0].find("homepage") != std::string::npos);
}

TEST_CASE("Manifests are refused field by field; with the field named", "[script][manifest]") {
    const auto with = [](const char* key, Json value) {
        Json m = good();
        m[key] = std::move(value);
        return m;
    };
    const auto without = [](const char* key) {
        Json m = good();
        m.erase(key);
        return m;
    };
    for (const char* key : {"id", "name", "version", "author", "licence", "minWabcutVersion", "capabilities", "main",
                            "manifestVersion"}) {
        INFO(key);
        CHECK(issues(without(key)).find(key) != std::string::npos);
    }
    CHECK(issues(with("manifestVersion", 2)).find("newer Wabcut") != std::string::npos);
    CHECK(issues(with("id", "BeatMarkers")).find("reverse-DNS") != std::string::npos);
    CHECK(issues(with("id", "com..example")).find("reverse-DNS") != std::string::npos);
    CHECK(issues(with("id", "com.example-")).find("reverse-DNS") != std::string::npos);
    CHECK(issues(with("version", "1.0")).find("version") != std::string::npos);
    CHECK(issues(with("version", "01.0.0")).find("version") != std::string::npos);
    CHECK(issues(with("version", "1.0.0+build")).find("version") != std::string::npos);
    CHECK(issues(with("name", "")).find("name") != std::string::npos);
    CHECK(issues(with("name", std::string(65, 'n'))).find("name") != std::string::npos);
    CHECK(issues(with("name", 7)).find("must be a string") != std::string::npos);
    // A name that could pass for another in the permission dialog: bidirectional overrides, invisible characters,
    // control characters.
    CHECK(issues(with("name", "Beat \xE2\x80\xAE" "srekraM")).find("invisible") != std::string::npos);
    CHECK(issues(with("name", "Beat\xE2\x80\x8BMarkers")).find("invisible") != std::string::npos);
    CHECK(issues(with("name", "Beat\nMarkers")).find("control") != std::string::npos);
    CHECK(issues(with("description", "Two\nlines")).empty()); // descriptions may break lines

    // Capabilities: known names, once each, never Network.
    CHECK(issues(with("capabilities", Json::array({"Network"}))).find("no network access") != std::string::npos);
    CHECK(issues(with("capabilities", Json::array({"Root"}))).find("capabilities[0]") != std::string::npos);
    CHECK(issues(with("capabilities", Json::array({"Read", "Read"}))).find("twice") != std::string::npos);
    CHECK(issues(with("capabilities", "Read")).find("array") != std::string::npos);
    CHECK(issues(with("capabilities", Json::array())).empty()); // UI and storage only

    // Scripts stay inside the extension.
    for (const char* path : {"../main.js", "/main.js", "C:/main.js", "main", "lib\\main.js", "a/./main.js",
                             "lib//main.js", "main.mjs"}) {
        INFO(path);
        CHECK(issues(with("main", path)).find("main") != std::string::npos);
    }
    CHECK(issues(with("scripts", Json::array({"main.js"}))).find("twice") != std::string::npos);
    CHECK(issues(with("scripts", Json::array({"../x.js"}))).find("scripts[0]") != std::string::npos);
    CHECK(issues(with("commands", Json::array({{{"id", "1st"}, {"title", "First"}}}))).find("commands[0].id") !=
          std::string::npos);
    CHECK(issues(with("commands", Json::array({{{"id", "a"}, {"title", "A"}}, {{"id", "a"}, {"title", "B"}}})))
              .find("declared twice") != std::string::npos);

    // An extension that needs a newer Wabcut is refused only when the running version is known.
    CHECK(issues(with("minWabcutVersion", "9.0.0")).empty());
    CHECK(issues(with("minWabcutVersion", "9.0.0"), parseVersion("0.1.0")).find("needs Wabcut 9.0.0") !=
          std::string::npos);

    // Several problems are reported together.
    Json bad = good();
    bad["id"] = "x";
    bad["version"] = "one";
    auto parsed = parseManifest(bad.dump());
    REQUIRE_FALSE(parsed.has_value());
    CHECK(parsed.error().issues.size() == 2);
}

TEST_CASE("Manifests are bounded before they are parsed", "[script][manifest]") {
    CHECK_FALSE(parseManifest(std::string(kMaxManifestBytes + 1, ' ')).has_value());
    CHECK_FALSE(parseManifest(std::string(100, '[') + std::string(100, ']')).has_value()); // too deep
    CHECK_FALSE(parseManifest("{\"id\": \"\xFF\"}").has_value());                         // not UTF-8
    CHECK_FALSE(parseManifest("[]").has_value());
    CHECK_FALSE(parseManifest("").has_value());
    // Members of the wrong type never throw.
    Json m = good();
    m["commands"] = Json::array({{{"id", 5}, {"title", Json::array()}}});
    m["scripts"] = Json::object();
    m["manifestVersion"] = "1";
    CHECK_FALSE(parseManifest(m.dump()).has_value());
}

TEST_CASE("Versions follow semver's order", "[script][manifest]") {
    const auto v = [](const char* text) { return *parseVersion(text); };
    CHECK(compareVersions(v("1.0.0"), v("1.0.1")) < 0);
    CHECK(compareVersions(v("1.10.0"), v("1.9.9")) > 0);
    CHECK(compareVersions(v("1.0.0-beta"), v("1.0.0")) < 0);
    CHECK(compareVersions(v("1.0.0-alpha"), v("1.0.0-beta")) < 0);
    CHECK(compareVersions(v("2.0.0"), v("2.0.0")) == 0);
    CHECK_FALSE(parseVersion("1.0.0-").has_value());
    CHECK_FALSE(parseVersion("1.0.0-a..b").has_value());
    CHECK_FALSE(parseVersion("1000000.0.0").has_value());
    CHECK_FALSE(parseVersion("-1.0.0").has_value());
}

TEST_CASE("An extension folder is read only through its manifest; and its content is hashed", "[script][manifest]") {
    const fs::path dir = scratch("folder");
    const fs::path ext = writeExtension(dir / "beats", good(),
                                        {{"main.js", "wabcut.ui.toast('hi');"}, {"lib/beats.js", "const x = 1;"},
                                         {"notes.txt", "not part of the extension"}});
    auto bundle = loadExtension(ext, ExtensionScope::User, parseVersion("0.1.0"));
    REQUIRE(bundle.has_value());
    REQUIRE(bundle->scripts.size() == 2);
    CHECK(bundle->scripts[0].path == "lib/beats.js"); // evaluation order: scripts, then main
    CHECK(bundle->scripts[1].source == "wabcut.ui.toast('hi');");
    CHECK(bundle->contentHash.size() == 64);
    const std::string hash = bundle->contentHash;

    // Changing any file changes the hash (trust is bound to it); a file the manifest does not name does not.
    writeFile(ext / "notes.txt", "changed");
    auto same = loadExtension(ext, ExtensionScope::User);
    REQUIRE(same.has_value());
    CHECK(same->contentHash == hash);
    writeFile(ext / "lib/beats.js", "const x = 2;");
    auto changed = loadExtension(ext, ExtensionScope::User);
    REQUIRE(changed.has_value());
    CHECK(changed->contentHash != hash);

    // Missing, oversized, non-UTF-8 and NUL-holding files are refused.
    fs::remove(ext / "lib/beats.js");
    auto missing = loadExtension(ext, ExtensionScope::User);
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().message.find("lib/beats.js") != std::string::npos);
    writeFile(ext / "lib/beats.js", std::string(kMaxScriptBytes + 1, ' '));
    CHECK(loadError(ext).find("larger") != std::string::npos);
    writeFile(ext / "lib/beats.js", "const x = '\xC3\x28';");
    CHECK(loadError(ext).find("UTF-8") != std::string::npos);
    writeFile(ext / "lib/beats.js", std::string("const x = 1;\0", 13));
    CHECK(loadError(ext).find("NUL") != std::string::npos);

    // A folder without a manifest, and a manifest that does not parse.
    auto nothing = loadExtension(dir / "nothing", ExtensionScope::User);
    REQUIRE_FALSE(nothing.has_value());
    CHECK(nothing.error().code == ErrorCode::NotFound);
    writeFile(dir / "broken" / "wabcut-extension.json", "{\"id\": 1}");
    auto broken = loadExtension(dir / "broken", ExtensionScope::User);
    REQUIRE_FALSE(broken.has_value());
    CHECK(broken.error().code == ErrorCode::ParseError);
    CHECK(broken.error().message.find("id") != std::string::npos);

    // findExtensions lists every folder with a manifest, the broken ones with their error.
    const auto found = findExtensions(dir, ExtensionScope::User);
    REQUIRE(found.size() == 2);
    CHECK_FALSE(found[0].bundle.has_value()); // beats (its script is still bad)
    CHECK_FALSE(found[1].bundle.has_value()); // broken
    CHECK(projectExtensionsRoot(dir / "film.wabcut") == dir / ".wabcut" / "extensions");
}

TEST_CASE("A script may not be a link that leads out of the extension", "[script][manifest]") {
    const fs::path dir = scratch("links");
    writeFile(dir / "secret.txt", "the user's secret");
    const fs::path ext = writeExtension(dir / "ext", manifest("com.example.links", "Links", {"Read"}), {});
    std::error_code ec;
    fs::create_symlink(dir / "secret.txt", ext / "main.js", ec);
    if (ec) {
        SKIP("cannot create symbolic links here (Windows without developer mode): " << ec.message());
    }
    auto bundle = loadExtension(ext, ExtensionScope::User);
    REQUIRE_FALSE(bundle.has_value());
    CHECK(bundle.error().message.find("link") != std::string::npos);
}

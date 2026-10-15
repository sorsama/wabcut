// MediaLocator: source fingerprints, project-relative paths and relinking.

#include "TimelineFixture.h"

#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
namespace fs = std::filesystem;

namespace {

fs::path testDir(const std::string& name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "model-locator" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void writeBytes(const fs::path& file, std::size_t size, char fill) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    const std::string data(size, fill);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void patchByte(const fs::path& file, std::size_t offset, char value) {
    std::fstream io(file, std::ios::binary | std::ios::in | std::ios::out);
    io.seekp(static_cast<std::streamoff>(offset));
    io.put(value);
}

} // namespace

TEST_CASE("A fingerprint identifies a file by size and sampled content", "[model][locator]") {
    const fs::path dir = testDir("fingerprint");
    const fs::path small = dir / "small.bin";
    writeBytes(small, 1000, 'a');
    const auto fp = fingerprintFile(small);
    REQUIRE(fp);
    CHECK(fp->size == 1000);
    CHECK(fp->mtimeNs > 0);
    CHECK(fp->sampleHash.size() == 16);
    CHECK(fp->isKnown());

    // The same bytes elsewhere: same content, whatever the modification time.
    const fs::path copy = dir / "copy.bin";
    fs::copy_file(small, copy);
    const auto copied = fingerprintFile(copy);
    REQUIRE(copied);
    CHECK(copied->sameContentAs(*fp));

    patchByte(copy, 500, 'b');
    CHECK_FALSE(fingerprintFile(copy)->sameContentAs(*fp));

    // Large files hash only the first and last 64 KiB.
    const fs::path large = dir / "large.bin";
    writeBytes(large, 300 * 1024, 'x');
    const auto before = fingerprintFile(large);
    REQUIRE(before);
    patchByte(large, 150 * 1024, 'y'); // the middle: not sampled
    CHECK(fingerprintFile(large)->sampleHash == before->sampleHash);
    patchByte(large, 300 * 1024 - 1, 'y'); // the last byte: sampled
    CHECK(fingerprintFile(large)->sampleHash != before->sampleHash);

    const auto missing = fingerprintFile(dir / "missing.bin");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::IoError);
    CHECK_FALSE(SourceFingerprint{}.isKnown());
}

TEST_CASE("Media paths are kept relative to the project file", "[model][locator]") {
    const fs::path dir = testDir("relative");
    const fs::path project = dir / "project" / "edit.wabcut";
    CHECK(relativeMediaPath(project, dir / "project" / "footage" / "a.mov") == "footage/a.mov");
    CHECK(relativeMediaPath(project, dir / "media" / u8"日本語.mov") == "../media/" + pathToUtf8(u8"日本語.mov"));
#ifdef _WIN32
    // Another drive has no relative path.
    const std::string otherDrive = dir.root_name() == fs::path("Z:") ? "Y:/clip.mov" : "Z:/clip.mov";
    CHECK(relativeMediaPath(project, otherDrive).empty());
#endif
    CHECK(pathFromUtf8(pathToUtf8(dir / u8"日本語")) == dir / u8"日本語");
}

TEST_CASE("Relinking tries the absolute path, then the project-relative one", "[model][locator]") {
    const fs::path dir = testDir("resolve");
    const fs::path project = dir / "moved" / "edit.wabcut";
    fs::create_directories(dir / "moved" / "footage");
    const fs::path clip = dir / "moved" / "footage" / "a.mov";
    writeBytes(clip, 10, 'z');

    MediaLocator locator;
    locator.path = pathToUtf8(dir / "old-place" / "a.mov"); // the project moved; this is gone
    locator.relativePath = "footage/a.mov";
    const auto found = resolveMediaPath(locator, project);
    REQUIRE(found);
    CHECK(fs::equivalent(*found, clip));

    locator.path = pathToUtf8(clip);
    locator.relativePath = "nowhere/a.mov";
    CHECK(fs::equivalent(*resolveMediaPath(locator, project), clip));

    locator.path = pathToUtf8(dir / "old-place" / "a.mov");
    CHECK_FALSE(resolveMediaPath(locator, project));
}

#ifdef _WIN32
#include <windows.h> // last: its macros reach no code below this test

// Review wave 5, A5-P1-8: every executable embeds cmake/utf8.manifest (wabcut_target_defaults()), so the narrow Win32
// and CRT calls and path::string() use UTF-8. Fails today: the process runs with the ANSI code page (1252, 874, ...),
// GetACP() is not CP_UTF8 and string() throws std::system_error for the emoji.
TEST_CASE("Windows executables run with UTF-8 as their code page", "[model][locator]") {
    CHECK(GetACP() == static_cast<UINT>(CP_UTF8));
    const fs::path name = fs::path(u8"วิดีโอ 😀") / u8"ตอนที่ 1.wabcut";
    std::string narrow;
    REQUIRE_NOTHROW(narrow = name.string());
    CHECK(narrow == pathToUtf8(name));
}
#endif

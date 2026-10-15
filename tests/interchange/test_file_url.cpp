// file:// URLs and paths in the forms editors write them.

#include <wabcut/interchange/FileUrl.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::interchange;

TEST_CASE("interchange: Premiere's Windows URLs decode to drive paths", "[interchange][url]") {
    CHECK(mediaReferenceToPath("file://localhost/C%3a/Projects/Wabcut%20Demo/A001.mov") ==
          "C:/Projects/Wabcut Demo/A001.mov");
    CHECK(mediaReferenceToPath("file://localhost/D%3A/media/sc01.mov") == "D:/media/sc01.mov");
    CHECK(mediaReferenceToPath("file:///C:/a%20b/c.mov") == "C:/a b/c.mov");
    CHECK(mediaReferenceToPath("file:/C:/a.mov") == "C:/a.mov");
    CHECK(mediaReferenceToPath("file://C:/a.mov") == "C:/a.mov");
    CHECK(mediaReferenceToPath("file:///C|/old/style.mov") == "C:/old/style.mov");
    CHECK(mediaReferenceToPath("FILE://LOCALHOST/C%3a/x.mov") == "C:/x.mov");
}

TEST_CASE("interchange: macOS and POSIX URLs decode to absolute paths", "[interchange][url]") {
    CHECK(mediaReferenceToPath("file://localhost/Volumes/RAID/Reel%202/A.mov") ==
          "/Volumes/RAID/Reel 2/A.mov");
    CHECK(mediaReferenceToPath("file:///Users/me/Movies/A.mov") == "/Users/me/Movies/A.mov");
    // UTF-8, percent-encoded.
    CHECK(mediaReferenceToPath("file:///Volumes/RAID/Caf%C3%A9.mov") == "/Volumes/RAID/Café.mov");
    // Query and fragment are not part of the path.
    CHECK(mediaReferenceToPath("file:///a/b.mov?x=1#frag") == "/a/b.mov");
}

TEST_CASE("interchange: UNC shares, plain and relative paths", "[interchange][url]") {
    CHECK(mediaReferenceToPath("file://fileserver/share/Footage/B001.mov") ==
          "//fileserver/share/Footage/B001.mov");
    CHECK(mediaReferenceToPath("C:\\Media\\a.mov") == "C:/Media/a.mov");
    CHECK(mediaReferenceToPath("/Volumes/x.mov") == "/Volumes/x.mov");
    CHECK(mediaReferenceToPath("Media/Interview_B.mov") == "Media/Interview_B.mov");
    // Bare paths are not percent-decoded (a file name may contain '%').
    CHECK(mediaReferenceToPath("100%25 done.mov") == "100%25 done.mov");
}

TEST_CASE("interchange: bad references are errors, not crashes", "[interchange][url]") {
    CHECK_FALSE(mediaReferenceToPath(""));
    CHECK_FALSE(mediaReferenceToPath("   "));
    CHECK(mediaReferenceToPath("http://example.com/a.mov").error().code == ErrorCode::ParseError);
    CHECK_FALSE(mediaReferenceToPath("file:///a%2"));
    CHECK_FALSE(mediaReferenceToPath("file:///a%zz"));
    CHECK_FALSE(mediaReferenceToPath("file:///a%00b"));
    CHECK_FALSE(mediaReferenceToPath("file:///a%FFb")); // invalid UTF-8
    CHECK_FALSE(mediaReferenceToPath("file://localhost"));
}

TEST_CASE("interchange: percent decoding and UTF-8 validation", "[interchange][url]") {
    CHECK(percentDecode("a%20b%2fc") == "a b/c");
    CHECK(percentDecode("%E2%82%AC") == "\xE2\x82\xAC");
    CHECK_FALSE(percentDecode("%"));
    CHECK_FALSE(percentDecode("%4"));
    CHECK(isValidUtf8("plain"));
    CHECK(isValidUtf8("Caf\xC3\xA9"));
    CHECK_FALSE(isValidUtf8("\xC3"));
    CHECK_FALSE(isValidUtf8("\xC0\xAF"));         // overlong
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));     // surrogate
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80")); // above U+10FFFF
}

TEST_CASE("interchange: path helpers accept both separators", "[interchange][url]") {
    CHECK(fileNameOf("C:\\a\\b.mov") == "b.mov");
    CHECK(fileNameOf("/a/b.mov") == "b.mov");
    CHECK(fileNameOf("b.mov") == "b.mov");
    CHECK(isWindowsAbsolutePath("C:/a"));
    CHECK(isWindowsAbsolutePath("c:\\a"));
    CHECK(isWindowsAbsolutePath("//server/share"));
    CHECK_FALSE(isWindowsAbsolutePath("/Volumes/a"));
    CHECK_FALSE(isWindowsAbsolutePath("a/b"));
    CHECK(isAbsoluteOnAnyPlatform("/Volumes/a"));
    CHECK(isAbsoluteOnAnyPlatform("C:/a"));
    CHECK_FALSE(isAbsoluteOnAnyPlatform("Media/a.mov"));
    CHECK(isFileUrl("file:///x"));
    CHECK(isFileUrl("FILE://localhost/x"));
    CHECK_FALSE(isFileUrl("C:/x"));
}

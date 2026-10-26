// P0 round: a recording's chapter markers (media/Chapters.h), as OBS's "Add Chapter Marker" hotkey writes them into an
// MKV: start and end times, titles bounded to 200 bytes of valid UTF-8, files without chapters and files that are not
// media. The clips come from tools/gen_test_media.py (obs_chapters.mkv, obs_chapter_zero.mkv).

#include "TestMedia.h"

#include <wabcut/media/Chapters.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

using namespace wabcut;
using namespace wabcut::media;
using namespace wabcut::media::test;

namespace {

/// The title of obs_chapters.mkv's fourth chapter as read: 216 bytes of Thai cut to 198 (whole characters only).
std::string thaiTitle() {
    const std::string unit = "\xE0\xB9\x84\xE0\xB8\xAE\xE0\xB9\x84\xE0\xB8\xA5\xE0\xB8\x95\xE0\xB9\x8C"; // 18 bytes
    std::string out;
    for (int i = 0; i < 11; ++i)
        out += unit;
    return out;
}

} // namespace

TEST_CASE("Chapters: an OBS-style MKV gives its chapter starts ends and bounded UTF-8 titles", "[media][chapters]") {
    const auto chapters = readChapters(mediaPath("obs_chapters.mkv"));
    REQUIRE(chapters);
    REQUIRE(chapters->size() == 5);
    const auto ms = [](std::int64_t milliseconds) { return Ticks{milliseconds * (kTicksPerSecond / 1000)}; };
    CHECK((*chapters)[0].start == ms(1000));
    CHECK((*chapters)[0].end == ms(2000));
    CHECK((*chapters)[0].title == "Boss fight");
    CHECK((*chapters)[1].start == ms(2000));
    CHECK((*chapters)[1].title.empty()); // untitled
    // An invalid byte reads as U+FFFD; the title stays valid UTF-8.
    CHECK((*chapters)[2].start == ms(2500));
    CHECK((*chapters)[2].title == "Clutch \xEF\xBF\xBD win");
    // 216 bytes of Thai: cut to at most 200 bytes, never inside a character.
    CHECK((*chapters)[3].start == ms(3000));
    CHECK((*chapters)[3].title.size() <= kMaxChapterTitleBytes);
    CHECK((*chapters)[3].title == thaiTitle());
    // A chapter past the end of the file is read as it is (import leaves it out).
    CHECK((*chapters)[4].start == ms(5000));
    CHECK((*chapters)[4].title == "After the end");
    for (const MediaChapter& c : *chapters)
        CHECK(c.end >= c.start);
}

TEST_CASE("Chapters: a lone chapter at 00:00 is read; a file without chapters has none", "[media][chapters]") {
    const auto zero = readChapters(mediaPath("obs_chapter_zero.mkv"));
    REQUIRE(zero);
    REQUIRE(zero->size() == 1);
    CHECK(zero->front().start == Ticks{});
    CHECK(zero->front().end == Ticks::fromSeconds(2));
    CHECK(zero->front().title == "Chapter 1");

    const auto none = readChapters(mediaPath("h264_1080p60_aac.mp4"));
    REQUIRE(none);
    CHECK(none->empty());
}

TEST_CASE("Chapters: a missing file is NotFound and a file that is not media is Unsupported", "[media][chapters]") {
    const auto missing = readChapters(mediaPath("no such recording.mkv"));
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == ErrorCode::NotFound);

    const std::filesystem::path dir = scratchDir("chapters");
    const std::filesystem::path text = dir / "notes.mkv";
    {
        std::ofstream out(text, std::ios::binary);
        out << "these are not the frames you are looking for\n";
    }
    const std::u8string utf8 = text.generic_u8string();
    const auto notMedia = readChapters(std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size()));
    REQUIRE_FALSE(notMedia);
    CHECK(notMedia.error().code == ErrorCode::Unsupported);
}

TEST_CASE("Chapters: a title's leading spaces and control characters never use up its bound", "[media][chapters]") {
    // Fix round 1: 300 leading spaces before a short title kept the title from being read at all.
    CHECK(cleanChapterTitle(std::string(300, ' ') + "Boss fight") == "Boss fight");
    CHECK(cleanChapterTitle(std::string(250, '\n') + std::string(190, 'a')) == std::string(190, 'a'));
    // Inside a title control characters are spaces; the ends are trimmed; only spaces is no title.
    CHECK(cleanChapterTitle("\tClutch\nwin  ") == "Clutch win");
    CHECK(cleanChapterTitle(std::string(500, ' ')).empty());
    CHECK(cleanChapterTitle({}).empty());
    // Long titles are still cut to the bound at a character boundary, after the leading spaces.
    const std::string longTitle = std::string(40, ' ') + std::string(300, 'b');
    CHECK(cleanChapterTitle(longTitle) == std::string(kMaxChapterTitleBytes, 'b'));
}

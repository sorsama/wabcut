// The title commands (C.4): title.add, title.setText, title.setFont through the engine.

#include "CommandHarness.h"

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V2 tgt |..........|
    V1 tgt |Aaaaaaaaaa|
    A1     |Aaaaaaaaaa|
)";

Json at(const char* track, int frame) { return Json{{"track", track}, {"time", tc(frame)}}; }

} // namespace

TEST_CASE("title.add makes a title clip in one undo step", "[command][titles]") {
    CommandHarness h(kPicture);
    const ProjectSnapshot original = h.state();

    const auto dry = h.run(Json{{"command", "title.add"},
                                {"params", Json{{"text", "Chapter One"}, {"at", tc(2)}, {"duration", tc(5)}}},
                                {"dryRun", true}});
    REQUIRE(dry.ok());
    CHECK_FALSE(dry.committed);
    CHECK(dry.changes.addedMedia.size() == 1);
    CHECK(*h.state() == *original);

    // Default track: the first targeted unlocked video track (V1, bottom up).
    const auto added = h.ok("title.add", Json{{"text", "Chapter One"},
                                              {"at", tc(2)},
                                              {"duration", tc(5)},
                                              {"track", "V2"},
                                              {"fontFamily", "Noto Sans"},
                                              {"style", Json{{"size", 120}, {"stroke.width", 4}}}});
    REQUIRE(added.createdIds.size() == 3); // clip, style effect, title media
    const ClipId clip{added.data["clip"].get<std::uint64_t>()};
    const MediaId media{added.data["media"].get<std::uint64_t>()};
    CHECK(added.createdIds[0] == clip.value);
    CHECK(added.createdIds[2] == media.value);
    CHECK(added.data["track"] == "V2");
    CHECK(added.data["start"] == tc(2));
    CHECK(added.data["end"] == tc(7));
    CHECK_SESSION(h, R"(
        V2 |..Ccccc...|
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
    )");
    CHECK(h.session.undoCount() == 1);
    CHECK(h.session.undoLabel() == "New Title");

    const Json details = h.ok("clip.details", Json{{"clip", clip.value}}).data;
    CHECK(details["media"]["generator"] ==
          Json{{"kind", "wabcut.title"}, {"text", "Chapter One"}, {"fontFamily", "Noto Sans"}, {"fontStyle", "Regular"}});
    REQUIRE(details["effects"].size() == 1);

    h.session.undo();
    CHECK(*h.state() == *original);
}

TEST_CASE("title.setText and title.setFont by clip or media", "[command][titles]") {
    CommandHarness h(kPicture);
    const auto added = h.ok("title.add", Json{{"text", "Hello"}, {"at", tc(0)}, {"duration", tc(4)}, {"track", "V2"}});
    const std::uint64_t media = added.data["media"].get<std::uint64_t>();

    const auto text = h.ok("title.setText", Json{{"clip", at("V2", 1)}, {"text", "Hello\nWorld"}});
    CHECK(text.data["media"] == media);
    CHECK(text.data["clipsInSequence"] == 1);
    CHECK(text.changes.modifiedMedia == std::vector<MediaId>{MediaId{media}});
    CHECK(h.state()->findMedia(MediaId{media})->generator->text == "Hello\nWorld");

    h.ok("title.setFont", Json{{"media", media}, {"fontFamily", "Noto Sans Thai"}, {"fontStyle", "Bold"}});
    CHECK(h.state()->findMedia(MediaId{media})->generator->fontFamily == "Noto Sans Thai");
    CHECK(h.state()->findMedia(MediaId{media})->generator->fontStyle == "Bold");
    CHECK(h.session.undoCount() == 3);

    CHECK(h.errorOf("title.setText", Json{{"text", "x"}}) == "InvalidParams");
    CHECK(h.errorOf("title.setFont", Json{{"media", media}}) == "InvalidParams");
    CHECK(h.errorOf("title.setText", Json{{"clip", at("V1", 1)}, {"text", "x"}}) == "EditRejected"); // not a title
    CHECK(h.errorOf("title.add", Json{{"text", "x"}, {"at", tc(0)}, {"track", "A1"}}) == "EditRejected");
    CHECK(h.errorOf("title.add", Json{{"text", "x"}, {"at", tc(0)}, {"style", Json{{"size", 0}}}}) == "InvalidParams");
    CHECK(h.errorOf("title.add", Json{{"text", "x"}, {"at", tc(0)}, {"style", Json{{"bogus", 1}}}}) == "InvalidParams");
}

TEST_CASE("title.add without a targeted video track takes the first free one", "[command][titles]") {
    CommandHarness h(R"(
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
    )");
    const auto onFree = h.ok("title.add", Json{{"text", "x"}, {"at", tc(0)}}); // V1 is busy: a new V2 (C.16)
    CHECK(onFree.data["track"] == "V2");
    CHECK(onFree.data["newTrack"] == true);
    const auto added = h.ok("title.add", Json{{"text", "Lower third"}, {"at", tc(6)}, {"track", "V1"}});
    CHECK(added.data["end"] == tc(6 + 5 * 25)); // default 5 s
}

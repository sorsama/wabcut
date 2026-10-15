// Source Monitor marks stored on media items (MediaItem::markIn / markOut): the edit, saving and loading, and
// clamping marks that a shorter relinked file left past the end.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::size_t count(const std::string& text, const std::string& word) {
    std::size_t n = 0;
    for (std::size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + word.size())) {
        ++n;
    }
    return n;
}

/// `state` with media `id` replaced by a copy changed by `change` (bypassing the edit's checks).
template <class F>
ProjectState withMedia(ProjectState state, MediaId id, F change) {
    for (MediaPtr& m : state.media) {
        if (m->id == id) {
            MediaItem copy = *m;
            change(copy);
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    return state;
}

} // namespace

TEST_CASE("Source marks are set and cleared on the media item only", "[model][edit][sourceMarks]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');

    const auto set = ok(edit::setSourceMarks(tl.state, s, {f(4), f(9)}));
    const MediaItem* item = set.state.findMedia(s);
    REQUIRE(item);
    CHECK(item->markIn == f(4));
    CHECK(item->markOut == f(9));
    CHECK(set.changes.modifiedMedia == std::vector<MediaId>{s});
    CHECK(set.state.sequences == tl.state.sequences); // no sequence is touched
    requireValid(set.state);

    // Only an In: the Out stays unset (the media end).
    const auto inOnly = ok(edit::setSourceMarks(set.state, s, {f(2), std::nullopt}));
    CHECK(inOnly.state.findMedia(s)->markIn == f(2));
    CHECK_FALSE(inOnly.state.findMedia(s)->markOut);

    // Clearing both, and setting what is already there, change nothing else.
    const auto cleared = ok(edit::setSourceMarks(set.state, s, {}));
    CHECK_FALSE(cleared.state.findMedia(s)->markIn);
    CHECK_FALSE(cleared.state.findMedia(s)->markOut);
    const auto same = ok(edit::setSourceMarks(set.state, s, {f(4), f(9)}));
    CHECK(same.changes.empty());
}

TEST_CASE("Source marks outside the media are rejected when set", "[model][edit][sourceMarks]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');
    CHECK(edit::setSourceMarks(tl.state, s, {f(-1), std::nullopt}).error().code == ErrorCode::InvalidArgument);
    CHECK(edit::setSourceMarks(tl.state, s, {f(5), f(5)}).error().code == ErrorCode::InvalidArgument);
    CHECK(edit::setSourceMarks(tl.state, s, {f(6), f(3)}).error().code == ErrorCode::InvalidArgument);
    CHECK(edit::setSourceMarks(tl.state, s, {std::nullopt, f(21)}).error().code == ErrorCode::InvalidArgument);
    CHECK(edit::setSourceMarks(tl.state, s, {f(20), std::nullopt}).error().code == ErrorCode::InvalidArgument);
    CHECK(edit::setSourceMarks(tl.state, MediaId{999999}, {}).error().code == ErrorCode::NotFound);
    // The last frame is a valid In, the end a valid Out.
    CHECK(edit::setSourceMarks(tl.state, s, {f(19), f(20)}));
}

TEST_CASE("Source marks survive saving and are written only when set", "[model][io][sourceMarks]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    // Sequences always write their marks (null when unset); media items write theirs only when set.
    const std::size_t sequenceMarks = count(*plain, "\"markIn\"");

    const ProjectState marked = ok(edit::setSourceMarks(tl.state, s, {f(3), std::nullopt})).state;
    const auto json = io::toJson(marked);
    REQUIRE(json);
    CHECK(count(*json, "\"markIn\"") == sequenceMarks + 1);
    CHECK(count(*json, "\"markOut\"") == count(*plain, "\"markOut\""));

    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == marked);
    CHECK(loaded->findMedia(s)->markIn == f(3));
    CHECK_FALSE(loaded->findMedia(s)->markOut);
    requireValid(*loaded);

    // A file without the keys (older builds) loads with no marks.
    const auto old = io::fromJson(*plain);
    REQUIRE(old);
    CHECK_FALSE(old->findMedia(s)->markIn);
    CHECK_FALSE(old->findMedia(s)->markOut);
}

TEST_CASE("Source marks past a shorter relinked file load and are clamped when used", "[model][io][sourceMarks]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');
    const ProjectState marked = ok(edit::setSourceMarks(tl.state, s, {f(12), f(18)})).state;
    // The file was relinked to a 10-frame version.
    const ProjectState shorter = withMedia(marked, s, [](MediaItem& m) {
        m.info.duration = f(10);
        for (auto& v : m.info.video) {
            v.duration = f(10);
        }
        for (auto& a : m.info.audio) {
            a.duration = f(10);
        }
    });
    requireValid(shorter); // not a load failure
    const auto json = io::toJson(shorter);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(loaded->findMedia(s)->markOut == f(18)); // kept as saved

    bool clamped = false;
    const edit::SourceMarks used = edit::storedSourceMarks(*loaded->findMedia(s), &clamped);
    CHECK(clamped);
    CHECK(used.out == f(10));
    CHECK_FALSE(used.in); // the In (12) no longer comes before the Out

    const ProjectState fine = ok(edit::setSourceMarks(tl.state, s, {f(2), f(8)})).state;
    clamped = true;
    const edit::SourceMarks kept = edit::storedSourceMarks(*fine.findMedia(s), &clamped);
    CHECK_FALSE(clamped);
    CHECK(kept.in == f(2));
    CHECK(kept.out == f(8));
}

TEST_CASE("Validation rejects inverted or negative source marks", "[model][sourceMarks]") {
    Timeline tl(R"(
        V1 tgt |Aaaa|
    )",
                {{'S', 20}});
    const MediaId s = tl.media('S');
    CHECK_FALSE(validate(withMedia(tl.state, s, [](MediaItem& m) { m.markIn = f(-2); })));
    CHECK_FALSE(validate(withMedia(tl.state, s, [](MediaItem& m) { m.markOut = Ticks{}; })));
    CHECK_FALSE(validate(withMedia(tl.state, s, [](MediaItem& m) {
        m.markIn = f(8);
        m.markOut = f(8);
    })));
    CHECK(validate(withMedia(tl.state, s, [](MediaItem& m) {
        m.markIn = f(8);
        m.markOut = f(9);
    })));
}

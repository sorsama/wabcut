// 0.7, stream C, batch C3 through the engine: the nest bin (card 112), one sequence per clip (card 055) and Reload Footage
// (card 143) run as commands in one undo step each, and a sequence they add is stamped with its creation time (card 144).

#include "CommandHarness.h"

#include <wabcut/command/EditingServices.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::test;
using command::Json;

TEST_CASE("bin.setNestTarget files Nest results in the chosen bin in one undo step (112)", "[command][nest][v07]") {
    CommandHarness h(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    const auto made = h.ok("bin.create", Json{{"name", "Nests"}});
    const std::uint64_t bin = made.data["bin"].get<std::uint64_t>();
    const std::size_t before = h.session.undoCount();
    const auto set = h.ok("bin.setNestTarget", Json{{"bin", bin}});
    CHECK(set.data["enabled"] == true);
    CHECK(h.session.undoCount() == before + 1);
    CHECK(edit::nestBinOf(*h.state()).value == bin);

    const ClipId a = h.clip("V1@0");
    const auto nested = h.ok("sequence.nest", Json{{"clips", Json::array({a.value})}, {"name", "Intro"}});
    const SequenceId nestedId{nested.data["nested"].get<std::uint64_t>()};
    CHECK(h.state()->findSequence(nestedId)->bin.value == bin);
    // The nested sequence is stamped by the session: Library's Date created.
    CHECK_FALSE(h.state()->findSequence(nestedId)->createdAt.empty());
    CHECK(h.state()->findSequence(h.tl.seq)->createdAt.empty()); // the old one has none

    // Off again, and the checks: no such bin; a search bin; a repeat of the same is not an undo step.
    h.ok("bin.setNestTarget", Json{{"bin", bin}, {"enabled", false}});
    CHECK_FALSE(edit::nestBinOf(*h.state()).isValid());
    const std::size_t steps = h.session.undoCount();
    h.ok("bin.setNestTarget", Json{{"bin", bin}, {"enabled", false}});
    CHECK(h.session.undoCount() == steps);
    CHECK(h.errorOf("bin.setNestTarget", Json{{"bin", 99999}}) == "NotFound");
    const auto search = h.ok("bin.createSearch", Json{{"name", "All"}, {"search", Json{{"good", true}}}});
    CHECK(h.errorOf("bin.setNestTarget", Json{{"bin", search.data["bin"]}}) == "EditRejected"); // it holds nothing
}

TEST_CASE("sequence.newFromClips makes a sequence per clip or one for all of them in one undo step (055)",
          "[command][newSequenceFromClip][v07]") {
    CommandHarness h("    V1 tgt |Aaaa|\n");
    const std::size_t before = h.session.undoCount();
    const std::size_t sequences = h.state()->sequences.size();

    const auto each = h.ok("sequence.newFromClips", Json{{"media", Json::array({"A", "B", "C"})}});
    CHECK(h.session.undoCount() == before + 1); // one step for all three
    REQUIRE(each.createdIds.size() == 3);
    CHECK(h.state()->sequences.size() == sequences + 3);
    REQUIRE(each.data["sequences"].size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
        const Sequence* s = h.state()->findSequence(SequenceId{each.createdIds[i]});
        REQUIRE(s != nullptr);
        CHECK(s->name == std::string(1, static_cast<char>('A' + i))); // each named after its clip, in the order given
        CHECK(s->videoTracks[0]->clips.size() == 1);
        CHECK_FALSE(s->createdAt.empty());
    }
    REQUIRE(h.session.undo());
    CHECK(h.state()->sequences.size() == sequences);

    const auto together = h.ok("sequence.newFromClips", Json{{"media", Json::array({"A", "B"})}, {"mode", "together"},
                                                             {"name", "Selects"}});
    REQUIRE(together.createdIds.size() == 1);
    const Sequence* all = h.state()->findSequence(SequenceId{together.createdIds.front()});
    REQUIRE(all != nullptr);
    CHECK(all->name == "Selects");
    CHECK(all->videoTracks[0]->clips.size() == 2);
    CHECK(together.data["sequences"][0]["clips"].get<int>() >= 2);

    // Bad input: an unknown clip changes nothing (not even the clips before it).
    const std::size_t steps = h.session.undoCount();
    CHECK(h.errorOf("sequence.newFromClips", Json{{"media", Json::array({"A", 99999})}}) != "");
    CHECK(h.session.undoCount() == steps);
    CHECK(h.errorOf("sequence.newFromClips", Json{{"media", Json::array()}}) == "InvalidParams");
}

namespace {

/// A host that reads `files` (path -> what is on disk now).
command::EnginePolicy hostReading(std::map<std::string, MediaItem> files) {
    command::EnginePolicy policy;
    policy.services = std::make_shared<command::MediaProbeServices>(
        [files = std::move(files)](const std::filesystem::path& file) -> std::expected<MediaItem, Error> {
            const auto it = files.find(file.generic_string());
            if (it == files.end()) {
                return std::unexpected(Error{ErrorCode::NotFound, "no such file"});
            }
            return it->second;
        });
    return policy;
}

MediaItem changed(const MediaItem& original, Ticks duration, const std::string& hash) {
    MediaItem item = original;
    item.info.duration = duration;
    for (VideoStreamInfo& v : item.info.video)
        v.duration = duration;
    for (AudioStreamInfo& a : item.info.audio)
        a.duration = duration;
    item.locator.fingerprint.size = 4242;
    item.locator.fingerprint.sampleHash = hash;
    return item;
}

} // namespace

TEST_CASE("media.reload reads changed files again in one undo step and leaves the rest (143)", "[command][editing][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaa..Bbbb|
    )");
    // A copy of A (another item for the same file) and a title.
    auto copy = edit::duplicateMedia(tl.state, tl.media('A'), "A again");
    REQUIRE(copy);
    tl.state = copy->state;
    {
        // B's file has been read before (a fingerprint is known); A's is changed below.
        auto known = std::make_shared<MediaItem>(*tl.state.findMedia(tl.media('B')));
        known->locator.fingerprint.size = 2048;
        known->locator.fingerprint.sampleHash = "00000000000000b1";
        for (MediaPtr& m : tl.state.media) {
            if (m->id == known->id)
                m = known;
        }
    }
    const MediaItem a = *tl.state.findMedia(tl.media('A'));
    const MediaItem b = *tl.state.findMedia(tl.media('B'));
    const Ticks before = a.info.duration;
    CommandHarness h(std::move(tl), hostReading({{a.locator.path, changed(a, before + Ticks::fromSeconds(10), "00000000000000aa")},
                                                 {b.locator.path, b}}));
    const std::size_t steps = h.session.undoCount();

    const auto out = h.ok("media.reload", Json{{"media", Json::array({"A"})}});
    CHECK(h.session.undoCount() == steps + 1);
    CHECK(out.data["reloaded"] == 2); // the copy shares the file: it is read with it
    CHECK(h.state()->findMedia(h.tl.media('A'))->info.duration == before + Ticks::fromSeconds(10));
    CHECK(h.state()->findMedia(copy->id)->info.duration == before + Ticks::fromSeconds(10));
    CHECK(h.state()->findMedia(h.tl.media('B'))->info.duration == b.info.duration);
    CHECK(out.data["warnings"].empty());
    CHECK_SESSION(h, R"(
        V1 |Aaaa..Bbbb|
    )");

    // B's file reads the same: nothing changes, no undo step.
    const std::size_t after = h.session.undoCount();
    const auto same = h.ok("media.reload", Json{{"media", Json::array({"B"})}});
    CHECK(same.data["reloaded"] == 0);
    CHECK(same.data["unchanged"] == 1);
    CHECK(h.session.undoCount() == after);

    // A file that cannot be read is reported, the others are still read.
    const auto mixed = h.ok("media.reload", Json{{"media", Json::array({"A", "C"})}});
    CHECK(mixed.data["failed"].size() == 1);
    CHECK(mixed.data["failed"][0]["name"] == "C");
    // Nothing readable at all: the error says which file.
    CHECK(h.errorOf("media.reload", Json{{"media", Json::array({"C"})}}) == "NotFound");

    // Without a host that reads media files the command is refused.
    CommandHarness bare("    V1 tgt |Aaaa|\n");
    CHECK(bare.errorOf("media.reload", Json{{"media", Json::array({"A"})}}) == "InvalidRequest");
}

TEST_CASE("media.reload lists clips that now run past a shorter file and keeps them (143)", "[command][editing][v07]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaa|
    )");
    const MediaItem a = *tl.state.findMedia(tl.media('A'));
    CommandHarness h(std::move(tl), hostReading({{a.locator.path, changed(a, f(3), "00000000000000bb")}}));
    const auto out = h.ok("media.reload", Json{{"media", Json::array({"A"})}});
    CHECK(out.data["reloaded"] == 1);
    CHECK_FALSE(out.data["warnings"].empty());
    CHECK_SESSION(h, R"(
        V1 |Aaaaaaaa|
    )");
}

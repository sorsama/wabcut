// A.4: offline audio takes on media items (MediaItem::audioTakes: Enhance Speech) and derived-media provenance
// (MediaItem::derived: separated stems): the operations, validation, change detection and project files (absent until
// set, so other projects stay byte-identical).

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V1 tgt |Aaaaaaaa|
    A1 tgt |Aaaaaaaa|
)";

AudioTake take(int stream, std::uint64_t generation = 1) {
    AudioTake t;
    t.stream = stream;
    t.kind = std::string(kEnhanceSpeechTake);
    t.path = "C:/cache/enhanced/A-a" + std::to_string(stream) + ".wav";
    t.model = "deepfilternet3@c94d91f70911/dsp1";
    t.source.size = 1000;
    t.source.sampleHash = "0123456789abcdef";
    t.generation = generation;
    return t;
}

ProjectState withItem(const ProjectState& state, MediaId id, auto change) {
    ProjectState out = state;
    for (MediaPtr& m : out.media) {
        if (m->id == id) {
            MediaItem copy = *m;
            change(copy);
            m = std::make_shared<const MediaItem>(std::move(copy));
        }
    }
    return out;
}

} // namespace

TEST_CASE("Audio takes: set, replace and forget through the operations", "[model][takes]") {
    Timeline tl(kPicture);
    const MediaId a = tl.media('A');
    auto s = edit::setAudioTake(tl.state, a, take(1));
    REQUIRE(s);
    s = edit::setAudioTake(*s, a, take(0));
    REQUIRE(s);
    const MediaItem& item = *s->findMedia(a);
    REQUIRE(item.audioTakes.size() == 2);
    CHECK(item.audioTakes[0].stream == 0); // kept sorted
    CHECK(item.audioTake(1, kEnhanceSpeechTake) != nullptr);
    CHECK(item.audioTake(1, "other") == nullptr);
    // Rendering again replaces the take of that stream.
    s = edit::setAudioTake(*s, a, take(0, 2));
    REQUIRE(s);
    CHECK(s->findMedia(a)->audioTakes.size() == 2);
    CHECK(s->findMedia(a)->audioTake(0, kEnhanceSpeechTake)->generation == 2);
    CHECK(validate(*s, Validation::Structure));
    // A media change, nothing else.
    const ChangeSet changes = diff(tl.state, *s);
    CHECK(changes.modifiedMedia == std::vector<MediaId>{a});
    // Forgetting.
    auto f = edit::removeAudioTake(*s, a, 0, kEnhanceSpeechTake);
    REQUIRE(f);
    CHECK(f->findMedia(a)->audioTakes.size() == 1);
    auto none = edit::removeAudioTake(*f, a, 0, kEnhanceSpeechTake); // nothing to forget: no change
    REQUIRE(none);
    CHECK(none->findMedia(a) == f->findMedia(a));

    // Refused: no such media, no such stream, no kind or path.
    CHECK_FALSE(edit::setAudioTake(tl.state, MediaId{999}, take(0)));
    CHECK_FALSE(edit::setAudioTake(tl.state, a, take(5)));
    AudioTake noPath = take(0);
    noPath.path.clear();
    CHECK_FALSE(edit::setAudioTake(tl.state, a, noPath));
}

TEST_CASE("Audio takes and derived provenance: validation", "[model][takes][validate]") {
    Timeline tl(kPicture);
    const MediaId a = tl.media('A');
    CHECK(validate(withItem(tl.state, a, [](MediaItem& m) { m.audioTakes = {take(0), take(1)}; }), Validation::Structure));
    CHECK_FALSE(validate(withItem(tl.state, a, [](MediaItem& m) { m.audioTakes = {take(1), take(0)}; }), Validation::Structure));
    CHECK_FALSE(validate(withItem(tl.state, a, [](MediaItem& m) { m.audioTakes = {take(0), take(0)}; }), Validation::Structure));
    CHECK_FALSE(validate(withItem(tl.state, a, [](MediaItem& m) { m.audioTakes = {take(-1)}; }), Validation::Structure));
    CHECK_FALSE(validate(withItem(tl.state, a, [](MediaItem& m) {
                             AudioTake t = take(0);
                             t.kind.clear();
                             m.audioTakes = {t};
                         }),
                         Validation::Structure));
    const DerivedProvenance good{a, "separateStems", "htdemucs@d05c269d0178/sep1", "vocals", "2026-10-12T03:00:00Z"};
    CHECK(validate(withItem(tl.state, a, [&](MediaItem& m) { m.derived = good; }), Validation::Structure));
    CHECK_FALSE(validate(withItem(tl.state, a, [&](MediaItem& m) {
                             m.derived = good;
                             m.derived->operation.clear();
                         }),
                         Validation::Structure));
    CHECK_FALSE(validate(withItem(tl.state, a, [&](MediaItem& m) {
                             m.derived = good;
                             m.derived->createdAt = "yesterday";
                         }),
                         Validation::Structure));
}

TEST_CASE("Audio takes and derived provenance in project files: absent until set, exact round trips",
          "[model][takes][io]") {
    Timeline tl(kPicture);
    const auto original = io::toJson(tl.state);
    REQUIRE(original);
    CHECK(original->find("\"audioTakes\"") == std::string::npos);
    CHECK(original->find("\"derived\"") == std::string::npos);
    const auto reloaded = io::fromJson(*original);
    REQUIRE(reloaded);
    CHECK(*io::toJson(*reloaded) == *original);

    const MediaId a = tl.media('A');
    ProjectState s = withItem(tl.state, a, [&](MediaItem& m) {
        m.audioTakes = {take(0, 7)};
        m.derived = DerivedProvenance{MediaId{42}, "separateStems", "htdemucs@d05c269d0178/sep1", "bass", "2026-10-12T03:00:00Z"};
    });
    const auto saved = io::toJson(s);
    REQUIRE(saved);
    const auto back = io::fromJson(*saved);
    REQUIRE(back);
    CHECK(back->findMedia(a)->audioTakes == s.findMedia(a)->audioTakes);
    CHECK(back->findMedia(a)->derived == s.findMedia(a)->derived);
    CHECK(*io::toJson(*back) == *saved);

    // A malformed take is rejected, not half-loaded.
    std::string bad = *saved;
    const auto at = bad.find("\"stream\": 0", bad.find("\"audioTakes\""));
    REQUIRE(at != std::string::npos);
    bad.replace(at, std::string("\"stream\": 0").size(), "\"stream\": \"zero\"");
    CHECK_FALSE(io::fromJson(bad));
}

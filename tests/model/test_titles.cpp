// Title clips (C.4): the generator media item, the clip and its style effect; text/font edits, validation, IO.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_test_macros.hpp>

#include <string>

using namespace wabcut;
using namespace wabcut::test;

namespace {

constexpr std::string_view kPicture = R"(
    V2     |..........|
    V1 tgt |Aaaaaaaaaa|
    A1     |Aaaaaaaaaa|
)";

const Clip& clipOf(const ProjectState& s, SequenceId seq, ClipId id) { return s.findSequence(seq)->findClip(id)->clip(); }

std::string replaceOnce(std::string text, std::string_view from, std::string_view to) {
    const auto at = text.find(from);
    REQUIRE(at != std::string::npos);
    return text.replace(at, from.size(), to);
}

} // namespace

TEST_CASE("addTitle creates the title item, its clip and its style in one edit", "[model][titles]") {
    Timeline tl(kPicture);
    const auto r = ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(2), f(5), "Title one\nsecond line",
                                     {{"size", ParamValue(120.0)}, {"fill.red", ParamValue(0.5)}},
                                     edit::TitleFont{"Noto Sans Thai", "Bold"}));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |..Ttttt...|
        V1 |Aaaaaaaaaa|
        A1 |Aaaaaaaaaa|
    )");

    // The media item: generated, no file, one unbounded still video stream at the canvas size.
    REQUIRE(r.createdMedia.isValid());
    const MediaItem* title = r.state.findMedia(r.createdMedia);
    REQUIRE(title);
    CHECK(title->isTitle());
    CHECK(title->name == "Title one");
    CHECK(title->generator->text == "Title one\nsecond line");
    CHECK(title->generator->fontFamily == "Noto Sans Thai");
    CHECK(title->generator->fontStyle == "Bold");
    CHECK(title->locator == MediaLocator{});
    REQUIRE(title->info.video.size() == 1);
    CHECK(title->info.audio.empty());
    CHECK(title->info.isStillImage);
    CHECK(title->info.video[0].width == tl.sequence().width);
    CHECK(title->info.video[0].height == tl.sequence().height);
    CHECK(title->info.video[0].frameRate == tl.sequence().frameRate);
    CHECK_FALSE(title->sourceExtent(StreamRef{StreamKind::Video, 0}).has_value());

    // The clip and its style effect.
    REQUIRE(r.placed.size() == 1);
    const Clip& clip = clipOf(r.state, tl.seq, r.placed[0]);
    CHECK(clip.media == r.createdMedia);
    CHECK(clip.start() == f(2));
    CHECK(clip.duration() == f(5));
    REQUIRE(clip.effects().size() == 1);
    const EffectInstance& style = clip.effects()[0];
    CHECK(style.effect == edit::kTitleStyleEffect);
    REQUIRE(r.createdEffects == std::vector<EffectInstanceId>{style.id});
    CHECK(effectParameterAt(style, "size", Ticks{}) == 120.0);
    CHECK(effectParameterAt(style, "fill.red", Ticks{}) == 0.5);
    CHECK(effectParameterAt(style, "alignment", Ticks{}) == 1.0);  // default: centred
    CHECK(effectParameterAt(style, "stroke.width", Ticks{}) == 0.0);

    // One ChangeSet: the new item and the new clip.
    CHECK(r.changes.addedMedia == std::vector<MediaId>{r.createdMedia});
    REQUIRE(r.changes.sequence(tl.seq));
    CHECK(r.changes.sequence(tl.seq)->addedClips == std::vector<ClipId>{r.placed[0]});
    CHECK(diff(tl.state, r.state).addedMedia == r.changes.addedMedia);
}

TEST_CASE("A title overwrites what is under it and can be trimmed to any length", "[model][titles]") {
    Timeline tl(kPicture);
    const auto r = ok(edit::addTitle(tl.state, tl.seq, tl.track("V1"), f(3), f(4), "Over"));
    CHECK_TIMELINE(r.state, tl.seq, R"(
        V2 |..........|
        V1 |AaaOoooA7a|
        A1 |Aaaaaaaaaa|
    )");
    const auto longer = ok(edit::rippleTrim(r.state, tl.seq, r.placed[0], edit::Edge::Tail, f(500))); // unbounded
    CHECK(clipOf(longer.state, tl.seq, r.placed[0]).duration() == f(504));
    CHECK(edit::titleMediaItem("", {}, 10, 10, kRate).name == "Title");
    CHECK(edit::titleMediaItem("\n  \n  Third line is long enough to be cut short at forty", {}, 10, 10, kRate).name ==
          "Third line is long enough to be cut shor\xE2\x80\xA6");
}

TEST_CASE("addTitle refuses what it cannot do", "[model][titles]") {
    Timeline tl(R"(
        V2 lock |..........|
        V1      |Aaaaaaaaaa|
        A1      |Aaaaaaaaaa|
    )");
    const SequenceId seq = tl.seq;
    const auto code = [&](edit::EditOutcome o) { return o ? std::string("ok") : o.error().message; };
    CHECK(code(edit::addTitle(tl.state, seq, tl.track("A1"), f(0), f(5), "x")).find("video track") != std::string::npos);
    CHECK(code(edit::addTitle(tl.state, seq, tl.track("V2"), f(0), f(5), "x")).find("locked") != std::string::npos);
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(0), Ticks{}, "x"));
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(-1), f(5), "x"));
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(0), f(5), "bad \xC3\x28 utf-8"));
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(0), f(5), "x", {}, edit::TitleFont{"", "Bold"}));
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(0), f(5), "x", {{"nope", ParamValue(1.0)}}));
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(0), f(5), "x", {{"size", ParamValue(0.0)}}));
    Curve curve;
    curve.keys = {Keyframe{Ticks{}, 0.0}, Keyframe{f(5), 2.0}};
    CHECK_FALSE(edit::addTitle(tl.state, seq, tl.track("V1"), f(0), f(5), "x", {{"alignment", ParamValue(curve)}}));
    CHECK_FALSE(edit::addTitle(tl.state, seq, TrackId{99999}, f(0), f(5), "x"));
    CHECK(edit::isValidUtf8("สวัสดี \xF0\x9F\x8E\xAC"));
    CHECK_FALSE(edit::isValidUtf8("\xC0\xAF"));         // overlong
    CHECK_FALSE(edit::isValidUtf8("\xED\xA0\x80"));     // surrogate
    CHECK_FALSE(edit::isValidUtf8("\xF4\x90\x80\x80")); // past U+10FFFF
    CHECK_FALSE(edit::isValidUtf8("\xE0\xB8"));         // truncated
}

TEST_CASE("Title style is keyframable like any effect parameter", "[model][titles]") {
    Timeline tl(kPicture);
    const auto r = ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(0), f(10), "Grow"));
    const ClipId clip = r.placed[0];
    const EffectInstanceId style = r.createdEffects[0];
    auto keyed = ok(edit::addKeyframe(r.state, tl.seq, clip, edit::ParamRef::ofEffect(style, "size"), Keyframe{Ticks{}, 50.0}));
    keyed = ok(edit::addKeyframe(keyed.state, tl.seq, clip, edit::ParamRef::ofEffect(style, "size"), Keyframe{f(10), 150.0}));
    const EffectInstance& instance = clipOf(keyed.state, tl.seq, clip).effects()[0];
    CHECK(effectParameterAt(instance, "size", f(5)) == 100.0);
    CHECK_FALSE(edit::addKeyframe(keyed.state, tl.seq, clip, edit::ParamRef::ofEffect(style, "alignment"),
                                  Keyframe{Ticks{}, 0.0}));
}

TEST_CASE("Title text and font change every clip of the title", "[model][titles]") {
    Timeline tl(kPicture);
    const auto added = ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(0), f(4), "Hello"));
    const MediaId title = added.createdMedia;
    // A second clip of the same title (like a copied clip).
    edit::SourceClip copy{title, TimeRange{Ticks{}, f(3)}, {edit::SourcePatch{StreamRef{StreamKind::Video, 0}, tl.track("V2")}}};
    const auto both = ok(edit::overwriteEdit(added.state, tl.seq, copy, f(6)));

    const auto text = ok(edit::setTitleText(both.state, title, "Bonjour\ntout le monde"));
    CHECK(text.state.findMedia(title)->generator->text == "Bonjour\ntout le monde");
    CHECK(text.state.findMedia(title)->name == "Hello"); // a rename is the user's
    CHECK(text.changes.modifiedMedia == std::vector<MediaId>{title});
    CHECK(text.changes.sequences.empty());
    CHECK(text.state.sequences == both.state.sequences); // clips untouched: they show the item

    const auto font = ok(edit::setTitleFont(text.state, title, std::nullopt, std::string("Italic")));
    CHECK(font.state.findMedia(title)->generator->fontFamily == "Arial");
    CHECK(font.state.findMedia(title)->generator->fontStyle == "Italic");

    CHECK(ok(edit::setTitleText(font.state, title, "Bonjour\ntout le monde")).changes.empty()); // no-op
    CHECK_FALSE(edit::setTitleFont(font.state, title, std::string(), std::nullopt));
    CHECK_FALSE(edit::setTitleText(font.state, title, "\xFF"));
    CHECK(edit::setTitleText(font.state, tl.media('A'), "x").error().message.find("not a title") != std::string::npos);
    CHECK(edit::setTitleText(font.state, MediaId{987654}, "x").error().code == ErrorCode::NotFound);

    // A clip of the title on a locked track: the title cannot change.
    edit::TrackStateChange lock;
    lock.locked = true;
    const auto locked = ok(edit::setTrackState(font.state, tl.seq, tl.track("V2"), lock));
    CHECK(edit::setTitleText(locked.state, title, "x").error().message.find("locked track V2") != std::string::npos);
}

TEST_CASE("Generated items have no file: validation rules", "[model][titles][validate]") {
    Timeline tl(kPicture);
    const auto added = ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(0), f(4), "Rules"));
    const MediaId id = added.createdMedia;
    const auto withItem = [&](auto change) {
        ProjectState s = added.state;
        for (MediaPtr& m : s.media) {
            if (m->id == id) {
                auto copy = std::make_shared<MediaItem>(*m);
                change(*copy);
                m = std::move(copy);
            }
        }
        return validate(s);
    };
    CHECK(withItem([](MediaItem&) {}));
    CHECK_FALSE(withItem([](MediaItem& m) { m.locator.path = "C:/titles/rules.png"; }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.locator.relativePath = "rules.png"; }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.locator.offline = true; }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.locator.fingerprint.size = 10; }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.locator.proxy = ProxyMedia{"C:/p.mov", {}}; }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.generator->kind.clear(); }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.generator->fontFamily.clear(); }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.generator->text = "\xC3"; }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.info.audio.push_back(AudioStreamInfo{}); }));
    CHECK_FALSE(withItem([](MediaItem& m) { m.info.isStillImage = false; }));
    // Unknown generator kinds (from a newer Wabcut) are kept; only the no-file rule applies.
    CHECK(withItem([](MediaItem& m) { m.generator->kind = "wabcut.shape"; m.info.audio.push_back(AudioStreamInfo{}); }));
}

TEST_CASE("Titles survive the project file; schema 4 files still load", "[model][titles][io]") {
    Timeline tl(kPicture);
    const auto added = ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(1), f(4), "สวัสดี \"world\"\nline 2",
                                         {{"shadow.opacity", ParamValue(0.8)}}, edit::TitleFont{"Noto Sans Thai", "Bold"}));
    const auto json = io::toJson(added.state);
    REQUIRE(json);
    const std::string current = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);
    CHECK(json->find(current) != std::string::npos);
    CHECK(json->find("\"generator\"") != std::string::npos);
    CHECK(json->find("\"wabcut.title.style\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == added.state);
    CHECK(loaded->findMedia(added.createdMedia)->generator->text == "สวัสดี \"world\"\nline 2");
    CHECK(*io::toJson(*loaded) == *json);

    // A schema 4 file (no generators) migrates.
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    const auto old = io::fromJson(replaceOnce(*plain, current, "\"schemaVersion\": 4"));
    REQUIRE(old);
    CHECK(*old == tl.state);

    // A generator without a kind does not load.
    CHECK_FALSE(io::fromJson(replaceOnce(*json, "\"kind\": \"wabcut.title\"", "\"kind\": \"\"")));
}

TEST_CASE("Title shadow colour: keyframable, black by default", "[model][titles]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find(edit::kTitleStyleEffect);
    REQUIRE(d);
    for (const char* key : {"shadow.red", "shadow.green", "shadow.blue"}) {
        const EffectParamInfo* info = d->param(key);
        REQUIRE(info);
        CHECK(info->defaultValue == 0.0);
        CHECK(info->maxValue == 1.0);
        CHECK(info->animatable);
    }
    CHECK(d->param("shadow.opacity"));
    Timeline tl(R"(
        V2 tgt |..........|
        V1 tgt |Aaaaaaaaaa|
    )");
    const auto r = ok(edit::addTitle(tl.state, tl.seq, tl.track("V2"), f(0), f(10), "Hi",
                                     {{"shadow.red", ParamValue(1.0)}, {"shadow.opacity", ParamValue(0.5)}}));
    const EffectInstance& style = r.state.findSequence(tl.seq)->findClip(r.placed.front())->clip().effects().front();
    CHECK(style.find("shadow.red")->constant() == 1.0);
    CHECK_FALSE(style.find("shadow.green")); // unset: the default
}

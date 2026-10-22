// 0.2: interlaced footage - Field Options (Reverse Field Dominance, Processing Options) and the automatic deinterlacing
// of interlaced footage in a progressive sequence (FieldOptions.h).

#include "TimelineFixture.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/FieldOptions.h>
#include <wabcut/model/FieldOptionsOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

using namespace wabcut;
using namespace wabcut::test;

namespace {

/// Interlaced 29.97 fps footage (as DV or broadcast HD comes) with one audio stream.
MediaId addInterlaced(ProjectState& state, FieldOrder order, Rate rate = kRate29_97) {
    MediaItem item;
    item.name = "Archive";
    item.locator.path = "D:/archive/tape01.mpg";
    item.info.duration = Ticks::fromSeconds(60);
    VideoStreamInfo v;
    v.index = 0;
    v.width = 720;
    v.height = 480;
    v.frameRate = rate;
    v.duration = Ticks::fromSeconds(60);
    v.fieldOrder = order;
    item.info.video.push_back(v);
    AudioStreamInfo a;
    a.index = 1;
    a.channels = 2;
    a.duration = Ticks::fromSeconds(60);
    item.info.audio.push_back(a);
    auto added = edit::addMedia(state, std::move(item));
    REQUIRE(added);
    state = std::move(added->state);
    return added->id;
}

Clip videoClip(ClipFieldOptions options = {}, Speed speed = {}) {
    Clip c;
    c.stream = {StreamKind::Video, 0};
    c.speed = speed;
    ClipContent content;
    content.fieldOptions = options;
    c.setContent(std::move(content));
    return c;
}

Sequence sequenceAt(Rate rate, FieldOrder fields = FieldOrder::Progressive) {
    Sequence s;
    s.frameRate = rate;
    s.fields = fields;
    return s;
}

MediaItem interlacedItem(FieldOrder order) {
    MediaItem item;
    VideoStreamInfo v;
    v.width = 720;
    v.height = 480;
    v.frameRate = kRate29_97;
    v.fieldOrder = order;
    item.info.video.push_back(v);
    return item;
}

} // namespace

TEST_CASE("Interlaced footage in a progressive sequence is deinterlaced from its first field", "[model][fields]") {
    const MediaItem upper = interlacedItem(FieldOrder::UpperFirst);
    const MediaItem lower = interlacedItem(FieldOrder::LowerFirst);
    const MediaItem progressive = interlacedItem(FieldOrder::Progressive);
    const Sequence p2997 = sequenceAt(kRate29_97);
    // One picture per frame: the dominant field throughout.
    CHECK(fieldRenderingOf(videoClip(), &upper, 0, p2997) == FieldRendering{FieldRender::KeepUpper, FieldRender::KeepUpper});
    CHECK(fieldRenderingOf(videoClip(), &lower, 0, p2997) == FieldRendering{FieldRender::KeepLower, FieldRender::KeepLower});
    CHECK(fieldRenderingOf(videoClip(), &progressive, 0, p2997).isProgressive());
    // Reverse Field Dominance swaps the order.
    ClipFieldOptions reverse;
    reverse.reverseFieldDominance = true;
    CHECK(fieldRenderingOf(videoClip(reverse), &upper, 0, p2997).first == FieldRender::KeepLower);
    // An interlaced sequence keeps both fields in its frames, unless Always Deinterlace.
    const Sequence i2997 = sequenceAt(kRate29_97, FieldOrder::UpperFirst);
    CHECK(fieldRenderingOf(videoClip(), &upper, 0, i2997).isProgressive());
    ClipFieldOptions always;
    always.processing = FieldProcessing::AlwaysDeinterlace;
    CHECK(fieldRenderingOf(videoClip(always), &upper, 0, i2997) ==
          FieldRendering{FieldRender::KeepUpper, FieldRender::KeepUpper});
    // Always Deinterlace on footage without fields keeps the upper field (lower with Reverse Field Dominance).
    CHECK(fieldRenderingOf(videoClip(always), &progressive, 0, p2997).first == FieldRender::KeepUpper);
    ClipFieldOptions alwaysReversed = always;
    alwaysReversed.reverseFieldDominance = true;
    CHECK(fieldRenderingOf(videoClip(alwaysReversed), &progressive, 0, p2997).first == FieldRender::KeepLower);
    // Flicker Removal blends the lines of any footage.
    ClipFieldOptions flicker;
    flicker.processing = FieldProcessing::FlickerRemoval;
    CHECK(fieldRenderingOf(videoClip(flicker), &progressive, 0, i2997) == FieldRendering{FieldRender::Blend, FieldRender::Blend});
    // Interlace Consecutive Frames is kept but changes nothing in a progressive render.
    ClipFieldOptions consecutive;
    consecutive.processing = FieldProcessing::InterlaceConsecutiveFrames;
    CHECK(fieldRenderingOf(videoClip(consecutive), &progressive, 0, i2997).isProgressive());
    // A nested canvas is progressive: only Always Deinterlace and Flicker Removal change it.
    CHECK(fieldRenderingOf(videoClip(), nullptr, 0, p2997).isProgressive());
    CHECK(fieldRenderingOf(videoClip(always), nullptr, 0, p2997).first == FieldRender::KeepUpper);
}

TEST_CASE("At the field rate both fields are shown, each for half a frame", "[model][fields]") {
    const MediaItem upper = interlacedItem(FieldOrder::UpperFirst);
    // 29.97i in a 59.94p sequence: the upper field first, then the lower one.
    const FieldRendering bob = fieldRenderingOf(videoClip(), &upper, 0, sequenceAt(kRate59_94));
    REQUIRE(bob == FieldRendering{FieldRender::KeepUpper, FieldRender::KeepLower});
    const Ticks frame = framesToTicks(1, kRate29_97);
    const Ticks pts = framesToTicks(10, kRate29_97);
    CHECK(fieldRenderAt(bob, pts, pts, frame) == FieldRender::KeepUpper);
    CHECK(fieldRenderAt(bob, pts + framesToTicks(1, kRate59_94), pts, frame) == FieldRender::KeepLower);
    CHECK(fieldRenderAt(bob, pts - Ticks{1}, pts, frame) == FieldRender::KeepUpper); // outside the frame: the first
    // Slowed to 50 % in a 29.97p sequence: the source advances half a frame per frame, so both fields are seen.
    CHECK(fieldRenderingOf(videoClip({}, Speed{1, 2}), &upper, 0, sequenceAt(kRate29_97)).second == FieldRender::KeepLower);
    // At normal speed a 29.97p sequence sees one field; a frame hold too.
    CHECK(fieldRenderingOf(videoClip({}, Speed{1, 1}), &upper, 0, sequenceAt(kRate29_97)).second == FieldRender::KeepUpper);
    CHECK(fieldRenderingOf(videoClip({}, Speed{0, 1}), &upper, 0, sequenceAt(kRate59_94)).second == FieldRender::KeepUpper);
    // Interpret Footage's field order wins over the file's.
    MediaItem interpreted = interlacedItem(FieldOrder::Progressive);
    interpreted.interpretation = MediaInterpretation{};
    interpreted.interpretation->fieldOrder = FieldOrder::LowerFirst;
    CHECK(effectiveFieldOrder(interpreted, 0) == FieldOrder::LowerFirst);
    CHECK(fieldRenderingOf(videoClip(), &interpreted, 0, sequenceAt(kRate59_94)) ==
          FieldRendering{FieldRender::KeepLower, FieldRender::KeepUpper});
}

TEST_CASE("Field Options are one edit on the video clips and are saved", "[model][fields]") {
    Timeline tl(R"(
        V1 tgt |..........|
        A1 tgt |..........|
    )");
    ProjectState state = tl.state;
    const MediaId archive = addInterlaced(state, FieldOrder::LowerFirst);
    edit::SourceClip source;
    source.media = archive;
    source.sourceRange = {Ticks{}, Ticks::fromSeconds(1)};
    source.patches = edit::defaultSourcePatches(*state.findSequence(tl.seq), *state.findMedia(archive));
    state = ok(edit::overwriteEdit(state, tl.seq, source, Ticks{})).state;
    const Sequence& seq = *state.findSequence(tl.seq);
    const ClipId video = seq.videoTracks[0]->clips[0].id;
    const ClipId audio = seq.audioTracks[0]->clips[0].id;

    ClipFieldOptions options;
    options.reverseFieldDominance = true;
    options.processing = FieldProcessing::FlickerRemoval;
    const auto changed = ok(edit::setFieldOptions(state, tl.seq, std::vector<ClipId>{video, audio}, options));
    REQUIRE(changed.changes.sequences.size() == 1);
    const Sequence& after = *changed.state.findSequence(tl.seq);
    CHECK(after.videoTracks[0]->clips[0].content().fieldOptions == options);
    CHECK(after.audioTracks[0]->clips[0].content().fieldOptions.isDefault()); // audio is skipped
    CHECK(ok(edit::setFieldOptions(changed.state, tl.seq, std::vector<ClipId>{video}, options)).changes.sequences.empty());
    CHECK_FALSE(edit::setFieldOptions(state, tl.seq, std::vector<ClipId>{audio}, options)); // no video clip

    // Saved and loaded: the clip's options and the stream's field order (schema 18).
    const auto text = io::toJson(changed.state);
    REQUIRE(text);
    CHECK(text->find("\"fieldOptions\"") != std::string::npos);
    const auto loaded = io::fromJson(*text);
    REQUIRE(loaded);
    CHECK(loaded->findSequence(tl.seq)->videoTracks[0]->clips[0].content().fieldOptions == options);
    CHECK(loaded->findMedia(archive)->info.video[0].fieldOrder == FieldOrder::LowerFirst);
    // A progressive clip writes neither.
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("fieldOptions") == std::string::npos);
    CHECK(plain->find("\"fieldOrder\"") == std::string::npos);
}

TEST_CASE("New Sequence From Clip takes interlaced footage's field order", "[model][fields]") {
    ProjectState state;
    const MediaId archive = addInterlaced(state, FieldOrder::UpperFirst);
    const auto made = edit::newSequenceFromClip(state, archive, "Tape 1");
    REQUIRE(made);
    CHECK(made->state.findSequence(made->id)->fields == FieldOrder::UpperFirst);
}

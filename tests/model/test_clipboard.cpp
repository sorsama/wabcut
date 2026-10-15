// C.11: the clipboard (Clipboard.h) - copy, JSON round trip and paste carrying speed, reverse, freeze frames, time
// remapping, effects and keyframes exactly; links, transitions and markers; Premiere's insert / overwrite rules and
// targeted tracks; cross-sequence and cross-project paste with media matched or added; Paste / Remove Attributes.

#include "TimelineFixture.h"

#include <wabcut/model/Clipboard.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Resolve.h>

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

std::vector<ClipId> allClips(const ProjectState& state, SequenceId seq) {
    std::vector<ClipId> out;
    state.findSequence(seq)->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            out.push_back(c.id);
        }
    });
    return out;
}

/// "V1@12" -> the clip there (the fixture's clipAt, by a label and a frame).
const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    const ClipId id = clipAt(state, seq, ref);
    return state.findSequence(seq)->findClip(id)->clip();
}

/// Everything a paste must carry, compared field by field (ids, start and link group are the paste's own).
void requireSameClip(const Clip& pasted, const Clip& original) {
    INFO("original clip " << original.id.value << ", pasted " << pasted.id.value);
    CHECK(pasted.media == original.media);
    CHECK(pasted.stream == original.stream);
    CHECK(pasted.duration() == original.duration());
    CHECK(pasted.sourceIn == original.sourceIn);
    CHECK(pasted.speed == original.speed);
    CHECK(pasted.reversed == original.reversed);
    CHECK(pasted.isFreeze() == original.isFreeze());
    CHECK(pasted.enabled == original.enabled);
    CHECK(pasted.colorIndex == original.colorIndex);
    CHECK(pasted.name() == original.name());
    CHECK(pasted.properties() == original.properties());
    CHECK(pasted.isRemapped() == original.isRemapped());
    if (original.isRemapped()) {
        CHECK(*pasted.timeRemap() == *original.timeRemap());
    }
    REQUIRE(pasted.effects().size() == original.effects().size());
    for (std::size_t i = 0; i < original.effects().size(); ++i) {
        EffectInstance a = pasted.effects()[i];
        EffectInstance b = original.effects()[i];
        CHECK(a.id != b.id); // a new instance
        a.id = b.id;
        CHECK(a == b);
    }
}

} // namespace

TEST_CASE("Clipboard: constant speed, reverse, freeze frames, time remap, effects and keyframes paste exactly",
          "[model][clipboard]") {
    // The app's paste placed clips at 100 % and lost these (UI engineer, C.11): the model clipboard carries them.
    Timeline tl(R"(
        V2     |..Dddddd............................|
        V1 tgt |AaaaaaaaaaB5bbbbbbbbCccccccccc......|
        A1 tgt |AaaaaaaaaaB5bbbbbbbbCccccccccc......|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId b = tl.clip("V1@10");
    const ClipId c = tl.clip("V1@20");
    edit::SpeedChange half;
    half.speed = {1, 2};
    half.keepDuration = true;
    s = ok(edit::setSpeed(s, tl.seq, std::span(&b, 1), half)).state;
    edit::SpeedChange reverse;
    reverse.speed = {1, 1};
    reverse.reversed = true;
    reverse.keepDuration = true;
    s = ok(edit::setSpeed(s, tl.seq, std::span(&c, 1), reverse)).state;
    s = ok(edit::frameHold(s, tl.seq, tl.clip("V1@0"), f(5))).state;
    s = ok(edit::setTimeRemapEnabled(s, tl.seq, tl.clip("V2@2"), true)).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&b, 1), "wabcut.blur.gaussian")).state;
    const Clip& bClip = s.findSequence(tl.seq)->findClip(b)->clip();
    s = ok(edit::addKeyframe(s, tl.seq, b, edit::ParamRef::of(ParamId::Opacity), Keyframe{bClip.sourceIn + f(1), 0.25})).state;
    requireValid(s);

    const std::vector<ClipId> ids = allClips(s, tl.seq);
    auto copied = copyClips(s, tl.seq, ids);
    REQUIRE(copied);
    CHECK(copied->duration == f(30));
    CHECK(copied->videoBase == 0);
    CHECK(copied->audioBase == 0);
    CHECK(copied->clipCount() == ids.size());

    // The JSON round trip is exact.
    const auto text = clipboardToJson(*copied);
    REQUIRE(text);
    const auto back = clipboardFromJson(*text);
    REQUIRE(back);
    CHECK(back->content == copied->content);
    CHECK(back->duration == copied->duration);

    // Pasted after the material, the clips are the same clips.
    const auto pasted = pasteClips(s, tl.seq, *back, f(40));
    REQUIRE(pasted);
    requireValid(pasted->edit.state);
    CHECK(pasted->report.pasted.size() == ids.size());
    CHECK(pasted->report.mediaAdded.empty());
    CHECK(pasted->edit.playhead == f(70));
    for (const ClipId id : ids) {
        const auto loc = s.findSequence(tl.seq)->findClip(id);
        const Clip& original = loc->clip();
        const std::string label = loc->track->kind == TrackKind::Video ? "V" : "A";
        const auto pos = s.findSequence(tl.seq)->trackPosition(loc->track->id);
        const std::string ref = label + std::to_string(pos->index + 1) + "@" +
                                std::to_string((original.start() + f(40)).value / kRate.ticksPerUnit());
        requireSameClip(clipIn(pasted->edit.state, tl.seq, ref), original);
    }
    // The pasted V1/A1 pairs are linked to each other, not to the originals.
    const Clip& pv = clipIn(pasted->edit.state, tl.seq, "V1@60");
    const Clip& pa = clipIn(pasted->edit.state, tl.seq, "A1@60");
    CHECK(pv.linkGroup.isValid());
    CHECK(pv.linkGroup == pa.linkGroup);
    CHECK(pv.linkGroup != clipIn(s, tl.seq, "V1@20").linkGroup);
}

TEST_CASE("Clipboard: links, transitions and markers among the copied clips come along", "[model][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaB5bbbbbbbb......................|
        A1 tgt |AaaaaaaaaaB5bbbbbbbb......................|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    edit::TransitionSpec spec;
    spec.outgoing = tl.clip("V1@0");
    spec.incoming = tl.clip("V1@10");
    spec.before = f(2);
    spec.after = f(2);
    s = ok(edit::addTransition(s, tl.seq, tl.track("V1"), spec)).state;
    s = ok(edit::addMarker(s, tl.seq, Marker{f(12), Ticks{}, "Logo"})).state;
    s = ok(edit::addMarker(s, tl.seq, Marker{f(25), Ticks{}, "Outside"})).state;
    const std::vector<ClipId> ids{tl.clip("V1@0"), tl.clip("V1@10")}; // their audio partners follow
    auto copied = copyClips(s, tl.seq, ids);
    REQUIRE(copied);
    CHECK(copied->clipCount() == 4);
    const auto pasted = pasteClips(s, tl.seq, *copied, f(22));
    REQUIRE(pasted);
    requireValid(pasted->edit.state);
    CHECK_TIMELINE(pasted->edit.state, tl.seq, R"(
        V1 |AaaaaaaaaaB5bbbbbbbb..AaaaaaaaaaB5bbbbbbbb|
        A1 |AaaaaaaaaaB5bbbbbbbb..AaaaaaaaaaB5bbbbbbbb|
    )");
    CHECK(links(pasted->edit.state, tl.seq) == "V1@0+A1@0 V1@10+A1@10 V1@22+A1@22 V1@32+A1@32");
    const Track& v1 = *pasted->edit.state.findSequence(tl.seq)->findTrack(tl.track("V1"));
    REQUIRE(v1.transitions.size() == 2);
    CHECK(v1.transitions[1].outgoing == clipAt(pasted->edit.state, tl.seq, "V1@22"));
    CHECK(v1.transitions[1].incoming == clipAt(pasted->edit.state, tl.seq, "V1@32"));
    const auto& markers = pasted->edit.state.findSequence(tl.seq)->markers;
    REQUIRE(markers.size() == 3); // "Logo" pasted at 22 + 12; "Outside" was not in the copied range
    CHECK(markers[2].start == f(34));
    CHECK(markers[2].name == "Logo");
}

TEST_CASE("Clipboard: overwrite replaces, insert opens room on the targets and sync-locked tracks", "[model][clipboard]") {
    Timeline tl(R"(
        V2     |....................|
        V1 tgt |AaaaBbbbCccc........|
        A1 tgt |AaaaBbbbCccc........|
        A2     |Dddddddddddd........|
    )");
    tl.linkMatching();
    const ClipId b = tl.clip("V1@4");
    auto copied = copyClips(tl.state, tl.seq, std::span(&b, 1));
    REQUIRE(copied);
    // Ctrl+V: overwrite at the playhead on the targeted tracks.
    const auto over = pasteClips(tl.state, tl.seq, *copied, f(0));
    REQUIRE(over);
    CHECK_TIMELINE(over->edit.state, tl.seq, R"(
        V2 |....................|
        V1 |BbbbBbbbCccc........|
        A1 |BbbbBbbbCccc........|
        A2 |Dddddddddddd........|
    )");
    // Ctrl+Shift+V: insert; A2 is sync-locked, so it opens too (D moves later).
    PasteOptions insert;
    insert.mode = PasteMode::Insert;
    const auto ins = pasteClips(tl.state, tl.seq, *copied, f(0), insert);
    REQUIRE(ins);
    CHECK_TIMELINE(ins->edit.state, tl.seq, R"(
        V2 |....................|
        V1 |BbbbAaaaBbbbCccc....|
        A1 |BbbbAaaaBbbbCccc....|
        A2 |....Dddddddddddd....|
    )");
    // Targeting V2 and A2 instead (with track offsets kept): the clips land there.
    PasteOptions targeted;
    targeted.videoTrack = tl.track("V2");
    targeted.audioTrack = tl.track("A2");
    const auto onV2 = pasteClips(tl.state, tl.seq, *copied, f(12), targeted);
    REQUIRE(onV2);
    CHECK_TIMELINE(onV2->edit.state, tl.seq, R"(
        V2 |............Bbbb....|
        V1 |AaaaBbbbCccc........|
        A1 |AaaaBbbbCccc........|
        A2 |DdddddddddddBbbb....|
    )");
    // Two video tracks copied, pasted from V2: a third video track is added.
    Timeline two(R"(
        V2     |..Eeee..............|
        V1 tgt |AaaaBbbbCccc........|
    )");
    const std::vector<ClipId> both{two.clip("V1@0"), two.clip("V2@2")};
    auto stacked = copyClips(two.state, two.seq, both);
    REQUIRE(stacked);
    PasteOptions fromV2;
    fromV2.videoTrack = two.track("V2");
    const auto added = pasteClips(two.state, two.seq, *stacked, f(14), fromV2);
    REQUIRE(added);
    CHECK(added->report.tracksAdded.size() == 1);
    CHECK_TIMELINE(added->edit.state, two.seq, R"(
        V3 |................Eeee|
        V2 |..Eeee........Aaaa..|
        V1 |AaaaBbbbCccc........|
    )");
    // A locked destination refuses; nothing on the clipboard refuses too.
    Timeline locked(R"(
        V1 tgt lock |AaaaBbbb....|
    )");
    CHECK_FALSE(pasteClips(locked.state, locked.seq, *stacked, f(8)));
    CHECK_FALSE(pasteClips(tl.state, tl.seq, *copied, Ticks{-1}));
}

TEST_CASE("Clipboard: paste into another sequence and another project finds or adds the media", "[model][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )");
    tl.linkMatching();
    ProjectState source = tl.state;
    // Media A has a known fingerprint.
    for (MediaPtr& m : source.media) {
        if (m->name == "A") {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->locator.fingerprint = SourceFingerprint{123456, 0, "00112233445566aa"};
            m = std::move(copy);
        }
    }
    const std::vector<ClipId> ids{tl.clip("V1@0"), tl.clip("V1@4")};
    auto copied = copyClips(source, tl.seq, ids);
    REQUIRE(copied);
    CHECK(copied->content.media.size() == 2); // A and B travel with the clips

    // Another sequence of the same project: the project's own media.
    edit::SequenceSettings settings;
    settings.frameRate = kRate;
    settings.videoTracks = 1;
    settings.audioTracks = 1;
    auto second = edit::addSequence(source, settings);
    REQUIRE(second);
    const auto same = pasteClips(second->state, second->id, *copied, f(0));
    REQUIRE(same);
    CHECK(same->report.mediaAdded.empty());
    CHECK(clipIn(same->edit.state, second->id, "V1@0").media == tl.media('A'));

    // Another project: A is there under another name, path and id (matched by fingerprint); B is not (added).
    Timeline other(R"(
        V1 tgt |Zzzz........|
        A1 tgt |Zzzz........|
    )");
    ProjectState target = other.state;
    // That project's own A (same id) is another file.
    for (MediaPtr& m : target.media) {
        if (m->name == "A") {
            auto elsewhere = std::make_shared<MediaItem>(*m);
            elsewhere->locator.path = "D:/other/A.mov";
            m = std::move(elsewhere);
        }
    }
    MediaItem moved = *target.findMedia(other.media('Z'));
    moved.name = "A (moved)";
    moved.locator.path = "E:/archive/A.mov";
    moved.locator.fingerprint = SourceFingerprint{123456, 99, "00112233445566aa"};
    auto withA = edit::addMedia(target, moved);
    REQUIRE(withA);
    target = withA->state;
    // B is not in the other project at all.
    auto removedB = edit::removeMedia(target, other.media('B'));
    REQUIRE(removedB);
    target = *removedB;
    const auto cross = pasteClips(target, other.seq, *copied, f(4));
    REQUIRE(cross);
    requireValid(cross->edit.state);
    CHECK(clipIn(cross->edit.state, other.seq, "V1@4").media == withA->id);
    CHECK(cross->report.mediaAdded.size() == 1);
    const MediaItem* addedB = cross->edit.state.findMedia(clipIn(cross->edit.state, other.seq, "V1@8").media);
    REQUIRE(addedB);
    CHECK(addedB->name == "B");
    CHECK(addedB->locator.path == "D:/footage/B.mov");
    REQUIRE(cross->report.notes.size() == 2);
    CHECK(cross->report.notes[0].find("matched by fingerprint") != std::string::npos);
    CHECK(cross->report.notes[1].find("added to the project") != std::string::npos);
}

TEST_CASE("Clipboard: redactions come with their media - after it was removed here, and into another project (review "
          "wave 5 E5-P2-4)",
          "[model][clipboard][redaction]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )");
    tl.linkMatching();
    Redaction r;
    r.media = tl.media('B');
    r.name = "Plate";
    r.source = {f(0), f(10)};
    r.audio = RedactionAudio::Bleep;
    r.region.shape = MaskShape::Rectangle;
    const auto plate = edit::addRedaction(tl.state, r);
    REQUIRE(plate);
    const ProjectState s = plate->state;
    const ClipId b = tl.clip("V1@4");
    auto copied = copyClips(s, tl.seq, std::span(&b, 1));
    REQUIRE(copied);
    REQUIRE(copied->content.redactions.size() == 1); // it travels in the document (and its JSON)
    const auto text = clipboardToJson(*copied);
    REQUIRE(text);
    const auto back = clipboardFromJson(*text);
    REQUIRE(back);
    CHECK(back->content.redactions.size() == 1);
    const auto pastedAt = [](const ProjectState& state, SequenceId seq, StreamKind kind, std::int64_t frame) {
        const auto leaves = resolveSequenceLeaves(state, seq, kind, f(frame));
        REQUIRE(leaves.size() == 1);
        return leaves.front().redactions.size();
    };

    // The same project, the same item: nothing is added twice.
    const auto again = pasteClips(s, tl.seq, *back, f(8));
    REQUIRE(again);
    CHECK(again->edit.state.redactions.size() == 1);
    CHECK(pastedAt(again->edit.state, tl.seq, StreamKind::Video, 9) == 1);

    // Cut, then "remove unused media", then paste: B comes back as a new item, still redacted (it played in clear).
    ProjectState cut = ok(edit::deleteClips(s, tl.seq, std::span(&b, 1))).state;
    auto removed = edit::removeMedia(cut, tl.media('B'));
    REQUIRE(removed);
    const auto readded = pasteClips(*removed, tl.seq, *back, f(4));
    REQUIRE(readded);
    requireValid(readded->edit.state);
    REQUIRE(readded->report.mediaAdded.size() == 1);
    CHECK(pastedAt(readded->edit.state, tl.seq, StreamKind::Video, 5) == 1);
    CHECK(pastedAt(readded->edit.state, tl.seq, StreamKind::Audio, 5) == 1);

    // Another project: the media is added with its redaction.
    Timeline other(R"(
        V1 tgt |............|
        A1 tgt |............|
    )");
    auto noB = edit::removeMedia(other.state, other.media('B'));
    REQUIRE(noB);
    const auto cross = pasteClips(*noB, other.seq, *back, f(0));
    REQUIRE(cross);
    requireValid(cross->edit.state);
    CHECK(pastedAt(cross->edit.state, other.seq, StreamKind::Video, 1) == 1);
}

TEST_CASE("Clipboard: a document from elsewhere is bounded and checked", "[model][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
    )");
    const ClipId a = tl.clip("V1@0");
    auto copied = copyClips(tl.state, tl.seq, std::span(&a, 1));
    REQUIRE(copied);
    const std::string text = *clipboardToJson(*copied);
    CHECK(clipboardFromJson(text));
    CHECK_FALSE(clipboardFromJson("{}"));
    CHECK_FALSE(clipboardFromJson("[1, 2, 3]"));
    CHECK_FALSE(clipboardFromJson(std::string(100'000, '[') + std::string(100'000, ']')));
    std::string newer = text;
    newer.replace(newer.find("\"version\":1"), 11, "\"version\":9");
    const auto refused = clipboardFromJson(newer);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == ErrorCode::VersionMismatch);
    std::string wrongLength = text;
    const std::string duration = "\"duration\":" + std::to_string(f(4).value);
    REQUIRE(wrongLength.find(duration) != std::string::npos);
    wrongLength.replace(wrongLength.find(duration), duration.size(), "\"duration\":" + std::to_string(f(40).value));
    CHECK_FALSE(clipboardFromJson(wrongLength));
}

TEST_CASE("Paste Attributes: motion, opacity, effects and time remapping, keyframe times kept or scaled",
          "[model][clipboard][attributes]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbbbbbbbbbbb|
    )");
    tl.linkMatching();
    ProjectState s = tl.state;
    const ClipId a = tl.clip("V1@0");
    const ClipId b = tl.clip("V1@10");
    // A: Scale keyframes at its 2nd and 8th frame, opacity 0.5, a blur.
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(2), 0.5})).state;
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(8), 1.5})).state;
    s = ok(edit::setParameter(s, tl.seq, a, edit::ParamRef::of(ParamId::Opacity), ParamValue(0.5))).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.blur.gaussian")).state;
    const Clip source = s.findSequence(tl.seq)->findClip(a)->clip();

    AttributeSet motion;
    motion.motion = true;
    motion.opacity = true;
    motion.allEffects = true;
    const std::vector<ClipId> targets{b, tl.clip("A1@10")};
    const auto pasted = pasteAttributes(s, tl.seq, source, targets, motion);
    REQUIRE(pasted);
    requireValid(pasted->edit.state);
    const Clip& bAfter = clipIn(pasted->edit.state, tl.seq, "V1@10");
    // Same times from the clip's start: B's curve starts at its source In (0), A's at 0 too.
    const Curve& scale = std::get<Curve>(bAfter.properties()[ParamId::Scale].data);
    REQUIRE(scale.keys.size() == 2);
    CHECK(scale.keys[0].time == f(2));
    CHECK(scale.keys[1].time == f(8));
    CHECK(bAfter.properties()[ParamId::Opacity] == ParamValue(0.5));
    REQUIRE(bAfter.effects().size() == 1);
    CHECK(bAfter.effects()[0].effect == "wabcut.blur.gaussian");
    CHECK(bAfter.effects()[0].id != source.effects()[0].id);
    REQUIRE(pasted->changes.size() == 2);
    CHECK(pasted->changes[0].applied == std::vector<std::string>{"Motion", "Opacity", "Gaussian Blur"});
    // The audio clip takes none of the video attributes, and says why.
    CHECK(pasted->changes[1].applied.empty());
    CHECK(pasted->changes[1].skipped.size() == 3);

    // Scale Attribute Times: B is twice as long, so the keys land at twice the offset.
    AttributeSet onlyMotion;
    onlyMotion.motion = true;
    const auto scaled = pasteAttributes(s, tl.seq, source, std::span(&b, 1), onlyMotion, true);
    REQUIRE(scaled);
    const Curve& stretched = std::get<Curve>(clipIn(scaled->edit.state, tl.seq, "V1@10").properties()[ParamId::Scale].data);
    REQUIRE(stretched.keys.size() == 2);
    CHECK(stretched.keys[0].time == f(4));
    CHECK(stretched.keys[1].time == f(16));

    // Time remapping: B takes A's remap (here the identity), starting from the frame B showed first.
    ProjectState remapped = ok(edit::setTimeRemapEnabled(s, tl.seq, a, true)).state;
    const Clip remapSource = remapped.findSequence(tl.seq)->findClip(a)->clip();
    AttributeSet remap;
    remap.timeRemap = true;
    const auto timed = pasteAttributes(remapped, tl.seq, remapSource, std::span(&b, 1), remap);
    REQUIRE(timed);
    requireValid(timed->edit.state);
    const Clip& bRemapped = clipIn(timed->edit.state, tl.seq, "V1@10");
    CHECK(bRemapped.isRemapped());
    CHECK(bRemapped.sourceTimeAt(bRemapped.start()) == clipIn(s, tl.seq, "V1@10").sourceTimeAt(f(10)));
    CHECK(bRemapped.duration() == f(20));

    // Nothing applicable refuses; an effect index past the source's stack refuses.
    AttributeSet volume;
    volume.volume = true;
    CHECK_FALSE(pasteAttributes(s, tl.seq, source, std::span(&b, 1), volume));
    AttributeSet missing;
    missing.effects = {3};
    CHECK_FALSE(pasteAttributes(s, tl.seq, source, std::span(&b, 1), missing));
}

TEST_CASE("Remove Attributes: parameters back to defaults, effects by id, time remapping off", "[model][clipboard][attributes]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbb|
    )");
    ProjectState s = tl.state;
    const ClipId a = tl.clip("V1@0");
    s = ok(edit::addKeyframe(s, tl.seq, a, edit::ParamRef::of(ParamId::Scale), Keyframe{f(2), 0.5})).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.blur.gaussian")).state;
    s = ok(edit::addEffect(s, tl.seq, std::span(&a, 1), "wabcut.sharpen")).state;
    s = ok(edit::setTimeRemapEnabled(s, tl.seq, a, true)).state;
    AttributeSet remove;
    remove.motion = true;
    remove.timeRemap = true;
    remove.effectIds = {"wabcut.blur.gaussian"};
    const auto removed = removeAttributes(s, tl.seq, std::span(&a, 1), remove);
    REQUIRE(removed);
    requireValid(removed->edit.state);
    const Clip& after = clipIn(removed->edit.state, tl.seq, "V1@0");
    CHECK(after.properties()[ParamId::Scale] == ClipProperties()[ParamId::Scale]);
    CHECK_FALSE(after.isRemapped());
    REQUIRE(after.effects().size() == 1);
    CHECK(after.effects()[0].effect == "wabcut.sharpen");
    CHECK(removed->changes[0].applied == std::vector<std::string>{"Time Remapping", "Motion", "Gaussian Blur"});
    // Again: nothing left to remove.
    CHECK_FALSE(removeAttributes(removed->edit.state, tl.seq, std::span(&a, 1), remove));
}

TEST_CASE("Clipboard: a clip elsewhere past the end of a relinked shorter file does not block a paste (E5-P2-2)", "[model][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
    )");
    // B was relinked to a render two frames shorter: its clip now plays past the end (loadable, not Strict).
    ProjectState s = tl.state;
    for (MediaPtr& m : s.media) {
        if (m->name == "B") {
            auto shorter = std::make_shared<MediaItem>(*m);
            shorter->info.duration = f(2);
            for (auto& v : shorter->info.video) {
                v.duration = f(2);
            }
            for (auto& a : shorter->info.audio) {
                a.duration = f(2);
            }
            m = std::move(shorter);
        }
    }
    REQUIRE(validate(s, Validation::Structure).has_value());
    REQUIRE_FALSE(validate(s, Validation::Strict).has_value());
    edit::SequenceSettings settings;
    settings.frameRate = kRate;
    settings.videoTracks = 1;
    settings.audioTracks = 1;
    auto second = edit::addSequence(s, settings);
    REQUIRE(second);
    const ClipId a = tl.clip("V1@0");
    auto copied = copyClips(second->state, tl.seq, std::span(&a, 1));
    REQUIRE(copied);
    const auto pasted = pasteClips(second->state, second->id, *copied, f(0));
    INFO((pasted ? std::string("pasted") : pasted.error().message));
    REQUIRE(pasted.has_value());
    CHECK(pasted->report.pasted.size() == 1);

    // The paste still checks what it brings: B's clip itself plays past the end of its file.
    const ClipId b = tl.clip("V1@4");
    auto copiedB = copyClips(second->state, tl.seq, std::span(&b, 1));
    REQUIRE(copiedB);
    const auto refused = pasteClips(second->state, second->id, *copiedB, f(0));
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().message.find("past the end") != std::string::npos);
}

TEST_CASE("Clipboard: a nested clip never lands inside the sequence it plays, nor binds another project's (E5-P2-3)", "[model][clipboard]") {
    Timeline tl(R"(
        V1 tgt |AaaaBbbb....|
    )");
    const ClipId a = tl.clip("V1@0");
    auto nest = edit::nestClips(tl.state, tl.seq, std::span(&a, 1), "N");
    REQUIRE(nest);
    const ProjectState s = nest->edit.state;
    const std::vector<ClipId> both{clipAt(s, tl.seq, "V1@0"), clipAt(s, tl.seq, "V1@4")};
    auto copied = copyClips(s, tl.seq, both);
    REQUIRE(copied);
    const auto noted = [](const PasteResult& r, std::string_view part) {
        return std::any_of(r.report.notes.begin(), r.report.notes.end(),
                           [&](const std::string& n) { return n.find(part) != std::string::npos; });
    };

    // The same project, N unchanged: the nested clip binds to N.
    const auto here = pasteClips(s, tl.seq, *copied, f(8));
    REQUIRE(here);
    CHECK(here->report.pasted.size() == 2);

    // Into N itself: N's clip would play N inside N. It is left out with a note; B is pasted.
    const auto inside = pasteClips(s, nest->nested, *copied, f(10));
    INFO((inside ? std::string("pasted") : inside.error().message));
    REQUIRE(inside.has_value());
    requireValid(inside->edit.state);
    CHECK(inside->report.pasted.size() == 1);
    CHECK(noted(*inside, "inside itself"));

    // A project whose "N" has the same id and name but other content (another project made from the same template):
    // the clip does not bind to it.
    const ProjectState otherN = ok(edit::addMarker(s, nest->nested, Marker{f(1), Ticks{}, "not the copied N"})).state;
    const auto elsewhere = pasteClips(otherN, tl.seq, *copied, f(8));
    REQUIRE(elsewhere);
    CHECK(elsewhere->report.pasted.size() == 1);
    CHECK(noted(*elsewhere, "not the sequence that was copied"));
}

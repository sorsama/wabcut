// Source-anchored redactions in evaluation (I.6): a region marked once in media time reaches every layer that shows
// a marked frame of that media - trimmed, sped up, slowed down, reversed, frozen, reused, in any sequence at any
// rate - and no other; per-instance overrides; razor halves; the region's keyframes follow source time; effect masks
// and the mask / mosaic effects map to the compositor. The oracle is computed from the clip's own time mapping
// (Clip::sourceTimeAt) and the media's frame grid, independently of evaluateRedactions().

#include "PlaybackTestSupport.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/Mask.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>

using namespace wabcut;
using namespace wabcut::playback;
using wabcut::playback::test::kFrame60;
using Catch::Approx;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

std::shared_ptr<MediaItem> fakeMedia(ProjectState& s, int w = 1920, int h = 1080) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = w;
    v.height = h;
    v.frameRate = kRate60;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

Clip clipOf(ProjectState& s, MediaId media, Ticks start, Ticks duration, Ticks sourceIn, Speed speed = {1, 1},
            bool reversed = false) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {start, duration};
    c.sourceIn = sourceIn;
    c.speed = speed;
    c.reversed = reversed;
    return c;
}

TrackPtr trackOf(ProjectState& s, std::vector<Clip> clips) {
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList(std::move(clips));
    return std::make_shared<const Track>(std::move(t));
}

SequencePtr sequenceOf(ProjectState& s, Rate rate, std::vector<TrackPtr> tracks) {
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = rate;
    for (const TrackPtr& t : tracks) {
        for (const Clip& c : t->clips) {
            seq.duration = std::max(seq.duration, c.timelineRange.end());
        }
    }
    seq.videoTracks = std::move(tracks);
    return std::make_shared<const Sequence>(std::move(seq));
}

Redaction redactionOf(MediaId media, Ticks start, Ticks end) {
    Redaction r;
    r.media = media;
    r.source = TimeRange::fromStartEnd(start, end);
    r.style = RedactionStyle::Mosaic;
    r.region.shape = MaskShape::Rectangle;
    r.region.set("center.x", 0.3);
    r.region.set("size.x", 0.2);
    return r;
}

ProjectState withRedaction(const ProjectState& s, Redaction r, RedactionId* id = nullptr) {
    auto added = edit::addRedaction(s, std::move(r));
    REQUIRE(added);
    if (id) {
        *id = added->id;
    }
    return std::move(added->state);
}

const Clip& clipIn(const Sequence& seq, ClipId id) {
    for (const TrackPtr& t : seq.videoTracks) {
        for (const Clip& c : t->clips) {
            if (c.id == id) {
                return c;
            }
        }
    }
    FAIL("no such clip");
    throw std::logic_error("unreachable");
}

/// Media frame (60 fps grid) on screen for `clip` at sequence time `t`.
std::int64_t shownFrame(const Clip& clip, Ticks t) { return ticksToFrames(clip.sourceTimeAt(t), kRate60); }

struct Sweep {
    int layers = 0;   ///< layers of the redacted media seen
    int redacted = 0; ///< of which redacted
    int wrong = 0;    ///< layers whose redaction disagrees with the oracle
};

/// Every frame of `seq` at its own rate: a layer of `media` is redacted exactly when its shown media frame lies in
/// [firstFrame, endFrame) and its clip is not excepted.
Sweep sweep(const ProjectState& s, const Sequence& seq, MediaId media, std::int64_t firstFrame, std::int64_t endFrame,
            const std::vector<ClipId>& excepted = {}) {
    Sweep out;
    std::vector<LayerSpec> layers;
    for (std::int64_t f = 0;; ++f) {
        const Ticks t = *framesToTicks(f, seq.frameRate, Rounding::Floor);
        if (t >= seq.duration) {
            break;
        }
        evaluateVideoLayers(s, seq, t, layers);
        for (const LayerSpec& layer : layers) {
            if (layer.media != media) {
                CHECK(layer.redactions.empty());
                continue;
            }
            ++out.layers;
            const std::int64_t frame = shownFrame(clipIn(seq, layer.clip), t);
            const bool expected = frame >= firstFrame && frame < endFrame &&
                                  std::find(excepted.begin(), excepted.end(), layer.clip) == excepted.end();
            out.redacted += layer.redactions.empty() ? 0 : 1;
            if (expected != !layer.redactions.empty()) {
                ++out.wrong;
                UNSCOPED_INFO("sequence frame " << f << ", clip " << layer.clip.value << ", media frame " << frame
                                                << ": expected " << (expected ? "redacted" : "clear"));
            }
            for (const render::LayerRedaction& r : layer.redactions) {
                CHECK(r.mask);
                CHECK(r.style == render::LayerRedaction::Style::Mosaic);
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("Redaction: every derived cut shows the marked source frames redacted, and only those",
          "[playback][evaluate][redaction]") {
    ProjectState s;
    auto a = fakeMedia(s);
    auto b = fakeMedia(s);
    s.media = {a, b};
    // Marked: media frames 120..179 (2 s .. 3 s).
    s = withRedaction(s, redactionOf(a->id, sec(2), sec(3)));

    // S1 (60 fps): an untouched use, a trim at 2x, a reversed use above them, and the other media.
    const SequencePtr s1 = sequenceOf(
        s, kRate60,
        {trackOf(s, {clipOf(s, a->id, sec(0), sec(5), sec(0)), clipOf(s, a->id, sec(5), sec(2), sec(1.5), {2, 1})}),
         trackOf(s, {clipOf(s, a->id, sec(0), sec(4), sec(0), {1, 1}, true), clipOf(s, b->id, sec(4), sec(3), sec(2))})});
    // S2 (24 fps, another rate): slow motion through the range, a freeze frame inside it, reuse of the same range.
    const SequencePtr s2 = sequenceOf(
        s, kRate24,
        {trackOf(s, {clipOf(s, a->id, sec(0), sec(3), sec(1), {1, 2}), clipOf(s, a->id, sec(3), sec(1), sec(2.5), {0, 1}),
                     clipOf(s, a->id, sec(4), sec(2), sec(1.9))})});
    // S3 (29.97 fps, not a whole number of ticks per frame): 3x speed reversed through it.
    const SequencePtr s3 =
        sequenceOf(s, kRate29_97, {trackOf(s, {clipOf(s, a->id, sec(0), sec(2), sec(0.5), {3, 1}, true)})});
    s.sequences = {s1, s2, s3};

    int total = 0;
    for (const SequencePtr& seq : s.sequences) {
        const Sweep r = sweep(s, *seq, a->id, 120, 180);
        INFO("sequence " << seq->id.value << ": " << r.redacted << " of " << r.layers << " layers redacted");
        CHECK(r.wrong == 0);
        CHECK(r.redacted > 0);
        CHECK(r.redacted < r.layers);
        total += r.redacted;
    }
    CHECK(total > 100); // (layers under an opaque full-canvas layer are culled)

    // A disabled redaction redacts nothing; the region's mode is ignored (always an area).
    ProjectState off = s;
    off.redactions.front().enabled = false;
    CHECK(sweep(off, *s1, a->id, 0, 0).wrong == 0);
}

TEST_CASE("Redaction: a marked frame stays redacted while it shows, whatever the sequence rate", "[playback][evaluate][redaction]") {
    ProjectState s;
    auto a = fakeMedia(s);
    s.media = {a};
    // Marked by time, not on the media's frame grid: 2.005 s .. 2.03 s touches frames 120 (2.0 .. 2.0167 s) and 121.
    s = withRedaction(s, redactionOf(a->id, sec(2.005), sec(2.03)));
    const Clip c = clipOf(s, a->id, sec(0), sec(4), sec(0));
    const Sequence seq = *sequenceOf(s, kRate60, {trackOf(s, {c})});
    std::vector<LayerSpec> layers;
    for (const double t : {2.0, 2.004, 2.01, 2.0166, 2.02, 2.033}) {
        evaluateVideoLayers(s, seq, sec(t), layers);
        REQUIRE(layers.size() == 1);
        INFO("t = " << t);
        CHECK_FALSE(layers[0].redactions.empty()); // frames 120 and 121 show the marked moment
    }
    for (const double t : {1.99, 2.034, 2.04}) {
        evaluateVideoLayers(s, seq, sec(t), layers);
        INFO("t = " << t);
        CHECK(layers[0].redactions.empty());
    }
}

TEST_CASE("Redaction: per-instance overrides; razor halves stay redacted", "[playback][evaluate][redaction]") {
    ProjectState s;
    auto a = fakeMedia(s);
    s.media = {a};
    RedactionId id;
    s = withRedaction(s, redactionOf(a->id, sec(0), sec(10)), &id);
    const Clip first = clipOf(s, a->id, sec(0), sec(4), sec(0));
    const Clip second = clipOf(s, a->id, sec(4), sec(4), sec(0));
    const TrackPtr track = trackOf(s, {first, second});
    s.sequences = {sequenceOf(s, kRate60, {track})};
    const SequenceId seqId = s.sequences.front()->id;

    auto excepted = edit::setRedactionOverride(s, id, first.id, false); // "don't redact here"
    REQUIRE(excepted);
    s = std::move(*excepted);
    CHECK(sweep(s, *s.findSequence(seqId), a->id, 0, 600, {first.id}).wrong == 0);

    // Razor the excepted clip: its right half is a new clip, redacted again until excepted too (the safe side).
    auto cut = edit::addEdit(s, seqId, sec(2), std::array{track->id});
    REQUIRE(cut);
    REQUIRE(cut->created.size() == 1);
    s = std::move(cut->state);
    CHECK(sweep(s, *s.findSequence(seqId), a->id, 0, 600, {first.id}).wrong == 0);
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, *s.findSequence(seqId), sec(3), layers);
    REQUIRE(layers.size() == 1);
    CHECK(layers[0].clip == cut->created.front());
    CHECK_FALSE(layers[0].redactions.empty());

    // Lifting the override redacts the clip again.
    auto lifted = edit::setRedactionOverride(s, id, first.id, true);
    REQUIRE(lifted);
    CHECK(sweep(*lifted, *lifted->findSequence(seqId), a->id, 0, 600).wrong == 0);
}

TEST_CASE("Redaction: the region's keyframes follow source time through a retimed clip", "[playback][evaluate][redaction]") {
    ProjectState s;
    auto a = fakeMedia(s);
    s.media = {a};
    Redaction r = redactionOf(a->id, sec(2), sec(4));
    Curve x;
    x.keys = {{sec(2), 0.0}, {sec(4), 0.2}}; // a tracked move: +0.1 per source second
    r.region.set("transform.x", ParamValue(x));
    s = withRedaction(s, r);
    const Clip fast = clipOf(s, a->id, sec(0), sec(2), sec(1), {2, 1}); // source 1 .. 5
    const Sequence seq = *sequenceOf(s, kRate60, {trackOf(s, {fast})});
    std::vector<LayerSpec> layers;
    for (const double t : {0.5, 1.0, 1.25, 1.49}) {
        evaluateVideoLayers(s, seq, sec(t), layers);
        REQUIRE(layers.size() == 1);
        REQUIRE(layers[0].redactions.size() == 1);
        const double source = 1.0 + 2.0 * t;
        INFO("t = " << t << ", source " << source);
        CHECK(layers[0].redactions[0].mask->shapes.at(0).affine[2] == Approx(0.1 * (source - 2.0)).margin(1e-9));
    }
}

TEST_CASE("Redaction: remapped holds and reverse use source time while effect masks keep animating",
          "[playback][evaluate][redaction][remap]") {
    ProjectState s;
    auto item = fakeMedia(s);
    s.media = {item};
    s = withRedaction(s, redactionOf(item->id, sec(2.4), sec(2.6)));
    Clip c = clipOf(s, item->id, sec(1), sec(3), sec(0.5));
    ClipContent content = c.content();
    Curve remap;
    remap.keys = {{sec(0), double(sec(1).value)}, {sec(0.5), double(sec(2.5).value)},
                  {sec(1.5), double(sec(2.5).value)}, {sec(2.5), double(sec(1.5).value)},
                  {sec(4), double(sec(4).value)}};
    content.timeRemap = remap;
    Mask mask;
    mask.id = 1;
    Curve offset;
    offset.keys = {{sec(0.5), 0.0}, {sec(3.5), 0.3}};
    mask.set("transform.x", ParamValue(offset));
    EffectInstance fx;
    fx.id = s.ids.allocate<EffectInstanceId>();
    fx.effect = "wabcut.mask";
    fx.masks = {mask};
    content.effects = {fx};
    c.setContent(std::move(content));
    const Sequence seq = *sequenceOf(s, kRate60, {trackOf(s, {c})});
    for (double t : {0.5, 0.75, 1.0, 1.5, 1.9, 2.0, 2.5, 3.0, 4.0, 4.2}) {
        INFO("timeline time " << t);
        CHECK(sourceTimeAround(c, sec(t)) == c.sourceTimeAt(sec(t)));
        CHECK(curveTimeAround(c, sec(t)) == c.curveTimeAt(sec(t)));
    }
    // Every sequence frame in the hold/reverse/reuse range: source privacy follows the decoded source mapping.
    for (int f = 60; f < 240; ++f) {
        const Ticks t = framesToTicks(f, kRate60);
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(s, seq, t, layers);
        REQUIRE(layers.size() == 1);
        CHECK(layers[0].sourceTime == c.sourceTimeAt(t));
        const auto expected = evaluateRedactions(s, c, c.sourceTimeAt(t), 1920.0 / 1080.0, kRate60);
        REQUIRE(layers[0].redactions.size() == expected.size());
        if (!expected.empty()) {
            CHECK(layers[0].redactions[0].style == expected[0].style);
            CHECK(*layers[0].redactions[0].mask == *expected[0].mask);
        }
        REQUIRE(layers[0].effects.size() == 1);
        CHECK(layers[0].effects[0].mask->shapes[0].affine[2] ==
              Approx(offset.valueAt(c.curveTimeAt(t))).margin(1e-9));
    }
}

TEST_CASE("Redaction: decoded VFR frame boundaries protect the entire displayed picture",
          "[playback][evaluate][redaction]") {
    ProjectState s;
    auto item = fakeMedia(s);
    s.media = {item};
    RedactionId id;
    s = withRedaction(s, redactionOf(item->id, sec(2.04), sec(2.06)), &id);
    Curve tracked;
    tracked.keys = {{sec(2.0), 0.0}, {sec(2.1), 0.25}};
    s.redactions.front().region.set("transform.x", ParamValue(tracked));
    const Clip c = clipOf(s, item->id, sec(0), sec(4), sec(0));
    const Sequence seq = *sequenceOf(s, kRate60, {trackOf(s, {c})});
    // A VFR picture spans 2.0..2.1 s, crossing the marked range. Its nominal 60 Hz slot does not.
    static const std::array<std::uint8_t, 16> pixels{};
    media::VideoFrame frame;
    frame.width = frame.height = 2;
    frame.pts = sec(2);
    frame.duration = sec(0.1);
    frame.format = media::PixelFormat::RGBA8;
    media::CpuPlanes planes;
    planes.planes[0] = {pixels.data(), 8, 2, 2};
    planes.owner = std::make_shared<int>(0);
    frame.payload = planes;
    REQUIRE(frame.isValid());
    const auto covered = [&](const ProjectState& state, Ticks requested, const media::VideoFrame& decoded) {
        std::vector<LayerSpec> specs;
        evaluateVideoLayers(state, seq, requested, specs);
        std::vector<media::VideoFrame> frames{decoded};
        std::vector<render::CompositeLayer> layers;
        buildCompositeLayers(specs, frames, layers); // shared by preview and export, including burn-in
        REQUIRE(layers.size() == 1);
        if (!layers.front().redactions.empty()) {
            // The same decoded picture keeps the same tracked region throughout its display interval.
            CHECK(layers.front().redactions.front().mask->shapes[0].affine[2] ==
                  Approx(tracked.valueAt(decoded.pts)).margin(1e-9));
        }
        return !layers.front().redactions.empty();
    };
    for (double t : {2.0, 2.02, 2.05, 2.08, 2.099}) {
        INFO("requested source time " << t);
        CHECK(covered(s, sec(t), frame));
    }
    // An explicit per-instance exception still wins after decoding.
    s.sequences = {std::make_shared<const Sequence>(seq)};
    auto excepted = edit::setRedactionOverride(s, id, c.id, false);
    REQUIRE(excepted);
    CHECK_FALSE(covered(*excepted, sec(2.02), frame));
    // Adjacent half-open intervals must not redact the next picture.
    frame.pts = sec(2.06);
    frame.duration = sec(0.04);
    CHECK_FALSE(covered(s, sec(2.061), frame)); // nominal 60 Hz still overlaps; the actual frame does not
}

TEST_CASE("Effect masks and the mask / mosaic effects reach the compositor", "[playback][evaluate][masks]") {
    ProjectState s;
    auto a = fakeMedia(s, 1280, 720);
    s.media = {a};
    Clip c = clipOf(s, a->id, sec(0), sec(2), sec(0));
    const auto instance = [&](const char* id) {
        EffectInstance e;
        e.id = s.ids.allocate<EffectInstanceId>();
        e.effect = id;
        return e;
    };
    Mask face;
    face.id = 1;
    face.shape = MaskShape::Ellipse;
    face.set("feather", 0.05);
    Mask off = face;
    off.id = 2;
    off.enabled = false;
    EffectInstance blur = instance("wabcut.blur.gaussian");
    blur.set("radius", ParamValue(8.0));
    blur.masks = {face};
    EffectInstance grade = instance("wabcut.color.grade");
    grade.set("basic.exposure", ParamValue(0.5));
    grade.masks = {face, off};
    EffectInstance cut = instance("wabcut.mask");
    cut.masks = {face};
    EffectInstance empty = instance("wabcut.mask"); // no mask: cuts nothing, left out
    EffectInstance mosaic = instance("wabcut.mosaic");
    mosaic.set("cellSize", ParamValue(0.04));
    ClipContent content = c.content();
    content.effects = {blur, grade, cut, empty, mosaic};
    c.setContent(std::move(content));
    const Sequence seq = *sequenceOf(s, kRate60, {trackOf(s, {c})});
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, sec(1), layers);
    REQUIRE(layers.size() == 1);
    const auto& fx = layers[0].effects;
    REQUIRE(fx.size() == 4);
    CHECK(fx[0].type == render::EffectType::GaussianBlur);
    REQUIRE(fx[0].mask);
    CHECK(fx[0].mask->shapes.size() == 1);
    CHECK(fx[0].mask->aspect == Approx(1280.0 / 720.0));
    CHECK(fx[0].mask->shapes[0].feather == Approx(0.05));
    CHECK(fx[0].mask->key != 0);
    CHECK(fx[1].type == render::EffectType::Grade);
    REQUIRE(fx[1].mask);
    CHECK(fx[1].mask->shapes.size() == 1); // the disabled mask is left out
    CHECK(*fx[1].mask == *fx[0].mask);     // same shape, same coverage key
    CHECK(fx[2].type == render::EffectType::OpacityMask);
    CHECK(fx[2].mask);
    CHECK(fx[3].type == render::EffectType::Mosaic);
    CHECK(fx[3].params[0] == Approx(0.04));
    CHECK_FALSE(fx[3].mask);
    CHECK(layers[0].bypassedEffects == 0);

    // The composite layer carries them (and the redactions) to the compositor.
    static const std::array<std::uint8_t, 16> pixels{};
    media::VideoFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.duration = kFrame60;
    frame.format = media::PixelFormat::RGBA8;
    media::CpuPlanes planes;
    planes.planes[0] = {pixels.data(), 8, 2, 2};
    planes.owner = std::make_shared<int>(0);
    frame.payload = planes;
    REQUIRE(frame.isValid());
    std::vector<media::VideoFrame> frames{frame};
    std::vector<render::CompositeLayer> composite;
    buildCompositeLayers(layers, frames, composite);
    REQUIRE(composite.size() == 1);
    CHECK(composite[0].effects.size() == 4);
    CHECK(composite[0].effects[0].mask == fx[0].mask);

    // A redaction of the media reaches the composite layer too.
    s = withRedaction(s, redactionOf(a->id, sec(0), sec(5)));
    evaluateVideoLayers(s, seq, sec(1), layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].redactions.size() == 1);
    frames = {frame};
    composite.clear();
    buildCompositeLayers(layers, frames, composite);
    REQUIRE(composite.size() == 1);
    REQUIRE(composite[0].redactions.size() == layers[0].redactions.size());
    CHECK(composite[0].redactions[0].style == layers[0].redactions[0].style);
    CHECK(*composite[0].redactions[0].mask == *layers[0].redactions[0].mask);
}

// Codex wave-4 P2-29: a region's opacity and invert must not weaken a redaction (an inverted one would expose exactly what
// it marks), and the marked edge must be fully covered: the feather lies outside it. A region from an older file or a
// direct edit with opacity 0.3, inverted and a wide feather is evaluated as a full-strength, un-inverted cover whose
// coverage is 1 everywhere inside the marked rectangle. Failed before the fix.
TEST_CASE("Redaction: opacity, invert and feather never weaken the marked area", "[playback][evaluate][redaction]") {
    ProjectState s;
    auto a = fakeMedia(s);
    s.media = {a};
    Redaction r = redactionOf(a->id, sec(0), sec(10)); // a rectangle: centre (0.3, 0.5), size 0.2 x 0.5
    r.region.inverted = true;
    r.region.set("opacity", 0.3);
    r.region.set("feather", 0.1);
    s = withRedaction(s, r);
    const Clip clip = clipOf(s, a->id, sec(0), sec(4), sec(0));
    s.sequences = {sequenceOf(s, kRate60, {trackOf(s, {clip})})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, *s.sequences.front(), sec(1), layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].redactions.size() == 1);
    const render::MaskSpec& spec = *layers[0].redactions[0].mask;
    REQUIRE(spec.shapes.size() == 1);
    CHECK(spec.shapes[0].opacity == 1.0);
    CHECK_FALSE(spec.shapes[0].inverted);
    CHECK(spec.shapes[0].mode == 0);
    constexpr int kW = 384, kH = 216;
    const std::vector<float> coverage = render::maskCoverageCpu(spec, kW, kH);
    const auto at = [&](double u, double v) {
        return coverage[static_cast<std::size_t>(v * kH) * kW + static_cast<std::size_t>(u * kW)];
    };
    // Inside the marked rectangle, up to its very edge: fully covered.
    for (const double u : {0.205, 0.25, 0.3, 0.35, 0.395}) {
        for (const double v : {0.255, 0.5, 0.745}) {
            INFO("(" << u << ", " << v << ")");
            CHECK(at(u, v) >= 0.999f);
        }
    }
    // Far outside it (beyond the feather): clear.
    CHECK(at(0.8, 0.5) == 0.0f);
    CHECK(at(0.3, 0.05) == 0.0f);
}

// Codex wave-4 P1-3 in the preview: evaluateVideoLayers draws a burned-in cue with the bleeped word replaced, as export
// does (one code path), timed words exactly, untimed cues whole; captions outside the bleep and a project without
// audio redactions are untouched; a chosen token is used. Failed before the fix (the preview drew the word).
TEST_CASE("Redaction: burned-in captions never draw a bleeped word, in the preview as in export",
          "[playback][evaluate][redaction][captions]") {
    ProjectState s;
    auto a = fakeMedia(s);
    s.media = {a};
    Redaction r = redactionOf(a->id, sec(0), sec(10));
    r.audio = RedactionAudio::Bleep;
    r.audioRanges = {TimeRange::fromStartEnd(sec(2.4), sec(2.6))}; // media 2.4 .. 2.6 s
    s = withRedaction(s, r);
    const Clip clip = clipOf(s, a->id, sec(0), sec(4), sec(1)); // media 1 .. 5 s: the bleep plays at 1.4 .. 1.6 s
    Sequence seq = *sequenceOf(s, kRate60, {trackOf(s, {clip})});
    CaptionTrack track;
    track.id = s.ids.allocate<CaptionTrackId>();
    track.enabled = true;
    track.cues = {{sec(1.2), sec(0.6), "call Alice now"}, {sec(2.0), sec(0.5), "all clear"}};
    seq.captionTracks = {track};
    auto timings = std::make_shared<CaptionWordTimings>();
    CaptionSettings captions;
    const auto textAt = [&](const ProjectState& project, double t) {
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(project, seq, sec(t), layers, false, nullptr, &captions);
        REQUIRE_FALSE(layers.empty());
        REQUIRE(layers.back().caption);
        return layers.back().caption->text;
    };
    CHECK(textAt(s, 1.3) == "[bleep]"); // no word timings: the whole cue
    CHECK(textAt(s, 2.1) == "all clear");
    timings->cues[{track.id.value, sec(1.2).value}] = {{sec(1.2), sec(1.4)}, {sec(1.4), sec(1.6)}, {sec(1.6), sec(1.8)}};
    captions.words = timings;
    CHECK(textAt(s, 1.3) == "call [bleep] now");
    captions.redactedWord = "***";
    CHECK(textAt(s, 1.3) == "call *** now");
    ProjectState unbleeped = s;
    unbleeped.redactions.front().audio = RedactionAudio::None;
    CHECK(textAt(unbleeped, 1.3) == "call Alice now");
    // evaluateCaption alone (no project) cannot know: it draws the cue as written.
    CHECK(evaluateCaption(seq, sec(1.3), captions)->text == "call Alice now");
}

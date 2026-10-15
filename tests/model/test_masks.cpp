// Masks and source-anchored redactions (I.6, docs/masks.md): shapes and their outlines, path interpolation with
// matching vertex counts, the tracker's transform, bounds on untrusted data, the edit ops with undo, IO round trips
// and byte-identical saves of projects that use neither.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

using namespace wabcut;
using namespace wabcut::test;
using Catch::Approx;

namespace {

Ticks s(double seconds) { return Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))}; }

MaskPathKey key(Ticks t, std::vector<MaskVertex> v, Interpolation i = Interpolation::Linear) { return {t, std::move(v), i}; }

std::vector<MaskVertex> triangle(double dx) {
    return {{0.2 + dx, 0.2}, {0.8 + dx, 0.2}, {0.5 + dx, 0.8}};
}

Mask pathMask() {
    Mask m;
    m.shape = MaskShape::Path;
    m.path = {key(Ticks{}, triangle(0.0)), key(s(2), triangle(0.1))};
    return m;
}

const Clip& clipIn(const ProjectState& state, SequenceId seq, ClipId clip) {
    return state.findSequence(seq)->findClip(clip)->clip();
}

} // namespace

TEST_CASE("Mask paths interpolate vertex by vertex: linear, hold and eased keys", "[model][masks]") {
    Mask m = pathMask();
    CHECK(maskPathAt(m, Ticks{}) == triangle(0.0));
    CHECK(maskPathAt(m, s(-1)) == triangle(0.0)); // clamped before the first key
    CHECK(maskPathAt(m, s(5)) == triangle(0.1));  // and after the last
    const auto half = maskPathAt(m, s(1));
    REQUIRE(half.size() == 3);
    CHECK(half[0].x == Approx(0.25));
    CHECK(half[2].y == Approx(0.8));
    m.path[0].interpolation = Interpolation::Hold;
    CHECK(maskPathAt(m, s(1.9)) == triangle(0.0));
    // Ease out of A and into B: slow at both ends, half way at the middle, monotonic.
    m.path[0].interpolation = Interpolation::EaseInOut;
    m.path[1].interpolation = Interpolation::EaseInOut;
    CHECK(maskPathAt(m, s(1))[0].x == Approx(0.25));
    CHECK(maskPathAt(m, s(0.2))[0].x < 0.2 + 0.1 * 0.1);
    double previous = 0.0;
    for (int i = 0; i <= 20; ++i) {
        const double x = maskPathAt(m, s(0.1 * i))[0].x;
        CHECK(x >= previous);
        previous = x;
    }
    // Tangents interpolate too.
    m.path[0].vertices[0].outX = 0.1;
    m.path[0].interpolation = Interpolation::Linear;
    m.path[1].interpolation = Interpolation::Linear;
    CHECK(maskPathAt(m, s(1))[0].outX == Approx(0.05));
}

TEST_CASE("Mask outlines: ellipse arcs, rounded rectangles, the transform about its anchor", "[model][masks]") {
    Mask e;
    e.shape = MaskShape::Ellipse;
    e.set("center.x", 0.5);
    e.set("center.y", 0.5);
    e.set("size.x", 0.4);
    e.set("size.y", 0.2);
    auto outline = maskOutlineAt(e, Ticks{}, 16.0 / 9.0);
    REQUIRE(outline.size() == 4);
    CHECK(outline[0].x == Approx(0.7)); // right
    CHECK(outline[0].y == Approx(0.5));
    CHECK(outline[1].y == Approx(0.6)); // bottom
    CHECK(outline[1].x == Approx(0.5));
    // Rotated 90 degrees in pixels: the 0.4-wide (aspect units) ellipse stands up.
    e.set("rotation", 90.0);
    outline = maskOutlineAt(e, Ticks{}, 16.0 / 9.0);
    CHECK(outline[0].x == Approx(0.5).margin(1e-9));
    CHECK(outline[0].y == Approx(0.5 + 0.2 * 16.0 / 9.0));

    Mask r;
    r.shape = MaskShape::Rectangle;
    CHECK(maskOutlineAt(r, Ticks{}, 1.0).size() == 4);
    r.set("roundness", 0.5);
    outline = maskOutlineAt(r, Ticks{}, 1.0);
    REQUIRE(outline.size() == 8);
    CHECK(outline[0].x == Approx(0.25 + 0.125)); // top edge starts after the corner radius
    CHECK(outline[0].y == Approx(0.25));
    // A zero-size shape has no outline.
    r.set("size.x", 0.0);
    CHECK(maskOutlineAt(r, Ticks{}, 1.0).empty());

    // The transform: translate, then rotate 180 degrees about the anchor.
    Mask t = pathMask();
    t.set("transform.x", 0.1);
    CHECK(maskOutlineAt(t, Ticks{}, 1.0)[0].x == Approx(0.3));
    t.set("transform.x", 0.0);
    t.set("transform.rotation", 180.0);
    const MaskAffine m = maskTransformAt(t, Ticks{}, 1.0);
    CHECK(m.a == Approx(-1.0));
    CHECK(m.c == Approx(1.0)); // about (0.5, 0.5)
    CHECK(maskOutlineAt(t, Ticks{}, 1.0)[0].x == Approx(0.8));
    CHECK(maskOutlineAt(t, Ticks{}, 1.0)[0].y == Approx(0.8));
    // Keyframed transform values (what the tracker writes) follow curve time.
    Curve x;
    x.keys = {{Ticks{}, 0.0}, {s(1), 0.2}};
    t.set("transform.rotation", 0.0);
    t.set("transform.x", ParamValue(x));
    CHECK(maskOutlineAt(t, s(0.5), 1.0)[0].x == Approx(0.2 + 0.025 + 0.1)); // the path moves too
}

TEST_CASE("Mask and redaction bounds reject malformed data", "[model][masks]") {
    Mask ok = pathMask();
    CHECK_FALSE(maskProblem(ok));
    Mask m = ok;
    m.path[1].vertices.pop_back(); // counts differ
    CHECK(maskProblem(m));
    m = ok;
    m.path[1].time = Ticks{}; // not increasing
    CHECK(maskProblem(m));
    m = ok;
    m.path[0].vertices[0].x = std::numeric_limits<double>::quiet_NaN();
    CHECK(maskProblem(m));
    m = ok;
    m.path = {key(Ticks{}, {{0, 0}, {1, 1}})}; // two vertices
    CHECK(maskProblem(m));
    m = ok;
    m.path = {};
    CHECK(maskProblem(m)); // a path mask without keys
    m = ok;
    m.set("opacity", 2.0);
    CHECK(maskProblem(m));
    m = ok;
    m.set("my.private.key", 3.0); // unknown keys are kept
    CHECK_FALSE(maskProblem(m));
    std::vector<Mask> list(kMaxMasks + 1, ok);
    for (std::uint32_t i = 0; i < list.size(); ++i) {
        list[i].id = i + 1;
    }
    CHECK(masksProblem(list));
    list.resize(2);
    list[1].id = list[0].id;
    CHECK(masksProblem(list)); // duplicate ids

    Redaction r;
    r.media = MediaId{5};
    r.source = {s(1), s(2)};
    CHECK_FALSE(redactionProblem(r));
    Redaction bad = r;
    bad.source.duration = Ticks{};
    CHECK(redactionProblem(bad));
    bad = r;
    bad.strength = 0.0;
    CHECK(redactionProblem(bad));
    bad = r;
    bad.exceptions = {ClipId{4}, ClipId{2}};
    CHECK(redactionProblem(bad)); // unsorted
    bad = r;
    bad.audioRanges = {{s(0), Ticks{}}};
    CHECK(redactionProblem(bad));
    // Codex wave-4 P2-11: an end past the largest time would overflow wherever start + duration is computed
    // (evaluation, the audio path). Failed before the fix.
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    bad = r;
    bad.source = {Ticks{kMax - 10}, Ticks{100}};
    CHECK(redactionProblem(bad));
    bad = r;
    bad.audioRanges = {{Ticks{kMax - 1}, Ticks{2}}};
    CHECK(redactionProblem(bad));
    bad = r;
    bad.source = {Ticks{kMax - 100}, Ticks{100}}; // ends exactly at the largest time: representable
    CHECK_FALSE(redactionProblem(bad));
}

TEST_CASE("Mask ops: add to an effect or as the clip's opacity mask, update, remove; one undo step each",
          "[model][masks]") {
    Timeline tl{R"(
        V1 tgt |Aaaa|
        A1 tgt |Aaaa|
    )"};
    const ClipId clip = tl.clip("V1@0");
    EditSession session;
    session.reset(tl.state);

    // The opacity mask effect is created on the first mask.
    Mask ellipse;
    ellipse.shape = MaskShape::Ellipse;
    std::uint32_t id = 0;
    REQUIRE(session.apply("Add Mask", [&](const ProjectState& st) -> std::expected<ProjectState, Error> {
        auto added = edit::addMask(st, tl.seq, clip, {}, ellipse);
        if (!added) {
            return std::unexpected(added.error());
        }
        id = added->id;
        return std::move(added->state);
    }));
    CHECK(id == 1);
    const EffectStack& stack = clipIn(*session.snapshot(), tl.seq, clip).effects();
    REQUIRE(stack.size() == 1);
    CHECK(stack[0].effect == "wabcut.mask");
    REQUIRE(stack[0].masks.size() == 1);
    CHECK(stack[0].masks[0].name == "Mask (1)");
    CHECK(validate(*session.snapshot()));

    // A second mask on the same opacity effect; then an update with keyframes and a removal.
    auto second = edit::addMask(*session.snapshot(), tl.seq, clip, {}, pathMask());
    REQUIRE(second);
    CHECK(second->id == 2);
    REQUIRE(session.apply("Add Mask", [&](const ProjectState&) -> std::expected<ProjectState, Error> { return second->state; }));
    Mask changed = clipIn(*session.snapshot(), tl.seq, clip).effects()[0].masks[1];
    Curve feather;
    feather.keys = {{Ticks{}, 0.0}, {s(1), 0.1}};
    changed.set("feather", ParamValue(feather));
    changed.mode = MaskMode::Subtract;
    REQUIRE(session.apply("Mask Feather", [&](const ProjectState& st) {
        return edit::updateMask(st, tl.seq, clip, {}, changed);
    }));
    CHECK(clipIn(*session.snapshot(), tl.seq, clip).effects()[0].masks[1] == changed);
    REQUIRE(session.apply("Remove Mask", [&](const ProjectState& st) { return edit::removeMask(st, tl.seq, clip, {}, 1); }));
    CHECK(clipIn(*session.snapshot(), tl.seq, clip).effects()[0].masks.size() == 1);
    // Undo walks back one step at a time.
    session.undo();
    CHECK(clipIn(*session.snapshot(), tl.seq, clip).effects()[0].masks.size() == 2);
    session.undo();
    CHECK(clipIn(*session.snapshot(), tl.seq, clip).effects()[0].masks[1].mode == MaskMode::Add);
    session.undo();
    session.undo();
    CHECK(clipIn(*session.snapshot(), tl.seq, clip).effects().empty());

    // Errors: audio clips, missing masks and effects, malformed masks.
    const ClipId audio = tl.clip("A1@0");
    CHECK_FALSE(edit::addMask(tl.state, tl.seq, audio, {}, ellipse));
    CHECK_FALSE(edit::updateMask(tl.state, tl.seq, clip, {}, ellipse)); // no opacity masks yet
    Mask broken = pathMask();
    broken.path[1].vertices.pop_back();
    CHECK_FALSE(edit::addMask(tl.state, tl.seq, clip, {}, broken));
    CHECK_FALSE(edit::addMask(tl.state, tl.seq, clip, EffectInstanceId{999}, ellipse));
    // Masks on a normal effect.
    const std::vector<ClipId> clips{clip};
    auto blurred = edit::addEffect(tl.state, tl.seq, clips, "wabcut.blur.gaussian");
    REQUIRE(blurred);
    const EffectInstanceId blur = blurred->createdEffects.at(0);
    auto onBlur = edit::addMask(blurred->state, tl.seq, clip, blur, ellipse);
    REQUIRE(onBlur);
    CHECK(clipIn(onBlur->state, tl.seq, clip).effects()[0].masks.size() == 1);
    CHECK(clipIn(onBlur->state, tl.seq, clip).effects().size() == 1); // no opacity effect added
}

TEST_CASE("Redaction ops: add, update, per-clip override, remove; ChangeSet and undo", "[model][masks]") {
    Timeline tl{R"(
        V1 tgt |Aaaa|
    )"};
    const MediaId media = tl.media('A');
    const ClipId clip = tl.clip("V1@0");
    EditSession session;
    session.reset(tl.state);
    Redaction r;
    r.media = media;
    r.name = "Plate";
    r.source = {Ticks{}, s(1)};
    r.region.shape = MaskShape::Rectangle;
    r.audio = RedactionAudio::Bleep;
    r.audioRanges = {{s(0.2), s(0.3)}};
    RedactionId id;
    REQUIRE(session.apply("Add Redaction", [&](const ProjectState& st) -> std::expected<ProjectState, Error> {
        auto added = edit::addRedaction(st, r);
        if (!added) {
            return std::unexpected(added.error());
        }
        id = added->id;
        return std::move(added->state);
    }));
    const ProjectState& after = *session.snapshot();
    REQUIRE(after.findRedaction(id));
    CHECK(after.findRedaction(id)->appliesTo(clip));
    CHECK(validate(after));
    CHECK(diff(tl.state, after).redactionsChanged);

    REQUIRE(session.apply("Don't Redact Here", [&](const ProjectState& st) { return edit::setRedactionOverride(st, id, clip, false); }));
    CHECK_FALSE(session.snapshot()->findRedaction(id)->appliesTo(clip));
    Redaction updated = *session.snapshot()->findRedaction(id);
    updated.style = RedactionStyle::Mosaic;
    REQUIRE(session.apply("Redaction Style", [&](const ProjectState& st) { return edit::updateRedaction(st, updated); }));
    CHECK(session.snapshot()->findRedaction(id)->style == RedactionStyle::Mosaic);
    REQUIRE(session.apply("Remove Redaction", [&](const ProjectState& st) { return edit::removeRedaction(st, id); }));
    CHECK(session.snapshot()->redactions.empty());
    session.undo();
    session.undo();
    CHECK(session.snapshot()->findRedaction(id)->style == RedactionStyle::Blur);
    session.undo();
    CHECK(session.snapshot()->findRedaction(id)->appliesTo(clip));

    // Errors: unknown media, bad ranges, unknown ids.
    Redaction stray = r;
    stray.media = MediaId{9999};
    CHECK_FALSE(edit::addRedaction(tl.state, stray));
    Redaction empty = r;
    empty.source.duration = Ticks{};
    CHECK_FALSE(edit::addRedaction(tl.state, empty));
    CHECK_FALSE(edit::removeRedaction(tl.state, RedactionId{77}));
    CHECK_FALSE(edit::setRedactionOverride(tl.state, RedactionId{77}, clip, false));
}

TEST_CASE("Masks and redactions survive a save; projects without them are byte-identical", "[model][masks][io]") {
    Timeline tl{R"(
        V1 tgt |Aaaa|
    )"};
    const ClipId clip = tl.clip("V1@0");
    // Without masks or redactions: the JSON has neither key, and a round trip is byte-identical.
    const auto plain = io::toJson(tl.state);
    REQUIRE(plain);
    CHECK(plain->find("\"masks\"") == std::string::npos);
    CHECK(plain->find("\"redactions\"") == std::string::npos);
    const auto reread = io::fromJson(*plain);
    REQUIRE(reread);
    CHECK(io::toJson(*reread).value() == *plain);

    Mask m = pathMask();
    m.inverted = true;
    m.mode = MaskMode::Intersect;
    Curve x;
    x.keys = {{Ticks{}, 0.0, Interpolation::EaseOut}, {s(1), 0.25}};
    m.set("transform.x", ParamValue(x));
    m.set("feather", 0.05);
    auto withMask = edit::addMask(tl.state, tl.seq, clip, {}, m);
    REQUIRE(withMask);
    Redaction r;
    r.media = tl.media('A');
    r.name = "Face";
    r.source = {s(0.1), s(0.5)};
    r.region = pathMask();
    r.style = RedactionStyle::Solid;
    r.colorR = 1.0;
    r.audio = RedactionAudio::Mute;
    r.audioRanges = {{s(0.1), s(0.1)}, {s(0.3), s(0.05)}};
    r.exceptions = {ClipId{3}};
    auto withBoth = edit::addRedaction(withMask->state, r);
    REQUIRE(withBoth);
    const auto json = io::toJson(withBoth->state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == withBoth->state);
    CHECK(io::toJson(*loaded).value() == *json);

    // Untrusted input: too many vertices, an unknown shape, a broken redaction.
    std::string tampered = *json;
    const auto shapeAt = tampered.find("\"shape\": \"path\"");
    REQUIRE(shapeAt != std::string::npos);
    tampered.replace(shapeAt, std::string("\"shape\": \"path\"").size(), "\"shape\": \"star\"");
    CHECK_FALSE(io::fromJson(tampered));
    std::string noRange = *json;
    const auto at = noRange.find("\"duration\": " + std::to_string(s(0.5).value));
    REQUIRE(at != std::string::npos);
    noRange.replace(at, std::string("\"duration\": " + std::to_string(s(0.5).value)).size(), "\"duration\": 0");
    CHECK_FALSE(io::fromJson(noRange));
}

TEST_CASE("Redactions are shared between snapshots until one changes; the undo budget counts them only then (C.10)",
          "[model][masks]") {
    // A tracked redaction holds ~420 KB of keys per tracked minute; twenty of them used to be copied by every edit and
    // undo step (the C.10 report). Snapshots now share the list until a redaction changes.
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    ProjectState state = tl.state;
    for (int i = 0; i < 5; ++i) {
        Redaction r;
        r.media = tl.media('A');
        r.name = "Plate " + std::to_string(i);
        r.source = {f(i), f(1)};
        r.region.shape = MaskShape::Rectangle;
        r.region.set("size.x", 0.2);
        r.region.set("size.y", 0.2);
        auto added = edit::addRedaction(state, r);
        REQUIRE(added);
        state = std::move(added->state);
    }
    const ProjectState before = state;
    const ProjectState trimmed = ok(edit::trimEdge(before, tl.seq, tl.clip("V1@0"), edit::Edge::Tail, -f(1))).state;
    CHECK(trimmed.redactions.sharesWith(before.redactions));
    // Changing one copies the list: the earlier snapshot keeps its own.
    const auto excepted = edit::setRedactionOverride(trimmed, before.redactions[2].id, tl.clip("V1@0"), false);
    REQUIRE(excepted);
    CHECK_FALSE(excepted->redactions.sharesWith(trimmed.redactions));
    CHECK(trimmed.redactions[2].exceptions.empty());
    CHECK_FALSE(excepted->redactions[2].exceptions.empty());
    CHECK(excepted->redactions.size() == 5);
    // The undo budget: a step that left them alone keeps no copy of them; one that changed them counts them.
    const ProjectState same = trimmed;
    const std::size_t unchanged = estimateExclusiveBytes(trimmed, same);
    CHECK(unchanged < 5 * sizeof(Redaction));
    CHECK(estimateExclusiveBytes(trimmed, *excepted) >= unchanged + 5 * sizeof(Redaction));
    // Equal contents compare equal whether shared or not.
    ProjectState copy = before;
    copy.redactions = std::vector<Redaction>(before.redactions.begin(), before.redactions.end());
    CHECK_FALSE(copy.redactions.sharesWith(before.redactions));
    CHECK(copy == before);
}

TEST_CASE("Matte masks (AI-14): a matte track with its bounding box; saved as schema 12; malformed ones refused", "[model][masks][io]") {
    Timeline tl{R"(
        V1 tgt |Aaaa|
    )"};
    Mask m;
    m.shape = MaskShape::Matte;
    m.name = "Person";
    m.set("center.x", 0.4);
    m.set("center.y", 0.5);
    m.set("size.x", 0.3);
    m.set("size.y", 0.6);
    m.set("feather", 0.01);
    m.set("expansion", -0.005); // a choke
    m.matte = MaskMatte{"0123456789abcdef/0/matte/sam2.1-tiny-video@aa7a8542-1122334455667788", tl.media('A'), 0, {s(0.0), s(2.0)},
                        "sam2.1-tiny-video@aa7a8542"};
    CHECK_FALSE(maskProblem(m));
    CHECK(toString(MaskShape::Matte) == "matte");
    CHECK(maskShapeFromString("matte") == MaskShape::Matte);
    // Without a matte the shape is its bounding box (what a frame without a matte falls back to).
    const auto box = maskShapeAt(m, Ticks{}, 16.0 / 9.0);
    REQUIRE(box.size() == 4);
    CHECK(box[0].x == Approx(0.25));
    CHECK(box[0].y == Approx(0.2));
    CHECK(box[2].x == Approx(0.55));
    CHECK(box[2].y == Approx(0.8));

    // The rules: a matte mask has its track and its bounding box; other shapes have no track.
    Mask noTrack = m;
    noTrack.matte.reset();
    CHECK(maskProblem(noTrack));
    Mask strayTrack = pathMask();
    strayTrack.matte = m.matte;
    CHECK(maskProblem(strayTrack));
    Mask noBox = m;
    noBox.params.erase(std::remove_if(noBox.params.begin(), noBox.params.end(), [](const MaskParam& p) { return p.key == "size.x"; }),
                       noBox.params.end());
    CHECK(maskProblem(noBox).value_or("").find("bounding box") != std::string::npos);
    Mask longTrack = m;
    longTrack.matte->track = std::string(kMaxMatteTrack + 1, 'x');
    CHECK(maskProblem(longTrack));

    // Saved and read back exactly (schema 12 or later: C.13 is 13); an older build says "newer version".
    auto added = edit::addMask(tl.state, tl.seq, tl.clip("V1@0"), {}, m);
    REQUIRE(added);
    const auto json = io::toJson(added->state);
    REQUIRE(json);
    const std::string current = "\"schemaVersion\": " + std::to_string(io::kSchemaVersion);
    CHECK(io::kSchemaVersion >= 12);
    CHECK(json->find(current) != std::string::npos);
    CHECK(json->find("\"shape\": \"matte\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    INFO((loaded ? std::string() : loaded.error().message));
    REQUIRE(loaded);
    CHECK(*loaded == added->state);
    std::string future = *json;
    future.replace(future.find(current), current.size(), "\"schemaVersion\": " + std::to_string(io::kSchemaVersion + 1));
    const auto refused = io::fromJson(future);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("newer version") != std::string::npos);
    // A matte without its track does not load.
    std::string broken = *json;
    const auto at = broken.find("\"matte\": {");
    REQUIRE(at != std::string::npos);
    broken.replace(at, std::string("\"matte\": {").size(), "\"matteX\": {");
    CHECK_FALSE(io::fromJson(broken));
}

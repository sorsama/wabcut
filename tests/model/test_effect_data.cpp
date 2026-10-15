// Effect data (curve points, asset references) and project assets (I.5, docs/color.md): the additive model extension
// of the colour grade. Bounds on untrusted data, typed asset ops, dangling references as warnings, IO round trips and
// byte-identical saves of projects that use neither.

#include "TimelineFixture.h"

#include <wabcut/model/ChangeSet.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <set>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::test;

namespace {

EffectData points(std::string key, std::vector<EffectPoint> pts) { return EffectData{std::move(key), std::move(pts)}; }
EffectData text(std::string key, std::string value) { return EffectData{std::move(key), std::move(value)}; }

const Clip& clipIn(const ProjectState& state, SequenceId seq, std::string_view ref) {
    return state.findSequence(seq)->findClip(clipAt(state, seq, ref))->clip();
}

MediaLocator lutLocator(std::string path) {
    MediaLocator l;
    l.path = std::move(path);
    l.fingerprint = SourceFingerprint{1234, 0, "00112233aabbccdd"};
    return l;
}

/// A timeline whose clip V1@0 has a grade.
struct Graded {
    Timeline tl{R"(
        V1 tgt |Aaaa|
    )"};
    ProjectState state;
    ClipId clip;
    EffectInstanceId grade;

    Graded() {
        clip = tl.clip("V1@0");
        const std::vector<ClipId> clips{clip};
        auto added = edit::addEffect(tl.state, tl.seq, clips, "wabcut.color.grade");
        REQUIRE(added);
        state = added->state;
        grade = added->createdEffects.at(0);
    }
};

} // namespace

TEST_CASE("Effect data: sorted entries, removal by empty value, bounds", "[model][effects][color]") {
    EffectInstance e;
    e.setData(points("curves.red", {{0, 0}, {0.5, 0.6}, {1, 1}}));
    e.setData(text("creative.look", "builtin:teal-orange"));
    e.setData(points("curves.blue", {{0, 0.1}, {1, 0.9}}));
    REQUIRE(e.data.size() == 3);
    CHECK(e.data[0].key == "creative.look");
    CHECK(e.data[1].key == "curves.blue");
    CHECK(e.data[2].key == "curves.red");
    REQUIRE(e.findData("curves.red"));
    CHECK(e.findData("curves.red")->points()->at(1) == EffectPoint{0.5, 0.6});
    CHECK(*e.findData("creative.look")->text() == "builtin:teal-orange");
    CHECK_FALSE(e.findData("curves.green"));
    e.setData(points("curves.red", {}));
    e.setData(text("creative.look", ""));
    CHECK(e.data.size() == 1);
    e.setData(text("never.there", "")); // removing a missing key is a no-op
    CHECK(e.data.size() == 1);

    CHECK_FALSE(effectDataProblem(points("curves.red", {{0, 0}, {1, 1}})));
    CHECK(effectDataProblem(points("", {{0, 0}})));
    CHECK(effectDataProblem(points("curves.red", {{0.5, 0}, {0.5, 1}})));  // x must strictly increase
    CHECK(effectDataProblem(points("curves.red", {{0.6, 0}, {0.5, 1}})));
    CHECK(effectDataProblem(points("curves.red", {{0, std::numeric_limits<double>::quiet_NaN()}})));
    CHECK(effectDataProblem(points("curves.red", {{0, 0}, {std::numeric_limits<double>::infinity(), 1}})));
    std::vector<EffectPoint> many;
    for (std::size_t i = 0; i <= kMaxEffectPoints; ++i) {
        many.push_back({static_cast<double>(i), 0.0});
    }
    CHECK(effectDataProblem(points("curves.red", many)));
    many.pop_back();
    CHECK_FALSE(effectDataProblem(points("curves.red", many)));
    CHECK(effectDataProblem(text("creative.look", std::string(kMaxEffectText + 1, 'x'))));
    CHECK_FALSE(effectDataProblem(text("creative.look", std::string(kMaxEffectText, 'x'))));
}

TEST_CASE("The grade descriptor: stage switches, keyframable scalars", "[model][effects][color]") {
    const EffectDescriptor* d = EffectRegistry::builtin().find("wabcut.color.grade");
    REQUIRE(d);
    CHECK(d->kind == EffectKind::Video);
    CHECK_FALSE(d->intrinsic);
    CHECK(d->params.size() == 62);
    std::set<std::string_view> keys;
    for (const EffectParamInfo& p : d->params) {
        CHECK_FALSE(p.key.empty());
        CHECK(keys.insert(p.key).second);
        CHECK(p.minValue <= p.defaultValue);
        CHECK(p.defaultValue <= p.maxValue);
        if (p.key.ends_with(".enabled") || p.key == "hsl.invert") {
            CHECK_FALSE(p.animatable);
        }
    }
    CHECK(d->param("basic.exposure")->animatable);
    CHECK(d->param("wheels.gain.red")->defaultValue == 1.0);
    CHECK(d->param("basic.saturation")->defaultValue == 100.0);
}

TEST_CASE("setEffect sets grade data in one undoable step", "[model][effects][color]") {
    Graded g;
    EditSession session(g.state);
    edit::EffectChange change;
    change.data = {points("curves.master", {{0, 0}, {0.25, 0.2}, {0.75, 0.82}, {1, 1}}), text("creative.look", "builtin:warm-film")};
    change.params = {{"basic.exposure", ParamValue(0.5)}};
    const auto result = session.apply("Grade", [&](const ProjectState& s) { return edit::setEffect(s, g.tl.seq, g.clip, g.grade, change); });
    REQUIRE(result);
    const SequenceChange* seqChange = result->changes.sequence(g.tl.seq);
    REQUIRE(seqChange);
    CHECK(seqChange->roles.has(PropertyRole::Effects));
    const EffectInstance& e = clipIn(*session.snapshot(), g.tl.seq, "V1@0").effects().at(0);
    CHECK(e.data.size() == 2);
    CHECK(e.findData("curves.master")->points()->size() == 4);
    CHECK(effectParameterAt(e, "basic.exposure", Ticks{}) == 0.5);
    requireValid(*session.snapshot());

    // Data only (no params) is a real change too; an empty value clears.
    edit::EffectChange clear;
    clear.data = {text("creative.look", "")};
    REQUIRE(session.apply("Clear look", [&](const ProjectState& s) { return edit::setEffect(s, g.tl.seq, g.clip, g.grade, clear); }));
    CHECK(clipIn(*session.snapshot(), g.tl.seq, "V1@0").effects().at(0).data.size() == 1);
    REQUIRE(session.undo());
    CHECK(clipIn(*session.snapshot(), g.tl.seq, "V1@0").effects().at(0).data.size() == 2);
    REQUIRE(session.undo());
    CHECK(clipIn(*session.snapshot(), g.tl.seq, "V1@0").effects().at(0).data.empty());
    REQUIRE(session.redo());
    CHECK(clipIn(*session.snapshot(), g.tl.seq, "V1@0").effects().at(0).data.size() == 2);

    // Malformed data is refused and changes nothing.
    edit::EffectChange bad;
    bad.data = {points("curves.red", {{0.5, 0}, {0.2, 1}})};
    CHECK_FALSE(edit::setEffect(*session.snapshot(), g.tl.seq, g.clip, g.grade, bad));
    // Unknown effects keep their data as they are.
    ProjectState unknown = *session.snapshot();
    {
        auto seq = std::make_shared<Sequence>(*unknown.findSequence(g.tl.seq));
        auto track = std::make_shared<Track>(*seq->videoTracks[0]);
        EffectStack stack = track->clips[0].effects();
        stack[0].effect = "vendor.grade";
        track->clips.mutableAt(0).setEffects(stack);
        seq->videoTracks[0] = track;
        unknown = unknown.withSequence(seq);
    }
    CHECK_FALSE(edit::setEffect(unknown, g.tl.seq, g.clip, g.grade, clear));
}

TEST_CASE("Effect stacks with malformed data are invalid; loading rejects them", "[model][effects][color][io]") {
    Graded g;
    const auto withData = [&](std::vector<EffectData> data) {
        ProjectState s = g.state;
        auto seq = std::make_shared<Sequence>(*s.findSequence(g.tl.seq));
        auto track = std::make_shared<Track>(*seq->videoTracks[0]);
        EffectStack stack = track->clips[0].effects();
        stack[0].data = std::move(data);
        track->clips.mutableAt(0).setEffects(stack);
        seq->videoTracks[0] = track;
        return s.withSequence(seq);
    };
    CHECK(validate(withData({points("curves.red", {{0, 0}, {1, 1}})})));
    CHECK_FALSE(validate(withData({points("curves.red", {{0, 0}}), points("curves.blue", {{0, 0}})}))); // unsorted keys
    CHECK_FALSE(validate(withData({points("curves.red", {{0.5, 0}, {0.5, 1}})})));
    std::vector<EffectData> tooMany;
    for (std::size_t i = 0; i <= kMaxEffectDataEntries; ++i) {
        tooMany.push_back(text("k" + std::to_string(1000 + i), "x"));
    }
    CHECK_FALSE(validate(withData(tooMany)));

    // IO: a good file round-trips; hand-edited bad data fails the load with its path.
    const ProjectState good = withData({text("creative.look", "asset:99"), points("curves.red", {{0, 0}, {0.5, 0.25}, {1, 1}})});
    requireValid(good);
    const auto json = io::toJson(good);
    REQUIRE(json);
    CHECK(json->find("\"data\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == good);
    const auto broken = [&](std::string_view from, std::string_view to) {
        std::string text = *json;
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
        return io::fromJson(text);
    };
    // x not increasing
    const auto unsorted = broken("0.5,\n", "1.5,\n");
    CHECK_FALSE(unsorted);
    if (!unsorted) {
        CHECK(unsorted.error().message.find("data") != std::string::npos);
    }
    CHECK_FALSE(broken("\"asset:99\"", std::string("\"") + std::string(kMaxEffectText + 1, 'x') + "\""));
    CHECK_FALSE(broken("\"text\": \"asset:99\"", "\"text\": 5"));
}

TEST_CASE("Assets: add, relink, remove, references", "[model][color][assets]") {
    Graded g;
    auto added = edit::addAsset(g.state, ProjectAsset{{}, std::string(kAssetKindLut), "Teal.cube", lutLocator("C:/luts/Teal.cube")});
    REQUIRE(added);
    const AssetId lut = added->id;
    CHECK(lut.isValid());
    CHECK(added->state.findAsset(lut));
    CHECK(added->state.findAsset(lut)->name == "Teal.cube");
    requireValid(added->state);
    CHECK_FALSE(edit::addAsset(g.state, ProjectAsset{{}, "", "x", lutLocator("C:/x.cube")}));
    CHECK_FALSE(edit::addAsset(g.state, ProjectAsset{{}, "lut", "x", MediaLocator{}}));

    CHECK(assetReference(lut) == "asset:" + std::to_string(lut.value));
    CHECK(assetOfReference(assetReference(lut)) == lut);
    CHECK_FALSE(assetOfReference("asset:").isValid());
    CHECK_FALSE(assetOfReference("asset:12x").isValid());
    CHECK_FALSE(assetOfReference("builtin:teal").isValid());
    CHECK_FALSE(assetOfReference("asset:99999999999999999999999").isValid());

    // An asset-only change is a real revision with assetsChanged.
    CHECK_FALSE(isSameRevision(g.state, added->state));
    CHECK(diff(g.state, added->state).assetsChanged);
    CHECK_FALSE(diff(g.state, added->state).empty());

    // Relink: a new locator, the same id.
    auto relinked = edit::relinkAsset(added->state, lut, lutLocator("D:/moved/Teal.cube"));
    REQUIRE(relinked);
    CHECK(relinked->findAsset(lut)->locator.path == "D:/moved/Teal.cube");
    CHECK(diff(added->state, *relinked).assetsChanged);
    CHECK_FALSE(edit::relinkAsset(added->state, AssetId{12345}, lutLocator("x")));

    // Referenced by the grade: removal refuses unless the references go too, in the same edit.
    edit::EffectChange use;
    use.data = {text("creative.look", assetReference(lut)), points("curves.red", {{0, 0}, {1, 1}})};
    auto used = edit::setEffect(*relinked, g.tl.seq, g.clip, g.grade, use);
    REQUIRE(used);
    const auto refused = edit::removeAsset(used->state, lut);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().message.find("Teal.cube") != std::string::npos);
    auto removed = edit::removeAsset(used->state, lut, true);
    REQUIRE(removed);
    CHECK_FALSE(removed->findAsset(lut));
    const EffectInstance& after = clipIn(*removed, g.tl.seq, "V1@0").effects().at(0);
    CHECK_FALSE(after.findData("creative.look"));
    CHECK(after.findData("curves.red")); // other data stays
    requireValid(*removed);
    // Unused: removed directly.
    CHECK(edit::removeAsset(*relinked, lut));

    // Validation: sorted unique ids below the generator, with a kind.
    ProjectState bad = added->state;
    bad.assets.push_back(bad.assets.front());
    CHECK_FALSE(validate(bad));
    bad = added->state;
    bad.assets.front().kind.clear();
    CHECK_FALSE(validate(bad));
    bad = added->state;
    bad.assets.front().id = AssetId{bad.ids.next + 5};
    CHECK_FALSE(validate(bad));
}

TEST_CASE("Dangling asset references load and validate; they are warnings", "[model][color][assets][io]") {
    Graded g;
    edit::EffectChange use;
    use.data = {text("basic.inputLut", "asset:777")};
    auto used = edit::setEffect(g.state, g.tl.seq, g.clip, g.grade, use);
    REQUIRE(used);
    requireValid(used->state);
    const auto warnings = assetReferenceWarnings(used->state);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].find("asset:777") != std::string::npos);
    CHECK(warnings[0].find("basic.inputLut") != std::string::npos);
    const auto json = io::toJson(used->state);
    REQUIRE(json);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(assetReferenceWarnings(*loaded).size() == 1);
}

TEST_CASE("Assets and data round-trip; projects without them save byte-identical", "[model][color][assets][io]") {
    Graded g;
    const auto plain = io::toJson(g.state);
    REQUIRE(plain);
    CHECK(plain->find("\"assets\"") == std::string::npos);
    CHECK(plain->find("\"data\"") == std::string::npos);
    // The same project saved before this extension existed: no new keys, same bytes after a reload.
    const auto reloaded = io::fromJson(*plain);
    REQUIRE(reloaded);
    CHECK(*io::toJson(*reloaded) == *plain);

    auto added = edit::addAsset(g.state, ProjectAsset{{}, "lut", "Look.cube", lutLocator("C:/luts/Look.cube")});
    REQUIRE(added);
    ProjectState s = added->state;
    s.assets.front().locator.relativePath = "luts/Look.cube";
    s.assets.front().locator.offline = true;
    edit::EffectChange use;
    use.data = {text("creative.look", assetReference(added->id)), points("curves.hueSat", {{0, 0.5}, {0.33, 0.8}, {0.66, 0.5}})};
    auto used = edit::setEffect(s, g.tl.seq, g.clip, g.grade, use);
    REQUIRE(used);
    const auto json = io::toJson(used->state);
    REQUIRE(json);
    CHECK(json->find("\"assets\"") != std::string::npos);
    const auto loaded = io::fromJson(*json);
    REQUIRE(loaded);
    CHECK(*loaded == used->state);
    CHECK(loaded->assets.front().locator.relativePath == "luts/Look.cube");
    CHECK(loaded->assets.front().locator.offline);
    CHECK(*io::toJson(*loaded) == *json);
}

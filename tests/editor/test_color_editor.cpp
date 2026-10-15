// I.5 in the editor: LUT assets saved with project-relative paths and found again after the project moves, relinked
// by name and fingerprint (one and all), collected into capsules; and the colour commands' pictures (one clip alone,
// its grade's input, its coverage) from the export renderer.

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/color/ColorCommands.h>
#include <wabcut/command/Engine.h>
#include <wabcut/color/Grade.h>
#include <wabcut/color/Looks.h>
#include <wabcut/editor/Capsule.h>
#include <wabcut/editor/ColorFrames.h>
#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/SourceEdit.h>

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>

#include <QGuiApplication>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) { return fs::path(WABCUT_TEST_MEDIA_DIR) / name; }

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void ensureGui() {
    if (!QGuiApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_editor_tests";
        static char* argv[] = {name, nullptr};
        static QGuiApplication app(argc, argv);
    }
}

void writeLut(const fs::path& file, const char* builtin) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << color::writeCube(*color::builtinLut(builtin));
}

AssetId addLut(EditSession& session, const fs::path& file) {
    ProjectAsset asset;
    asset.kind = std::string(kAssetKindLut);
    asset.name = pathToUtf8(file.filename());
    asset.locator.path = pathToUtf8(fs::absolute(file));
    asset.locator.fingerprint = *fingerprintFile(file);
    AssetId id;
    auto r = session.apply("Import LUT", [&](const ProjectState& s) {
        auto created = edit::addAsset(s, asset);
        if (created) {
            id = created->id;
        }
        return created;
    });
    REQUIRE(r);
    return id;
}

/// A sequence with the H.264 clip on V1 (scaled to half size when `half`), and a grade on it.
struct GradedProject {
    SequenceId sequence;
    MediaId media;
    ClipId clip;
    EffectInstanceId grade;
};

GradedProject makeGraded(EditSession& session, ProjectDocument& doc, const fs::path& dir, bool half) {
    GradedProject g;
    fs::create_directories(dir / "media");
    fs::copy_file(testMedia("h264_1080p60_aac.mp4"), dir / "media" / "clip.mp4", fs::copy_options::overwrite_existing);
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    g.sequence = created->id;
    session.reset(std::move(created->state));
    REQUIRE(doc.saveAs(dir / "project.wabcut"));
    const std::vector<fs::path> files{dir / "media" / "clip.mp4"};
    const MediaImportResult imported = importMediaFiles(session, files, {.projectFile = dir / "project.wabcut"});
    REQUIRE(imported.added.size() == 1);
    g.media = imported.added.front();
    auto placed = dropMedia(session, {.media = g.media, .sequence = g.sequence, .at = Ticks{}});
    REQUIRE(placed);
    g.clip = placed->placed.front();
    auto graded = session.apply("Grade", [&](const ProjectState& s) {
        const std::vector<ClipId> clips{g.clip};
        auto r = edit::addEffect(s, g.sequence, clips, std::string(color::kGradeEffect), {{"basic.exposure", ParamValue(1.5)}});
        if (r) {
            g.grade = r->createdEffects.front();
        }
        if (r && half) {
            edit::ClipAttributeChange change;
            ClipProperties p = s.findSequence(g.sequence)->findClip(g.clip)->clip().properties();
            p[ParamId::Scale] = ParamValue(0.5);
            change.properties = p;
            const std::vector<ClipId> one{g.clip};
            return edit::setClipAttributes(r->state, g.sequence, one, change);
        }
        return r;
    });
    REQUIRE(graded);
    REQUIRE(doc.save());
    return g;
}

} // namespace

TEST_CASE("LUT assets: relative paths on save, found after a move, relinked by fingerprint", "[editor][color][lut]") {
    const fs::path dir = workDir("color_lut_relink");
    EditSession session;
    ProjectDocument doc(session);
    auto created = edit::addSequence(ProjectState{}, edit::SequenceSettings{});
    REQUIRE(created);
    session.reset(std::move(created->state));
    fs::create_directories(dir / "project");
    REQUIRE(doc.saveAs(dir / "project" / "project.wabcut"));
    writeLut(dir / "project" / "luts" / "Look.cube", "builtin:warm-film");
    const AssetId lut = addLut(session, dir / "project" / "luts" / "Look.cube");
    REQUIRE(doc.save());
    {
        const auto saved = io::loadProject(dir / "project" / "project.wabcut");
        REQUIRE(saved);
        REQUIRE(saved->findAsset(lut));
        CHECK(saved->findAsset(lut)->locator.relativePath == "luts/Look.cube");
    }
    // The whole project folder moves: the LUT is found through its relative path.
    fs::rename(dir / "project", dir / "moved");
    REQUIRE(doc.open(dir / "moved" / "project.wabcut"));
    REQUIRE(doc.assetStatus().size() == 1);
    CHECK_FALSE(doc.assetStatus().front().offline);
    CHECK(fs::path(pathFromUtf8(doc.assetStatus().front().path)) == fs::absolute(dir / "moved" / "luts" / "Look.cube"));
    // The LUT file moves on its own: offline, then relinked by name and fingerprint.
    fs::create_directories(dir / "elsewhere" / "deep");
    fs::rename(dir / "moved" / "luts" / "Look.cube", dir / "elsewhere" / "deep" / "Look.cube");
    REQUIRE(doc.refreshMediaStatus());
    CHECK(doc.assetStatus().front().offline);
    const auto candidates = doc.findAssetRelinkCandidates(lut, {dir / "elsewhere"});
    REQUIRE(candidates.size() == 1);
    CHECK(candidates.front().fingerprintMatches);
    REQUIRE(doc.relinkAsset(lut, candidates.front().file));
    CHECK_FALSE(doc.assetStatus().front().offline);
    CHECK(session.undoLabel() == "Relink LUT");
    REQUIRE(session.undo());
    CHECK(doc.assetStatus().front().offline);
    REQUIRE(doc.relinkAll({dir / "elsewhere"}));
    CHECK_FALSE(doc.assetStatus().front().offline);
    // A different file is refused unless forced.
    writeLut(dir / "other" / "Look.cube", "builtin:cool-night");
    CHECK_FALSE(doc.relinkAsset(lut, dir / "other" / "Look.cube"));
    CHECK(doc.relinkAsset(lut, dir / "other" / "Look.cube", true));
}

TEST_CASE("Collect Files carries the LUTs a grade uses", "[editor][color][capsule]") {
    if (!fs::exists(testMedia("h264_1080p60_aac.mp4"))) {
        SKIP("test media missing");
    }
    const fs::path dir = workDir("color_capsule");
    EditSession session;
    ProjectDocument doc(session);
    const GradedProject g = makeGraded(session, doc, dir / "project", false);
    writeLut(dir / "project" / "luts" / "Film.cube", "builtin:teal-orange");
    writeLut(dir / "project" / "luts" / "Unused.cube", "builtin:mono-silver");
    const AssetId used = addLut(session, dir / "project" / "luts" / "Film.cube");
    addLut(session, dir / "project" / "luts" / "Unused.cube");
    REQUIRE(session.apply("Look", [&](const ProjectState& s) {
        edit::EffectChange change;
        change.data = {EffectData{std::string(color::gradekey::look), assetReference(used)}};
        return edit::setEffect(s, g.sequence, g.clip, g.grade, change);
    }));
    REQUIRE(doc.save());
    CapsuleOptions options;
    options.destination = dir / "capsules";
    options.name = "Graded";
    options.verify = true;
    const auto result = collectProject(*session.snapshot(), doc.file(), options, {}, {});
    REQUIRE(result);
    INFO(result->notes.size());
    CHECK(result->lutFiles == 1); // the unused LUT stays behind (unused files are left out by default)
    CHECK(result->verified);
    CHECK(result->offlineAfterReopen == 0);
    CHECK(std::ranges::count_if(result->files, [](const CapsuleFile& f) { return f.role == "lut"; }) == 1);
    CHECK(fs::exists(dir / "capsules" / "Graded" / "LUTs" / "Film.cube"));
    const auto capsule = io::loadProject(result->projectFile);
    REQUIRE(capsule);
    REQUIRE(capsule->findAsset(used));
    CHECK(capsule->findAsset(used)->locator.relativePath == "LUTs/Film.cube");
    CHECK(capsule->assets.size() == 1);
}

TEST_CASE("Colour frames: a clip alone, before its grade, with its coverage", "[editor][color][gpu]") {
    if (!fs::exists(testMedia("h264_1080p60_aac.mp4"))) {
        SKIP("test media missing");
    }
    ensureGui();
    const fs::path dir = workDir("color_frames");
    EditSession session;
    ProjectDocument doc(session);
    const GradedProject g = makeGraded(session, doc, dir, true);
    ColorFrameService frames(doc.file());
    const Ticks at = Ticks::fromSeconds(1);
    const auto graded = frames.frame(*session.snapshot(), g.sequence, color::FrameRequest{at, g.clip, {}, 320});
    const auto input = frames.frame(*session.snapshot(), g.sequence, color::FrameRequest{at, g.clip, g.grade, 320});
    const auto whole = frames.frame(*session.snapshot(), g.sequence, color::FrameRequest{at, {}, {}, 320});
    REQUIRE(graded);
    REQUIRE(input);
    REQUIRE(whole);
    CHECK(graded->width == 320);
    // Coverage: the clip at half size covers a quarter of the canvas.
    double covered = 0.0;
    for (std::size_t i = 3; i < graded->rgba.size(); i += 4) {
        covered += graded->rgba[i];
    }
    covered /= static_cast<double>(graded->width) * graded->height;
    CHECK(covered > 0.2);
    CHECK(covered < 0.3);
    // The grade's input is darker than the graded picture (+1.5 stops), inside the clip.
    const auto meanLuma = [](const color::ColorImage& im) {
        double sum = 0, n = 0;
        for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
            if (im.rgba[i + 3] > 0.5f) {
                sum += 0.2126 * im.rgba[i] + 0.7152 * im.rgba[i + 1] + 0.0722 * im.rgba[i + 2];
                ++n;
            }
        }
        return n > 0 ? sum / n : 0.0;
    };
    CHECK(meanLuma(*graded) > meanLuma(*input) + 0.05);
    // The whole sequence counts every pixel.
    for (std::size_t i = 3; i < whole->rgba.size(); i += 4) {
        REQUIRE(whole->rgba[i] == 1.0f);
    }
    // soloClipState keeps only the clip, with the stack cut before the grade.
    const ProjectState solo = soloClipState(*session.snapshot(), g.sequence, g.clip, g.grade);
    CHECK(solo.findSequence(g.sequence)->findClip(g.clip)->clip().effects().empty());
}

TEST_CASE("Colour on a multicam clip reads the active angle through the clip (0.1.2)", "[editor][color][gpu][multicam]") {
    if (!fs::exists(testMedia("h264_1080p60_aac.mp4"))) {
        SKIP("test media missing");
    }
    ensureGui();
    const fs::path dir = workDir("color_multicam");
    EditSession session;
    ProjectDocument doc(session);
    const GradedProject g = makeGraded(session, doc, dir, false);
    // A one-angle multicam source of the same media, edited in after the graded clip.
    REQUIRE(session.apply("Multicam", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        edit::MulticamSpec spec;
        spec.angles = {g.media};
        spec.sync = MulticamSync::Manual;
        auto mc = edit::createMulticam(s, spec);
        if (!mc) {
            return std::unexpected(mc.error());
        }
        const Sequence& outer = *mc->state.findSequence(g.sequence);
        edit::SourceClip src;
        src.sequence = mc->id;
        src.sourceRange = TimeRange::fromStartEnd(Ticks{}, Ticks::fromSeconds(2));
        src.patches = edit::defaultSourcePatches(outer, *mc->state.findSequence(mc->id));
        src.videoAngle = AngleSet::of(0);
        src.audioAngles = AngleSet::of(0);
        auto placed = edit::overwriteEdit(mc->state, g.sequence, src, outer.duration);
        if (!placed) {
            return std::unexpected(placed.error());
        }
        return std::move(placed->state);
    }));
    ClipId multicamClip;
    for (const Clip& c : session.snapshot()->findSequence(g.sequence)->videoTracks.front()->clips) {
        if (c.isNested()) {
            multicamClip = c.id;
        }
    }
    REQUIRE(multicamClip.isValid());
    const Ticks at = session.snapshot()->findSequence(g.sequence)->findClip(multicamClip)->clip().start() + Ticks::fromSeconds(1);
    const auto meanLuma = [](const color::ColorImage& im) {
        double sum = 0, n = 0;
        for (std::size_t i = 0; i < im.rgba.size(); i += 4) {
            if (im.rgba[i + 3] > 0.5f) {
                sum += 0.2126 * im.rgba[i] + 0.7152 * im.rgba[i + 1] + 0.0722 * im.rgba[i + 2];
                ++n;
            }
        }
        return n > 0 ? sum / n : 0.0;
    };

    // The clip alone: the angle's picture fills its frame (the multicam sequence's canvas), so it counts as picture.
    ColorFrameService frames(doc.file());
    const auto shown = frames.frame(*session.snapshot(), g.sequence, color::FrameRequest{at, multicamClip, {}, 320});
    REQUIRE(shown);
    double covered = 0.0;
    for (std::size_t i = 3; i < shown->rgba.size(); i += 4) {
        covered += shown->rgba[i];
    }
    covered /= static_cast<double>(shown->width) * shown->height;
    CHECK(covered > 0.9);
    CHECK(meanLuma(*shown) > 0.02);

    // color.auto grades the multicam clip itself (it used to refuse: "the clip shows nothing at that time").
    command::EnginePolicy policy;
    policy.services = std::make_shared<ColorFrameService>(doc.file());
    command::CommandEngine engine(session, ai::edit::editingCommands(), policy);
    const auto run = [&](command::Json document) {
        const auto result = engine.executeJson(document);
        INFO(command::toJson(result).dump());
        REQUIRE(result.ok());
        return result;
    };
    run({{"command", "color.auto"}, {"params", {{"sequence", g.sequence.value}, {"clip", multicamClip.value}}}});
    const Clip& autoed = session.snapshot()->findSequence(g.sequence)->findClip(multicamClip)->clip();
    CHECK(autoed.isNested());
    REQUIRE(color::gradeOf(autoed) != nullptr);

    // And the grade really applies to the angle's picture: +1.5 stops is brighter than the grade's input.
    run({{"command", "color.grade.set"},
         {"params", {{"sequence", g.sequence.value}, {"clips", {multicamClip.value}}, {"params", {{"basic.exposure", 1.5}}}}}});
    const Clip& graded = session.snapshot()->findSequence(g.sequence)->findClip(multicamClip)->clip();
    const EffectInstanceId grade = color::gradeOf(graded)->id;
    const auto withGrade = frames.frame(*session.snapshot(), g.sequence, color::FrameRequest{at, multicamClip, {}, 320});
    const auto input = frames.frame(*session.snapshot(), g.sequence, color::FrameRequest{at, multicamClip, grade, 320});
    REQUIRE(withGrade);
    REQUIRE(input);
    CHECK(meanLuma(*withGrade) > meanLuma(*input) + 0.05);
}

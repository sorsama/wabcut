// 0.5 (p0 round, stream C): Scene Edit Detection… (SceneDetectBridge, singleton `SceneDetect`), offscreen.
//   * the bridge on the synchronous switch with shots handed over in memory: a cut at each edit with the linked audio,
//     markers at each, both, one undo step each that one undo reverts; the detector and the inverted Sensitivity reach
//     the analysis; the options are remembered; Cancel changes nothing; the toast text is built from the result data;
//   * which clips it acts on and why not (no selection, sound only, a locked track), for the menus and the palette;
//   * in the shell: the Clip menu row and the clip menu's row (greyed out with the reason without video), the dialog it
//     opens, a run from the dialog and its toast; screenshots of the dialog, dark and light.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "CommandPalette.h"
#include "ProjectController.h"
#include "SceneDetectBridge.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/command/Command.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQuickItem>
#include <QSettings>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace an = wabcut::ai::analysis;

namespace {

/// Shots handed over in memory by media id (source time), like MapAnalysisProvider, that also records what it was
/// asked and reports progress (so a cancel requested in `during` stops the command at that checkpoint).
class TestShots final : public an::AnalysisProvider {
public:
    std::map<std::uint64_t, std::vector<std::int64_t>> cutFrames; ///< media id -> source frames that start a shot
    std::vector<an::ShotOptions> asked;
    std::function<void()> during;

    std::expected<an::ShotAnalysis, Error> shots(const MediaItem& media, int, const an::ShotOptions& options,
                                                 const an::AnalysisProgress& progress) override {
        asked.push_back(options);
        if (during)
            during();
        if (progress && !progress(0.5))
            return std::unexpected(Error{wabcut::ErrorCode::Cancelled, "cancelled"});
        an::ShotAnalysis analysis;
        analysis.frames = 1000;
        for (const std::int64_t frame : cutFrames[media.id.value])
            analysis.cuts.push_back(an::ShotCut{f(frame), frame, an::CutKind::Cut, 0.9f});
        return analysis;
    }
    std::expected<an::BeatAnalysis, Error> beats(const MediaItem& media, int, const an::BeatOptions&,
                                                 const an::AnalysisProgress&) override {
        return std::unexpected(Error{wabcut::ErrorCode::NotFound, "no beats for '" + media.name + "'"});
    }
};

/// A on V1 and A1 (linked, 100 frames from its frame 0), B on V1 after it (40 frames), C on A2 alone (sound only).
std::string picture(const char* v1Flags = "tgt") {
    return std::string("V1 ") + v1Flags + " |A" + std::string(99, 'a') + "B" + std::string(39, 'b') + "|\n" +
           "A1 tgt |A" + std::string(99, 'a') + std::string(40, '.') + "|\n" +
           "A2     |" + std::string(10, '.') + "C" + std::string(19, 'c') + std::string(110, '.') + "|";
}

struct SceneRig {
    Timeline tl;
    ProjectController controller;
    SceneDetectBridge scenes;
    std::shared_ptr<TestShots> shots = std::make_shared<TestShots>();

    explicit SceneRig(const std::string& text = picture(), bool link = true) : tl(text) {
        if (link)
            tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        scenes.setController(&controller);
        scenes.setServices(std::make_shared<an::AnalysisServices>(shots));
        scenes.setSynchronous(true);
        // A cuts at its frames 30 and 70; B at its frame 20.
        shots->cutFrames[tl.media('A').value] = {30, 70};
        shots->cutFrames[tl.media('B').value] = {20};
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const Sequence& seq() const { return *controller.currentSequence(); }
    std::size_t clipsOn(std::string_view track) const { return seq().findTrack(tl.track(track))->clips.size(); }
    QString key(std::string_view ref) const { return clipKey(tl.clip(ref)); }
    std::vector<std::string> markerNames() const {
        std::vector<std::string> out;
        for (const Marker& m : seq().markers)
            out.push_back(m.name);
        return out;
    }
};

void forgetOptions() {
    QSettings().remove(QStringLiteral("sceneDetect"));
}

/// `state` with an empty second sequence "Other" (its id in `other`).
ProjectState withOtherSequence(const ProjectState& state, SequenceId* other) {
    edit::SequenceSettings settings;
    settings.name = "Other";
    settings.frameRate = kRate;
    auto created = edit::addSequence(state, settings);
    REQUIRE(created);
    *other = created->id;
    return std::move(created->state);
}

/// Makes `id` the current sequence.
void showSequence(ProjectController& controller, SequenceId id) {
    const ProjectSnapshot snap = controller.snapshot();
    for (std::size_t i = 0; i < snap->sequences.size(); ++i) {
        if (snap->sequences[i]->id == id)
            controller.setCurrentSequenceIndex(static_cast<int>(i));
    }
    REQUIRE(controller.currentSequenceId() == id);
}

/// The finished() signals of a bridge: { ok, cancelled, message } each.
struct FinishedLog {
    struct Call {
        bool ok = false;
        bool cancelled = false;
        QString message;
    };
    std::vector<Call> calls;
    explicit FinishedLog(SceneDetectBridge& scenes) {
        QObject::connect(&scenes, &SceneDetectBridge::finished, &scenes,
                         [this](bool ok, bool cancelled, const QString& message) { calls.push_back({ok, cancelled, message}); });
    }
};

} // namespace

TEST_CASE("Scene Edit Detection cuts at each edit with the linked audio in one undo step", "[app][scenedetect]") {
    resetSettings();
    forgetOptions();
    SceneRig r;
    FinishedLog finished(r.scenes);
    const std::size_t before = r.undo();
    REQUIRE(r.scenes.run({r.key("V1@0")}, {{QStringLiteral("cuts"), true}, {QStringLiteral("markers"), false}}).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Scene Edit Detection"));
    // A is razored at 30 and 70, its linked audio too; B is untouched; no markers.
    REQUIRE(r.clipsOn("V1") == 4);
    CHECK(r.clipsOn("A1") == 3);
    CHECK(clipAt(*r.controller.snapshot(), r.tl.seq, "V1@30").isValid());
    CHECK(clipAt(*r.controller.snapshot(), r.tl.seq, "V1@70").isValid());
    CHECK(clipAt(*r.controller.snapshot(), r.tl.seq, "A1@30").isValid());
    CHECK(r.seq().markers.empty());
    // The default detector and Sensitivity 50: the detector's own threshold.
    REQUIRE(r.shots->asked.size() == 1);
    CHECK(r.shots->asked[0].detector == an::ShotDetector::Combined);
    CHECK(r.shots->asked[0].contentThreshold == Catch::Approx(27.0));
    // The toast from the result data.
    REQUIRE(finished.calls.size() == 1);
    CHECK(finished.calls[0].ok);
    CHECK_FALSE(finished.calls[0].cancelled);
    CHECK(finished.calls[0].message == QStringLiteral("Split 1 clip at 2 cuts"));
    CHECK(r.scenes.message() == QStringLiteral("Split 1 clip at 2 cuts"));
    CHECK_FALSE(r.scenes.busy());

    REQUIRE(r.controller.undo());
    CHECK(r.clipsOn("V1") == 2);
    CHECK(r.clipsOn("A1") == 1);
}

TEST_CASE("Scene Edit Detection adds markers at each cut without cutting in one undo step", "[app][scenedetect]") {
    resetSettings();
    forgetOptions();
    SceneRig r;
    const std::size_t before = r.undo();
    REQUIRE(r.scenes.run({r.key("V1@0"), r.key("V1@100")},
                         {{QStringLiteral("cuts"), false}, {QStringLiteral("markers"), true}})
                .isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Scene Edit Detection"));
    CHECK(r.clipsOn("V1") == 2); // nothing razored
    // Shot markers at A's two cuts and B's one (sequence markers, cyan), in one step.
    REQUIRE(r.seq().markers.size() == 3);
    CHECK(r.markerNames() == std::vector<std::string>{"Shot 2", "Shot 3", "Shot 2"});
    CHECK(r.seq().markers[0].start == f(30));
    CHECK(r.seq().markers[1].start == f(70));
    CHECK(r.seq().markers[2].start == f(120));
    CHECK(r.scenes.message() == QStringLiteral("Added 3 markers at scene edits in 2 clips"));
    REQUIRE(r.controller.undo());
    CHECK(r.seq().markers.empty());
}

TEST_CASE("Scene Edit Detection with cuts and markers is still one undo step", "[app][scenedetect]") {
    resetSettings();
    forgetOptions();
    SceneRig r;
    const std::size_t before = r.undo();
    REQUIRE(r.scenes.run({r.key("V1@0"), r.key("V1@100")},
                         {{QStringLiteral("cuts"), true}, {QStringLiteral("markers"), true}})
                .isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.clipsOn("V1") == 5);
    CHECK(r.seq().markers.size() == 3);
    CHECK(r.scenes.message() == QStringLiteral("Split 2 clips at 3 cuts and added 3 markers"));
    REQUIRE(r.controller.undo());
    CHECK(r.clipsOn("V1") == 2);
    CHECK(r.seq().markers.empty());
}

TEST_CASE("Scene Edit Detection: the inverted Sensitivity and the detector reach the analysis and are remembered",
          "[app][scenedetect]") {
    resetSettings();
    forgetOptions();
    CHECK(SceneDetectBridge::thresholdFor(50) == Catch::Approx(27.0));
    CHECK(SceneDetectBridge::thresholdFor(0) == Catch::Approx(54.0));
    CHECK(SceneDetectBridge::thresholdFor(100) == Catch::Approx(13.5));
    CHECK(SceneDetectBridge::thresholdFor(90) < SceneDetectBridge::thresholdFor(10)); // higher finds more
    {
        SceneRig r;
        // Defaults: cuts on, markers off, combined, 50.
        CHECK(r.scenes.options().value(QStringLiteral("cuts")).toBool());
        CHECK_FALSE(r.scenes.options().value(QStringLiteral("markers")).toBool());
        CHECK(r.scenes.options().value(QStringLiteral("detector")).toString() == QStringLiteral("combined"));
        CHECK(r.scenes.options().value(QStringLiteral("sensitivity")).toDouble() == Catch::Approx(50));
        REQUIRE(r.scenes.run({r.key("V1@0")}, {{QStringLiteral("detector"), QStringLiteral("content")},
                                               {QStringLiteral("sensitivity"), 90}})
                    .isEmpty());
        REQUIRE(r.shots->asked.size() == 1);
        CHECK(r.shots->asked[0].detector == an::ShotDetector::Content);
        CHECK(r.shots->asked[0].contentThreshold == Catch::Approx(SceneDetectBridge::thresholdFor(90)));
        // Fades through black has no content threshold: the detector's own settings.
        REQUIRE(r.controller.undo());
        REQUIRE(r.scenes.run({r.key("V1@0")}, {{QStringLiteral("detector"), QStringLiteral("threshold")}}).isEmpty());
        REQUIRE(r.shots->asked.size() == 2);
        CHECK(r.shots->asked[1].detector == an::ShotDetector::Threshold);
        CHECK(r.shots->asked[1].contentThreshold == Catch::Approx(27.0));
        // Neither cuts nor markers: refused, nothing runs.
        CHECK_FALSE(r.scenes.run({r.key("V1@0")}, {{QStringLiteral("cuts"), false}, {QStringLiteral("markers"), false}})
                        .isEmpty());
        CHECK(r.shots->asked.size() == 2);
        r.scenes.setOption(QStringLiteral("cuts"), true);
        r.scenes.setOption(QStringLiteral("detector"), QStringLiteral("nonsense")); // ignored
    }
    SceneDetectBridge again;
    CHECK(again.options().value(QStringLiteral("detector")).toString() == QStringLiteral("threshold"));
    CHECK(again.options().value(QStringLiteral("sensitivity")).toDouble() == Catch::Approx(90));
    CHECK(again.options().value(QStringLiteral("cuts")).toBool());
    CHECK_FALSE(again.options().value(QStringLiteral("markers")).toBool());
    forgetOptions();
}

TEST_CASE("Scene Edit Detection: Cancel stops it and changes nothing", "[app][scenedetect]") {
    resetSettings();
    forgetOptions();
    SceneRig r;
    FinishedLog finished(r.scenes);
    r.shots->during = [&r] { r.scenes.cancel(); }; // the Cancel button while the analysis runs
    const std::size_t before = r.undo();
    REQUIRE(r.scenes.run({r.key("V1@0")}, {{QStringLiteral("markers"), true}}).isEmpty());
    CHECK(r.undo() == before);
    CHECK(r.clipsOn("V1") == 2);
    CHECK(r.seq().markers.empty());
    REQUIRE(finished.calls.size() == 1);
    CHECK_FALSE(finished.calls[0].ok);
    CHECK(finished.calls[0].cancelled);
    CHECK(finished.calls[0].message == QStringLiteral("Cancelled"));
    CHECK_FALSE(r.scenes.busy());
    // The next run works again.
    r.shots->during = {};
    REQUIRE(r.scenes.run({r.key("V1@0")}, {{QStringLiteral("markers"), false}}).isEmpty());
    CHECK(r.undo() == before + 1);
    forgetOptions();
}

TEST_CASE("Scene Edit Detection acts on the selected video clips and says why not", "[app][scenedetect][palette]") {
    resetSettings();
    forgetOptions();
    SceneRig r;
    const auto reason = [&r] { return r.scenes.availability().value(QStringLiteral("reason")).toString(); };
    const auto enabled = [&r] { return r.scenes.availability().value(QStringLiteral("enabled")).toBool(); };
    // Nothing selected.
    CHECK_FALSE(enabled());
    CHECK(reason() == QStringLiteral("Select a video clip first"));
    CHECK(r.scenes.availability().value(QStringLiteral("note")).toString() == QStringLiteral("no video clip"));
    // Sound only.
    r.controller.selectClipById(r.tl.clip("A2@10"), ProjectController::Replace);
    CHECK_FALSE(enabled());
    CHECK(reason() == QStringLiteral("Scene Edit Detection works on video clips: select one"));
    // A linked audio clip stands for its picture.
    r.controller.selectClipById(r.tl.clip("A1@0"), ProjectController::Replace);
    CHECK(enabled());
    CHECK(r.scenes.availability().value(QStringLiteral("clips")).toStringList() == QStringList{r.key("V1@0")});
    CHECK(r.scenes.targets({r.key("A1@0"), r.key("A2@10")}) == QStringList{r.key("V1@0")});
    // Timeline order whatever the selection's order; the palette's answer.
    CHECK(r.scenes.targets({r.key("V1@100"), r.key("V1@0")}) == (QStringList{r.key("V1@0"), r.key("V1@100")}));
    QVariantMap state = r.scenes.commandAvailability(QStringLiteral("clip.sceneEditDetection"));
    CHECK(state.value(QStringLiteral("handled")).toBool());
    CHECK(state.value(QStringLiteral("enabled")).toBool());
    CHECK(r.scenes.commandAvailability(QStringLiteral("clip.audioGain")).isEmpty());
    CHECK(SceneDetectBridge::handlesCommand(QStringLiteral("clip.sceneEditDetection")));
    CHECK(KeymapCommandProvider::viewHandles(QStringLiteral("clip.sceneEditDetection")));
    // A locked video track.
    SceneRig locked(picture("tgt lock"), false); // (links cannot be made on a locked track)
    locked.controller.selectClipById(locked.tl.clip("V1@0"), ProjectController::Replace);
    CHECK_FALSE(locked.scenes.availability().value(QStringLiteral("enabled")).toBool());
    CHECK(locked.scenes.availability().value(QStringLiteral("reason")).toString() == QStringLiteral("The video track is locked"));
    CHECK(locked.scenes.availability().value(QStringLiteral("note")).toString() == QStringLiteral("track locked"));
    CHECK_FALSE(locked.scenes.run({locked.key("V1@0")}).isEmpty());
    CHECK(locked.shots->asked.empty());
    state = locked.scenes.commandAvailability(QStringLiteral("clip.sceneEditDetection"));
    CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
    CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("The video track is locked"));
}

// Fix round 1 (QA Argon #16): the dialog keeps the clips AND the sequence it was opened for; a selection change or
// another sequence becoming current before Analyze changes nothing about what is analysed.
TEST_CASE("Scene Edit Detection acts on the clips and sequence the dialog was opened for", "[app][scenedetect]") {
    resetSettings();
    forgetOptions();
    SceneRig r;
    SequenceId other;
    r.controller.resetProject(withOtherSequence(r.tl.state, &other), r.tl.seq);
    r.controller.selectClipById(r.tl.clip("V1@0"), ProjectController::Replace);
    // What the dialog keeps when it opens.
    const QVariantMap opened = r.scenes.availability();
    REQUIRE(opened.value(QStringLiteral("enabled")).toBool());
    const QStringList clips = opened.value(QStringLiteral("clips")).toStringList();
    const QString sequence = opened.value(QStringLiteral("sequence")).toString();
    CHECK(clips == QStringList{r.key("V1@0")});
    CHECK(sequence == sequenceKey(r.tl.seq));
    // Meanwhile the selection moves to a clip with sound only and the other sequence becomes current.
    r.controller.selectClipById(r.tl.clip("A2@10"), ProjectController::Replace);
    showSequence(r.controller, other);
    // Analyze: the clips it was opened for, in their sequence; the user stays where they are.
    const std::size_t before = r.undo();
    REQUIRE(r.scenes.run(clips, {{QStringLiteral("sequence"), sequence}}).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.currentSequenceId() == other);
    const Sequence* original = r.controller.snapshot()->findSequence(r.tl.seq);
    REQUIRE(original);
    CHECK(original->findTrack(r.tl.track("V1"))->clips.size() == 4);
    CHECK(original->findTrack(r.tl.track("A1"))->clips.size() == 3);
    CHECK(r.scenes.message() == QStringLiteral("Split 1 clip at 2 cuts"));
    // Undo puts them back.
    REQUIRE(r.controller.undo());
    CHECK(r.controller.snapshot()->findSequence(r.tl.seq)->findTrack(r.tl.track("V1"))->clips.size() == 2);
    // A sequence that is gone: refused, nothing runs.
    const std::size_t asked = r.shots->asked.size();
    CHECK(r.scenes.run(clips, {{QStringLiteral("sequence"), QStringLiteral("s999999")}}) ==
          QStringLiteral("That sequence is no longer in the project"));
    CHECK(r.shots->asked.size() == asked);
    forgetOptions();
}

TEST_CASE("Scene Edit Detection's toast text is built from the result data", "[app][scenedetect]") {
    using command::Json;
    const auto split = [](Json cuts, std::vector<std::string> warnings = {}) {
        command::CommandResult r;
        r.data = Json{{"cutFields", Json::array({"clip", "time", "kind", "confidence"})}, {"cuts", std::move(cuts)}};
        r.warnings = std::move(warnings);
        return r;
    };
    const auto cut = [](std::uint64_t clip) { return Json::array({clip, "00:00:01:00", "cut", 0.9}); };
    CHECK(SceneDetectBridge::summary(split(Json::array({cut(12)})), true, false, 1) == QStringLiteral("Split 1 clip at 1 cut"));
    CHECK(SceneDetectBridge::summary(split(Json::array({cut(12), cut(12), cut(14)})), true, false, 3) ==
          QStringLiteral("Split 2 clips at 3 cuts"));
    CHECK(SceneDetectBridge::summary(split(Json::array()), true, false, 1) == QStringLiteral("No scene edits found in the clip"));
    CHECK(SceneDetectBridge::summary(split(Json::array()), true, false, 4) == QStringLiteral("No scene edits found in 4 clips"));
    CHECK(SceneDetectBridge::summary(split(Json::array({cut(12)}), {"clip 14 has no video to find shots in"}), true, false, 2) ==
          QStringLiteral("Split 1 clip at 1 cut; 1 clip was skipped"));
    // A batch: markers (analysis.detectShots per clip), then the cuts.
    command::CommandResult batch;
    batch.steps.push_back(command::StepResult{"analysis.detectShots", {}, Json{{"markersAdded", 1}, {"cuts", Json::array()}}, {}});
    batch.steps.push_back(command::StepResult{"analysis.detectShots", {}, Json{{"markersAdded", 0}, {"cuts", Json::array()}}, {}});
    CHECK(SceneDetectBridge::summary(batch, false, true, 2) == QStringLiteral("Added 1 marker at scene edits in 1 clip"));
    batch.steps.push_back(command::StepResult{"clip.splitAtShots", {}, split(Json::array({cut(12)})).data, {}});
    CHECK(SceneDetectBridge::summary(batch, true, true, 2) == QStringLiteral("Split 1 clip at 1 cut and added 1 marker"));
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

namespace {

QQuickItem* findNamed(QQuickItem* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findNamed(child, name))
            return found;
    }
    return nullptr;
}

QRectF sceneRect(const QQuickItem* item) { return item->mapRectToScene(QRectF(0, 0, item->width(), item->height())); }

/// A control of a WabcutDialog (its body and its buttons).
QQuickItem* inDialog(QObject* dialog, const char* name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* found = findNamed(content->parentItem() ? content->parentItem() : content, QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

/// Saves the dialog's card (with a margin) from one grab of the window.
void saveDialog(Shell& shell, QObject* dialog, const QString& file) {
    auto* background = dialog->property("background").value<QQuickItem*>();
    REQUIRE(background);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF r = sceneRect(background).adjusted(-16, -16, 16, 16).intersected(
        QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
}

} // namespace

TEST_CASE("Scene Edit Detection in the Clip menu and the clip menu opens its dialog; screenshots dark and light",
          "[app][scenedetect][qml][screenshot]") {
    resetSettings();
    clearMessages();
    forgetOptions();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* scenes = shell.engine.singletonInstance<SceneDetectBridge*>("Wabcut", "SceneDetect");
    auto* palette = shell.engine.singletonInstance<CommandPalette*>("Wabcut", "CommandPalette");
    REQUIRE(controller);
    REQUIRE(scenes);
    REQUIRE(palette);
    auto shots = std::make_shared<TestShots>();
    scenes->setServices(std::make_shared<an::AnalysisServices>(shots));
    scenes->setSynchronous(true);
    Timeline tl(picture());
    tl.linkMatching();
    shots->cutFrames[tl.media('A').value] = {30, 70};
    SequenceId other;
    controller->resetProject(withOtherSequence(tl.state, &other), tl.seq);
    spin(200);
    const auto undo = [&] { return controller->session().undoCount(); };

    // The palette row and the Clip menu say why not without a video clip.
    QVariantMap state = palette->commandState(QStringLiteral("clip.sceneEditDetection"));
    CHECK(state.value(QStringLiteral("known")).toBool());
    CHECK(state.value(QStringLiteral("title")).toString() == QStringLiteral("Scene Edit Detection…"));
    CHECK_FALSE(state.value(QStringLiteral("enabled")).toBool());
    CHECK(state.value(QStringLiteral("reason")).toString() == QStringLiteral("Select a video clip first"));
    QObject* barRow = shell.window->findChild<QObject*>(QStringLiteral("menuCommand_clip.sceneEditDetection"));
    REQUIRE(barRow);
    CHECK(barRow->property("text").toString() == QStringLiteral("Scene Edit Detection…"));

    // The clip menu on a clip with sound only: greyed out, with the reason.
    QObject* clipMenu = shell.window->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(clipMenu);
    QQuickItem* lanes = findNamed(shell.window->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    QObject* row = clipMenu->findChild<QObject*>(QStringLiteral("sceneEditDetectionMenuItem"));
    REQUIRE(row);
    const auto openMenu = [&](std::string_view ref, QPointF at) {
        controller->selectClip(clipKey(tl.clip(ref)));
        REQUIRE(QMetaObject::invokeMethod(clipMenu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                          Q_ARG(QVariant, clipKey(tl.clip(ref))), Q_ARG(QVariant, at)));
        REQUIRE(waitFor([&] { return clipMenu->property("opened").toBool(); }, 5000));
    };
    openMenu("A2@10", QPointF(300, 80));
    CHECK_FALSE(row->property("enabled").toBool());
    CHECK(row->property("text").toString() == QStringLiteral("Scene Edit Detection… (no video clip)"));
    QMetaObject::invokeMethod(clipMenu, "close");
    REQUIRE(waitFor([&] { return !clipMenu->property("visible").toBool(); }, 5000));

    // On the video clip: on; choosing it opens the dialog once the menu has closed.
    openMenu("V1@0", QPointF(200, 20));
    CHECK(row->property("enabled").toBool());
    CHECK(row->property("text").toString() == QStringLiteral("Scene Edit Detection…"));
    CHECK(palette->commandState(QStringLiteral("clip.sceneEditDetection")).value(QStringLiteral("enabled")).toBool());
    REQUIRE(QMetaObject::invokeMethod(row, "triggered"));
    QMetaObject::invokeMethod(clipMenu, "close");
    QObject* loader = shell.window->findChild<QObject*>(QStringLiteral("sceneEditDialogLoader"));
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    CHECK(inDialog(dialog, "sceneEditClipCount")->property("text").toString() == QStringLiteral("1 video clip selected"));
    CHECK(inDialog(dialog, "sceneEditCuts")->property("checked").toBool());
    CHECK_FALSE(inDialog(dialog, "sceneEditMarkers")->property("checked").toBool());
    CHECK(inDialog(dialog, "sceneEditDetector")->property("text").toString() == QStringLiteral("Cuts, fades and dissolves"));
    CHECK(inDialog(dialog, "sceneEditSensitivity")->property("value").toDouble() == Catch::Approx(50));
    CHECK(inDialog(dialog, "sceneEditSensitivity")->isEnabled());
    CHECK(inDialog(dialog, "sceneEditAnalyze")->isEnabled());
    {
        auto* content = dialog->property("contentItem").value<QQuickItem*>();
        const QStringList issues = auditAccessibility(content);
        INFO(issues.join(QStringLiteral("; ")).toStdString());
        CHECK(issues.isEmpty());
    }
    // Markers too; the screenshot (dark).
    REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "sceneEditMarkers"), "toggle"));
    REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "sceneEditMarkers"), "toggled"));
    CHECK(dialog->property("markers").toBool());
    spin(100);
    saveDialog(shell, dialog, QStringLiteral("scene-edit-detection-dialog.png"));
    // The detector is the editing dialogs' dropdown (EditingChoice): a secondary pill, label then chevron, its menu of
    // checkable rows with the current one checked; choosing a row sets it. Fades through black: Sensitivity does not
    // apply.
    QQuickItem* detector = inDialog(dialog, "sceneEditDetector");
    CHECK(detector->inherits("QQuickAbstractButton"));
    CHECK(detector->property("variant").toString() == QStringLiteral("secondary"));
    CHECK(detector->property("iconName").toString() == QStringLiteral("chevron-down"));
    REQUIRE(QMetaObject::invokeMethod(detector, "clicked"));
    QObject* detectorMenu = nullptr;
    for (QObject* o : detector->findChildren<QObject*>()) {
        if (o->inherits("QQuickMenu"))
            detectorMenu = o;
    }
    REQUIRE(detectorMenu);
    REQUIRE(waitFor([&] { return detectorMenu->property("opened").toBool(); }, 5000));
    REQUIRE(detectorMenu->property("count").toInt() == 4);
    const auto menuRow = [&](int i) {
        QQuickItem* item = nullptr;
        REQUIRE(QMetaObject::invokeMethod(detectorMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i)));
        REQUIRE(item);
        return item;
    };
    CHECK(menuRow(0)->property("checked").toBool());
    CHECK_FALSE(menuRow(3)->property("checked").toBool());
    CHECK(menuRow(3)->property("text").toString() == QStringLiteral("Fades through black"));
    REQUIRE(QMetaObject::invokeMethod(menuRow(3), "triggered"));
    REQUIRE(waitFor([&] { return !detectorMenu->property("visible").toBool(); }, 5000));
    CHECK(dialog->property("detector").toString() == QStringLiteral("threshold"));
    CHECK(detector->property("text").toString() == QStringLiteral("Fades through black"));
    CHECK_FALSE(inDialog(dialog, "sceneEditSensitivity")->isEnabled());
    dialog->setProperty("detector", QStringLiteral("combined"));
    // The light theme.
    shell.setAppearance(false, "ultraviolet");
    spin(300);
    saveDialog(shell, dialog, QStringLiteral("scene-edit-detection-dialog-light.png"));
    shell.setAppearance(true, "ultraviolet");
    spin(100);

    // Fix round 1 (QA Argon #16): before Analyze the selection moves to a clip with sound only and another sequence
    // becomes current (a script, the assistant); the dialog still acts on the clip it was opened for, in its sequence.
    controller->selectClip(clipKey(tl.clip("A2@10")));
    showSequence(*controller, other);
    CHECK(inDialog(dialog, "sceneEditAnalyze")->isEnabled());
    // Analyze: one undo step, the dialog closes, the toast says what happened.
    const std::size_t before = undo();
    REQUIRE(QMetaObject::invokeMethod(inDialog(dialog, "sceneEditAnalyze"), "clicked"));
    CHECK(undo() == before + 1);
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 5000));
    CHECK(controller->currentSequenceId() == other);
    showSequence(*controller, tl.seq);
    CHECK(controller->currentSequence()->findTrack(tl.track("V1"))->clips.size() == 4);
    CHECK(controller->currentSequence()->markers.size() == 2);
    QObject* toast = shell.window->findChild<QObject*>(QStringLiteral("toast"));
    REQUIRE(toast);
    REQUIRE(waitFor([&] { return toast->property("text").toString() == QStringLiteral("Split 1 clip at 2 cuts and added 2 markers"); },
                    3000));
    // Remembered: markers on next time.
    CHECK(scenes->options().value(QStringLiteral("markers")).toBool());

    // From the keymap command without a video clip: no dialog, the reason as a toast.
    controller->selectClip(clipKey(tl.clip("A2@10")));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.sceneEditDetection"))));
    CHECK_FALSE(dialog->property("visible").toBool());
    forgetOptions();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

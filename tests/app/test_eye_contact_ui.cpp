// AI Eye Contact (card 72) and the face-aware Morph Cut (card 131) in the shell, through the real Main.qml offscreen. AI Eye Contact is a clip
// effect whose Effect Controls header says what it needs (the two models to download, with the licence card), Process runs the runner over the
// clip's media (a fake backend and frame source here: no model, no helper) and the header says what is done; the Morph Cut's header says whether
// its face detector is on this computer and offers the download. Screenshots in the dark and the light theme.

#include "ShellFixture.h"

#include "AiVideoBridge.h"
#include "EffectControls.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "video/VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/Gaze.h>
#include <wabcut/ai/video/MorphFaces.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQuickItem>

#include <filesystem>
#include <fstream>
#include <functional>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace avt = wabcut::ai::video::test;

namespace {

/// A frame source of N pictures at 25 fps and a fixed size, whatever file it is asked for.
class TinySource final : public ai::video::IFrameSource {
public:
    explicit TinySource(int frames) : m_inner(frames, 16, 12) {}
    std::expected<std::optional<ai::video::SourceFrame>, Error> frameAt(Ticks time, const std::stop_token& stop) override {
        return m_inner.frameAt(time, stop);
    }

private:
    avt::FakeSource m_inner;
};

/// One stand-in model file (the right size and digest marker, as the store checks).
void installStandIn(const std::filesystem::path& root, const ai::ModelInfo& info) {
    const ai::ModelStore store(root);
    const std::filesystem::path file = store.pathFor(info);
    std::filesystem::create_directories(file.parent_path());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << 'x';
    }
    std::filesystem::resize_file(file, info.size);
    std::filesystem::path marker = file;
    marker += ".verified";
    std::ofstream(marker, std::ios::trunc) << info.sha256;
}

void installStandIn(const std::filesystem::path& root, ai::video::Model m) {
    const ai::ModelInfo* info = ai::findModel(ai::video::describe(m).storeId);
    REQUIRE(info != nullptr);
    installStandIn(root, *info);
}

QQuickItem* visibleNamed(Shell& shell, const QString& name) {
    const std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == name && item->isVisible())
            return item;
        for (QQuickItem* child : item->childItems()) {
            if (QQuickItem* found = find(child))
                return found;
        }
        return nullptr;
    };
    return find(shell.window->contentItem());
}

} // namespace

TEST_CASE("AI Eye Contact: the header offers both models, Process measures the eyes and says so (card 72)", "[app][ai][video][eyecontact]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* ai = shell.engine.singletonInstance<AiVideoBridge*>("Wabcut", "AiVideo");
    REQUIRE(controller);
    REQUIRE(ai);
    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    const std::size_t undoBefore = controller->session().undoCount();
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiEyeContactEffect)); })
                .has_value());
    CHECK(controller->session().undoCount() == undoBefore + 1); // adding it is one undo step
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});

    auto cache = std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-gaze-cache"));
    auto backend = std::make_shared<avt::FakeBackend>();
    ai->setServices(cache, backend, [&](const std::string&, int) -> std::unique_ptr<ai::video::IFrameSource> {
        return std::make_unique<TinySource>(1000);
    });
    const std::filesystem::path models = avt::freshDir("ui-gaze-models");
    ai->setModelRoot(models);
    ai->setHelperAvailable(true);
    ai->setSynchronous(true);

    // The effect shows the AI header: two models (the detector is tiny), Apache-2.0 with the licence card.
    QVariantMap info = ai->effectInfo(section);
    REQUIRE(info.value(QStringLiteral("ai")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("eyeContact"));
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK(info.value(QStringLiteral("models")).toStringList() ==
          QStringList{QStringLiteral("YuNet face detector"), QStringLiteral("Face landmarks")});
    CHECK(info.value(QStringLiteral("downloadText")).toString().contains(QStringLiteral("5 MB")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("Apache License 2.0")));
    CHECK(info.value(QStringLiteral("label")).toString() == QStringLiteral("Eye measurements"));
    CHECK_FALSE(ai->process(section)); // nothing runs without the models

    // One of the two is not enough.
    installStandIn(models, ai::video::Model::FaceLandmarks);
    ai->setModelRoot(models);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK_FALSE(ai->process(section));

    // Both are there: not measured yet, and the message says what the effect does meanwhile.
    installStandIn(models, ai::video::Model::FaceDetect);
    ai->setModelRoot(models);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(info.value(QStringLiteral("message")).toString().contains(QStringLiteral("not measured")));

    // Process: one table of measurements per frame of the clip, and the header says Processed.
    REQUIRE(ai->process(section));
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(info.value(QStringLiteral("processed")).toInt() == info.value(QStringLiteral("frames")).toInt());
    REQUIRE_FALSE(backend->calls.empty());
    CHECK(backend->calls.front().model == ai::video::Model::FaceLandmarks);
    const auto target = ai::video::gazeTarget(*controller->snapshot(), *controller->currentSequence(),
                                              controller->currentSequence()->findClip(clip)->clip(), 0);
    REQUIRE(target.has_value());
    const auto stored = cache->get(target->identity, Ticks{}, framesToTicks(1, Rate{25, 1}));
    REQUIRE(stored);
    const std::vector<GazeFace> faces = ai::video::gazeFacesOfStored(*stored);
    REQUIRE(faces.size() == 1);
    CHECK(faces[0].eyes[0].valid());
    const std::size_t callsAfterProcess = backend->calls.size();

    // Moving a slider is one undo step and does not measure again (the measurements belong to the picture): the state stays ready.
    const std::size_t undoNow = controller->session().undoCount();
    edit::EffectChange tuned;
    tuned.params = {EffectParam{"amount", ParamValue(60.0)}, EffectParam{"downTo", ParamValue(30.0)}};
    REQUIRE(controller->session()
                .apply("Eye Contact", [&](const ProjectState& s) { return edit::setEffect(s, seq, clip, effect, tuned); })
                .has_value());
    CHECK(controller->session().undoCount() == undoNow + 1);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(backend->calls.size() == callsAfterProcess);
    REQUIRE(controller->undo());
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    // Delete: the measurements go, and Process can make them again.
    ai->discard(section);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(ai->overview().value(QStringLiteral("count")).toInt() == 1);
    CHECK(ai->overview().value(QStringLiteral("pending")).toInt() == 1);
    // Turning the effect off leaves nothing to process.
    edit::EffectChange off;
    off.enabled = false;
    REQUIRE(controller->session()
                .apply("Disable", [&](const ProjectState& s) { return edit::setEffect(s, seq, clip, effect, off); })
                .has_value());
    CHECK(ai->overview().value(QStringLiteral("count")).toInt() == 0);
}

TEST_CASE("Morph Cut's header says whether its face detector is there and offers the download (card 131)", "[app][ai][video][morphcut]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* ai = shell.engine.singletonInstance<AiVideoBridge*>("Wabcut", "AiVideo");
    REQUIRE(controller);
    REQUIRE(ai);
    Timeline tl("V1 tgt |AaaaaaaaaaB5bbbbbbbb......................|\nA1 tgt |AaaaaaaaaaB5bbbbbbbb......................|");
    tl.linkMatching();
    edit::TransitionSpec spec;
    spec.outgoing = tl.clip("V1@0");
    spec.incoming = tl.clip("V1@10");
    spec.before = framesToTicks(2, Rate{25, 1});
    spec.after = framesToTicks(2, Rate{25, 1});
    spec.effect = std::string(kMorphCutTransition);
    ProjectState state = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), spec)).state;
    controller->resetProject(state, tl.seq);
    const TransitionId transition = controller->currentSequence()->findTrack(tl.track("V1"))->transitions.front().id;
    const std::filesystem::path models = avt::freshDir("ui-morph-models");
    ai->setModelRoot(models);
    ai->setHelperAvailable(true);

    // Nothing selected: not a Morph Cut. Selected, with the detector missing: it says so and offers 0.2 MB with the licence.
    CHECK_FALSE(ai->morphInfo().value(QStringLiteral("morphCut")).toBool());
    controller->selectTransition(transitionKey(transition));
    QVariantMap info = ai->morphInfo();
    REQUIRE(info.value(QStringLiteral("morphCut")).toBool());
    CHECK(info.value(QStringLiteral("faceAware")).toBool()); // on by default
    CHECK_FALSE(info.value(QStringLiteral("installed")).toBool());
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK(info.value(QStringLiteral("downloadText")).toString() == QStringLiteral("Download 0.2 MB"));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("MIT")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("YuNet")));
    CHECK_FALSE(ai::video::faceDetectorInstalled(models));
    // Installed: ready, nothing to download.
    installStandIn(models, *ai::video::faceDetectorModel());
    ai->setModelRoot(models);
    info = ai->morphInfo();
    CHECK(info.value(QStringLiteral("installed")).toBool());
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(info.value(QStringLiteral("downloadText")).toString().isEmpty());
    CHECK_FALSE(ai->downloadModels(QStringLiteral("morph"))); // nothing is missing
    // The switch off is said too; a build without the helper cannot look for faces.
    edit::TransitionChange change;
    change.params = std::vector<EffectParam>{EffectParam{"faceAware", ParamValue(0.0)}};
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Face Aware", [&](const ProjectState& s) { return edit::setTransition(s, seq, transition, change); })
                .has_value());
    info = ai->morphInfo();
    CHECK_FALSE(info.value(QStringLiteral("faceAware")).toBool());
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("off"));
    REQUIRE(controller->undo());
    ai->setHelperAvailable(false);
    CHECK(ai->morphInfo().value(QStringLiteral("state")).toString() == QStringLiteral("noHelper"));
}

TEST_CASE("AI Eye Contact and Morph Cut in the shell: Effect Controls rows and the headers (screenshots)", "[app][ai][video][shot][eyecontact]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* ai = shell.engine.singletonInstance<AiVideoBridge*>("Wabcut", "AiVideo");
    REQUIRE(controller);
    REQUIRE(ai);

    Timeline tl("V1 tgt |AaaaaaaaaaB5bbbbbbbb......................|\nA1 tgt |AaaaaaaaaaB5bbbbbbbb......................|");
    tl.linkMatching();
    edit::TransitionSpec spec;
    spec.outgoing = tl.clip("V1@0");
    spec.incoming = tl.clip("V1@10");
    spec.before = framesToTicks(2, Rate{25, 1});
    spec.after = framesToTicks(2, Rate{25, 1});
    spec.effect = std::string(kMorphCutTransition);
    ProjectState state = ok(edit::addTransition(tl.state, tl.seq, tl.track("V1"), spec)).state;
    controller->resetProject(state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiEyeContactEffect)); })
                .has_value());
    const QString eyeSection = QStringLiteral("e%1").arg(controller->currentSequence()->findClip(clip)->clip().effects().front().id.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    ai->setServices(std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-cache-eye-shot")), std::make_shared<avt::FakeBackend>());
    ai->setHelperAvailable(true);
    ai->setModelRoot(avt::freshDir("ui-models-eye-shot")); // empty: both models and the Morph Cut's detector are offered

    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    const auto shoot = [&](const QString& header, const QString& name, int rowsBelow = 0) {
        REQUIRE(fx.rowOf(header) >= 0);
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(header) + rowsBelow), Q_ARG(int, 0)));
        spin(80);
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            const double dpr = image.devicePixelRatio();
            const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
            REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                        .save(outputDir() + QStringLiteral("/%1-%2.png").arg(name, dark ? QStringLiteral("dark") : QStringLiteral("light"))));
        }
        shell.setAppearance(true, "ultraviolet");
    };
    // AI Eye Contact: the AI header offers the two models (one Download with the total size), and its rows follow.
    const QString eyeHeader = QStringLiteral("h:") + eyeSection;
    REQUIRE(fx.rowOf(eyeHeader) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(eyeHeader)), Q_ARG(int, 0)));
    spin(80);
    QQuickItem* download = nullptr;
    CHECK(waitFor([&] { return (download = visibleNamed(shell, QStringLiteral("aiVideoDownload"))) != nullptr; }));
    REQUIRE(download != nullptr);
    CHECK(download->property("text").toString() == QStringLiteral("Download 5 MB"));
    QQuickItem* status = visibleNamed(shell, QStringLiteral("aiVideoStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->property("text").toString() == QStringLiteral("Model needed"));
    shoot(eyeHeader, QStringLiteral("effect-controls-ai-eye-contact"));
    shoot(eyeHeader, QStringLiteral("effect-controls-ai-eye-contact-more"), 3); // Down From, Down To, Side Limit, Faces, Show
    // The effect's name is not cut in the header at this panel width: the status gives way (it elides), the name does not.
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(eyeHeader)), Q_ARG(int, 0)));
    spin(80);
    const std::function<void(QQuickItem*, std::vector<QQuickItem*>&)> collect = [&](QQuickItem* item, std::vector<QQuickItem*>& out) {
        if (item->objectName() == QStringLiteral("sectionTitle") && item->isVisible()) {
            out.push_back(item);
        }
        for (QQuickItem* child : item->childItems()) {
            collect(child, out);
        }
    };
    bool whole = false;
    for (int i = 0; i < 20 && !whole; ++i) {
        spin(40);
        std::vector<QQuickItem*> titles;
        collect(shell.window->contentItem(), titles);
        for (QQuickItem* title : titles) {
            if (title->property("text").toString() == QStringLiteral("AI Eye Contact")) {
                whole = title->width() >= title->property("implicitWidth").toDouble() - 0.5;
            }
        }
    }
    CHECK(whole);

    // Morph Cut: select the transition; its header says the face detector is needed and offers the download; the Face Aware switch is a row.
    const TransitionId transition = controller->currentSequence()->findTrack(tl.track("V1"))->transitions.front().id;
    controller->selectTransition(transitionKey(transition));
    spin(80);
    REQUIRE(fx.rowOf(QStringLiteral("h:transition")) >= 0);
    REQUIRE(fx.rowOf(QStringLiteral("t:faceAware")) >= 0);
    CHECK(fx.row(QStringLiteral("t:faceAware")).value(QStringLiteral("kind")).toString() == QStringLiteral("toggle"));
    CHECK(fx.row(QStringLiteral("t:faceAware")).value(QStringLiteral("title")).toString() == QStringLiteral("Face Aware"));
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:transition"))), Q_ARG(int, 0)));
    spin(80);
    QQuickItem* faceDownload = nullptr;
    CHECK(waitFor([&] { return (faceDownload = visibleNamed(shell, QStringLiteral("morphFacesDownload"))) != nullptr; }));
    REQUIRE(faceDownload != nullptr);
    CHECK(faceDownload->property("text").toString() == QStringLiteral("Download 0.2 MB"));
    QQuickItem* faceStatus = visibleNamed(shell, QStringLiteral("morphFacesStatus"));
    REQUIRE(faceStatus != nullptr);
    CHECK(faceStatus->property("text").toString() == QStringLiteral("Face detector needed"));
    shoot(QStringLiteral("h:transition"), QStringLiteral("effect-controls-morph-cut"));
}

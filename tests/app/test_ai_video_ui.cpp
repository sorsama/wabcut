// AI Restore and AI Denoise in the shell (0.7), through the real Main.qml offscreen: the effects are ordinary clip effects (added,
// edited and undone like any), their Effect Controls header says what they need and what is done (models to download with the
// licence card, Process, Cancel, Delete), Process runs the runner over the clip's media (a fake backend and frame source here:
// no model, no helper) and the playhead frame picks the pictures up; the monitor has its badge, the Export panel its notice.
// Screenshots in the dark and the light theme.

#include "ShellFixture.h"

#include "AiVideoBridge.h"
#include "EffectControls.h"
#include "ExportController.h"
#include "PlaybackBridge.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "video/VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QImage>
#include <QQuickItem>

#include <filesystem>
#include <fstream>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace avt = wabcut::ai::video::test;

namespace {

/// A frame source of N pictures at 25 fps and a fixed size, whatever file it is asked for.
class SmallSource final : public ai::video::IFrameSource {
public:
    explicit SmallSource(int frames) : m_inner(frames, 16, 12) {}
    std::expected<std::optional<ai::video::SourceFrame>, Error> frameAt(Ticks time, const std::stop_token& stop) override {
        return m_inner.frameAt(time, stop);
    }

private:
    avt::FakeSource m_inner;
};

void installModels(const std::filesystem::path& root) {
    const ai::ModelStore store(root);
    for (const ai::video::Model m : {ai::video::Model::Denoise, ai::video::Model::Deblur, ai::video::Model::UpscaleFast4x,
                                     ai::video::Model::Upscale2x, ai::video::Model::Upscale4x}) {
        const ai::ModelInfo* info = ai::findModel(ai::video::describe(m).storeId);
        REQUIRE(info != nullptr);
        const std::filesystem::path file = store.pathFor(*info);
        std::filesystem::create_directories(file.parent_path());
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out << 'x';
        }
        std::filesystem::resize_file(file, info->size);
        std::filesystem::path marker = file;
        marker += ".verified";
        std::ofstream(marker, std::ios::trunc) << info->sha256;
    }
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

/// An item by object name, visible or not.
QQuickItem* namedAny(Shell& shell, const QString& name) {
    const std::function<QQuickItem*(QQuickItem*)> find = [&](QQuickItem* item) -> QQuickItem* {
        if (item->objectName() == name)
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

TEST_CASE("AI video effects: header states, Process, Delete and the export notice", "[app][ai][video]") {
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

    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiRestoreEffect)); })
                .has_value());
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});

    // The services: a fresh cache, a fake backend and frame source, stand-in models in a store of their own.
    auto cache = std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-cache"));
    auto backend = std::make_shared<avt::FakeBackend>();
    int sourcesMade = 0;
    ai->setServices(cache, backend, [&](const std::string&, int) -> std::unique_ptr<ai::video::IFrameSource> {
        ++sourcesMade;
        return std::make_unique<SmallSource>(1000); // longer than any clip: the range decides
    });
    const std::filesystem::path models = avt::freshDir("ui-models");
    ai->setModelRoot(models);
    ai->setHelperAvailable(true);
    ai->setSynchronous(true);

    CHECK_FALSE(ai->effectInfo(QStringLiteral("e999999")).value(QStringLiteral("ai")).toBool());

    // No model yet: the header offers the download, with the size and the licence card.
    QVariantMap info = ai->effectInfo(section);
    REQUIRE(info.value(QStringLiteral("ai")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("restore"));
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK(info.value(QStringLiteral("downloadText")).toString().startsWith(QStringLiteral("Download ")));
    CHECK(info.value(QStringLiteral("downloadText")).toString().endsWith(QStringLiteral(" MB")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("MIT License")));
    CHECK(info.value(QStringLiteral("models")).toStringList().contains(QStringLiteral("NAFNet deblurring")));
    CHECK(info.value(QStringLiteral("frames")).toInt() > 10);
    // Process refuses with the reason; nothing is made.
    CHECK_FALSE(ai->process(section));
    CHECK(ai->message().contains(QStringLiteral("NAFNet")));
    CHECK(backend->calls.empty());
    // The export panel's notice: the clip has pictures missing and the export cannot make them.
    QVariantMap needs = ai->exportNeeds(QStringLiteral("whole"));
    CHECK(needs.value(QStringLiteral("count")).toInt() == 1);
    CHECK_FALSE(needs.value(QStringLiteral("canMake")).toBool());
    CHECK(needs.value(QStringLiteral("why")).toString().contains(QStringLiteral("NAFNet")));

    // Models on this computer: ready to process.
    installModels(models);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(info.value(QStringLiteral("processed")).toInt() == 0);
    CHECK_FALSE(info.value(QStringLiteral("estimate")).toString().isEmpty());
    needs = ai->exportNeeds(QStringLiteral("whole"));
    CHECK(needs.value(QStringLiteral("canMake")).toBool());
    CHECK(needs.value(QStringLiteral("frames")).toInt() == info.value(QStringLiteral("frames")).toInt());
    CHECK(needs.value(QStringLiteral("names")).toStringList().front().contains(QStringLiteral("V1: ")));
    CHECK(ai->overview().value(QStringLiteral("pending")).toInt() == 1);

    // The header in Effect Controls (the list scrolled to it, as a grab lays the shell out).
    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(fx.rowOf(QStringLiteral("h:") + section) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + section)), Q_ARG(int, 0)));
    spin(50);
    QQuickItem* process = nullptr;
    CHECK(waitFor([&] { return (process = visibleNamed(shell, QStringLiteral("aiVideoProcess"))) != nullptr; }));
    QQuickItem* status = visibleNamed(shell, QStringLiteral("aiVideoStatus"));
    REQUIRE(process != nullptr);
    REQUIRE(status != nullptr);
    CHECK(process->property("text").toString() == QStringLiteral("Process"));
    CHECK(status->property("text").toString() == QStringLiteral("Not processed"));

    // Process: a job over the clip's media, one picture per frame, the header says so afterwards.
    REQUIRE(ai->process(section));
    INFO(ai->message().toStdString());
    CHECK_FALSE(ai->busy());
    CHECK(sourcesMade == 1);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(info.value(QStringLiteral("processed")).toInt() == info.value(QStringLiteral("frames")).toInt());
    CHECK(info.value(QStringLiteral("percent")).toInt() == 100);
    CHECK(static_cast<int>(backend->calls.size()) == info.value(QStringLiteral("frames")).toInt());
    CHECK(backend->calls.front().model == ai::video::Model::Deblur); // the default settings: deblur only (Auto scale: 1x)
    CHECK(ai->message().startsWith(QStringLiteral("Done")));
    CHECK(waitFor([&] { return status->property("text").toString() == QStringLiteral("Processed"); }));
    CHECK(ai->exportNeeds(QStringLiteral("whole")).value(QStringLiteral("count")).toInt() == 0);
    CHECK(ai->overview().value(QStringLiteral("pending")).toInt() == 0);
    // Nothing left to do: Process says so and runs nothing.
    const std::size_t made = backend->calls.size();
    CHECK_FALSE(ai->process(section));
    CHECK(ai->message().contains(QStringLiteral("already processed")));
    CHECK(backend->calls.size() == made);
    // Processing is not a project edit: no undo step was added by it.
    CHECK(controller->undoText() == QStringLiteral("Undo Apply Effect"));

    // A setting that changes the pictures starts a new set (the old one stays on disk for when it comes back).
    REQUIRE(controller->session()
                .apply("Set Deblur",
                       [&](const ProjectState& s) {
                           return edit::setParameter(s, seq, clip, edit::ParamRef::ofEffect(effect, "deblur"), ParamValue(80.0));
                       })
                .has_value());
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    REQUIRE(controller->undo());
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    // Amount is a mix at display time: changing it keeps the pictures.
    REQUIRE(controller->session()
                .apply("Set Amount",
                       [&](const ProjectState& s) {
                           return edit::setParameter(s, seq, clip, edit::ParamRef::ofEffect(effect, "amount"), ParamValue(40.0));
                       })
                .has_value());
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    REQUIRE(controller->undo());

    // Delete removes the pictures.
    ai->discard(section);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(ai->effectInfo(section).value(QStringLiteral("processed")).toInt() == 0);

    // A cancel mid-run keeps what was made; Process continues.
    backend->calls.clear();
    backend->onCall = [&](int index) {
        if (index == 4) {
            ai->cancel();
        }
    };
    CHECK(ai->process(section));
    CHECK(ai->message().startsWith(QStringLiteral("Cancelled")));
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("partial"));
    CHECK(info.value(QStringLiteral("processed")).toInt() == 5);
    backend->onCall = nullptr;
    CHECK(ai->process(section));
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));

    // A failing backend ends the run with its message and the state stays honest.
    ai->discard(section);
    backend->failAtCall = static_cast<int>(backend->calls.size()) + 2;
    CHECK(ai->process(section));
    CHECK(ai->message().startsWith(QStringLiteral("Processing stopped")));
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("partial"));

    // A build without the helper says so instead of pretending.
    ai->setHelperAvailable(false);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("noHelper"));
    CHECK_FALSE(ai->process(section));
    ai->setHelperAvailable(true);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("AI video effects: a clip whose settings change nothing, and clips that cannot have them", "[app][ai][video]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* ai = shell.engine.singletonInstance<AiVideoBridge*>("Wabcut", "AiVideo");
    REQUIRE(controller);
    REQUIRE(ai);
    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    ai->setServices(std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-cache-2")), std::make_shared<avt::FakeBackend>());
    ai->setHelperAvailable(true);
    ai->setModelRoot(avt::freshDir("ui-models-2"));
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiDenoiseEffect)); })
                .has_value());
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    CHECK(ai->effectInfo(section).value(QStringLiteral("kind")).toString() == QStringLiteral("denoise"));
    REQUIRE(controller->session()
                .apply("Set Strength",
                       [&](const ProjectState& s) {
                           return edit::setParameter(s, seq, clip, edit::ParamRef::ofEffect(effect, "strength"), ParamValue(0.0));
                       })
                .has_value());
    const QVariantMap info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("inactive"));
    CHECK(info.value(QStringLiteral("message")).toString().contains(QStringLiteral("changes nothing")));
    CHECK_FALSE(ai->process(section));
    CHECK(ai->exportNeeds(QStringLiteral("whole")).value(QStringLiteral("count")).toInt() == 0);
    CHECK(ai->overview().value(QStringLiteral("count")).toInt() == 0);
    // The audio partner has no video effects at all.
    CHECK_FALSE(ai->effectInfo(QStringLiteral("e0")).value(QStringLiteral("ai")).toBool());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("AI video effects: the Program Monitor's badge, the Effect Controls header and the Export notice are in the shell",
          "[app][ai][video][shot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* ai = shell.engine.singletonInstance<AiVideoBridge*>("Wabcut", "AiVideo");
    auto* playback = shell.engine.singletonInstance<PlaybackBridge*>("Wabcut", "Playback");
    REQUIRE(controller);
    REQUIRE(ai);
    REQUIRE(playback);
    CHECK(playback->property("aiUnprocessedLayers").isValid());
    CHECK(playback->property("aiUnprocessedLayers").toInt() == 0);

    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiRestoreEffect)); })
                .has_value());
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    ai->setServices(std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-cache-3")), std::make_shared<avt::FakeBackend>());
    ai->setHelperAvailable(true);
    ai->setModelRoot(avt::freshDir("ui-models-3")); // empty: the download card shows
    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(fx.rowOf(QStringLiteral("h:") + section) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + section)), Q_ARG(int, 0)));
    spin(80);
    QQuickItem* download = nullptr;
    CHECK(waitFor([&] { return (download = visibleNamed(shell, QStringLiteral("aiVideoDownload"))) != nullptr; }));
    REQUIRE(download != nullptr);
    CHECK(download->property("text").toString().startsWith(QStringLiteral("Download ")));

    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/effect-controls-ai-restore-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
    }
    shell.setAppearance(true, "ultraviolet");

    // The Program Monitor has the badge for unprocessed layers (hidden while none is shown; the scheduler's count has its own test).
    QQuickItem* badge = namedAny(shell, QStringLiteral("aiUnprocessedBadge"));
    REQUIRE(badge != nullptr);
    CHECK_FALSE(badge->isVisible());

    // The Export panel says what the export does about the unprocessed AI effects: it cannot make them without the model
    // (the reason is shown, and Process now does nothing), and with the model on this computer it processes them first.
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(exporter != nullptr);
    exporter->setLastFolder(QString::fromStdString(avt::freshDir("ui-export-3").string()));
    shell.window->setProperty("workspace", 2);
    QQuickItem* notice = nullptr;
    CHECK(waitFor([&] { return (notice = visibleNamed(shell, QStringLiteral("exportAiNotice"))) != nullptr; }));
    REQUIRE(notice != nullptr);
    CHECK(notice->property("text").toString().contains(QStringLiteral("cannot make them")));
    CHECK(notice->property("text").toString().contains(QStringLiteral("NAFNet")));
    CHECK(visibleNamed(shell, QStringLiteral("exportAiProcess")) == nullptr); // no button that cannot do anything
    const auto saveNotice = [&](const char* kind) {
        for (const bool dark : {true, false}) {
            shell.setAppearance(dark, "ultraviolet");
            REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            QQuickItem* now = visibleNamed(shell, QStringLiteral("exportAiNotice"));
            REQUIRE(now != nullptr);
            const double dpr = image.devicePixelRatio();
            const QRectF r = now->mapRectToScene(QRectF(0, 0, now->width(), now->height())).adjusted(-24, -24, 24, 24);
            REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                        .save(outputDir() + QStringLiteral("/export-ai-notice-%1-%2.png").arg(QLatin1String(kind), dark ? QStringLiteral("dark") : QStringLiteral("light"))));
        }
        shell.setAppearance(true, "ultraviolet");
    };
    saveNotice("no-model");
    const std::filesystem::path models = avt::freshDir("ui-models-3b");
    installModels(models);
    ai->setModelRoot(models);
    spin(80);
    notice = visibleNamed(shell, QStringLiteral("exportAiNotice"));
    REQUIRE(notice != nullptr);
    CHECK(notice->property("text").toString().contains(QStringLiteral("the export processes them first")));
    CHECK_FALSE(notice->property("text").toString().contains(QStringLiteral("cannot make them")));
    CHECK(visibleNamed(shell, QStringLiteral("exportAiProcess")) != nullptr); // Process now is offered once the model is there
    saveNotice("process");

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

namespace {

/// One stand-in model file (the right size and digest marker, as the store checks).
void installModel(const std::filesystem::path& root, ai::video::Model m) {
    const ai::ModelStore store(root);
    const ai::ModelInfo* info = ai::findModel(ai::video::describe(m).storeId);
    REQUIRE(info != nullptr);
    const std::filesystem::path file = store.pathFor(*info);
    std::filesystem::create_directories(file.parent_path());
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << 'x';
    }
    std::filesystem::resize_file(file, info->size);
    std::filesystem::path marker = file;
    marker += ".verified";
    std::ofstream(marker, std::ios::trunc) << info->sha256;
}

} // namespace

TEST_CASE("AI Refocus: the header offers the depth model, Process makes the depth maps and says so (card 181)", "[app][ai][video][refocus]") {
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
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiRefocusEffect)); })
                .has_value());
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});

    auto cache = std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-refocus-cache"));
    auto backend = std::make_shared<avt::FakeBackend>();
    ai->setServices(cache, backend, [&](const std::string&, int) -> std::unique_ptr<ai::video::IFrameSource> {
        return std::make_unique<SmallSource>(1000);
    });
    const std::filesystem::path models = avt::freshDir("ui-refocus-models");
    ai->setModelRoot(models);
    ai->setHelperAvailable(true);
    ai->setSynchronous(true);

    // The effect shows the AI header: the depth model is Apache-2.0 and about 99 MB, with its licence card.
    QVariantMap info = ai->effectInfo(section);
    REQUIRE(info.value(QStringLiteral("ai")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("refocus"));
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK(info.value(QStringLiteral("models")).toStringList() == QStringList{QStringLiteral("Depth Anything V2 Small")});
    CHECK(info.value(QStringLiteral("downloadText")).toString().contains(QStringLiteral("99 MB")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("Apache")));
    CHECK_FALSE(ai->process(section)); // nothing runs without the model

    // The model is there: no depth maps yet, and the message says what the effect does meanwhile.
    installModel(models, ai::video::Model::Depth);
    ai->setModelRoot(models);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(info.value(QStringLiteral("message")).toString().contains(QStringLiteral("depth maps")));

    // Process: one depth map per frame of the clip, 8 bits, and the header says Processed.
    REQUIRE(ai->process(section));
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(info.value(QStringLiteral("processed")).toInt() == info.value(QStringLiteral("frames")).toInt());
    REQUIRE_FALSE(backend->calls.empty());
    CHECK(backend->calls.front().model == ai::video::Model::Depth);
    const auto target = ai::video::depthTarget(*controller->snapshot(), *controller->currentSequence(),
                                               controller->currentSequence()->findClip(clip)->clip(), 0);
    REQUIRE(target.has_value());
    const auto stored = cache->get(target->identity, Ticks{}, framesToTicks(1, Rate{25, 1}));
    REQUIRE(stored);
    REQUIRE(stored->planes.size() == 1);
    CHECK(stored->planes[0].bytesPerTexel == 1);
    // Moving the focus does not make the maps again (they belong to the picture): the state stays ready.
    edit::EffectChange focus;
    focus.params = {EffectParam{"focus", ParamValue(70.0)}};
    REQUIRE(controller->session()
                .apply("Focus", [&](const ProjectState& s) { return edit::setEffect(s, seq, clip, effect, focus); })
                .has_value());
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    // Delete: the maps go, and Process can make them again.
    ai->discard(section);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(ai->overview().value(QStringLiteral("count")).toInt() == 1);
    CHECK(ai->overview().value(QStringLiteral("pending")).toInt() == 1);
}

TEST_CASE("AI Frame Interpolation: the clip's section offers the model and Process makes a picture per output frame (card 133)",
          "[app][ai][video][interpolation]") {
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
    const QString section = QStringLiteral("i%1").arg(clip.value);
    controller->selectClipById(clip, ProjectController::Replace);

    auto cache = std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-interp-cache"));
    auto backend = std::make_shared<avt::FakeBackend>();
    ai->setServices(cache, backend, [&](const std::string&, int) -> std::unique_ptr<ai::video::IFrameSource> {
        return std::make_unique<SmallSource>(1000);
    });
    const std::filesystem::path models = avt::freshDir("ui-interp-models");
    ai->setModelRoot(models);
    ai->setHelperAvailable(true);
    ai->setSynchronous(true);

    // The clip plays at full speed with Frame Sampling: it has no such section.
    CHECK_FALSE(ai->effectInfo(section).value(QStringLiteral("ai")).toBool());
    // Half speed and AI Frame Interpolation.
    edit::SpeedChange slow;
    slow.speed = Speed{1, 2};
    REQUIRE(controller->session()
                .apply("Speed", [&](const ProjectState& s) { return edit::setSpeed(s, seq, std::span(&clip, 1), slow); })
                .has_value());
    REQUIRE(controller->session()
                .apply("Time Interpolation",
                       [&](const ProjectState& s) {
                           return edit::setTimeInterpolation(s, seq, std::span(&clip, 1), TimeInterpolation::AiInterpolation);
                       })
                .has_value());
    QVariantMap info = ai->effectInfo(section);
    REQUIRE(info.value(QStringLiteral("ai")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("interpolation"));
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK(info.value(QStringLiteral("models")).toStringList() == QStringList{QStringLiteral("RIFE frame interpolation")});
    CHECK(info.value(QStringLiteral("downloadText")).toString().contains(QStringLiteral("23 MB")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("MIT")));
    const int frames = info.value(QStringLiteral("frames")).toInt();
    CHECK(frames == static_cast<int>(controller->currentSequence()->findClip(clip)->clip().duration().value /
                                     framesToTicks(1, Rate{25, 1}).value)); // one picture per output frame

    installModel(models, ai::video::Model::Interpolate);
    ai->setModelRoot(models);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(info.value(QStringLiteral("message")).toString().contains(QStringLiteral("Optical Flow")));
    REQUIRE(ai->process(section));
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    // Every other output frame falls between two frames at half speed: the model ran for those, the rest are stubs.
    CHECK(backend->interpolations.size() >= static_cast<std::size_t>(frames / 2 - 1));
    CHECK(backend->interpolations.size() <= static_cast<std::size_t>(frames / 2 + 1));
    for (const auto& call : backend->interpolations) {
        CHECK(call.weight == Catch::Approx(0.5).margin(0.02));
    }
    // The sequence's overview and the export notice count it with the other AI work.
    CHECK(ai->overview().value(QStringLiteral("count")).toInt() == 1);
    CHECK(ai->overview().value(QStringLiteral("pending")).toInt() == 0);
    // Back to Optical Flow: the section is gone and the pictures stay in the cache for when it is chosen again.
    REQUIRE(controller->session()
                .apply("Time Interpolation",
                       [&](const ProjectState& s) {
                           return edit::setTimeInterpolation(s, seq, std::span(&clip, 1), TimeInterpolation::OpticalFlow);
                       })
                .has_value());
    CHECK_FALSE(ai->effectInfo(section).value(QStringLiteral("ai")).toBool());
    CHECK(ai->overview().value(QStringLiteral("count")).toInt() == 0);
}

TEST_CASE("AI Refocus and AI Frame Interpolation in the shell: the effect's header and the Speed/Duration dialog (screenshots)",
          "[app][ai][video][shot]") {
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

    Timeline tl("V1 |Aaaaaaaaaaaaaaaaaaaaaaaa|\nA1 |Aaaaaaaaaaaaaaaaaaaaaaaa|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    const ClipId clip = clipAt(*controller->snapshot(), tl.seq, "V1@0");
    const SequenceId seq = tl.seq;
    REQUIRE(controller->session()
                .apply("Apply Effect",
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiRefocusEffect)); })
                .has_value());
    edit::SpeedChange slow;
    slow.speed = Speed{1, 2};
    REQUIRE(controller->session()
                .apply("Speed", [&](const ProjectState& s) { return edit::setSpeed(s, seq, std::span(&clip, 1), slow); })
                .has_value());
    REQUIRE(controller->session()
                .apply("Time Interpolation",
                       [&](const ProjectState& s) {
                           return edit::setTimeInterpolation(s, seq, std::span(&clip, 1), TimeInterpolation::AiInterpolation);
                       })
                .has_value());
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    ai->setServices(std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-cache-4")), std::make_shared<avt::FakeBackend>());
    ai->setHelperAvailable(true);
    ai->setModelRoot(avt::freshDir("ui-models-4")); // empty: both models are offered for download

    // Effect Controls: the AI Refocus header offers the depth model (Download with its size), and its rows are there.
    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    REQUIRE(fx.rowOf(QStringLiteral("h:") + section) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + section)), Q_ARG(int, 0)));
    spin(80);
    QQuickItem* download = nullptr;
    CHECK(waitFor([&] { return (download = visibleNamed(shell, QStringLiteral("aiVideoDownload"))) != nullptr; }));
    REQUIRE(download != nullptr);
    CHECK(download->property("text").toString() == QStringLiteral("Download 99 MB"));
    QQuickItem* status = visibleNamed(shell, QStringLiteral("aiVideoStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->property("text").toString() == QStringLiteral("Model needed"));
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                    .save(outputDir() + QStringLiteral("/effect-controls-ai-refocus-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
    }
    shell.setAppearance(true, "ultraviolet");

    // The clip menu's Time Interpolation has the fourth choice, checked for this clip.
    QObject* submenu = shell.object("timeInterpolationSubmenu");
    REQUIRE(submenu != nullptr);
    REQUIRE(QMetaObject::invokeMethod(submenu, "refresh"));
    CHECK(submenu->property("current").toString() == QStringLiteral("aiInterpolation"));
    if (QObject* aiItem = submenu->findChild<QObject*>(QStringLiteral("aiInterpolationItem"))) { // the menu's items live as its children
        CHECK(aiItem->property("text").toString() == QStringLiteral("AI Frame Interpolation"));
        CHECK(aiItem->property("checked").toBool());
    }
    // Choosing it from the menu is the same edit as the dialog's: from Optical Flow back to AI Frame Interpolation, one undo step.
    REQUIRE(QMetaObject::invokeMethod(submenu, "choose", Q_ARG(QVariant, QStringLiteral("opticalFlow"))));
    CHECK(controller->currentSequence()->findClip(clip)->clip().timeInterpolation() == TimeInterpolation::OpticalFlow);
    REQUIRE(QMetaObject::invokeMethod(submenu, "choose", Q_ARG(QVariant, QStringLiteral("aiInterpolation"))));
    CHECK(submenu->property("error").toString().isEmpty());
    CHECK(controller->currentSequence()->findClip(clip)->clip().timeInterpolation() == TimeInterpolation::AiInterpolation);
    CHECK(submenu->property("current").toString() == QStringLiteral("aiInterpolation"));

    // The Speed/Duration dialog: the fourth tab, what the model does, and the clip's state with Download.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("clip.speedDuration"))));
    QObject* dialog = shell.object("speedDurationDialog");
    REQUIRE(dialog != nullptr);
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
    CHECK(dialog->property("interpolation").toString() == QStringLiteral("aiInterpolation"));
    QQuickItem* tabs = namedAny(shell, QStringLiteral("timeInterpolationTabs"));
    REQUIRE(tabs != nullptr);
    CHECK(tabs->property("model").toStringList().size() == 4);
    CHECK(tabs->property("model").toStringList().last() == QStringLiteral("AI Frame Interpolation"));
    CHECK(tabs->property("currentIndex").toInt() == 3);
    QQuickItem* hint = nullptr;
    CHECK(waitFor([&] { return (hint = visibleNamed(shell, QStringLiteral("aiInterpolationHint"))) != nullptr; }));
    REQUIRE(hint != nullptr);
    CHECK(hint->property("text").toString().contains(QStringLiteral("Optical Flow")));
    QQuickItem* statusRow = nullptr;
    CHECK(waitFor([&] { return (statusRow = visibleNamed(shell, QStringLiteral("aiInterpolationStatus"))) != nullptr; }));
    REQUIRE(statusRow != nullptr);
    {
        // Its own Download button (the interpolation model, 23 MB), beside the state text.
        const std::function<QQuickItem*(QQuickItem*, const QString&)> inside = [&](QQuickItem* item, const QString& name) -> QQuickItem* {
            for (QQuickItem* child : item->childItems()) {
                if (child->objectName() == name)
                    return child;
                if (QQuickItem* found = inside(child, name))
                    return found;
            }
            return nullptr;
        };
        QQuickItem* ownDownload = inside(statusRow, QStringLiteral("aiVideoDownload"));
        REQUIRE(ownDownload != nullptr);
        CHECK(ownDownload->isVisible());
        CHECK(ownDownload->property("text").toString() == QStringLiteral("Download 23 MB"));
        QQuickItem* ownState = inside(statusRow, QStringLiteral("aiVideoStatus"));
        REQUIRE(ownState != nullptr);
        CHECK(ownState->property("text").toString() == QStringLiteral("Model needed"));
    }
    for (const bool dark : {true, false}) {
        shell.setAppearance(dark, "ultraviolet");
        REQUIRE(shell.settle(dark, shell.themeColor("accentFill")));
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(outputDir() + QStringLiteral("/speed-duration-ai-interpolation-%1.png").arg(dark ? QStringLiteral("dark") : QStringLiteral("light"))));
    }
    shell.setAppearance(true, "ultraviolet");
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

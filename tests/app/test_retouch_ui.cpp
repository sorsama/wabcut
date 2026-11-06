// Skin Smoothing (card 100) and AI Face Retouch (card 132) in the shell, through the real Main.qml offscreen. Skin Smoothing is an ordinary
// effect (its rows are in Effect Controls, edits are single undo steps); AI Face Retouch is a clip effect whose Effect Controls header says
// what it needs (the two face models to download, with the licence card), Process runs the runner over the clip's media (a fake backend and
// frame source here: no model, no helper) and the header says what is done. Screenshots in the dark and the light theme.

#include "ShellFixture.h"

#include "AiVideoBridge.h"
#include "EffectControls.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "video/VideoTestUtil.h"

#include <wabcut/ai/ModelStore.h>
#include <wabcut/ai/video/Targets.h>
#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/FinishingOps.h>

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
void installStandIn(const std::filesystem::path& root, ai::video::Model m) {
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

TEST_CASE("AI Face Retouch: the header offers both face models, Process makes the face maps and says so (card 132)", "[app][ai][video][faces]") {
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
                       [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(kAiFaceRetouchEffect)); })
                .has_value());
    CHECK(controller->session().undoCount() == undoBefore + 1); // adding it is one undo step
    const EffectInstanceId effect = controller->currentSequence()->findClip(clip)->clip().effects().front().id;
    const QString section = QStringLiteral("e%1").arg(effect.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});

    auto cache = std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-faces-cache"));
    auto backend = std::make_shared<avt::FakeBackend>();
    ai->setServices(cache, backend, [&](const std::string&, int) -> std::unique_ptr<ai::video::IFrameSource> {
        return std::make_unique<TinySource>(1000);
    });
    const std::filesystem::path models = avt::freshDir("ui-faces-models");
    ai->setModelRoot(models);
    ai->setHelperAvailable(true);
    ai->setSynchronous(true);

    // The effect shows the AI header: two models (the detector is tiny), MIT, with the licence card that names the training data.
    QVariantMap info = ai->effectInfo(section);
    REQUIRE(info.value(QStringLiteral("ai")).toBool());
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("faceRetouch"));
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK(info.value(QStringLiteral("models")).toStringList() ==
          QStringList{QStringLiteral("YuNet face detector"), QStringLiteral("BiSeNet face parsing")});
    CHECK(info.value(QStringLiteral("downloadText")).toString().contains(QStringLiteral("53 MB")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("MIT")));
    CHECK(info.value(QStringLiteral("card")).toString().contains(QStringLiteral("CelebAMask-HQ")));
    CHECK(info.value(QStringLiteral("label")).toString() == QStringLiteral("Face maps"));
    CHECK_FALSE(ai->process(section)); // nothing runs without the models

    // One of the two is not enough.
    installStandIn(models, ai::video::Model::FaceParse);
    ai->setModelRoot(models);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("needsModel"));
    CHECK_FALSE(ai->process(section));

    // Both are there: no face maps yet, and the message says what the effect does meanwhile.
    installStandIn(models, ai::video::Model::FaceDetect);
    ai->setModelRoot(models);
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("notProcessed"));
    CHECK(info.value(QStringLiteral("message")).toString().contains(QStringLiteral("face maps")));

    // Process: one face map per frame of the clip, three 8-bit planes, and the header says Processed.
    REQUIRE(ai->process(section));
    info = ai->effectInfo(section);
    CHECK(info.value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(info.value(QStringLiteral("processed")).toInt() == info.value(QStringLiteral("frames")).toInt());
    REQUIRE_FALSE(backend->calls.empty());
    CHECK(backend->calls.front().model == ai::video::Model::FaceParse);
    const auto target = ai::video::faceTarget(*controller->snapshot(), *controller->currentSequence(),
                                              controller->currentSequence()->findClip(clip)->clip(), 0);
    REQUIRE(target.has_value());
    const auto stored = cache->get(target->identity, Ticks{}, framesToTicks(1, Rate{25, 1}));
    REQUIRE(stored);
    REQUIRE(stored->planes.size() == 3);
    CHECK(stored->planes[0].bytesPerTexel == 1);
    const std::size_t callsAfterProcess = backend->calls.size();

    // Moving a slider is one undo step and does not make the maps again (they belong to the picture): the state stays ready.
    const std::size_t undoNow = controller->session().undoCount();
    edit::EffectChange tuned;
    tuned.params = {EffectParam{"smoothing", ParamValue(80.0)}, EffectParam{"teeth", ParamValue(70.0)}};
    REQUIRE(controller->session()
                .apply("Retouch", [&](const ProjectState& s) { return edit::setEffect(s, seq, clip, effect, tuned); })
                .has_value());
    CHECK(controller->session().undoCount() == undoNow + 1);
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    CHECK(backend->calls.size() == callsAfterProcess);
    REQUIRE(controller->undo());
    CHECK(ai->effectInfo(section).value(QStringLiteral("state")).toString() == QStringLiteral("ready"));
    // Delete: the maps go, and Process can make them again.
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

TEST_CASE("Skin Smoothing and AI Face Retouch in the shell: Effect Controls rows and the AI header (screenshots)", "[app][ai][video][shot][retouch]") {
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
    for (const char* id : {"wabcut.retouch.skinSmoothing", "wabcut.ai.faceRetouch"}) {
        REQUIRE(controller->session()
                    .apply("Apply Effect", [&](const ProjectState& s) { return edit::addEffect(s, seq, std::span(&clip, 1), std::string(id)); })
                    .has_value());
    }
    REQUIRE(controller->currentSequence()->findClip(clip)->clip().effects().size() == 2);
    const QString skinSection = QStringLiteral("e%1").arg(controller->currentSequence()->findClip(clip)->clip().effects()[0].id.value);
    const QString faceSection = QStringLiteral("e%1").arg(controller->currentSequence()->findClip(clip)->clip().effects()[1].id.value);
    controller->selectClipById(clip, ProjectController::Replace);
    controller->setPlayhead(Ticks{});
    ai->setServices(std::make_shared<ai::video::ProcessedCache>(avt::freshDir("ui-cache-faces-shot")), std::make_shared<avt::FakeBackend>());
    ai->setHelperAvailable(true);
    ai->setModelRoot(avt::freshDir("ui-models-faces-shot")); // empty: both face models are offered for download

    REQUIRE_FALSE(shell.grab().isNull());
    EffectControlsModel& fx = *controller->effectControls();
    QObject* list = shell.object("effectControlsList");
    REQUIRE(list != nullptr);
    QQuickItem* panel = shell.item("effectControls");
    REQUIRE(panel != nullptr);
    // Skin Smoothing is an ordinary effect: its header is not the AI one, its rows are all there (Show is a menu, the rest sliders).
    REQUIRE(fx.rowOf(QStringLiteral("h:") + skinSection) >= 0);
    CHECK_FALSE(ai->effectInfo(skinSection).value(QStringLiteral("ai")).toBool());
    const auto shoot = [&](const QString& section, const QString& name, int rowsBelow = 0) {
        REQUIRE(fx.rowOf(QStringLiteral("h:") + section) >= 0);
        REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + section) + rowsBelow), Q_ARG(int, 0)));
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
    shoot(skinSection, QStringLiteral("effect-controls-skin-smoothing"));
    shoot(skinSection, QStringLiteral("effect-controls-skin-smoothing-more"), 3); // Size, Skin Tone, Tone Range, Edge Softness, Show
    // AI Face Retouch: its header offers the two models (one Download with the total size), and its rows follow.
    REQUIRE(fx.rowOf(QStringLiteral("h:") + faceSection) >= 0);
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + faceSection)), Q_ARG(int, 0)));
    spin(80);
    QQuickItem* download = nullptr;
    CHECK(waitFor([&] { return (download = visibleNamed(shell, QStringLiteral("aiVideoDownload"))) != nullptr; }));
    REQUIRE(download != nullptr);
    CHECK(download->property("text").toString() == QStringLiteral("Download 53 MB"));
    QQuickItem* status = visibleNamed(shell, QStringLiteral("aiVideoStatus"));
    REQUIRE(status != nullptr);
    CHECK(status->property("text").toString() == QStringLiteral("Model needed"));
    shoot(faceSection, QStringLiteral("effect-controls-ai-face-retouch"));
    shoot(faceSection, QStringLiteral("effect-controls-ai-face-retouch-more"), 3); // Smoothing Size, Blemish Removal, Blemish Size, Teeth, Eyes, Show
    // The effect's name is not cut in the header at this panel width: the status gives way (it elides), the name does not.
    REQUIRE(QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, fx.rowOf(QStringLiteral("h:") + faceSection)), Q_ARG(int, 0)));
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
            if (title->property("text").toString() == QStringLiteral("AI Face Retouch")) {
                whole = title->width() >= title->property("implicitWidth").toDouble() - 0.5;
            }
        }
    }
    CHECK(whole);
}

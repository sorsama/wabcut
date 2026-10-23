// AI-11 in the app, on the UI shell with fixtures (transcripts with several takes of each line, a synthetic screen
// recording; no model, no decoder): the "Rough cut" sheet makes a cut from a script, shows the alignment report, swaps a
// take and creates the sequence as one undo step; the Takes panel finds repeated lines and stages "keep best takes";
// the Auto zoom dialog previews the zoom window over the recording and applies it. The sheet, panel and dialog are made
// from the Wabcut module directly (their menu hooks are Main.qml's). Screenshots: <build>/test-output/ui/
// rough-cut-report.png, takes-panel.png and auto-zoom-preview.png.

#include "ShellFixture.h"

#include "BridgeIds.h"
#include "CreatorBridge.h"
#include "ProjectController.h"
#include "ShortsBridge.h"
#include "creator/CreatorTestUtil.h"
#include "creator/ScreenRecording.h"

#include <wabcut/ai/autozoom/AutoZoomCommands.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/ai/roughcut/RoughCut.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QPainter>
#include <QQmlComponent>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QUrl>

#include <cmath>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using wabcut::ai::autozoom::test::ScreenRecording;

namespace {

void loadInto(ProjectController* controller, const ProjectState& state, const QString& name) {
    const QString path = outputDir() + "/" + name;
    REQUIRE(io::saveProject(state, std::filesystem::path(path.toStdWString())));
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(path)));
}

/// Gives the fixture's media and sequence readable names for the screenshots.
void rename(ProjectState& state, std::initializer_list<std::pair<MediaId, const char*>> media, SequenceId sequence, const char* name) {
    for (MediaPtr& m : state.media) {
        for (const auto& [id, label] : media) {
            if (m->id == id) {
                auto renamed = std::make_shared<MediaItem>(*m);
                renamed->name = label;
                m = renamed;
            }
        }
    }
    auto seq = std::make_shared<Sequence>(*state.findSequence(sequence));
    seq->name = name;
    state = state.withSequence(seq);
}

/// A drawn frame of a talking-head recording: a backdrop in the media's own colour and the speaker.
QImage studioFrame(const MediaItem& media, Ticks time) {
    const bool b = media.name.find("B-cam") != std::string::npos;
    QImage image(320, 180, QImage::Format_RGB888);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient wall(0, 0, 320, 180);
    wall.setColorAt(0, b ? QColor(70, 52, 44) : QColor(38, 46, 70));
    wall.setColorAt(1, b ? QColor(32, 24, 22) : QColor(18, 20, 32));
    p.fillRect(image.rect(), wall);
    p.setPen(Qt::NoPen);
    const double x = (b ? 190.0 : 140.0) + 6.0 * std::sin(time.toSeconds());
    p.setBrush(b ? QColor(160, 90, 80) : QColor(70, 110, 170));
    p.drawRoundedRect(QRectF(x - 34, 92, 68, 100), 22, 22);
    p.setBrush(QColor(224, 172, 140));
    p.drawEllipse(QPointF(x, 70), 17, 21);
    return image;
}

/// One of the Wabcut module's components, made in the shell's window.
QObject* make(Shell& shell, const char* type, QVariantMap properties = {}) {
    QQmlComponent component(&shell.engine);
    component.loadFromModule("Wabcut", type);
    INFO(component.errorString().toStdString());
    REQUIRE(component.isReady());
    properties.insert(QStringLiteral("parent"), QVariant::fromValue(shell.window->contentItem()));
    QObject* object = component.createWithInitialProperties(properties);
    REQUIRE(object != nullptr);
    object->setParent(shell.window.get());
    return object;
}

/// The shell's picture cropped to an item.
QImage crop(Shell& shell, const QImage& image, QQuickItem* item) {
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    return image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect());
}

} // namespace

TEST_CASE("Rough cut sheet: the alignment report, a swapped take and the cut as one undo step; screenshot",
          "[ai][ui][creator][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* roughCut = shell.engine.singletonInstance<RoughCutBridge*>("Wabcut", "RoughCut");
    REQUIRE(controller != nullptr);
    REQUIRE(roughCut != nullptr);

    ai::roughcut::test::Studio studio(ai::roughcut::test::launchTakes());
    ProjectState state = studio.tl.state;
    rename(state, {{studio.tl.media('A'), "Studio A-cam.mov"}, {studio.tl.media('B'), "Studio B-cam.mov"}}, studio.tl.seq, "Launch video");
    loadInto(controller, state, QStringLiteral("roughcut-fixture.wabcut"));
    roughCut->setServices(studio.services);
    roughCut->setSynchronous(true);
    setPreviewFramesForTests(studioFrame);
    shell.resize(1600, 1100);

    QObject* sheet = make(shell, "RoughCutSheet");
    REQUIRE(QMetaObject::invokeMethod(sheet, "open"));
    REQUIRE(waitFor([&] { return sheet->property("opened").toBool(); }));
    auto* script = sheet->findChild<QObject*>("roughCutScript");
    REQUIRE(script != nullptr);
    script->setProperty("text", QString::fromUtf8(ai::roughcut::test::kLaunchScript));
    REQUIRE(QMetaObject::invokeMethod(sheet, "make"));
    REQUIRE(roughCut->waitForIdle(20'000));
    INFO("Qt messages: " << join(takeMessages()));
    INFO(roughCut->message().toStdString());
    const QVariantMap report = roughCut->report();
    const QVariantList beats = report.value(QStringLiteral("beats")).toList();
    REQUIRE(beats.size() == 5);
    CHECK(report.value(QStringLiteral("found")).toInt() == 5);
    const QVariantList editorTakes = beats[1].toMap().value(QStringLiteral("takes")).toList();
    REQUIRE(editorTakes.size() == 3);
    CHECK(editorTakes.front().toMap().value(QStringLiteral("chosen")).toBool());
    CHECK(roughCut->staged());

    // Click an alternate: a staged edit, the report follows.
    CHECK(roughCut->swapTake(2, 2).isEmpty());
    REQUIRE(roughCut->waitForIdle(20'000));
    const QVariantList swapped = roughCut->report().value(QStringLiteral("beats")).toList()[1].toMap().value(QStringLiteral("takes")).toList();
    REQUIRE(swapped.size() == 3);
    CHECK(swapped[1].toMap().value(QStringLiteral("chosen")).toBool());

    auto* list = sheet->findChild<QQuickItem*>("roughCutReport");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == 5; }));
    spin(800);
    const QImage image = shell.grab();
    REQUIRE(image.save(outputDir() + QStringLiteral("/rough-cut-report.png")));

    const std::size_t before = controller->snapshot()->sequences.size();
    CHECK(roughCut->apply().isEmpty());
    CHECK(controller->snapshot()->sequences.size() == before + 1);
    const Sequence* made = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(made != nullptr);
    REQUIRE(made->derivedFrom);
    CHECK(made->derivedFrom->generator == "roughcut/1");
    CHECK(ai::roughcut::recordOfSequence(*made)->beats[1].chosen == 1);
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->sequences.size() == before);
    setPreviewFramesForTests({});
    delete sheet;
}

TEST_CASE("Takes panel: repeated lines grouped with word diffs and quality; keep best takes staged and applied; screenshot",
          "[ai][ui][creator][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* takes = shell.engine.singletonInstance<TakesBridge*>("Wabcut", "Takes");
    REQUIRE(controller != nullptr);
    REQUIRE(takes != nullptr);

    // A 40 s recording cut into the sequence whole: false starts, a messy take, a clean one.
    Timeline tl("V1 |A" + std::string(999, 'a') + "|\nA1 |A" + std::string(999, 'a') + "|", {{'A', 1100}});
    tl.linkMatching();
    std::vector<ai::edit::test::W> words;
    using ai::roughcut::test::say;
    say(words, 1.0, "Welcome back to the studio everyone.");
    say(words, 4.0, "So the new...");
    say(words, 6.0, "So the new timeline um is uh much faster.");
    say(words, 10.0, "So the new timeline is much faster.");
    say(words, 14.0, "We tested the first version yesterday.");
    say(words, 22.0, "Exports finish in half the time now.");
    say(words, 26.0, "Exports finish in half the, the time now.");
    say(words, 31.0, "Thanks for watching and see you soon.");
    auto transcripts = ai::edit::test::provider(tl, {{'A', ai::edit::test::transcript("en", words)}});
    auto services = std::make_shared<command::ServiceBundle>(
        std::vector<std::shared_ptr<command::CommandServices>>{std::make_shared<ai::edit::TranscriptServices>(transcripts)});
    ProjectState state = tl.state;
    rename(state, {{tl.media('A'), "Studio A-cam.mov"}}, tl.seq, "Episode 14");
    loadInto(controller, state, QStringLiteral("takes-fixture.wabcut"));
    takes->setServices(services);
    takes->setSynchronous(true);
    shell.resize(1600, 1300);

    auto* panel = qobject_cast<QQuickItem*>(make(shell, "TakesPanel", {{QStringLiteral("width"), 560}, {QStringLiteral("height"), 1180}}));
    REQUIRE(panel != nullptr);
    panel->setPosition(QPointF(40, 40));
    panel->setZ(1000);

    CHECK(takes->find({}).isEmpty());
    REQUIRE(takes->waitForIdle(20'000));
    INFO(takes->message().toStdString());
    const QVariantList groups = takes->groups();
    REQUIRE(groups.size() >= 2);
    const QVariantMap timeline = groups.front().toMap();
    CHECK(timeline.value(QStringLiteral("takes")).toList().size() >= 2);
    const QVariantMap best = timeline.value(QStringLiteral("takes")).toList().front().toMap();
    CHECK(best.value(QStringLiteral("best")).toBool());
    CHECK(best.value(QStringLiteral("fillers")).toInt() == 0);
    CHECK_FALSE(timeline.value(QStringLiteral("takes")).toList()[1].toMap().value(QStringLiteral("diff")).toList().isEmpty());

    // Keep best takes: one staged edit with its review list.
    CHECK(takes->useBest().isEmpty());
    REQUIRE(takes->waitForIdle(20'000));
    REQUIRE(takes->staged());
    CHECK(takes->review().size() >= 2);
    auto* list = panel->findChild<QQuickItem*>("takesList");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == groups.size(); }));
    spin(600);
    const QImage image = shell.grab();
    REQUIRE(crop(shell, image, panel).save(outputDir() + QStringLiteral("/takes-panel.png")));

    const std::size_t clipsBefore = controller->snapshot()->findSequence(controller->currentSequenceId())->audioTracks.front()->clips.size();
    CHECK(takes->apply().isEmpty());
    const Sequence* after = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(after != nullptr);
    CHECK(after->audioTracks.front()->clips.size() > clipsBefore); // the repeats cut out
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->findSequence(controller->currentSequenceId())->audioTracks.front()->clips.size() == clipsBefore);
    delete panel;
}

TEST_CASE("Auto zoom dialog: the zoom window over the recording, applied as one undo step; screenshot", "[ai][ui][creator][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* zoom = shell.engine.singletonInstance<AutoZoomBridge*>("Wabcut", "AutoZoom");
    REQUIRE(controller != nullptr);
    REQUIRE(zoom != nullptr);

    Timeline tl("V1 |A" + std::string(499, 'a') + "|\nA1 |A" + std::string(499, 'a') + "|", {{'A', 600}});
    tl.linkMatching();
    ProjectState state = tl.state;
    rename(state, {{tl.media('A'), "Screen recording 2026-10-12.mp4"}}, tl.seq, "Tutorial");
    loadInto(controller, state, QStringLiteral("autozoom-fixture.wabcut"));
    static const ScreenRecording recording;
    auto frames = [](const MediaItem&, int) -> std::expected<ai::autozoom::FrameReader, Error> { return recording.reader(); };
    zoom->setServices(std::make_shared<ai::autozoom::AutoZoomServices>(std::move(frames)));
    zoom->setSynchronous(true);
    setPreviewFramesForTests([](const MediaItem&, Ticks t) {
        const std::vector<std::uint8_t> rgb = recording.frame(t.toSeconds());
        return QImage(rgb.data(), ScreenRecording::kWidth, ScreenRecording::kHeight, ScreenRecording::kWidth * 3, QImage::Format_RGB888)
            .scaled(640, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    });
    shell.resize(1600, 1000);

    const Sequence* seq = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(seq != nullptr);
    const ClipId clip = seq->videoTracks.front()->clips.front().id;
    QObject* dialog = make(shell, "AutoZoomDialog");
    REQUIRE(QMetaObject::invokeMethod(dialog, "openFor", Q_ARG(QVariant, clipKey(clip))));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(zoom->waitForIdle(30'000));
    INFO("Qt messages: " << join(takeMessages()));
    INFO(zoom->message().toStdString());
    const QVariantMap preview = zoom->zoomPreview();
    const QVariantList path = preview.value(QStringLiteral("path")).toList();
    REQUIRE(path.size() == 200);
    CHECK(preview.value(QStringLiteral("segments")).toList().size() >= 2);
    CHECK(path[40].toMap().value(QStringLiteral("zoom")).toDouble() > 1.5); // 4 s: on the button
    dialog->setProperty("sample", 40);
    spin(1500); // frames decode asynchronously; the popup transition
    const QImage image = shell.grab();
    REQUIRE(image.save(outputDir() + QStringLiteral("/auto-zoom-preview.png")));
    auto* window = dialog->findChild<QQuickItem*>("autoZoomWindow");
    auto* stage = dialog->findChild<QQuickItem*>("autoZoomPreview");
    REQUIRE(window != nullptr);
    REQUIRE(stage != nullptr);
    CHECK(window->isVisible());
    CHECK(window->width() < stage->width() * 0.7); // zoomed in: the window is a part of the picture
    CHECK(window->x() > stage->width() * 0.4);    // on the right, where the button is

    CHECK(zoom->apply().isEmpty());
    const Clip& zoomed = controller->snapshot()->findSequence(controller->currentSequenceId())->findClip(clip)->clip();
    CHECK(zoomed.properties().value(ParamId::Scale, Ticks{kTicksPerSecond * 4}) > 1.5);
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->findSequence(controller->currentSequenceId())->findClip(clip)->clip().properties().value(
              ParamId::Scale, Ticks{kTicksPerSecond * 4}) == 1.0);
    setPreviewFramesForTests({});
    delete dialog;
}


// The frames of the creator previews (and of Shorts and Reframe) are decoded straight from the file, outside the
// compositor: the media's source redactions apply to them as to the library's thumbnails (review wave 4, P2-9).
TEST_CASE("Preview frames show the media's redactions", "[ai][ui][creator][security]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller != nullptr);
    Timeline tl("V1 |A" + std::string(99, 'a') + "|\nA1 |A" + std::string(99, 'a') + "|", {{'A', 200}});
    Redaction r;
    r.media = tl.media('A');
    r.source = TimeRange{Ticks{}, Ticks::fromSeconds(4)};
    r.style = RedactionStyle::Solid;
    r.colorR = 1.0;
    r.colorB = 1.0;
    r.region.shape = MaskShape::Ellipse;
    r.region.set("size.x", 0.4);
    r.region.set("size.y", 0.4);
    auto added = edit::addRedaction(tl.state, r);
    REQUIRE(added);
    loadInto(controller, added->state, QStringLiteral("redacted-frames.wabcut"));
    (void)shell.engine.singletonInstance<ShortsBridge*>("Wabcut", "Shorts"); // registers image://wabcut-frames
    setPreviewFramesForTests([](const MediaItem&, Ticks) {
        QImage white(320, 180, QImage::Format_RGB888);
        white.fill(Qt::white);
        return white;
    });
    setPreviewFrameContext(controller->snapshot(), {});
    auto* provider = dynamic_cast<QQuickImageProvider*>(shell.engine.imageProvider(QStringLiteral("wabcut-frames")));
    REQUIRE(provider != nullptr);
    QSize size;
    const auto at = [&](double seconds) {
        return provider->requestImage(mediaKey(tl.media('A')) + QStringLiteral("/") + QString::number(Ticks::fromSeconds(seconds).value),
                                      &size, QSize());
    };
    const QImage inside = at(1.0);
    REQUIRE_FALSE(inside.isNull());
    const QColor centre = inside.pixelColor(160, 90);
    INFO(centre.name().toStdString());
    CHECK(centre.red() > 200);
    CHECK(centre.green() < 60);
    CHECK(centre.blue() > 200);
    CHECK(inside.pixelColor(8, 8) == QColor(Qt::white));
    CHECK(at(6.0).pixelColor(160, 90) == QColor(Qt::white)); // after the redacted range
    setPreviewFramesForTests({});
}

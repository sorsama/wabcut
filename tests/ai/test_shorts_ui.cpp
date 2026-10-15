// AI-9 in the app, on the real Main.qml with fixtures (a synthetic podcast, its transcript, scripted subjects, drawn
// frames; no model, no decoder): the Shorts panel finds shorts (cards with frames, score bars, titles) and makes two
// as one undo step; the Reframe dialog previews the frame path over the source and creates the 9:16 sequence.
// Screenshots: <build>/test-output/ui/shorts-panel.png, shorts-window.png and reframe-dialog.png.

#include "ShellFixture.h"

#include "ProjectController.h"
#include "ShortsBridge.h"
#include "shorts/ShortsTestUtil.h"

#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/reframe/ReframeCommands.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QPainter>
#include <QQuickItem>
#include <QUrl>

#include <cmath>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using namespace wabcut::ai::shorts::test;

namespace {

/// The podcast on disk (media ids survive the save), its transcript and its subjects: two people at a table, the one
/// talking switching every 8 seconds.
struct Studio {
    Studio() : podcast(makePodcast()), tl(avPicture(static_cast<int>(std::ceil(podcast.seconds * 25.0))), {{'A', static_cast<std::int64_t>(std::ceil(podcast.seconds * 25.0)) + 10}}) {
        tl.linkMatching();
        const MediaId media = tl.media('A');
        transcripts = ai::edit::test::provider(tl, {{'A', ai::edit::test::transcript("en", podcast.words)}});
        auto subjects = std::make_shared<ai::vision::MapSubjectProvider>();
        subjects->add(media, analysis());
        // Names for the screenshots (after every lookup by letter).
        for (MediaPtr& m : tl.state.media) {
            if (m->id == media) {
                auto renamed = std::make_shared<MediaItem>(*m);
                renamed->name = "Podcast Ep 12.mov";
                m = renamed;
            }
        }
        auto seq = std::make_shared<Sequence>(*tl.state.findSequence(tl.seq));
        seq->name = "Podcast Ep 12";
        tl.state = tl.state.withSequence(seq);
        services = std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
            std::make_shared<ai::edit::TranscriptServices>(transcripts), std::make_shared<ai::reframe::ReframeServices>(subjects)});
    }

    ai::vision::SubjectAnalysis analysis() const {
        ai::vision::SubjectAnalysis a;
        a.models = "yunet-2023mar-8f2383e4+yolox-tiny-427cc366/v1";
        a.frameWidth = 1920;
        a.frameHeight = 1080;
        a.end = Ticks{static_cast<std::int64_t>(podcast.seconds * static_cast<double>(kTicksPerSecond))};
        for (double t = 0.1; t < podcast.seconds; t += 0.2) {
            ai::vision::SubjectSample s;
            s.time = Ticks{static_cast<std::int64_t>(t * static_cast<double>(kTicksPerSecond))};
            const bool left = static_cast<int>(t / 8.0) % 2 == 0;
            s.faces = {ai::vision::SubjectBox{ai::vision::Box{static_cast<float>(hostX(t) - 0.05), 0.24f, 0.1f, 0.2f}, 0.92f, 1, left ? 0.08f : 0.0f},
                       ai::vision::SubjectBox{ai::vision::Box{0.67f, 0.26f, 0.1f, 0.2f}, 0.9f, 2, left ? 0.0f : 0.08f}};
            s.people = {ai::vision::SubjectBox{ai::vision::Box{static_cast<float>(hostX(t) - 0.11), 0.2f, 0.22f, 0.8f}, 0.85f, 3, 0.0f},
                        ai::vision::SubjectBox{ai::vision::Box{0.61f, 0.22f, 0.22f, 0.78f}, 0.85f, 4, 0.0f}};
            a.samples.push_back(std::move(s));
        }
        return a;
    }
    /// The host leans about over time.
    static double hostX(double t) { return 0.3 + 0.03 * std::sin(t / 5.0); }

    /// A drawn frame of the studio at source time t: a lit backdrop, a table and the two people.
    static QImage frame(Ticks time) {
        const double t = time.toSeconds();
        QImage image(640, 360, QImage::Format_RGB888);
        QPainter p(&image);
        p.setRenderHint(QPainter::Antialiasing);
        QLinearGradient wall(0, 0, 640, 360);
        wall.setColorAt(0, QColor(38, 44, 66));
        wall.setColorAt(1, QColor(18, 20, 32));
        p.fillRect(image.rect(), wall);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 196, 120, 40));
        p.drawEllipse(QPointF(500, 60), 160, 90); // a warm practical light
        const auto person = [&](double cx, QColor shirt, bool talking) {
            p.setBrush(shirt);
            p.drawRoundedRect(QRectF(cx * 640 - 62, 150, 124, 230), 40, 40);
            p.setBrush(QColor(224, 172, 140));
            p.drawEllipse(QPointF(cx * 640, 122), 30, 38);
            p.setBrush(QColor(60, 40, 40));
            p.drawEllipse(QPointF(cx * 640, 140), talking ? 9 : 7, talking ? 6 : 2);
        };
        const bool left = static_cast<int>(t / 8.0) % 2 == 0;
        person(hostX(t), QColor(70, 110, 170), left && static_cast<int>(t * 4) % 2 == 0);
        person(0.72, QColor(170, 90, 80), !left && static_cast<int>(t * 4) % 2 == 0);
        p.setBrush(QColor(92, 62, 44));
        p.drawRect(QRectF(0, 300, 640, 60)); // the table
        p.setBrush(QColor(40, 40, 46));
        p.drawRoundedRect(QRectF(300, 262, 40, 50), 6, 6); // a microphone
        return image;
    }

    Podcast podcast;
    Timeline tl;
    std::shared_ptr<ai::edit::MapTranscriptProvider> transcripts;
    std::shared_ptr<command::ServiceBundle> services;
};

void loadInto(ProjectController* controller, const Studio& studio, const QString& name) {
    const QString path = outputDir() + "/" + name;
    REQUIRE(io::saveProject(studio.tl.state, std::filesystem::path(path.toStdWString())));
    REQUIRE(controller->loadProject(QUrl::fromLocalFile(path)));
}

void openRail(Shell& shell, const char* key) {
    QVariant ignored;
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_RETURN_ARG(QVariant, ignored),
                                      Q_ARG(QVariant, QString::fromLatin1(key))));
}

} // namespace

TEST_CASE("The Shorts panel finds shorts and makes them in one undo step; screenshot", "[ai][ui][shorts][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* shorts = shell.engine.singletonInstance<ShortsBridge*>("Wabcut", "Shorts");
    REQUIRE(controller != nullptr);
    REQUIRE(shorts != nullptr);

    Studio studio;
    loadInto(controller, studio, QStringLiteral("shorts-fixture.wabcut"));
    setPreviewFramesForTests([](const MediaItem&, Ticks t) { return Studio::frame(t); });
    shorts->setServices(studio.services);
    shorts->setSynchronous(true);
    openRail(shell, "shorts");
    QQuickItem* panel = shell.item("shortsPanel");
    REQUIRE(panel != nullptr);
    REQUIRE(waitFor([&] { return panel->opacity() == 1.0; }));

    CHECK(shorts->find(3, QString(), false).isEmpty());
    REQUIRE(shorts->waitForIdle(20'000));
    INFO(shorts->message().toStdString());
    REQUIRE(shorts->candidates().size() == 3);
    const QVariantMap best = shorts->candidates().front().toMap();
    CHECK(best.value(QStringLiteral("title")).toString().contains(QStringLiteral("price")));
    CHECK(best.value(QStringLiteral("thumbs")).toStringList().size() == 6);
    CHECK(best.value(QStringLiteral("parts")).toList().size() >= 7);
    shorts->setTitle(0, QStringLiteral("Why we doubled our price"));
    CHECK(shorts->candidates().front().toMap().value(QStringLiteral("title")).toString() == QStringLiteral("Why we doubled our price"));

    QQuickItem* list = shell.item("shortsList");
    REQUIRE(list != nullptr);
    REQUIRE(waitFor([&] { return list->property("count").toInt() == 3; }));
    spin(1200); // the frames decode asynchronously

    // Make two: staged first, then one undo step.
    CHECK(shorts->create({0, 1}, {}).isEmpty());
    REQUIRE(shorts->waitForIdle(20'000));
    REQUIRE(shorts->pendingCount() == 2);
    CHECK(shorts->pendingNames().front().contains(QStringLiteral("Why we doubled our price")));
    shell.resize(1600, 1700); // a tall window: the browser slot shows whole cards
    spin(600);

    const QImage image = shell.grab();
    REQUIRE(image.save(outputDir() + QStringLiteral("/shorts-window.png")));
    const double dpr = shell.window->effectiveDevicePixelRatio();
    const QRectF r = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(outputDir() + QStringLiteral("/shorts-panel.png")));

    const std::size_t before = controller->snapshot()->sequences.size();
    CHECK(shorts->apply().isEmpty());
    CHECK(controller->snapshot()->sequences.size() == before + 2);
    CHECK(shorts->createdCount() == 2);
    const Sequence* made = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(made != nullptr);
    CHECK(made->width == 1080);
    CHECK(made->height == 1920);
    REQUIRE(controller->session().undo());
    CHECK(controller->snapshot()->sequences.size() == before);
    setPreviewFramesForTests({});
}

TEST_CASE("The Reframe dialog previews the frame path and creates the 9:16 sequence; screenshot", "[ai][ui][reframe][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    INFO(join(shell.errors));
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* reframe = shell.engine.singletonInstance<ReframeBridge*>("Wabcut", "Reframe");
    REQUIRE(controller != nullptr);
    REQUIRE(reframe != nullptr);

    Studio studio;
    loadInto(controller, studio, QStringLiteral("reframe-fixture.wabcut"));
    setPreviewFramesForTests([](const MediaItem&, Ticks t) { return Studio::frame(t); });
    reframe->setServices(studio.services);
    reframe->setSynchronous(true);

    // Sequence menu > Reframe…: the menu item loads and opens the dialog, which previews (synchronously here).
    QObject* menuItem = shell.object("reframeMenuItem");
    REQUIRE(menuItem != nullptr);
    REQUIRE(QMetaObject::invokeMethod(menuItem, "triggered"));
    auto* loader = shell.item("reframeDialogLoader");
    REQUIRE(loader != nullptr);
    auto* dialog = loader->property("item").value<QObject*>();
    REQUIRE(dialog != nullptr);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE(reframe->waitForIdle(20'000));
    INFO("Qt messages: " << join(takeMessages()));
    INFO(reframe->message().toStdString());
    const QVariantMap preview = reframe->preview();
    REQUIRE(preview.value(QStringLiteral("clips")).toList().size() == 1);
    CHECK(preview.value(QStringLiteral("width")).toInt() == 1080);
    const QVariantMap clip = preview.value(QStringLiteral("clips")).toList().front().toMap();
    CHECK(clip.value(QStringLiteral("guide")).toString() == QStringLiteral("faces"));
    const int samples = clip.value(QStringLiteral("path")).toList().size();
    REQUIRE(samples >= 10);
    dialog->setProperty("sample", samples / 5);
    spin(1500); // frames decode asynchronously; the dialog's popup transition
    const QImage image = shell.grab(); // also lays the popup out (the window is never shown)
    REQUIRE(image.save(outputDir() + QStringLiteral("/reframe-dialog.png")));
    auto* window = dialog->findChild<QQuickItem*>("reframeWindow");
    REQUIRE(window != nullptr);
    CHECK(window->isVisible());
    CHECK(window->width() > 10.0);
    CHECK(window->height() > window->width()); // a 9:16 window on a 16:9 picture

    const std::size_t before = controller->snapshot()->sequences.size();
    CHECK(reframe->apply().isEmpty());
    CHECK(controller->snapshot()->sequences.size() == before + 1);
    const Sequence* made = controller->snapshot()->findSequence(controller->currentSequenceId());
    REQUIRE(made != nullptr);
    REQUIRE(made->derivedFrom);
    CHECK(made->derivedFrom->generator == "reframe/1");
    setPreviewFramesForTests({});
}

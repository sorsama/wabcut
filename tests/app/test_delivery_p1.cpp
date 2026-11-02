// 0.6 (P1 round, stream 3C): delivery.
//   * #52 dialogue / music / effects stems with the master: ExportController.describe lists the stems a "By track role"
//         export writes and queueExport queues one audio-only job per role right after the export, named after it
//         ("<name>_DX.wav"), retried on its own; Send to Queue with several destinations writes one set;
//   * #57 caption tracks with timeline hand-offs: Export Timeline's Captions > Sidecar files writes each caption track
//         next to the timeline (the report and summary say so); the Captions page's Export all tracks writes every
//         track through captions.export, and the single-track export goes through it too;
//   * in the window: the Audio stems row of the export settings and the Captions row of Export Timeline (screenshots,
//     dark and light).

#include "A11yAudit.h"
#include "CaptionsBridge.h"
#include "ExportController.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include <wabcut/export/ExportJob.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>

#include <filesystem>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "delivery-p1" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
}

bool samePath(const QString& a, const QString& b) {
    return !a.isEmpty() && QDir::cleanPath(QDir::fromNativeSeparators(a)).compare(
                               QDir::cleanPath(QDir::fromNativeSeparators(b)), Qt::CaseInsensitive) == 0;
}

QQuickItem* findWith(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findWith(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* named(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

/// An item of a dialog (popups live in the window's overlay).
QQuickItem* itemNamed(const Shell& shell, QObject* dialog, const QString& name) {
    if (QQuickItem* found = named(shell.window->contentItem(), name))
        return found;
    const auto items = dialog->findChildren<QQuickItem*>(name);
    return items.isEmpty() ? nullptr : items.front();
}

CaptionTrack captionTrack(ProjectState& state, const char* name, const char* language, std::vector<const char*> texts) {
    CaptionTrack t;
    t.id = state.ids.allocate<CaptionTrackId>();
    t.name = name;
    t.language = language;
    Ticks at{};
    for (const char* text : texts) {
        t.cues.push_back(CaptionCue{at, Ticks{kTicksPerSecond / 10}, text, {}});
        at = at + Ticks{kTicksPerSecond / 5};
    }
    return t;
}

/// The fixture timeline with A1 a Dialogue track, A2 a Music track and A3 one without a role; and three caption tracks
/// ("English" en with 2 cues, "Thai" th, an empty "Spare").
ProjectState deliveryProject(Timeline& tl) {
    ProjectState state = tl.state;
    Sequence s = tl.sequence();
    const TrackRole roles[] = {TrackRole::Dialogue, TrackRole::Music, TrackRole::None};
    for (std::size_t i = 0; i < s.audioTracks.size() && i < 3; ++i) {
        auto t = std::make_shared<Track>(*s.audioTracks[i]);
        t->role = roles[i];
        s.audioTracks[i] = std::move(t);
    }
    s.captionTracks.push_back(captionTrack(state, "English", "en", {"Hello", "World"}));
    s.captionTracks.push_back(captionTrack(state, "Thai", "th", {"Sawasdee"}));
    s.captionTracks.push_back(captionTrack(state, "Spare", "", {}));
    return state.withSequence(std::make_shared<const Sequence>(std::move(s)));
}

constexpr const char* kPicture = "V1 |Aaaa|\nA1 |Aaaa|\nA2 |Bbbb|\nA3 |Cccc|";

} // namespace

// ---- #52: stems ------------------------------------------------------------------------------------------------------

TEST_CASE("Stems: the Export controller lists the stems and queues one per track role after the export",
          "[app][export][stems][p1]") {
    resetSettings();
    const fs::path dir = freshDir("stems");
    Timeline tl(kPicture);
    ProjectController project;
    project.resetProject(deliveryProject(tl), tl.seq);
    ExportController controller;
    controller.setProject(&project);
    controller.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    controller.setProjectFile(dir / "Film.wabcut");
    const QVariantMap stems{{QStringLiteral("stems"), QStringLiteral("role")},
                            {QStringLiteral("output"), q(dir / "Film.mp4")},
                            {QStringLiteral("hold"), true}};

    // describe: Dialogue, Music, Other - each a WAV named after the export.
    const QVariantMap d = controller.describe({}, QStringLiteral("youtube-1080p"), stems);
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("stemsOn")).toBool());
    const QVariantList list = d.value(QStringLiteral("stems")).toList();
    REQUIRE(list.size() == 3);
    CHECK(list[0].toMap().value(QStringLiteral("key")).toString() == QStringLiteral("dialogue"));
    CHECK(list[0].toMap().value(QStringLiteral("file")).toString() == QStringLiteral("Film_DX.wav"));
    CHECK(list[1].toMap().value(QStringLiteral("file")).toString() == QStringLiteral("Film_MX.wav"));
    CHECK(list[2].toMap().value(QStringLiteral("file")).toString() == QStringLiteral("Film_OTHER.wav"));
    CHECK(d.value(QStringLiteral("stemsText")).toString().startsWith(QStringLiteral("Also writes 3 stems, 24-bit WAV")));
    CHECK(d.value(QStringLiteral("stemsText")).toString().contains(QStringLiteral("Dialogue (Film_DX.wav)")));

    // queueExport: the export, then its stems, held like it.
    const QString job = controller.queueExport({}, QStringLiteral("youtube-1080p"), stems);
    REQUIRE_FALSE(job.isEmpty());
    ExportJobModel* jobs = controller.jobs();
    REQUIRE(jobs->rowCount() == 4);
    const auto row = [&](int i) { return jobs->rows()[static_cast<std::size_t>(i)]; };
    CHECK(samePath(row(0).outputPath, q(dir / "Film.mp4")));
    CHECK(samePath(row(1).outputPath, q(dir / "Film_DX.wav")));
    CHECK(samePath(row(2).outputPath, q(dir / "Film_MX.wav")));
    CHECK(samePath(row(3).outputPath, q(dir / "Film_OTHER.wav")));
    CHECK(row(1).presetName == QStringLiteral("Dialogue stem"));
    CHECK(row(3).presetName == QStringLiteral("Other stem"));
    CHECK(row(1).held);
    CHECK(row(1).overrides.value(QStringLiteral("stemOnly")).toString() == QStringLiteral("dialogue"));
    CHECK_FALSE(row(1).overrides.contains(QStringLiteral("stems")));
    CHECK(row(0).origin == row(1).origin);

    // Several destinations: one set of stems, after the first one.
    controller.cancelAll();
    REQUIRE(waitFor([&] { return jobs->activeCount() == 0; }, 30'000));
    controller.clearFinished();
    const QStringList queued = controller.sendToQueue({}, {QStringLiteral("youtube-1080p"), QStringLiteral("audio-wav")},
                                                      {{QStringLiteral("stems"), QStringLiteral("role")}}, true);
    CHECK(queued.size() == 2);
    CHECK(jobs->rowCount() == 5);
    int stemRows = 0;
    for (const ExportJobModel::Row& r : jobs->rows())
        stemRows += r.presetName.endsWith(QStringLiteral(" stem")) ? 1 : 0;
    CHECK(stemRows == 3);

    // A stem is retried on its own (a cancelled one: the same file, the same role).
    controller.cancelAll();
    REQUIRE(waitFor([&] { return jobs->activeCount() == 0; }, 30'000));
    const QString stemJob = jobs->rows()[1].jobId;
    const QString again = controller.retry(stemJob);
    REQUIRE_FALSE(again.isEmpty());
    const QVariantMap retried = controller.job(again);
    CHECK(retried.value(QStringLiteral("presetName")).toString() == QStringLiteral("Dialogue stem"));
    CHECK(retried.value(QStringLiteral("outputPath")).toString().endsWith(QStringLiteral("_DX.wav")));
    controller.cancelAll();
    REQUIRE(waitFor([&] { return jobs->activeCount() == 0; }, 30'000));

    // A sequence whose audio is all muted has nothing to split: the line says so and only the export is queued.
    Sequence muted = *project.snapshot()->findSequence(tl.seq);
    for (TrackPtr& t : muted.audioTracks) {
        auto copy = std::make_shared<Track>(*t);
        copy->muted = true;
        t = std::move(copy);
    }
    project.resetProject(project.snapshot()->withSequence(std::make_shared<const Sequence>(std::move(muted))), tl.seq);
    const QVariantMap none = controller.describe({}, QStringLiteral("youtube-1080p"), stems);
    CHECK(none.value(QStringLiteral("stems")).toList().isEmpty());
    CHECK(none.value(QStringLiteral("stemsText")).toString().startsWith(QStringLiteral("No audio track plays")));
    controller.clearFinished();
    REQUIRE_FALSE(controller.queueExport({}, QStringLiteral("youtube-1080p"), stems).isEmpty());
    CHECK(jobs->rowCount() == 1);
    controller.cancelAll();
    REQUIRE(waitFor([&] { return jobs->activeCount() == 0; }, 30'000));
    resetSettings();
}

// ---- #57: the Captions page's exports ------------------------------------------------------------------------------------

TEST_CASE("Captions: Export all tracks writes each caption track and the single track goes through captions.export",
          "[app][captions][p1]") {
    resetSettings();
    clearMessages();
    const fs::path dir = freshDir("all-tracks");
    Shell shell;
    REQUIRE(shell.window);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
    REQUIRE(controller);
    REQUIRE(captions);
    Timeline tl(kPicture);
    controller->resetProject(deliveryProject(tl), tl.seq);
    spin(100);
    const QString sequenceName = QString::fromStdString(tl.sequence().name);

    CHECK(captions->exportAllTracks(QUrl::fromLocalFile(q(dir)), QStringLiteral("srt")).isEmpty());
    CHECK(captions->property("message").toString() == QStringLiteral("Saved 2 caption file(s)"));
    const QString english = q(dir) + QLatin1Char('/') + sequenceName + QStringLiteral(" - English.en.srt");
    const QString thai = q(dir) + QLatin1Char('/') + sequenceName + QStringLiteral(" - Thai.th.srt");
    REQUIRE(QFileInfo::exists(english));
    REQUIRE(QFileInfo::exists(thai));
    CHECK_FALSE(QFileInfo::exists(q(dir) + QLatin1Char('/') + sequenceName + QStringLiteral(" - Spare.srt")));
    QFile file(english);
    REQUIRE(file.open(QIODevice::ReadOnly));
    const QByteArray text = file.readAll();
    CHECK(text.contains("Hello"));
    CHECK(text.contains("World"));
    // Another format, and the refusals.
    CHECK(captions->exportAllTracks(QUrl::fromLocalFile(q(dir)), QStringLiteral("vtt")).isEmpty());
    CHECK(QFileInfo::exists(q(dir) + QLatin1Char('/') + sequenceName + QStringLiteral(" - Thai.th.vtt")));
    CHECK_FALSE(captions->exportAllTracks(QUrl::fromLocalFile(q(dir / "missing")), QStringLiteral("srt")).isEmpty());
    CHECK_FALSE(captions->exportAllTracks(QUrl::fromLocalFile(q(dir)), QStringLiteral("doc")).isEmpty());

    // The shown track's SRT / VTT export (now captions.export's writer).
    CHECK(captions->exportFile(QUrl::fromLocalFile(q(dir / "shown.vtt")), QStringLiteral("vtt")).isEmpty());
    CHECK(captions->property("message").toString() == QStringLiteral("Saved shown.vtt"));
    QFile vtt(q(dir / "shown.vtt"));
    REQUIRE(vtt.open(QIODevice::ReadOnly));
    CHECK(vtt.readAll().startsWith("WEBVTT"));
    // Nothing changed in the project.
    CHECK(controller->session().undoCount() == 0);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

// ---- In the window ------------------------------------------------------------------------------------------------------

TEST_CASE("Delivery: the Audio stems row and Export Timeline's caption sidecars in the window; screenshots",
          "[app][export][interchange][qml][screenshot][p1]") {
    for (const bool dark : {true, false}) {
        DYNAMIC_SECTION((dark ? "dark" : "light")) {
            resetSettings();
            clearMessages();
            const fs::path dir = freshDir(dark ? "window-dark" : "window-light");
            A11yScope a11y;
            Shell shell;
            REQUIRE(shell.window);
            shell.resize(kShotWidth, kShotHeight);
            shell.theme->setProperty("reduceMotion", true);
            shell.theme->setProperty("dark", dark);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
            auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
            REQUIRE(controller);
            REQUIRE(exporter);
            REQUIRE(lifecycle);
            exporter->setLastFolder(q(dir));
            Timeline tl(kPicture);
            controller->resetProject(deliveryProject(tl), tl.seq);

            // ---- The Export workspace: Audio stems > By track role, the line naming the files, the toggle.
            shell.window->setProperty("workspace", 2);
            spin(200);
            QQuickItem* content = shell.window->contentItem();
            QQuickItem* panel = nullptr;
            REQUIRE(waitFor([&] {
                panel = findWith(content, [](QQuickItem* i) { return i->property("sessionPicks").isValid(); });
                return panel != nullptr && panel->isVisible();
            }));
            QQuickItem* drawer = named(content, QStringLiteral("exportSettingsDrawer"));
            REQUIRE(drawer);
            drawer->setProperty("open", true);
            QQuickItem* stemsTabs = named(content, QStringLiteral("exportStemsTabs"));
            QQuickItem* plan = named(content, QStringLiteral("exportStemsPlan"));
            QQuickItem* noMaster = named(content, QStringLiteral("exportStemsWithoutMaster"));
            REQUIRE(stemsTabs);
            REQUIRE(plan);
            REQUIRE(noMaster);
            CHECK(stemsTabs->isVisible());
            CHECK_FALSE(plan->isVisible());
            REQUIRE(QMetaObject::invokeMethod(stemsTabs, "activated", Q_ARG(int, 1)));
            CHECK(panel->property("overrides").toMap().value(QStringLiteral("stems")).toString() == QStringLiteral("role"));
            REQUIRE(waitFor([&] { return plan->isVisible() && noMaster->isVisible(); }));
            CHECK(stemsTabs->property("currentIndex").toInt() == 1);
            CHECK(plan->property("text").toString().contains(QStringLiteral("_DX.wav")));
            CHECK(plan->property("text").toString().contains(QStringLiteral("_OTHER.wav")));
            REQUIRE(QMetaObject::invokeMethod(noMaster, "toggled"));
            {
                // Screenshot: the Flickable holding the drawer scrolled so the Audio stems row is near the top.
                spin(400); // the drawer opens (its height animates)
                QQuickItem* scroll = stemsTabs->parentItem();
                while (scroll && !scroll->property("contentY").isValid())
                    scroll = scroll->parentItem();
                REQUIRE(scroll);
                auto* flickContent = scroll->property("contentItem").value<QQuickItem*>();
                REQUIRE(flickContent);
                const QPointF at = stemsTabs->mapToItem(flickContent, QPointF(0, 0));
                scroll->setProperty("contentY", std::max(0.0, at.y() - 60.0));
                spin(100);
                const QImage shot = shell.grab();
                REQUIRE_FALSE(shot.isNull());
                const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
                const double dpr = shot.devicePixelRatio();
                REQUIRE(shot.copy(QRectF(rect.topLeft() * dpr, rect.size() * dpr).toRect())
                            .save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-stems.png")
                                                                  : QStringLiteral("export-stems-light.png"))));
            }
            {
                const QStringList issues = auditAccessibility(drawer);
                INFO(issues.join(QStringLiteral("; ")).toStdString());
                CHECK(issues.isEmpty());
            }
            REQUIRE(QMetaObject::invokeMethod(stemsTabs, "activated", Q_ARG(int, 0)));
            CHECK_FALSE(panel->property("overrides").toMap().contains(QStringLiteral("stems")));

            // ---- Export Timeline: Captions > Sidecar files writes each caption track next to the timeline.
            shell.window->setProperty("workspace", 0);
            spin(100);
            QObject* dialog = shell.object("exportTimelineDialog");
            REQUIRE(dialog);
            REQUIRE(QMetaObject::invokeMethod(dialog, "openForCurrent"));
            REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }));
            dialog->setProperty("file", QUrl::fromLocalFile(q(dir / "cut.xml")));
            QQuickItem* captionTabs = itemNamed(shell, dialog, QStringLiteral("exportTimelineCaptions"));
            QQuickItem* hint = itemNamed(shell, dialog, QStringLiteral("exportTimelineCaptionsHint"));
            QQuickItem* formatButton = itemNamed(shell, dialog, QStringLiteral("exportTimelineCaptionFormat"));
            REQUIRE(captionTabs);
            REQUIRE(hint);
            REQUIRE(formatButton);
            CHECK(captionTabs->property("currentIndex").toInt() == 0);
            CHECK_FALSE(hint->isVisible());
            REQUIRE(QMetaObject::invokeMethod(captionTabs, "activated", Q_ARG(int, 1)));
            REQUIRE(waitFor([&] { return hint->isVisible() && formatButton->isVisible(); }));
            CHECK(dialog->property("captionSidecars").toBool());
            CHECK(hint->property("text").toString().contains(QStringLiteral("cut - <track name>.<language>.srt")));
            REQUIRE(QMetaObject::invokeMethod(dialog, "setCaptionFormat", Q_ARG(QString, QStringLiteral("vtt"))));
            CHECK(formatButton->property("text").toString() == QStringLiteral(".vtt"));
            spin(300);
            {
                const QImage image = shell.grab();
                REQUIRE_FALSE(image.isNull());
                CHECK(image.save(QDir(outputDir()).filePath(dark ? QStringLiteral("export-timeline-captions.png")
                                                                 : QStringLiteral("export-timeline-captions-light.png"))));
            }
            QVariantMap report;
            bool done = false;
            const QMetaObject::Connection connection =
                QObject::connect(lifecycle, &ProjectLifecycle::timelineExported, lifecycle, [&](const QVariantMap& r) {
                    report = r;
                    done = true;
                });
            QQuickItem* ok = itemNamed(shell, dialog, QStringLiteral("exportTimelineOk"));
            REQUIRE(ok);
            REQUIRE(QMetaObject::invokeMethod(ok, "clicked"));
            QElapsedTimer timer;
            timer.start();
            while (!done && timer.elapsed() < 20000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
                QThread::msleep(5);
            }
            QObject::disconnect(connection);
            REQUIRE(done);
            INFO(report.value(QStringLiteral("error")).toString().toStdString());
            REQUIRE(report.value(QStringLiteral("ok")).toBool());
            const QStringList files = report.value(QStringLiteral("captionFiles")).toStringList();
            REQUIRE(files.size() == 2);
            CHECK(QFileInfo(files[0]).fileName() == QStringLiteral("cut - English.en.vtt"));
            CHECK(QFileInfo(files[1]).fileName() == QStringLiteral("cut - Thai.th.vtt"));
            CHECK(QFileInfo::exists(files[0]));
            CHECK(report.value(QStringLiteral("summary")).toString().endsWith(QStringLiteral(" · 2 caption files next to it")));
            bool carried = false;
            for (const QVariant& e : report.value(QStringLiteral("entries")).toList()) {
                const QVariantMap entry = e.toMap();
                carried = carried || (entry.value(QStringLiteral("category")).toString() == QStringLiteral("caption") &&
                                      entry.value(QStringLiteral("outcome")).toString() == QStringLiteral("carried"));
            }
            CHECK(carried);
            // The choice is remembered for the next Export Timeline.
            spin(300);
            REQUIRE(QMetaObject::invokeMethod(dialog, "openForCurrent"));
            CHECK(captionTabs->property("currentIndex").toInt() == 1);
            CHECK(dialog->property("captionFormat").toString() == QStringLiteral("vtt"));
            QMetaObject::invokeMethod(dialog, "close");
            if (QObject* reportDialog = shell.object("importReportDialog"))
                QMetaObject::invokeMethod(reportDialog, "close");
            spin(200);
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
    resetSettings();
}

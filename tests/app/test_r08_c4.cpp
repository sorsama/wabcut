// Stream C, batch C4 (0.7), app level:
//   * #23  the Library list's optional thumbnails and the size slider that sizes them;
//   * #4   the export history: finished exports kept, listed in the queue dialog's History tab, Show / Open / Export Again;
//   * #176 the pre-export check dialog (the rows, their fixes, Export anyway) and the Export button's own check;
//   * #103 what the newer codecs do here: the About dialog's list and the Properties line of a clip.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "ExportController.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"

#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJSValue>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QWheelEvent>

#include <filesystem>
#include <functional>
#include <memory>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

namespace ex = wabcut::exporter;
namespace fs = std::filesystem;

fs::path freshDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "app" / "v07-c4" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

QString q(const fs::path& p) {
    return QString::fromStdU16String(p.u16string());
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

QQuickItem* byName(QQuickItem* root, const QString& name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

/// Every item under `root` whose object name starts with `prefix`.
QList<QQuickItem*> allWithPrefix(QQuickItem* root, const QString& prefix) {
    QList<QQuickItem*> out;
    std::function<void(QQuickItem*)> walk = [&](QQuickItem* item) {
        if (!item)
            return;
        if (item->objectName().startsWith(prefix))
            out.append(item);
        for (QQuickItem* child : item->childItems())
            walk(child);
    };
    walk(root);
    return out;
}

QVariant plain(const QVariant& value) {
    return value.userType() == qMetaTypeId<QJSValue>() ? value.value<QJSValue>().toVariant() : value;
}

void saveItemShot(Shell& shell, const QQuickItem* item, const QString& name) {
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()))
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

/// A sequence of `seconds` of nothing (exports render silence and the synthetic picture).
ProjectState emptyProject(double seconds) {
    ProjectState state;
    Sequence seq;
    seq.id = state.ids.allocate<SequenceId>();
    seq.name = "Clip";
    seq.width = 640;
    seq.height = 360;
    seq.frameRate = kRate30;
    seq.duration = Ticks::fromSeconds(static_cast<std::int64_t>(seconds));
    state.sequences.push_back(std::make_shared<const Sequence>(std::move(seq)));
    return state;
}

bool finished(const ExportController& c, const QString& jobId, int timeoutMs = 60'000) {
    return waitFor([&] { return c.job(jobId).value(QStringLiteral("finished")).toBool(); }, timeoutMs);
}

/// `count` pictures on disk as media items of a project (so the Library has thumbnails to show).
ProjectState pictureProject(const fs::path& dir, int count) {
    ProjectState state = emptyProject(1.0); // a sequence and the pictures: every row fits in view at the tall row height
    const QColor colors[] = {QColor(200, 60, 60), QColor(60, 160, 90), QColor(70, 90, 200), QColor(220, 180, 60)};
    for (int i = 0; i < count; ++i) {
        QImage picture(320, 180, QImage::Format_RGB32);
        picture.fill(colors[i % 4]);
        for (int y = 40; y < 140; ++y) {
            for (int x = 40 + i * 20; x < 120 + i * 20; ++x)
                picture.setPixel(x, y, qRgb(255, 255, 255));
        }
        const fs::path file = dir / ("Picture " + std::to_string(i + 1) + ".png");
        REQUIRE(picture.save(q(file)));
        const std::string path = file.u8string().empty() ? std::string() : std::string(reinterpret_cast<const char*>(file.u8string().c_str()));
        auto info = media::probe(path);
        REQUIRE(info.has_value());
        MediaItem item;
        item.id = state.ids.allocate<MediaId>();
        item.name = "Picture " + std::to_string(i + 1);
        item.locator.path = path;
        item.info = *info;
        state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    }
    return state;
}

} // namespace

// ---- #23 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Library list view: thumbnails in the Name column, rows that follow the size slider; screenshots (023)",
          "[app][library][qml][screenshot][v07]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(controller);
    REQUIRE(library);
    const fs::path dir = freshDir("list-thumbnails");
    ProjectState state = pictureProject(dir, 3);
    controller->resetProject(std::move(state));
    library->setPersistent(false);
    library->setView(QStringLiteral("list"));
    spin(100);
    REQUIRE_FALSE(shell.grab().isNull()); // a hidden window lays out when it renders
    spin(100);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* list = byName(content, QStringLiteral("libraryList"));
    REQUIRE(list);
    QQuickItem* table = byName(content, QStringLiteral("libraryTable"));
    REQUIRE(table);
    QQuickItem* zoom = byName(content, QStringLiteral("libraryZoomRow"));
    REQUIRE(zoom);

    // Off (the default): text rows of the theme's height, no pictures, no size slider under the list.
    CHECK_FALSE(library->listThumbnails());
    const int textRow = list->property("rowHeight").toInt();
    CHECK(textRow > 0);
    CHECK(allWithPrefix(list, QStringLiteral("libraryListThumb_")).isEmpty());
    CHECK_FALSE(zoom->isVisible());

    // On: rows twice the height at 100 %, a picture in each Name cell of a media item, the slider back.
    library->setListThumbnails(true);
    REQUIRE(waitFor([&] { return list->property("rowHeight").toInt() == textRow * 2; }, 3000));
    spin(200);
    (void)shell.grab();
    CHECK(zoom->isVisible());
    // The three picture files are media items of the Library (the fixture's own media are offline: no picture to show).
    LibraryModel* model = library->items();
    QStringList pictureKeys;
    for (int row = 0; row < model->rowCount(); ++row) {
        if (model->cellText(row, LibraryColumn::Name).startsWith(QStringLiteral("Picture ")))
            pictureKeys << model->keyOf(row);
    }
    REQUIRE(pictureKeys.size() == 3);
    const auto pictureItems = [&] {
        QList<QQuickItem*> out;
        for (const QString& key : pictureKeys) {
            if (QQuickItem* item = byName(list, QStringLiteral("libraryListThumb_") + key))
                out.append(item);
        }
        return out;
    };
    QList<QQuickItem*> pictures;
    REQUIRE(waitFor([&] {
        (void)shell.grab();
        pictures = pictureItems();
        return pictures.size() == 3;
    }, 5000));
    // The table lays the rows out at their new height on its next polish (a hidden window polishes when it renders).
    const auto cellHeight = [&] {
        const QList<QQuickItem*> now = pictureItems();
        return now.isEmpty() ? -1.0 : now.front()->parentItem()->height();
    };
    bool laidOut = waitFor([&] {
        (void)shell.grab();
        return cellHeight() == textRow * 2;
    }, 5000);
    UNSCOPED_INFO("cell height " << cellHeight() << " row height " << list->property("rowHeight").toInt() << " text row "
                                 << textRow << " table content height " << table->property("contentHeight").toDouble());
    if (!laidOut) {
        // Diagnostics: what the provider says, and whether an explicit forceLayout now moves the rows.
        const QJSValue provider = table->property("rowHeightProvider").value<QJSValue>();
        UNSCOPED_INFO("provider row 0 -> " << (provider.isCallable() ? provider.call({QJSValue(0)}).toInt() : -1));
        QMetaObject::invokeMethod(table, "forceLayout");
        spin(100);
        (void)shell.grab();
        spin(100);
        (void)shell.grab();
        UNSCOPED_INFO("after forceLayout: cell height " << cellHeight() << " content height "
                                                        << table->property("contentHeight").toDouble());
    }
    REQUIRE(laidOut);
    pictures = pictureItems();
    for (QQuickItem* picture : pictures) {
        INFO(picture->objectName().toStdString());
        CHECK(picture->isVisible());
        CHECK(picture->height() == picture->parentItem()->height() - 4); // Theme.space(1) of padding
        CHECK(picture->width() > picture->height()); // 16:9
        CHECK(picture->parentItem()->height() == textRow * 2);
    }
    // The picture itself arrives (the file is a real image).
    const auto imageOf = [](QQuickItem* picture) {
        return findWith(picture, [](QQuickItem* i) { return i->metaObject()->className() == QByteArray("QQuickImage"); });
    };
    const bool pictured = waitFor([&] {
        (void)shell.grab();
        for (QQuickItem* picture : pictureItems()) {
            const QQuickItem* image = imageOf(picture);
            if (!image || !image->isVisible())
                return false;
        }
        return true;
    }, 10000);
    for (QQuickItem* picture : pictureItems()) {
        const QQuickItem* image = imageOf(picture);
        UNSCOPED_INFO(picture->objectName().toStdString() << ": image " << (image ? "found" : "missing") << ", status "
                                                           << (image ? image->property("status").toInt() : -1) << ", source "
                                                           << (image ? image->property("source").toString().toStdString() : std::string()));
    }
    REQUIRE(pictured);

    // The size slider sizes the rows (and the keys, the ··· menu and Ctrl+wheel are the same property).
    library->setThumbnailScale(1.5);
    REQUIRE(waitFor([&] { return list->property("rowHeight").toInt() == static_cast<int>(std::lround(textRow * 2 * 1.5)); }, 3000));
    library->setThumbnailScale(0.5);
    REQUIRE(waitFor([&] { return list->property("rowHeight").toInt() == static_cast<int>(std::lround(textRow * 2 * 0.5)); }, 3000));
    library->setThumbnailScale(1.0);
    REQUIRE(waitFor([&] { return list->property("rowHeight").toInt() == textRow * 2; }, 3000));
    // Page Up / Down move by the rows in view at this row height.
    CHECK(list->property("pageRows").toInt() == std::max(1, static_cast<int>(table->height() / (textRow * 2))));

    // Ctrl+wheel over the list steps the scale; a plain wheel scrolls and leaves it.
    const QPointF where = table->mapToScene(QPointF(table->width() / 2, table->height() / 2));
    {
        QWheelEvent wheel(where, where, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(shell.window.get(), &wheel);
    }
    spin(100);
    CHECK(library->thumbnailScale() == 1.25);
    {
        QWheelEvent wheel(where, where, QPoint(), QPoint(0, -120), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(shell.window.get(), &wheel);
        QCoreApplication::sendEvent(shell.window.get(), &wheel);
    }
    spin(100);
    CHECK(library->thumbnailScale() == 0.75);
    {
        QWheelEvent wheel(where, where, QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(shell.window.get(), &wheel);
    }
    spin(100);
    CHECK(library->thumbnailScale() == 0.75);
    library->setThumbnailScale(1.0);
    spin(150);

    // The ··· menu has the switch, and the choice is saved with the Library's other settings.
    library->setPersistent(true);
    library->setListThumbnails(false);
    library->setListThumbnails(true);
    CHECK(QSettings().value(QStringLiteral("library/listThumbnails")).toBool());
    library->setPersistent(false);

    // Screenshots: dark and light, at 100 %.
    REQUIRE(waitFor([&] { return list->property("rowHeight").toInt() == textRow * 2; }, 3000));
    spin(300);
    (void)shell.grab();
    saveItemShot(shell, byName(content, QStringLiteral("libraryPanel")), QStringLiteral("library-list-thumbnails-dark.png"));
    shell.theme->setProperty("dark", false);
    spin(700);
    (void)shell.grab();
    saveItemShot(shell, byName(content, QStringLiteral("libraryPanel")), QStringLiteral("library-list-thumbnails-light.png"));
    shell.theme->setProperty("dark", true);

    // Off again: back to text rows, the slider under the list goes.
    library->setListThumbnails(false);
    REQUIRE(waitFor([&] { return list->property("rowHeight").toInt() == textRow; }, 3000));
    spin(100);
    (void)shell.grab();
    CHECK_FALSE(zoom->isVisible());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Library: only the Name column of the model asks for thumbnails (023)", "[app][library][v07]") {
    resetSettings();
    Timeline tl("V1 |Aa|");
    ProjectController controller;
    LibraryBridge library;
    controller.resetProject(tl.state, tl.seq);
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setAnalysisProbe({});
    library.setController(&controller);
    LibraryModel* items = library.items();
    REQUIRE(items->rowCount() > 0);
    REQUIRE(items->columnCount() > 1);
    for (int row = 0; row < items->rowCount(); ++row) {
        for (int column = 1; column < items->columnCount(); ++column)
            CHECK(items->data(items->index(row, column), LibraryModel::ThumbnailRole).toString().isEmpty());
    }
}

// ---- #4 -------------------------------------------------------------------------------------------------------------

TEST_CASE("Export history keeps finished exports and Export Again repeats them (004)", "[app][export][history][v07]") {
    resetSettings();
    const fs::path dir = freshDir("history");
    ProjectController project;
    project.resetProject(emptyProject(1.0));
    ExportController c;
    c.setProject(&project);
    c.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    c.setProjectFile(dir / "Show.wabcut");
    QStringList revealed;
    QStringList opened;
    c.setRevealHandler([&](const QString& path) {
        revealed << path;
        return true;
    });
    c.setOpenHandler([&](const QString& path) {
        opened << path;
        return true;
    });
    REQUIRE(c.history().isEmpty());

    const QString job = c.queueExport({}, QStringLiteral("audio-wav"), {{QStringLiteral("output"), q(dir / "one.wav")}});
    REQUIRE_FALSE(job.isEmpty());
    REQUIRE(finished(c, job));
    REQUIRE(c.job(job).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
    QVariantList history = c.history();
    REQUIRE(history.size() == 1);
    QVariantMap first = history.front().toMap();
    CHECK(first.value(QStringLiteral("title")).toString() == QStringLiteral("one.wav"));
    CHECK(QDir::fromNativeSeparators(first.value(QStringLiteral("outputPath")).toString()) == QDir::fromNativeSeparators(q(dir / "one.wav")));
    CHECK(first.value(QStringLiteral("exists")).toBool());
    CHECK(first.value(QStringLiteral("canRepeat")).toBool());
    CHECK(first.value(QStringLiteral("sequenceName")).toString() == QStringLiteral("Clip"));
    CHECK(first.value(QStringLiteral("projectName")).toString() == QStringLiteral("Show"));
    CHECK_FALSE(first.value(QStringLiteral("whenText")).toString().isEmpty());
    CHECK_FALSE(first.value(QStringLiteral("presetName")).toString().isEmpty());

    // Show and Open act on the file.
    CHECK(c.revealHistory(0));
    CHECK(c.openHistory(0));
    REQUIRE(revealed.size() == 1);
    REQUIRE(opened.size() == 1);
    CHECK(QFileInfo(revealed.front()).fileName() == QStringLiteral("one.wav"));
    CHECK_FALSE(c.openHistory(5)); // no such entry

    // Export Again: the file is there, so the new one is numbered; the same preset and settings.
    const QString again = c.exportAgain(0);
    REQUIRE_FALSE(again.isEmpty());
    REQUIRE(finished(c, again));
    REQUIRE(c.job(again).value(QStringLiteral("state")).toString() == QStringLiteral("done"));
    CHECK(QFileInfo::exists(q(dir / "one (2).wav")));
    history = c.history();
    REQUIRE(history.size() == 2);
    CHECK(history.front().toMap().value(QStringLiteral("title")).toString() == QStringLiteral("one (2).wav")); // newest first

    // A file that is gone: greyed (exists false), Show falls back to its folder, Open refuses, Export Again names it again.
    QFile::remove(q(dir / "one.wav"));
    c.refreshHistory();
    history = c.history();
    bool found = false;
    for (const QVariant& v : history) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("title")).toString() == QStringLiteral("one.wav")) {
            found = true;
            CHECK_FALSE(m.value(QStringLiteral("exists")).toBool());
        }
    }
    CHECK(found);
    const int goneIndex = [&] {
        for (const QVariant& v : history) {
            if (v.toMap().value(QStringLiteral("title")).toString() == QStringLiteral("one.wav"))
                return v.toMap().value(QStringLiteral("index")).toInt();
        }
        return -1;
    }();
    REQUIRE(goneIndex >= 0);
    CHECK_FALSE(c.openHistory(goneIndex));
    CHECK(c.revealHistory(goneIndex));
    CHECK(QFileInfo(revealed.back()).isDir());
    const QString third = c.exportAgain(goneIndex);
    REQUIRE_FALSE(third.isEmpty());
    REQUIRE(finished(c, third));
    CHECK(QFileInfo::exists(q(dir / "one.wav"))); // the old name is free again

    // The list is kept between sessions.
    {
        ExportController next;
        CHECK(next.history().size() == c.history().size());
        CHECK(next.history().size() == 3);
    }

    // Another project, or a sequence that is gone, says so and queues nothing.
    ExportController other;
    other.setProject(&project);
    other.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    other.setProjectFile(dir / "Other.wabcut");
    CHECK(other.exportAgain(0).isEmpty());
    CHECK(other.lastError().contains(QStringLiteral("Show")));
    project.resetProject(ProjectState{});
    c.setProject(&project);
    CHECK(c.exportAgain(0).isEmpty());
    CHECK(c.lastError().contains(QStringLiteral("not in this project")));
    CHECK(c.jobs()->activeCount() == 0);

    // Removing one entry and clearing the list.
    const int before = c.history().size();
    c.removeHistory(0);
    CHECK(c.history().size() == before - 1);
    c.clearHistory();
    CHECK(c.history().isEmpty());
    CHECK(ExportController().history().isEmpty());
}

TEST_CASE("Export history is capped at 100 entries and a failed export is not kept (004)", "[app][export][history][v07]") {
    resetSettings();
    const fs::path dir = freshDir("history-cap");
    // 105 old entries are in the settings; one more export pushes the oldest out.
    QJsonArray old;
    for (int i = 0; i < 105; ++i) {
        QJsonObject e;
        e[QStringLiteral("when")] = QStringLiteral("2026-01-01T00:00:00Z");
        e[QStringLiteral("title")] = QStringLiteral("old %1.mp4").arg(i);
        e[QStringLiteral("outputPath")] = q(dir / ("old " + std::to_string(i) + ".mp4"));
        old.append(e);
    }
    QSettings().setValue(QStringLiteral("export/history"), QString::fromUtf8(QJsonDocument(old).toJson(QJsonDocument::Compact)));
    ProjectController project;
    project.resetProject(emptyProject(1.0));
    ExportController c;
    c.setProject(&project);
    c.setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    REQUIRE(c.history().size() == 105);
    CHECK_FALSE(c.history().front().toMap().value(QStringLiteral("canRepeat")).toBool()); // no request kept with it
    const QString job = c.queueExport({}, QStringLiteral("audio-wav"), {{QStringLiteral("output"), q(dir / "new.wav")}});
    REQUIRE(finished(c, job));
    const QVariantList history = c.history();
    CHECK(history.size() == 100);
    CHECK(history.front().toMap().value(QStringLiteral("title")).toString() == QStringLiteral("new.wav"));
    CHECK(history.back().toMap().value(QStringLiteral("title")).toString() == QStringLiteral("old 98.mp4"));

    // An export that fails (the output folder does not exist) leaves the history as it was.
    const QString failed = c.queueExport({}, QStringLiteral("audio-wav"), {{QStringLiteral("output"), q(dir / "no" / "such" / "x.wav")}});
    if (!failed.isEmpty()) {
        REQUIRE(finished(c, failed));
        CHECK(c.job(failed).value(QStringLiteral("state")).toString() != QStringLiteral("done"));
    }
    CHECK(c.history().size() == 100);
    CHECK(c.history().front().toMap().value(QStringLiteral("title")).toString() == QStringLiteral("new.wav"));
    c.clearHistory();
}

TEST_CASE("Export Queue dialog: the History tab lists exports with Show, Open and Export Again; screenshots (004)",
          "[app][export][history][qml][screenshot][v07]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller);
    REQUIRE(exporter);
    const fs::path dir = freshDir("history-dialog");
    controller->resetProject(emptyProject(1.0));
    exporter->setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    QStringList opened;
    exporter->setOpenHandler([&](const QString& path) {
        opened << path;
        return true;
    });
    // Three finished exports; the first one's file is deleted afterwards.
    for (const char* name : {"Intro.wav", "Highlights.wav", "Credits.wav"}) {
        const QString job = exporter->queueExport({}, QStringLiteral("audio-wav"), {{QStringLiteral("output"), q(dir / name)}});
        REQUIRE(finished(*exporter, job));
    }
    QFile::remove(q(dir / "Intro.wav"));
    exporter->refreshHistory();
    REQUIRE(exporter->history().size() == 3);

    // The dialog lives in the Export panel (workspace 2).
    shell.window->setProperty("workspace", 2);
    spin(300);
    QObject* queueDialog = shell.object("exportQueueDialog");
    REQUIRE(queueDialog);
    REQUIRE(QMetaObject::invokeMethod(queueDialog, "openOn", Q_ARG(QVariant, QStringLiteral("history"))));
    REQUIRE(waitFor([&] { return queueDialog->property("visible").toBool(); }, 5000));
    spin(400);
    (void)shell.grab();
    QQuickItem* popup = qobject_cast<QQuickItem*>(queueDialog->property("contentItem").value<QObject*>());
    REQUIRE(popup);
    QQuickItem* page = byName(popup, QStringLiteral("exportHistoryPage"));
    REQUIRE(page);
    CHECK(page->isVisible());
    QQuickItem* list = byName(popup, QStringLiteral("exportHistoryList"));
    REQUIRE(list);
    CHECK(list->property("count").toInt() == 3);
    // Newest first: Credits, Highlights, Intro (the file of Intro is gone: greyed).
    QQuickItem* title0 = byName(popup, QStringLiteral("exportHistoryTitle_0"));
    QQuickItem* title2 = byName(popup, QStringLiteral("exportHistoryTitle_2"));
    REQUIRE(title0);
    REQUIRE(title2);
    CHECK(title0->property("text").toString() == QStringLiteral("Credits.wav"));
    CHECK(title2->property("text").toString().startsWith(QStringLiteral("Intro.wav")));
    CHECK(title2->property("text").toString().contains(QStringLiteral("not found")));
    QQuickItem* open0 = byName(popup, QStringLiteral("exportHistoryOpen_0"));
    QQuickItem* open2 = byName(popup, QStringLiteral("exportHistoryOpen_2"));
    REQUIRE(open0);
    REQUIRE(open2);
    CHECK(open0->property("enabled").toBool());
    CHECK_FALSE(open2->property("enabled").toBool());
    CHECK(byName(popup, QStringLiteral("exportHistoryEmpty"))->isVisible() == false);

    // Nothing runs off the dialog at the smallest window, and the controls are named for assistive technology.
    shell.resize(1100, 700);
    spin(400);
    (void)shell.grab();
    for (const QString& name : {QStringLiteral("exportHistoryShow_0"), QStringLiteral("exportHistoryOpen_0"),
                                QStringLiteral("exportHistoryAgain_0"), QStringLiteral("exportHistoryRemove_0")}) {
        QQuickItem* button = byName(popup, name);
        REQUIRE(button);
        const QRectF at = button->mapRectToScene(QRectF(0, 0, button->width(), button->height()));
        INFO(name.toStdString() << " " << at.left() << ".." << at.right());
        CHECK(at.left() >= 0);
        CHECK(at.right() <= shell.window->width());
    }
    CHECK(auditAccessibility(popup).isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    spin(400);
    (void)shell.grab();
    saveItemShot(shell, popup->parentItem() ? popup->parentItem() : popup, QStringLiteral("export-history-dark.png"));
    shell.theme->setProperty("dark", false);
    spin(700);
    (void)shell.grab();
    saveItemShot(shell, popup->parentItem() ? popup->parentItem() : popup, QStringLiteral("export-history-light.png"));
    shell.theme->setProperty("dark", true);

    // Export Again from the dialog queues the job and goes back to the queue tab.
    QQuickItem* again1 = byName(popup, QStringLiteral("exportHistoryAgain_1"));
    REQUIRE(again1);
    const int jobsBefore = exporter->jobs()->rowCount();
    REQUIRE(QMetaObject::invokeMethod(again1, "clicked"));
    REQUIRE(waitFor([&] { return exporter->jobs()->rowCount() > jobsBefore; }, 5000));
    CHECK_FALSE(page->isVisible()); // Export Again went back to the queue tab
    // The repeated export finishes and is listed too (four entries now); only then is the list cleared.
    REQUIRE(waitFor([&] { return exporter->jobs()->activeCount() == 0; }, 30000));
    REQUIRE(waitFor([&] { return exporter->history().size() == 4; }, 5000));
    REQUIRE(QMetaObject::invokeMethod(queueDialog, "openOn", Q_ARG(QVariant, QStringLiteral("history"))));
    exporter->clearHistory();
    spin(300);
    CHECK(page->isVisible());
    CHECK(byName(popup, QStringLiteral("exportHistoryEmpty"))->isVisible());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- #49 ------------------------------------------------------------------------------------------------------------

TEST_CASE("Export details say how much of a smart-rendered export was copied and why the rest was not (049)",
          "[app][export][v07]") {
    resetSettings();
    ExportJobModel jobs;
    ExportJobModel::Row row;
    row.jobId = QStringLiteral("j1");
    row.queueId = 1;
    row.title = QStringLiteral("cut.mp4");
    jobs.append(row);
    ex::ExportJobStatus status;
    status.id = 1;
    status.progress.phase = ex::ExportPhase::Done;
    ex::ExportResult result;
    result.frames = 200;
    result.copiedFrames = 164; // 82 %
    result.duration = Ticks::fromSeconds(7);
    result.realtimeFactor = 12.0;
    result.smartRenderCodec = "H.264";
    result.smartRenderNotes = {ex::SmartRenderNote{"Effects on the clip", 24, 60, "00:00:02:00"},
                               ex::SmartRenderNote{"The clip starts inside a GOP", 12, 0, "00:00:00:00"}};
    status.result = result;
    const ExportJobModel::Row* done = jobs.update(1, status);
    REQUIRE(done);
    CHECK(done->detail.contains(QStringLiteral("H.264 passthrough: 82 % copied, 18 % re-encoded")));
    // One line per reason, with where it starts.
    const QStringList lines = done->smartNotes.split(QLatin1Char('\n'));
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].contains(QStringLiteral("24 frame")));
    CHECK(lines[0].contains(QStringLiteral("00:00:02:00")));
    CHECK(lines[0].contains(QStringLiteral("Effects on the clip")));
    CHECK(lines[1].contains(QStringLiteral("The clip starts inside a GOP")));
    // Nothing copied is said too (not "0 of ..."): a long-GOP export that could not copy anything.
    status.result->copiedFrames = 0;
    status.result->smartRenderNotes = {ex::SmartRenderNote{"The source was encoded with other settings", 200, 0, "00:00:00:00"}};
    const ExportJobModel::Row* none = jobs.update(1, status);
    REQUIRE(none);
    CHECK(none->detail.contains(QStringLiteral("0 % copied, 100 % re-encoded")));
    // An intra-only smart render (ProRes, DNx) keeps its old wording.
    status.result->smartRenderCodec.clear();
    status.result->smartRenderNotes.clear();
    status.result->copiedFrames = 12;
    status.result->frames = 24;
    const ExportJobModel::Row* intra = jobs.update(1, status);
    REQUIRE(intra);
    CHECK(intra->detail.contains(QStringLiteral("12 of 24 frames copied")));
    CHECK(intra->smartNotes.isEmpty());
}

// ---- #176 -----------------------------------------------------------------------------------------------------------

TEST_CASE("Pre-export check: the controller turns the rows into sentences and a measurement into loudness rows (176)",
          "[app][export][check][v07]") {
    resetSettings();
    Timeline tl("V1 |Aa..Bb|");
    ProjectController project;
    project.resetProject(tl.state, tl.seq);
    ExportController c;
    c.setProject(&project);
    // The standard fixture's media are offline (no files): the check says so, first, with a Relink fix.
    QVariantList rows = c.preExportCheck({}, QStringLiteral("youtube-1080p"), {});
    REQUIRE_FALSE(rows.isEmpty());
    const QVariantMap first = rows.front().toMap();
    CHECK(first.value(QStringLiteral("severity")).toString() == QStringLiteral("problem"));
    CHECK(first.value(QStringLiteral("kind")).toString() == QStringLiteral("offlineMedia"));
    CHECK(first.value(QStringLiteral("fix")).toString() == QStringLiteral("relink"));
    CHECK(first.value(QStringLiteral("fixText")).toString() == QStringLiteral("Relink…"));
    CHECK(first.value(QStringLiteral("text")).toString().contains(QStringLiteral("offline or missing")));
    CHECK(first.value(QStringLiteral("hasPlace")).toBool());
    CHECK_FALSE(first.value(QStringLiteral("timecode")).toString().isEmpty());
    // Go to puts the playhead on the row's place.
    project.setPlayhead(Ticks::fromSeconds(0));
    QVariantMap place = first;
    place[QStringLiteral("ticks")] = QVariant::fromValue<qlonglong>(Ticks::fromSeconds(2).value);
    CHECK(c.goToCheckRow(place));
    CHECK(project.playhead() == Ticks::fromSeconds(2));
    CHECK_FALSE(c.goToCheckRow(QVariantMap{{QStringLiteral("hasPlace"), false}}));
    // A gap between two clips has its sentence, place and fix.
    bool gap = false;
    for (const QVariant& v : rows) {
        const QVariantMap m = v.toMap();
        if (m.value(QStringLiteral("kind")).toString() == QStringLiteral("gap")) {
            gap = true;
            CHECK(m.value(QStringLiteral("text")).toString().contains(QStringLiteral("No picture for")));
            CHECK(m.value(QStringLiteral("fix")).toString() == QStringLiteral("closeGap"));
        }
    }
    CHECK(gap);

    // The settings that do not resolve are one problem row.
    rows = c.preExportCheck({}, QStringLiteral("youtube-1080p"), {{QStringLiteral("range"), QStringLiteral("workArea")}});
    REQUIRE(rows.size() == 1);
    CHECK(rows.front().toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("invalidSettings"));

    // Loudness: measured off the GUI thread, then in the rows for this project and no other.
    c.setLoudnessMeasurer([](const std::stop_token&) { return std::pair<double, double>(-22.0, 0.4); });
    c.measureLoudness({}, QStringLiteral("youtube-1080p"), {});
    CHECK(c.measuringLoudness());
    REQUIRE(waitFor([&] { return !c.measuringLoudness(); }, 10000));
    CHECK(c.measuredLoudness().value(QStringLiteral("ok")).toBool());
    CHECK(c.measuredLoudness().value(QStringLiteral("integratedLufs")).toDouble() == -22.0);
    rows = c.preExportCheck({}, QStringLiteral("youtube-1080p"), {}, {{QStringLiteral("name"), QStringLiteral("YouTube")}, {QStringLiteral("lufs"), -14.0}});
    bool clips = false;
    bool offTarget = false;
    for (const QVariant& v : rows) {
        const QString kind = v.toMap().value(QStringLiteral("kind")).toString();
        clips = clips || kind == QStringLiteral("audioClips");
        offTarget = offTarget || kind == QStringLiteral("loudnessOffTarget");
    }
    CHECK(clips);
    CHECK(offTarget);
    // An edit makes a new snapshot: the measurement no longer applies.
    project.setPlayhead(Ticks{});
    Timeline other("V1 |A|");
    project.resetProject(other.state, other.seq);
    rows = c.preExportCheck({}, QStringLiteral("youtube-1080p"), {}, {{QStringLiteral("name"), QStringLiteral("YouTube")}, {QStringLiteral("lufs"), -14.0}});
    for (const QVariant& v : rows)
        CHECK(v.toMap().value(QStringLiteral("kind")).toString() != QStringLiteral("loudnessOffTarget"));
    // Cancelling a measurement ends it as cancelled.
    c.setLoudnessMeasurer([](const std::stop_token& stop) {
        for (int i = 0; i < 500 && !stop.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return std::pair<double, double>(-10.0, -1.0);
    });
    c.measureLoudness({}, QStringLiteral("youtube-1080p"), {});
    CHECK(c.measuringLoudness());
    c.cancelLoudnessMeasure();
    REQUIRE(waitFor([&] { return !c.measuringLoudness(); }, 10000));
    // The setting that runs the check before an export is on by default and remembered.
    CHECK(c.checkBeforeExport());
    c.setCheckBeforeExport(false);
    CHECK_FALSE(ExportController().checkBeforeExport());
    c.setCheckBeforeExport(true);
}

TEST_CASE("Pre-export check dialog: rows, Go to, fixes and Export anyway; the Export button runs it; screenshots (176)",
          "[app][export][check][qml][screenshot][v07]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* exporter = shell.engine.singletonInstance<ExportController*>("Wabcut", "ExportController");
    REQUIRE(controller);
    REQUIRE(exporter);
    exporter->setVideoSourceFactory(ex::syntheticVideoSourceFactory());
    // A sequence of 1280 x 720 with nothing on it (a clean export), exported as a vertical video (a size warning).
    ProjectState state = emptyProject(1.0);
    Sequence seq = *state.sequences.front();
    seq.width = 1280;
    seq.height = 720;
    seq.captionTracks.push_back(CaptionTrack{CaptionTrackId{1}, "Captions", "en", true,
                                             {CaptionCue{Ticks{kTicksPerSecond / 5}, Ticks{kTicksPerSecond / 2}, "Hello", {}}}, {}, {}});
    state.sequences.front() = std::make_shared<const Sequence>(std::move(seq));
    controller->resetProject(std::move(state));
    shell.window->setProperty("workspace", 2);
    spin(300);
    QQuickItem* content = shell.window->contentItem();
    QQuickItem* panel = findWith(content, [](QQuickItem* i) { return i->property("gamingPreset").isValid(); });
    REQUIRE(panel);
    QObject* dialog = shell.object("preExportCheckDialog");
    REQUIRE(dialog);

    // The Check button opens the list; the captions the file would not show are a note, the size a warning.
    panel->setProperty("overrides", QVariantMap{{QStringLiteral("width"), 1080}, {QStringLiteral("height"), 1920}});
    spin(200);
    QQuickItem* check = byName(content, QStringLiteral("exportCheck"));
    REQUIRE(check);
    CHECK(check->isVisible());
    CHECK(check->property("enabled").toBool());
    REQUIRE(QMetaObject::invokeMethod(check, "clicked"));
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }, 5000));
    spin(400);
    (void)shell.grab();
    QQuickItem* body = qobject_cast<QQuickItem*>(dialog->property("contentItem").value<QObject*>());
    REQUIRE(body);
    const QVariantList rows = plain(dialog->property("rows")).toList();
    QStringList kinds;
    for (const QVariant& v : rows)
        kinds << v.toMap().value(QStringLiteral("kind")).toString();
    INFO(kinds.join(QLatin1Char(',')).toStdString());
    CHECK(kinds.contains(QStringLiteral("sizeMismatch")));
    CHECK(kinds.contains(QStringLiteral("captionsNotInFile")));
    CHECK(dialog->property("warningCount").toInt() >= 1);
    CHECK(dialog->property("noteCount").toInt() >= 1);
    CHECK(byName(body, QStringLiteral("preExportCheckSummary"))->property("text").toString().contains(QStringLiteral("warning")));
    REQUIRE(byName(body, QStringLiteral("preExportCheckList")));
    CHECK(byName(body, QStringLiteral("preExportCheckList"))->property("count").toInt() == rows.size());
    // The Check dialog opened from the button has no Export anyway.
    CHECK_FALSE(byName(body, QStringLiteral("preExportCheckExport"))->isVisible());
    CHECK(auditAccessibility(body).isEmpty());
    saveItemShot(shell, body->parentItem() ? body->parentItem() : body, QStringLiteral("pre-export-check-dark.png"));
    shell.theme->setProperty("dark", false);
    spin(700);
    (void)shell.grab();
    saveItemShot(shell, body->parentItem() ? body->parentItem() : body, QStringLiteral("pre-export-check-light.png"));
    shell.theme->setProperty("dark", true);

    // A fix that changes this export: "Use the sequence size" puts the sequence's own size into the overrides.
    int sizeRow = -1;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("sizeMismatch"))
            sizeRow = i;
    }
    REQUIRE(sizeRow >= 0);
    QQuickItem* fix = byName(body, QStringLiteral("preExportCheckFix_%1").arg(sizeRow));
    REQUIRE(fix);
    REQUIRE(fix->isVisible());
    REQUIRE(QMetaObject::invokeMethod(fix, "clicked"));
    spin(200);
    const QVariantMap overrides = plain(panel->property("overrides")).toMap();
    CHECK(overrides.value(QStringLiteral("width")).toInt() == 1280);
    CHECK(overrides.value(QStringLiteral("height")).toInt() == 720);
    QStringList after;
    for (const QVariant& v : plain(dialog->property("rows")).toList())
        after << v.toMap().value(QStringLiteral("kind")).toString();
    CHECK_FALSE(after.contains(QStringLiteral("sizeMismatch"))); // the list was checked again
    // Burn the captions in.
    int captionRow = -1;
    const QVariantList now = plain(dialog->property("rows")).toList();
    for (int i = 0; i < now.size(); ++i) {
        if (now[i].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("captionsNotInFile"))
            captionRow = i;
    }
    REQUIRE(captionRow >= 0);
    REQUIRE(QMetaObject::invokeMethod(byName(body, QStringLiteral("preExportCheckFix_%1").arg(captionRow)), "clicked"));
    spin(200);
    CHECK(plain(panel->property("overrides")).toMap().value(QStringLiteral("captions")).toString() == QStringLiteral("burnIn"));
    CHECK(plain(dialog->property("rows")).toList().isEmpty());
    CHECK(byName(body, QStringLiteral("preExportCheckSummary"))->property("text").toString().contains(QStringLiteral("looks ready")));
    QQuickItem* close = byName(body, QStringLiteral("preExportCheckClose"));
    REQUIRE(close);
    REQUIRE(QMetaObject::invokeMethod(close, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 5000));

    // The Export button: a clean export (the captions the file would not show are only a note) starts at once while the
    // setting is on, as it is by default ...
    CHECK(exporter->checkBeforeExport());
    const fs::path dir = freshDir("check-export");
    const auto outputTo = [&](const char* name) {
        panel->setProperty("outputPath", QStringLiteral("file:///") + QDir::fromNativeSeparators(q(dir / name)));
    };
    QQuickItem* exportButton = byName(content, QStringLiteral("exportButton"));
    REQUIRE(exportButton);
    panel->setProperty("overrides", QVariantMap{});
    outputTo("clean.mp4");
    spin(300);
    const int cleanJobs = exporter->jobs()->rowCount();
    REQUIRE(QMetaObject::invokeMethod(exportButton, "clicked"));
    REQUIRE(waitFor([&] { return exporter->jobs()->rowCount() > cleanJobs; }, 5000));
    CHECK_FALSE(dialog->property("visible").toBool());
    // ... one with a warning shows the list first, with Export anyway.
    panel->setProperty("overrides", QVariantMap{{QStringLiteral("width"), 1080}, {QStringLiteral("height"), 1920}});
    outputTo("warned.mp4");
    spin(300);
    const int jobs = exporter->jobs()->rowCount();
    REQUIRE(QMetaObject::invokeMethod(exportButton, "clicked"));
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }, 5000));
    CHECK(exporter->jobs()->rowCount() == jobs); // nothing queued yet
    CHECK(dialog->property("exportMode").toBool());
    QQuickItem* anyway = byName(body, QStringLiteral("preExportCheckExport"));
    REQUIRE(anyway);
    CHECK(anyway->isVisible());
    CHECK(anyway->property("enabled").toBool()); // a warning does not stop an export
    spin(200);
    (void)shell.grab();
    saveItemShot(shell, body->parentItem() ? body->parentItem() : body, QStringLiteral("pre-export-check-export-dark.png"));
    REQUIRE(QMetaObject::invokeMethod(anyway, "clicked"));
    REQUIRE(waitFor([&] { return exporter->jobs()->rowCount() > jobs; }, 5000));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 5000));

    // With the setting off, the Export button starts it without the list.
    exporter->setCheckBeforeExport(false);
    outputTo("unchecked.mp4");
    spin(300);
    const int jobs2 = exporter->jobs()->rowCount();
    REQUIRE(QMetaObject::invokeMethod(exportButton, "clicked"));
    REQUIRE(waitFor([&] { return exporter->jobs()->rowCount() > jobs2; }, 5000));
    CHECK_FALSE(dialog->property("visible").toBool());
    exporter->setCheckBeforeExport(true);

    // At the smallest window nothing of the dialog runs off its edge.
    shell.resize(1100, 700);
    REQUIRE(QMetaObject::invokeMethod(check, "clicked"));
    REQUIRE(waitFor([&] { return dialog->property("visible").toBool(); }, 5000));
    spin(400);
    (void)shell.grab();
    QQuickItem* card = body->parentItem() ? body->parentItem() : body;
    const QRectF cardRect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    CHECK(cardRect.left() >= 0);
    CHECK(cardRect.right() <= shell.window->width());
    CHECK(cardRect.top() >= 0);
    CHECK(cardRect.bottom() <= shell.window->height());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- #103 -----------------------------------------------------------------------------------------------------------

TEST_CASE("Newer codecs: the About dialog lists what plays and what is decode-only; Properties says it for a clip (103)",
          "[app][codecs][qml][screenshot][v07]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(library);

    const QVariantList table = library->codecSupport();
    REQUIRE(table.size() == 4);
    QMap<QString, QVariantMap> byId;
    for (const QVariant& v : table)
        byId.insert(v.toMap().value(QStringLiteral("id")).toString(), v.toMap());
    REQUIRE(byId.contains(QStringLiteral("apv")));
    CHECK(byId[QStringLiteral("apv")].value(QStringLiteral("plays")).toBool());
    CHECK_FALSE(byId[QStringLiteral("apv")].value(QStringLiteral("exports")).toBool());
    CHECK(byId[QStringLiteral("apv")].value(QStringLiteral("text")).toString().contains(QStringLiteral("Decode only")));
    CHECK(byId[QStringLiteral("av1-422")].value(QStringLiteral("text")).toString().contains(QStringLiteral("Decode only")));
    CHECK(byId[QStringLiteral("hevc-422")].value(QStringLiteral("exports")).toBool());
    CHECK(byId[QStringLiteral("hevc-422")].value(QStringLiteral("text")).toString().contains(QStringLiteral("8 bits")));
    CHECK(byId[QStringLiteral("mv-hevc")].value(QStringLiteral("text")).toString().contains(QStringLiteral("base view")));

    // Get Properties of an APV clip and of a 4:2:2 HEVC one carries the line; an ordinary H.264 clip does not.
    const auto linesFor = [&](const QString& codec, const QString& chroma) {
        QVariantMap video{{QStringLiteral("codec"), codec},    {QStringLiteral("profile"), QString()}, {QStringLiteral("width"), 1920},
                          {QStringLiteral("height"), 1080},    {QStringLiteral("frameRate"), 25.0},    {QStringLiteral("variableFrameRate"), false},
                          {QStringLiteral("pixelFormat"), QStringLiteral("yuv422p10le")}, {QStringLiteral("bitDepth"), 10},
                          {QStringLiteral("chroma"), chroma},  {QStringLiteral("fieldOrder"), QStringLiteral("progressive")},
                          {QStringLiteral("alpha"), false},    {QStringLiteral("rotation"), 0}};
        QVariantMap info{{QStringLiteral("name"), QStringLiteral("clip.mp4")}, {QStringLiteral("kind"), QStringLiteral("media")},
                         {QStringLiteral("container"), QStringLiteral("MP4")},
                         {QStringLiteral("duration"), QStringLiteral("0:10")}, {QStringLiteral("video"), video}};
        return library->propertyLines(info);
    };
    const auto supportLine = [&](const QVariantList& lines) -> QString {
        for (const QVariant& v : lines) {
            const QVariantMap m = v.toMap();
            if (m.value(QStringLiteral("label")).toString() == QStringLiteral("Codec support"))
                return m.value(QStringLiteral("value")).toString();
        }
        return {};
    };
    CHECK(supportLine(linesFor(QStringLiteral("apv"), QStringLiteral("4:2:2"))).contains(QStringLiteral("Decode only")));
    CHECK(supportLine(linesFor(QStringLiteral("hevc"), QStringLiteral("4:2:2"))).contains(QStringLiteral("Plays")));
    CHECK(supportLine(linesFor(QStringLiteral("av1"), QStringLiteral("4:2:2"))).contains(QStringLiteral("Decode only")));
    CHECK(supportLine(linesFor(QStringLiteral("h264"), QStringLiteral("4:2:2"))).isEmpty());
    CHECK(supportLine(linesFor(QStringLiteral("hevc"), QStringLiteral("4:2:0"))).isEmpty());

    // The About dialog lists the four.
    QObject* about = shell.object("aboutDialog");
    REQUIRE(about);
    REQUIRE(QMetaObject::invokeMethod(about, "open"));
    REQUIRE(waitFor([&] { return about->property("visible").toBool(); }, 5000));
    spin(400);
    (void)shell.grab();
    QQuickItem* body = qobject_cast<QQuickItem*>(about->property("contentItem").value<QObject*>());
    REQUIRE(body);
    for (const char* id : {"apv", "hevc-422", "av1-422", "mv-hevc"}) {
        QQuickItem* entry = byName(body, QStringLiteral("aboutCodec_") + QString::fromLatin1(id));
        INFO(id);
        REQUIRE(entry);
        CHECK(entry->isVisible());
    }
    CHECK(auditAccessibility(body).isEmpty());
    saveItemShot(shell, body->parentItem() ? body->parentItem() : body, QStringLiteral("about-codecs-dark.png"));
    shell.theme->setProperty("dark", false);
    spin(700);
    (void)shell.grab();
    saveItemShot(shell, body->parentItem() ? body->parentItem() : body, QStringLiteral("about-codecs-light.png"));
    shell.theme->setProperty("dark", true);
    shell.resize(1100, 700);
    spin(400);
    (void)shell.grab();
    QQuickItem* card = body->parentItem() ? body->parentItem() : body;
    const QRectF cardRect = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()));
    CHECK(cardRect.top() >= 0);
    CHECK(cardRect.bottom() <= shell.window->height());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

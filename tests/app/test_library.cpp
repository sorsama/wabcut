// M5k: the Library (Premiere's Project panel). Offscreen; no window is shown.
//   * bins: create, rename, nest, move (bridge and real drops on a bin card), delete, each one undo step;
//   * the list view: columns, sorting, resizing, reordering, the chooser; virtualised scrolling of 10,000 items (bench);
//   * metadata edits (name, label, comment, description, poster frame) as undoable edits;
//   * subclips: Make Subclip from the Source Monitor's marks, their range in the Library, three-point edits and
//     restricted trims;
//   * Find filters, unused media and duplicates, Reveal in File Explorer;
//   * hover scrub from the filmstrip atlas, with redacted frames hidden;
//   * screenshots: the grid with bins, the list view, a subclip.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "FilmstripSource.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "PosterFrames.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SourceMonitorBridge.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QMimeData>
#include <QPainter>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlExpression>
#include <QQmlProperty>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <numeric>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

QString key(MediaId id) { return mediaKey(id); }

/// A project from a timeline picture, a Library over it (no settings, a fake analysis scan run in place).
struct LibraryRig {
    Timeline tl;
    ProjectController controller;
    LibraryBridge library;

    explicit LibraryRig(std::string_view picture = R"(
        V1 tgt |AaaaBbbb....|
        A1 tgt |AaaaBbbb....|
    )")
        : tl(picture) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setAnalysisProbe([](const MediaItem& item, const std::filesystem::path&) {
            LibraryMediaStatus s;
            s.transcribed = item.name == "A" || item.name == "C";
            s.indexed = item.name == "A";
            s.shots = item.name == "B";
            return s;
        });
        library.setController(&controller);
    }
    MediaId media(char letter) const { return tl.media(letter); }
    std::size_t undo() { return controller.session().undoCount(); }
    const MediaItem& item(MediaId id) const { return *controller.snapshot()->findMedia(id); }
    QStringList keys() const {
        QStringList out;
        for (int r = 0; r < library.items()->rowCount(); ++r)
            out << library.items()->keyOf(r);
        return out;
    }
    QString cell(const QString& k, LibraryColumn column) const {
        return library.items()->cellText(library.items()->rowOf(k), column);
    }
};

/// Finds an item by objectName in the visual tree (delegates are not QObject children of the window).
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

/// Items in the visual tree under `root` (itself included).
int countItems(QQuickItem* root) {
    int n = 1;
    for (QQuickItem* child : root->childItems())
        n += countItems(child);
    return n;
}

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v.empty() ? 0.0 : v[std::min(v.size() - 1, static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5))];
}

} // namespace

TEST_CASE("Library: a sequence with nothing to draw (offline media) asks for no rendered thumbnail", "[app][library]") {
    // Release blocker (I.9): a sequence card asked the sequence-thumbnail provider for a frame it could not render, and
    // every such card warned "QQuickImage: no picture". The fixture's media have no files: offline.
    LibraryRig r;
    LibraryModel* items = r.library.items();
    int sequences = 0;
    for (int row = 0; row < items->rowCount(); ++row) {
        const QModelIndex index = items->index(row, 0);
        const QString kind = items->data(index, LibraryModel::KindRole).toString();
        INFO(items->keyOf(row).toStdString() << " " << kind.toStdString());
        if (kind == QStringLiteral("sequence")) {
            ++sequences;
            CHECK(items->data(index, LibraryModel::ThumbnailRole).toString().isEmpty());
        }
    }
    CHECK(sequences >= 1);
}

TEST_CASE("Library bins: create, rename, nest, move and delete, each one undo step", "[app][library]") {
    LibraryRig r;
    const QString a = key(r.media('A'));
    const QString b = key(r.media('B'));
    const QString c = key(r.media('C'));
    REQUIRE(r.library.items()->rowCount() == 26 + 1); // A..Z and the sequence, at the root
    CHECK(r.keys().front().startsWith(QLatin1Char('s'))); // sequences before media

    // New Bin: "Bin 01" at the root, first in the list, asks for a name.
    QString renamed;
    QObject::connect(&r.library, &LibraryBridge::renameRequested, [&](const QString& k) { renamed = k; });
    const auto before = r.undo();
    const QString footage = r.library.newBin();
    REQUIRE(footage.startsWith(QLatin1Char('b')));
    CHECK(r.undo() == before + 1);
    CHECK(renamed == footage);
    CHECK(r.keys().front() == footage); // bins first
    CHECK(r.cell(footage, LibraryColumn::Name) == QStringLiteral("Bin 01"));
    REQUIRE(r.library.rename(footage, QStringLiteral("Footage")));
    CHECK(r.cell(footage, LibraryColumn::Name) == QStringLiteral("Footage"));
    CHECK(r.undo() == before + 2);

    // Move two clips into it (one step); the bin shows them, the root no longer does.
    REQUIRE(r.library.moveItems({a, b}, footage));
    CHECK(r.undo() == before + 3);
    CHECK_FALSE(r.keys().contains(a));
    r.library.openBin(footage);
    CHECK(r.library.currentBin() == footage);
    CHECK(r.keys() == QStringList({a, b}));
    const QVariantList crumbs = r.library.breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Footage"));

    // A bin inside it; a bin cannot move into its own child.
    CHECK(r.item(r.media('A')).bin == *parseBinKey(footage));
    const QString day = r.library.newBin(QStringLiteral("Day 1"));
    REQUIRE_FALSE(day.isEmpty());
    CHECK(r.controller.snapshot()->findBin(*parseBinKey(day))->parent == *parseBinKey(footage));
    CHECK_FALSE(r.library.moveItems({footage}, day));
    CHECK_FALSE(r.library.message().isEmpty());

    // Undo walks back: the move, the rename, the bin.
    r.library.openBin(QString());
    r.controller.undo(); // Day 1
    r.controller.undo(); // the move
    CHECK(r.keys().contains(a));
    r.controller.redo();
    CHECK_FALSE(r.keys().contains(a));

    // New Bin from Selection: one step, named after the first item.
    r.library.select(c, LibraryBridge::Replace);
    const auto beforeFromSelection = r.undo();
    const QString fromSelection = r.library.newBinFromSelection();
    REQUIRE_FALSE(fromSelection.isEmpty());
    CHECK(r.undo() == beforeFromSelection + 1);
    CHECK(r.cell(fromSelection, LibraryColumn::Name) == QStringLiteral("C"));
    CHECK(r.item(r.media('C')).bin == *parseBinKey(fromSelection));

    // Tabs: a bin opened as a tab, closed again.
    r.library.openBinInTab(footage);
    CHECK(r.library.tabs().size() == 2);
    r.library.closeTab(footage);
    CHECK(r.library.tabs().size() == 1);
    CHECK(r.library.currentBin().isEmpty());

    // Deleting a bin whose media a sequence uses is refused (A is on V1); unused contents go with it (one step).
    CHECK_FALSE(r.library.deleteItems({footage}));
    const auto beforeDelete = r.undo();
    REQUIRE(r.library.deleteItems({fromSelection}));
    CHECK(r.undo() == beforeDelete + 1);
    CHECK(r.controller.snapshot()->findMedia(r.media('C')) == nullptr);
    r.controller.undo();
    CHECK(r.controller.snapshot()->findMedia(r.media('C')) != nullptr);
}

TEST_CASE("Library bins: dropping cards on a bin card moves them", "[app][library][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(library);
    REQUIRE(controller);
    library->setPersistent(false);
    const QString bin = library->newBin(QStringLiteral("Selects"));
    REQUIRE_FALSE(bin.isEmpty());
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* binCard = findNamed(shell.window->contentItem(), QStringLiteral("libraryCard_") + bin);
    REQUIRE(binCard);
    const MediaPtr first = controller->snapshot()->media.front();
    const QString moved = mediaKey(first->id);
    const auto undo = controller->session().undoCount();

    QMimeData mime;
    mime.setData(QStringLiteral("application/x-wabcut-library"), moved.toUtf8());
    const QPointF at = binCard->mapToScene(QPointF(binCard->width() / 2, binCard->height() / 3));
    QDragEnterEvent enter(at.toPoint(), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    QDragMoveEvent move(at.toPoint(), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &move);
    QDropEvent drop(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &drop);
    spin(30);
    CHECK(controller->snapshot()->findMedia(first->id)->bin == *parseBinKey(bin));
    CHECK(controller->session().undoCount() == undo + 1);
    takeMessages();
}

TEST_CASE("Library bins: a sequence card drags into a bin card", "[app][library][qml]") {
    // 2026-10-15 report: sequences (and nested ones) would not go into a bin. The drag carries the sequence (into a
    // bin, and onto the timeline to nest it) and a bin card takes it.
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(library);
    REQUIRE(controller);
    library->setPersistent(false);
    const QString bin = library->newBin(QStringLiteral("Selects"));
    REQUIRE_FALSE(bin.isEmpty());
    REQUIRE_FALSE(controller->snapshot()->sequences.empty());
    const SequenceId sequence = controller->snapshot()->sequences.front()->id;
    const QString sequenceCardKey = sequenceKey(sequence);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* root = shell.window->contentItem();
    QQuickItem* binCard = findNamed(root, QStringLiteral("libraryCard_") + bin);
    QQuickItem* sequenceCard = findNamed(root, QStringLiteral("librarySequence_") + sequenceCardKey);
    REQUIRE(binCard);
    REQUIRE(sequenceCard);

    // A press and drag on the sequence card: its drag goes active with the sequence's data. The system drag
    // (Drag.Automatic) would wait for the real mouse, so the test makes it a plain one (Drag.None).
    QPointer<QQuickItem> binCardAlive(binCard);
    QQuickItem* proxy = findNamed(sequenceCard, QStringLiteral("libraryCardDrag"));
    REQUIRE(proxy);
    QQmlContext* context = qmlContext(proxy);
    REQUIRE(QQmlProperty(proxy, QStringLiteral("Drag.dragType"), context).write(0));
    const QPointF from = sequenceCard->mapToScene(QPointF(sequenceCard->width() / 2, sequenceCard->height() / 3));
    const auto mouse = [&](QEvent::Type type, QPointF pos, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, pos, pos, button, buttons, Qt::NoModifier);
        QCoreApplication::sendEvent(shell.window.get(), &event);
    };
    mouse(QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= 8; ++i) {
        mouse(QEvent::MouseMove, from + QPointF(6.0 * i, 0), Qt::NoButton, Qt::LeftButton);
        spin(5);
    }
    REQUIRE(QQmlProperty(proxy, QStringLiteral("Drag.active"), context).read().toBool());
    const QVariantMap data = QQmlProperty(proxy, QStringLiteral("Drag.mimeData"), context).read().toMap();
    CHECK(data.value(QStringLiteral("application/x-wabcut-library")).toString() == sequenceCardKey);
    CHECK(data.value(QStringLiteral("application/x-wabcut-media")).toString() == sequenceCardKey);

    // While the drag holds (the card selected, its picture rendering), the bin card stays and lights up under it.
    spin(1500);
    REQUIRE(binCardAlive);
    CHECK(findNamed(root, QStringLiteral("libraryCard_") + bin) == binCard);
    QQuickItem* binDrop = nullptr;
    for (QQuickItem* child : binCard->childItems()) {
        if (QByteArray(child->metaObject()->className()).contains("DropArea"))
            binDrop = child;
    }
    REQUIRE(binDrop);
    const auto undo = controller->session().undoCount();
    QMimeData mime;
    for (auto it = data.cbegin(); it != data.cend(); ++it)
        mime.setData(it.key(), it.value().toString().toUtf8());
    const QPointF at = binCard->mapToScene(QPointF(binCard->width() / 2, binCard->height() / 3));
    QDragEnterEvent enter(at.toPoint(), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    QDragMoveEvent move(at.toPoint(), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &move);
    spin(5);
    CHECK(binDrop->property("containsDrag").toBool());
    QDropEvent drop(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(shell.window.get(), &drop);
    spin(30);
    mouse(QEvent::MouseButtonRelease, from + QPointF(48, 0), Qt::LeftButton, Qt::NoButton);
    spin(30);
    CHECK(controller->snapshot()->findSequence(sequence)->bin == *parseBinKey(bin));
    CHECK(controller->session().undoCount() == undo + 1);
    takeMessages();
}

TEST_CASE("Library list: metadata columns, sorting, resizing, reordering and the chooser", "[app][library]") {
    LibraryRig r(R"(
        V1 tgt |AaaaBbbb....|
    )");
    LibraryModel& model = *r.library.items();
    const QString a = key(r.media('A'));

    // The defaults: name, label, type, duration, fps, resolution, codec, audio, proxy, analysis, generated, date.
    const QVariantList columns = r.library.columns();
    REQUIRE(columns.size() == 12);
    CHECK(columns.front().toMap().value(QStringLiteral("id")).toString() == QStringLiteral("name"));
    CHECK(r.cell(a, LibraryColumn::MediaType) == QStringLiteral("Video and audio"));
    CHECK(r.cell(a, LibraryColumn::Duration) == QStringLiteral("00:00:40:00")); // 1000 frames at 25 fps
    CHECK(r.cell(a, LibraryColumn::FrameRate) == QStringLiteral("25 fps"));
    CHECK(r.cell(a, LibraryColumn::Resolution) == QStringLiteral("1920 × 1080"));
    CHECK(r.cell(a, LibraryColumn::Codec) == QStringLiteral("h264 / aac"));
    CHECK(r.cell(a, LibraryColumn::AudioChannels) == QStringLiteral("48 kHz Stereo, 2 stream(s)"));
    CHECK(r.cell(a, LibraryColumn::Analysis) == QStringLiteral("Transcribed · Indexed"));
    CHECK(r.cell(key(r.media('B')), LibraryColumn::Analysis) == QStringLiteral("Shots"));
    CHECK(r.cell(a, LibraryColumn::Label) == QStringLiteral("Violet"));
    CHECK(model.columnCount() == 12);

    // Sorting: by name descending, then by duration (C is made shorter).
    r.library.sortBy(QStringLiteral("name"));
    CHECK_FALSE(r.library.sortAscending());
    CHECK(r.keys()[1] == key(r.media('Z'))); // after the sequence
    ProjectState shorter = *r.controller.snapshot();
    for (MediaPtr& m : shorter.media) {
        if (m->name == "C") {
            MediaItem item = *m;
            item.info.duration = f(10);
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(r.controller.session().commit("Shorter", shorter));
    r.library.sortBy(QStringLiteral("duration"));
    CHECK(r.library.sortColumn() == QStringLiteral("duration"));
    CHECK(r.library.sortAscending());
    CHECK(r.keys()[1] == key(r.media('C')));

    // The chooser, resizing and moving a column.
    r.library.setColumnVisible(QStringLiteral("path"), true);
    r.library.setColumnVisible(QStringLiteral("name"), false); // the name always shows
    CHECK(r.library.columns().size() == 13);
    r.library.setColumnWidth(QStringLiteral("codec"), 300);
    const auto widthOf = [&](const QString& id) {
        for (const QVariant& c : r.library.columns()) {
            if (c.toMap().value(QStringLiteral("id")).toString() == id)
                return c.toMap().value(QStringLiteral("width")).toInt();
        }
        return -1;
    };
    CHECK(widthOf(QStringLiteral("codec")) == 300);
    r.library.moveColumn(6, 1); // codec next to the name
    CHECK(r.library.columns()[1].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("codec"));
    r.library.moveColumn(1, 3); // and two places to the right again
    CHECK(r.library.columns()[3].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("codec"));
    CHECK(r.library.columns()[1].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("label"));
    CHECK(model.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString() == QStringLiteral("Codec"));
    r.library.resetColumns();
    CHECK(r.library.columns().size() == 12);
    CHECK(r.library.sortColumn() == QStringLiteral("name"));
}

TEST_CASE("Library metadata edits are undoable: name, label, comment, description, poster frame", "[app][library]") {
    LibraryRig r;
    const QString a = key(r.media('A'));
    const QString b = key(r.media('B'));
    const auto start = r.undo();

    REQUIRE(r.library.rename(a, QStringLiteral("Interview")));
    REQUIRE(r.library.setLabel({a, b}, 6)); // two items, one step
    REQUIRE(r.library.setComment(a, QStringLiteral("Best take")));
    REQUIRE(r.library.setDescription(a, QStringLiteral("Wide, golden hour")));
    REQUIRE(r.library.setPosterFrame(a, 0.5));
    CHECK(r.undo() == start + 5);
    CHECK(r.controller.undoText().contains(QStringLiteral("Poster Frame")));
    CHECK(r.item(r.media('A')).name == "Interview");
    CHECK(r.item(r.media('B')).colorIndex == 6);
    CHECK(r.cell(a, LibraryColumn::Label) == QStringLiteral("Rose"));
    CHECK(r.item(r.media('A')).comment == "Best take");
    CHECK(r.item(r.media('A')).description == "Wide, golden hour");
    // The poster frame is a frame of the media: 500 frames into 1000.
    REQUIRE(r.item(r.media('A')).posterFrame);
    CHECK(*r.item(r.media('A')).posterFrame == f(500));
    CHECK(r.library.details(a).value(QStringLiteral("posterText")).toString() == QStringLiteral("00:00:20:00"));
    // Inline comment editing in the list goes through the same edit; empty names are refused.
    CHECK_FALSE(r.library.rename(a, QStringLiteral("  ")));
    for (int i = 0; i < 5; ++i)
        r.controller.undo();
    CHECK(r.item(r.media('A')).name == "A");
    CHECK(r.item(r.media('B')).colorIndex == 0);
    CHECK_FALSE(r.item(r.media('A')).posterFrame);
}

TEST_CASE("Make Subclip from the Source Monitor's marks; the subclip in three-point edits and restricted trims",
          "[app][library][subclip]") {
    LibraryRig r(R"(
        V1 tgt |........................|
        A1 tgt |........................|
    )");
    SourceMonitorBridge source;
    source.setController(&r.controller);
    r.library.setSourceMonitor(&source);
    const MediaId s = r.media('S');
    CHECK_FALSE(r.library.canMakeSubclip());
    REQUIRE(source.load(key(s)));
    CHECK(r.library.canMakeSubclip());
    source.seek(f(100));
    source.markIn();
    source.seek(f(119));
    source.markOut(); // [100, 120)

    const auto before = r.undo();
    const QString sub = r.library.makeSubclip(QStringLiteral("Take 2"), true);
    REQUIRE_FALSE(sub.isEmpty());
    CHECK(r.undo() == before + 1);
    const MediaItem& item = r.item(*parseMediaKey(sub));
    REQUIRE(item.subclip);
    CHECK(item.subclip->range == TimeRange{f(100), f(20)});
    CHECK(item.subclip->master == s);
    const QVariantMap details = r.library.details(sub);
    CHECK(details.value(QStringLiteral("kind")).toString() == QStringLiteral("subclip"));
    CHECK(details.value(QStringLiteral("inText")).toString() == QStringLiteral("00:00:04:00"));
    CHECK(details.value(QStringLiteral("outText")).toString() == QStringLiteral("00:00:04:19"));
    CHECK(r.cell(sub, LibraryColumn::Duration) == QStringLiteral("00:00:00:20"));
    CHECK(r.library.items()->data(r.library.items()->index(r.library.items()->rowOf(sub), 0), LibraryModel::KindRole)
              .toString() == QStringLiteral("subclip"));

    // Three-point edit from the subclip: its marks are its range.
    const bool loaded = source.load(sub);
    INFO(source.message().toStdString());
    REQUIRE(loaded);
    CHECK(source.inText() == QStringLiteral("00:00:04:00")); // the subclip's range is its marks
    REQUIRE(source.edit(true).isEmpty());
    const SequencePtr seq = r.controller.currentSequence();
    REQUIRE(seq->videoTracks.front()->clips.size() == 1);
    const Clip placed = seq->videoTracks.front()->clips[0];
    CHECK(placed.sourceIn == f(100));
    CHECK(placed.duration() == f(20));

    // Restricted: the clip's tail cannot grow past the subclip.
    auto trimmed = edit::trimEdge(*r.controller.snapshot(), r.tl.seq, placed.id, edit::Edge::Tail, f(30));
    REQUIRE(trimmed);
    CHECK(trimmed->state.findSequence(r.tl.seq)->videoTracks.front()->clips[0].duration() == f(20));
    // Edit Subclip: unrestricted, the whole file is usable.
    REQUIRE(r.library.editSubclip(sub, QStringLiteral("00:00:04:00"), QStringLiteral("00:00:04:19"), false));
    trimmed = edit::trimEdge(*r.controller.snapshot(), r.tl.seq, placed.id, edit::Edge::Tail, f(4));
    REQUIRE(trimmed);
    CHECK(trimmed->state.findSequence(r.tl.seq)->videoTracks.front()->clips[0].duration() == f(24));
    // Edit Subclip refuses an Out before the In.
    CHECK_FALSE(r.library.editSubclip(sub, QStringLiteral("00:00:05:00"), QStringLiteral("00:00:04:00"), true));
}

TEST_CASE("Find filters: text, type, label, analysis, offline, AI, unused, duplicates and dates", "[app][library]") {
    LibraryRig r(R"(
        V1 tgt |AaaaBbbb....|
    )");
    const QString a = key(r.media('A'));
    const QString b = key(r.media('B'));
    // Media facts for the filters: C offline, D generated, E a duplicate of F's file, G with a comment and a date.
    ProjectState s = *r.controller.snapshot();
    for (MediaPtr& m : s.media) {
        MediaItem item = *m;
        if (item.name == "C")
            item.locator.offline = true;
        if (item.name == "D")
            item.generated = GeneratedProvenance{"fal", "m", std::string(64, 'a'), "2026-10-11T09:30:00Z", "video",
                                                 "text-to-video", {}};
        if (item.name == "E" || item.name == "F")
            item.locator.fingerprint = SourceFingerprint{1234, 1, "00112233aabbccdd"};
        if (item.name == "G") {
            item.comment = "golden hour";
            item.info.modifiedTime = 1767225600; // 2026-01-01 UTC
        }
        m = std::make_shared<const MediaItem>(std::move(item));
    }
    REQUIRE(r.controller.session().commit("Facts", s));
    r.library.setLabel({b}, 3);

    const auto only = [&](const QVariantMap& filters, const QString& text = {}) {
        r.library.clearFilters();
        r.library.setFindText(text);
        r.library.setFilters(filters);
        QStringList keys = r.keys();
        keys.sort();
        return keys;
    };
    CHECK(only({}, QStringLiteral("golden")) == QStringList{key(r.media('G'))});
    CHECK(r.library.filtering());
    CHECK(only({{QStringLiteral("type"), QStringLiteral("sequence")}}) == QStringList{sequenceKey(r.tl.seq)});
    CHECK(only({{QStringLiteral("label"), 3}}) == QStringList{b});
    // The fake analysis says A and C are transcribed; C is offline now, so its file cannot be analysed.
    CHECK(only({{QStringLiteral("transcribed"), true}}) == QStringList{a});
    CHECK(only({{QStringLiteral("offline"), true}}) == QStringList{key(r.media('C'))});
    CHECK(only({{QStringLiteral("generated"), true}}) == QStringList{key(r.media('D'))});
    QStringList duplicates{key(r.media('E')), key(r.media('F'))};
    duplicates.sort();
    CHECK(only({{QStringLiteral("duplicates"), true}}) == duplicates);
    CHECK(r.library.duplicateGroups().size() == 1);
    CHECK(only({{QStringLiteral("unused"), true}}).size() == 24); // all but A and B
    CHECK(only({{QStringLiteral("from"), QStringLiteral("2025-12-31")}, {QStringLiteral("to"), QStringLiteral("2026-01-02")}})
          == QStringList{key(r.media('G'))});
    CHECK(r.library.filterCount() == 1);
    // Smart results are narrowed by the same filters (the text aside).
    r.library.setFilters({{QStringLiteral("generated"), true}});
    CHECK(r.library.passesFilters(key(r.media('D'))));
    CHECK_FALSE(r.library.passesFilters(a));
    // The Audio tab lists bins and audio-only media.
    r.library.clearFilters();
    r.library.setScope(QStringLiteral("audio"));
    CHECK(r.keys().isEmpty()); // the fixture's media all have video
    r.library.setScope(QStringLiteral("all"));
    CHECK_FALSE(r.keys().isEmpty());
}

TEST_CASE("Unused media and Remove Unused; duplicates skip subclips; Reveal shows the file", "[app][library]") {
    LibraryRig r(R"(
        V1 tgt |AaaaBbbb....|
    )");
    CHECK(r.library.unusedCount() == 24);
    // A subclip of A shares its file on purpose: not a duplicate.
    ProjectState s = *r.controller.snapshot();
    auto sub = edit::makeSubclip(s, r.media('A'), {f(0), f(10)}, "A.Subclip");
    REQUIRE(sub);
    REQUIRE(r.controller.session().commit("Subclip", sub->state));
    CHECK(r.library.duplicateGroups().isEmpty());
    CHECK(r.library.unusedCount() == 25);

    const auto before = r.undo();
    CHECK(r.library.removeUnused() == 25);
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.snapshot()->media.size() == 2);
    CHECK(r.library.unusedCount() == 0);
    r.controller.undo();
    CHECK(r.controller.snapshot()->media.size() == 27);

    QString revealed;
    LibraryBridge::setRevealer([&](const QString& path) {
        revealed = path;
        return true;
    });
    // The fixture's files do not exist: Reveal says so; a real file is revealed.
    CHECK_FALSE(r.library.reveal(key(r.media('A'))));
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("A.mov"));
    {
        QFile f(file);
        REQUIRE(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    ProjectState relinked = *r.controller.snapshot();
    for (MediaPtr& m : relinked.media) {
        if (m->name == "A") {
            MediaItem item = *m;
            item.locator.path = file.toStdString();
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(r.controller.session().commit("Relink", relinked));
    CHECK(r.library.reveal(key(r.media('A'))));
    CHECK(QFileInfo(revealed) == QFileInfo(file));
    LibraryBridge::setRevealer({}); // back to File Explorer / Finder
}

TEST_CASE("Hover scrub shows filmstrip tiles, never decodes, and hides redacted frames", "[app][library][filmstrip]") {
    LibraryRig r;
    // A ready atlas of 16 tiles one second apart for every media item.
    class Atlases final : public FilmstripSource {
    public:
        Atlases() {
            std::vector<QImage> images;
            std::vector<Ticks> times;
            for (int i = 0; i < 16; ++i) {
                QImage image(128, 72, QImage::Format_RGBA8888);
                image.fill(QColor::fromHsv(i * 20, 160, 200));
                images.push_back(image);
                times.push_back(Ticks::fromSeconds(i));
            }
            m_atlas = buildFilmstripAtlas(images, times, 72);
            m_atlas->serial = 1;
        }
        Lookup lookup(MediaId) const override {
            ++lookups;
            return {State::Ready, m_atlas};
        }
        mutable int lookups = 0;

    private:
        std::shared_ptr<FilmstripAtlas> m_atlas;
    };
    auto atlases = std::make_shared<Atlases>();
    RenderHarness harness(QSize(320, 180));
    REQUIRE(harness.initialize());
    auto scrub = std::make_unique<FilmstripScrub>();
    FilmstripScrub* raw = scrub.get();
    scrub->setParentItem(harness.window().contentItem());
    scrub->setSize(QSizeF(320, 180));
    scrub->setController(&r.controller);
    scrub->setSource(atlases);
    harness.own(std::move(scrub));
    raw->setMedia(key(r.media('A')));
    raw->setPosition(0.25); // 10 s into 40 s
    harness.frame();
    CHECK(raw->ready());
    CHECK(raw->timecode() == QStringLiteral("00:00:09:24"));
    CHECK(atlases->lookups > 0);

    // A redaction over 9-11 s of A: the tile there is hidden (the atlas is the raw file).
    Redaction plate;
    plate.media = r.media('A');
    plate.name = "Plate";
    plate.source = {Ticks::fromSeconds(9), Ticks::fromSeconds(2)};
    auto added = edit::addRedaction(*r.controller.snapshot(), plate);
    REQUIRE(added);
    REQUIRE(r.controller.session().commit("Redact", added->state));
    harness.frame();
    CHECK(raw->redacted());
    CHECK_FALSE(raw->ready());
    raw->setPosition(0.75);
    harness.frame();
    CHECK(raw->ready());
}

TEST_CASE("Library list virtualises 10,000 items: scroll p95 under 2 ms", "[app][library][perf][gpu]") {
    resetSettings();
    clearMessages();
    // 10,000 media items at the root.
    ProjectState state;
    for (int i = 0; i < 10'000; ++i) {
        MediaItem item;
        item.name = QStringLiteral("Clip %1").arg(i, 5, 10, QLatin1Char('0')).toStdString();
        item.locator.path = "D:/footage/" + item.name + ".mov";
        item.info.duration = f(250 + i % 100);
        VideoStreamInfo v;
        v.index = 0;
        v.codec = "h264";
        v.width = 1920;
        v.height = 1080;
        v.frameRate = kRate25;
        item.info.video.push_back(v);
        AudioStreamInfo au;
        au.index = 1;
        au.codec = "aac";
        au.channels = 2;
        item.info.audio.push_back(au);
        item.colorIndex = i % 16;
        auto added = edit::addMedia(state, std::move(item));
        REQUIRE(added);
        state = std::move(added->state);
    }
    auto seq = edit::addSequence(state, {});
    REQUIRE(seq);

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "LibraryListView"); // loads the module and its singletons
    INFO(component.errorString().toStdString());
    REQUIRE_FALSE(component.isError());
    auto* controller = engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* library = engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(controller);
    REQUIRE(library);
    library->setPersistent(false);
    library->setAnalysisProbe({});
    QElapsedTimer listing;
    listing.start();
    controller->resetProject(std::move(seq->state), seq->id);
    const double listMs = static_cast<double>(listing.nsecsElapsed()) / 1e6;
    REQUIRE(library->items()->rowCount() == 10'001);
    library->setView(QStringLiteral("list"));

    const QSize size(900, 500);
    RenderHarness harness(size);
    REQUIRE(harness.initialize());
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* view = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(view);
    view->setParentItem(harness.window().contentItem());
    view->setSize(QSizeF(size));
    harness.frame();
    QQuickItem* table = findNamed(view, QStringLiteral("libraryTable"));
    REQUIRE(table);
    harness.frame();
    // Cells exist for the rows in view only: 500 px of 28 px rows, 12 columns, a dozen items per cell.
    const int itemsAtStart = countItems(table);
    INFO("items in the table at start: " << itemsAtStart);
    CHECK(itemsAtStart > 100);   // cells were made
    CHECK(itemsAtStart < 6000);  // ... for the rows in view, not for 10,000
    REQUIRE(findNamed(table, QStringLiteral("libraryCellEditor")) == nullptr);

    // Scroll 240 frames of 23 px down and back, three passes, the best time of each frame (like the timeline bench,
    // which times its own update). The update is what a scroll costs the GUI thread: the new contentY, TableView's
    // polish (cells made or reused for the rows coming into view) and the scene graph sync, up to afterSynchronizing.
    // The full offscreen frame (rendering the whole window on this machine's RHI backend) is reported too.
    QElapsedTimer clock;
    clock.start();
    double syncedAt = 0.0;
    QObject::connect(&harness.window(), &QQuickWindow::afterSynchronizing, &harness.window(),
                     [&] { syncedAt = static_cast<double>(clock.nsecsElapsed()) / 1e6; }, Qt::DirectConnection);
    std::vector<double> best(240, 1e9);
    std::vector<double> bestFrame(240, 1e9);
    int maxItems = 0;
    for (int pass = 0; pass < 3; ++pass) {
        table->setProperty("contentY", 0.0);
        harness.frame();
        for (int i = 0; i < 240; ++i) {
            const double y = i < 120 ? i * 23.0 : (240 - i) * 23.0;
            const double start = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            table->setProperty("contentY", y);
            harness.frame();
            const double done = static_cast<double>(clock.nsecsElapsed()) / 1e6;
            const auto k = static_cast<std::size_t>(i);
            best[k] = std::min(best[k], syncedAt - start);
            bestFrame[k] = std::min(bestFrame[k], done - start);
        }
        maxItems = std::max(maxItems, countItems(table));
    }
    CHECK(maxItems < 6000); // scrolling reuses cells instead of piling them up
    const double p95 = percentile(best, 0.95);
    const double mean = std::accumulate(best.begin(), best.end(), 0.0) / static_cast<double>(best.size());
    const double frameP95 = percentile(bestFrame, 0.95);
    INFO("scroll update mean " << mean << " ms, p95 " << p95 << " ms; full frame p95 " << frameP95 << " ms; listing "
                               << listMs << " ms");
    CHECK(p95 <= 2.0);
    // Sorting 10,000 rows by duration and by name.
    QElapsedTimer sorting;
    sorting.start();
    library->sortBy(QStringLiteral("duration"));
    library->sortBy(QStringLiteral("name"));
    const double sortMs = static_cast<double>(sorting.nsecsElapsed()) / 1e6 / 2.0;
    writeOutputFile(QStringLiteral("library-perf.txt"),
                    QStringLiteral("Library list, 10,000 media items, view 900x500, 28 px rows, 12 columns\n"
                                   "Listing on a new project: %1 ms\n"
                                   "Scroll update (240 frames of 23 px, best of 3 passes; contentY + polish + sync): "
                                   "mean %2 ms, p95 %3 ms (limit 2 ms)\n"
                                   "Full offscreen frame incl. rendering the window: p95 %5 ms\n"
                                   "Items in the table: %6 at start, at most %7 while scrolling\n"
                                   "Sort 10,000 rows: %4 ms per sort\n")
                        .arg(listMs, 0, 'f', 1)
                        .arg(mean, 0, 'f', 3)
                        .arg(p95, 0, 'f', 3)
                        .arg(sortMs, 0, 'f', 1)
                        .arg(frameP95, 0, 'f', 3)
                        .arg(itemsAtStart)
                        .arg(maxItems));
    view->setParentItem(nullptr);
    object.reset();
}

TEST_CASE("Screenshots: the Library grid with bins, the list view and a subclip", "[app][library][screenshots][qml]") {
    for (const char* shot : {"library-grid-bins.png", "library-list.png", "library-subclip.png"}) {
        resetSettings();
        clearMessages();
        Shell shell;
        REQUIRE(shell.window != nullptr);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
        auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        REQUIRE(library);
        REQUIRE(controller);
        library->setPersistent(false);
        // Two bins, one with clips, a labelled and commented item, a subclip.
        const ProjectSnapshot snap = controller->snapshot();
        REQUIRE(snap->media.size() >= 4);
        const QString footage = library->newBin(QStringLiteral("Footage"));
        library->newBin(QStringLiteral("Music"));
        REQUIRE(library->moveItems({mediaKey(snap->media[2]->id), mediaKey(snap->media[3]->id)}, footage));
        library->setLabel({mediaKey(snap->media[0]->id)}, 7);
        library->setComment(mediaKey(snap->media[0]->id), QStringLiteral("Hero shot"));
        // The subclip covers frames 240-480 of the hero shot (its marks, whole frames of its rate), so its range shows.
        const MediaItem& hero = *snap->media[0];
        const Rate heroRate = hero.info.hasVideo() && hero.info.video.front().frameRate.isValid()
                                  ? hero.info.video.front().frameRate
                                  : kRate25;
        auto marked = edit::setSourceMarks(*controller->snapshot(), hero.id,
                                           {framesToTicks(240, heroRate), framesToTicks(480, heroRate)});
        REQUIRE(marked);
        REQUIRE(controller->session().commit("Marks", marked->state));
        const QString sub = library->makeSubclipOf(mediaKey(snap->media[0]->id), QStringLiteral("Hero, the smile"), true);
        REQUIRE_FALSE(sub.isEmpty());
        library->select(sub, LibraryBridge::Replace);
        const bool list = QLatin1String(shot) != QLatin1String("library-grid-bins.png");
        if (list) {
            // The list's columns need room: the browser widened as a gutter drag does (40 % of the work area at most).
            QQuickItem* editor = shell.item("editorShell");
            REQUIRE(editor);
            editor->setProperty("userLibraryWidth", 640.0);
            library->setView(QStringLiteral("list"));
        }
        if (QLatin1String(shot) == QLatin1String("library-list.png")) {
            library->setColumnVisible(QStringLiteral("comment"), true);
            library->moveColumn(12, 2); // the comment next to the label, in view
        }
        if (QLatin1String(shot) == QLatin1String("library-subclip.png")) {
            // Media In / Out next to the name: the subclip's range.
            library->setColumnVisible(QStringLiteral("in"), true);
            library->setColumnVisible(QStringLiteral("out"), true);
            const QVariantList visible = library->columns();
            int in = -1;
            for (int i = 0; i < visible.size(); ++i) {
                if (visible[i].toMap().value(QStringLiteral("id")).toString() == QStringLiteral("in"))
                    in = i;
            }
            REQUIRE(in > 0);
            library->moveColumn(in, 1);     // In after the name
            library->moveColumn(in + 1, 2); // Out after it
        }
        spin(50);
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        QQuickItem* panel = shell.item("libraryPanel");
        REQUIRE(panel);
        const QRectF rect = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
        const double dpr = image.devicePixelRatio();
        REQUIRE(image.copy(QRect(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                                 static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr)))
                    .save(QDir(outputDir()).filePath(QString::fromLatin1(shot))));
        const QStringList messages = takeMessages();
        INFO(join(messages));
        CHECK(messages.isEmpty());
    }
}

TEST_CASE("Library keys: Premiere's Project panel commands", "[app][library][keys]") {
    LibraryRig r;
    const QString a = key(r.media('A'));
    CHECK(LibraryBridge::handlesCommand(QStringLiteral("project.newBin")));
    CHECK_FALSE(LibraryBridge::handlesCommand(QStringLiteral("timeline.rippleDelete")));

    // New Bin, the views and hover scrub.
    const auto before = r.undo();
    REQUIRE(r.library.runCommand(QStringLiteral("project.newBin")));
    CHECK(r.undo() == before + 1);
    REQUIRE(r.library.runCommand(QStringLiteral("project.listView")));
    CHECK(r.library.view() == QStringLiteral("list"));
    REQUIRE(r.library.runCommand(QStringLiteral("project.toggleView")));
    CHECK(r.library.view() == QStringLiteral("grid"));
    const bool scrub = r.library.hoverScrub();
    REQUIRE(r.library.runCommand(QStringLiteral("project.hoverScrub")));
    CHECK(r.library.hoverScrub() != scrub);

    // Delete needs a selection; Find and Make Subclip ask the panel.
    r.library.clearSelection();
    CHECK_FALSE(r.library.commandAvailability(QStringLiteral("project.delete")).value(QStringLiteral("enabled")).toBool());
    r.library.select(key(r.media('Z')), LibraryBridge::Replace);
    REQUIRE(r.library.runCommand(QStringLiteral("project.delete")));
    CHECK(r.controller.snapshot()->findMedia(r.media('Z')) == nullptr);
    bool find = false;
    QObject::connect(&r.library, &LibraryBridge::findRequested, [&] { find = true; });
    REQUIRE(r.library.runCommand(QStringLiteral("edit.find")));
    CHECK(find);
    CHECK_FALSE(r.library.runCommand(QStringLiteral("clip.makeSubclip"))); // no Source Monitor
    SourceMonitorBridge source;
    source.setController(&r.controller);
    r.library.setSourceMonitor(&source);
    REQUIRE(source.load(a));
    bool asked = false;
    QObject::connect(&r.library, &LibraryBridge::makeSubclipRequested, [&] { asked = true; });
    REQUIRE(r.library.runCommand(QStringLiteral("clip.makeSubclip")));
    CHECK(asked);
    // Open in Source Monitor: the selected media.
    r.library.select(key(r.media('B')), LibraryBridge::Replace);
    REQUIRE(r.library.runCommand(QStringLiteral("project.openInSourceMonitor")));
    CHECK(source.mediaKeyText() == key(r.media('B')));
}

// 0.2: Import Image Sequence from the Library (media.importImageSequence through the bridge's own reader) lands in the
// current bin as one undo step; Premiere's Project panel zoom steps the thumbnails in quarters between 0.5 and 2.
TEST_CASE("Library: Import Image Sequence adds numbered stills as one clip in the current bin", "[app][library]") {
    LibraryRig r;
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    for (int i = 1; i <= 6; ++i) {
        QImage still(32, 18, QImage::Format_RGB32);
        still.fill(qRgb(30 * i, 30 * i, 30 * i));
        REQUIRE(still.save(dir.filePath(QStringLiteral("render_%1.png").arg(i, 3, 10, QLatin1Char('0'))), "PNG"));
    }
    const QString bin = r.library.newBin(QStringLiteral("Renders"));
    REQUIRE_FALSE(bin.isEmpty());
    r.library.openBin(bin);
    const auto before = r.undo();
    const QString added = r.library.importImageSequence(QUrl::fromLocalFile(dir.filePath(QStringLiteral("render_001.png"))), 25.0);
    REQUIRE_FALSE(added.isEmpty());
    CHECK(r.undo() == before + 1);
    const auto id = parseMediaKey(added);
    REQUIRE(id);
    const MediaItem& item = r.item(*id);
    CHECK(item.name == "render_001.png");
    CHECK_FALSE(item.info.isStillImage);
    REQUIRE(item.info.video.size() == 1);
    CHECK(item.info.video.front().frameRate == Rate{25, 1});
    CHECK(item.info.video.front().frameCount == 6);
    CHECK(item.bin == r.library.bin());
    CHECK(r.library.isSelected(added));

    // A file without a frame number is refused with a message and changes nothing.
    QImage plain(8, 8, QImage::Format_RGB32);
    plain.fill(Qt::black);
    REQUIRE(plain.save(dir.filePath(QStringLiteral("poster.png")), "PNG"));
    CHECK(r.library.importImageSequence(QUrl::fromLocalFile(dir.filePath(QStringLiteral("poster.png"))), 25.0).isEmpty());
    CHECK_FALSE(r.library.message().isEmpty());
    CHECK(r.undo() == before + 1);
}

TEST_CASE("Library: thumbnail zoom steps in quarters between half and double size", "[app][library]") {
    LibraryRig r;
    CHECK(r.library.thumbnailScale() == 1.0);
    r.library.setThumbnailScale(1.3);
    CHECK(r.library.thumbnailScale() == 1.25);
    r.library.setThumbnailScale(9.0);
    CHECK(r.library.thumbnailScale() == 2.0);
    r.library.setThumbnailScale(0.1);
    CHECK(r.library.thumbnailScale() == 0.5);
    r.library.setThumbnailScale(std::numeric_limits<double>::quiet_NaN());
    CHECK(r.library.thumbnailScale() == 1.0);
}
// UX rework (L): Premiere's Project panel zoom keys (Shift+] / Shift+[ and = / -) step the card size, and its selection
// keys (arrows, Home, End, PageUp, PageDown, Shift+arrows) ask the panel to move the selection in the view it shows.
// Before, the keymap resolved them in the Library and nothing ran them: the keys were swallowed.
TEST_CASE("Library keys: zoom keys step the card size and selection keys ask the panel", "[app][library][keys]") {
    LibraryRig r;
    for (const char* id : {"project.thumbnailSizeNext", "project.thumbnailSizePrevious", "project.zoomIn",
                           "project.zoomOut", "project.moveSelectionUp", "project.moveSelectionDown",
                           "project.moveSelectionLeft", "project.moveSelectionRight", "project.moveSelectionHome",
                           "project.moveSelectionEnd", "project.moveSelectionPageUp", "project.moveSelectionPageDown",
                           "project.extendSelectionUp", "project.extendSelectionDown", "project.extendSelectionLeft",
                           "project.extendSelectionRight"}) {
        INFO(id);
        CHECK(LibraryBridge::handlesCommand(QString::fromLatin1(id)));
        CHECK(r.library.commandAvailability(QString::fromLatin1(id)).value(QStringLiteral("handled")).toBool());
    }
    CHECK_FALSE(LibraryBridge::handlesCommand(QStringLiteral("project.nextColumnField")));

    // The slider's bounds, and the keys stepping between them.
    CHECK(r.library.thumbnailScaleMin() == 0.5);
    CHECK(r.library.thumbnailScaleMax() == 2.0);
    CHECK(r.library.thumbnailScaleStep() == 0.25);
    REQUIRE(r.library.thumbnailScale() == 1.0);
    REQUIRE(r.library.runCommand(QStringLiteral("project.thumbnailSizeNext")));
    CHECK(r.library.thumbnailScale() == 1.25);
    REQUIRE(r.library.runCommand(QStringLiteral("project.zoomIn")));
    CHECK(r.library.thumbnailScale() == 1.5);
    REQUIRE(r.library.runCommand(QStringLiteral("project.zoomOut")));
    REQUIRE(r.library.runCommand(QStringLiteral("project.thumbnailSizePrevious")));
    CHECK(r.library.thumbnailScale() == 1.0);
    // At a bound the key stays the Library's (= never zooms the sequence instead) and the palette says why.
    r.library.setThumbnailScale(2.0);
    const QVariantMap atMax = r.library.commandAvailability(QStringLiteral("project.zoomIn"));
    CHECK_FALSE(atMax.value(QStringLiteral("enabled")).toBool());
    CHECK_FALSE(atMax.value(QStringLiteral("reason")).toString().isEmpty());
    CHECK(r.library.commandAvailability(QStringLiteral("project.zoomOut")).value(QStringLiteral("enabled")).toBool());
    CHECK(r.library.runCommand(QStringLiteral("project.zoomIn")));
    CHECK(r.library.thumbnailScale() == 2.0);
    r.library.setThumbnailScale(0.5);
    CHECK_FALSE(r.library.commandAvailability(QStringLiteral("project.thumbnailSizePrevious")).value(QStringLiteral("enabled")).toBool());
    CHECK(r.library.runCommand(QStringLiteral("project.thumbnailSizePrevious")));
    CHECK(r.library.thumbnailScale() == 0.5);

    // The selection keys: the panel moves the selection (it knows its columns and page size).
    REQUIRE(r.library.items()->rowCount() > 0);
    QStringList moves;
    QObject::connect(&r.library, &LibraryBridge::selectionMoveRequested, [&](const QString& where, bool extend) {
        moves << (extend ? where + QStringLiteral("+") : where);
    });
    for (const char* id : {"project.moveSelectionHome", "project.moveSelectionEnd", "project.moveSelectionPageUp",
                           "project.moveSelectionPageDown", "project.moveSelectionDown", "project.moveSelectionRight",
                           "project.extendSelectionLeft", "project.extendSelectionUp"}) {
        INFO(id);
        CHECK(r.library.commandAvailability(QString::fromLatin1(id)).value(QStringLiteral("enabled")).toBool());
        CHECK(r.library.runCommand(QString::fromLatin1(id)));
    }
    CHECK(moves == (QStringList{QStringLiteral("home"), QStringLiteral("end"), QStringLiteral("pageUp"),
                                QStringLiteral("pageDown"), QStringLiteral("down"), QStringLiteral("right"),
                                QStringLiteral("left+"), QStringLiteral("up+")}));
}

// ---- 0.4: Import Folder, File > Import into the open bin, and where import dialogs open ----

namespace {

std::filesystem::path libraryTestMedia(const char* name) { return std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveLibraryTestMedia() {
    return std::filesystem::is_regular_file(libraryTestMedia("h264_1080p60_aac.mp4")) &&
           std::filesystem::is_regular_file(libraryTestMedia("sine_48k.wav"));
}

/// Copies test media `name` to `to` (folders made).
void copyMedia(const char* name, const QString& to) {
    REQUIRE(QDir().mkpath(QFileInfo(to).absolutePath()));
    QFile::remove(to);
    REQUIRE(QFile::copy(QString::fromStdU16String(libraryTestMedia(name).u16string()), to));
}

/// A Library and ProjectFile over one controller, with a new (not demo) project.
struct ImportRig {
    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    LibraryBridge library;

    ImportRig() {
        library.setPersistent(false);
        library.setSynchronous(true);
        library.setController(&controller);
        lifecycle.newProject(); // the demo ignores `bin`: its import starts a new project
    }
    const Bin* binNamed(const std::string& name) const {
        const ProjectSnapshot snap = controller.snapshot();
        for (const Bin& b : snap->bins) {
            if (b.name == name)
                return &b;
        }
        return nullptr;
    }
    BinId binIdNamed(const std::string& name) const {
        const Bin* b = binNamed(name);
        return b ? b->id : BinId{};
    }
    BinId parentOf(const std::string& name) const {
        const Bin* b = binNamed(name);
        return b ? b->parent : BinId{};
    }
};

} // namespace

TEST_CASE("Library: Import Folder puts the folder in the open bin as nested bins in one undo step", "[app][library][import]") {
    if (!haveLibraryTestMedia())
        SKIP("test media missing");
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString session = dir.filePath(QStringLiteral("Session1"));
    copyMedia("h264_1080p60_aac.mp4", session + QStringLiteral("/a.mp4"));
    copyMedia("sine_48k.wav", session + QStringLiteral("/Cam B/b.wav"));
    {
        QFile notes(session + QStringLiteral("/notes.txt"));
        REQUIRE(notes.open(QIODevice::WriteOnly));
        notes.write("not media");
    }
    ImportRig r;
    const QString target = r.library.newBin(QStringLiteral("Target"));
    REQUIRE_FALSE(target.isEmpty());
    r.library.openBin(target);
    REQUIRE(r.library.currentBin() == target);
    const BinId targetId = r.library.bin();
    const auto before = r.controller.session().undoCount();

    const QVariantMap result =
        r.lifecycle.importFiles({QUrl::fromLocalFile(session)}, {{QStringLiteral("bin"), r.library.currentBin()}});
    CHECK(result.value("added").toInt() == 2);
    CHECK(result.value("folders").toInt() == 1);
    CHECK(result.value("bins").toInt() == 2);
    CHECK_FALSE(result.value("truncated").toBool());
    const QVariantList skipped = result.value("skipped").toList();
    REQUIRE(skipped.size() == 1);
    CHECK(skipped.front().toMap().value("name").toString() == QStringLiteral("notes.txt"));
    CHECK(skipped.front().toMap().value("reason").toString() == QStringLiteral("notMedia"));
    CHECK(result.value("folderSummary").toString() ==
          QStringLiteral("Imported 2 files from Session1 into 2 bins. 1 file was left out (not media)."));
    CHECK(r.controller.session().undoCount() == before + 1);
    CHECK(r.controller.session().undoLabel() == "Import Folder");

    const BinId folderBin = r.binIdNamed("Session1");
    const BinId camBin = r.binIdNamed("Cam B");
    REQUIRE(folderBin.isValid());
    REQUIRE(camBin.isValid());
    CHECK(r.parentOf("Session1") == targetId);
    CHECK(r.parentOf("Cam B") == folderBin);
    for (const MediaPtr& m : r.controller.snapshot()->media) {
        INFO(m->name);
        CHECK(m->bin == (m->name == "a.mp4" ? folderBin : camBin));
    }
    CHECK(r.lifecycle.detectImport(QUrl::fromLocalFile(session)).value("kind").toString() == QStringLiteral("folder"));

    // One undo takes the items and both bins away.
    REQUIRE(r.controller.undo());
    CHECK(r.controller.snapshot()->media.empty());
    CHECK_FALSE(r.binIdNamed("Session1").isValid());
    CHECK_FALSE(r.binIdNamed("Cam B").isValid());
    CHECK(r.binIdNamed("Target").isValid());

    // A folder without media: nothing is added, the toast says why.
    const QString notes = dir.filePath(QStringLiteral("Notes"));
    REQUIRE(QDir().mkpath(notes));
    {
        QFile text(notes + QStringLiteral("/readme.txt"));
        REQUIRE(text.open(QIODevice::WriteOnly));
    }
    const QVariantMap none = r.lifecycle.importFiles({QUrl::fromLocalFile(notes)}, {{QStringLiteral("bin"), target}});
    CHECK(none.value("added").toInt() == 0);
    CHECK(none.value("bins").toInt() == 0);
    CHECK(none.value("folderSummary").toString() == QStringLiteral("No media in Notes: 1 file was left out (not media)."));
}

TEST_CASE("Library: File Import puts plain files in the open bin", "[app][library][import]") {
    if (!haveLibraryTestMedia())
        SKIP("test media missing");
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    copyMedia("sine_48k.wav", dir.filePath(QStringLiteral("tone.wav")));
    copyMedia("sine_48k.wav", dir.filePath(QStringLiteral("tone2.wav")));
    copyMedia("sine_48k.wav", dir.filePath(QStringLiteral("tone3.wav")));
    ImportRig r;
    const QString clips = r.library.newBin(QStringLiteral("Clips"));
    REQUIRE_FALSE(clips.isEmpty());
    r.library.openBin(clips);
    const BinId clipsId = r.library.bin();

    const QVariantMap result = r.lifecycle.importFiles({QUrl::fromLocalFile(dir.filePath(QStringLiteral("tone.wav")))},
                                                       {{QStringLiteral("bin"), r.library.currentBin()}});
    CHECK(result.value("added").toInt() == 1);
    CHECK(result.value("folders").toInt() == 0);
    CHECK(result.value("folderSummary").toString().isEmpty());
    CHECK(r.controller.session().undoLabel() == "Import");
    const auto binOf = [&](const char* name) {
        for (const MediaPtr& m : r.controller.snapshot()->media) {
            if (m->name == name)
                return m->bin;
        }
        return BinId{};
    };
    CHECK(binOf("tone.wav") == clipsId);

    // A search bin holds nothing: its parent gets the file. An unknown bin: the root.
    const QString search =
        r.library.newSearchBin(QStringLiteral("Find tone"), {{QStringLiteral("text"), QStringLiteral("tone")}});
    REQUIRE_FALSE(search.isEmpty());
    r.lifecycle.importFiles({QUrl::fromLocalFile(dir.filePath(QStringLiteral("tone2.wav")))}, {{QStringLiteral("bin"), search}});
    CHECK(binOf("tone2.wav") == clipsId);
    r.lifecycle.importFiles({QUrl::fromLocalFile(dir.filePath(QStringLiteral("tone3.wav")))},
                            {{QStringLiteral("bin"), QStringLiteral("b999999")}});
    CHECK(binOf("tone3.wav") == BinId{});
}

TEST_CASE("Library: import dialogs open where the last import came from", "[app][library][import]") {
    QSettings().remove(QStringLiteral("import"));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    for (const char* sub : {"a", "b/Session", "c1", "c2", "c3", "c4", "c5", "c6"})
        REQUIRE(QDir().mkpath(dir.filePath(QString::fromLatin1(sub))));
    const auto same = [](const QString& a, const QString& b) {
        return QDir::cleanPath(QDir::fromNativeSeparators(a)) == QDir::cleanPath(b);
    };

    ProjectController controller;
    ProjectLifecycle lifecycle(&controller);
    CHECK(lifecycle.importFolder() == lifecycle.folder()); // nothing remembered yet
    int changes = 0;
    QObject::connect(&lifecycle, &ProjectLifecycle::importFolderChanged, [&] { ++changes; });

    // A file chosen: its folder.
    lifecycle.rememberImportFolder({QUrl::fromLocalFile(dir.filePath(QStringLiteral("a/x.mp4")))});
    CHECK(changes == 1);
    CHECK(same(lifecycle.importFolder().toLocalFile(), dir.filePath(QStringLiteral("a"))));
    {
        ProjectController other;
        ProjectLifecycle fresh(&other);
        CHECK(same(fresh.importFolder().toLocalFile(), dir.filePath(QStringLiteral("a"))));
    }
    // A folder chosen: its parent (the next sibling is one click away).
    lifecycle.rememberImportFolder({QUrl::fromLocalFile(dir.filePath(QStringLiteral("b/Session")))});
    CHECK(same(lifecycle.importFolder().toLocalFile(), dir.filePath(QStringLiteral("b"))));
    // That folder gone: the project's folder again.
    REQUIRE(QDir(dir.filePath(QStringLiteral("b"))).removeRecursively());
    CHECK(lifecycle.importFolder() == lifecycle.folder());

    // The recent folders: newest first, five at most, only folders that are there.
    for (const char* sub : {"c1", "c2", "c3", "c4", "c5", "c6"})
        lifecycle.rememberImportFolder({QUrl::fromLocalFile(dir.filePath(QString::fromLatin1(sub) + QStringLiteral("/x.mp4")))});
    const QStringList recent = ProjectLifecycle::recentImportFolders();
    REQUIRE(recent.size() == 5);
    CHECK(same(recent[0], dir.filePath(QStringLiteral("c6"))));
    CHECK(same(recent[4], dir.filePath(QStringLiteral("c2"))));
    REQUIRE(QDir(dir.filePath(QStringLiteral("c4"))).removeRecursively());
    CHECK(ProjectLifecycle::recentImportFolders().size() == 4);

    // Fix round 1 (D): a folder on a network share is taken as remembered, without asking the share (one that does
    // not answer would hold the window at start-up for the redirector's timeout; the dialog falls back by itself).
    const QString share = QStringLiteral("//wabcut-no-such-host.invalid/footage/Session");
    QSettings().setValue(QStringLiteral("import/lastFolder"), QDir::toNativeSeparators(share));
    QSettings().setValue(QStringLiteral("import/recentFolders"), QStringList{QDir::toNativeSeparators(share)});
    CHECK(same(lifecycle.importFolder().toLocalFile(), share));
    const QStringList remote = ProjectLifecycle::recentImportFolders();
    REQUIRE(remote.size() == 1);
    CHECK(same(remote.front(), share));
    QSettings().remove(QStringLiteral("import"));
}

TEST_CASE("Library: a folder dropped on the timeline comes in as bins and only the loose files are edited in",
          "[app][library][import]") {
    if (!haveLibraryTestMedia())
        SKIP("test media missing");
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString captures = dir.filePath(QStringLiteral("Captures"));
    copyMedia("sine_48k.wav", captures + QStringLiteral("/a.wav"));
    copyMedia("sine_48k.wav", captures + QStringLiteral("/Day2/b.wav"));
    copyMedia("sine_48k.wav", dir.filePath(QStringLiteral("loose.wav")));
    ImportRig r;
    REQUIRE(r.controller.currentSequence());
    const auto clipsOnTimeline = [&] {
        std::size_t n = 0;
        if (const SequencePtr seq = r.controller.currentSequence())
            seq->forEachTrack([&](const Track& t) { n += t.clips.size(); });
        return n;
    };
    const std::size_t before = r.controller.session().undoCount();

    // Fix round 1 (D): the folder becomes bins at the Library's root in the import's one step and stays off the
    // timeline (a capture folder would have put hundreds of clips there, an undo step each); the loose file is edited
    // in as before; the toast says where the folder went.
    const QVariantMap result =
        r.lifecycle.dropFiles({QUrl::fromLocalFile(captures), QUrl::fromLocalFile(dir.filePath(QStringLiteral("loose.wav")))},
                              QStringLiteral("00:00:00:00"), QString(), true);
    CHECK(result.value("added").toInt() == 3);
    CHECK(result.value("folders").toInt() == 1);
    CHECK(result.value("bins").toInt() == 2);
    CHECK(result.value("placed").toInt() == 1);
    CHECK(clipsOnTimeline() == 1);
    CHECK(r.controller.selection().size() == 1);
    REQUIRE(r.binIdNamed("Captures").isValid());
    CHECK(r.parentOf("Captures") == BinId{});
    CHECK(r.parentOf("Day2") == r.binIdNamed("Captures"));
    CHECK(result.value("folderSummary").toString() ==
          QStringLiteral("Imported 3 files from Captures into 2 bins. Folders go into the Library as bins: drag their "
                         "clips onto the timeline from there."));
    // The import (with the folder) is one step and the loose file's edit another, as a drop of files always made.
    CHECK(r.controller.session().undoCount() == before + 2);
    REQUIRE(r.controller.undo());
    CHECK(clipsOnTimeline() == 0);
    CHECK(r.controller.session().undoLabel() == "Import Folder");
    REQUIRE(r.controller.undo());
    CHECK(r.controller.snapshot()->media.empty());
    CHECK_FALSE(r.binIdNamed("Captures").isValid());

    // A folder alone: bins, nothing on the timeline, one undo step.
    const QVariantMap alone = r.lifecycle.dropFiles({QUrl::fromLocalFile(captures)}, QStringLiteral("00:00:00:00"), QString(), true);
    CHECK(alone.value("added").toInt() == 2);
    CHECK(alone.value("placed").toInt() == 0);
    CHECK(clipsOnTimeline() == 0);
    CHECK(r.controller.session().undoCount() == before + 1);
    CHECK(r.controller.session().undoLabel() == "Import Folder");
}

namespace {

/// A left click on `item`'s centre through the window system (as the platform delivers it: handlers see a real tap).
void clickCentre(QQuickWindow* window, QQuickItem* item, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    const QPointF at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    const auto send = [&](QEvent::Type type, Qt::MouseButtons buttons) {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
            window, at, window->mapToGlobal(at), buttons, button, type, modifiers);
    };
    send(QEvent::MouseMove, Qt::NoButton);
    send(QEvent::MouseButtonPress, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
}

/// Runs `code` in Main.qml's context (its ids: docking, window, ...) and returns its value.
QVariant evaluateInWindow(const Shell& shell, const QString& code) {
    QQmlExpression expression(qmlContext(shell.window.get()), shell.window.get(), code);
    const QVariant result = expression.evaluate();
    INFO(code.toStdString() << ": " << expression.error().toString().toStdString());
    REQUIRE_FALSE(expression.hasError());
    return result;
}

/// Maximize Frame on the Media Browser's panel group (or back): its header has room for its tools beside the tabs.
void toggleMaximizeMediaBrowser(const Shell& shell) {
    evaluateInWindow(shell, QStringLiteral("docking.toggleMaximize(docking.groupOfPanel('mediaBrowser').group)"));
    spin(100);
}

/// Saves `item`'s part of `image` as test-output/ui/`name`.
void saveItemShot(const QImage& image, const QQuickItem* item, const char* name) {
    const QRectF rect = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    const double dpr = image.devicePixelRatio();
    REQUIRE(image.copy(QRect(static_cast<int>(rect.x() * dpr), static_cast<int>(rect.y() * dpr),
                             static_cast<int>(rect.width() * dpr), static_cast<int>(rect.height() * dpr)))
                .save(QDir(outputDir()).filePath(QString::fromLatin1(name))));
}

} // namespace

TEST_CASE("Library: File menu Import Folder and the Media Browser import folders through the window",
          "[app][library][qml][screenshots]") {
    if (!haveLibraryTestMedia())
        SKIP("test media missing");
    resetSettings();
    clearMessages();
    QSettings().remove(QStringLiteral("import"));
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // Footage/Shoot: a.mp4 in Day1, b.wav in Day1/Audio, notes left out; Footage/B-roll: c.wav (the Media Browser's).
    const QString footage = dir.filePath(QStringLiteral("Footage"));
    copyMedia("h264_1080p60_aac.mp4", footage + QStringLiteral("/Shoot/Day1/a.mp4"));
    copyMedia("sine_48k.wav", footage + QStringLiteral("/Shoot/Day1/Audio/b.wav"));
    copyMedia("sine_48k.wav", footage + QStringLiteral("/B-roll/c.wav"));
    // Fix round 1 (D): a loose clip and another folder, chosen together in the Media Browser.
    copyMedia("h264_1080p60_aac.mp4", footage + QStringLiteral("/d.mp4"));
    copyMedia("sine_48k.wav", footage + QStringLiteral("/Interviews/e.wav"));
    {
        QFile notes(footage + QStringLiteral("/Shoot/notes.txt"));
        REQUIRE(notes.open(QIODevice::WriteOnly));
        notes.write("not media");
    }

    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    QQuickWindow* w = shell.window.get();
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* lifecycle = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* library = shell.engine.singletonInstance<LibraryBridge*>("Wabcut", "Library");
    REQUIRE(controller);
    REQUIRE(lifecycle);
    REQUIRE(library);
    library->setPersistent(false);
    lifecycle->newProject(); // over the sample project an import starts a new project (its bins at the root)
    REQUIRE_FALSE(lifecycle->demo());
    const QString target = library->newBin(QStringLiteral("Selects"));
    REQUIRE_FALSE(target.isEmpty());
    library->openBin(target);
    REQUIRE(library->currentBin() == target);
    const auto binNamed = [&](const char* name) -> const Bin* {
        for (const Bin& b : controller->snapshot()->bins) {
            if (b.name == name)
                return &b;
        }
        return nullptr;
    };
    const BinId targetId = binNamed("Selects")->id;

    // File > Import from Media Browser, Import…, Import Folder… (Premiere's order); the folder picker opens at the
    // folder imports come from.
    QObject* fileMenu = shell.object("fileMenu");
    REQUIRE(fileMenu);
    QObject* fromBrowser = fileMenu->findChild<QObject*>(QStringLiteral("fileMenuImportFromMediaBrowser"));
    QObject* importRow = fileMenu->findChild<QObject*>(QStringLiteral("fileMenuImport"));
    QObject* folderRow = fileMenu->findChild<QObject*>(QStringLiteral("fileMenuImportFolder"));
    REQUIRE(fromBrowser);
    REQUIRE(importRow);
    REQUIRE(folderRow);
    CHECK(fromBrowser->property("command").toString() == QStringLiteral("file.importFromMediaBrowser"));
    CHECK(importRow->property("command").toString() == QStringLiteral("file.import"));
    CHECK(folderRow->property("command").toString() == QStringLiteral("file.importFolder"));
    // The Library's ··· menu has Import folder… too (it asks the window once the menu has closed).
    QObject* toolsFolderRow = shell.window->findChild<QObject*>(QStringLiteral("libraryImportFolder"));
    REQUIRE(toolsFolderRow);
    CHECK(toolsFolderRow->property("text").toString() == QStringLiteral("Import folder…"));
    QObject* folderDialog = shell.object("importFolderDialog");
    REQUIRE(folderDialog);
    CHECK(folderDialog->property("currentFolder").toUrl() == lifecycle->importFolder());

    // The chosen folder (as the picker hands it over): a bin in the open bin, subfolders nested, one undo step, a toast.
    const std::size_t before = controller->session().undoCount();
    QVariant result;
    REQUIRE(QMetaObject::invokeMethod(w, "importUrls", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QVariant(QVariantList{QUrl::fromLocalFile(footage + QStringLiteral("/Shoot"))}))));
    const QVariantMap summary = result.toMap();
    INFO(summary.value(QStringLiteral("folderSummary")).toString().toStdString());
    CHECK(summary.value(QStringLiteral("added")).toInt() == 2);
    CHECK(summary.value(QStringLiteral("bins")).toInt() == 3);
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(controller->session().undoLabel() == "Import Folder");
    const Bin* shoot = binNamed("Shoot");
    const Bin* day1 = binNamed("Day1");
    const Bin* audio = binNamed("Audio");
    REQUIRE(shoot);
    REQUIRE(day1);
    REQUIRE(audio);
    CHECK(shoot->parent == targetId);
    CHECK(day1->parent == shoot->id);
    CHECK(audio->parent == day1->id);
    CHECK(library->currentBin() == target); // the open bin stays open and shows the new bin
    QObject* toast = shell.object("toast");
    REQUIRE(toast);
    CHECK(toast->property("text").toString() ==
          QStringLiteral("Imported 2 files from Shoot into 3 bins. 1 file was left out (not media)."));
    spin(50);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("library-import-folder.png"))));
    }

    // The Media Browser: a folder row is chosen by a click, Import reads "Import 1"; Ctrl+Alt+I (its command) imports
    // it as a bin in the open bin.
    REQUIRE(QMetaObject::invokeMethod(w, "runCommand", Q_ARG(QVariant, QStringLiteral("window.mediaBrowser"))));
    QObject* browserObject = nullptr;
    REQUIRE(waitFor([&] {
        browserObject = shell.window->findChild<QObject*>(QStringLiteral("mediaBrowserPanel"));
        return browserObject != nullptr;
    }, 5000));
    auto* browser = qobject_cast<QQuickItem*>(browserObject);
    REQUIRE(browser);
    bool openedOk = false;
    REQUIRE(QMetaObject::invokeMethod(browser, "openFolder", Q_RETURN_ARG(bool, openedOk), Q_ARG(QVariant, footage)));
    CHECK(openedOk);
    const QVariantList entries = browser->property("entries").toList();
    int brollRow = -1;
    for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("B-roll"))
            brollRow = i;
    }
    REQUIRE(brollRow >= 0);
    CHECK(entries[brollRow].toMap().value(QStringLiteral("folder")).toBool());
    CHECK_FALSE(browser->property("canImport").toBool()); // the current row is a folder, nothing chosen
    spin(50);
    QQuickItem* row = nullptr;
    REQUIRE(waitFor([&] {
        row = findNamed(browser, QStringLiteral("mediaBrowserRow_%1").arg(brollRow));
        return row != nullptr && row->width() > 0;
    }, 5000));
    clickCentre(w, row);
    CHECK(browser->property("selectedCount").toInt() == 1);
    CHECK(browser->property("canImport").toBool());
    QQuickItem* importButton = findNamed(browser, QStringLiteral("mediaBrowserImport"));
    REQUIRE(importButton);
    CHECK(importButton->property("text").toString() == QStringLiteral("Import 1"));
    CHECK(importButton->isEnabled());
    // The screenshot with the panel's group maximized (Maximize Frame): its header shows "Import 1" beside the tabs.
    toggleMaximizeMediaBrowser(shell);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        saveItemShot(image, browser, "media-browser-import-folder.png");
    }
    toggleMaximizeMediaBrowser(shell);
    const std::size_t beforeBrowser = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(w, "runCommand", Q_ARG(QVariant, QStringLiteral("file.importFromMediaBrowser"))));
    REQUIRE(waitFor([&] { return binNamed("B-roll") != nullptr; }, 5000));
    CHECK(binNamed("B-roll")->parent == targetId);
    CHECK(controller->session().undoCount() == beforeBrowser + 1);
    CHECK(controller->session().undoLabel() == "Import Folder");
    // Nothing chosen any more (the folder listed again): the command shows the Media Browser, nothing is imported.
    REQUIRE(QMetaObject::invokeMethod(browser, "openFolder", Q_RETURN_ARG(bool, openedOk), Q_ARG(QVariant, footage)));
    CHECK_FALSE(browser->property("canImport").toBool());
    const std::size_t media = controller->snapshot()->media.size();
    REQUIRE(QMetaObject::invokeMethod(w, "runCommand", Q_ARG(QVariant, QStringLiteral("file.importFromMediaBrowser"))));
    spin(50);
    CHECK(controller->snapshot()->media.size() == media);
    CHECK(browser->isVisible());

    // Fix round 1 (D): a clip and a folder chosen together (Ctrl+click) are ONE undo step "Import Folder": the clip as
    // the Media Browser imports clips, the folder as a bin.
    QString loosePath;
    QString interviewsPath;
    for (const QVariant& v : browser->property("entries").toList()) {
        const QVariantMap e = v.toMap();
        if (e.value(QStringLiteral("path")).toString().endsWith(QStringLiteral("d.mp4")))
            loosePath = e.value(QStringLiteral("path")).toString();
        if (e.value(QStringLiteral("name")).toString() == QStringLiteral("Interviews"))
            interviewsPath = e.value(QStringLiteral("path")).toString();
    }
    REQUIRE_FALSE(loosePath.isEmpty());
    REQUIRE_FALSE(interviewsPath.isEmpty());
    REQUIRE(QMetaObject::invokeMethod(browser, "toggle", Q_ARG(QVariant, loosePath), Q_ARG(QVariant, true)));
    REQUIRE(QMetaObject::invokeMethod(browser, "toggle", Q_ARG(QVariant, interviewsPath), Q_ARG(QVariant, true)));
    CHECK(browser->property("selectedCount").toInt() == 2);
    const std::size_t mediaBefore = controller->snapshot()->media.size();
    const std::size_t beforeMixed = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(browser, "importSelected"));
    REQUIRE(waitFor([&] { return binNamed("Interviews") != nullptr && controller->snapshot()->media.size() == mediaBefore + 2; },
                    5000)); // d.mp4 and Interviews/e.wav
    CHECK(binNamed("Interviews")->parent == targetId);
    CHECK(controller->session().undoCount() == beforeMixed + 1);
    CHECK(controller->session().undoLabel() == "Import Folder");
    REQUIRE(controller->undo());
    CHECK(binNamed("Interviews") == nullptr);
    CHECK(controller->snapshot()->media.size() == mediaBefore);
    CHECK(binNamed("B-roll") != nullptr); // the earlier import stays

    QSettings().remove(QStringLiteral("import"));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
    resetSettings();
}

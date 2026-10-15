// M5m: version history and review notes (C.9) in the app, offscreen; no window is shown.
//   * History: the folder opened next to the saved project (none for an unsaved one), checkpoints (save, auto only
//     after a change, before an agent, a named version), the list newest first, naming, restore (one undo step, the
//     project as it was kept as a version), restore a sequence as a new one, export a version as a project, compare
//     (lines, clip rows, the timeline's highlights of the changed ranges, dropped by the next edit).
//   * Review: notes at the playhead and over In-Out with a mark, the author, replies, resolve, delete, the filters, the
//     note at the playhead and its mark, notes to markers, seeking to a note, packages out (index.html, notes.json,
//     JPEG thumbnails from the export renderer) and back in (added, updated, conflicts).
//   * The mark overlay: a click marks a spot, a drag a rectangle (in frame fractions).
// Screenshots: review-notes.png (the Review panel and a mark over the Program Monitor), version-history.png (the
// versions and a comparison).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "HistoryBridge.h"
#include "ProjectController.h"
#include "ReviewBridge.h"
#include "ReviewFrames.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/ReviewPackage.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMouseEvent>
#include <QQmlComponent>
#include <QTemporaryDir>
#include <QUrl>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

constexpr std::string_view kPicture = R"(
    V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
)";

std::filesystem::path pathOf(const QTemporaryDir& dir, const char* name) {
    return std::filesystem::path(QDir(dir.path()).filePath(QString::fromLatin1(name)).toStdU16String());
}

QUrl urlOf(const std::filesystem::path& p) { return QUrl::fromLocalFile(QString::fromStdU16String(p.u16string())); }

/// Lifts the clip at `ref` (one undo step).
void liftClip(ProjectController& controller, const Timeline& tl, std::string_view ref) {
    controller.setSelection({tl.clip(ref)});
    controller.deleteSelection();
}

std::size_t clipCount(const ProjectController& controller) {
    std::size_t n = 0;
    const SequencePtr seq = controller.currentSequence();
    for (const TrackPtr& t : seq->videoTracks)
        n += t->clips.size();
    for (const TrackPtr& t : seq->audioTracks)
        n += t->clips.size();
    return n;
}

QVariantMap versionAt(const HistoryBridge& h, int index) { return h.versions().value(index).toMap(); }

/// The note with `id` among the notes shown (empty when it is not shown).
QVariantMap noteById(const ReviewBridge& review, int id) {
    for (const QVariant& v : review.notes()) {
        if (v.toMap().value(QStringLiteral("id")).toInt() == id)
            return v.toMap();
    }
    return {};
}

/// The id of the shown note with `text` (0 when none).
int noteIdWithText(const ReviewBridge& review, const QString& text) {
    for (const QVariant& v : review.notes()) {
        if (v.toMap().value(QStringLiteral("text")).toString() == text)
            return v.toMap().value(QStringLiteral("id")).toInt();
    }
    return 0;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

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

void sendMouse(QQuickWindow* window, QEvent::Type type, QPointF at, Qt::MouseButtons buttons) {
    QMouseEvent event(type, at, at, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, {});
    QCoreApplication::sendEvent(window, &event);
}

/// A component of the Wabcut module made in the shell's engine and put over the window (the panels are placed by
/// Main.qml in the app; the tests place them themselves).
std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QRectF at) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/test/inline.qml")));
    QObject* object = component.create();
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    auto* item = qobject_cast<QQuickItem*>(object);
    REQUIRE(item);
    item->setParentItem(shell.window->contentItem());
    item->setPosition(at.topLeft());
    item->setSize(at.size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

} // namespace

// ---- Version history ----------------------------------------------------------------------------------------------

TEST_CASE("History: no history for an unsaved project; the folder opens next to the saved file", "[app][history]") {
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    HistoryBridge history;
    history.setController(&controller);
    CHECK_FALSE(history.available());
    CHECK_FALSE(history.reason().isEmpty());
    CHECK(history.checkpoint(QStringLiteral("manual"), QStringLiteral("x")) == history.reason());
    CHECK(history.versions().isEmpty());

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    controller.setProjectFile(pathOf(dir, "Film.wabcut"));
    history.reopen();
    REQUIRE(history.available());
    CHECK(history.reason().isEmpty());
    CHECK(std::filesystem::exists(ProjectHistory::folderFor(controller.projectFile())));
    CHECK(history.services() != nullptr);
}

TEST_CASE("History: checkpoints (save, auto after a change, agent, named), the list newest first, naming",
          "[app][history]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    controller.setProjectFile(pathOf(dir, "Film.wabcut"));
    HistoryBridge history;
    history.setController(&controller);
    REQUIRE(history.available());

    const auto undo = controller.session().undoCount();
    history.onSaved();
    REQUIRE(history.versions().size() == 1);
    CHECK(versionAt(history, 0).value(QStringLiteral("kind")).toString() == QStringLiteral("save"));
    CHECK(versionAt(history, 0).value(QStringLiteral("latest")).toBool());
    CHECK(controller.session().undoCount() == undo); // a checkpoint is never an undo step

    // Auto: nothing changed, nothing added; after an edit, a version.
    history.autoCheckpoint();
    CHECK(history.versions().size() == 1);
    liftClip(controller, tl, "V1@10");
    history.autoCheckpoint();
    REQUIRE(history.versions().size() == 2);
    CHECK(versionAt(history, 0).value(QStringLiteral("kind")).toString() == QStringLiteral("auto"));
    CHECK(versionAt(history, 0).value(QStringLiteral("kindText")).toString() == QStringLiteral("Autosave"));
    history.autoCheckpoint();
    CHECK(history.versions().size() == 2);

    // Before an agent; a named version (newest first, its title the name).
    liftClip(controller, tl, "V1@20");
    history.checkpointBeforeAgent();
    CHECK(versionAt(history, 0).value(QStringLiteral("kind")).toString() == QStringLiteral("agent"));
    CHECK_FALSE(history.saveVersion(QStringLiteral("  ")).isEmpty());
    REQUIRE(history.saveVersion(QStringLiteral("Client cut v2")).isEmpty());
    REQUIRE(history.versions().size() == 4);
    QVariantMap named = versionAt(history, 0);
    CHECK(named.value(QStringLiteral("title")).toString() == QStringLiteral("Client cut v2"));
    CHECK(named.value(QStringLiteral("named")).toBool());
    CHECK(named.value(QStringLiteral("kind")).toString() == QStringLiteral("manual"));
    CHECK_FALSE(named.value(QStringLiteral("createdText")).toString().isEmpty());
    CHECK_FALSE(named.value(QStringLiteral("sizeText")).toString().isEmpty());
    CHECK_FALSE(history.diskText().isEmpty());
    CHECK(versionAt(history, 3).value(QStringLiteral("kind")).toString() == QStringLiteral("save")); // oldest last

    // Rename and unname.
    const int firstId = versionAt(history, 3).value(QStringLiteral("id")).toInt();
    REQUIRE(history.name(firstId, QStringLiteral("Assembly")).isEmpty());
    CHECK(versionAt(history, 3).value(QStringLiteral("title")).toString() == QStringLiteral("Assembly"));
    REQUIRE(history.name(firstId, QString()).isEmpty());
    CHECK_FALSE(versionAt(history, 3).value(QStringLiteral("named")).toBool());

    // The timer: an auto checkpoint after a change.
    history.setAutoIntervalSeconds(1);
    liftClip(controller, tl, "A1@0");
    const auto before = history.versions().size();
    CHECK(waitFor([&] { return history.versions().size() == before + 1; }, 6000));
}

TEST_CASE("History: restore (one undo step), restore a sequence as a new one, export a version", "[app][history]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Timeline tl(kPicture);
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    controller.setProjectFile(pathOf(dir, "Film.wabcut"));
    HistoryBridge history;
    history.setController(&controller);
    REQUIRE(history.saveVersion(QStringLiteral("All three")).isEmpty());
    const int allThree = versionAt(history, 0).value(QStringLiteral("id")).toInt();
    const std::size_t clips = clipCount(controller);
    liftClip(controller, tl, "V1@10");
    REQUIRE(clipCount(controller) < clips);

    // The sequences a version had.
    const QVariantList seqs = history.sequencesOf(allThree);
    REQUIRE(seqs.size() == 1);
    CHECK(seqs[0].toMap().value(QStringLiteral("key")).toString() == sequenceKey(tl.seq));

    // Restore: one undo step; the project as it was is kept as a "restore" version first.
    const auto undo = controller.session().undoCount();
    REQUIRE(history.restore(allThree).isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK(clipCount(controller) == clips);
    CHECK(versionAt(history, 0).value(QStringLiteral("kind")).toString() == QStringLiteral("restore"));
    controller.undo();
    CHECK(clipCount(controller) < clips);

    // A sequence of the version as a new one.
    const auto sequences = controller.snapshot()->sequences.size();
    REQUIRE(history.restoreSequence(allThree, sequenceKey(tl.seq), QStringLiteral("Old cut")).isEmpty());
    REQUIRE(controller.snapshot()->sequences.size() == sequences + 1);
    CHECK(controller.snapshot()->sequences.back()->name == "Old cut");
    CHECK_FALSE(history.restoreSequence(allThree, QStringLiteral("junk")).isEmpty());

    // Export a version as its own project file.
    const std::filesystem::path exported = pathOf(dir, "Film (all three).wabcut");
    REQUIRE(history.exportVersion(allThree, urlOf(exported)).isEmpty());
    CHECK(std::filesystem::exists(exported));
    CHECK_FALSE(history.exportVersion(allThree, urlOf(exported)).isEmpty()); // exists: refused without overwrite
    CHECK(history.exportVersion(allThree, urlOf(exported), true).isEmpty());
    CHECK_FALSE(history.restore(9999).isEmpty());
}

TEST_CASE("History: compare lists the changes and highlights the changed ranges on the timeline", "[app][history][timeline]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Rig rig(kPicture);
    rig.view->setParent(rig.harness.window().contentItem()); // found like the app's views (QObject children of the window)
    rig.controller.setProjectFile(pathOf(dir, "Film.wabcut"));
    HistoryBridge history;
    history.setController(&rig.controller);
    REQUIRE(history.saveVersion(QStringLiteral("Before")).isEmpty());
    const int before = versionAt(history, 0).value(QStringLiteral("id")).toInt();
    liftClip(rig.controller, rig.tl, "V1@10"); // lifts B on V1 and A1 (linked)

    REQUIRE(history.compare(before, QStringLiteral("current")).isEmpty());
    QVariantMap c = history.comparison();
    CHECK(c.value(QStringLiteral("fromText")).toString() == QStringLiteral("“Before”"));
    CHECK(c.value(QStringLiteral("toText")).toString() == QStringLiteral("Now"));
    CHECK_FALSE(c.value(QStringLiteral("empty")).toBool());
    CHECK_FALSE(c.value(QStringLiteral("summary")).toStringList().isEmpty());
    const QVariantList sequences = c.value(QStringLiteral("sequences")).toList();
    REQUIRE(sequences.size() == 1);
    const QVariantMap seq = sequences[0].toMap();
    CHECK(seq.value(QStringLiteral("current")).toBool());
    CHECK(seq.value(QStringLiteral("kind")).toString() == QStringLiteral("changed"));
    const QVariantList clips = seq.value(QStringLiteral("clips")).toList();
    REQUIRE_FALSE(clips.isEmpty());
    CHECK(clips[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("removed"));
    CHECK_FALSE(clips[0].toMap().value(QStringLiteral("beforeText")).toString().isEmpty());

    // The timeline: the removed clip's range struck through.
    const TimelineView::ExternalPreview* preview = rig.view->externalPreview();
    REQUIRE(preview != nullptr);
    REQUIRE_FALSE(preview->removed.empty());
    CHECK(preview->removed.front().track == rig.tl.track("V1"));
    CHECK(preview->removed.front().start == seconds(0.4)); // frame 10 at 25 fps
    rig.harness.frame();

    // Comparing a version with itself: no changes. Another edit makes a comparison stale and drops the highlights.
    REQUIRE(history.compare(before, before).isEmpty());
    CHECK(history.comparison().value(QStringLiteral("empty")).toBool());
    REQUIRE(history.compare(before).isEmpty());
    REQUIRE(rig.view->externalPreview() != nullptr);
    liftClip(rig.controller, rig.tl, "V1@20");
    CHECK(rig.view->externalPreview() == nullptr);
    CHECK(history.comparison().value(QStringLiteral("stale")).toBool());
    history.clearComparison();
    CHECK(history.comparison().isEmpty());
    CHECK_FALSE(history.compare(4242).isEmpty());
}

// ---- Review notes -------------------------------------------------------------------------------------------------

TEST_CASE("Review: notes with marks, author, replies, resolve, filters, delete, the note at the playhead", "[app][review]") {
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    ReviewBridge review;
    review.setController(&controller);
    CHECK(review.notes().isEmpty());
    CHECK(review.authorName() == QStringLiteral("Editor"));
    CHECK_FALSE(review.add(QStringLiteral("  ")).isEmpty());

    // At the playhead, with a rectangle drawn on the frame: one undo step.
    controller.setPlayhead(seconds(0.48));
    review.setDrawing(true);
    review.setPendingMark(0.7, 0.05, 0.2, 0.1);
    CHECK(review.shownMark().value(QStringLiteral("pending")).toBool());
    const auto undo = controller.session().undoCount();
    REQUIRE(review.add(QStringLiteral("The logo is too small")).isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    CHECK_FALSE(review.drawing());
    CHECK(review.pendingMark().isEmpty());
    REQUIRE(review.notes().size() == 1);
    QVariantMap first = review.notes()[0].toMap();
    const int firstId = first.value(QStringLiteral("id")).toInt();
    CHECK(first.value(QStringLiteral("startText")).toString() == QStringLiteral("00:00:00:12"));
    CHECK_FALSE(first.value(QStringLiteral("range")).toBool());
    CHECK(first.value(QStringLiteral("author")).toString() == QStringLiteral("Editor"));
    CHECK(first.value(QStringLiteral("hasMark")).toBool());
    CHECK(first.value(QStringLiteral("mark")).toMap().value(QStringLiteral("x")).toDouble() == Catch::Approx(0.7));
    CHECK(first.value(QStringLiteral("mark")).toMap().value(QStringLiteral("width")).toDouble() == Catch::Approx(0.2));
    // It is the note at the playhead, and the monitor shows its mark.
    CHECK(review.currentNote() == firstId);
    CHECK(review.shownMark().value(QStringLiteral("y")).toDouble() == Catch::Approx(0.05));
    CHECK_FALSE(review.shownMark().value(QStringLiteral("pending")).toBool());
    controller.setPlayhead(seconds(2));
    CHECK(review.currentNote() == 0);
    CHECK(review.shownMark().isEmpty());

    // Over In-Out, by another author; a spot mark clamped into the frame.
    review.setAuthor(QStringLiteral("Ana"));
    controller.setPlayhead(seconds(0.4));
    controller.markInAtPlayhead();
    controller.setPlayhead(seconds(0.8));
    controller.markOutAtPlayhead();
    review.setPendingMark(1.4, -0.2);
    CHECK(review.pendingMark().value(QStringLiteral("x")).toDouble() == Catch::Approx(1.0));
    CHECK(review.pendingMark().value(QStringLiteral("y")).toDouble() == Catch::Approx(0.0));
    review.setPendingMark(0.5, 0.5);
    REQUIRE(review.add(QStringLiteral("Hold this shot longer"), true).isEmpty());
    REQUIRE(review.notes().size() == 2);
    const int secondId = noteIdWithText(review, QStringLiteral("Hold this shot longer"));
    REQUIRE(secondId > 0);
    CHECK(review.notes()[0].toMap().value(QStringLiteral("id")).toInt() == secondId); // in time order: In is earlier
    const QVariantMap second = noteById(review, secondId);
    CHECK(second.value(QStringLiteral("range")).toBool());
    CHECK(second.value(QStringLiteral("endText")).toString() == QStringLiteral("00:00:00:21")); // Out ends its frame
    CHECK(second.value(QStringLiteral("author")).toString() == QStringLiteral("Ana"));

    // A thread, resolve, the filters.
    CHECK_FALSE(review.reply(firstId, QStringLiteral(" ")).isEmpty());
    REQUIRE(review.reply(firstId, QStringLiteral("Bigger in v3")).isEmpty());
    first = noteById(review, firstId);
    CHECK(first.value(QStringLiteral("replyCount")).toInt() == 1);
    CHECK(first.value(QStringLiteral("replies")).toList()[0].toMap().value(QStringLiteral("author")).toString() == QStringLiteral("Ana"));
    REQUIRE(review.resolve(firstId).isEmpty());
    CHECK(noteById(review, firstId).value(QStringLiteral("resolved")).toBool());
    CHECK(review.openCount() == 1);
    CHECK(review.resolvedCount() == 1);
    review.setStatusFilter(QStringLiteral("open"));
    REQUIRE(review.notes().size() == 1);
    CHECK(review.notes()[0].toMap().value(QStringLiteral("id")).toInt() == secondId);
    review.setStatusFilter(QStringLiteral("resolved"));
    REQUIRE(review.notes().size() == 1);
    CHECK(review.notes()[0].toMap().value(QStringLiteral("id")).toInt() == firstId);
    review.setStatusFilter(QStringLiteral("nonsense"));
    CHECK(review.statusFilter() == QStringLiteral("all"));
    review.setQuery(QStringLiteral("v3")); // finds the reply's text
    REQUIRE(review.notes().size() == 1);
    CHECK(review.notes()[0].toMap().value(QStringLiteral("id")).toInt() == firstId);
    review.setQuery({});
    REQUIRE(review.resolve(firstId, false).isEmpty());
    CHECK(review.openCount() == 2);

    // Seeking selects the note and shows its mark.
    REQUIRE(review.seekToNote(firstId));
    CHECK(controller.playhead() == seconds(0.48));
    CHECK(review.selectedNote() == firstId);
    CHECK(review.shownMark().value(QStringLiteral("x")).toDouble() == Catch::Approx(0.7));
    CHECK_FALSE(review.seekToNote(9999));

    // Delete: one undo step.
    REQUIRE(review.remove(firstId).isEmpty());
    CHECK(review.notes().size() == 1);
    CHECK(review.selectedNote() == 0);
    controller.undo();
    CHECK(review.notes().size() == 2);
    CHECK_FALSE(review.remove(9999).isEmpty());
}

TEST_CASE("Review: notes to markers, all sequences", "[app][review]") {
    Timeline tl(kPicture);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    ReviewBridge review;
    review.setController(&controller);
    CHECK_FALSE(review.toMarkers().isEmpty()); // nothing to turn into markers
    controller.setPlayhead(seconds(0.2));
    REQUIRE(review.add(QStringLiteral("Cut earlier")).isEmpty());
    controller.setPlayhead(seconds(1.0));
    REQUIRE(review.add(QStringLiteral("Colour match\nwith the wide")).isEmpty());
    const std::size_t markers = controller.currentSequence()->markers.size();
    REQUIRE(review.toMarkers(false).isEmpty());
    CHECK(controller.currentSequence()->markers.size() == markers + 2);
    CHECK(review.notes().size() == 2);
    REQUIRE(review.toMarkers(true).isEmpty()); // and remove the notes
    CHECK(review.notes().isEmpty());

    // A note of another sequence shows with "all sequences".
    controller.setPlayhead(seconds(0.2));
    REQUIRE(review.add(QStringLiteral("Here")).isEmpty());
    edit::SequenceSettings settings;
    settings.name = "Second";
    REQUIRE(controller.applyEditFn("New Sequence", [&](const ProjectState& s) -> edit::EditOutcome {
        auto created = edit::addSequence(s, settings);
        if (!created)
            return std::unexpected(created.error());
        edit::EditResult r;
        r.state = std::move(created->state);
        r.changes = diff(s, r.state);
        return r;
    }));
    controller.setCurrentSequenceIndex(1);
    CHECK(review.notes().isEmpty());
    review.setAllSequences(true);
    REQUIRE(review.notes().size() == 1);
    CHECK(review.notes()[0].toMap().value(QStringLiteral("sequenceName")).toString() == QStringLiteral("Fixture"));
    // Seeking to it switches to its sequence.
    REQUIRE(review.seekToNote(review.notes()[0].toMap().value(QStringLiteral("id")).toInt()));
    CHECK(controller.currentSequenceId() == tl.seq);
}

TEST_CASE("Review packages: export with rendered JPEG thumbnails, then import added, updated and conflicts",
          "[app][review][gpu]") {
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A title on V1 so the export renderer draws a picture (the fixture's media have no files).
    Timeline tl(kPicture);
    ProjectState state = tl.state;
    auto titled = edit::addTitle(state, tl.seq, tl.track("V1"), Ticks{}, seconds(1.2), "Review me",
                                 {{"size", ParamValue(220.0)}});
    REQUIRE(titled);
    state = std::move(titled->state);
    ProjectController controller;
    controller.resetProject(state, tl.seq);
    ReviewBridge review;
    review.setController(&controller);
    review.setSynchronous(true);
    controller.setPlayhead(seconds(0.2));
    review.setPendingMark(0.25, 0.25, 0.5, 0.5);
    REQUIRE(review.add(QStringLiteral("Title too big")).isEmpty());
    controller.setPlayhead(seconds(0.8));
    REQUIRE(review.add(QStringLiteral("Typo in the second line")).isEmpty());
    REQUIRE(review.notes().size() == 2);
    const int titleNote = review.notes()[0].toMap().value(QStringLiteral("id")).toInt();
    const int typoNote = review.notes()[1].toMap().value(QStringLiteral("id")).toInt();

    // The renderer's thumbnail on its own: JPEG at most 640 wide at the sequence's aspect.
    const ReviewFrames frames;
    auto thumb = frames.frame(*controller.snapshot(), tl.seq, seconds(0.2), 640);
    INFO((thumb ? std::string() : thumb.error().message));
    REQUIRE(thumb);
    CHECK(thumb->mime == "image/jpeg");
    REQUIRE(thumb->bytes.size() > 100);
    CHECK(static_cast<unsigned char>(thumb->bytes[0]) == 0xFF);
    CHECK(static_cast<unsigned char>(thumb->bytes[1]) == 0xD8);
    const QImage decoded = QImage::fromData(QByteArray::fromStdString(thumb->bytes), "JPEG");
    CHECK(decoded.width() == 640);
    CHECK(decoded.height() == 360);
    CHECK_FALSE(frames.frame(*controller.snapshot(), SequenceId{9999}, Ticks{}, 640));

    // Export: the page, the notes and the thumbnails.
    const std::filesystem::path package = pathOf(dir, "Client.wabreview");
    REQUIRE(review.exportPackage(urlOf(package), QStringLiteral("Client cut v2")).isEmpty());
    QVariantMap result = review.packageResult();
    INFO(result.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(result.value(QStringLiteral("ok")).toBool());
    CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("export"));
    CHECK(result.value(QStringLiteral("notes")).toInt() == 2);
    CHECK(result.value(QStringLiteral("frames")).toInt() == 2);
    CHECK_FALSE(review.busy());
    REQUIRE(std::filesystem::exists(package / "index.html"));
    REQUIRE(std::filesystem::exists(package / "notes.json"));
    CHECK(readFile(package / "index.html").find("Client cut v2") != std::string::npos);
    int jpegs = 0;
    for (const auto& entry : std::filesystem::directory_iterator(package / "frames")) {
        const std::string bytes = readFile(entry.path());
        if (bytes.size() > 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xD8)
            ++jpegs;
    }
    CHECK(jpegs == 2);

    // The client's answers: a reply and "resolved" on one note, the other note's text changed (a conflict: the
    // project's is kept), and a new note.
    Json doc = Json::parse(readFile(package / "notes.json"));
    for (Json& n : doc["notes"]) {
        if (n["id"] == titleNote) {
            n["replies"].push_back(Json{{"author", "Client"}, {"text", "Fixed, thanks"}, {"createdAt", "2026-10-12T10:00:00Z"}});
            n["status"] = "resolved";
        } else if (n["id"] == typoNote) {
            n["text"] = "Typo in the THIRD line";
        }
    }
    Json added = doc["notes"][0];
    added["id"] = 0;
    added["start"] = seconds(1.0).value;
    added["text"] = "Music too loud here";
    added["author"] = "Client";
    added["mark"] = nullptr;
    added["replies"] = Json::array();
    added["status"] = "open";
    doc["notes"].push_back(added);
    {
        std::ofstream out(package / "notes.json", std::ios::binary | std::ios::trunc);
        out << doc.dump(2);
    }
    const auto undo = controller.session().undoCount();
    REQUIRE(review.importPackage(urlOf(package)).isEmpty());
    CHECK(controller.session().undoCount() == undo + 1);
    result = review.packageResult();
    CHECK(result.value(QStringLiteral("kind")).toString() == QStringLiteral("import"));
    CHECK(result.value(QStringLiteral("added")).toInt() == 1);
    CHECK(result.value(QStringLiteral("updated")).toInt() == 1);
    CHECK(result.value(QStringLiteral("conflicts")).toStringList().size() == 1);
    REQUIRE(review.notes().size() == 3);
    for (const QVariant& v : review.notes()) {
        const QVariantMap n = v.toMap();
        if (n.value(QStringLiteral("id")).toInt() == titleNote) {
            CHECK(n.value(QStringLiteral("resolved")).toBool());
            CHECK(n.value(QStringLiteral("replyCount")).toInt() == 1);
        }
        if (n.value(QStringLiteral("id")).toInt() == typoNote)
            CHECK(n.value(QStringLiteral("text")).toString() == QStringLiteral("Typo in the second line"));
    }
    CHECK_FALSE(review.importPackage(urlOf(pathOf(dir, "missing.wabreview"))).isEmpty());
    CHECK_FALSE(review.packageResult().value(QStringLiteral("ok")).toBool());
}

// ---- The monitor overlay and the screenshots ----------------------------------------------------------------------

TEST_CASE("Review mark overlay: a click marks a spot, a drag a rectangle, in frame fractions", "[app][review][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    auto* review = shell.engine.singletonInstance<ReviewBridge*>("Wabcut", "Review");
    REQUIRE(review);
    REQUIRE_FALSE(shell.grab().isNull());
    const auto overlay = makeItem(shell, "import Wabcut\nReviewMarkOverlay {}\n", QRectF(200, 100, 640, 360));
    QQuickWindow* w = shell.window.get();

    // Not drawing: the overlay takes no clicks.
    sendMouse(w, QEvent::MouseButtonPress, QPointF(300, 200), Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, QPointF(300, 200), Qt::NoButton);
    CHECK(review->pendingMark().isEmpty());

    review->setDrawing(true);
    // A click: a spot.
    sendMouse(w, QEvent::MouseButtonPress, QPointF(200 + 320, 100 + 90), Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, QPointF(200 + 320, 100 + 90), Qt::NoButton);
    QVariantMap mark = review->pendingMark();
    REQUIRE_FALSE(mark.isEmpty());
    CHECK(mark.value(QStringLiteral("x")).toDouble() == Catch::Approx(0.5).margin(0.005));
    CHECK(mark.value(QStringLiteral("y")).toDouble() == Catch::Approx(0.25).margin(0.005));
    CHECK(mark.value(QStringLiteral("width")).toDouble() == 0.0);
    // A drag: a rectangle (from bottom right to top left works too).
    sendMouse(w, QEvent::MouseButtonPress, QPointF(200 + 480, 100 + 270), Qt::LeftButton);
    sendMouse(w, QEvent::MouseMove, QPointF(200 + 400, 100 + 200), Qt::LeftButton);
    sendMouse(w, QEvent::MouseMove, QPointF(200 + 160, 100 + 90), Qt::LeftButton);
    sendMouse(w, QEvent::MouseButtonRelease, QPointF(200 + 160, 100 + 90), Qt::NoButton);
    mark = review->pendingMark();
    CHECK(mark.value(QStringLiteral("x")).toDouble() == Catch::Approx(0.25).margin(0.005));
    CHECK(mark.value(QStringLiteral("y")).toDouble() == Catch::Approx(0.25).margin(0.005));
    CHECK(mark.value(QStringLiteral("width")).toDouble() == Catch::Approx(0.5).margin(0.005));
    CHECK(mark.value(QStringLiteral("height")).toDouble() == Catch::Approx(0.5).margin(0.005));
    // The overlay draws it.
    QQuickItem* rect = findNamed(overlay.get(), QStringLiteral("reviewMarkRect"));
    REQUIRE(rect);
    CHECK(rect->isVisible());
    CHECK(rect->width() == Catch::Approx(320).margin(1));
    review->setDrawing(false);
    review->clearPendingMark();
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: the Review panel's notes and a mark over the Program Monitor", "[app][review][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* review = shell.engine.singletonInstance<ReviewBridge*>("Wabcut", "Review");
    REQUIRE(controller);
    REQUIRE(review);
    Timeline tl(kPicture);
    controller->resetProject(tl.state, tl.seq);
    shell.preferences->setProperty("reviewAuthor", QStringLiteral("Mali"));
    REQUIRE_FALSE(shell.grab().isNull());

    // A few notes: a spot, a rectangle with a thread, a resolved one over a range.
    controller->setPlayhead(seconds(0.2));
    review->setPendingMark(0.62, 0.3);
    REQUIRE(review->add(QStringLiteral("Logo clips the safe area on the right")).isEmpty());
    controller->setPlayhead(seconds(0.48));
    review->setPendingMark(0.18, 0.55, 0.34, 0.3);
    REQUIRE(review->add(QStringLiteral("Can we grade this shot warmer? It cuts against the wide.")).isEmpty());
    const int graded = review->notes()[1].toMap().value(QStringLiteral("id")).toInt();
    review->setAuthor(QStringLiteral("Client"));
    REQUIRE(review->reply(graded, QStringLiteral("Yes please, closer to the opening scene.")).isEmpty());
    controller->setPlayhead(seconds(0.8));
    REQUIRE(review->add(QStringLiteral("Music swell lands late")).isEmpty());
    const int music = review->notes()[2].toMap().value(QStringLiteral("id")).toInt();
    REQUIRE(review->resolve(music).isEmpty());
    REQUIRE(review->seekToNote(graded));

    // The panel over the browser slot (Main.qml places it there in the app), the overlay over the program picture.
    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(library);
    const QRectF libraryRect = library->mapRectToScene(QRectF(0, 0, library->width(), library->height()));
    const auto panel = makeItem(shell, "import Wabcut\nReviewPanel {}\n", libraryRect);
    QQuickItem* frameArea = findNamed(shell.window->contentItem(), QStringLiteral("programFrameArea"));
    REQUIRE(frameArea);
    const QRectF frameRect = frameArea->mapRectToScene(QRectF(0, 0, frameArea->width(), frameArea->height()));
    const auto overlay = makeItem(shell, "import Wabcut\nReviewMarkOverlay {}\n", frameRect);
    spin(200);
    CHECK(review->authorName() == QStringLiteral("Mali")); // the panel binds Preferences.reviewAuthor
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("review-notes.png"))));
    QQuickItem* list = findNamed(panel.get(), QStringLiteral("reviewNotesList"));
    REQUIRE(list);
    CHECK(list->property("count").toInt() == 3);
    QQuickItem* rect = findNamed(overlay.get(), QStringLiteral("reviewMarkRect"));
    REQUIRE(rect);
    CHECK(rect->isVisible());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: the version history with a comparison", "[app][history][qml][screenshot]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* history = shell.engine.singletonInstance<HistoryBridge*>("Wabcut", "History");
    REQUIRE(controller);
    REQUIRE(history);
    Timeline tl(kPicture);
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setProjectFile(pathOf(dir, "Documentary.wabcut"));
    history->reopen();
    REQUIRE(history->available());
    REQUIRE_FALSE(shell.grab().isNull());

    history->onSaved();
    liftClip(*controller, tl, "V1@20");
    history->autoCheckpoint();
    REQUIRE(history->saveVersion(QStringLiteral("Client cut v1")).isEmpty());
    const int v1 = versionAt(*history, 0).value(QStringLiteral("id")).toInt();
    liftClip(*controller, tl, "V1@0");
    history->checkpointBeforeAgent();
    REQUIRE(history->saveVersion(QStringLiteral("Client cut v2")).isEmpty());
    // v1 against now: the clip lifted since.
    REQUIRE(history->compare(v1, QStringLiteral("current")).isEmpty());
    CHECK_FALSE(history->comparison().value(QStringLiteral("empty")).toBool());

    QQuickItem* library = shell.item("libraryPanel");
    REQUIRE(library);
    const QRectF libraryRect = library->mapRectToScene(QRectF(0, 0, library->width(), library->height()));
    const auto panel = makeItem(shell, "import Wabcut\nReviewPanel {}\n", libraryRect);
    REQUIRE(QMetaObject::invokeMethod(panel.get(), "showPage", Q_ARG(QVariant, 1)));
    spin(200);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("version-history.png"))));
    QQuickItem* list = findNamed(panel.get(), QStringLiteral("historyVersionsList"));
    REQUIRE(list);
    CHECK(list->property("count").toInt() == history->versions().size());
    QQuickItem* comparison = findNamed(panel.get(), QStringLiteral("historyComparison"));
    REQUIRE(comparison);
    CHECK(comparison->isVisible());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// 0.5 (P0 round, stream A): caption tracks as lanes on the timeline (Premiere's caption tracks at the top of the Timeline).
//   * one compact row per caption track above the top video track (Show caption tracks), its cues drawn with their
//     first line, a hidden track dimmed;
//   * a click selects a cue (in CaptionsBridge: the Captions page follows) and moves the playhead to it; caption rows
//     never take clip drops, track targeting or marquee selections;
//   * trim and move by dragging (frames, snapping, kept between the neighbours unless overlaps are allowed), the Razor
//     tool, Add Edit on a targeted caption row, Delete: one undo step each;
//   * in the shell: the caption track headers (target chip, name, eye), the display toggle, a double-click that opens
//     the cue's text in the Captions page; screenshots in the dark and light themes.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "CaptionsBridge.h"
#include "CommandJob.h"
#include "ProjectController.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/model/CaptionOps.h>
#include <wabcut/model/CaptionStyle.h>
#include <wabcut/model/FinishingOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QHoverEvent>
#include <QQuickItem>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

std::string run(char letter, int frames) {
    return std::string(1, letter) + std::string(static_cast<std::size_t>(frames - 1), static_cast<char>(std::tolower(letter)));
}

/// V1 and A1 targeted: media A for 23 frames, then B for 77 (an edit at frame 23).
std::string picture() {
    const std::string clips = run('A', 23) + run('B', 77);
    return "V1 tgt |" + clips + "|\nA1 tgt |" + clips + "|\n";
}

struct Cue {
    int from = 0; ///< frames
    int to = 0;
    const char* text = "";
};

/// A caption track with `cues`, through the commands the Captions page runs.
CaptionTrackId addCaptions(ProjectController& c, const std::string& name, const std::string& language,
                           const std::vector<Cue>& cues) {
    const auto made = CommandJob::runNow(&c, nullptr, "captions.addTrack", Json{{"name", name}, {"language", language}});
    INFO(commandErrorText(made).toStdString());
    REQUIRE(made.ok());
    REQUIRE_FALSE(made.createdIds.empty());
    const CaptionTrackId id{made.createdIds.front()};
    for (const Cue& cue : cues) {
        const auto r = CommandJob::runNow(&c, nullptr, "captions.addCue",
                                          Json{{"captionTrack", id.value}, {"start", f(cue.from).value}, {"end", f(cue.to).value}, {"text", cue.text}});
        INFO(commandErrorText(r).toStdString());
        REQUIRE(r.ok());
    }
    return id;
}

/// The caption track as it is now (a copy).
CaptionTrack trackOf(const ProjectController& c, CaptionTrackId id) {
    for (const CaptionTrack& t : c.currentSequence()->captionTracks) {
        if (t.id == id)
            return t;
    }
    FAIL("no caption track " << id.value);
    return {};
}

/// The middle of caption row `row` in the view's coordinates.
double captionY(const TimelineView& v, std::size_t row) {
    REQUIRE(row < v.captionRows().size());
    const auto& r = v.captionRows()[row];
    return r.y + r.height / 2 - v.scrollY();
}

std::pair<Ticks, Ticks> span(const CaptionCue& cue) {
    return {cue.start, cue.end()};
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

QRectF sceneRect(const QQuickItem* item) {
    return item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
}

void click(QObject* button) {
    REQUIRE(button);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
}

void flip(QObject* toggle, bool on) {
    REQUIRE(toggle);
    toggle->setProperty("checked", on);
    REQUIRE(QMetaObject::invokeMethod(toggle, "toggled"));
}

/// A control inside a popover (its body), by objectName.
QObject* inPopover(QObject* popover, const char* name) {
    if (QObject* found = popover->findChild<QObject*>(QLatin1String(name)))
        return found;
    auto* content = popover->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* found = findNamed(content->parentItem() ? content->parentItem() : content, QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

/// The real shell (Main.qml, hidden, 1600 x 960, reduced motion) over a project from a timeline picture.
struct ShellProject {
    Shell shell;
    ProjectController* controller = nullptr;
    CaptionsBridge* captions = nullptr;
    Timeline tl;

    explicit ShellProject(std::string_view picture, bool dark) : tl(picture) {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(dark, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        captions = shell.engine.singletonInstance<CaptionsBridge*>("Wabcut", "Captions");
        REQUIRE(controller);
        REQUIRE(captions);
        tl.linkMatching();
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    QObject* object(const QString& name) const { return shell.window->findChild<QObject*>(name); }
    std::size_t undo() const { return controller->session().undoCount(); }
    void settle() const {
        REQUIRE_FALSE(shell.grab().isNull());
        spin(30);
    }
    /// One frame (polish, layouts, render) of the hidden window.
    void render() const { (void)shell.grab(); }
    void shot(const QRectF& rect, const QString& file) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = rect.intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

QObject* child(QObject* parent, const char* name) {
    QObject* found = parent->findChild<QObject*>(QLatin1String(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

void trigger(QObject* item) {
    REQUIRE(item);
    REQUIRE(QMetaObject::invokeMethod(item, "triggered"));
}

void waitClosed(QObject* popup) {
    REQUIRE(waitFor([&] { return !popup->property("visible").toBool(); }, 5000));
    spin(50);
}

/// A popup's (menu, dialog) rectangle in the scene: its background.
QRectF popupRect(QObject* popup) {
    auto* background = popup->property("background").value<QQuickItem*>();
    REQUIRE(background);
    return sceneRect(background);
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Caption lanes show a row per caption track above the video tracks with cue labels", "[app][timeline][captions]") {
    Rig rig(picture());
    const CaptionTrackId en = addCaptions(rig.controller, "English", "en", {{5, 20, "One"}, {30, 45, "Two words\nhere"}, {60, 80, "Three"}});
    const CaptionTrackId th = addCaptions(rig.controller, "Thai", "th", {{10, 40, "Sawasdee"}});
    rig.harness.frame();
    TimelineView* v = rig.view;
    const TimelineStyle& st = v->parsedStyle();

    REQUIRE(v->captionRows().size() == 2);
    CHECK(v->captionRows()[0].id == en);
    CHECK(v->captionRows()[1].id == th);
    CHECK(v->captionRows()[0].y == 0);
    CHECK(v->captionRows()[1].y == st.captionTrackHeight + st.trackGap);
    CHECK(v->captionRows()[0].height == st.captionTrackHeight);
    // The video tracks start below them (V1 is the top video row here).
    CHECK(v->rows().front().y == 2 * (st.captionTrackHeight + st.trackGap));

    const QVariantList items = v->captionRowItems();
    REQUIRE(items.size() == 2);
    const QVariantMap first = items[0].toMap();
    CHECK(first.value(QStringLiteral("id")).toString() == captionTrackKey(en));
    CHECK(first.value(QStringLiteral("label")).toString() == QStringLiteral("C1"));
    CHECK(first.value(QStringLiteral("name")).toString() == QStringLiteral("English"));
    CHECK(first.value(QStringLiteral("language")).toString() == QStringLiteral("en"));
    CHECK(first.value(QStringLiteral("cues")).toInt() == 3);
    CHECK(first.value(QStringLiteral("enabled")).toBool());
    CHECK_FALSE(first.value(QStringLiteral("targeted")).toBool());
    CHECK(items[1].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("C2"));

    // Each cue is drawn over its time, labelled with its first line.
    CHECK(v->stats().captionCues == 4);
    CHECK(v->stats().dimmedCaptionCues == 0);
    CHECK(v->stats().captionTexts.value(TimelineStats::captionKey(0, 0)) == QStringLiteral("One"));
    CHECK(v->stats().captionTexts.value(TimelineStats::captionKey(0, 1)) == QStringLiteral("Two words"));
    CHECK(v->stats().captionTexts.value(TimelineStats::captionKey(1, 0)) == QStringLiteral("Sawasdee"));
    const QRectF drawn = v->stats().captionRects.value(TimelineStats::captionKey(0, 1));
    CHECK(std::abs(drawn.left() - rig.x(30)) < 1.0);
    CHECK(std::abs(drawn.right() - rig.x(45)) < 2.0);
    CHECK(drawn.top() >= v->captionRows()[0].y - v->scrollY());
    CHECK(drawn.bottom() <= v->captionRows()[0].y + v->captionRows()[0].height - v->scrollY());

    // A hidden track is drawn dimmed.
    REQUIRE(CommandJob::runNow(&rig.controller, nullptr, "captions.setTrack", Json{{"captionTrack", th.value}, {"enabled", false}}).ok());
    rig.harness.frame();
    CHECK(v->stats().dimmedCaptionCues == 1);
    CHECK_FALSE(v->captionRowItems()[1].toMap().value(QStringLiteral("enabled")).toBool());

    // Show caption tracks off: no rows, V1 at the top; on again: back.
    v->setShowCaptionTracks(false);
    rig.harness.frame();
    CHECK(v->captionRows().empty());
    CHECK(v->captionRowItems().isEmpty());
    CHECK(v->rows().front().y == 0);
    CHECK(v->stats().captionCues == 0);
    v->setShowCaptionTracks(true);
    rig.harness.frame();
    CHECK(v->captionRows().size() == 2);
    CHECK(v->stats().captionCues == 4);
}

TEST_CASE("Caption lanes select a cue on click and seek to it and never select or take clips", "[app][timeline][captions]") {
    Rig rig(picture());
    const CaptionTrackId en = addCaptions(rig.controller, "English", "en", {{5, 20, "One"}, {30, 45, "Two words here"}, {60, 80, "Three"}});
    rig.harness.frame();
    TimelineView* v = rig.view;
    const double cy = captionY(*v, 0);
    rig.controller.setSelection({rig.clip("V1@0")});
    rig.controller.setPlayhead(f(90));

    // A click on a cue selects it (the clips stop being selected) and moves the playhead to its start.
    rig.press(QPointF(rig.x(37), cy));
    rig.release(QPointF(rig.x(37), cy));
    CHECK(v->captionSelection().track == en);
    CHECK(v->captionSelection().cue == 1);
    CHECK(v->selectedCaptionItem().value(QStringLiteral("cue")).toInt() == 1);
    CHECK(v->selectedCaptionItem().value(QStringLiteral("track")).toString() == captionTrackKey(en));
    CHECK(rig.controller.selection().empty());
    CHECK(rig.controller.playhead() == f(30));

    // What is under the pointer there.
    const QVariantMap at = v->captionAt(rig.x(37), cy);
    CHECK(at.value(QStringLiteral("track")).toString() == captionTrackKey(en));
    CHECK(at.value(QStringLiteral("cue")).toInt() == 1);
    CHECK(at.value(QStringLiteral("edge")).toString().isEmpty());
    CHECK(v->captionAt(rig.x(44.9), cy).value(QStringLiteral("edge")).toString() == QStringLiteral("tail"));
    CHECK(v->captionAt(rig.x(30.1), cy).value(QStringLiteral("edge")).toString() == QStringLiteral("head"));
    CHECK(v->captionAt(rig.x(52), cy).isEmpty());
    // Hovering a cue's edge shows the trim cursor (and lights the edge), its body the arrow; the razor its own.
    const auto hover = [&](double frame) {
        const QPointF at(rig.x(frame), cy);
        QHoverEvent move(QEvent::HoverMove, at, at, at);
        QCoreApplication::sendEvent(v, &move);
    };
    hover(44.9);
    CHECK(v->cursorKey() == QStringLiteral("trimTail"));
    hover(30.1);
    CHECK(v->cursorKey() == QStringLiteral("trimHead"));
    hover(37);
    CHECK(v->cursorKey().isEmpty());
    v->setTool(QStringLiteral("razor"));
    hover(37);
    CHECK(v->cursorKey() == QStringLiteral("razor"));
    v->setTool(QStringLiteral("select"));

    // A click on a clip selects the clip and deselects the cue.
    rig.press(QPointF(rig.x(10), rig.y("V1")));
    rig.release(QPointF(rig.x(10), rig.y("V1")));
    CHECK_FALSE(v->captionSelection().valid());
    CHECK_FALSE(rig.controller.selection().empty());

    // A drag from an empty part of a caption row draws no marquee and edits nothing; the press deselects.
    const auto undo = rig.undoSteps();
    rig.press(QPointF(rig.x(50), cy));
    CHECK(rig.controller.selection().empty());
    rig.move(QPointF(rig.x(52), cy));
    rig.move(QPointF(rig.x(56), rig.y("A1")));
    CHECK_FALSE(v->marqueeActive());
    rig.release(QPointF(rig.x(56), rig.y("A1")));
    CHECK(rig.controller.selection().empty());
    CHECK(rig.undoSteps() == undo);

    // A caption row is no clip row: no clip, no track and no drop target there.
    CHECK(v->clipAt(rig.x(10), cy).isEmpty());
    CHECK(v->trackAt(cy).isEmpty());
    CHECK(v->dropPoint(rig.x(10), cy).value(QStringLiteral("track")).toString().isEmpty());
    // A marquee over the caption rows and V1 selects V1's clips, never a cue.
    v->selectInRect(QRectF(rig.x(0), cy, rig.x(15) - rig.x(0), rig.y("V1") - cy), 0);
    CHECK_FALSE(rig.controller.selection().empty());
    CHECK_FALSE(v->captionSelection().valid());

    // A double-click on a cue asks to open its text.
    QString askedTrack;
    int askedCue = -1;
    QObject::connect(v, &TimelineView::captionDoubleClicked, [&](const QString& track, int cue) {
        askedTrack = track;
        askedCue = cue;
    });
    const QPointF onThree(rig.x(70), cy);
    rig.press(onThree);
    rig.release(onThree);
    rig.press(onThree);
    rig.send(QEvent::MouseButtonDblClick, onThree, Qt::LeftButton, Qt::LeftButton, {});
    rig.release(onThree);
    CHECK(askedTrack == captionTrackKey(en));
    CHECK(askedCue == 2);
    CHECK(v->captionSelection().cue == 2);

    // A right click selects the cue and asks for its menu; captionInfo says what the menu shows.
    QString menuTrack;
    int menuCue = -1;
    QObject::connect(v, &TimelineView::captionContextMenuRequested, [&](const QString& track, int cue, double, double) {
        menuTrack = track;
        menuCue = cue;
    });
    const QPointF onOne(rig.x(10), cy);
    rig.press(onOne, {}, Qt::RightButton);
    rig.send(QEvent::MouseButtonRelease, onOne, Qt::RightButton, Qt::NoButton, {});
    CHECK(menuTrack == captionTrackKey(en));
    CHECK(menuCue == 0);
    CHECK(v->captionSelection().cue == 0);
    rig.controller.setPlayhead(f(12));
    const QVariantMap info = v->captionInfo(captionTrackKey(en), 0);
    CHECK(info.value(QStringLiteral("label")).toString() == QStringLiteral("C1"));
    CHECK(info.value(QStringLiteral("text")).toString() == QStringLiteral("One"));
    CHECK(info.value(QStringLiteral("start")).toString() == rig.controller.formatTimecode(f(5)));
    CHECK(info.value(QStringLiteral("duration")).toString() == rig.controller.formatTimecode(f(15)));
    CHECK(info.value(QStringLiteral("splittable")).toBool());
    rig.controller.setPlayhead(f(20)); // on its end: nothing to split there
    CHECK_FALSE(v->captionInfo(captionTrackKey(en), 0).value(QStringLiteral("splittable")).toBool());
    CHECK(v->captionInfo(captionTrackKey(en), 9).isEmpty());
}

TEST_CASE("Caption lanes trim and move cues by dragging with snapping and clamping in one undo step each",
          "[app][timeline][captions][edit]") {
    Rig rig(picture(), 4.0); // 4 px a frame: the snap threshold (8 px) is two frames
    const CaptionTrackId en = addCaptions(rig.controller, "English", "en", {{5, 20, "One"}, {30, 45, "Two words here"}, {60, 80, "Three"}});
    rig.harness.frame();
    TimelineView* v = rig.view;
    const double cy = captionY(*v, 0);
    const auto cue = [&](std::size_t i) { return span(trackOf(rig.controller, en).cues.at(i)); };
    const auto before = rig.undoSteps();

    SECTION("a tail trim moves the end by frames and undoes") {
        rig.drag(QPointF(rig.x(19.8), cy), QPointF(rig.x(25.8), cy)); // +6 frames
        CHECK(cue(0) == std::pair(f(5), f(26)));
        CHECK(rig.undoSteps() == before + 1);
        CHECK(v->captionSelection().cue == 0);
        REQUIRE(rig.controller.undo());
        CHECK(cue(0) == std::pair(f(5), f(20)));
    }
    SECTION("a trim snaps to a clip edge and shows the snap line and the new duration") {
        rig.drag(QPointF(rig.x(19.8), cy), QPointF(rig.x(24.2), cy), {}, false); // 24 is a frame from the edit at 23
        CHECK(v->gestureActive());
        CHECK(std::abs(v->snapX() - rig.x(23)) < 1.0);
        CHECK(v->gestureReadout().contains(QStringLiteral("Duration")));
        CHECK(std::abs(v->stats().captionRects.value(TimelineStats::captionKey(0, 0)).right() - rig.x(23)) < 2.0);
        rig.release(QPointF(rig.x(24.2), cy));
        CHECK_FALSE(v->gestureActive());
        CHECK(cue(0) == std::pair(f(5), f(23)));
        CHECK(rig.undoSteps() == before + 1);
    }
    SECTION("trims stop at the neighbouring cues") {
        rig.drag(QPointF(rig.x(19.8), cy), QPointF(rig.x(39.8), cy)); // +20 frames: the next cue starts at 30
        CHECK(cue(0) == std::pair(f(5), f(30)));
        rig.drag(QPointF(rig.x(60.2), cy), QPointF(rig.x(30.2), cy)); // the head of the third, 30 frames earlier
        CHECK(cue(2) == std::pair(f(45), f(80)));
        CHECK(rig.undoSteps() == before + 2);
    }
    SECTION("a body drag moves the cue and keeps its length and stops at the cue before") {
        rig.drag(QPointF(rig.x(70), cy), QPointF(rig.x(64), cy)); // -6 frames
        CHECK(cue(2) == std::pair(f(54), f(74)));
        CHECK(rig.undoSteps() == before + 1);
        rig.drag(QPointF(rig.x(64), cy), QPointF(rig.x(4), cy));
        CHECK(cue(2) == std::pair(f(45), f(65)));
        CHECK(rig.undoSteps() == before + 2);
        REQUIRE(rig.controller.undo());
        CHECK(cue(2) == std::pair(f(54), f(74)));
    }
    SECTION("with overlaps allowed nothing stops a cue") {
        CaptionStyle style = trackOf(rig.controller, en).style;
        style.allowOverlap = true;
        REQUIRE(rig.controller.applyEditFn("Caption Style", [&](const ProjectState& s) {
            return edit::setCaptionStyle(s, rig.tl.seq, en, style);
        }));
        rig.harness.frame();
        rig.drag(QPointF(rig.x(19.8), cy), QPointF(rig.x(39.8), cy));
        CHECK(cue(0) == std::pair(f(5), f(40)));
    }
    SECTION("Esc cancels a drag and nothing changes") {
        rig.drag(QPointF(rig.x(19.8), cy), QPointF(rig.x(27.8), cy), {}, false);
        REQUIRE(v->gestureActive());
        rig.key(Qt::Key_Escape);
        CHECK_FALSE(v->gestureActive());
        rig.release(QPointF(rig.x(27.8), cy));
        CHECK(cue(0) == std::pair(f(5), f(20)));
        CHECK(rig.undoSteps() == before);
    }
    SECTION("only the Selection tool drags cues") {
        v->setTool(QStringLiteral("ripple"));
        rig.drag(QPointF(rig.x(19.8), cy), QPointF(rig.x(25.8), cy));
        CHECK(cue(0) == std::pair(f(5), f(20)));
        CHECK(rig.undoSteps() == before);
        CHECK(v->captionSelection().cue == 0);
    }
}

TEST_CASE("Caption lanes split and delete cues with the razor and Add Edit and Delete in one undo step each",
          "[app][timeline][captions][edit]") {
    Rig rig(picture());
    const CaptionTrackId en = addCaptions(rig.controller, "English", "en",
                                          {{5, 20, "One two"}, {30, 45, "Two words here"}, {60, 80, "Three little words"}});
    rig.harness.frame();
    TimelineView* v = rig.view;
    const double cy = captionY(*v, 0);
    const auto cues = [&] { return trackOf(rig.controller, en).cues; };
    const auto clipsOnV1 = [&] { return rig.controller.currentSequence()->videoTracks[0]->clips.size(); };
    const auto before = rig.undoSteps();
    REQUIRE(clipsOnV1() == 2);

    SECTION("the razor splits the cue it clicks") {
        v->setTool(QStringLiteral("razor"));
        rig.press(QPointF(rig.x(37.2), cy));
        rig.release(QPointF(rig.x(37.2), cy));
        REQUIRE(cues().size() == 4);
        CHECK(cues()[1].end() == f(37));
        CHECK(cues()[2].start == f(37));
        CHECK(rig.undoSteps() == before + 1);
        CHECK(clipsOnV1() == 2); // the clips are not cut
    }
    SECTION("Add Edit splits the cue under the playhead on a targeted caption row with the clips in one undo step") {
        rig.controller.setPlayhead(f(65));
        CHECK_FALSE(v->addEditAtPlayhead()); // no caption row targeted and no cue selected: the clips' Add Edit alone
        CHECK(rig.undoSteps() == before);
        v->setCaptionTargeted(captionTrackKey(en), true);
        CHECK(v->isCaptionTargeted(captionTrackKey(en)));
        CHECK(v->captionRowItems()[0].toMap().value(QStringLiteral("targeted")).toBool());
        REQUIRE(v->addEditAtPlayhead());
        CHECK(rig.undoSteps() == before + 1);
        REQUIRE(cues().size() == 4);
        CHECK(cues()[2].end() == f(65));
        CHECK(cues()[3].start == f(65));
        CHECK(clipsOnV1() == 3); // the targeted V1 and A1 are cut there too
        CHECK(rig.controller.currentSequence()->audioTracks[0]->clips.size() == 3);
        REQUIRE(rig.controller.undo());
        CHECK(cues().size() == 3);
        CHECK(clipsOnV1() == 2);
        v->setCaptionTargeted(captionTrackKey(en), false);
        CHECK_FALSE(v->isCaptionTargeted(captionTrackKey(en)));
    }
    SECTION("Add Edit with a cue selected under the playhead splits only that cue") {
        v->selectCaption(captionTrackKey(en), 1);
        rig.controller.setPlayhead(f(40));
        REQUIRE(v->addEditAtPlayhead());
        REQUIRE(cues().size() == 4);
        CHECK(cues()[1].end() == f(40));
        CHECK(clipsOnV1() == 2);
        CHECK(rig.undoSteps() == before + 1);
    }
    SECTION("Delete removes the selected cue") {
        CHECK_FALSE(v->deleteSelectedCaption());
        v->selectCaption(captionTrackKey(en), 0);
        REQUIRE(v->deleteSelectedCaption());
        REQUIRE(cues().size() == 2);
        CHECK(cues()[0].start == f(30));
        CHECK_FALSE(v->captionSelection().valid());
        CHECK(rig.undoSteps() == before + 1);
        REQUIRE(rig.controller.undo());
        CHECK(cues().size() == 3);
    }
}

TEST_CASE("Caption lanes drag cues that already overlap on a track that forbids overlaps without a jump",
          "[app][timeline][captions][edit]") {
    // Fix round 1 (QA #1). Saving a caption preset again restyles the tracks that follow it without looking at their
    // cues (edit::saveCaptionPreset), so the model API can leave overlapping cues on a track whose style now forbids
    // overlaps (the commands refuse that state: validate()). The clamp bounds of a drag then never pass the dragged cue
    // as it is: no empty clamp range (undefined behaviour, an assertion in debug builds) and no jump to a neighbour.
    Rig rig(picture());
    CaptionStyle loose;
    loose.allowOverlap = true;
    const auto preset = edit::saveCaptionPreset(rig.tl.state, CaptionPreset{"Loose", loose});
    REQUIRE(preset);
    const auto made = edit::addCaptionTrack(*preset, rig.tl.seq, "English", "en", loose);
    REQUIRE(made);
    const CaptionTrackId en = made->createdCaptionTrack;
    const auto follows = edit::setCaptionTrackStyle(made->state, rig.tl.seq, en, "Loose");
    REQUIRE(follows);
    ProjectState s = follows->state;
    // A [5, 20), B [15, 30) over A's end, C [20, 25) inside B (B's tail stays free to grab).
    for (const Cue& cue : std::vector<Cue>{{5, 20, "One two"}, {15, 30, "Two words here"}, {20, 25, "Inner"}}) {
        const auto added = edit::addCaptionCue(s, rig.tl.seq, en, CaptionCue{f(cue.from), f(cue.to) - f(cue.from), cue.text, {}});
        REQUIRE(added);
        s = added->state;
    }
    CaptionStyle strict = loose;
    strict.allowOverlap = false;
    const auto restyled = edit::saveCaptionPreset(s, CaptionPreset{"Loose", strict});
    REQUIRE(restyled);
    rig.controller.resetProject(*restyled, rig.tl.seq);
    rig.harness.frame();
    TimelineView* v = rig.view;
    const CaptionTrack track = trackOf(rig.controller, en);
    REQUIRE_FALSE(track.style.allowOverlap);
    REQUIRE(edit::captionTrackProblem(track).has_value()); // the overlaps its style forbids
    const double cy = captionY(*v, 0);
    const auto preview = [&] { return v->stats().captionRects.value(TimelineStats::captionKey(0, 1)); };
    const auto before = rig.undoSteps();
    const auto unchanged = [&] {
        CHECK_FALSE(v->gestureActive());
        CHECK(trackOf(rig.controller, en).cues == track.cues);
        CHECK(rig.undoSteps() == before);
    };

    SECTION("a body drag keeps B where it is instead of jumping past A's end") {
        rig.drag(QPointF(rig.x(17.5), cy), QPointF(rig.x(13.5), cy), {}, false); // 4 frames earlier
        REQUIRE(v->gestureActive());
        CHECK(std::abs(preview().left() - rig.x(15)) < 1.0);
        CHECK(std::abs(preview().right() - rig.x(30)) < 2.0);
        rig.move(QPointF(rig.x(21.5), cy)); // 4 frames later
        CHECK(std::abs(preview().left() - rig.x(15)) < 1.0);
        rig.release(QPointF(rig.x(21.5), cy)); // back where it was: no edit
        unchanged();
    }
    SECTION("a head trim moves B's start later from where it is") {
        rig.drag(QPointF(rig.x(15.2), cy), QPointF(rig.x(17.2), cy), {}, false);
        REQUIRE(v->gestureActive());
        CHECK(std::abs(preview().left() - rig.x(17)) < 1.0);
        rig.move(QPointF(rig.x(12.2), cy)); // not earlier than it is: A ends at 20
        CHECK(std::abs(preview().left() - rig.x(15)) < 1.0);
        rig.key(Qt::Key_Escape);
        rig.release(QPointF(rig.x(12.2), cy));
        unchanged();
    }
    SECTION("a tail trim shortens B and never lengthens it past where it is") {
        rig.drag(QPointF(rig.x(29.8), cy), QPointF(rig.x(26.8), cy), {}, false); // 3 frames shorter
        REQUIRE(v->gestureActive());
        CHECK(std::abs(preview().right() - rig.x(27)) < 2.0);
        rig.move(QPointF(rig.x(33.8), cy));
        CHECK(std::abs(preview().right() - rig.x(30)) < 2.0);
        // The model refuses an edit on a track that breaks its rules: no undo step, nothing changes.
        rig.move(QPointF(rig.x(26.8), cy));
        rig.release(QPointF(rig.x(26.8), cy));
        unchanged();
    }
}

TEST_CASE("Caption lanes Add Edit with a selection cuts only the selected items as Premiere does",
          "[app][timeline][captions][edit]") {
    // Fix round 1 (QA #11).
    Rig rig(picture());
    const CaptionTrackId en = addCaptions(rig.controller, "English", "en",
                                          {{5, 20, "One two"}, {30, 45, "Two words here"}, {60, 80, "Three little words"}, {85, 95, "Bye"}});
    rig.harness.frame();
    TimelineView* v = rig.view;
    v->setCaptionTargeted(captionTrackKey(en), true);
    const auto cues = [&] { return trackOf(rig.controller, en).cues; };
    const auto clipsOnV1 = [&] { return rig.controller.currentSequence()->videoTracks[0]->clips.size(); };
    const std::vector<CaptionCue> original = cues();
    const auto before = rig.undoSteps();

    SECTION("selected clips under the playhead are cut alone and the targeted caption row stays whole") {
        rig.controller.setSelection({rig.clip("V1@0")});
        rig.controller.setPlayhead(f(10)); // inside V1's first clip and the first cue
        CHECK_FALSE(v->addEditAtPlayhead()); // the panel runs ProjectController's Add Edit
        CHECK(v->captionMessage().isEmpty());
        CHECK(cues() == original);
        CHECK(rig.undoSteps() == before);
        rig.controller.razorAtPlayhead(false);
        CHECK(rig.undoSteps() == before + 1);
        CHECK(clipsOnV1() == 3);
        CHECK(cues() == original);
    }
    SECTION("selected clips the playhead misses cut the targeted tracks and caption rows as with nothing selected") {
        rig.controller.setSelection({rig.clip("V1@0")});
        rig.controller.setPlayhead(f(65)); // past the selected clip (it ends at 23), inside the third cue
        REQUIRE(v->addEditAtPlayhead());
        CHECK(v->captionMessage().isEmpty());
        CHECK(rig.undoSteps() == before + 1);
        REQUIRE(cues().size() == 5);
        CHECK(cues()[2].end() == f(65));
        CHECK(clipsOnV1() == 3);
    }
    SECTION("a selected cue the playhead is not inside cuts nothing and says why") {
        v->selectCaption(captionTrackKey(en), 0);
        rig.controller.setPlayhead(f(40)); // inside the second cue, not the selected first
        CHECK(v->addEditAtPlayhead()); // handled: the clips are not cut either
        CHECK_FALSE(v->captionMessage().isEmpty());
        CHECK(cues() == original);
        CHECK(clipsOnV1() == 2);
        CHECK(rig.undoSteps() == before);
    }
    SECTION("a selected cue that cannot split cuts nothing and says why") {
        v->selectCaption(captionTrackKey(en), 3);
        rig.controller.setPlayhead(f(90)); // inside the one-word cue: its text has nowhere to break
        CHECK(v->addEditAtPlayhead());
        CHECK_FALSE(v->captionMessage().isEmpty());
        CHECK(cues() == original);
        CHECK(clipsOnV1() == 2);
        CHECK(rig.undoSteps() == before);
        // A cue that can split clears the message.
        v->selectCaption(captionTrackKey(en), 2);
        rig.controller.setPlayhead(f(70));
        REQUIRE(v->addEditAtPlayhead());
        CHECK(v->captionMessage().isEmpty());
        CHECK(cues().size() == 5);
        CHECK(rig.undoSteps() == before + 1);
    }
    SECTION("with nothing selected every overlapping cue under the playhead on a targeted row is cut in one undo step") {
        CaptionStyle style = trackOf(rig.controller, en).style;
        style.allowOverlap = true;
        REQUIRE(rig.controller.applyEditFn("Caption Style", [&](const ProjectState& s) {
            return edit::setCaptionStyle(s, rig.tl.seq, en, style);
        }));
        REQUIRE(rig.controller.applyEditFn("Add Caption", [&](const ProjectState& s) {
            return edit::addCaptionCue(s, rig.tl.seq, en, CaptionCue{f(10), f(15), "Over the first", {}});
        }));
        const auto steps = rig.undoSteps();
        rig.controller.setPlayhead(f(12)); // inside [5, 20) and [10, 25)
        REQUIRE(v->addEditAtPlayhead());
        CHECK(v->captionMessage().isEmpty());
        CHECK(rig.undoSteps() == steps + 1);
        const std::vector<CaptionCue> now = cues();
        REQUIRE(now.size() == 7);
        CHECK(std::ranges::count_if(now, [](const CaptionCue& c) { return c.end() == f(12); }) == 2);
        CHECK(std::ranges::count_if(now, [](const CaptionCue& c) { return c.start == f(12); }) == 2);
        CHECK(clipsOnV1() == 3); // with the targeted tracks' clips
        REQUIRE(rig.controller.undo());
        CHECK(cues().size() == 5);
        CHECK(clipsOnV1() == 2);
    }
}

TEST_CASE("Caption lanes rebuild no cue geometry and lay out no labels on a pure scroll", "[app][timeline][captions]") {
    // Fix round 1 (QA #8): the caption layer rebuilt its mesh and visited every cue on each scrolled pixel. A scroll
    // inside the built range now rebuilds no geometry (the content transform moves it): a horizontal one places the
    // labels of the cues in the view again, a vertical one touches nothing. A new built range visits only the cues near
    // it (a binary search on the starts), however long the track.
    std::string pic;
    for (int i = 6; i >= 1; --i)
        pic += "V" + std::to_string(i) + (i == 1 ? " tgt" : "") + " |Aaaaaaaaaa|\n";
    for (int i = 1; i <= 3; ++i)
        pic += "A" + std::to_string(i) + " |Aaaaaaaaaa|\n";
    Rig rig(pic, 10.0, false);
    CaptionTrack longTrack;
    longTrack.name = "Long";
    longTrack.language = "en";
    constexpr int kCues = 3000; // 42 frames apart, 40 long: 84 minutes
    for (int i = 0; i < kCues; ++i)
        longTrack.cues.push_back(CaptionCue{f(42 * i + 1), f(40), "Cue " + std::to_string(i), {}});
    const auto made = edit::setCaptionTrack(rig.tl.state, rig.tl.seq, longTrack);
    REQUIRE(made);
    // The sequence as long as its captions (the view scrolls over the sequence's duration).
    const Sequence* seq = made->state.findSequence(rig.tl.seq);
    REQUIRE(seq);
    auto longer = std::make_shared<Sequence>(*seq);
    longer->duration = f(42 * kCues + 50);
    rig.controller.resetProject(made->state.withSequence(std::move(longer)), rig.tl.seq);
    TimelineView* v = rig.view;
    const auto settle = [&] {
        rig.harness.frame();
        for (int i = 0; i < 20 && v->stats().labelsPending; ++i)
            rig.harness.frame();
        REQUIRE_FALSE(v->stats().labelsPending);
    };
    settle();
    REQUIRE(v->captionRows().size() == 1);
    const QString track = captionTrackKey(v->captionRows()[0].id);
    REQUIRE(v->contentHeight() > v->height() + 60); // room for a vertical scroll

    // A new built range far down the track (out of the old one: a full rebuild) visits a handful of the 3000 cues.
    v->setScrollX(10.0 * 42 * 1500); // cue 1500 starts a frame after the view's left edge
    rig.harness.frame();
    CHECK(v->stats().fullRebuild);
    CHECK(v->stats().captionMeshBuilt);
    CHECK(v->stats().captionCues >= 3);
    CHECK(v->stats().captionCues <= 6);
    CHECK(v->stats().captionCuesVisited < 20);
    settle();
    const quint64 key = TimelineStats::captionKey(0, 1500);
    CHECK(v->stats().captionTexts.value(key) == QStringLiteral("Cue 1500"));

    // A horizontal scroll inside the built range: no geometry, no text layout, only the cues in the view visited.
    const double sx = v->scrollX();
    v->setScrollX(sx + 30);
    rig.harness.frame();
    CHECK_FALSE(v->stats().fullRebuild);
    CHECK_FALSE(v->stats().captionMeshBuilt);
    CHECK(v->stats().captionLabelLayouts == 0);
    CHECK(v->stats().captionCuesVisited > 0);
    CHECK(v->stats().captionCuesVisited <= 4);
    CHECK(v->stats().captionTexts.value(key) == QStringLiteral("Cue 1500")); // the label sticks to the left edge
    CHECK(std::abs(v->stats().captionRects.value(key).left() - (10.0 * (42 * 1500 + 1) - v->scrollX())) < 1.0);

    // A vertical scroll: nothing at all; the drawn blocks move with the content.
    const QRectF above = v->stats().captionRects.value(key);
    v->setScrollY(40);
    REQUIRE(v->scrollY() == 40);
    rig.harness.frame();
    CHECK_FALSE(v->stats().captionMeshBuilt);
    CHECK(v->stats().captionLabelLayouts == 0);
    CHECK(v->stats().captionCuesVisited == 0);
    CHECK(v->stats().captionTexts.value(key) == QStringLiteral("Cue 1500"));
    CHECK(std::abs(v->stats().captionRects.value(key).top() - (above.top() - 40)) < 0.01);

    // A content change (the selection outline) rebuilds the blocks, still visiting only the built range.
    v->selectCaption(track, 1501);
    rig.harness.frame();
    CHECK(v->stats().captionMeshBuilt);
    CHECK(v->stats().captionCuesVisited < 20);
}

TEST_CASE("Caption lanes keep the selected cue in CaptionsBridge and its eye toggle is one undo step", "[app][timeline][captions]") {
    Rig rig(picture());
    CaptionsBridge captions;
    captions.setController(&rig.controller);
    rig.view->setCaptions(&captions);
    const CaptionTrackId en = addCaptions(rig.controller, "English", "en", {{5, 20, "One"}, {30, 45, "Two words here"}, {60, 80, "Three"}});
    const CaptionTrackId th = addCaptions(rig.controller, "Thai", "th", {{10, 40, "Sawasdee"}});
    rig.harness.frame();
    TimelineView* v = rig.view;

    // A cue clicked on C2: the Captions page shows C2 with that cue selected.
    rig.press(QPointF(rig.x(20), captionY(*v, 1)));
    rig.release(QPointF(rig.x(20), captionY(*v, 1)));
    CHECK(captions.track() == captionTrackKey(th));
    CHECK(captions.selectedCue() == 0);
    CHECK(v->captionSelection().track == th);
    CHECK(v->captionSelection().cue == 0);
    // Selecting a clip clears it.
    rig.press(QPointF(rig.x(10), rig.y("V1")));
    rig.release(QPointF(rig.x(10), rig.y("V1")));
    CHECK(captions.selectedCue() == -1);
    CHECK_FALSE(v->captionSelection().valid());
    // The page selects too, and showing another track clears it.
    captions.selectCue(captionTrackKey(en), 2);
    CHECK(captions.track() == captionTrackKey(en));
    CHECK(captions.selectedCue() == 2);
    CHECK(v->captionSelection().cue == 2);
    captions.setTrack(captionTrackKey(th));
    CHECK(captions.selectedCue() == -1);
    // editCue asks the page to open a cue's text (one that exists).
    int asked = -1;
    QObject::connect(&captions, &CaptionsBridge::editCueRequested, [&](int index) { asked = index; });
    captions.editCue(0);
    CHECK(asked == 0);
    asked = -1;
    captions.editCue(7);
    CHECK(asked == -1);

    // The header's eye hides a track: one undo step; the row dims.
    const auto before = rig.undoSteps();
    CHECK(captions.setTrackEnabledFor(captionTrackKey(th), false).isEmpty());
    CHECK(rig.undoSteps() == before + 1);
    CHECK_FALSE(trackOf(rig.controller, th).enabled);
    rig.harness.frame();
    CHECK(v->stats().dimmedCaptionCues == 1);
    REQUIRE(rig.controller.undo());
    CHECK(trackOf(rig.controller, th).enabled);
    CHECK_FALSE(captions.setTrackEnabledFor(QStringLiteral("nonsense"), false).isEmpty());

    // A deleted cue is no longer selected.
    captions.selectCue(captionTrackKey(en), 2);
    REQUIRE(v->deleteSelectedCaption());
    CHECK(captions.selectedCue() == -1);
    CHECK(trackOf(rig.controller, en).cues.size() == 2);
}

TEST_CASE("Caption lanes in the shell with their headers and the eye and the display toggle in dark and light themes",
          "[app][qml][timeline][captions][screenshot]") {
    bool dark = true;
    SECTION("dark") { dark = true; }
    SECTION("light") { dark = false; }
    resetSettings();
    clearMessages();
    ShellProject p(picture(), dark);
    const CaptionTrackId en = addCaptions(*p.controller, "English", "en",
                                          {{5, 20, "Welcome back"}, {30, 45, "Today we cut\na whole episode"}, {60, 80, "Let's go"}});
    const CaptionTrackId th = addCaptions(*p.controller, "Thai", "th", {{10, 40, "Sawasdee krub"}});
    auto* view = qobject_cast<TimelineView*>(p.item(QStringLiteral("timelineView")));
    REQUIRE(view);
    view->setPixelsPerSecond(260);
    view->setScrollX(0);
    view->setScrollY(0);
    spin(300);
    p.settle();

    // The headers: one per caption row, as high as it, above the video tracks' headers.
    REQUIRE(waitFor([&] { return p.item(QStringLiteral("captionHeader_C2")) != nullptr; }));
    REQUIRE(view->captionRows().size() == 2);
    QQuickItem* header = p.item(QStringLiteral("captionHeader_C1"));
    REQUIRE(header);
    CHECK(std::abs(header->height() - view->captionRows()[0].height) < 0.5);
    const double rowTop = view->mapToScene(QPointF(0, view->captionRows()[0].y - view->scrollY())).y();
    CHECK(std::abs(header->mapToScene(QPointF(0, 0)).y() - rowTop) < 1.0);
    QQuickItem* name = p.item(QStringLiteral("captionName_C1"));
    REQUIRE(name);
    CHECK(name->property("text").toString() == QStringLiteral("English (en)"));
    CHECK(p.item(QStringLiteral("captionName_C2"))->property("text").toString() == QStringLiteral("Thai (th)"));
    // V1's header lines up with its row below the caption rows.
    QQuickItem* v1Target = nullptr;
    for (QQuickItem* candidate : header->parentItem()->childItems()) {
        if (candidate->property("shortName").toString() == QStringLiteral("V1"))
            v1Target = candidate;
    }
    REQUIRE(v1Target);
    const double v1Top = view->mapToScene(QPointF(0, view->rows().front().y - view->scrollY())).y();
    CHECK(std::abs(v1Target->mapToScene(QPointF(0, 0)).y() - v1Top) < 1.0);

    // A selected cue (the Captions page follows) for the picture.
    view->selectCaption(captionTrackKey(en), 1);
    CHECK(p.captions->selectedCue() == 1);
    CHECK(p.captions->track() == captionTrackKey(en));
    spin(100);
    p.settle();
    QQuickItem* panel = p.item(QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    p.shot(sceneRect(panel), dark ? QStringLiteral("timeline-caption-lanes.png") : QStringLiteral("timeline-caption-lanes-light.png"));

    if (dark) {
        // The chip targets the row for Add Edit (view state: no undo step).
        const auto before = p.undo();
        click(p.item(QStringLiteral("captionTarget_C1")));
        CHECK(view->isCaptionTargeted(captionTrackKey(en)));
        CHECK(p.undo() == before);
        REQUIRE(waitFor([&] { return p.item(QStringLiteral("captionHeader_C1"))->property("targeted").toBool(); }));
        // The eye hides C2: one undo step, its header follows.
        click(p.item(QStringLiteral("captionOutput_C2")));
        CHECK(p.undo() == before + 1);
        CHECK_FALSE(trackOf(*p.controller, th).enabled);
        REQUIRE(waitFor([&] {
            QQuickItem* h = p.item(QStringLiteral("captionHeader_C2"));
            return h && !h->property("trackEnabled").toBool();
        }));
        spin(100);
        p.settle();
        p.shot(sceneRect(panel).adjusted(0, 0, 0, -sceneRect(panel).height() / 2),
               QStringLiteral("timeline-caption-lanes-targeted-hidden.png"));

        // Timeline Display > Caption tracks: off hides the rows and their headers.
        QObject* popover = p.object(QStringLiteral("timelineDisplayPopover"));
        REQUIRE(popover);
        REQUIRE(QMetaObject::invokeMethod(popover, "open"));
        REQUIRE(waitFor([&] { return popover->property("opened").toBool(); }, 5000));
        QObject* toggle = inPopover(popover, "captionTracksToggle");
        CHECK(toggle->property("checked").toBool());
        flip(toggle, false);
        CHECK_FALSE(p.shell.preferences->property("timelineShowCaptionTracks").toBool());
        CHECK(view->captionRows().empty());
        REQUIRE(waitFor([&] { return p.item(QStringLiteral("captionHeader_C1")) == nullptr; }));
        flip(toggle, true);
        CHECK(view->captionRows().size() == 2);
        REQUIRE(QMetaObject::invokeMethod(popover, "close"));
        REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }, 5000));

        // A double-click on a cue opens its text for typing in the Captions page.
        REQUIRE(QMetaObject::invokeMethod(view, "captionDoubleClicked", Q_ARG(QString, captionTrackKey(en)), Q_ARG(int, 2)));
        REQUIRE(waitFor([&] {
            QQuickItem* tabs = p.item(QStringLiteral("transcriptTabs"));
            return tabs && tabs->property("currentIndex").toInt() == 2;
        }));
        // The page waits for its list's layout, which a hidden window only gets when it renders a frame: render.
        REQUIRE(waitFor([&] {
            p.render();
            QQuickItem* row = p.item(QStringLiteral("captionCue_2"));
            QQuickItem* text = row ? findNamed(row, QStringLiteral("captionText")) : nullptr;
            return text && text->hasActiveFocus();
        }));
        CHECK(p.item(QStringLiteral("captionCue_2"))->property("selected").toBool() == (p.captions->selectedCue() == 2));

        // The cue menu (the right click selected the cue): Split at Playhead, Delete, Select in Captions.
        QObject* menu = p.object(QStringLiteral("captionCueMenu"));
        REQUIRE(menu);
        const auto openMenu = [&](int cue) {
            view->selectCaption(captionTrackKey(en), cue);
            emit view->captionContextMenuRequested(captionTrackKey(en), cue, 300, captionY(*view, 0));
            REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 5000));
        };
        p.controller->setPlayhead(f(40)); // inside the second cue
        openMenu(1);
        CHECK(child(menu, "captionMenuTitle")->property("text").toString() == QStringLiteral("C1 · Today we cut"));
        CHECK(child(menu, "captionMenuSplit")->property("enabled").toBool());
        p.settle();
        p.shot(popupRect(menu).adjusted(-320, -60, 40, 40), QStringLiteral("timeline-caption-menu.png"));
        std::size_t steps = p.undo();
        trigger(child(menu, "captionMenuSplit"));
        waitClosed(menu);
        CHECK(p.undo() == steps + 1);
        CHECK(trackOf(*p.controller, en).cues.size() == 4);
        // Select in Captions: the Captions page comes forward with the cue.
        p.item(QStringLiteral("transcriptTabs"))->setProperty("currentIndex", 0);
        openMenu(3);
        trigger(child(menu, "captionMenuSelect"));
        waitClosed(menu);
        REQUIRE(waitFor([&] { return p.item(QStringLiteral("transcriptTabs"))->property("currentIndex").toInt() == 2; }));
        CHECK(p.captions->selectedCue() == 3);
        // Delete: one undo step.
        openMenu(3);
        steps = p.undo();
        trigger(child(menu, "captionMenuDelete"));
        waitClosed(menu);
        CHECK(p.undo() == steps + 1);
        CHECK(trackOf(*p.controller, en).cues.size() == 3);
        // No split where the playhead is not inside the cue.
        openMenu(0);
        CHECK_FALSE(child(menu, "captionMenuSplit")->property("enabled").toBool());
        REQUIRE(QMetaObject::invokeMethod(menu, "close"));
        waitClosed(menu);
    }
    resetSettings();
    checkNoMessages();
}

TEST_CASE("Caption lanes open a cue far down the Captions page for typing before the page was laid out",
          "[app][qml][timeline][captions]") {
    // Fix round 1 (QA #15): the request reached the page as the Captions tab came forward, before its list had a size;
    // the list did not scroll and the cue's text never took the keyboard. Now the page waits for the list (a frame at a
    // time) and then shows the cue and opens its text.
    resetSettings();
    clearMessages();
    ShellProject p(picture(), true);
    std::vector<std::string> texts;
    for (int i = 0; i < 40; ++i)
        texts.push_back("Caption line number " + std::to_string(i + 1));
    std::vector<Cue> many;
    for (int i = 0; i < 40; ++i)
        many.push_back({2 * i, 2 * i + 2, texts[static_cast<std::size_t>(i)].c_str()});
    const CaptionTrackId en = addCaptions(*p.controller, "English", "en", many);
    p.captions->setTrack(captionTrackKey(en));
    spin(100);
    p.settle();
    QQuickItem* tabs = p.item(QStringLiteral("transcriptTabs"));
    REQUIRE(tabs);
    REQUIRE(tabs->property("currentIndex").toInt() != 2); // the Captions page has not been shown
    const auto textOf = [&](int cue) {
        QQuickItem* row = p.item(QStringLiteral("captionCue_%1").arg(cue));
        return row ? findNamed(row, QStringLiteral("captionText")) : nullptr;
    };
    const auto inList = [&](int cue) {
        QQuickItem* row = p.item(QStringLiteral("captionCue_%1").arg(cue));
        QQuickItem* list = p.item(QStringLiteral("captionCues"));
        REQUIRE(row);
        REQUIRE(list);
        const QRectF r = row->mapRectToItem(list, QRectF(0, 0, row->width(), row->height()));
        return list->height() > 0 && r.top() >= -1 && r.bottom() <= list->height() + 1;
    };

    // A double-click on the timeline (editCue): the page comes forward, scrolls to cue 36 and its text takes the keys.
    p.captions->editCue(35);
    // The page waits for its list's layout, which a hidden window only gets when it renders a frame (the app's window
    // renders them anyway): render while waiting.
    const bool focused = waitFor([&] {
        p.render();
        QQuickItem* text = textOf(35);
        return text && text->hasActiveFocus();
    });
    if (!focused) {
        QQuickItem* page = p.item(QStringLiteral("captionsPage"));
        QQuickItem* list = p.item(QStringLiteral("captionCues"));
        QQuickItem* focusItem = p.shell.window->activeFocusItem();
        UNSCOPED_INFO("page " << (page ? page->property("pendingCue").toInt() : -2) << " tries "
                              << (page ? page->property("pendingTries").toInt() : -2) << " list "
                              << (list ? list->height() : -2) << " visible " << (list && list->isVisible()) << " count "
                              << (list ? list->property("count").toInt() : -2) << " row " << (textOf(35) != nullptr)
                              << " focus " << (focusItem ? focusItem->objectName().toStdString() : std::string("none"))
                              << " " << (focusItem ? focusItem->metaObject()->className() : "") << " messages "
                              << join(takeMessages()));
    }
    REQUIRE(focused);
    CHECK(tabs->property("currentIndex").toInt() == 2);
    CHECK(inList(35));

    // Select in Captions from another tab: the page shows cue 3 (at the top again), the keyboard stays where it was.
    tabs->setProperty("currentIndex", 0);
    p.settle();
    p.captions->showCue(2);
    REQUIRE(waitFor([&] { return tabs->property("currentIndex").toInt() == 2; }));
    REQUIRE(waitFor([&] {
        p.render();
        return textOf(2) != nullptr && inList(2);
    }));
    CHECK_FALSE(textOf(2)->hasActiveFocus());
    resetSettings();
    checkNoMessages();
}

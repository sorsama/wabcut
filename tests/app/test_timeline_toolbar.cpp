// 0.1.3 timeline feedback: snapped drops from outside the lanes (a Library card dropped after a clip used to land a
// frame or more after it, leaving a gap), the edit cursors (EditCursors: what a press does, on hover and while the
// press holds), and the Tracks toolbar (Premiere's tool groups with flyouts in the header, the modes as labelled
// toggles beside the sequence tabs, nothing vanishing on narrow panels).

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "EditCursors.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <catch2/catch_test_macros.hpp>
#include <wabcut/editor/SourceEdit.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/Timecode.h>

#include <QCoreApplication>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QImage>
#include <QMimeData>
#include <QTemporaryDir>
#include <QUrl>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlProperty>
#include <QtGui/private/qguiapplication_p.h>
#include <qpa/qwindowsysteminterface.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <tuple>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

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

QObject* findObject(QObject* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    for (QObject* child : root->children()) {
        if (QObject* found = findObject(child, name))
            return found;
    }
    if (auto* item = qobject_cast<QQuickItem*>(root)) {
        for (QQuickItem* child : item->childItems()) {
            if (QObject* found = findObject(child, name))
                return found;
        }
    }
    return nullptr;
}

/// The pointer moves over the lanes with no button down (a hover).
void hover(Rig& rig, QPointF pos, Qt::KeyboardModifiers mods = {}) {
    rig.send(QEvent::MouseMove, pos, Qt::NoButton, Qt::NoButton, mods);
    rig.harness.frame();
}

void modifierKey(Rig& rig, QEvent::Type type, int key, Qt::KeyboardModifiers mods) {
    QKeyEvent event(type, key, mods);
    QCoreApplication::sendEvent(&rig.harness.window(), &event);
}

/// Qt Quick gives hover events the application's last known modifiers (QGuiApplication::keyboardModifiers()), which
/// the synthetic key events of an earlier section leave behind (the platform keeps them right in the app).
void clearModifiers() {
    QGuiApplicationPrivate::modifier_buttons = Qt::NoModifier;
}

QString timecode(const Rig& rig, double frames) {
    return QString::fromStdString(ticksToTimecode(f(static_cast<std::int64_t>(frames)), rig.tl.sequence().frameRate, false));
}

/// `seconds` of 16-bit stereo silence at 48 kHz, as a WAV file.
bool writeWav(const QString& path, double seconds) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    const quint32 rate = 48000;
    const quint16 channels = 2;
    const quint16 bits = 16;
    const quint32 data = static_cast<quint32>(seconds * rate) * channels * (bits / 8);
    QByteArray bytes;
    const auto u32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i)
            bytes.append(static_cast<char>((v >> (8 * i)) & 0xff));
    };
    const auto u16 = [&](quint16 v) {
        bytes.append(static_cast<char>(v & 0xff));
        bytes.append(static_cast<char>(v >> 8));
    };
    bytes.append("RIFF");
    u32(36 + data);
    bytes.append("WAVEfmt ");
    u32(16);
    u16(1); // PCM
    u16(channels);
    u32(rate);
    u32(rate * channels * (bits / 8));
    u16(static_cast<quint16>(channels * (bits / 8)));
    u16(bits);
    bytes.append("data");
    u32(data);
    bytes.append(QByteArray(static_cast<qsizetype>(data), '\0'));
    return file.write(bytes) == bytes.size();
}

/// The clips of a track as [start, end) in seconds, with their media.
std::vector<std::tuple<double, double, MediaId>> clipsOn(const Track& track) {
    std::vector<std::tuple<double, double, MediaId>> out;
    for (const Clip& clip : track.clips)
        out.emplace_back(clip.start().toSeconds(), clip.end().toSeconds(), clip.media);
    return out;
}

} // namespace

// ---- Drops from outside the lanes ----------------------------------------------------------------------------------

TEST_CASE("A drop next to a clip snaps against it and lands with no gap", "[app][timeline][edit][drop]") {
    SECTION("the clip in front: a Library drop just after A lands on A's end") {
        Rig rig(R"(
            V1 |Aaaa......Bbbb|
            A1 |Aaaa......Bbbb|
        )", 2.0); // 2 px per frame: the 8 px threshold reaches 4 frames
        // A pointer a frame and a half after A (in the gap): the drop snaps back onto A's tail.
        const QVariantMap point = rig.view->dropPoint(rig.x(5.5), rig.y("V1"));
        CHECK(point.value(QStringLiteral("snapped")).toBool());
        CHECK(point.value(QStringLiteral("time")).toString() == timecode(rig, 4));
        CHECK(point.value(QStringLiteral("x")).toDouble() == rig.x(4));
        CHECK(point.value(QStringLiteral("track")).toString() == trackKey(rig.tl.track("V1")));

        // The drop itself, as Main's drop area makes it (ProjectFile.dropMedia -> editor::dropMedia): the new clip
        // starts exactly where A ends.
        const auto at = parseTimecode(point.value(QStringLiteral("time")).toString().toStdString(),
                                      rig.tl.sequence().frameRate, false);
        REQUIRE(at);
        editor::MediaDrop drop;
        drop.media = rig.tl.media('B');
        drop.sequence = rig.tl.seq;
        drop.at = *at;
        drop.track = rig.tl.track("V1");
        REQUIRE(editor::dropMedia(rig.controller.session(), drop));
        const Sequence* seq = rig.controller.snapshot()->findSequence(rig.tl.seq);
        REQUIRE(seq);
        const auto& v1 = seq->tracks(TrackKind::Video)[0]->clips;
        REQUIRE(v1.size() >= 3);
        CHECK(v1[0].end() == f(4));
        CHECK(v1[1].start() == f(4)); // no gap after A
        CHECK(v1[1].media == rig.tl.media('B'));
    }
    SECTION("snapping off: the frame under the pointer") {
        Rig rig(R"(
            V1 |Aaaa......Bbbb|
        )", 2.0);
        rig.view->setSnapping(false);
        const QVariantMap point = rig.view->dropPoint(rig.x(5.5), rig.y("V1"));
        CHECK_FALSE(point.value(QStringLiteral("snapped")).toBool());
        CHECK(point.value(QStringLiteral("time")).toString() == timecode(rig, 5));
    }
    SECTION("out of reach of every target: the frame under the pointer") {
        Rig rig(R"(
            V1 |Aaaa..............Bbbb|
        )", 2.0);
        rig.controller.setPlayhead(f(0));
        const QVariantMap point = rig.view->dropPoint(rig.x(11.5), rig.y("V1"));
        CHECK_FALSE(point.value(QStringLiteral("snapped")).toBool());
        CHECK(point.value(QStringLiteral("time")).toString() == timecode(rig, 11));
    }
    SECTION("the next clip's head snaps too, and below the tracks there is no track") {
        Rig rig(R"(
            V1 |Aaaa......Bbbb|
        )", 2.0);
        CHECK(rig.view->dropPoint(rig.x(8.6), rig.y("V1")).value(QStringLiteral("time")).toString() == timecode(rig, 10));
        CHECK(rig.view->dropPoint(rig.x(8.6), rig.view->height() - 1).value(QStringLiteral("track")).toString().isEmpty());
    }
}

TEST_CASE("Moves and trims snap to the neighbouring clip from either side, linked audio along",
          "[app][timeline][edit]") {
    // 2 px per frame: the 8 px threshold reaches 4 frames.
    const char* picture = R"(
        V1 |Aaaa......Bbbb......Cccc|
        A1 |Aaaa......Bbbb......Cccc|
    )";
    SECTION("B dragged left: its head lands on A's tail") {
        Rig rig(picture, 2.0);
        rig.drag({rig.x(12), rig.y("V1")}, {rig.x(7), rig.y("V1")}); // head 10 -> 5, a frame after A
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBbbb............Cccc|
            A1 |AaaaBbbb............Cccc|
        )"));
    }
    SECTION("B dragged right: its tail lands on C's head") {
        Rig rig(picture, 2.0);
        rig.drag({rig.x(12), rig.y("V1")}, {rig.x(17), rig.y("V1")}); // tail 14 -> 19, a frame before C
        CHECK(rig.picture() == normalize(R"(
            V1 |Aaaa............BbbbCccc|
            A1 |Aaaa............BbbbCccc|
        )"));
    }
    SECTION("dragged a frame into A (an overwrite), B snaps back against it") {
        Rig rig(picture, 2.0);
        rig.drag({rig.x(12), rig.y("V1")}, {rig.x(5), rig.y("V1")}); // head 10 -> 3
        CHECK(rig.picture() == normalize(R"(
            V1 |AaaaBbbb............Cccc|
            A1 |AaaaBbbb............Cccc|
        )"));
    }
    SECTION("B's tail trimmed toward C stops on C's head") {
        Rig rig(picture, 2.0);
        rig.drag({rig.x(14) - 1, rig.y("V1")}, {rig.x(19) - 1, rig.y("V1")}); // tail 14 -> 19
        CHECK(rig.picture() == normalize(R"(
            V1 |Aaaa......BbbbbbbbbbCccc|
            A1 |Aaaa......BbbbbbbbbbCccc|
        )"));
    }
}

TEST_CASE("A move drag shows the dragged clips where they will land, snapped, before the release",
          "[app][timeline][edit]") {
    // 0.1.3 bug: a long clip dragged against the clip in front was drawn into it (the lift's 2 % scale grew it around
    // its centre, tens of pixels on a long clip) and jumped back on release, though the snap line was right. The
    // preview must be the landing place: every dragged clip, its linked audio and the rest of the selection.
    // 4 px per frame (the 8 px threshold reaches 2 frames); B is 100 frames (400 px) long.
    const std::string bLong = "B" + std::string(99, 'b');
    const std::string picture = "\n        V2 |" + std::string(20, '.') + "Cccccccccccccccccccc" + std::string(73, '.') + "|\n"
                                + "        V1 |Aaaaaaaaaa..." + bLong + "|\n"
                                + "        A1 |Aaaaaaaaaa..." + bLong + "|\n";
    struct Case {
        const char* name;
        double to; ///< where the pointer goes (frames) from frame 50 inside B, whose head is at 13
    };
    // Raw head 11.5 (two frames short of A's tail, within reach) and 9.5 (overlapping A by half a frame).
    const Case cases[] = {{"within the threshold, short of the clip in front", 48.5},
                          {"the pointer overlapping the clip in front", 46.5}};
    for (const Case& c : cases) {
        DYNAMIC_SECTION(c.name) {
            Rig rig(picture, 4.0);
            const ClipId b = rig.clip("V1@13");
            const ClipId audio = rig.clip("A1@13");
            const ClipId other = rig.clip("V2@20");
            rig.controller.setSelection({b, other}); // a multi-track selection: C moves along
            const auto before = rig.controller.snapshot();
            // Out past the drag threshold first, then to the target (a 1.5 frame drag is shorter than the threshold).
            rig.press({rig.x(50), rig.y("V1")});
            rig.move({rig.x(54), rig.y("V1")});
            for (int i = 1; i <= 4; ++i)
                rig.move({rig.x(54 + (c.to - 54) * i / 4.0), rig.y("V1")});
            REQUIRE(rig.view->gestureActive());
            CHECK(rig.controller.snapshot() == before); // a preview: the model is untouched
            CHECK(rig.view->snapX() == rig.x(10));      // snapped onto A's tail

            // Drawn where they will land: B's head on A's tail (the 1 px gap only at its own tail), its audio the
            // same, C moved by the same 3 frames.
            const QHash<quint64, QRectF> drawn = rig.view->stats().liftedRects;
            REQUIRE(drawn.contains(b.value));
            REQUIRE(drawn.contains(audio.value));
            REQUIRE(drawn.contains(other.value));
            const auto near = [](double a, double e) { return std::abs(a - e) < 0.5; };
            INFO("B drawn from " << drawn[b.value].left() << " to " << drawn[b.value].right() << ", lands on "
                 << rig.x(10) << " to " << rig.x(110));
            CHECK(near(drawn[b.value].left(), rig.x(10)));
            CHECK(near(drawn[b.value].right(), rig.x(110) - 1));
            CHECK(near(drawn[audio.value].left(), rig.x(10)));
            CHECK(near(drawn[audio.value].right(), rig.x(110) - 1));
            CHECK(near(drawn[other.value].left(), rig.x(17)));
            CHECK(near(drawn[other.value].right(), rig.x(37) - 1));

            // The release lands exactly there.
            rig.release({rig.x(c.to), rig.y("V1")});
            const Sequence* seq = rig.controller.snapshot()->findSequence(rig.tl.seq);
            REQUIRE(seq);
            CHECK(seq->tracks(TrackKind::Video)[0]->findClip(b)->start() == f(10));
            CHECK(seq->tracks(TrackKind::Audio)[0]->findClip(audio)->start() == f(10));
            CHECK(seq->tracks(TrackKind::Video)[1]->findClip(other)->start() == f(17));
        }
    }
}

TEST_CASE("The drop indicator follows a drag over the lanes and goes when it leaves", "[app][timeline][drop]") {
    Rig rig(R"(
        V1 |Aaaa......Bbbb|
    )", 2.0);
    CHECK(rig.view->dropIndicator().isEmpty());
    const QVariantMap shown = rig.view->showDropIndicator(rig.x(5.5), rig.y("V1"));
    rig.harness.frame();
    CHECK(shown.value(QStringLiteral("snapped")).toBool());
    CHECK(rig.view->dropIndicator().value(QStringLiteral("x")).toDouble() == rig.x(4));
    rig.view->showDropIndicator(rig.x(7), rig.y("V1")); // moved: 3 frames from A's tail and B's head
    rig.harness.frame();
    CHECK_FALSE(rig.view->dropIndicator().isEmpty());
    rig.view->clearDropIndicator();
    rig.harness.frame();
    CHECK(rig.view->dropIndicator().isEmpty());
}

// ---- Edit cursors ---------------------------------------------------------------------------------------------------

TEST_CASE("Edit cursors say what a press does, on hover and while it holds", "[app][timeline][cursors]") {
    Rig rig(R"(
        V1 |AaaaBbbb....|
        A1 |AaaaBbbb....|
    )");
    const double v1 = rig.y("V1");
    clearModifiers();

    SECTION("selection tool: a body is the arrow, an edge a trim bracket (Ctrl: ripple), a hold moves") {
        hover(rig, {rig.x(1.5), v1});
        CHECK(rig.view->cursorKey().isEmpty());
        CHECK(rig.view->cursor().shape() == Qt::ArrowCursor);
        hover(rig, {rig.x(4) - 2, v1});
        CHECK(rig.view->cursorKey() == QStringLiteral("trimTail"));
        CHECK(rig.view->cursor().shape() == Qt::BitmapCursor);
        hover(rig, {rig.x(4) + 2, v1});
        CHECK(rig.view->cursorKey() == QStringLiteral("trimHead"));
        // Ctrl over an edge: a ripple trim. A modifier alone changes the cursor (the view has the focus once clicked).
        // Like a platform's, a synthetic key event carries the modifiers from before it; QKeyEvent::modifiers() gives
        // those after it.
        rig.view->forceActiveFocus();
        modifierKey(rig, QEvent::KeyPress, Qt::Key_Control, {});
        CHECK(rig.view->cursorKey() == QStringLiteral("rippleHead"));
        modifierKey(rig, QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
        CHECK(rig.view->cursorKey() == QStringLiteral("trimHead"));

        // Holding a body: the move cursor at once, through the drag; Ctrl makes it an insert move.
        rig.press({rig.x(6), v1});
        CHECK(rig.view->cursorKey() == QStringLiteral("move"));
        rig.move({rig.x(7), v1});
        rig.move({rig.x(8), v1});
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->cursorKey() == QStringLiteral("move"));
        modifierKey(rig, QEvent::KeyPress, Qt::Key_Control, {});
        CHECK(rig.view->cursorKey() == QStringLiteral("insert"));
        CHECK(rig.view->gestureReadout().startsWith(QStringLiteral("Insert")));
        modifierKey(rig, QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
        CHECK(rig.view->cursorKey() == QStringLiteral("move"));
        // Released over B's body: the arrow again.
        rig.release({rig.x(8), v1});
        CHECK_FALSE(rig.view->gestureActive());
        CHECK(rig.view->cursorKey().isEmpty());
    }
    SECTION("a trim keeps its bracket while the pointer drags away from the edge") {
        hover(rig, {rig.x(8) - 2, v1});
        REQUIRE(rig.view->cursorKey() == QStringLiteral("trimTail"));
        rig.drag({rig.x(8) - 2, v1}, {rig.x(6) - 2, v1}, {}, false);
        REQUIRE(rig.view->gestureActive());
        CHECK(rig.view->cursorKey() == QStringLiteral("trimTail"));
        rig.release({rig.x(6) - 2, v1});
    }
    SECTION("every tool has its own cursor") {
        struct Case {
            const char* tool;
            double frame;
            const char* expected;
        };
        // Frame 4 is the cut between A and B: 4 - 0.2 is A's tail zone, 4 + 0.2 B's head zone, 2 A's body.
        const Case cases[] = {
            {"ripple", 3.8, "rippleTail"}, {"ripple", 4.2, "rippleHead"}, {"ripple", 2, ""},
            {"roll", 3.8, "roll"},         {"rateStretch", 3.8, "rateTail"}, {"rateStretch", 4.2, "rateHead"},
            {"slip", 2, "slip"},           {"slide", 6, "slide"},           {"razor", 2, "razor"},
            {"razor", 10, "razor"},        {"pen", 2, "pen"},               {"trackForward", 10, "trackForward"},
            {"trackBackward", 2, "trackBackward"}, {"select", 2, ""},
        };
        for (const Case& c : cases) {
            INFO(c.tool << " at frame " << c.frame);
            rig.view->setTool(QString::fromLatin1(c.tool));
            hover(rig, {rig.x(c.frame), v1});
            CHECK(rig.view->cursorKey() == QString::fromLatin1(c.expected));
        }
    }
    SECTION("switching the tool with the pointer over the lanes updates the cursor at once") {
        hover(rig, {rig.x(2), v1});
        CHECK(rig.view->cursorKey().isEmpty());
        rig.view->setTool(QStringLiteral("razor"));
        CHECK(rig.view->cursorKey() == QStringLiteral("razor"));
        rig.view->setTool(QStringLiteral("select"));
        CHECK(rig.view->cursorKey().isEmpty());
    }
    SECTION("leaving the lanes gives the arrow back") {
        hover(rig, {rig.x(4) - 2, v1});
        REQUIRE_FALSE(rig.view->cursorKey().isEmpty());
        QHoverEvent leave(QEvent::HoverLeave, QPointF(-1, -1), QPointF(-1, -1), QPointF(rig.x(4) - 2, v1));
        QCoreApplication::sendEvent(rig.view, &leave);
        CHECK(rig.view->cursorKey().isEmpty());
    }
}

TEST_CASE("Edit cursors are Boxicons glyphs with a white outline at the screen's pixel ratio", "[app][cursors]") {
    const QStringList keys = editcursors::keys();
    REQUIRE(keys.size() == 26); // 0.4: "rearrange" (Ctrl+Alt-drag)
    // A contact sheet for review: every cursor on a dark and a light background, at 1x and 2x.
    QImage sheet(keys.size() * 72, 4 * 72, QImage::Format_ARGB32_Premultiplied);
    sheet.fill(QColor(0x18, 0x16, 0x20));
    QPainter painter(&sheet);
    painter.fillRect(QRect(0, 2 * 72, sheet.width(), 2 * 72), QColor(0xF2, 0xF0, 0xF6));
    for (int i = 0; i < keys.size(); ++i) {
        const QString& key = keys[i];
        INFO(key.toStdString());
        CHECK(editcursors::known(key));
        const QImage one = editcursors::image(key, 1.0);
        REQUIRE_FALSE(one.isNull());
        CHECK(one.size() == QSize(32, 32));
        const QImage two = editcursors::image(key, 2.0);
        CHECK(two.size() == QSize(64, 64));
        CHECK(two.devicePixelRatio() == 2.0);
        const QPoint hot = editcursors::hotSpot(key);
        CHECK(QRect(0, 0, 32, 32).contains(hot));
        // Ink (the glyph or its tint) inside a white outline: both kinds of opaque pixels are there.
        int white = 0, coloured = 0;
        for (int y = 0; y < one.height(); ++y) {
            for (int x = 0; x < one.width(); ++x) {
                const QColor c = one.pixelColor(x, y);
                if (c.alpha() < 250)
                    continue;
                if (c.red() > 240 && c.green() > 240 && c.blue() > 240)
                    ++white;
                else
                    ++coloured;
            }
        }
        CHECK(white > 20);
        CHECK(coloured > 20);
        CHECK(editcursors::cursor(key, 1.0).shape() == Qt::BitmapCursor);
        for (int row = 0; row < 4; ++row) {
            const QImage& img = row % 2 == 0 ? one : two;
            painter.drawImage(QPointF(i * 72 + 20, row * 72 + 20), img);
        }
    }
    painter.end();
    sheet.save(QDir(outputDir()).filePath(QStringLiteral("edit-cursors.png")));
    // Trim cursors take the trim colours; unknown keys are the arrow.
    const EditCursorColors colours{QColor(255, 0, 0), QColor(0, 255, 0)};
    const QImage trim = editcursors::image(QStringLiteral("trimHead"), 1.0, colours);
    bool red = false;
    for (int y = 0; y < 32 && !red; ++y) {
        for (int x = 0; x < 32 && !red; ++x)
            red = trim.pixelColor(x, y) == QColor(255, 0, 0);
    }
    CHECK(red);
    CHECK(editcursors::image(QStringLiteral("nope"), 1.0).isNull());
    CHECK(editcursors::cursor(QStringLiteral("nope"), 1.0).shape() == Qt::ArrowCursor);
    CHECK(editcursors::cursor(QString(), 1.0).shape() == Qt::ArrowCursor);
}

// ---- The Tracks toolbar in the shell --------------------------------------------------------------------------------

TEST_CASE("Tracks toolbar: tool groups with flyouts, modes apart from the tools, drops snap in the shell",
          "[app][timeline][toolbar][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* panel = shell.item("timelinePanel");
    REQUIRE(panel);
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    QQuickItem* root = shell.window->contentItem();

    // The palette: one button per group, all shown; the active tool's group is checked.
    QQuickItem* palette = findNamed(root, QStringLiteral("timelineToolPalette"));
    REQUIRE(palette);
    CHECK(palette->isVisible());
    for (const char* group : {"select", "track", "ripple", "razor", "slip", "pen", "hand", "type"}) {
        INFO(group);
        QQuickItem* button = findNamed(palette, QStringLiteral("toolGroup_") + QString::fromLatin1(group));
        REQUIRE(button);
        CHECK(button->isVisible());
        CHECK(button->property("checked").toBool() == (QString::fromLatin1(group) == QStringLiteral("select")));
    }

    // R (Rate Stretch) through the keymap: its group shows it and is checked.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("tool.rateStretch"))));
    CHECK(panel->property("tool").toString() == QStringLiteral("rateStretch"));
    QQuickItem* ripple = findNamed(palette, QStringLiteral("toolGroup_ripple"));
    REQUIRE(ripple);
    CHECK(ripple->property("iconName").toString() == QStringLiteral("gauge"));
    CHECK(ripple->property("checked").toBool());
    CHECK(view->tool() == QStringLiteral("rateStretch"));

    // Its flyout lists the group's tools; picking Rolling Edit makes it the tool and the group's face.
    REQUIRE(QMetaObject::invokeMethod(ripple, "openFlyout"));
    QObject* flyout = findObject(ripple, QStringLiteral("toolFlyout_ripple"));
    REQUIRE(flyout);
    REQUIRE(waitFor([&] { return flyout->property("opened").toBool(); }, 5000));
    QObject* roll = findObject(flyout, QStringLiteral("toolFlyoutItem_roll"));
    REQUIRE(roll);
    CHECK_FALSE(roll->property("checked").toBool());
    REQUIRE(QMetaObject::invokeMethod(roll, "triggered"));
    CHECK(panel->property("tool").toString() == QStringLiteral("roll"));
    CHECK(ripple->property("iconName").toString() == QStringLiteral("unfold-horizontal"));
    QMetaObject::invokeMethod(flyout, "close");
    // Back to V: the ripple group keeps Roll as its face.
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("tool.selection"))));
    CHECK_FALSE(ripple->property("checked").toBool());
    CHECK(ripple->property("iconName").toString() == QStringLiteral("unfold-horizontal"));

    // The modes are labelled toggles beside the sequence tabs, not in the tool palette.
    QQuickItem* snap = findNamed(root, QStringLiteral("snapToggle"));
    QQuickItem* linked = findNamed(root, QStringLiteral("linkedSelectionButton"));
    REQUIRE(snap);
    REQUIRE(linked);
    CHECK_FALSE(findNamed(palette, QStringLiteral("snapToggle")));
    CHECK(snap->property("text").toString() == QStringLiteral("Snap"));
    CHECK(snap->property("showLabel").toBool());
    CHECK(snap->property("checked").toBool() == panel->property("snapping").toBool());
    const bool snapping = panel->property("snapping").toBool();
    REQUIRE(QMetaObject::invokeMethod(snap, "toggle"));
    REQUIRE(QMetaObject::invokeMethod(snap, "toggled"));
    CHECK(panel->property("snapping").toBool() == !snapping);
    CHECK(view->snapping() == !snapping);
    panel->setProperty("snapping", true);
    CHECK(snap->property("checked").toBool());

    // The hand and zoom tools' cursors come from EditCursors: an open hand, a magnifier.
    QQuickItem* toolCursor = findNamed(root, QStringLiteral("timelineViewToolCursor"));
    REQUIRE(toolCursor);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("tool.hand"))));
    CHECK(toolCursor->property("shape").toString() == QStringLiteral("hand"));
    CHECK(toolCursor->cursor().shape() == Qt::BitmapCursor);
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("tool.zoom"))));
    CHECK(toolCursor->property("shape").toString() == QStringLiteral("zoomIn"));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "runCommand", Q_ARG(QVariant, QStringLiteral("tool.selection"))));

    // A drag from the Library over the lanes: the panel's trackDrop (what Main's drop area calls) snaps to the end of
    // the last clip on a video track and shows the insertion line; endDrop clears it.
    const auto& rows = view->rows();
    REQUIRE_FALSE(rows.empty());
    const Track* track = nullptr;
    int rowIndex = -1;
    for (std::size_t i = 0; i < rows.size() && !track; ++i) {
        if (!rows[i].audio && !rows[i].track->clips.empty()) {
            track = rows[i].track.get();
            rowIndex = static_cast<int>(i);
        }
    }
    REQUIRE(track);
    const Ticks end = track->clips.back().end();
    view->setScrollX(std::max(0.0, end.toSeconds() * view->pixelsPerSecond() - view->width() / 2));
    const double endX = end.toSeconds() * view->pixelsPerSecond() - view->scrollX();
    const double rowY = rows[static_cast<std::size_t>(rowIndex)].y + rows[static_cast<std::size_t>(rowIndex)].height / 2 - view->scrollY();
    const QPointF inPanel = view->mapToItem(panel, QPointF(endX + 4, rowY));
    QVariant result;
    REQUIRE(QMetaObject::invokeMethod(panel, "trackDrop", Q_RETURN_ARG(QVariant, result),
                                      Q_ARG(QVariant, QVariant::fromValue<QObject*>(panel)), Q_ARG(QVariant, inPanel.x()),
                                      Q_ARG(QVariant, inPanel.y())));
    const QVariantMap at = result.toMap();
    CHECK(at.value(QStringLiteral("snapped")).toBool());
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    CHECK(at.value(QStringLiteral("time")).toString() == controller->formatTimecode(end));
    CHECK(at.value(QStringLiteral("track")).toString() == trackKey(track->id));
    CHECK_FALSE(view->dropIndicator().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(panel, "endDrop", Q_ARG(QVariant, QVariant()))); // the drag left
    CHECK(view->dropIndicator().isEmpty());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Files dragged from File Explorer onto a track are imported and edited in there, cutting nothing below",
          "[app][timeline][drop][qml]") {
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    // A picture and a second of sound already in the project, and a new picture: files as Explorer drags them.
    const QString still = dir.filePath(QStringLiteral("Still.png"));
    const QString title = dir.filePath(QStringLiteral("Title card.png"));
    const QString tone = dir.filePath(QStringLiteral("Tone.wav"));
    {
        QImage picture(64, 36, QImage::Format_RGB32);
        picture.fill(Qt::darkCyan);
        REQUIRE(picture.save(still));
        picture.fill(Qt::darkMagenta);
        REQUIRE(picture.save(title));
    }
    REQUIRE(writeWav(tone, 1.0));

    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(file);
    REQUIRE(controller);
    const QVariantMap imported = file->importFiles({QUrl::fromLocalFile(still), QUrl::fromLocalFile(tone)});
    REQUIRE(imported.value(QStringLiteral("added")).toInt() == 2);
    MediaId stillId;
    MediaId toneId;
    for (const MediaPtr& m : controller->snapshot()->media) {
        if (m->name == "Still.png")
            stillId = m->id;
        if (m->name == "Tone.wav")
            toneId = m->id;
    }
    REQUIRE(stillId);
    REQUIRE(toneId);
    SequencePtr seq = controller->currentSequence();
    REQUIRE(seq);
    REQUIRE(seq->videoTracks.size() >= 2);
    REQUIRE(seq->audioTracks.size() >= 2);
    const TrackId v1 = seq->videoTracks[0]->id;
    const TrackId v2 = seq->videoTracks[1]->id;
    const TrackId a1 = seq->audioTracks[0]->id;
    const TrackId a2 = seq->audioTracks[1]->id;
    // The layer below: the picture on V1 from 0 (5 s, a still's length) and the sound on A1 from 0.
    REQUIRE(file->dropMedia(mediaKey(stillId), QStringLiteral("00:00:00:00"), trackKey(v1), true));
    REQUIRE(file->dropMedia(mediaKey(toneId), QStringLiteral("00:00:00:00"), trackKey(a1), true));
    REQUIRE_FALSE(shell.grab().isNull());

    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    view->setSnapping(false);
    // The drag over V2 at 2 s, then the drop: the new picture and the sound (already in the project), in that order.
    const auto row = std::ranges::find_if(view->rows(), [&](const TimelineView::Row& r) { return r.track->id == v2; });
    REQUIRE(row != view->rows().end());
    const QPointF local(2.0 * view->pixelsPerSecond() - view->scrollX() + 1, row->y - view->scrollY() + row->height / 2);
    const QVariantMap landing = view->dropPoint(local.x(), local.y());
    CHECK(landing.value(QStringLiteral("track")).toString() == trackKey(v2));
    const auto at = parseTimecode(landing.value(QStringLiteral("time")).toString().toStdString(), seq->frameRate, false);
    REQUIRE(at);
    const QPoint p = view->mapToScene(local).toPoint();
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(title), QUrl::fromLocalFile(tone)});
    QDragEnterEvent enter(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    CHECK(enter.isAccepted());
    QDragMoveEvent move(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &move);
    CHECK_FALSE(view->dropIndicator().isEmpty()); // the insertion line shows where it lands
    QDropEvent drop(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &drop);
    spin(20);
    CHECK(view->dropIndicator().isEmpty());

    seq = controller->currentSequence();
    MediaId titleId;
    for (const MediaPtr& m : controller->snapshot()->media) {
        if (m->name == "Title card.png")
            titleId = m->id;
    }
    REQUIRE(titleId); // imported
    const double start = at->toSeconds();
    // The new picture on V2 where it was dropped, the sound after it on A2 (the audio track matching V2).
    CHECK(clipsOn(*seq->findTrack(v2)) == std::vector{std::tuple{start, start + 5.0, titleId}});
    CHECK(clipsOn(*seq->findTrack(a2)) == std::vector{std::tuple{start + 5.0, start + 6.0, toneId}});
    // Nothing below was cut or moved: the drop overwrote only where it landed (an insert used to split V1 and A1 at
    // 2 s and push their tails along).
    CHECK(clipsOn(*seq->findTrack(v1)) == std::vector{std::tuple{0.0, 5.0, stillId}});
    CHECK(clipsOn(*seq->findTrack(a1)) == std::vector{std::tuple{0.0, 1.0, toneId}});

    // The Library cards of the stills cannot show their thumbnails here (QQuickImage reports "No decoder for png" for
    // still thumbnails in this test run); that is the Library's, not the drop's.
    QStringList messages = takeMessages();
    messages.removeIf([](const QString& m) { return m.contains(QStringLiteral("No decoder for png")); });
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("A drag held over the ruler or below the tracks adds a track there and lands on it",
          "[app][timeline][drop][qml]") {
    // 2026-10-15 request: a picture held above V2 for two seconds adds V3 for the drop (below the last audio track, an
    // audio track). A track the drop does not use goes again; a drag passing by adds nothing.
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString still = dir.filePath(QStringLiteral("Portrait.png"));
    {
        QImage picture(64, 36, QImage::Format_RGB32);
        picture.fill(Qt::darkCyan);
        REQUIRE(picture.save(still));
    }
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(file);
    REQUIRE(controller);
    REQUIRE(file->importFiles({QUrl::fromLocalFile(still)}).value(QStringLiteral("added")).toInt() == 1);
    MediaId stillId;
    for (const MediaPtr& m : controller->snapshot()->media) {
        if (m->name == "Portrait.png")
            stillId = m->id;
    }
    REQUIRE(stillId);
    REQUIRE_FALSE(shell.grab().isNull());

    QQuickItem* panel = shell.item("timelinePanel");
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(panel);
    REQUIRE(view);
    QQuickItem* band = findNamed(shell.window->contentItem(), QStringLiteral("newTrackHoldBand"));
    QQuickItem* bandLabel = findNamed(shell.window->contentItem(), QStringLiteral("newTrackHoldLabel"));
    REQUIRE(band);
    REQUIRE(bandLabel);
    panel->setProperty("newTrackHoldMs", 150);
    view->setSnapping(false);

    SequencePtr seq = controller->currentSequence();
    REQUIRE(seq);
    const std::size_t videoTracks = seq->videoTracks.size();
    const std::size_t audioTracks = seq->audioTracks.size();
    REQUIRE(videoTracks >= 1);
    std::vector<std::size_t> clipsBefore;
    seq->forEachTrack([&](const Track& t) { clipsBefore.push_back(t.clips.size()); });
    const std::size_t undoBefore = controller->session().undoCount();

    QMimeData mime;
    mime.setData(QStringLiteral("application/x-wabcut-media"), mediaKey(stillId).toUtf8());
    const auto drag = [&](QEvent::Type type, QPointF local) {
        const QPoint p = view->mapToScene(local).toPoint();
        if (type == QEvent::DragEnter) {
            QDragEnterEvent event(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
            QCoreApplication::sendEvent(shell.window.get(), &event);
        } else if (type == QEvent::DragMove) {
            QDragMoveEvent event(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
            QCoreApplication::sendEvent(shell.window.get(), &event);
        } else {
            QDropEvent event(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
            QCoreApplication::sendEvent(shell.window.get(), &event);
        }
        spin(5);
    };
    const auto leave = [&] {
        QDragLeaveEvent event;
        QCoreApplication::sendEvent(shell.window.get(), &event);
        spin(5);
    };
    const double atX = 2.0 * view->pixelsPerSecond() - view->scrollX() + 1;
    // Over the ruler, just above the lanes (the ruler sits a small gap above them).
    const QPointF overRuler(atX, -12);

    // Held over the ruler: the band names the track it will add, then V(n+1) is there and the drop lands on it.
    drag(QEvent::DragEnter, overRuler);
    CHECK(band->isVisible());
    CHECK(bandLabel->property("text").toString() == QStringLiteral("Hold to add track V%1").arg(videoTracks + 1));
    CHECK(controller->currentSequence()->videoTracks.size() == videoTracks);
    CHECK(view->dropIndicator().isEmpty()); // nothing to land on yet
    REQUIRE(waitFor([&] { return controller->currentSequence()->videoTracks.size() == videoTracks + 1; }, 5000));
    seq = controller->currentSequence();
    const TrackId added = seq->videoTracks.back()->id; // the top one
    CHECK_FALSE(band->isVisible());
    const QVariantMap landing = view->dropIndicator();
    CHECK(landing.value(QStringLiteral("track")).toString() == trackKey(added));
    const auto landsAt = parseTimecode(landing.value(QStringLiteral("time")).toString().toStdString(), seq->frameRate,
                                       seq->dropFrame);
    REQUIRE(landsAt);
    CHECK(std::abs(landsAt->toSeconds() - 2.0) < 0.1);
    CHECK(view->scrollY() == 0.0);
    drag(QEvent::Drop, overRuler);
    spin(20);
    seq = controller->currentSequence();
    REQUIRE(seq->videoTracks.size() == videoTracks + 1);
    const Track* top = seq->findTrack(added);
    REQUIRE(top);
    REQUIRE(top->clips.size() == 1);
    CHECK(top->clips[0].media == stillId);
    CHECK(top->clips[0].start() == *landsAt);
    // Nothing else moved: the other tracks keep their clips (the picture has no sound).
    std::vector<std::size_t> clipsAfter;
    seq->forEachTrack([&](const Track& t) {
        if (t.id != added)
            clipsAfter.push_back(t.clips.size());
    });
    CHECK(clipsAfter == clipsBefore);
    CHECK(controller->session().undoCount() == undoBefore + 2); // Add Track, then the drop
    CHECK(view->dropIndicator().isEmpty());

    // Held below the last track: A(m+1) appears for the drop; the drag leaves instead, and the track goes again, its
    // step undone.
    const double contentBottom = view->contentHeight() - view->scrollY();
    const QPointF belowTracks(atX, contentBottom + 6 < view->height() ? contentBottom + 6 : view->height() + 4);
    drag(QEvent::DragEnter, belowTracks);
    CHECK(band->isVisible());
    CHECK(bandLabel->property("text").toString() == QStringLiteral("Hold to add track A%1").arg(audioTracks + 1));
    REQUIRE(waitFor([&] { return controller->currentSequence()->audioTracks.size() == audioTracks + 1; }, 5000));
    seq = controller->currentSequence();
    CHECK(view->dropIndicator().value(QStringLiteral("track")).toString() == trackKey(seq->audioTracks.back()->id));
    leave();
    seq = controller->currentSequence();
    CHECK(seq->audioTracks.size() == audioTracks);
    CHECK(seq->videoTracks.size() == videoTracks + 1);
    CHECK(controller->session().undoCount() == undoBefore + 2);
    CHECK_FALSE(band->isVisible());

    // A drag that only passes over the ruler and drops on a track adds nothing.
    panel->setProperty("newTrackHoldMs", 60000);
    drag(QEvent::DragEnter, overRuler);
    CHECK(band->isVisible());
    const auto row = std::ranges::find_if(view->rows(), [](const TimelineView::Row& r) { return !r.audio; });
    REQUIRE(row != view->rows().end());
    const QPointF onTrack(4.0 * view->pixelsPerSecond() - view->scrollX() + 1, row->y - view->scrollY() + row->height / 2);
    drag(QEvent::DragMove, onTrack);
    CHECK_FALSE(band->isVisible());
    drag(QEvent::Drop, onTrack);
    spin(20);
    seq = controller->currentSequence();
    CHECK(seq->videoTracks.size() == videoTracks + 1);
    CHECK(seq->audioTracks.size() == audioTracks);
    CHECK(controller->session().undoCount() == undoBefore + 3); // just the drop

    QStringList messages = takeMessages();
    messages.removeIf([](const QString& m) { return m.contains(QStringLiteral("No decoder for png")); });
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("A sequence dragged from the Project panel onto a track is nested there", "[app][timeline][drop][nested][qml]") {
    // 2026-10-15 bug: Project panel sequences carried no timeline data, so the lanes refused them.
    resetSettings();
    clearMessages();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString still = dir.filePath(QStringLiteral("Card.png"));
    {
        QImage picture(64, 36, QImage::Format_RGB32);
        picture.fill(Qt::darkMagenta);
        REQUIRE(picture.save(still));
    }
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    auto* file = shell.engine.singletonInstance<ProjectLifecycle*>("Wabcut", "ProjectFile");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(file);
    REQUIRE(controller);
    REQUIRE(file->importFiles({QUrl::fromLocalFile(still)}).value(QStringLiteral("added")).toInt() == 1);
    MediaId stillId;
    for (const MediaPtr& m : controller->snapshot()->media) {
        if (m->name == "Card.png")
            stillId = m->id;
    }
    REQUIRE(stillId);
    SequencePtr seq = controller->currentSequence();
    REQUIRE(seq);
    REQUIRE(seq->videoTracks.size() >= 2);
    const TrackId v2 = seq->videoTracks[1]->id;

    // A second sequence holding the picture: the one to nest. The timeline keeps showing the first.
    EditSession& session = controller->session();
    SequenceId nestedId;
    REQUIRE(session.apply("New Sequence", [&](const ProjectState& state) {
        auto created = edit::addSequence(state, edit::SequenceSettings{});
        if (created)
            nestedId = created->id;
        return created;
    }));
    REQUIRE(editor::dropMedia(session, {.media = stillId, .sequence = nestedId, .at = Ticks{}}));
    const Ticks length = controller->snapshot()->findSequence(nestedId)->duration;
    REQUIRE(length > Ticks{});
    REQUIRE(controller->currentSequence()->id == seq->id);
    REQUIRE_FALSE(shell.grab().isNull());

    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    view->setSnapping(false);
    const auto row = std::ranges::find_if(view->rows(), [&](const TimelineView::Row& r) { return r.track->id == v2; });
    REQUIRE(row != view->rows().end());
    const QPointF local(1.0 * view->pixelsPerSecond() - view->scrollX() + 1, row->y - view->scrollY() + row->height / 2);
    const QString landing = view->dropPoint(local.x(), local.y()).value(QStringLiteral("time")).toString();
    const auto at = parseTimecode(landing.toStdString(), seq->frameRate, seq->dropFrame);
    REQUIRE(at);

    // What a Project panel sequence carries: its key as the timeline's media data.
    QMimeData mime;
    mime.setData(QStringLiteral("application/x-wabcut-library"), sequenceKey(nestedId).toUtf8());
    mime.setData(QStringLiteral("application/x-wabcut-media"), sequenceKey(nestedId).toUtf8());
    const QPoint p = view->mapToScene(local).toPoint();
    QDragEnterEvent enter(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &enter);
    CHECK(enter.isAccepted());
    QDragMoveEvent move(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &move);
    CHECK(view->dropIndicator().value(QStringLiteral("track")).toString() == trackKey(v2));
    QDropEvent drop(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
    QCoreApplication::sendEvent(shell.window.get(), &drop);
    spin(20);

    // A nested clip on V2 from the drop, the whole sequence long, its mix linked on an audio track.
    seq = controller->currentSequence();
    const Track* onV2 = seq->findTrack(v2);
    REQUIRE(onV2);
    REQUIRE(onV2->clips.size() == 1);
    const Clip& picture = onV2->clips[0];
    CHECK(picture.sequence == nestedId);
    CHECK(picture.start() == *at);
    CHECK(picture.end() == *at + length);
    int mixes = 0;
    for (const TrackPtr& t : seq->audioTracks) {
        for (const Clip& c : t->clips) {
            if (c.sequence == nestedId) {
                ++mixes;
                CHECK(c.linkGroup == picture.linkGroup);
            }
        }
    }
    CHECK(mixes == 1);

    // The sequence itself cannot go into its own timeline: refused with a reason, nothing changes.
    const std::size_t steps = session.undoCount();
    CHECK_FALSE(file->dropMedia(sequenceKey(seq->id), QStringLiteral("00:00:00:00"), trackKey(v2), true));
    CHECK_FALSE(file->lastError().isEmpty());
    CHECK(session.undoCount() == steps);

    QStringList messages = takeMessages();
    messages.removeIf([](const QString& m) { return m.contains(QStringLiteral("No decoder for png")); });
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The window shows the lanes' own cursor; the hand and zoom cursors only with their tools",
          "[app][timeline][cursors][qml]") {
    // 0.1.3 bug: over the lanes the pointer was always the zoom magnifier. Qt Quick's cursor lookup does not skip
    // disabled items, and the disabled hand/zoom overlay over the lanes kept a cursor: its CursorArea's magnifier,
    // and under that the arrow every MouseArea sets for itself. The items' own cursors were right; what the window
    // showed was not.
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickWindow* window = shell.window.get();
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    QQuickItem* toolCursor = findNamed(window->contentItem(), QStringLiteral("timelineViewToolCursor"));
    REQUIRE(toolCursor);

    // A clip edge and an empty stretch of a lane, in scene coordinates.
    const auto& rows = view->rows();
    const Track* track = nullptr;
    int rowIndex = -1;
    for (std::size_t i = 0; i < rows.size() && !track; ++i) {
        if (!rows[i].audio && !rows[i].track->clips.empty()) {
            track = rows[i].track.get();
            rowIndex = static_cast<int>(i);
        }
    }
    REQUIRE(track);
    const auto& row = rows[static_cast<std::size_t>(rowIndex)];
    const double y = row.y + row.height / 2 - view->scrollY();
    const double tail = track->clips.front().end().toSeconds() * view->pixelsPerSecond() - view->scrollX();
    REQUIRE(tail > 4);
    REQUIRE(tail < view->width());
    const QPointF edge = view->mapToScene(QPointF(tail - 2, y));
    // An empty stretch of a lane: the first point, from the right (clear of the vertical scroll bar over the lanes'
    // right end), with no clip under it.
    QPointF empty(-1, -1);
    for (const auto& r : rows) {
        const double ry = r.y + r.height / 2 - view->scrollY();
        for (double x = view->width() - 40; x > 4 && empty.x() < 0; x -= 8) {
            if (view->clipAt(x, ry).isEmpty() && view->clipAt(x - 12, ry).isEmpty() && view->clipAt(x + 12, ry).isEmpty()
                && !view->trackAt(ry).isEmpty()) // clear of every clip edge's reach too
                empty = view->mapToScene(QPointF(x, ry));
        }
        if (empty.x() >= 0)
            break;
    }
    REQUIRE(empty.x() >= 0);

    const auto hoverAt = [&](QPointF scene) {
        // Through the window system, as the platform delivers a pointer move (the cursor position Qt Quick's cursor
        // updates read included); two moves, as a pointer arriving makes.
        for (const QPointF p : {scene + QPointF(1, 0), scene})
            QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(
                window, p, window->mapToGlobal(p), Qt::NoButton, Qt::NoButton, QEvent::MouseMove);
    };
    const auto sameCursor = [](const QCursor& a, const QCursor& b) {
        if (a.shape() != b.shape())
            return false;
        return a.shape() != Qt::BitmapCursor || a.pixmap().toImage() == b.pixmap().toImage();
    };
    // Which edit cursor a cursor is, for the failure messages.
    const auto describe = [&](const QCursor& c) {
        std::string text = "shape " + std::to_string(static_cast<int>(c.shape()));
        for (const QString& key : editcursors::keys()) {
            if (sameCursor(c, editcursors::cursor(key, window->effectiveDevicePixelRatio())))
                text += " (" + key.toStdString() + ")";
        }
        return text;
    };

    // Selection: the trim bracket on the edge, the arrow on the empty lane, what the view says.
    hoverAt(edge);
    INFO("window " << describe(window->cursor()) << ", view " << describe(view->cursor()));
    CHECK(view->cursorKey() == QStringLiteral("trimTail"));
    CHECK(sameCursor(window->cursor(), view->cursor()));
    CHECK_FALSE(sameCursor(window->cursor(), editcursors::cursor(QStringLiteral("zoomIn"), window->effectiveDevicePixelRatio())));
    hoverAt(empty);
    CHECK(view->cursorKey().isEmpty());
    CHECK(window->cursor().shape() == Qt::ArrowCursor);

    // Zoom and hand: the overlay's cursors.
    REQUIRE(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, QStringLiteral("tool.zoom"))));
    hoverAt(edge);
    CHECK(sameCursor(window->cursor(), editcursors::cursor(QStringLiteral("zoomIn"), window->effectiveDevicePixelRatio())));
    REQUIRE(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, QStringLiteral("tool.hand"))));
    hoverAt(empty);
    CHECK(sameCursor(window->cursor(), editcursors::cursor(QStringLiteral("hand"), window->effectiveDevicePixelRatio())));

    // Back to Selection, then the razor: the view's cursors again.
    REQUIRE(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, QStringLiteral("tool.selection"))));
    hoverAt(edge);
    CHECK(view->cursorKey() == QStringLiteral("trimTail"));
    CHECK(sameCursor(window->cursor(), view->cursor()));
    REQUIRE(QMetaObject::invokeMethod(window, "runCommand", Q_ARG(QVariant, QStringLiteral("tool.razor"))));
    hoverAt(empty);
    CHECK(view->cursorKey() == QStringLiteral("razor"));
    CHECK(sameCursor(window->cursor(), view->cursor()));

    // The lead's gutters and track-height handles keep their resize cursors.
    QQuickItem* splitter = findNamed(window->contentItem(), QStringLiteral("splitterCursor"));
    REQUIRE(splitter);
    CHECK(splitter->cursor().shape() == Qt::BitmapCursor);
    QList<QQuickItem*> handles;
    std::function<void(QQuickItem*)> collect = [&](QQuickItem* item) {
        if (item->objectName() == QStringLiteral("trackResizeHandle"))
            handles << item;
        for (QQuickItem* child : item->childItems())
            collect(child);
    };
    collect(window->contentItem());
    REQUIRE_FALSE(handles.isEmpty());
    const QPointF handle = handles.front()->mapToScene(QPointF(handles.front()->width() / 2, handles.front()->height() / 2));
    hoverAt(handle);
    CHECK(sameCursor(window->cursor(), editcursors::cursor(QStringLiteral("resizeV"), window->effectiveDevicePixelRatio())));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Tracks toolbar on a narrow panel: labels go, then the view toggles fold, the palette folds, nothing vanishes",
          "[app][timeline][toolbar][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* panel = shell.item("timelinePanel");
    REQUIRE(panel);
    QQuickItem* root = shell.window->contentItem();
    QQuickItem* palette = findNamed(root, QStringLiteral("timelineToolPalette"));
    QQuickItem* snap = findNamed(root, QStringLiteral("snapToggle"));
    QQuickItem* volume = findNamed(root, QStringLiteral("volumeKeysToggle"));
    QQuickItem* viewMenu = findNamed(root, QStringLiteral("timelineViewMenuButton"));
    REQUIRE(palette);
    REQUIRE(snap);
    REQUIRE(volume);
    REQUIRE(viewMenu);

    // Free the panel from its dock's anchors to give it the widths a narrow dock or window would.
    QQmlProperty(panel, QStringLiteral("anchors.fill")).write(QVariant::fromValue<QQuickItem*>(nullptr));
    // A grab after each width: a hidden window lays out its Rows (the palette) only when it renders.
    const auto at = [&](double width) {
        panel->setWidth(width);
        spin(30);
        REQUIRE_FALSE(shell.grab().isNull());
    };
    at(1200);
    CHECK(snap->property("showLabel").toBool());
    CHECK(volume->isVisible());
    CHECK_FALSE(viewMenu->isVisible());
    CHECK(findNamed(palette, QStringLiteral("toolGroup_ripple")));

    at(560); // no room for the labels
    CHECK_FALSE(snap->property("showLabel").toBool());
    CHECK(volume->isVisible());

    at(452); // the view toggles fold into a menu; the palette folds into one button with the active tool
    CHECK_FALSE(volume->isVisible());
    CHECK(viewMenu->isVisible());
    CHECK(palette->isVisible());
    QQuickItem* all = findNamed(palette, QStringLiteral("toolGroup_all"));
    REQUIRE(all);
    CHECK(all->property("iconName").toString() == QStringLiteral("mouse-pointer-2"));
    CHECK_FALSE(findNamed(palette, QStringLiteral("toolGroup_ripple")));
    // The tool bar row never runs into the tabs' side: it keeps inside the panel.
    QQuickItem* toolbar = findNamed(root, QStringLiteral("timelineToolbar"));
    REQUIRE(toolbar);
    CHECK(toolbar->x() >= 0);

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

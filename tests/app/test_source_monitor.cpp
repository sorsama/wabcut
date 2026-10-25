// M5h: the Source Monitor and Premiere-style three-/four-point editing. Dual monitors in the Editing workspace and
// the narrow fallback; the Source Monitor's keymap scope (I/O/X, Shift+I/O, Ctrl+Shift+I/O/X act on the source);
// source marks stored on the media (undo, saved and reopened); source patching and track targeting in the track
// headers; Insert/Overwrite with three-point placement and each Fit Clip choice; the ghost preview; match frame and
// reverse match frame; drags of video only and audio only; Export Frame against the offscreen render. Offscreen;
// no window is shown.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "KeymapDispatcher.h"
#include "ProgramToolsBridge.h"
#include "ProjectController.h"
#include "SourceMonitorBridge.h"
#include "TimelineView.h"

#include <wabcut/command/Engine.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/keymap/Key.h>
#include <wabcut/model/ProjectIO.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QMouseEvent>
#include <QQuickItem>
#include <QTemporaryDir>
#include <QUrl>

#include <cstring>
#include <functional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// "00:00:01:04" at the fixtures' 25 fps.
QString tc(int frames) {
    return QStringLiteral("00:00:%1:%2").arg(frames / 25, 2, 10, QLatin1Char('0')).arg(frames % 25, 2, 10, QLatin1Char('0'));
}

// 12 frames of A on V1/A1 (linked), empty V2 and A2; S is a 50-frame source with video and two audio streams.
constexpr const char* kPicture = R"(
    V2     |............|
    V1 tgt |Aaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaa|
    A2     |............|
)";
const std::map<char, std::int64_t> kMedia{{'S', 50}, {'T', 10}};

struct Fixture {
    Timeline tl{kPicture, kMedia};
    ProjectController controller;
    SourceMonitorBridge source;

    Fixture() {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        source.setController(&controller);
    }
    QString media(char letter) const { return mediaKey(tl.media(letter)); }
    QString track(const char* label) const { return trackKey(tl.track(label)); }
    const MediaItem& item(char letter) const { return *controller.snapshot()->findMedia(tl.media(letter)); }
    std::string picture() const { return wabcut::test::render(*controller.snapshot(), tl.seq, kPicture); }
    /// Marks the loaded source [in, out) by moving there and pressing I and O (the frame before `out`).
    void markSource(int in, int out) {
        source.seek(f(in));
        source.markIn();
        source.seek(f(out - 1));
        source.markOut();
    }
    void markSequence(int in, int lastFrame) {
        controller.setPlayhead(f(in));
        controller.markInAtPlayhead();
        controller.setPlayhead(f(lastFrame));
        controller.markOutAtPlayhead();
    }
};

QQuickItem* findItem(QQuickItem* root, const std::function<bool(QQuickItem*)>& match) {
    if (!root)
        return nullptr;
    if (match(root))
        return root;
    for (QQuickItem* child : root->childItems()) {
        if (QQuickItem* found = findItem(child, match))
            return found;
    }
    return nullptr;
}

QQuickItem* named(QQuickItem* root, const QString& name) {
    return findItem(root, [&](QQuickItem* i) { return i->objectName() == name; });
}

void mouse(QQuickWindow* window, QEvent::Type type, QPointF p, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = {}) {
    QMouseEvent event(type, p, p, type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QCoreApplication::sendEvent(window, &event);
}

void click(QQuickWindow* window, QQuickItem* item, Qt::KeyboardModifiers mods = {}) {
    const QPointF p = item->mapToScene(QPointF(item->width() / 2, item->height() / 2));
    mouse(window, QEvent::MouseButtonPress, p, Qt::LeftButton, mods);
    mouse(window, QEvent::MouseButtonRelease, p, Qt::NoButton, mods);
}

void tap(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

bool isMac() {
    return QGuiApplication::platformName() == QLatin1String("cocoa");
}

/// Clips of `media` on video / audio tracks of the current sequence.
std::pair<int, int> clipsOf(const ProjectController& controller, MediaId media) {
    int video = 0;
    int audio = 0;
    controller.currentSequence()->forEachTrack([&](const Track& t) {
        for (const Clip& c : t.clips) {
            if (c.media == media)
                ++(t.kind == TrackKind::Video ? video : audio);
        }
    });
    return {video, audio};
}

/// The track header row of a track key ("t3").
QQuickItem* headerOf(QQuickItem* root, const QString& track) {
    return findItem(root, [&](QQuickItem* i) {
        return i->property("trackId").isValid() && i->property("trackId").toString() == track &&
               i->property("shortName").isValid();
    });
}

} // namespace

// ---- Marks ------------------------------------------------------------------------------------------------------

TEST_CASE("Source marks: set and cleared as undoable commands, stored on the media, kept when the project is reopened",
          "[app][source]") {
    Fixture fx;
    REQUIRE(fx.source.load(fx.media('S')));
    CHECK(fx.source.loaded());
    CHECK(fx.source.name() == QStringLiteral("S"));
    CHECK(fx.source.kind() == QStringLiteral("video"));
    CHECK(fx.source.durationText() == tc(50));
    CHECK(fx.source.positionText() == tc(0));
    CHECK(fx.source.inOutText() == tc(50)); // the whole source without marks

    fx.markSource(10, 20);
    CHECK(fx.item('S').markIn == f(10));
    CHECK(fx.item('S').markOut == f(20));
    CHECK(fx.source.inText() == tc(10));
    CHECK(fx.source.outText() == tc(19)); // the last marked frame, like the sequence Out
    CHECK(fx.source.inOutText() == tc(10));
    CHECK(fx.source.inFraction() == 0.2);
    CHECK(fx.source.outFraction() == 0.4);
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Set Source Marks"));

    // Go to In / Out, then the clears; each is one undo step.
    fx.source.goToIn();
    CHECK(fx.source.positionText() == tc(10));
    fx.source.goToOut();
    CHECK(fx.source.positionText() == tc(19));
    const std::size_t steps = fx.controller.session().undoCount();
    fx.source.clearIn();
    CHECK_FALSE(fx.item('S').markIn);
    CHECK(fx.item('S').markOut == f(20));
    fx.controller.undo();
    CHECK(fx.item('S').markIn == f(10));
    fx.source.clearOut();
    CHECK_FALSE(fx.item('S').markOut);
    fx.source.clearInOut();
    CHECK_FALSE(fx.item('S').markIn);
    CHECK(fx.controller.session().undoCount() == steps + 2);
    fx.source.markClip(); // X: the whole source
    CHECK(fx.item('S').markIn == f(0));
    CHECK(fx.item('S').markOut == f(50));
    fx.markSource(10, 20);

    // Saved and reopened: the marks come back with the media, and the source opens at its In.
    const auto json = io::toJson(*fx.controller.snapshot());
    REQUIRE(json);
    const auto reopened = io::fromJson(*json);
    REQUIRE(reopened);
    ProjectController controller;
    controller.resetProject(*reopened, fx.tl.seq);
    SourceMonitorBridge source;
    source.setController(&controller);
    REQUIRE(source.load(fx.media('S')));
    CHECK(source.inText() == tc(10));
    CHECK(source.outText() == tc(19));
    CHECK(source.positionText() == tc(10));
    CHECK(source.recent().size() == 1);
}

// ---- Three-point editing ------------------------------------------------------------------------------------

TEST_CASE("Insert and Overwrite from the Source Monitor follow Premiere's three-point rules", "[app][source][threepoint]") {
    SECTION("source In/Out at the playhead; the playhead parks after the edit") {
        Fixture fx;
        REQUIRE(fx.source.load(fx.media('S')));
        fx.markSource(4, 7);
        fx.controller.setPlayhead(f(2));
        CHECK(fx.source.edit(true).isEmpty());
        CHECK(fx.picture() == normalize("V2 |...............|\nV1 |AaS4sA2aaaaaaaa|\nA1 |AaS4sA2aaaaaaaa|\nA2 |..S4s..........|"));
        CHECK(fx.controller.playhead() == f(5));
        CHECK(fx.controller.undoText() == QStringLiteral("Undo Insert from Source"));
    }
    SECTION("source In with the sequence In/Out: the sequence sets the length (overwrite)") {
        Fixture fx;
        REQUIRE(fx.source.load(fx.media('S')));
        fx.source.seek(f(4));
        fx.source.markIn();
        fx.markSequence(3, 6);
        CHECK_FALSE(fx.source.needsFit());
        CHECK(fx.source.edit(false).isEmpty());
        CHECK(fx.picture() == normalize("V2 |............|\nV1 |AaaS4ssA7aaa|\nA1 |AaaS4ssA7aaa|\nA2 |...S4ss.....|"));
    }
    SECTION("four marks of different lengths need a Fit Clip choice") {
        Fixture fx;
        REQUIRE(fx.source.load(fx.media('S')));
        fx.markSource(4, 10);
        fx.markSequence(3, 6);
        CHECK(fx.source.needsFit());
        CHECK(fx.controller.markDurationText() == tc(4)); // 0.4: was SourceMonitor.sequenceInOutText
        const std::string before = fx.picture();
        CHECK(fx.source.edit(true) == QStringLiteral("fit"));
        CHECK(fx.picture() == before); // nothing happened yet
        CHECK(fx.source.edit(true, QStringLiteral("ignoreSourceIn")).isEmpty());
        CHECK(fx.picture() == normalize("V2 |................|\nV1 |AaaS6ssA3aaaaaaa|\nA1 |AaaS6ssA3aaaaaaa|\nA2 |...S6ss.........|"));
    }
}

// ---- Patching and targeting -----------------------------------------------------------------------------------

TEST_CASE("Source patching chooses the destination tracks; switched-off indicators stay out", "[app][source][patching]") {
    Fixture fx;
    REQUIRE(fx.source.load(fx.media('S')));
    // Premiere's default: V1, A1 and A2 to the tracks of the same names.
    QVariantMap map = fx.source.patchMap();
    CHECK(map.value(fx.track("V1")).toMap().value("source").toString() == QStringLiteral("V1"));
    CHECK(map.value(fx.track("A1")).toMap().value("source").toString() == QStringLiteral("A1"));
    CHECK(map.value(fx.track("A2")).toMap().value("source").toString() == QStringLiteral("A2"));
    CHECK(fx.source.indicators().size() == 3);
    CHECK(fx.source.canEdit());

    // Click an empty slot (V2), switch A1 off (Alt-click), drag A2 onto A1 (they swap).
    fx.source.patchTrack(fx.track("V2"));
    fx.source.togglePatch(QStringLiteral("A1"));
    map = fx.source.patchMap();
    CHECK(map.value(fx.track("V2")).toMap().value("source").toString() == QStringLiteral("V1"));
    CHECK_FALSE(map.contains(fx.track("V1")));
    CHECK_FALSE(map.value(fx.track("A1")).toMap().value("enabled").toBool());
    fx.source.movePatch(QStringLiteral("A2"), fx.track("A1"));
    map = fx.source.patchMap();
    CHECK(map.value(fx.track("A1")).toMap().value("source").toString() == QStringLiteral("A2"));
    CHECK(map.value(fx.track("A1")).toMap().value("enabled").toBool());
    CHECK(map.value(fx.track("A2")).toMap().value("source").toString() == QStringLiteral("A1"));
    CHECK_FALSE(map.value(fx.track("A2")).toMap().value("enabled").toBool());

    // The edit uses it: the picture to V2, audio stream 2 to A1, nothing on A2.
    fx.source.seek(f(0));
    fx.source.markIn();
    fx.source.seek(f(2));
    fx.source.markOut();
    fx.controller.setPlayhead(f(5));
    REQUIRE(fx.source.edit(false).isEmpty());
    const Sequence& seq = *fx.controller.currentSequence();
    CHECK(seq.findTrack(fx.tl.track("V2"))->clips.size() == 1);
    REQUIRE(seq.findTrack(fx.tl.track("A1"))->clipIndexAt(f(6)));
    const Clip& audio = seq.findTrack(fx.tl.track("A1"))->clips[*seq.findTrack(fx.tl.track("A1"))->clipIndexAt(f(6))];
    CHECK(audio.media == fx.tl.media('S'));
    CHECK(audio.stream == StreamRef{StreamKind::Audio, 1});
    CHECK(seq.findTrack(fx.tl.track("A2"))->clips.empty());

    // Targeting goes through sequence.setTargets: one undo step.
    fx.source.setTargeted(fx.track("V2"), true);
    fx.source.setTargeted(fx.track("A1"), false);
    CHECK(edit::targetedTracks(*fx.controller.currentSequence()) ==
          std::vector<TrackId>{fx.tl.track("V1"), fx.tl.track("V2")});
    CHECK(fx.controller.undoText() == QStringLiteral("Undo Set Track Targets"));
}

// ---- Match frame ----------------------------------------------------------------------------------------------

TEST_CASE("Match Frame loads the clip under the playhead with its range marked; Reverse Match Frame finds it again",
          "[app][source][matchframe]") {
    Timeline tl(R"(
        V1 tgt |AaaaS7ssss..|
        A1 tgt |AaaaS7ssss..|
    )",
                kMedia);
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SourceMonitorBridge source;
    source.setController(&controller);
    int shown = 0;
    QObject::connect(&source, &SourceMonitorBridge::showSourceRequested, [&] { ++shown; });

    controller.setPlayhead(f(6));
    REQUIRE(source.matchFrame());
    CHECK(source.mediaId() == tl.media('S'));
    CHECK(source.positionText() == tc(9)); // source frame 7 + 2 frames into the clip
    CHECK(source.inText() == tc(7));
    CHECK(source.outText() == tc(12));
    CHECK(controller.undoText() == QStringLiteral("Undo Match Frame"));
    CHECK(shown == 1);

    // Another source frame of S: Shift+R moves the program playhead to where it is used.
    source.seek(f(11));
    controller.setPlayhead(f(0));
    REQUIRE(source.reverseMatchFrame());
    CHECK(controller.playhead() == f(8));
    source.seek(f(30)); // not used in the sequence
    CHECK_FALSE(source.reverseMatchFrame());
    CHECK_FALSE(source.message().isEmpty());

    controller.setPlayhead(f(11)); // nothing there
    CHECK_FALSE(source.matchFrame());
}

// ---- Export Frame -------------------------------------------------------------------------------------------------

TEST_CASE("Export Frame writes a PNG identical to the offscreen render of the frame at the playhead",
          "[app][source][exportframe][gpu]") {
    Timeline tl(R"(
        V1 tgt |............|
        A1 tgt |............|
    )");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    {
        command::CommandEngine engine(controller.session());
        command::CommandRequest title;
        title.command = "title.add";
        title.params = command::Json{{"text", "Export Frame"}, {"at", 0}, {"duration", f(12).value}, {"track", "V1"},
                                     {"sequence", tl.seq.value}};
        REQUIRE(engine.execute(title).ok());
    }
    controller.setPlayhead(f(4));
    ProgramToolsBridge tools;
    tools.setController(&controller);

    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = tools.exportFrame(QUrl::fromLocalFile(dir.path()));
    INFO(tools.message().toStdString());
    REQUIRE_FALSE(path.isEmpty());
    CHECK(QFileInfo(path).fileName().endsWith(QStringLiteral(".png")));
    CHECK(QFileInfo(path).fileName().contains(QStringLiteral("00;00;00;04")));
    QImage written(path);
    REQUIRE_FALSE(written.isNull());
    written = written.convertToFormat(QImage::Format_RGBA8888);

    // The offscreen render of the same frame, straight from the export renderer.
    const ProjectSnapshot snap = controller.snapshot();
    const Sequence& seq = *snap->findSequence(tl.seq);
    exporter::RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 1;
    auto rendered = exporter::renderedVideoSourceFactory(options)(
        *snap, seq, exporter::VideoSourceFormat{seq.width, seq.height, seq.frameRate, f(4), 1, {}});
    REQUIRE(rendered);
    REQUIRE((*rendered)->request(0));
    auto picture = (*rendered)->next();
    (*rendered)->stop();
    REQUIRE(picture);
    const media::CpuPlanes* cpu = picture->cpu();
    REQUIRE(cpu);
    REQUIRE(written.width() == picture->width);
    REQUIRE(written.height() == picture->height);
    std::size_t different = 0;
    for (int y = 0; y < picture->height; ++y) {
        if (std::memcmp(written.constScanLine(y), cpu->planes[0].data + static_cast<std::ptrdiff_t>(y) * cpu->planes[0].stride,
                        static_cast<std::size_t>(picture->width) * 4) != 0)
            ++different;
    }
    CHECK(different == 0);
    // And it is a picture: the title's text is there.
    const QRgb first = written.pixel(0, 0);
    bool varied = false;
    for (int y = 0; y < written.height() && !varied; y += 7) {
        for (int x = 0; x < written.width() && !varied; x += 7)
            varied = written.pixel(x, y) != first;
    }
    CHECK(varied);

    // A second export of the same frame does not overwrite the first.
    const QString again = tools.exportFrame(QUrl::fromLocalFile(dir.path()));
    CHECK(again != path);
    CHECK(QFileInfo::exists(again));
}

// ---- The shell ----------------------------------------------------------------------------------------------------

TEST_CASE("Editing workspace: Source and Program monitors side by side; narrow windows show one with a switch",
          "[app][source][layout][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    REQUIRE(source != nullptr);
    QQuickItem* area = shell.item("monitorArea");
    QQuickItem* sourcePanel = shell.item("sourceMonitorPanel");
    QQuickItem* programPanel = shell.item("monitorPanel");
    REQUIRE(area != nullptr);
    REQUIRE(sourcePanel != nullptr);
    REQUIRE(programPanel != nullptr);

    for (const auto [w, h] : {std::pair{1920, 1080}, std::pair{kShotWidth, kShotHeight}}) {
        INFO(w << "x" << h);
        shell.resize(w, h);
        spin(50);
        CHECK(area->property("dual").toBool());
        CHECK(sourcePanel->isVisible());
        CHECK(programPanel->isVisible());
        // Source on the left (each monitor fills its dock slot: compare where they are in the window).
        CHECK(sourcePanel->mapToScene({}).x() + sourcePanel->width() < programPanel->mapToScene({}).x());
        CHECK(std::abs(sourcePanel->width() - programPanel->width()) <= 1.0);
        CHECK_FALSE(named(programPanel, QStringLiteral("monitorSwitch"))->isVisible());
    }

    // Narrow: one monitor, with the Source/Program switch.
    shell.resize(shell.theme->property("windowMinWidth").toInt(), 760);
    spin(50);
    CHECK(area->property("single").toBool());
    CHECK(programPanel->isVisible());
    CHECK_FALSE(sourcePanel->isVisible());
    CHECK(std::abs(programPanel->width() - area->width()) <= 1.0);
    QQuickItem* programSwitch = named(programPanel, QStringLiteral("monitorSwitch"));
    REQUIRE(programSwitch != nullptr);
    CHECK(programSwitch->isVisible());
    // Loading a source shows the Source Monitor; the switch goes back.
    REQUIRE(source->load(mediaKey(controller->snapshot()->media.front()->id)));
    spin(20);
    CHECK(sourcePanel->isVisible());
    CHECK_FALSE(programPanel->isVisible());
    QMetaObject::invokeMethod(named(sourcePanel, QStringLiteral("monitorSwitch")), "activated", Q_ARG(int, 1));
    spin(20);
    CHECK(programPanel->isVisible());
    CHECK_FALSE(sourcePanel->isVisible());

    // The other workspaces show the Program Monitor only.
    shell.resize(kShotWidth, kShotHeight);
    shell.window->setProperty("workspace", 1);
    spin(50);
    CHECK_FALSE(area->property("dual").toBool());
    CHECK_FALSE(sourcePanel->isVisible());
    CHECK(programPanel->isVisible());
    shell.window->setProperty("workspace", 0);
    spin(50);
    CHECK(area->property("dual").toBool());

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("The Source Monitor's keys act on the source; the same keys in the Program Monitor act on the sequence",
          "[app][source][keys][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* keys = shell.engine.singletonInstance<KeymapDispatcher*>("Wabcut", "KeymapDispatcher");
    Timeline tl{kPicture, kMedia};
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(source->load(mediaKey(tl.media('S'))));
    const auto marks = [&] {
        const MediaItem* s = controller->snapshot()->findMedia(tl.media('S'));
        return std::pair{s->markIn, s->markOut};
    };

    // The scopes resolve differently: Ctrl/Cmd+R is Show Rulers in the Program Monitor, Speed/Duration (Application)
    // in the Source Monitor, which has no panel keys of its own (Adobe lists none: it uses the Application keys).
    const auto chord = keymap::parseKeyChord(isMac() ? "Cmd+R" : "Ctrl+R", keymap::currentPlatform());
    REQUIRE(chord);
    CHECK(keys->keymap().resolve(keymap::Scope::ProgramMonitor, *chord) == std::optional<keymap::CommandId>("programMonitor.showRulers"));
    CHECK(keys->keymap().resolve(keymap::Scope::SourceMonitor, *chord) == std::optional<keymap::CommandId>("clip.speedDuration"));

    // Focus in the Source Monitor: I, O, Shift+I, X and the clears mark the source, never the sequence.
    QQuickItem* sourceWell = shell.item("sourceMonitorWell");
    REQUIRE(sourceWell != nullptr);
    sourceWell->forceActiveFocus();
    REQUIRE(sourceWell->hasActiveFocus());
    CHECK(keys->scopeAt(sourceWell) == QStringLiteral("sourceMonitor"));
    source->seek(f(10));
    tap(shell.window.get(), Qt::Key_I, {}, QStringLiteral("i"));
    source->seek(f(20));
    tap(shell.window.get(), Qt::Key_O, {}, QStringLiteral("o"));
    CHECK(marks() == std::pair{std::optional<Ticks>(f(10)), std::optional<Ticks>(f(21))});
    CHECK_FALSE(controller->currentSequence()->markIn);
    CHECK_FALSE(controller->currentSequence()->markOut);
    tap(shell.window.get(), Qt::Key_I, Qt::ShiftModifier, QStringLiteral("I")); // Go to In
    CHECK(source->positionText() == tc(10));
    const Qt::KeyboardModifiers clear = isMac() ? Qt::KeyboardModifiers(Qt::AltModifier)
                                                : Qt::KeyboardModifiers(Qt::ControlModifier | Qt::ShiftModifier);
    tap(shell.window.get(), Qt::Key_I, clear); // Clear In
    CHECK_FALSE(marks().first);
    CHECK(marks().second == f(21));
    tap(shell.window.get(), Qt::Key_X, {}, QStringLiteral("x")); // Mark Clip: the whole source
    CHECK(marks() == std::pair{std::optional<Ticks>(f(0)), std::optional<Ticks>(f(50))});
    tap(shell.window.get(), Qt::Key_X, clear); // Clear In and Out
    CHECK(marks() == std::pair{std::optional<Ticks>(), std::optional<Ticks>()});
    tap(shell.window.get(), Qt::Key_Right); // a frame step moves the source, not the program playhead
    const Ticks programPlayhead = controller->playhead();
    CHECK(source->positionText() == tc(11));
    CHECK(controller->playhead() == programPlayhead);

    // Focus in the Program Monitor: the same I marks the sequence.
    QQuickItem* programWell = shell.item("programMonitorWell");
    REQUIRE(programWell != nullptr);
    programWell->forceActiveFocus();
    CHECK(keys->scopeAt(programWell) == QStringLiteral("programMonitor"));
    controller->setPlayhead(f(3));
    tap(shell.window.get(), Qt::Key_I, {}, QStringLiteral("i"));
    CHECK(controller->currentSequence()->markIn == f(3));
    CHECK_FALSE(marks().first);

    // , inserts from the source wherever the focus is; F matches the frame under the program playhead.
    source->seek(f(0));
    source->markIn();
    source->seek(f(1));
    source->markOut();
    controller->clearMarks(true, true);
    controller->setPlayhead(f(2));
    tap(shell.window.get(), Qt::Key_Comma, {}, QStringLiteral(","));
    CHECK(clipsOf(*controller, tl.media('S')) == std::pair{1, 2});
    controller->setPlayhead(f(8));
    tap(shell.window.get(), Qt::Key_F, {}, QStringLiteral("f"));
    CHECK(source->mediaId() == tl.media('A'));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Track headers: source patching and targeting columns reflect and edit the model; screenshot",
          "[app][source][patching][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    Timeline tl{kPicture, kMedia};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(source->load(mediaKey(tl.media('S'))));
    REQUIRE_FALSE(shell.grab().isNull()); // lays the headers out
    QQuickItem* content = shell.window->contentItem();
    const auto slotOf = [&](const char* label) { return named(headerOf(content, trackKey(tl.track(label))), QStringLiteral("sourcePatch")); };
    const auto patchOf = [&](const char* label) { return source->patchMap().value(trackKey(tl.track(label))).toMap(); };
    REQUIRE(slotOf("V2") != nullptr);
    REQUIRE(slotOf("A2") != nullptr);
    CHECK(slotOf("V1")->isVisible());
    CHECK(slotOf("V1")->property("patched").toBool());
    CHECK_FALSE(slotOf("V2")->property("patched").toBool());

    // Click V2's empty slot: the source's video goes there.
    click(shell.window.get(), slotOf("V2"));
    CHECK(patchOf("V2").value("source").toString() == QStringLiteral("V1"));
    CHECK(slotOf("V2")->property("patchOn").toBool());
    // Alt-click A2's indicator: switched off; a plain click switches it on again.
    click(shell.window.get(), slotOf("A2"), Qt::AltModifier);
    CHECK_FALSE(patchOf("A2").value("enabled").toBool());
    click(shell.window.get(), slotOf("A2"));
    CHECK(patchOf("A2").value("enabled").toBool());
    // Drag A1's indicator onto A2: they swap.
    const QPointF from = slotOf("A1")->mapToScene(QPointF(slotOf("A1")->width() / 2, slotOf("A1")->height() / 2));
    const QPointF to = slotOf("A2")->mapToScene(QPointF(slotOf("A2")->width() / 2, slotOf("A2")->height() / 2));
    mouse(shell.window.get(), QEvent::MouseButtonPress, from, Qt::LeftButton);
    for (int step = 1; step <= 4; ++step)
        mouse(shell.window.get(), QEvent::MouseMove, from + (to - from) * step / 4.0, Qt::LeftButton);
    mouse(shell.window.get(), QEvent::MouseButtonRelease, to, Qt::NoButton);
    CHECK(patchOf("A2").value("source").toString() == QStringLiteral("A1"));
    CHECK(patchOf("A1").value("source").toString() == QStringLiteral("A2"));

    // The target chip: sequence.setTargets.
    QQuickItem* v2Target = named(headerOf(content, trackKey(tl.track("V2"))), QStringLiteral("trackTarget"));
    REQUIRE(v2Target != nullptr);
    click(shell.window.get(), v2Target);
    CHECK(edit::targetedTracks(*controller->currentSequence()) ==
          std::vector<TrackId>{tl.track("V1"), tl.track("V2"), tl.track("A1")});
    CHECK(controller->undoText() == QStringLiteral("Undo Set Track Targets"));
    // Switch A1's source track (on A2) off again for the picture: indicators on, one off, targets on V1, V2 and A1.
    click(shell.window.get(), slotOf("A2"), Qt::AltModifier);

    shell.grab();
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* timeline = shell.item("timelinePanel");
    REQUIRE(timeline != nullptr);
    const double dpr = image.devicePixelRatio();
    const QRectF r = timeline->mapRectToScene(QRectF(0, 0, shell.theme->property("trackHeaderWidth").toDouble() + 120, timeline->height()));
    image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
        .save(QDir(outputDir()).filePath(QStringLiteral("source-patching.png")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Fit Clip dialog: each choice through the dialog, Always use this choice, and the ghost preview; screenshots",
          "[app][source][fourpoint][qml][screenshots]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
    REQUIRE(view != nullptr);
    Timeline tl{kPicture, kMedia};
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(source->load(mediaKey(tl.media('S'))));
    // Source 4..10 (6 frames) into the sequence 3..6 (4 frames).
    source->seek(f(4));
    source->markIn();
    source->seek(f(9));
    source->markOut();
    controller->setPlayhead(f(3));
    controller->markInAtPlayhead();
    controller->setPlayhead(f(6));
    controller->markOutAtPlayhead();
    REQUIRE(source->needsFit());

    // Hovering Insert shows the edit as a ghost (a dry run); leaving clears it.
    source->previewEdit(true);
    REQUIRE(view->externalPreview() != nullptr);
    CHECK(view->externalPreview()->added.size() == 3); // V1, A1 and A2 (S has two audio streams)
    source->clearPreview();
    CHECK(view->externalPreview() == nullptr);

    QObject* dialog = shell.object("fitClipDialog");
    REQUIRE(dialog != nullptr);
    QQuickItem* lanes = shell.item("timelineLanesArea");
    REQUIRE(lanes != nullptr);
    const std::string before = wabcut::test::render(*controller->snapshot(), tl.seq, kPicture);
    struct Choice {
        const char* fit;
        const char* insert;
    };
    const std::vector<Choice> choices{
        {"speed", ""}, // a retimed clip: checked below
        {"ignoreSourceIn", "V2 |................|\nV1 |AaaS6ssA3aaaaaaa|\nA1 |AaaS6ssA3aaaaaaa|\nA2 |...S6ss.........|"},
        {"ignoreSourceOut", "V2 |................|\nV1 |AaaS4ssA3aaaaaaa|\nA1 |AaaS4ssA3aaaaaaa|\nA2 |...S4ss.........|"},
        {"ignoreSequenceIn", "V2 |..................|\nV1 |AS4ssssA1aaaaaaaaa|\nA1 |AS4ssssA1aaaaaaaaa|\nA2 |.S4ssss...........|"},
        {"ignoreSequenceOut", "V2 |..................|\nV1 |AaaS4ssssA3aaaaaaa|\nA1 |AaaS4ssssA3aaaaaaa|\nA2 |...S4ssss.........|"},
    };
    bool shot = false;
    for (const Choice& choice : choices) {
        INFO(choice.fit);
        lanes->forceActiveFocus();
        controller->setPlayhead(f(0));
        tap(shell.window.get(), Qt::Key_Comma, {}, QStringLiteral(",")); // Insert: the dialog asks
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
        CHECK(wabcut::test::render(*controller->snapshot(), tl.seq, kPicture) == before);
        REQUIRE_FALSE(shell.grab().isNull()); // lays the dialog out
        QQuickItem* option = named(shell.window->contentItem(), QStringLiteral("fitChoice_") + QString::fromLatin1(choice.fit));
        REQUIRE(option != nullptr);
        click(shell.window.get(), option);
        CHECK(dialog->property("choice").toString() == QString::fromLatin1(choice.fit));
        if (!shot) {
            shot = true;
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            image.save(QDir(outputDir()).filePath(QStringLiteral("fit-clip-dialog.png")));
        }
        click(shell.window.get(), named(shell.window->contentItem(), QStringLiteral("fitClipOk")));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
        CHECK(controller->undoText() == QStringLiteral("Undo Insert from Source"));
        if (std::string_view(choice.fit) == "speed") {
            const Track& v1 = *controller->currentSequence()->findTrack(tl.track("V1"));
            REQUIRE(v1.clipIndexAt(f(3)));
            const Clip& fitted = v1.clips[*v1.clipIndexAt(f(3))];
            CHECK(fitted.speed == Speed{3, 2});
            CHECK(fitted.duration() == f(4));
            CHECK(fitted.sourceIn == f(4));
            CHECK(v1.clips[*v1.clipIndexAt(f(7))].sourceIn == f(3)); // the rest of A follows the retimed clip
        } else {
            CHECK(wabcut::test::render(*controller->snapshot(), tl.seq, choice.insert) == normalize(choice.insert));
        }
        controller->undo();
        CHECK(wabcut::test::render(*controller->snapshot(), tl.seq, kPicture) == before);
    }

    // "Always use this choice": remembered in Preferences, and the next four-point edit does not ask.
    lanes->forceActiveFocus();
    tap(shell.window.get(), Qt::Key_Period, {}, QStringLiteral("."));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }));
    REQUIRE_FALSE(shell.grab().isNull());
    click(shell.window.get(), named(shell.window->contentItem(), QStringLiteral("fitChoice_ignoreSequenceOut")));
    click(shell.window.get(), named(shell.window->contentItem(), QStringLiteral("fitClipRemember")));
    click(shell.window.get(), named(shell.window->contentItem(), QStringLiteral("fitClipOk")));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }));
    CHECK(shell.preferences->property("fitClipChoice").toString() == QStringLiteral("ignoreSequenceOut"));
    CHECK(wabcut::test::render(*controller->snapshot(), tl.seq, kPicture) ==
          normalize("V2 |............|\nV1 |AaaS4ssssA9a|\nA1 |AaaS4ssssA9a|\nA2 |...S4ssss...|"));
    controller->undo();
    lanes->forceActiveFocus();
    tap(shell.window.get(), Qt::Key_Period, {}, QStringLiteral("."));
    spin(50);
    CHECK_FALSE(dialog->property("opened").toBool());
    CHECK(wabcut::test::render(*controller->snapshot(), tl.seq, kPicture) ==
          normalize("V2 |............|\nV1 |AaaS4ssssA9a|\nA1 |AaaS4ssssA9a|\nA2 |...S4ssss...|"));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Dragging from the Source Monitor: the picture edits video and audio, the handles video only or audio only",
          "[app][source][drag][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    auto* view = qobject_cast<TimelineView*>(shell.object("timelineView"));
    REQUIRE(view != nullptr);
    Timeline tl{kPicture, kMedia};
    controller->resetProject(tl.state, tl.seq);
    REQUIRE(source->load(mediaKey(tl.media('T'))));
    source->seek(f(0));
    source->markIn();
    source->seek(f(1));
    source->markOut();
    REQUIRE_FALSE(shell.grab().isNull());

    // The handles carry what they drag.
    QQuickItem* sourcePanel = shell.item("sourceMonitorPanel");
    CHECK(named(sourcePanel, QStringLiteral("dragVideoOnly"))->property("streams").toString() == QStringLiteral("video"));
    CHECK(named(sourcePanel, QStringLiteral("dragAudioOnly"))->property("streams").toString() == QStringLiteral("audio"));
    CHECK(named(sourcePanel, QStringLiteral("sourcePictureDrag"))->property("streams").toString() == QStringLiteral("av"));

    // A drop on the lanes (what the drag ends in): the marked range where it is dropped.
    const auto dropAt = [&](const char* streams, Ticks at, const char* track) {
        const auto row = std::ranges::find_if(view->rows(), [&](const TimelineView::Row& r) { return r.track->id == tl.track(track); });
        REQUIRE(row != view->rows().end());
        const QPointF local(at.toSeconds() * view->pixelsPerSecond() - view->scrollX() + 2, row->y - view->scrollY() + row->height / 2);
        const QPoint p = view->mapToScene(local).toPoint();
        QMimeData mime;
        mime.setData(QStringLiteral("application/x-wabcut-source"), QByteArray(streams));
        QDragEnterEvent enter(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(shell.window.get(), &enter);
        QDragMoveEvent move(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(shell.window.get(), &move);
        QDropEvent drop(p, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QCoreApplication::sendEvent(shell.window.get(), &drop);
        spin(20);
    };
    dropAt("video", f(4), "V2");
    CHECK(clipsOf(*controller, tl.media('T')) == std::pair{1, 0});
    CHECK(controller->currentSequence()->findTrack(tl.track("V2"))->clips.size() == 1);
    dropAt("audio", f(20), "A1"); // its two streams fill A1 and A2
    CHECK(clipsOf(*controller, tl.media('T')) == std::pair{1, 2});
    dropAt("av", f(30), "V1");
    CHECK(clipsOf(*controller, tl.media('T')) == std::pair{2, 4});

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

TEST_CASE("Screenshot: the Editing workspace with the Source and Program monitors", "[app][source][screenshots][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window != nullptr);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* source = shell.engine.singletonInstance<SourceMonitorBridge*>("Wabcut", "SourceMonitor");
    // A title as the source: it renders without media files, so the Source Monitor shows a picture.
    {
        command::CommandEngine engine(controller->session());
        command::CommandRequest title;
        title.command = "title.add";
        title.params = command::Json{{"text", "Source Monitor"},
                                     {"at", controller->currentSequence()->contentEnd().value},
                                     {"duration", Ticks::fromSeconds(8).value},
                                     {"track", "V3"},
                                     {"sequence", controller->currentSequenceId().value}};
        const auto added = engine.execute(title);
        INFO(command::toJson(added).dump());
        REQUIRE(added.ok());
    }
    MediaId titleMedia;
    for (const MediaPtr& m : controller->snapshot()->media) {
        if (m->isTitle())
            titleMedia = m->id;
    }
    REQUIRE(source->load(mediaKey(titleMedia)));
    source->seek(Ticks::fromSeconds(1));
    source->markIn();
    source->seek(Ticks::fromSeconds(5));
    source->markOut();
    source->seek(Ticks::fromSeconds(2));
    REQUIRE(QMetaObject::invokeMethod(shell.window.get(), "showBrowserPanel", Q_ARG(QVariant, QVariant(QStringLiteral("library")))));
    QQuickItem* picture = shell.item("sourceMonitorPicture");
    REQUIRE(picture != nullptr);
    // The source renders on its own thread; a few frames until it presents.
    waitFor([&] {
        shell.grab();
        return picture->property("hasFrame").toBool();
    });
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    CHECK(shell.item("monitorArea")->property("dual").toBool());
    image.save(QDir(outputDir()).filePath(QStringLiteral("editing-dual-monitors.png")));

    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

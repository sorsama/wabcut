// M5l: multi-camera editing and nested sequences, offscreen; no window is shown.
//   * Create Multi-Camera Source Sequence: the Library's items in the dialog, the proposed sync, multicam.create (by
//     timecode, and by sound with the A.5 report: offsets and confidence);
//   * the angle grid: every angle in one offscreen composite at a reduced resolution, its cost (the bench writes
//     test-output/ui/multicam-grid-perf.txt);
//   * switching: while playing the switches are recorded and sent at stop as one undo step; paused, Select switches
//     the cut under the playhead and Cut cuts there; picture, sound or both; the camera keys in the shell; Flatten;
//   * nesting: Nest…, the double-click that opens a nested clip as a sequence tab, the breadcrumb back, Reveal in
//     Library, Unnest; nested and multi-camera clips' stripe, angle names and angle cuts in the timeline.
// Screenshots: the multi-camera view and a nested sequence tab.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "CommandJob.h"
#include "LibraryBridge.h"
#include "MulticamEditBridge.h"
#include "MulticamGrid.h"
#include "NestingBridge.h"
#include "ProjectController.h"
#include "SequenceTabsBridge.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/audio/edit/AudioAnalysis.h>
#include <wabcut/audio/edit/AudioSync.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/Nesting.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TitleOps.h>

#include <catch2/catch_test_macros.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
using command::Json;

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// An empty main sequence of `frames` frames (25 fps): V1 and A1 targeted, A2 below them.
std::string emptyPicture(int frames = 100) {
    const std::string blank(static_cast<std::size_t>(frames), '.');
    return "V1 tgt |" + blank + "|\nA1 tgt |" + blank + "|\nA2     |" + blank + "|";
}

struct Cams {
    ProjectState state;
    SequenceId main;
    SequenceId source;
};

/// The hue of angle `i`'s title (distinct for 16 angles).
int camHue(int i) {
    static constexpr int kHues[] = {0, 120, 230, 55, 290, 175, 25, 330, 95, 205, 145, 265, 10, 75, 190, 310};
    return kHues[i % 16];
}

/// `state` plus a multicam source of `count` titled angles ("Cam 1".., big text, a colour of its own each, 4 s): a
/// synthetic picture per angle that renders without media files.
Cams addTitledCams(ProjectState state, SequenceId main, int count, double length = 4.0) {
    const Sequence& mainSeq = *state.findSequence(main);
    edit::SequenceSettings settings;
    settings.name = "Cameras";
    settings.frameRate = mainSeq.frameRate;
    settings.videoTracks = count;
    settings.audioTracks = 0;
    auto created = edit::addSequence(state, settings);
    REQUIRE(created);
    state = std::move(created->state);
    const SequenceId source = created->id;
    for (int i = 0; i < count; ++i) {
        const TrackId track = state.findSequence(source)->videoTracks[static_cast<std::size_t>(i)]->id;
        const QColor colour = QColor::fromHsv(camHue(i), 210, 255);
        std::vector<EffectParam> style{{"size", ParamValue(300.0)},
                                       {"fill.red", ParamValue(colour.redF())},
                                       {"fill.green", ParamValue(colour.greenF())},
                                       {"fill.blue", ParamValue(colour.blueF())}};
        auto titled = edit::addTitle(state, source, track, Ticks{}, seconds(length), "Cam " + std::to_string(i + 1), style);
        REQUIRE(titled);
        state = std::move(titled->state);
    }
    auto seq = std::make_shared<Sequence>(*state.findSequence(source));
    MulticamInfo info;
    info.sync = MulticamSync::InPoints;
    for (int i = 0; i < count; ++i)
        info.angles.push_back(MulticamAngle{"Cam " + std::to_string(i + 1), seq->videoTracks[static_cast<std::size_t>(i)]->id, {}, Ticks{}});
    seq->multicam = info;
    state = state.withSequence(seq);
    return {std::move(state), main, source};
}

/// `tl` plus a multicam source of media A, B and C (picture and sound each; synced at their starts).
Cams addMediaCams(const Timeline& tl) {
    edit::MulticamSpec spec;
    spec.name = "Interview";
    spec.angles = {tl.media('A'), tl.media('B'), tl.media('C')};
    spec.sync = MulticamSync::InPoints;
    spec.angleNames = {"Wide", "Close", "Side"};
    auto made = edit::createMulticam(tl.state, spec);
    REQUIRE(made);
    return {std::move(made->state), tl.seq, made->id};
}

/// Edits the source into the main sequence at 0 (a multicam clip on V1 and its sound on A1).
void placeMulticam(ProjectController& controller, const Cams& cams, Ticks at = {}) {
    const auto placed = CommandJob::runNow(&controller, nullptr, "sequence.overwrite",
                                           Json{{"sequence", cams.main.value}, {"sourceSequence", cams.source.value}, {"at", at.value}});
    INFO(commandErrorText(placed).toStdString());
    REQUIRE(placed.ok());
}

/// "start:angle" of each clip of a track (1-based angles, whole seconds), e.g. "0:1 2:3".
std::string angleRuns(const ProjectController& controller, bool audio, std::size_t index = 0) {
    const SequencePtr seq = controller.currentSequence();
    const auto& tracks = audio ? seq->audioTracks : seq->videoTracks;
    std::string out;
    for (const Clip& c : tracks[index]->clips) {
        if (!out.empty())
            out += ' ';
        out += std::to_string(c.start().value / kTicksPerSecond) + ":" +
               (c.isNested() ? std::to_string(c.angles.first() + 1) : std::string("-"));
    }
    return out;
}

/// The mean colour of the bright pixels of grid cell `cell` (row-major), or an invalid colour when it has few.
QColor cellColour(const QImage& image, int columns, int cell) {
    const int w = image.width() / columns;
    const int h = image.height() / columns;
    const int x0 = (cell % columns) * w;
    const int y0 = (cell / columns) * h;
    double r = 0, g = 0, b = 0;
    int n = 0;
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            const QColor c = image.pixelColor(x, y);
            if (std::max({c.red(), c.green(), c.blue()}) < 120)
                continue;
            r += c.red();
            g += c.green();
            b += c.blue();
            ++n;
        }
    }
    if (n < 40)
        return {};
    return QColor(static_cast<int>(r / n), static_cast<int>(g / n), static_cast<int>(b / n));
}

int hueDistance(int a, int b) {
    const int d = std::abs(a - b) % 360;
    return std::min(d, 360 - d);
}

/// A.5 audio sync stand-in: each media item starts at a fixed time on the reference's clock, with a fixed confidence.
class FixedSync final : public audio::edit::AudioSyncService {
public:
    std::map<std::uint64_t, double> startSeconds;
    std::map<std::uint64_t, double> confidence;
    int calls = 0;

    std::expected<Result, Error> sync(const ProjectState&, std::span<const Angle> angles, std::size_t reference,
                                      const audio::edit::SyncOptions& options, std::stop_token,
                                      const std::function<void(double)>& progress) override {
        ++calls;
        Result r;
        r.report.reference = reference;
        const double ref = startSeconds[angles[reference].media.front().value];
        for (const Angle& a : angles) {
            const std::uint64_t m = a.media.front().value;
            const double at = startSeconds[m] - ref;
            audio::edit::SyncMatch match;
            match.synced = true;
            match.offset = std::llround(at * options.refineRate);
            match.offsetAtStart = match.offset;
            match.confidence = confidence.contains(m) ? confidence[m] : 1.0;
            match.overlapSeconds = 3.0;
            match.windows = 4;
            r.report.matches.push_back(match);
            r.starts.push_back({seconds(at)});
            r.order.push_back(a.media);
        }
        if (progress)
            progress(1.0);
        return r;
    }
};

std::shared_ptr<command::CommandServices> syncServices(std::shared_ptr<FixedSync> sync) {
    auto audio = std::make_shared<audio::edit::AudioServices>(nullptr, nullptr);
    audio->sync = sync;
    std::vector<std::shared_ptr<command::CommandServices>> parts;
    parts.push_back(std::make_shared<command::MulticamServices>(std::make_shared<audio::edit::MulticamAudioSync>(sync)));
    parts.push_back(audio);
    return std::make_shared<command::ServiceBundle>(std::move(parts));
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

void sendKey(QQuickWindow* window, int key, Qt::KeyboardModifiers mods = {}, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, mods, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, mods, text);
    QCoreApplication::sendEvent(window, &release);
}

void saveCrop(const QImage& image, const QQuickItem* item, const QString& name) {
    const double dpr = image.devicePixelRatio();
    const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
    REQUIRE(image.copy(QRectF(r.x() * dpr, r.y() * dpr, r.width() * dpr, r.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(name)));
}

struct GridBench {
    int angles = 0;
    int columns = 0;
    double firstMs = 0; ///< paused: the first frame, the renderer opened for it
    double meanMs = 0;  ///< playing: request to readback per frame, the source kept open
    double p95Ms = 0;
    double maxMs = 0;
    int frames = 0;
    QString backend;
};

/// The angle grid of `angles` titled angles at `size`: one paused frame, then `frames` frames played through the
/// worker (each frame waited for), as the Program Monitor's view draws them.
GridBench benchGrid(int angles, QSize size, int frames) {
    Timeline tl(emptyPicture());
    const Cams cams = addTitledCams(tl.state, tl.seq, angles, 8.0);
    GridBench out;
    out.angles = angles;
    out.columns = multicamGridColumns(static_cast<std::size_t>(angles));
    auto grid = multicamGridState(cams.state, cams.source, out.columns);
    REQUIRE(grid);
    auto shared = std::make_shared<const ProjectState>(std::move(*grid));

    std::mutex mutex;
    std::condition_variable published;
    std::vector<double> costs;
    QString error;
    QString backend;
    MulticamGridRenderer renderer([&](std::expected<MulticamGridFrame, QString> frame, std::uint64_t) {
        std::lock_guard lock(mutex);
        if (frame) {
            costs.push_back(frame->milliseconds);
            backend = frame->backend;
        } else {
            error = frame.error();
        }
        published.notify_all();
    });
    const auto waitFrame = [&](std::size_t count) {
        std::unique_lock lock(mutex);
        const bool ok = published.wait_for(lock, std::chrono::seconds(20), [&] { return costs.size() >= count || !error.isEmpty(); });
        INFO(error.toStdString());
        REQUIRE(ok);
        REQUIRE(error.isEmpty());
    };
    renderer.setSource(shared, cams.source, size, {}, 1); // paused at 0: one frame
    waitFrame(1);
    const Rate rate = shared->findSequence(cams.source)->frameRate;
    for (int i = 1; i <= frames + 1; ++i) {
        renderer.setTime(*framesToTicks(i, rate, Rounding::Floor), true);
        waitFrame(static_cast<std::size_t>(i) + 1);
    }
    renderer.clear();
    std::lock_guard lock(mutex);
    out.firstMs = costs.front();
    // The paused frame opened the source; the played ones are the steady cost.
    std::vector<double> steady(costs.begin() + 1, costs.end());
    REQUIRE_FALSE(steady.empty());
    out.frames = static_cast<int>(steady.size());
    out.meanMs = std::accumulate(steady.begin(), steady.end(), 0.0) / static_cast<double>(steady.size());
    std::sort(steady.begin(), steady.end());
    out.p95Ms = steady[std::min(steady.size() - 1, static_cast<std::size_t>(0.95 * static_cast<double>(steady.size())))];
    out.maxMs = steady.back();
    out.backend = backend;
    return out;
}

} // namespace

// ---- Create Multi-Camera Source Sequence -----------------------------------------------------------------------------

TEST_CASE("Multicam create: the bridge describes the Library items, proposes the sync and reports each angle",
          "[app][multicam]") {
    Timeline tl(emptyPicture());
    // A and B have start timecodes 2 s apart; C has none.
    for (MediaPtr& m : tl.state.media) {
        if (m->id != tl.media('A') && m->id != tl.media('B'))
            continue;
        auto copy = std::make_shared<MediaItem>(*m);
        copy->info.startTimecode = seconds(3600 + (m->id == tl.media('B') ? 2 : 0));
        m = copy;
    }
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    LibraryBridge library;
    library.setController(&controller);
    MulticamEditBridge mc;
    mc.setController(&controller);
    mc.setLibrary(&library);
    auto sync = std::make_shared<FixedSync>();
    mc.setServices(syncServices(sync));
    mc.setSynchronous(true);
    QString shown;
    QObject::connect(&library, &LibraryBridge::itemShown, [&](const QString& key) { shown = key; });

    const QString a = mediaKey(tl.media('A'));
    const QString b = mediaKey(tl.media('B'));
    const QString c = mediaKey(tl.media('C'));
    const QVariantList items = mc.describeMedia({a, b, c, QStringLiteral("s999"), QStringLiteral("junk")});
    REQUIRE(items.size() == 3);
    CHECK(items[0].toMap().value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
    CHECK(items[0].toMap().value(QStringLiteral("hasTimecode")).toBool());
    CHECK_FALSE(items[2].toMap().value(QStringLiteral("hasTimecode")).toBool());
    CHECK(mc.proposedSync({a, b}) == QStringLiteral("timecode"));
    CHECK(mc.proposedSync({a, b, c}) == QStringLiteral("audio")); // all have sound
    CHECK(mc.proposedSync({}) == QStringLiteral("inPoints"));

    // By timecode: B starts 2 s after A; one undo step; the Library shows the new source.
    const auto before = controller.session().undoCount();
    REQUIRE(mc.createSource({a, b}, QStringLiteral("timecode"), QStringLiteral("Panel"), {QString(), QStringLiteral("Host")}).isEmpty());
    QVariantMap report = mc.report();
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(report.value(QStringLiteral("name")).toString() == QStringLiteral("Panel"));
    CHECK(report.value(QStringLiteral("syncName")).toString() == QStringLiteral("Timecode"));
    QVariantList angles = report.value(QStringLiteral("angles")).toList();
    REQUIRE(angles.size() == 2);
    CHECK(angles[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("A"));
    CHECK(angles[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Host"));
    CHECK(angles[1].toMap().value(QStringLiteral("offset")).toString().endsWith(QStringLiteral("02:00")));
    CHECK(angles[1].toMap().value(QStringLiteral("confidence")).toString() == QStringLiteral("exact"));
    const auto source = parseSequenceKey(report.value(QStringLiteral("sequence")).toString());
    REQUIRE(source);
    const Sequence* made = controller.snapshot()->findSequence(*source);
    REQUIRE(made);
    REQUIRE(made->multicam);
    CHECK(made->multicam->angles.size() == 2);
    CHECK(shown == report.value(QStringLiteral("sequence")).toString());
    CHECK(sync->calls == 0);

    // By sound: audio.syncClips measures first (the report's confidence), then multicam.create on the same analysis.
    sync->startSeconds = {{tl.media('A').value, 0.0}, {tl.media('B').value, 1.5}, {tl.media('C').value, 0.25}};
    sync->confidence = {{tl.media('B').value, 0.93}, {tl.media('C').value, 0.71}};
    REQUIRE(mc.createSource({a, b, c}, QStringLiteral("audio"), {}).isEmpty());
    report = mc.report();
    INFO(report.value(QStringLiteral("error")).toString().toStdString());
    REQUIRE(report.value(QStringLiteral("ok")).toBool());
    CHECK(sync->calls >= 1);
    CHECK(report.value(QStringLiteral("sync")).toString() == QStringLiteral("audio"));
    angles = report.value(QStringLiteral("angles")).toList();
    REQUIRE(angles.size() == 3);
    CHECK(angles[0].toMap().value(QStringLiteral("confidence")).toString() == QStringLiteral("100 %"));
    CHECK(angles[1].toMap().value(QStringLiteral("confidence")).toString() == QStringLiteral("93 %"));
    CHECK(angles[2].toMap().value(QStringLiteral("confidence")).toString() == QStringLiteral("71 %"));
    CHECK(report.value(QStringLiteral("name")).toString() == QStringLiteral("A Multicam"));

    // Refusals come back as text: no timecode on C.
    REQUIRE(mc.createSource({a, c}, QStringLiteral("timecode"), {}).isEmpty());
    CHECK_FALSE(mc.report().value(QStringLiteral("ok")).toBool());
    CHECK_FALSE(mc.message().isEmpty());
    CHECK_FALSE(mc.createSource({QStringLiteral("junk")}, QStringLiteral("timecode"), {}).isEmpty());
}

TEST_CASE("Multicam create in the shell: the Library's menu opens the dialog, Create runs multicam.create",
          "[app][multicam][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mc = shell.engine.singletonInstance<MulticamEditBridge*>("Wabcut", "Multicam");
    REQUIRE(controller);
    REQUIRE(mc);
    mc->setSynchronous(true);
    auto sync = std::make_shared<FixedSync>();
    mc->setServices(syncServices(sync));
    Timeline tl(emptyPicture());
    controller->resetProject(tl.state, tl.seq);
    REQUIRE_FALSE(shell.grab().isNull());

    const QStringList keys{mediaKey(tl.media('A')), mediaKey(tl.media('B'))};
    mc->openCreate(keys);
    QObject* loader = shell.object("multicamCreateDialogLoader");
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* popup = content->parentItem();
    CHECK(dialog->property("items").toList().size() == 2);
    QQuickItem* method = findNamed(popup, QStringLiteral("multicamSyncMethod"));
    REQUIRE(method);
    CHECK(method->property("currentIndex").toInt() == 4); // no timecodes, both have sound: Audio
    sync->startSeconds = {{tl.media('A').value, 0.0}, {tl.media('B').value, 0.8}};
    sync->confidence = {{tl.media('B').value, 0.88}};
    QQuickItem* name = findNamed(popup, QStringLiteral("multicamSequenceName"));
    REQUIRE(name);
    name->setProperty("text", QStringLiteral("Two cameras"));
    QQuickItem* create = findNamed(popup, QStringLiteral("multicamCreateButton"));
    REQUIRE(create);
    const auto before = controller->session().undoCount();
    REQUIRE(QMetaObject::invokeMethod(create, "clicked"));
    REQUIRE(waitFor([&] { return dialog->property("done").toBool(); }, 5000));
    CHECK(controller->session().undoCount() == before + 1);
    CHECK(mc->report().value(QStringLiteral("name")).toString() == QStringLiteral("Two cameras"));
    QQuickItem* report = findNamed(popup, QStringLiteral("multicamReport"));
    REQUIRE(report);
    CHECK(report->isVisible());
    spin(100);
    {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("multicam-create-report.png"))));
    }
    QMetaObject::invokeMethod(dialog, "close");
    spin(200);
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- The angle grid --------------------------------------------------------------------------------------------------

TEST_CASE("Angle grid: every angle in its cell of one offscreen composite at the reduced resolution, and its cost",
          "[app][multicam][gpu]") {
    CHECK(multicamGridColumns(1) == 2);
    CHECK(multicamGridColumns(4) == 2);
    CHECK(multicamGridColumns(5) == 3);
    CHECK(multicamGridColumns(9) == 3);
    CHECK(multicamGridColumns(10) == 4);
    CHECK(multicamGridColumns(16) == 4);

    Timeline tl(emptyPicture());
    const Cams cams = addTitledCams(tl.state, tl.seq, 4);
    // The grid project: every angle scaled into its cell; refused for a sequence that is not a multicam source.
    CHECK_FALSE(multicamGridState(cams.state, cams.main, 2));
    ProjectController controller;
    controller.resetProject(cams.state, cams.main);
    placeMulticam(controller, cams);
    MulticamEditBridge mc;
    mc.setController(&controller);
    mc.setSynchronous(true);
    controller.setPlayhead(seconds(1));
    REQUIRE_FALSE(mc.clip().isEmpty());
    CHECK(mc.sourceName() == QStringLiteral("Cameras"));
    CHECK(mc.columns() == 2);
    const QVariantList angles = mc.angles();
    REQUIRE(angles.size() == 4);
    CHECK(angles[0].toMap().value(QStringLiteral("video")).toBool());
    CHECK(angles[2].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Cam 3"));
    CHECK(angles[2].toMap().value(QStringLiteral("keyText")).toString() == QStringLiteral("3"));
    CHECK(mc.gridImage().isNull()); // drawn only while the view is on

    mc.setViewOn(true);
    const QImage grid = mc.gridImage();
    INFO(mc.gridStats().value(QStringLiteral("error")).toString().toStdString());
    REQUIRE_FALSE(grid.isNull());
    CHECK(grid.size() == QSize(640, 360));
    grid.save(QDir(outputDir()).filePath(QStringLiteral("multicam-grid-4.png")));
    for (int cell = 0; cell < 4; ++cell) {
        const QColor colour = cellColour(grid, 2, cell);
        INFO("cell " << cell << " " << describe(colour));
        REQUIRE(colour.isValid());
        CHECK(hueDistance(colour.hsvHue(), camHue(cell)) <= 25);
    }
    const QVariantMap stats = mc.gridStats();
    CHECK(stats.value(QStringLiteral("frames")).toInt() >= 1);
    CHECK(stats.value(QStringLiteral("lastMs")).toDouble() > 0);
    CHECK_FALSE(stats.value(QStringLiteral("backend")).toString().isEmpty());
    CHECK(stats.value(QStringLiteral("size")).toString() == QStringLiteral("640×360"));
    // Moving the playhead draws again; switching the view off drops the grid.
    controller.setPlayhead(seconds(2));
    CHECK(mc.gridStats().value(QStringLiteral("frames")).toInt() >= 2);
    mc.setViewOn(false);
    CHECK(mc.gridImage().isNull());
}

TEST_CASE("Angle grid cost: 2x2, 3x3 and 4x4 grids played through the offscreen compositor", "[app][multicam][gpu]") {
    QString text = QStringLiteral("# Multi-camera angle grid (M5l): titled synthetic angles, 640x360 grid\n"
                                  "# first = paused frame with the renderer opened for it; mean/p95/max = played frames, "
                                  "request to readback (composite + upload + readback), source kept open\n");
    for (const int angles : {4, 9, 16}) {
        const GridBench b = benchGrid(angles, QSize(640, 360), 48);
        INFO(angles << " angles");
        CHECK(b.frames >= 40);
        CHECK(b.meanMs > 0);
        text += QStringLiteral("%1 angles (%2x%2): first %3 ms, mean %4 ms, p95 %5 ms, max %6 ms over %7 frames · %8\n")
                    .arg(angles)
                    .arg(b.columns)
                    .arg(b.firstMs, 0, 'f', 2)
                    .arg(b.meanMs, 0, 'f', 2)
                    .arg(b.p95Ms, 0, 'f', 2)
                    .arg(b.maxMs, 0, 'f', 2)
                    .arg(b.frames)
                    .arg(b.backend);
    }
    writeOutputFile(QStringLiteral("multicam-grid-perf.txt"), text);
    UNSCOPED_INFO(text.toStdString());
}

// ---- Switching -------------------------------------------------------------------------------------------------------

TEST_CASE("Live switching: the camera keys while playing are recorded and sent at stop as one undo step",
          "[app][multicam]") {
    Timeline tl(emptyPicture());
    const Cams cams = addMediaCams(tl);
    ProjectController controller;
    controller.resetProject(cams.state, cams.main);
    placeMulticam(controller, cams);
    MulticamEditBridge mc;
    mc.setController(&controller);
    mc.setSynchronous(true);
    REQUIRE_FALSE(mc.clip().isEmpty());
    REQUIRE(mc.angles().size() == 3);
    CHECK(angleRuns(controller, false) == "0:1");

    const auto before = controller.session().undoCount();
    mc.setPlayingForTests(true);
    controller.setPlayhead(seconds(1));
    CHECK(mc.selectAngle(2));
    controller.setPlayhead(seconds(1.5));
    CHECK(mc.selectAngle(2)); // the same angle again: nothing new
    controller.setPlayhead(seconds(2));
    CHECK(mc.cutToAngle(3));
    // The view shows what was recorded: angle 3 is in the picture and heard.
    const QVariantList now = mc.angles();
    CHECK(now[2].toMap().value(QStringLiteral("video")).toBool());
    CHECK(now[2].toMap().value(QStringLiteral("audio")).toBool());
    controller.setPlayhead(seconds(3));
    CHECK(mc.selectAngle(1));
    CHECK(mc.pendingSwitches() == 3);
    CHECK(controller.session().undoCount() == before); // nothing is sent while playing
    CHECK(angleRuns(controller, false) == "0:1");

    mc.setPlayingForTests(false);
    CHECK(mc.pendingSwitches() == 0);
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(angleRuns(controller, false) == "0:1 1:2 2:3 3:1");
    CHECK(angleRuns(controller, true) == "0:1 1:2 2:3 3:1"); // both: the sound followed
    controller.undo();
    CHECK(angleRuns(controller, false) == "0:1");
    CHECK(angleRuns(controller, true) == "0:1");
}

TEST_CASE("Paused switching: Select switches the cut under the playhead, Cut cuts there; picture, sound or both",
          "[app][multicam]") {
    Timeline tl(emptyPicture());
    const Cams cams = addMediaCams(tl);
    ProjectController controller;
    controller.resetProject(cams.state, cams.main);
    placeMulticam(controller, cams);
    MulticamEditBridge mc;
    mc.setController(&controller);
    mc.setSynchronous(true);
    const auto before = controller.session().undoCount();

    // Picture only: a cut at 2 s; the sound stays on angle 1.
    mc.setTarget(QStringLiteral("video"));
    controller.setPlayhead(seconds(2));
    CHECK(mc.cutToAngle(2));
    CHECK(angleRuns(controller, false) == "0:1 2:2");
    CHECK(angleRuns(controller, true) == "0:1");
    // Select at 1 s switches the piece under the playhead, no new cut.
    controller.setPlayhead(seconds(1));
    CHECK(mc.selectAngle(3));
    CHECK(angleRuns(controller, false) == "0:3 2:2");
    // Sound only: the audio clip under the playhead takes angle 2; the picture stays.
    mc.setTarget(QStringLiteral("audio"));
    controller.setPlayhead(seconds(3));
    CHECK(mc.selectAngle(2));
    CHECK(angleRuns(controller, false) == "0:3 2:2");
    CHECK(angleRuns(controller, true) == "0:2");
    const QVariantList angles = mc.angles();
    CHECK(angles[1].toMap().value(QStringLiteral("audio")).toBool());
    CHECK(angles[1].toMap().value(QStringLiteral("video")).toBool());
    CHECK(controller.session().undoCount() == before + 3);
    // An angle the source does not have is refused with a reason.
    CHECK_FALSE(mc.selectAngle(7));
    CHECK(mc.message().contains(QStringLiteral("7")));
    CHECK(controller.session().undoCount() == before + 3);
    mc.setTarget(QStringLiteral("nonsense"));
    CHECK(mc.target() == QStringLiteral("both"));
}

TEST_CASE("Camera commands: available in the multi-camera view only; Flatten replaces the clip by the active angle",
          "[app][multicam]") {
    Timeline tl(emptyPicture());
    const Cams cams = addMediaCams(tl);
    ProjectController controller;
    controller.resetProject(cams.state, cams.main);
    placeMulticam(controller, cams);
    MulticamEditBridge mc;
    mc.setController(&controller);
    mc.setSynchronous(true);

    CHECK(MulticamEditBridge::handlesCommand(QStringLiteral("multicam.selectCamera9")));
    CHECK(MulticamEditBridge::handlesCommand(QStringLiteral("multicam.cutToCamera1")));
    CHECK(MulticamEditBridge::handlesCommand(QStringLiteral("multicam.toggleView")));
    CHECK_FALSE(MulticamEditBridge::handlesCommand(QStringLiteral("multicam.selectCamera10")));
    QVariantMap a = mc.commandAvailability(QStringLiteral("multicam.selectCamera2"));
    CHECK(a.value(QStringLiteral("handled")).toBool());
    CHECK_FALSE(a.value(QStringLiteral("enabled")).toBool());
    CHECK_FALSE(mc.runCommand(QStringLiteral("multicam.selectCamera2"))); // the view is off
    CHECK(angleRuns(controller, false) == "0:1");
    CHECK(mc.runCommand(QStringLiteral("multicam.toggleView")));
    CHECK(mc.viewOn());
    CHECK(mc.commandAvailability(QStringLiteral("multicam.selectCamera2")).value(QStringLiteral("enabled")).toBool());
    CHECK_FALSE(mc.commandAvailability(QStringLiteral("multicam.selectCamera5")).value(QStringLiteral("enabled")).toBool());
    CHECK(mc.runCommand(QStringLiteral("multicam.selectCamera2")));
    CHECK(angleRuns(controller, false) == "0:2");
    controller.setPlayhead(seconds(2));
    CHECK(mc.runCommand(QStringLiteral("multicam.cutToCamera3")));
    CHECK(angleRuns(controller, false) == "0:2 2:3");
    mc.setViewOn(false);

    // Flatten the piece at 2 s: C's own clip from where the multicam clip showed it.
    const auto loc = controller.currentSequence()->videoTracks[0]->clipIndexAt(seconds(2.5));
    REQUIRE(loc);
    const ClipId piece = controller.currentSequence()->videoTracks[0]->clips[*loc].id;
    const auto before = controller.session().undoCount();
    INFO(mc.message().toStdString());
    CHECK(mc.flatten(clipKey(piece)).isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const auto& v1 = controller.currentSequence()->videoTracks[0]->clips;
    const auto flat = controller.currentSequence()->videoTracks[0]->clipIndexAt(seconds(2.5));
    REQUIRE(flat);
    CHECK_FALSE(v1[*flat].isNested());
    CHECK(v1[*flat].media == tl.media('C'));
    CHECK_FALSE(mc.flatten(clipKey(v1[*flat].id)).isEmpty()); // not a multicam clip any more
}

// Review wave 5, A5-P2-20. The bridge made the app's audio services once, for the project file open at its first use, and
// kept them: the stems and takes of project B then went next to A. Fails today: the services are not made again for B.
TEST_CASE("Multicam services are made again when another project file is open", "[app][multicam]") {
    Timeline tl(emptyPicture());
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    MulticamEditBridge mc;
    mc.setController(&controller);
    mc.setSynchronous(true);
    std::vector<std::string> builtFor;
    mc.setServiceFactory([&controller, &builtFor] {
        builtFor.push_back(pathToUtf8(controller.projectFile()));
        return syncServices(std::make_shared<FixedSync>());
    });
    const std::filesystem::path dir = std::filesystem::path(WABCUT_TEST_OUTPUT_DIR) / "app-multicam-services";
    const QString noClip = clipKey(ClipId{987654}); // multicam.flatten fails on it, after the services were asked for
    controller.setProjectFile(dir / "A.wabcut");
    CHECK_FALSE(mc.flatten(noClip).isEmpty());
    CHECK_FALSE(mc.flatten(noClip).isEmpty());
    CHECK(builtFor == std::vector<std::string>{pathToUtf8(dir / "A.wabcut")}); // the same project: made once
    controller.setProjectFile(dir / "B.wabcut");
    CHECK_FALSE(mc.flatten(noClip).isEmpty());
    CHECK(builtFor == std::vector<std::string>{pathToUtf8(dir / "A.wabcut"), pathToUtf8(dir / "B.wabcut")});
}

// Review wave 5, A5-P1-8. A bridge job that threw (path::string() on a name outside the code page did) ended the app:
// std::terminate on its worker thread, or the exception thrown out of start() into a Qt slot when synchronous. It ends as
// a failure now, with the exception's message. Fails today: the synchronous start() lets the exception out.
TEST_CASE("A bridge job that throws ends as a failure on the calling thread and on its worker", "[app][multicam]") {
    BridgeJob job;
    QObject context;
    const auto throwing = [](const std::atomic<bool>&) -> BridgeJob::Finish {
        throw std::runtime_error("No mapping for the Unicode character exists in the target multi-byte code page.");
    };
    bool started = false;
    REQUIRE_NOTHROW(started = job.start(&context, true, throwing));
    CHECK(started);
    CHECK_FALSE(job.running());

    QString failure;
    const auto failed = [&failure](const QString& message) -> BridgeJob::Finish {
        return [&failure, message] { failure = message; };
    };
    REQUIRE(job.start(&context, true, throwing, failed));
    CHECK(failure.startsWith(QStringLiteral("No mapping for the Unicode character")));

    // On the worker, with the job's default failure: the result comes through the event loop.
    failure.clear();
    job.setFailed(failed);
    REQUIRE(job.start(&context, false, throwing));
    REQUIRE(waitFor([&job] { return !job.running(); }, 5000));
    CHECK(failure.startsWith(QStringLiteral("No mapping for the Unicode character")));
}

TEST_CASE("Multi-camera view in the shell: Shift+0 shows the grid, 1-9 select, Ctrl+1-9 cut; screenshot",
          "[app][multicam][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* mc = shell.engine.singletonInstance<MulticamEditBridge*>("Wabcut", "Multicam");
    REQUIRE(controller);
    REQUIRE(mc);
    mc->setSynchronous(true);
    Timeline tl(emptyPicture());
    const Cams cams = addTitledCams(tl.state, tl.seq, 4);
    controller->resetProject(cams.state, cams.main);
    placeMulticam(*controller, cams);
    controller->setPlayhead(seconds(1));
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* lanes = findNamed(w->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    lanes->forceActiveFocus();

    // Shift+0: Toggle Multi-Camera View.
    sendKey(w, Qt::Key_ParenRight, Qt::ShiftModifier, QStringLiteral(")"));
    REQUIRE(mc->viewOn());
    QQuickItem* view = shell.item("multicamView");
    if (!view)
        view = findNamed(w->contentItem(), QStringLiteral("multicamView"));
    REQUIRE(view);
    REQUIRE_FALSE(mc->gridImage().isNull());
    // 2: Select Camera 2 (paused: the clip under the playhead switches).
    sendKey(w, Qt::Key_2, {}, QStringLiteral("2"));
    CHECK(angleRuns(*controller, false) == "0:2");
    // Ctrl+3 at 2 s: Cut to Camera 3.
    controller->setPlayhead(seconds(2));
    lanes->forceActiveFocus();
    sendKey(w, Qt::Key_3, Qt::ControlModifier);
    CHECK(angleRuns(*controller, false) == "0:2 2:3");
    // A click on an angle cell selects it.
    spin(50);
    REQUIRE_FALSE(shell.grab().isNull());
    QQuickItem* cell4 = findNamed(view, QStringLiteral("multicamAngle_4"));
    REQUIRE(cell4);
    REQUIRE(QMetaObject::invokeMethod(cell4, "activate", Q_ARG(QVariant, false)));
    CHECK(angleRuns(*controller, false) == "0:2 2:4");
    // The active angle's cell is marked.
    spin(50);
    CHECK(cell4->property("active").toBool());

    // The app's path: the grid worker draws asynchronously; stepping on reuses its open source.
    mc->setSynchronous(false);
    const auto framesDrawn = [&] { return mc->gridStats().value(QStringLiteral("frames")).toInt(); };
    REQUIRE(waitFor([&] { return framesDrawn() >= 1; }, 20000));
    for (int step = 1; step <= 6; ++step) {
        const int before = framesDrawn();
        controller->setPlayhead(seconds(2.0 + step * 0.04));
        REQUIRE(waitFor([&] { return framesDrawn() > before; }, 10000));
    }
    INFO(mc->gridStats().value(QStringLiteral("error")).toString().toStdString());
    CHECK(mc->gridStats().value(QStringLiteral("error")).toString().isEmpty());
    CHECK_FALSE(mc->gridImage().isNull());
    CHECK(mc->gridStats().value(QStringLiteral("steadyFrames")).toInt() >= 5); // stepping on did not reopen it
    CHECK(mc->gridStats().value(QStringLiteral("openMs")).toDouble() > 0);

    // Screenshot: the multi-camera view beside the program picture, and the timeline's angle names.
    auto* timeline = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(timeline);
    timeline->setPixelsPerSecond(220);
    timeline->setScrollX(0);
    spin(100);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* monitor = shell.item("monitorPanel");
    REQUIRE(monitor);
    saveCrop(image, monitor, QStringLiteral("multicam-view.png"));
    REQUIRE(image.save(QDir(outputDir()).filePath(QStringLiteral("multicam-shell.png"))));
    CHECK(timeline->stats().angleCuts >= 1);

    // Shift+0 again: back to the program picture alone.
    lanes->forceActiveFocus();
    sendKey(w, Qt::Key_ParenRight, Qt::ShiftModifier, QStringLiteral(")"));
    CHECK_FALSE(mc->viewOn());
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// ---- Nesting ---------------------------------------------------------------------------------------------------------

TEST_CASE("Nest, open the nested clip as a sequence tab, the breadcrumb back, reveal in the Library, unnest",
          "[app][nesting]") {
    Timeline tl(R"(
        V1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
        A1 tgt |AaaaaaaaaaBbbbbbbbbbCccccccccc|
    )");
    tl.linkMatching();
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    SequenceTabsBridge tabs;
    tabs.setController(&controller);
    LibraryBridge library;
    library.setController(&controller);
    NestingBridge nesting;
    nesting.setController(&controller);
    nesting.setTabs(&tabs);
    nesting.setLibrary(&library);
    int libraryRequests = 0;
    QString shown;
    QObject::connect(&nesting, &NestingBridge::libraryRequested, [&] { ++libraryRequests; });
    QObject::connect(&library, &LibraryBridge::itemShown, [&](const QString& key) { shown = key; });
    const QString parentKey = sequenceKey(tl.seq);

    CHECK_FALSE(nesting.canNest());
    CHECK_FALSE(nesting.nestSelection({}).isEmpty());
    CHECK(nesting.defaultName() == QStringLiteral("Nested Sequence 01"));
    controller.setSelection({tl.clip("V1@0"), tl.clip("V1@10")});
    REQUIRE(nesting.canNest());
    const auto before = controller.session().undoCount();
    REQUIRE(nesting.nestSelection(QStringLiteral("  ")).isEmpty()); // blank: the default name
    CHECK(controller.session().undoCount() == before + 1);
    CHECK(nesting.defaultName() == QStringLiteral("Nested Sequence 02"));
    const SequencePtr parent = controller.currentSequence();
    REQUIRE(parent->videoTracks[0]->clips.size() == 2); // the nest and C
    const Clip& nest = parent->videoTracks[0]->clips[0];
    REQUIRE(nest.isNested());
    CHECK(parent->audioTracks[0]->clips[0].isNested()); // the linked sound went with it
    const QString nestKey = clipKey(nest.id);
    const SequenceId nestedId = nest.sequence;

    QVariantMap info = nesting.clipInfo(nestKey);
    CHECK(info.value(QStringLiteral("nested")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("multicam")).toBool());
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Nested Sequence 01"));
    CHECK(info.value(QStringLiteral("sequence")).toString() == sequenceKey(nestedId));
    CHECK_FALSE(nesting.clipInfo(clipKey(parent->videoTracks[0]->clips[1].id)).value(QStringLiteral("nested")).toBool());
    CHECK(nesting.breadcrumb().isEmpty());

    // Open: a tab for the nested sequence, the playhead on the frame the clip showed (frame 12 in).
    controller.setPlayhead(seconds(0.48));
    REQUIRE(nesting.openNested(nestKey));
    CHECK(tabs.current() == sequenceKey(nestedId));
    CHECK(controller.currentSequenceId() == nestedId);
    CHECK(controller.playhead() == seconds(0.48));
    QVariantList crumbs = nesting.breadcrumb();
    REQUIRE(crumbs.size() == 2);
    CHECK(crumbs[0].toMap().value(QStringLiteral("key")).toString() == parentKey);
    CHECK_FALSE(crumbs[0].toMap().value(QStringLiteral("current")).toBool());
    CHECK(crumbs[1].toMap().value(QStringLiteral("current")).toBool());
    CHECK(crumbs[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("Nested Sequence 01"));
    // A clip that is not nested does not open.
    const QString inner = clipKey(controller.currentSequence()->videoTracks[0]->clips[0].id);
    CHECK_FALSE(nesting.openNested(inner));
    CHECK_FALSE(nesting.message().isEmpty());

    // Back through the breadcrumb.
    REQUIRE(nesting.goTo(parentKey));
    CHECK(controller.currentSequenceId() == tl.seq);
    CHECK(tabs.count() == 2);

    // Reveal: the Library comes forward and shows the nested sequence.
    REQUIRE(nesting.revealInLibrary(nestKey));
    CHECK(libraryRequests == 1);
    CHECK(shown == sequenceKey(nestedId));

    // Unnest: the clips come back; one undo step.
    const auto beforeUnnest = controller.session().undoCount();
    CHECK(nesting.unnest(nestKey).isEmpty());
    CHECK(controller.session().undoCount() == beforeUnnest + 1);
    CHECK(controller.currentSequence()->videoTracks[0]->clips.size() == 3);
    CHECK_FALSE(nesting.unnest(clipKey(controller.currentSequence()->videoTracks[0]->clips[0].id)).isEmpty());
}

TEST_CASE("Timeline: nested and multi-camera clips get their stripe, the angle name and angle cuts; a double-click "
          "on one asks to open it",
          "[app][nesting][timeline]") {
    Rig rig(emptyPicture(150), 6.0, false);
    const Cams cams = addTitledCams(rig.tl.state, rig.tl.seq, 3);
    rig.controller.resetProject(cams.state, rig.tl.seq);
    placeMulticam(rig.controller, cams);
    rig.harness.frame();
    TimelineView* v = rig.view;
    CHECK(v->stats().nestedClips == 1);
    CHECK(v->stats().angleCuts == 0);
    const ClipId first = rig.controller.currentSequence()->videoTracks[0]->clips[0].id;
    CHECK(v->stats().labelTexts.value(first.value) == QStringLiteral("MC1 · Cam 1"));

    // An angle switch at 2 s: two pieces, the cut marked, each named after its angle.
    const auto cut = CommandJob::runNow(&rig.controller, nullptr, "multicam.switchAngle",
                                        Json{{"clip", first.value}, {"angle", 3}, {"at", seconds(2).value}});
    REQUIRE(cut.ok());
    for (int i = 0; i < 3; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
    CHECK(v->stats().nestedClips == 2);
    CHECK(v->stats().angleCuts == 1);
    const SequencePtr cutSeq = rig.controller.currentSequence();
    const auto& clips = cutSeq->videoTracks[0]->clips;
    REQUIRE(clips.size() == 2);
    CHECK(v->stats().labelTexts.value(clips[1].id.value) == QStringLiteral("MC3 · Cam 3"));
    const QString secondKey = clipKey(clips[1].id);

    // A double-click on a clip asks for it; elsewhere it does not.
    QString asked;
    QObject::connect(v, &TimelineView::clipDoubleClicked, [&](const QString& key) { asked = key; });
    // The platform's sequence: press, release, press, double-click, release.
    const auto doubleClick = [&](QPointF at) {
        rig.press(at);
        rig.release(at);
        rig.press(at);
        rig.send(QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton, {});
        rig.release(at);
    };
    doubleClick(QPointF(rig.x(70), rig.y("V1")));
    CHECK(asked == secondKey);
    asked.clear();
    doubleClick(QPointF(rig.x(20), rig.y("A2")));
    CHECK(asked.isEmpty());
}

TEST_CASE("Nested sequence tab in the shell: Nest from the clip menu, double-click opens it, the breadcrumb leads back; "
          "screenshot",
          "[app][nesting][qml][screenshot]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* nesting = shell.engine.singletonInstance<NestingBridge*>("Wabcut", "Nesting");
    auto* tabs = shell.engine.singletonInstance<SequenceTabsBridge*>("Wabcut", "SequenceTabs");
    REQUIRE(controller);
    REQUIRE(nesting);
    REQUIRE(tabs);
    const auto clipsOf = [](char letter, int frames) {
        return std::string(1, letter) + std::string(static_cast<std::size_t>(frames - 1), static_cast<char>(letter + 32));
    };
    Timeline tl("V1 tgt |" + clipsOf('A', 60) + clipsOf('B', 70) + clipsOf('C', 50) + clipsOf('D', 80) + "|\nA1 tgt |" +
                clipsOf('A', 60) + clipsOf('B', 70) + clipsOf('C', 50) + clipsOf('D', 80) + "|");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    QQuickWindow* w = shell.window.get();
    REQUIRE_FALSE(shell.grab().isNull());

    // The clip menu: Nest… is offered for the selection and opens the name dialog.
    controller->setSelection({tl.clip("V1@60"), tl.clip("V1@130")});
    QObject* menu = w->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(menu);
    QQuickItem* lanes = findNamed(w->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, clipKey(tl.clip("V1@60"))), Q_ARG(QVariant, QPointF(200, 20))));
    QObject* nestItem = menu->findChild<QObject*>(QStringLiteral("nestMenuItem"));
    REQUIRE(nestItem);
    CHECK(nestItem->property("enabled").toBool());
    CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("openNestedMenuItem"))->property("visible").toBool());
    REQUIRE(QMetaObject::invokeMethod(nestItem, "triggered"));
    QMetaObject::invokeMethod(menu, "close");
    QObject* dialog = w->findChild<QObject*>(QStringLiteral("nestDialog"));
    REQUIRE(dialog);
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 5000));
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    QQuickItem* nameField = findNamed(content->parentItem(), QStringLiteral("nestName"));
    REQUIRE(nameField);
    CHECK(nameField->property("text").toString() == QStringLiteral("Nested Sequence 01"));
    nameField->setProperty("text", QStringLiteral("Interview Cut"));
    QQuickItem* ok = findNamed(content->parentItem(), QStringLiteral("nestOk"));
    REQUIRE(ok);
    REQUIRE(QMetaObject::invokeMethod(ok, "clicked"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 5000));
    const SequencePtr parent = controller->currentSequence();
    const auto index = parent->videoTracks[0]->clipIndexAt(seconds(3));
    REQUIRE(index);
    const Clip& nest = parent->videoTracks[0]->clips[*index];
    REQUIRE(nest.isNested());
    const QString nestKey = clipKey(nest.id);
    const SequenceId nestedId = nest.sequence;

    // The clip menu of the nested clip: open / reveal / unnest, no flatten.
    REQUIRE(QMetaObject::invokeMethod(menu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, nestKey), Q_ARG(QVariant, QPointF(300, 20))));
    CHECK(menu->findChild<QObject*>(QStringLiteral("openNestedMenuItem"))->property("visible").toBool());
    CHECK(menu->findChild<QObject*>(QStringLiteral("revealNestedMenuItem"))->property("visible").toBool());
    CHECK(menu->findChild<QObject*>(QStringLiteral("unnestMenuItem"))->property("visible").toBool());
    CHECK_FALSE(menu->findChild<QObject*>(QStringLiteral("flattenMenuItem"))->property("visible").toBool());
    QMetaObject::invokeMethod(menu, "close");
    spin(50);

    // A double-click on the nested clip opens it in a tab; the breadcrumb shows the way back.
    auto* view = qobject_cast<TimelineView*>(shell.item("timelineView"));
    REQUIRE(view);
    emit view->clipDoubleClicked(nestKey);
    REQUIRE(waitFor([&] { return controller->currentSequenceId() == nestedId; }, 3000));
    CHECK(tabs->current() == sequenceKey(nestedId));
    CHECK(nesting->breadcrumb().size() == 2);
    spin(100);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    QQuickItem* crumbs = findNamed(w->contentItem(), QStringLiteral("nestedBreadcrumb"));
    REQUIRE(crumbs);
    CHECK(crumbs->isVisible());
    QQuickItem* panel = shell.item("timelinePanel");
    REQUIRE(panel);
    saveCrop(image, panel, QStringLiteral("nested-sequence-tab.png"));

    // The parent's crumb goes back.
    QQuickItem* back = findNamed(crumbs, QStringLiteral("nestedCrumb_") + sequenceKey(tl.seq));
    REQUIRE(back);
    REQUIRE(QMetaObject::invokeMethod(back, "clicked"));
    REQUIRE(waitFor([&] { return controller->currentSequenceId() == tl.seq; }, 3000));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

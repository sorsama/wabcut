// ProgramMonitor (QQuickRhiItem) end to end: a PlaybackController decodes a test-media frame, the monitor
// presents it on the scene graph's QRhi, and the window is grabbed offscreen (never shown). Screenshots land
// in <build>/test-output/render/.

#include "ProgramMonitor.h"
#include "RhiPolicy.h"

#include <wabcut/media/Probe.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/render/OffscreenRenderer.h>
#include <wabcut/playback/video/PlaybackController.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QtQml/QQmlExtensionPlugin>

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <cmath>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

Q_IMPORT_QML_PLUGIN(WabcutPlugin)

using namespace wabcut;

namespace {

int g_argc = 1;
char g_argv0[] = "wabcut_monitor_tests";
char* g_argv[] = {g_argv0, nullptr};
QGuiApplication* g_app = nullptr;
std::unique_ptr<QTemporaryDir> g_settings;

class QtApplicationListener final : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testRunStarting(const Catch::TestRunInfo&) override {
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0"); // deterministic pixels
        QCoreApplication::setOrganizationName(QStringLiteral("WabcutTests"));
        QCoreApplication::setOrganizationDomain(QStringLiteral("tests.wabcut.org"));
        QCoreApplication::setApplicationName(QStringLiteral("wabcut_monitor_tests"));
        g_settings = std::make_unique<QTemporaryDir>();
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, g_settings->path());
        g_app = new QGuiApplication(g_argc, g_argv);
        QQuickStyle::setStyle(QStringLiteral("Basic"));
        using namespace wabcut::app;
        applyRhiBackend(resolveRhiBackend({}, qgetenv("QSG_RHI_BACKEND"), currentRhiPlatform()));
    }
    void testRunEnded(const Catch::TestRunStats&) override {
        delete g_app;
        g_app = nullptr;
        g_settings.reset();
    }
};

void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

bool waitFor(const std::function<bool()>& condition, int timeoutMs = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        spin(10);
    }
    return true;
}

QString outputDir() {
    const QString dir = QStringLiteral(WABCUT_TEST_OUTPUT_DIR "/render");
    QDir().mkpath(dir);
    return dir;
}

std::string mediaPath(const char* name) {
    const std::u8string p = (std::filesystem::path(WABCUT_TEST_MEDIA_DIR) / name).generic_u8string();
    return std::string(reinterpret_cast<const char*>(p.data()), p.size());
}

/// One 1080p60 sequence with the H.264 test clip on V1 (sequence frame n shows source frame n).
struct ClipProject {
    ProjectState state;
    SequenceId sequence;

    explicit ClipProject(const char* clip, EffectStack effects = {}, ClipProperties properties = {}) {
        auto info = media::probe(mediaPath(clip));
        REQUIRE(info.has_value());
        auto item = std::make_shared<MediaItem>();
        item->id = state.ids.allocate<MediaId>();
        item->locator.path = mediaPath(clip);
        item->info = std::move(*info);
        Track track;
        track.id = state.ids.allocate<TrackId>();
        Clip c;
        c.id = state.ids.allocate<ClipId>();
        c.media = item->id;
        c.timelineRange = {Ticks{}, Ticks::fromSeconds(10)};
        if (!effects.empty() || properties != ClipProperties{}) {
            ClipContent content;
            content.properties = properties;
            content.effects = std::move(effects);
            for (EffectInstance& e : content.effects) {
                e.id = state.ids.allocate<EffectInstanceId>();
            }
            c.setContent(std::move(content));
        }
        track.clips.push_back(c);
        auto seq = std::make_shared<Sequence>();
        seq->id = state.ids.allocate<SequenceId>();
        seq->frameRate = kRate60;
        seq->videoTracks.push_back(std::make_shared<const Track>(std::move(track)));
        seq->duration = seq->contentEnd();
        sequence = seq->id;
        state.media.push_back(std::move(item));
        state.sequences.push_back(std::move(seq));
    }
};

/// Frame number from the burnt-in barcode inside a picture placed at `rect` (scaled from 1920x1080).
std::int64_t barcodeIn(const QImage& image, const QRect& rect) {
    const double s = rect.width() / 1920.0;
    std::int64_t index = 0;
    for (int bit = 0; bit < 16; ++bit) {
        const QColor c = image.pixelColor(rect.x() + static_cast<int>((bit * 32 + 16) * s),
                                          rect.y() + static_cast<int>(16 * s));
        if (c.green() > 128) {
            index |= std::int64_t{1} << bit;
        }
    }
    return index;
}

bool haveClip() {
    return std::filesystem::is_regular_file(std::filesystem::path(WABCUT_TEST_MEDIA_DIR) /
                                            "h264_1080p60_aac.mp4");
}

} // namespace

CATCH_REGISTER_LISTENER(QtApplicationListener)

TEST_CASE("ProgramMonitor presents a decoded frame, letterboxed, at the effective DPR", "[monitor][gpu]") {
    if (!haveClip()) {
        SKIP("test media missing");
    }
    ClipProject project("h264_1080p60_aac.mp4");
    EditSession session(project.state);
    playback::PlaybackController controller;
    controller.setSession(&session);
    controller.setSequence(project.sequence);

    QQuickWindow window;
    window.setColor(Qt::darkGray);
    window.resize(960, 720); // 4:3: the 16:9 canvas is letterboxed top and bottom
    window.contentItem()->setSize(QSizeF(960, 720));
    auto* monitor = new app::ProgramMonitor(window.contentItem());
    monitor->setSize(QSizeF(960, 720));
    monitor->setSurroundColor(QColor(20, 20, 24));
    CHECK_FALSE(monitor->hasFrame());
    window.create(); // native handle for the offscreen grab; never shown

    // Before any frame: transparent, the window colour shows through.
    QImage before = window.grabWindow();
    REQUIRE_FALSE(before.isNull());
    CHECK(before.pixelColor(480, 360) == QColor(Qt::darkGray));
    CHECK_FALSE(monitor->hasFrame());
    monitor->setPlaybackController(&controller);

    const std::uint64_t publishedBefore = controller.stats().published;
    controller.seek(framesToTicks(37, kRate60));
    REQUIRE(waitFor([&] { return controller.stats().published > publishedBefore; }));
    spin(100); // deliver the monitor's queued wake-up (update())
    const QImage image = window.grabWindow();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(outputDir() + QStringLiteral("/program_monitor_decoded.png")));
    REQUIRE(waitFor([&] { return monitor->hasFrame(); }, 2000));
    INFO("render backend: " << monitor->renderBackend().toStdString());
    CHECK_FALSE(monitor->renderBackend().isEmpty());
    CHECK(controller.stats().renderBackend == monitor->renderBackend().toStdString());

    // Letterbox: 960 x 540 picture centred in 960 x 720 (90 px bars of the surround colour).
    const double dpr = window.effectiveDevicePixelRatio();
    CHECK(monitor->effectiveColorBufferSize() == QSize(qRound(960 * dpr), qRound(720 * dpr)));
    const QColor bar = image.pixelColor(480, 40);
    INFO("bar " << bar.name().toStdString());
    CHECK(std::abs(bar.red() - 20) <= 1);
    CHECK(std::abs(bar.blue() - 24) <= 1);
    CHECK(barcodeIn(image, QRect(0, 90, 960, 540)) == 37);
}

TEST_CASE("Program monitor in the full shell shows the decoded clip", "[monitor][gpu][shell]") {
    if (!haveClip()) {
        SKIP("test media missing");
    }
    ClipProject project("h264_1080p60_aac.mp4");
    EditSession session(project.state);
    playback::PlaybackController controller;
    controller.setSession(&session);
    controller.setSequence(project.sequence);

    QQmlEngine engine;
    QQmlComponent component(&engine, "Wabcut", "Main");
    REQUIRE_FALSE(component.isError());
    std::unique_ptr<QQuickWindow> window(qobject_cast<QQuickWindow*>(
        component.createWithInitialProperties({{QStringLiteral("visible"), false}})));
    REQUIRE(window);
    window->create();
    window->resize(1600, 960);
    window->contentItem()->setSize(QSizeF(1600, 960));
    auto* monitor = window->findChild<app::ProgramMonitor*>(QStringLiteral("programMonitor"));
    REQUIRE(monitor != nullptr);
    CHECK_FALSE(monitor->hasFrame());

    monitor->setPlaybackController(&controller);
    const std::uint64_t publishedBefore = controller.stats().published;
    controller.seek(framesToTicks(37, kRate60));
    REQUIRE(waitFor([&] { return controller.stats().published > publishedBefore; }));
    spin(200); // the monitor's queued wake-up; layouts and theme transitions settle
    const QImage image = window->grabWindow();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(outputDir() + QStringLiteral("/shell_program_monitor.png")));
    REQUIRE(waitFor([&] { return monitor->hasFrame(); }, 2000));

    // The monitor area shows the decoded frame (barcode 37), not the placeholder.
    const QPointF topLeft = monitor->mapToScene(QPointF(0, 0));
    const QRect area(qRound(topLeft.x()), qRound(topLeft.y()), qRound(monitor->width()),
                     qRound(monitor->height()));
    const double aspect = 1920.0 / 1080.0; // the sequence canvas is letterboxed into the monitor
    QRect fitted = area;
    if (area.width() / static_cast<double>(area.height()) > aspect) {
        fitted.setWidth(qRound(area.height() * aspect));
        fitted.moveLeft(area.x() + (area.width() - fitted.width()) / 2);
    } else {
        fitted.setHeight(qRound(area.width() / aspect));
        fitted.moveTop(area.y() + (area.height() - fitted.height()) / 2);
    }
    INFO("monitor picture " << fitted.x() << "," << fitted.y() << " " << fitted.width() << "x"
                            << fitted.height());
    CHECK(barcodeIn(image, fitted) == 37);
}

TEST_CASE("Preview equals offscreen (export path) rendering for an effects timeline", "[monitor][gpu][effects]") {
    if (!haveClip()) {
        SKIP("test media missing");
    }
    // A scaled, rotated clip with crop, blur, correction, vignette and a drop shadow.
    const auto instance = [](const char* id, std::vector<std::pair<std::string, double>> params) {
        EffectInstance e;
        e.effect = id;
        for (const auto& [key, value] : params) {
            e.set(key, ParamValue(value));
        }
        return e;
    };
    EffectStack effects = {
        instance("wabcut.crop", {{"left", 0.05}, {"right", 0.1}, {"feather", 20.0}}),
        instance("wabcut.blur.gaussian", {{"radius", 6.0}}),
        instance("wabcut.color.basic", {{"exposure", 0.4}, {"saturation", 140.0}, {"temperature", 30.0}}),
        instance("wabcut.vignette", {{"amount", -1.5}}),
        instance("wabcut.dropShadow", {{"opacity", 0.8}, {"distance", 30.0}, {"softness", 24.0}}),
    };
    ClipProperties motion;
    motion.set(ParamId::Scale, 0.8);
    motion.set(ParamId::Rotation, 4.0);
    ClipProject project("h264_1080p60_aac.mp4", effects, motion);
    EditSession session(project.state);
    playback::PlaybackController controller;
    controller.setSession(&session);
    controller.setSequence(project.sequence);

    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(960, 540); // the canvas aspect: no letterbox, the monitor area is the picture
    window.contentItem()->setSize(QSizeF(960, 540));
    auto* monitor = new app::ProgramMonitor(window.contentItem());
    monitor->setSize(QSizeF(960, 540));
    monitor->setSurroundColor(QColor(0, 0, 0));
    window.create();
    monitor->setPlaybackController(&controller);
    const std::uint64_t publishedBefore = controller.stats().published;
    controller.seek(framesToTicks(37, kRate60));
    REQUIRE(waitFor([&] { return controller.stats().published > publishedBefore; }));
    spin(100);
    const QImage preview = window.grabWindow().convertToFormat(QImage::Format_RGB888);
    REQUIRE_FALSE(preview.isNull());
    REQUIRE(waitFor([&] { return monitor->hasFrame(); }, 2000));
    if (window.effectiveDevicePixelRatio() != 1.0) {
        SKIP("needs a device pixel ratio of 1 to compare pixels");
    }
    const auto shown = controller.mailbox()->current();
    REQUIRE(shown);
    REQUIRE(shown->layers.size() == 1);
    CHECK(shown->layers[0].effects.size() == 5);

    // The same composite through the export renderer, display transform, at the monitor's size.
    render::OffscreenOptions options;
    options.role = render::OutputRole::Display;
    options.compositor = {render::TexturePool::Export, 1, 48};
    auto renderer = render::OffscreenRenderer::create(options);
    REQUIRE(renderer);
    auto exported = (*renderer)->render(shown, QSize(960, 540));
    REQUIRE(exported);
    double mse = 0.0;
    for (int y = 0; y < 540; ++y) {
        const std::uint8_t* row = preview.constScanLine(y);
        for (int x = 0; x < 960; ++x) {
            const std::uint8_t* e = exported->pixel(x, y);
            for (int c = 0; c < 3; ++c) {
                const double d = double(row[x * 3 + c]) - double(e[c]);
                mse += d * d;
            }
        }
    }
    mse /= 960.0 * 540.0 * 3.0;
    const double psnr = mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    preview.save(outputDir() + QStringLiteral("/effects_preview.png"));
    QImage(exported->rgba.data(), 960, 540, 960 * 4, QImage::Format_RGBA8888).save(outputDir() + QStringLiteral("/effects_export.png"));
    WARN("preview vs offscreen PSNR " << psnr << " dB");
    CHECK(psnr >= 45.0);
}

namespace {

/// A probed media item of the test clip, added to `state`.
MediaId addClipMedia(ProjectState& state, const char* clip) {
    auto info = media::probe(mediaPath(clip));
    REQUIRE(info.has_value());
    auto item = std::make_shared<MediaItem>();
    item->id = state.ids.allocate<MediaId>();
    item->locator.path = mediaPath(clip);
    item->info = std::move(*info);
    const MediaId id = item->id;
    state.media.push_back(std::move(item));
    return id;
}

Clip mediaClip(ProjectState& state, MediaId media, Ticks start, Ticks duration, Ticks sourceIn, ClipProperties p = {}) {
    Clip c;
    c.id = state.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {start, duration};
    c.sourceIn = sourceIn;
    c.setProperties(p);
    return c;
}

SequencePtr sequenceOf(ProjectState& state, std::vector<std::vector<Clip>> tracks, std::optional<MulticamInfo> multicam = {}) {
    auto seq = std::make_shared<Sequence>();
    seq->id = state.ids.allocate<SequenceId>();
    seq->frameRate = kRate60;
    for (std::vector<Clip>& clips : tracks) {
        Track track;
        track.id = state.ids.allocate<TrackId>();
        for (Clip& c : clips) {
            track.clips.push_back(c);
        }
        seq->videoTracks.push_back(std::make_shared<const Track>(std::move(track)));
    }
    if (multicam) {
        for (std::size_t i = 0; i < multicam->angles.size() && i < seq->videoTracks.size(); ++i) {
            multicam->angles[i].video = seq->videoTracks[i]->id;
        }
        seq->multicam = std::move(multicam);
    }
    seq->duration = seq->contentEnd();
    return seq;
}

/// The monitor's picture (960x540, no letterbox) once the controller published `frame` (60 fps).
QImage monitorPicture(ProjectState state, SequenceId sequence, std::int64_t frame, const char* screenshot) {
    EditSession session(std::move(state));
    playback::PlaybackController controller;
    controller.setSession(&session);
    controller.setSequence(sequence);
    QQuickWindow window;
    window.setColor(Qt::black);
    window.resize(960, 540);
    window.contentItem()->setSize(QSizeF(960, 540));
    auto* monitor = new app::ProgramMonitor(window.contentItem());
    monitor->setSize(QSizeF(960, 540));
    monitor->setSurroundColor(QColor(0, 0, 0));
    window.create();
    monitor->setPlaybackController(&controller);
    const std::uint64_t publishedBefore = controller.stats().published;
    controller.seek(framesToTicks(frame, kRate60));
    REQUIRE(waitFor([&] { return controller.stats().published > publishedBefore; }));
    spin(150);
    (void)window.grabWindow(); // the window is never shown: a grab is what renders it
    REQUIRE(waitFor([&] { return monitor->hasFrame(); }, 2000));
    const QImage image = window.grabWindow();
    REQUIRE_FALSE(image.isNull());
    CHECK(image.save(outputDir() + QStringLiteral("/%1.png").arg(QString::fromLatin1(screenshot))));
    return image;
}

} // namespace

TEST_CASE("Program monitor shows a nested clip and a multicam program output (C.8)", "[monitor][gpu][nested]") {
    if (!haveClip()) {
        SKIP("test media missing");
    }
    SECTION("a nested clip at 75 %, with a picture-in-picture inside it") {
        ProjectState state;
        const MediaId media = addClipMedia(state, "h264_1080p60_aac.mp4");
        ClipProperties pip;
        pip.set(ParamId::Scale, 0.35);
        pip.set(ParamId::PositionX, 560.0);
        pip.set(ParamId::PositionY, 300.0);
        SequencePtr nested = sequenceOf(state, {{mediaClip(state, media, Ticks{}, Ticks::fromSeconds(5), Ticks{})},
                                                {mediaClip(state, media, Ticks{}, Ticks::fromSeconds(5), Ticks::fromSeconds(3), pip)}});
        Clip nestClip;
        nestClip.id = state.ids.allocate<ClipId>();
        nestClip.sequence = nested->id;
        nestClip.stream = {StreamKind::Video, 0};
        nestClip.timelineRange = {Ticks{}, Ticks::fromSeconds(5)};
        ClipProperties smaller;
        smaller.set(ParamId::Scale, 0.75);
        nestClip.setProperties(smaller);
        SequencePtr program = sequenceOf(state, {{nestClip}});
        const SequenceId programId = program->id;
        state.sequences.push_back(nested);
        state.sequences.push_back(program);
        const QImage image = monitorPicture(state, programId, 37, "monitor_nested_clip");
        // The nested picture at 75 % of the 960x540 monitor: 720x405 at (120, 67.5); V1's barcode in its top-left corner.
        CHECK(barcodeIn(image, QRect(120, 68, 720, 405)) == 37);
        CHECK(image.pixelColor(40, 270) == QColor(0, 0, 0)); // outside the nested clip: nothing
    }
    SECTION("a four-angle multicam clip cut between angles") {
        ProjectState state;
        const MediaId media = addClipMedia(state, "h264_1080p60_aac.mp4");
        // Four angles of the recording, 0, 100, 200 and 300 frames later in the source: each shows its own frame number.
        std::vector<std::vector<Clip>> angles;
        MulticamInfo info;
        info.sync = MulticamSync::Manual;
        for (int a = 0; a < 4; ++a) {
            angles.push_back({mediaClip(state, media, Ticks{}, Ticks::fromSeconds(5), framesToTicks(100 * a, kRate60))});
            info.angles.push_back({"Camera " + std::to_string(a + 1), {}, {}, Ticks{}});
        }
        SequencePtr source = sequenceOf(state, std::move(angles), info);
        std::vector<Clip> cuts;
        for (int k = 0; k < 4; ++k) {
            Clip c;
            c.id = state.ids.allocate<ClipId>();
            c.sequence = source->id;
            c.angles = AngleSet::of(static_cast<std::size_t>((k + 2) % 4)); // angles 3, 4, 1, 2
            c.stream = {StreamKind::Video, 0};
            c.timelineRange = {framesToTicks(30 * k, kRate60), framesToTicks(30, kRate60)};
            c.sourceIn = framesToTicks(30 * k, kRate60);
            cuts.push_back(c);
        }
        SequencePtr program = sequenceOf(state, {cuts});
        const SequenceId programId = program->id;
        state.sequences.push_back(source);
        state.sequences.push_back(program);
        // Frame 45 is in the second cut: angle 4, source frame 45 + 300.
        const QImage image = monitorPicture(state, programId, 45, "monitor_multicam_program");
        CHECK(barcodeIn(image, QRect(0, 0, 960, 540)) == 345);
        const QImage first = monitorPicture(state, programId, 10, "monitor_multicam_program_angle3");
        CHECK(barcodeIn(first, QRect(0, 0, 960, 540)) == 210);
    }
}
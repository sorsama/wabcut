// M5o (gaming): the Export panel's gaming presets and their size / time estimate, offscreen; no window is shown.
//   * Each preset's settings: size, frame rate (capped, never raised), codec, container, bitrate.
//   * Discord size targets: the bitrate fills 95 % of the target and the estimate stays under it, for short and long
//     ranges; a range too long for any picture is refused with the length that fits.
//   * The estimate follows the range (In to Out) and the encoder.
//   * GIF and WebM use the exporter's own presets (I.9: gif-480p15, webm-vp9-720p).
//   * In the window: the cards, choosing one drives the panel's summary and estimate; a screenshot.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "ExportController.h"
#include "GamingExport.h"
#include "ProjectController.h"

#include <wabcut/export/ExportSettings.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QKeyEvent>
#include <QQuickItem>
#include <QQuickWindow>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;
namespace ex = wabcut::exporter;

namespace {

/// A project whose one sequence is `width` x `height` at `rate`, `seconds` long, with In / Out when given.
struct Rig {
    Timeline tl{"V1 |....|"};
    ProjectController controller;
    GamingExport gaming;

    Rig(Rate rate, int width, int height, double seconds, std::optional<std::pair<double, double>> marks = {}) {
        Sequence s = tl.sequence();
        s.frameRate = rate;
        s.width = width;
        s.height = height;
        s.duration = Ticks::fromSeconds(seconds);
        if (marks) {
            s.markIn = Ticks::fromSeconds(marks->first);
            s.markOut = Ticks::fromSeconds(marks->second);
        }
        const ProjectState state = tl.state.withSequence(std::make_shared<const Sequence>(std::move(s)));
        controller.resetProject(state, tl.seq);
        gaming.setController(&controller);
    }

    /// The settings a gaming preset exports with, over `range`.
    ex::ExportSettings settings(const char* id, const char* range = "whole") {
        const QVariantMap plan = gaming.plan(QString::fromLatin1(id), QString(), QString::fromLatin1(range));
        INFO(id << ": " << plan.value(QStringLiteral("error")).toString().toStdString());
        REQUIRE(plan.value(QStringLiteral("ok")).toBool());
        const auto s = ExportController::settingsFor(plan.value(QStringLiteral("presetId")).toString(),
                                                     plan.value(QStringLiteral("overrides")).toMap());
        INFO(id);
        REQUIRE(s);
        return *s;
    }
};

double mbps(std::int64_t bitsPerSecond) {
    return static_cast<double>(bitsPerSecond) / 1e6;
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

void tap(QQuickWindow* window, int key, const QString& text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &press);
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QCoreApplication::sendEvent(window, &release);
}

QQuickItem* named(QQuickItem* root, const char* name) {
    return findWith(root, [&](QQuickItem* i) { return i->objectName() == QLatin1String(name); });
}

} // namespace

TEST_CASE("Gaming export presets: size, frame rate, codec, container and bitrate of each", "[app][export]") {
    Rig r(Rate{60, 1}, 2560, 1440, 120.0);

    struct Expect {
        const char* id;
        int width;
        int height;
        double videoMbps;
        double maxMbps;
        int audioKbps;
        ex::ScaleMode scale;
    };
    const Expect expected[] = {
        {"youtube-1080p60", 1920, 1080, 18, 27, 384, ex::ScaleMode::Fit},
        {"youtube-1440p60", 2560, 1440, 30, 45, 384, ex::ScaleMode::Fit},
        {"youtube-4k60", 3840, 2160, 68, 85, 384, ex::ScaleMode::Fit},
        {"vertical-1080x1920-60", 1080, 1920, 15, 22, 320, ex::ScaleMode::Fill},
        {"twitch-1080p60", 1920, 1080, 8, 10, 160, ex::ScaleMode::Fit},
    };
    for (const Expect& e : expected) {
        INFO(e.id);
        const ex::ExportSettings s = r.settings(e.id);
        CHECK(s.container == ex::Container::Mp4);
        CHECK(s.video.codec == ex::VideoCodec::H264);
        CHECK(s.video.encoder == "auto"); // hardware first
        CHECK(s.video.rateControl == ex::RateControl::Bitrate);
        CHECK(*s.video.width == e.width);
        CHECK(*s.video.height == e.height);
        CHECK(mbps(s.video.bitrate) == Catch::Approx(e.videoMbps));
        CHECK(mbps(s.video.maxBitrate) == Catch::Approx(e.maxMbps));
        CHECK(s.video.scaleMode == e.scale);
        CHECK(s.audio.codec == ex::AudioCodec::Aac);
        CHECK(s.audio.bitrate == e.audioKbps * 1000);
        CHECK_FALSE(s.video.frameRate.has_value()); // a 60 fps sequence stays at its own rate
    }
    // The 1440p sequence upscaled for YouTube 4K60 says so.
    const QVariantMap plan = r.gaming.plan(QStringLiteral("youtube-4k60"), QString(), QStringLiteral("whole"));
    CHECK(plan.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("Upscaled")));
}

TEST_CASE("Gaming export presets: frame rates are capped at 60, never raised", "[app][export]") {
    {
        Rig high(Rate{144, 1}, 1920, 1080, 30.0); // a 144 Hz capture
        const ex::ExportSettings s = high.settings("youtube-1080p60");
        REQUIRE(s.video.frameRate.has_value());
        CHECK(*s.video.frameRate == Rate{60, 1});
        const QVariantMap plan = high.gaming.plan(QStringLiteral("youtube-1080p60"), QString(), QStringLiteral("whole"));
        CHECK(plan.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("144 fps")));
    }
    {
        Rig thirty(Rate{30, 1}, 1920, 1080, 30.0);
        CHECK_FALSE(thirty.settings("youtube-1080p60").video.frameRate.has_value());
    }
    {
        Rig ntsc(Rate{60000, 1001}, 1920, 1080, 30.0); // 59.94 stays 59.94
        CHECK_FALSE(ntsc.settings("youtube-1080p60").video.frameRate.has_value());
    }
    // The override key itself: "60", "59.94" and "60000/1001"; 0 = the sequence's.
    for (const auto& [text, rate] : {std::pair{"60", Rate{60, 1}}, std::pair{"59.94", Rate{60000, 1001}},
                                     std::pair{"30000/1001", Rate{30000, 1001}}}) {
        const auto s = ExportController::settingsFor(QStringLiteral("youtube-1080p"),
                                                     {{QStringLiteral("frameRate"), QString::fromLatin1(text)}});
        INFO(text);
        REQUIRE(s);
        REQUIRE(s->video.frameRate.has_value());
        CHECK(*s->video.frameRate == rate);
    }
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("youtube-1080p"), {{QStringLiteral("frameRate"), 0}})
                    ->video.frameRate.has_value());
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("youtube-1080p"), {{QStringLiteral("frameRate"), QStringLiteral("fast")}}));
    CHECK_FALSE(ExportController::settingsFor(QStringLiteral("youtube-1080p"), {{QStringLiteral("frameRate"), 47.3}}));
    // A tight cap for size targets.
    const auto capped = ExportController::settingsFor(
        QStringLiteral("youtube-1080p"), {{QStringLiteral("bitrateMbps"), 4.0}, {QStringLiteral("maxBitrateMbps"), 4.0}});
    REQUIRE(capped);
    CHECK(capped->video.maxBitrate == capped->video.bitrate);
}

TEST_CASE("Gaming export presets: Discord size targets fit 95 percent of the target for short and long ranges",
          "[app][export]") {
    // The arithmetic: the streams fill at most 95 % of the target with the container's share.
    for (const double seconds : {5.0, 30.0, 120.0, 600.0}) {
        for (const std::int64_t target : {std::int64_t{10'000'000}, std::int64_t{25'000'000}, std::int64_t{50'000'000}}) {
            const std::int64_t video = GamingExport::targetVideoBitrate(target, seconds, 128'000);
            if (video == 0)
                continue;
            const double bytes = (static_cast<double>(video) + 128'000.0) * seconds / 8.0 * 1.01;
            INFO(seconds << " s, " << target << " B");
            CHECK(bytes <= static_cast<double>(target) * 0.95 + 1);
        }
    }
    CHECK(GamingExport::targetVideoBitrate(10'000'000, 0.0, 128'000) == 0);

    // Through the presets: 30 s in 10 MB is 720p or less, under the target; 25 and 50 MB get more.
    Rig r(Rate{60, 1}, 1920, 1080, 30.0);
    double previous = 0;
    for (const char* id : {"discord-10mb", "discord-25mb", "discord-50mb"}) {
        INFO(id);
        const QVariantMap e = r.gaming.estimateFor(QString::fromLatin1(id), QString(), QStringLiteral("whole"));
        REQUIRE(e.value(QStringLiteral("ok")).toBool());
        const double target = std::string_view(id) == "discord-10mb"   ? 10e6
                              : std::string_view(id) == "discord-25mb" ? 25e6
                                                                       : 50e6;
        CHECK(e.value(QStringLiteral("bytes")).toDouble() < target);
        CHECK(e.value(QStringLiteral("bytes")).toDouble() > target * 0.85); // it uses the room it has
        CHECK(e.value(QStringLiteral("approximate")).toBool());
        CHECK(e.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("two-pass")));
        CHECK(e.value(QStringLiteral("videoMbps")).toDouble() > previous);
        previous = e.value(QStringLiteral("videoMbps")).toDouble();
        // 0.1.2: the exporter's size target (two passes when the first ends over), not a one-pass bitrate.
        CHECK(e.value(QStringLiteral("presetId")).toString() == QString::fromLatin1(id));
        const ex::ExportSettings s = r.settings(id);
        CHECK(s.video.rateControl == ex::RateControl::TargetSize);
        CHECK(static_cast<double>(s.video.targetBytes) == target);
    }
    CHECK(*r.settings("discord-10mb").video.height <= 720);
    CHECK(*r.settings("discord-50mb").video.height == 1080);

    // A whole hour does not fit 10 MB: refused, with the length that does.
    Rig hour(Rate{60, 1}, 1920, 1080, 3600.0);
    const QVariantMap tooLong = hour.gaming.plan(QStringLiteral("discord-10mb"), QString(), QStringLiteral("whole"));
    CHECK_FALSE(tooLong.value(QStringLiteral("ok")).toBool());
    CHECK(tooLong.value(QStringLiteral("error")).toString().startsWith(QStringLiteral("Too long for 10 MB")));
    // A portrait sequence keeps its orientation.
    Rig portrait(Rate{60, 1}, 1080, 1920, 20.0);
    const ex::ExportSettings vertical = portrait.settings("discord-25mb");
    CHECK(*vertical.video.height > *vertical.video.width);
}

TEST_CASE("Gaming export estimate follows the range and the encoder", "[app][export]") {
    Rig r(Rate{60, 1}, 1920, 1080, 120.0, std::pair{10.0, 40.0});
    const QVariantMap whole = r.gaming.estimateFor(QStringLiteral("youtube-1080p60"), QString(), QStringLiteral("whole"));
    const QVariantMap marked = r.gaming.estimateFor(QStringLiteral("youtube-1080p60"), QString(), QStringLiteral("inOut"));
    REQUIRE(whole.value(QStringLiteral("ok")).toBool());
    REQUIRE(marked.value(QStringLiteral("ok")).toBool());
    CHECK(whole.value(QStringLiteral("seconds")).toDouble() == Catch::Approx(120.0).margin(0.05));
    CHECK(marked.value(QStringLiteral("seconds")).toDouble() == Catch::Approx(30.0).margin(0.05));
    // 18 Mb/s + 384 kb/s for 30 s, + 1 %.
    CHECK(marked.value(QStringLiteral("bytes")).toDouble() == Catch::Approx((18e6 + 384e3) * 30 / 8 * 1.01).epsilon(0.01));
    CHECK(whole.value(QStringLiteral("bytes")).toDouble() > marked.value(QStringLiteral("bytes")).toDouble() * 3.9);
    CHECK(whole.value(QStringLiteral("encodeSeconds")).toDouble() > marked.value(QStringLiteral("encodeSeconds")).toDouble());
    // Not probed: the software encoder is timed (the slower case), and the text says auto.
    CHECK(whole.value(QStringLiteral("encoderText")).toString() == QStringLiteral("Auto · hardware when available"));
    CHECK_FALSE(whole.value(QStringLiteral("hardware")).toBool());
    // A Discord target fills its room over the shorter range with a higher bitrate.
    const QVariantMap discordWhole = r.gaming.estimateFor(QStringLiteral("discord-25mb"), QString(), QStringLiteral("whole"));
    const QVariantMap discordMarked = r.gaming.estimateFor(QStringLiteral("discord-25mb"), QString(), QStringLiteral("inOut"));
    CHECK(discordMarked.value(QStringLiteral("videoMbps")).toDouble() > discordWhole.value(QStringLiteral("videoMbps")).toDouble());

    // The speed model: hardware beats software, 4K is slower than 1080p, more frames take longer.
    const double nvenc1080 = GamingExport::encodeSeconds("h264", "h264_nvenc", 1920, 1080, 3600);
    const double x264_1080 = GamingExport::encodeSeconds("h264", "libx264", 1920, 1080, 3600);
    const double nvenc2160 = GamingExport::encodeSeconds("h264", "h264_nvenc", 3840, 2160, 3600);
    const double amf1080 = GamingExport::encodeSeconds("h264", "h264_amf", 1920, 1080, 3600);
    CHECK(nvenc1080 < x264_1080);
    CHECK(nvenc2160 > nvenc1080 * 2.5);
    CHECK(amf1080 > nvenc1080);
    CHECK(GamingExport::encodeSeconds("h264", "h264_nvenc", 1920, 1080, 7200) > nvenc1080);
    // About a minute of 1080p60 on NVENC: 3600 frames at ~83 fps x 1.3 + 2 s.
    CHECK(nvenc1080 == Catch::Approx(2.0 + 3600.0 / 83.2 * 1.3).epsilon(0.01));
    CHECK(GamingExport::sizeText(850e3) == QStringLiteral("850 KB"));
    CHECK(GamingExport::sizeText(9.6e6) == QStringLiteral("9.6 MB"));
    CHECK(GamingExport::sizeText(25e6) == QStringLiteral("25 MB"));
    CHECK(GamingExport::sizeText(1.24e9) == QStringLiteral("1.24 GB"));
    CHECK(GamingExport::encodeText(45) == QStringLiteral("about 45 s"));
    CHECK(GamingExport::encodeText(200) == QStringLiteral("about 3 min 20 s"));
}

TEST_CASE("Gaming export presets: GIF and WebM use the exporter's own presets (I.9); vertical crop note",
          "[app][export]") {
    Rig r(Rate{60, 1}, 1920, 1080, 30.0);
    QStringList ids;
    for (const QVariant& v : r.gaming.presets()) {
        const QVariantMap p = v.toMap();
        ids << p.value(QStringLiteral("id")).toString();
        INFO(p.value(QStringLiteral("id")).toString().toStdString());
        CHECK(p.value(QStringLiteral("available")).toBool());
        CHECK(p.value(QStringLiteral("reason")).toString().isEmpty());
    }
    CHECK(ids.contains(QStringLiteral("gif")));
    CHECK(ids.contains(QStringLiteral("webm")));
    CHECK(r.gaming.groups().size() == 5);
    const QVariantMap gif = r.gaming.plan(QStringLiteral("gif"), QString(), QStringLiteral("inOut"));
    CHECK(gif.value(QStringLiteral("ok")).toBool());
    CHECK(gif.value(QStringLiteral("presetId")).toString() == QStringLiteral("gif-480p15"));
    CHECK(gif.value(QStringLiteral("overrides")).toMap().value(QStringLiteral("range")).toString() == QStringLiteral("inOut"));
    const QVariantMap webm = r.gaming.plan(QStringLiteral("webm"), QString(), QStringLiteral("whole"));
    CHECK(webm.value(QStringLiteral("ok")).toBool());
    CHECK(webm.value(QStringLiteral("presetId")).toString() == QStringLiteral("webm-vp9-720p"));
    // A 16:9 sequence to 9:16: the crop is said, the sequence is not changed.
    const QVariantMap vertical = r.gaming.plan(QStringLiteral("vertical-1080x1920-60"), QString(), QStringLiteral("whole"));
    REQUIRE(vertical.value(QStringLiteral("ok")).toBool());
    CHECK(vertical.value(QStringLiteral("notes")).toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("centre-cropped")));
    CHECK(r.controller.currentSequence()->width == 1920);
}

TEST_CASE("Export panel: the gaming presets drive the summary and the estimate; screenshot",
          "[app][export][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    REQUIRE(controller);
    Timeline tl("V1 |....|");
    Sequence s = tl.sequence();
    s.frameRate = Rate{60, 1};
    s.duration = Ticks::fromSeconds(45);
    controller->resetProject(tl.state.withSequence(std::make_shared<const Sequence>(std::move(s))), tl.seq);
    shell.window->setProperty("workspace", 2);
    spin(200);

    QQuickItem* content = shell.window->contentItem();
    QQuickItem* panel = findWith(content, [](QQuickItem* i) { return i->property("gamingPreset").isValid(); });
    REQUIRE(panel);
    QQuickItem* youtube = named(content, "gamingPreset_youtube-1080p60");
    QQuickItem* discord = named(content, "gamingPreset_discord-10mb");
    QQuickItem* gif = named(content, "gamingPreset_gif");
    REQUIRE(youtube);
    REQUIRE(discord);
    REQUIRE(gif);
    CHECK(youtube->isEnabled());
    CHECK(gif->isEnabled()); // the exporter's GIF preset (I.9)
    // The exporter's own gaming presets are not listed twice: "All presets" leaves out what the cards offer.
    CHECK(named(content, "exportPreset_youtube-1080p60") == nullptr);
    CHECK(named(content, "exportPreset_discord-10mb") == nullptr);
    CHECK(named(content, "exportPreset_gif-480p15") == nullptr);
    // Every available card carries its estimate.
    REQUIRE(waitFor([&] { return youtube->property("estimate").toString().startsWith(QStringLiteral("≈ ")); }, 3000));
    CHECK(discord->property("estimate").toString().contains(QStringLiteral("MB")));

    REQUIRE(QMetaObject::invokeMethod(youtube, "clicked"));
    CHECK(panel->property("gamingPreset").toString() == QStringLiteral("youtube-1080p60"));
    CHECK(youtube->property("selected").toBool());
    CHECK_FALSE(named(content, "exportPreset_youtube-1080p")->property("selected").toBool());
    const QVariantMap summary = panel->property("summary").toMap();
    CHECK(summary.value(QStringLiteral("ok")).toBool());
    CHECK(summary.value(QStringLiteral("resolution")).toString().startsWith(QStringLiteral("1920 × 1080")));
    CHECK(summary.value(QStringLiteral("quality")).toString() == QStringLiteral("18.0 Mb/s"));
    QQuickItem* estimate = named(content, "exportEstimate");
    REQUIRE(estimate);
    CHECK(estimate->isVisible());
    CHECK(named(content, "exportEstimateLine")->property("text").toString().startsWith(QStringLiteral("About ")));

    // Discord: the size target's notes, and the estimate under 10 MB.
    REQUIRE(QMetaObject::invokeMethod(discord, "clicked"));
    const QVariantMap discordEstimate = panel->property("estimate").toMap();
    REQUIRE(discordEstimate.value(QStringLiteral("ok")).toBool());
    CHECK(discordEstimate.value(QStringLiteral("bytes")).toDouble() < 10e6);
    CHECK(panel->property("estimateNotes").toStringList().join(QLatin1Char(' ')).contains(QStringLiteral("two-pass")));

    // The keyboard: the group is one Tab stop (the chosen preset); the arrows and Home move, Space chooses.
    QQuickItem* group = named(content, "gamingPresets");
    REQUIRE(group);
    {
        const QStringList issues = auditAccessibility(group); // radio buttons with names, the unavailable ones too
        INFO(issues.join(QStringLiteral("; ")).toStdString());
        CHECK(issues.isEmpty());
    }
    const QStringList order = group->property("order").toStringList();
    REQUIRE(order.size() >= 3);
    CHECK(order.contains(QStringLiteral("gif"))); // every available preset is in the order
    CHECK(group->property("tabStop").toString() == QStringLiteral("discord-10mb"));
    CHECK(discord->property("focusPolicy").toInt() == Qt::StrongFocus);
    CHECK(youtube->property("focusPolicy").toInt() == Qt::ClickFocus); // out of the Tab chain
    discord->forceActiveFocus(Qt::TabFocusReason);
    QQuickWindow* w = shell.window.get();
    tap(w, Qt::Key_Right);
    const qsizetype at = order.indexOf(QStringLiteral("discord-10mb"));
    REQUIRE(w->activeFocusItem());
    CHECK(w->activeFocusItem()->objectName() == QStringLiteral("gamingPreset_") + order.value(std::min(at + 1, order.size() - 1)));
    tap(w, Qt::Key_Home);
    CHECK(w->activeFocusItem()->objectName() == QStringLiteral("gamingPreset_") + order.first());
    tap(w, Qt::Key_End);
    CHECK(w->activeFocusItem()->objectName() == QStringLiteral("gamingPreset_") + order.last());
    tap(w, Qt::Key_Left);
    CHECK(w->activeFocusItem()->objectName() == QStringLiteral("gamingPreset_") + order.value(order.size() - 2));
    tap(w, Qt::Key_Space, QStringLiteral(" "));
    CHECK(panel->property("gamingPreset").toString() == order.value(order.size() - 2));
    CHECK(group->property("tabStop").toString() == order.value(order.size() - 2));

    // A built-in preset clears the gaming choice.
    REQUIRE(QMetaObject::invokeMethod(named(content, "exportPreset_match-source"), "clicked"));
    CHECK(panel->property("gamingPreset").toString().isEmpty());
    REQUIRE(QMetaObject::invokeMethod(youtube, "clicked"));

    spin(300);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("gaming-export-presets.png"))));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

// Design language v3: the Export panel reflows with its slot - the tiles over the details in a narrow one, the details
// in a column of their own beside the tiles in a wide one - and the Export button stays pinned in view either way.
TEST_CASE("Export panel reflows: stacked in a narrow slot, two columns in a wide one; screenshots",
          "[app][export][qml][screenshot]") {
    struct Size {
        int width;
        int height;
        const char* file;
    };
    for (const Size size : {Size{1100, 700, "export-panel-narrow.png"}, Size{2560, 1200, "export-panel-wide.png"}}) {
        DYNAMIC_SECTION(size.file) {
            resetSettings();
            clearMessages();
            Shell shell;
            REQUIRE(shell.window);
            INFO(join(shell.errors));
            shell.resize(size.width, size.height);
            shell.theme->setProperty("reduceMotion", true);
            auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
            REQUIRE(controller);
            Timeline tl("V1 |....|");
            Sequence s = tl.sequence();
            s.frameRate = Rate{60, 1};
            s.duration = Ticks::fromSeconds(45);
            controller->resetProject(tl.state.withSequence(std::make_shared<const Sequence>(std::move(s))), tl.seq);
            shell.window->setProperty("workspace", 2);
            spin(200);

            QQuickItem* content = shell.window->contentItem();
            QQuickItem* panel = findWith(content, [](QQuickItem* i) { return i->property("gamingPreset").isValid(); });
            REQUIRE(panel);
            QQuickItem* youtube = named(content, "gamingPreset_youtube-1080p60");
            REQUIRE(youtube);
            REQUIRE(waitFor([&] { return youtube->property("estimate").toString().startsWith(QStringLiteral("≈ ")); }, 3000));
            REQUIRE(QMetaObject::invokeMethod(youtube, "clicked"));
            QObject* drawer = shell.object("exportSettingsDrawer");
            if (!drawer)
                drawer = named(content, "exportSettingsDrawer");
            REQUIRE(drawer);
            drawer->setProperty("open", true);
            spin(300);

            // A hidden window lays out when it renders: the geometry is read after the grab.
            const QImage image = shell.grab();
            REQUIRE_FALSE(image.isNull());
            const QRectF panelRect(0, 0, panel->width(), panel->height());
            QQuickItem* details = named(content, "exportDetails");
            QQuickItem* button = named(content, "exportButton");
            QQuickItem* footer = named(content, "exportFooter");
            REQUIRE(details);
            REQUIRE(button);
            REQUIRE(footer);
            const QRectF detailsRect = details->mapRectToItem(panel, QRectF(0, 0, details->width(), details->height()));
            const QRectF buttonRect = button->mapRectToItem(panel, QRectF(0, 0, button->width(), button->height()));
            const QRectF tileRect = youtube->mapRectToItem(panel, QRectF(0, 0, youtube->width(), youtube->height()));
            QQuickItem* page = named(content, "exportPage");
            REQUIRE(page);
            const bool wide = page->property("wide").toBool();
            UNSCOPED_INFO("panel " << panel->width() << " x " << panel->height() << ", details at " << detailsRect.x()
                                   << ", button " << buttonRect.x() << "," << buttonRect.y() << " "
                                   << buttonRect.width() << " x " << buttonRect.height());
            // The Export button: whole, inside the panel, at its foot.
            CHECK(panelRect.contains(buttonRect));
            CHECK(buttonRect.height() >= 36);
            CHECK(buttonRect.bottom() > panel->height() * 0.7);
            // Tiles are never squeezed below their size.
            CHECK(youtube->width() >= 170);
            if (std::string_view(size.file) == "export-panel-wide.png") {
                REQUIRE(wide); // the wide window gives the panel room for two columns
                CHECK(detailsRect.x() > tileRect.right());           // the details beside the tiles
                CHECK(buttonRect.x() > tileRect.right());            // and the button under them
            } else {
                CHECK_FALSE(wide);
                CHECK(detailsRect.x() < panel->width() * 0.25);      // the details under the tiles
                CHECK(buttonRect.width() > panel->width() * 0.25);
            }
            const double dpr = image.devicePixelRatio();
            const QRectF area = panel->mapRectToScene(panelRect);
            REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                        .save(QDir(outputDir()).filePath(QString::fromLatin1(size.file))));
            const QStringList messages = takeMessages();
            INFO(join(messages));
            CHECK(messages.isEmpty());
        }
    }
}

// M5n: the audio channels UI (A.9) in the app, offscreen; no window is shown and no audio device is opened.
//   * AudioChannelsBridge: what clips and media items play (audio.channels), Modify > Audio Channels maps and presets,
//     Fill / Swap / Split to Mono, Channel Volume (drags merge, keys at the playhead), track formats, the master's
//     format and the 5.1 panner; one undo step each;
//   * the timeline's channel badges (scene graph micro-tags);
//   * in the shell: the Audio Channels dialog (the grid, the presets, OK), the clip menu's submenu, the track header's
//     format badge and menu, the mixer's 5.1 panner and six-channel master meter, Channel Volume in Effect Controls,
//     the export's audio channels and AC-3; screenshots.

#include "ShellFixture.h"
#include "TimelineFixture.h"
#include "TimelineHarness.h"

#include "AudioChannelsBridge.h"
#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "ExportController.h"
#include "MeterBar.h"
#include "ProjectController.h"
#include "TimelineView.h"

#include <wabcut/model/AudioChannelOps.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QPointer>
#include <QQmlComponent>
#include <QQuickItem>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

// A: an interview file, two microphones on L and R. B: a picture without sound. C: a 5.1 file.
constexpr const char* kPicture = R"(
    V2     |..........Bbbbbbbbbb............................|
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2     |................................................|
    A3     |......................Cccccccccccccccc..........|
)";

/// The picture with B silent and C a 5.1 file.
Timeline makeTimeline() {
    Timeline tl(kPicture);
    tl.link("V1@0 A1@0");
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media('B')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->info.audio.clear();
            m = copy;
        } else if (m->id == tl.media('C')) {
            auto copy = std::make_shared<MediaItem>(*m);
            copy->info.audio[0].channels = 6;
            copy->info.audio[0].channelLayout = "5.1";
            m = copy;
        }
    }
    return tl;
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

QStringList routes(const QVariantMap& info) {
    QStringList out;
    for (const QVariant& c : info.value(QStringLiteral("channels")).toList()) {
        QStringList sources;
        for (const QVariant& s : c.toMap().value(QStringLiteral("sources")).toList())
            sources << QString::number(s.toInt());
        out << sources.join(QLatin1Char('+')) + (c.toMap().value(QStringLiteral("muted")).toBool() ? QStringLiteral("m") : QString());
    }
    return out;
}

/// The bridge on a plain controller (no window).
struct ChannelsRig {
    Timeline tl = makeTimeline();
    ProjectController controller;
    AudioChannelsBridge channels;

    ChannelsRig() {
        controller.resetProject(tl.state, tl.seq);
        channels.setController(&controller);
    }
    QString key(std::string_view ref) const { return clipKey(clipAt(*controller.snapshot(), tl.seq, ref)); }
    std::size_t undo() { return controller.session().undoCount(); }
    const Clip& clip(std::string_view ref) const {
        return controller.currentSequence()->findClip(clipAt(*controller.snapshot(), tl.seq, ref))->clip();
    }
    QString badge(std::string_view ref) const { return channelBadge(*controller.snapshot(), clip(ref)); }
    const Track& track(std::string_view label) const { return *controller.currentSequence()->findTrack(tl.track(label)); }
};

} // namespace

TEST_CASE("Audio channels: what clips play and Modify Audio Channels presets and maps, one undo step each",
          "[app][channels]") {
    ChannelsRig r;
    const QStringList a1{r.key("A1@0")};

    // The interview file plays as it is: two source channels, L and R.
    QVariantMap info = r.channels.info(a1);
    REQUIRE(info.value(QStringLiteral("ok")).toBool());
    CHECK(info.value(QStringLiteral("sourceChannels")).toInt() == 2);
    CHECK(info.value(QStringLiteral("mapFrom")).toString() == QStringLiteral("none"));
    CHECK(info.value(QStringLiteral("format")).toString() == QStringLiteral("stereo"));
    CHECK(routes(info) == QStringList{QStringLiteral("1"), QStringLiteral("2")});
    CHECK(info.value(QStringLiteral("sourceLabels")).toStringList() == QStringList{QStringLiteral("L"), QStringLiteral("R")});
    CHECK(r.badge("A1@0").isEmpty());
    // A video clip stands for its linked audio.
    CHECK(r.channels.info({r.key("V1@0")}).value(QStringLiteral("ok")).toBool());
    // The 5.1 file has six source channels, labelled as 5.1.
    info = r.channels.info({r.key("A3@22")});
    CHECK(info.value(QStringLiteral("sourceChannels")).toInt() == 6);
    CHECK(info.value(QStringLiteral("format")).toString() == QStringLiteral("5.1"));
    CHECK(info.value(QStringLiteral("sourceLabels")).toStringList().size() == 6);

    // Fill Left with Right: both sides play the right channel.
    std::size_t before = r.undo();
    REQUIRE(r.channels.fill(a1, QStringLiteral("right")).isEmpty());
    CHECK(r.undo() == before + 1);
    info = r.channels.info(a1);
    CHECK(info.value(QStringLiteral("mapFrom")).toString() == QStringLiteral("clip"));
    CHECK(routes(info) == QStringList{QStringLiteral("2"), QStringLiteral("2")});
    CHECK(r.badge("A1@0") == QStringLiteral("FILL L"));
    // Fill Right with Left, Swap Channels.
    REQUIRE(r.channels.fill(a1, QStringLiteral("left")).isEmpty());
    CHECK(routes(r.channels.info(a1)) == QStringList{QStringLiteral("1"), QStringLiteral("1")});
    CHECK(r.badge("A1@0") == QStringLiteral("FILL R"));
    REQUIRE(r.channels.swap(a1).isEmpty());
    CHECK(routes(r.channels.info(a1)) == QStringList{QStringLiteral("2"), QStringLiteral("1")});
    CHECK(r.badge("A1@0") == QStringLiteral("SWAP"));
    CHECK(r.undo() == before + 3);

    // A map from the dialog: mono, both microphones summed. The clip's channel volumes are kept.
    REQUIRE(r.channels.applyPreset(a1, QStringLiteral("stereo")).isEmpty());
    r.controller.selectClip(r.key("A1@0"));
    REQUIRE(r.channels.volume().value(QStringLiteral("available")).toBool());
    REQUIRE(r.channels.setChannelVolume(1, -9.0).isEmpty());
    r.channels.endChannelVolume();
    before = r.undo();
    const QVariantMap mono{{QStringLiteral("format"), QStringLiteral("mono")},
                           {QStringLiteral("channels"), QVariantList{QVariantMap{{QStringLiteral("sources"), QVariantList{1, 2}}}}}};
    REQUIRE(r.channels.applyMap(a1, mono).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Modify Audio Channels"));
    info = r.channels.info(a1);
    CHECK(info.value(QStringLiteral("format")).toString() == QStringLiteral("mono"));
    CHECK(routes(info) == QStringList{QStringLiteral("1+2")});
    CHECK(r.badge("A1@0") == QStringLiteral("SUM"));
    REQUIRE(r.clip("A1@0").channelMap());
    CHECK(r.clip("A1@0").channelMap()->channels[0].gainDb == ParamValue(-9.0));
    // A muted channel.
    const QVariantMap muted{{QStringLiteral("format"), QStringLiteral("stereo")},
                            {QStringLiteral("channels"), QVariantList{QVariantMap{{QStringLiteral("sources"), QVariantList{1}}},
                                                                      QVariantMap{{QStringLiteral("sources"), QVariantList{2}},
                                                                                  {QStringLiteral("muted"), true}}}}};
    REQUIRE(r.channels.applyMap(a1, muted).isEmpty());
    CHECK(routes(r.channels.info(a1)) == QStringList{QStringLiteral("1"), QStringLiteral("2m")});
    CHECK(r.badge("A1@0") == QStringLiteral("MUTED"));

    // A map the clip format cannot hold is refused, nothing changes.
    before = r.undo();
    const QVariantMap wrong{{QStringLiteral("format"), QStringLiteral("5.1")},
                            {QStringLiteral("channels"), QVariantList{QVariantMap{{QStringLiteral("sources"), QVariantList{1}}}}}};
    CHECK_FALSE(r.channels.applyMap(a1, wrong).isEmpty());
    CHECK_FALSE(r.channels.message().isEmpty());
    CHECK(r.undo() == before);
    CHECK_FALSE(r.channels.applyMap(a1, QVariantMap{{QStringLiteral("format"), QStringLiteral("quad")}}).isEmpty());
    CHECK(r.undo() == before);

    // Default: no map, the stream as it is.
    REQUIRE(r.channels.applyPreset(a1, QStringLiteral("default")).isEmpty());
    CHECK(r.channels.info(a1).value(QStringLiteral("mapFrom")).toString() == QStringLiteral("none"));
    CHECK(r.badge("A1@0").isEmpty());
}

TEST_CASE("Audio channels: media item maps and Split to Mono", "[app][channels]") {
    ChannelsRig r;
    const QStringList media{mediaKey(r.tl.media('A'))};

    // The Library item's map: its clips without a map of their own follow it.
    QVariantMap info = r.channels.mediaInfo(media);
    REQUIRE(info.value(QStringLiteral("ok")).toBool());
    CHECK(info.value(QStringLiteral("mapFrom")).toString() == QStringLiteral("none"));
    std::size_t before = r.undo();
    REQUIRE(r.channels.applyMediaPreset(media, QStringLiteral("swap")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.channels.mediaInfo(media).value(QStringLiteral("mapFrom")).toString() == QStringLiteral("media"));
    CHECK(r.channels.info({r.key("A1@0")}).value(QStringLiteral("mapFrom")).toString() == QStringLiteral("media"));
    CHECK(r.badge("A1@0") == QStringLiteral("SWAP"));
    const QVariantMap fill{{QStringLiteral("format"), QStringLiteral("stereo")},
                           {QStringLiteral("channels"), QVariantList{QVariantMap{{QStringLiteral("sources"), QVariantList{1}}},
                                                                     QVariantMap{{QStringLiteral("sources"), QVariantList{1}}}}}};
    REQUIRE(r.channels.applyMediaMap(media, fill).isEmpty());
    CHECK(r.badge("A1@0") == QStringLiteral("FILL R"));
    // A picture without sound has no channels.
    CHECK_FALSE(r.channels.mediaInfo({mediaKey(r.tl.media('B'))}).value(QStringLiteral("ok")).toBool());
    REQUIRE(r.channels.applyMediaPreset(media, QStringLiteral("default")).isEmpty());

    // Split to Mono: the clip keeps channel 1, a linked copy on the free A2 plays channel 2.
    before = r.undo();
    REQUIRE(r.channels.splitToMono(r.key("A1@0")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.track("A2").clips.size() == 1);
    const Clip& left = r.clip("A1@0");
    const Clip& right = r.track("A2").clips.front();
    REQUIRE(left.channelMap());
    REQUIRE(right.channelMap());
    CHECK(left.channelMap()->format == ChannelFormat::Mono);
    CHECK(left.channelMap()->channels[0].sources == std::vector<std::int32_t>{0});
    CHECK(right.channelMap()->channels[0].sources == std::vector<std::int32_t>{1});
    CHECK(r.badge("A1@0") == QStringLiteral("MONO"));
    CHECK(channelBadge(*r.controller.snapshot(), right) == QStringLiteral("CH 2"));
    // A picture without sound has nothing to split.
    CHECK_FALSE(r.channels.splitToMono(r.key("V2@10")).isEmpty());
}

TEST_CASE("Audio channels: Channel Volume follows the clip and the playhead; drags merge, keys toggle",
          "[app][channels]") {
    ChannelsRig r;
    CHECK_FALSE(r.channels.volume().value(QStringLiteral("available")).toBool());
    r.controller.selectClip(r.key("V1@0")); // the linked audio is the Audio section's clip
    QVariantMap volume = r.channels.volume();
    REQUIRE(volume.value(QStringLiteral("available")).toBool());
    CHECK(volume.value(QStringLiteral("clip")).toString() == r.key("A1@0"));
    QVariantList rows = volume.value(QStringLiteral("channels")).toList();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("L"));
    CHECK(rows[1].toMap().value(QStringLiteral("label")).toString() == QStringLiteral("R"));
    CHECK(rows[0].toMap().value(QStringLiteral("db")).toDouble() == 0.0);

    // A drag: one undo step.
    std::size_t before = r.undo();
    for (const double db : {-2.0, -4.0, -6.5})
        REQUIRE(r.channels.setChannelVolume(2, db).isEmpty());
    r.channels.endChannelVolume();
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Channel Volume"));
    rows = r.channels.volume().value(QStringLiteral("channels")).toList();
    CHECK(rows[1].toMap().value(QStringLiteral("db")).toDouble() == Catch::Approx(-6.5));
    REQUIRE(r.clip("A1@0").channelMap());
    CHECK(r.clip("A1@0").channelMap()->channels[1].gainDb == ParamValue(-6.5));

    // Animate L: a key at the playhead; another key later; the slider moves the key at the playhead.
    r.controller.setPlayhead(f(10));
    REQUIRE(r.channels.setChannelAnimated(1, true).isEmpty());
    rows = r.channels.volume().value(QStringLiteral("channels")).toList();
    CHECK(rows[0].toMap().value(QStringLiteral("animated")).toBool());
    CHECK(rows[0].toMap().value(QStringLiteral("keyed")).toBool());
    r.controller.setPlayhead(f(30));
    rows = r.channels.volume().value(QStringLiteral("channels")).toList();
    CHECK_FALSE(rows[0].toMap().value(QStringLiteral("keyed")).toBool());
    REQUIRE(r.channels.toggleChannelKey(1).isEmpty());
    REQUIRE(r.channels.setChannelVolume(1, -12.0).isEmpty());
    r.channels.endChannelVolume();
    const Curve* curve = r.clip("A1@0").channelMap()->channels[0].gainDb.curve();
    REQUIRE(curve);
    REQUIRE(curve->keys.size() == 2);
    CHECK(curve->keys[1].value == -12.0);
    r.controller.setPlayhead(f(20));
    rows = r.channels.volume().value(QStringLiteral("channels")).toList();
    CHECK(rows[0].toMap().value(QStringLiteral("db")).toDouble() == Catch::Approx(-6.0).margin(0.01));
    // The key at the playhead goes again; the stopwatch off keeps the value there.
    r.controller.setPlayhead(f(30));
    REQUIRE(r.channels.toggleChannelKey(1).isEmpty());
    CHECK(r.clip("A1@0").channelMap()->channels[0].gainDb.curve()->keys.size() == 1);
    REQUIRE(r.channels.setChannelAnimated(1, false).isEmpty());
    CHECK(r.clip("A1@0").channelMap()->channels[0].gainDb.isConstant());
    CHECK_FALSE(r.channels.setChannelVolume(3, 0).isEmpty()); // a stereo clip has two channels

    // A clip without sound: nothing to show.
    r.controller.selectClip(r.key("V2@10"));
    CHECK_FALSE(r.channels.volume().value(QStringLiteral("available")).toBool());
}

TEST_CASE("Audio channels: track formats, the master format and the 5.1 panner", "[app][channels]") {
    ChannelsRig r;
    const QString a1 = trackKey(r.tl.track("A1"));
    CHECK(r.channels.trackFormats().value(a1).toString() == QStringLiteral("stereo"));
    CHECK(r.channels.masterFormat() == QStringLiteral("stereo"));
    CHECK_FALSE(r.channels.trackFormats().contains(trackKey(r.tl.track("V1"))));

    std::size_t before = r.undo();
    REQUIRE(r.channels.setTrackFormat(a1, QStringLiteral("mono")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.track("A1").format == ChannelFormat::Mono);
    CHECK(r.channels.trackFormats().value(a1).toString() == QStringLiteral("mono"));
    REQUIRE(r.channels.setTrackFormat(a1, QStringLiteral("adaptive")).isEmpty());
    CHECK(r.channels.trackFormats().value(a1).toString() == QStringLiteral("adaptive"));
    CHECK_FALSE(r.channels.setTrackFormat(a1, QStringLiteral("quad")).isEmpty());
    CHECK_FALSE(r.channels.setTrackFormat(trackKey(r.tl.track("V1")), QStringLiteral("mono")).isEmpty());

    REQUIRE(r.channels.setMasterFormat(QStringLiteral("5.1")).isEmpty());
    CHECK(r.channels.masterFormat() == QStringLiteral("5.1"));
    CHECK(r.controller.currentSequence()->masterFormat == ChannelFormat::Surround51);
    CHECK_FALSE(r.channels.setMasterFormat(QStringLiteral("mono")).isEmpty());

    // The puck: a drag is one undo step; values are clamped.
    before = r.undo();
    REQUIRE(r.channels.setSurroundPan(a1, -0.2, 0.8, -144).isEmpty());
    REQUIRE(r.channels.setSurroundPan(a1, -0.5, 0.4, -144).isEmpty());
    REQUIRE(r.channels.setSurroundPan(a1, -0.5, 0.4, -12).isEmpty());
    r.channels.endSurroundPan();
    CHECK(r.undo() == before + 1);
    const QVariantMap pan = r.channels.surround().value(a1).toMap();
    CHECK(pan.value(QStringLiteral("x")).toDouble() == Catch::Approx(-0.5));
    CHECK(pan.value(QStringLiteral("y")).toDouble() == Catch::Approx(0.4));
    CHECK(pan.value(QStringLiteral("lfeDb")).toDouble() == Catch::Approx(-12));
    REQUIRE(r.channels.setSurroundPan(a1, 4, -4, 20).isEmpty());
    r.channels.endSurroundPan();
    CHECK(r.track("A1").surround.x == ParamValue(1.0));
    CHECK(r.track("A1").surround.y == ParamValue(-1.0));
    CHECK(r.track("A1").surround.lfeDb == ParamValue(0.0));
    // Undo comes back to the bridge.
    REQUIRE(r.controller.undo());
    CHECK(r.channels.surround().value(a1).toMap().value(QStringLiteral("x")).toDouble() == Catch::Approx(-0.5));
}

TEST_CASE("Audio channels in the timeline: channel badges as micro-tags on the clips", "[app][channels][timeline]") {
    Rig rig(R"(
        A1 |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
        A2 |Bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A3 |Cccccccccccccccccccccccccccccccccccccccc|
    )",
            8.0, false);
    AudioChannelsBridge channels;
    channels.setController(&rig.controller);
    REQUIRE(channels.swap({clipKey(rig.clip("A1@0"))}).isEmpty());
    REQUIRE(channels.applyPreset({clipKey(rig.clip("A2@0"))}, QStringLiteral("mono"), 2).isEmpty());
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
    const TimelineStats& stats = rig.view->stats();
    CHECK(stats.badgeTexts.value(rig.clip("A1@0").value) == QStringLiteral("SWAP"));
    CHECK(stats.badgeTexts.value(rig.clip("A2@0").value) == QStringLiteral("CH 2"));
    CHECK_FALSE(stats.badgeTexts.contains(rig.clip("A3@0").value)); // plays the stream as it is
    // Default again: the badge goes.
    REQUIRE(channels.applyPreset({clipKey(rig.clip("A1@0"))}, QStringLiteral("default")).isEmpty());
    for (int i = 0; i < 4; ++i) {
        QCoreApplication::processEvents();
        rig.harness.frame();
    }
    CHECK_FALSE(rig.view->stats().badgeTexts.contains(rig.clip("A1@0").value));
}

TEST_CASE("Audio channels export: audio channels and AC-3 in the export settings", "[app][channels]") {
    ChannelsRig r;
    ExportController exporter;
    exporter.setProject(&r.controller);
    QVariantMap d = exporter.describe({}, QStringLiteral("match-source"));
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("audio")).toString().endsWith(QStringLiteral("stereo")));
    CHECK(d.value(QStringLiteral("audioChannels")).toInt() == 2);
    CHECK(d.value(QStringLiteral("audioCodec")).toString() == QStringLiteral("aac"));
    // A 5.1 master exported "as the master" with AC-3 gets six channels.
    REQUIRE(r.channels.setMasterFormat(QStringLiteral("5.1")).isEmpty());
    d = exporter.describe({}, QStringLiteral("match-source"),
                          {{QStringLiteral("audioChannels"), 0}, {QStringLiteral("audioCodec"), QStringLiteral("ac3")}});
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("audioCodec")).toString() == QStringLiteral("ac3"));
    CHECK(d.value(QStringLiteral("audio")).toString().startsWith(QStringLiteral("AC-3")));
    CHECK(d.value(QStringLiteral("audio")).toString().endsWith(QStringLiteral("5.1")));
    d = exporter.describe({}, QStringLiteral("match-source"), {{QStringLiteral("audioChannels"), 1}});
    CHECK(d.value(QStringLiteral("audio")).toString().endsWith(QStringLiteral("mono")));
    // Three channels is not an export layout.
    d = exporter.describe({}, QStringLiteral("match-source"), {{QStringLiteral("audioChannels"), 3}});
    CHECK_FALSE(d.value(QStringLiteral("ok")).toBool());
    // A WAV keeps PCM even when AC-3 was chosen before.
    d = exporter.describe({}, QStringLiteral("audio-wav"), {{QStringLiteral("audioCodec"), QStringLiteral("ac3")}});
    REQUIRE(d.value(QStringLiteral("ok")).toBool());
    CHECK(d.value(QStringLiteral("audio")).toString().startsWith(QStringLiteral("PCM")));
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

namespace {

struct ShellRig {
    Shell shell;
    ProjectController* controller = nullptr;
    AudioChannelsBridge* channels = nullptr;
    Timeline tl = makeTimeline();

    ShellRig() {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        channels = shell.engine.singletonInstance<AudioChannelsBridge*>("Wabcut", "AudioChannels");
        REQUIRE(controller);
        REQUIRE(channels);
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    QString key(std::string_view ref) const { return clipKey(clipAt(*controller->snapshot(), tl.seq, ref)); }
    std::size_t undo() const { return controller->session().undoCount(); }
    const Clip& clip(std::string_view ref) const {
        return controller->currentSequence()->findClip(clipAt(*controller->snapshot(), tl.seq, ref))->clip();
    }
    QQuickItem* item(const QString& name) const { return findNamed(shell.window->contentItem(), name); }
    /// Saves the scene rectangle of `area` (plus a margin) as a screenshot.
    void shot(QQuickItem* area, const QString& file, double margin = 0) const {
        const QImage image = shell.grab();
        REQUIRE_FALSE(image.isNull());
        const double dpr = image.devicePixelRatio();
        const QRectF r = area->mapRectToScene(QRectF(0, 0, area->width(), area->height()))
                             .adjusted(-margin, -margin, margin, margin)
                             .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toRect()).save(QDir(outputDir()).filePath(file)));
    }
};

QObject* openDialog(Shell& shell, const char* loaderName) {
    QObject* loader = shell.object(loaderName);
    INFO(loaderName);
    REQUIRE(loader);
    QObject* dialog = nullptr;
    REQUIRE(waitFor([&] {
        dialog = loader->property("item").value<QObject*>();
        return dialog && dialog->property("opened").toBool();
    }, 5000));
    return dialog;
}

QQuickItem* inDialog(QObject* dialog, const char* name) {
    auto* content = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    return findNamed(content->parentItem() ? content->parentItem() : content, QString::fromLatin1(name));
}

void click(QObject* button) {
    REQUIRE(button);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Audio Channels dialog: the grid from audio.channels, presets fill it, OK is one undo step; screenshot",
          "[app][channels][qml][screenshot]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    const QString a1 = r.key("A1@0");

    // The clip menu's Audio Channels… (Main opens the dialog).
    r.channels->openDialog({a1});
    QObject* dialog = openDialog(r.shell, "audioChannelsDialogLoader");
    CHECK(dialog->property("mode").toString() == QStringLiteral("clips"));
    CHECK(dialog->property("format").toString() == QStringLiteral("stereo"));
    CHECK(dialog->property("sourceCount").toInt() == 2);
    QQuickItem* lFromL = inDialog(dialog, "routeCell_1_1");
    QQuickItem* lFromR = inDialog(dialog, "routeCell_1_2");
    QQuickItem* rFromR = inDialog(dialog, "routeCell_2_2");
    QQuickItem* muteR = inDialog(dialog, "muteCell_2");
    REQUIRE(lFromL);
    REQUIRE(lFromR);
    REQUIRE(rFromR);
    REQUIRE(muteR);
    CHECK(lFromL->property("on").toBool());
    CHECK_FALSE(lFromR->property("on").toBool());
    CHECK(rFromR->property("on").toBool());
    CHECK_FALSE(muteR->property("on").toBool());

    // A preset fills the grid; nothing changes until OK.
    const std::size_t before = r.undo();
    click(inDialog(dialog, "presetSwap"));
    CHECK_FALSE(lFromL->property("on").toBool());
    CHECK(lFromR->property("on").toBool());
    CHECK(r.undo() == before);
    // A cell: L also gets L (both averaged); R muted.
    click(lFromL);
    click(muteR);
    CHECK(lFromL->property("on").toBool());
    CHECK(muteR->property("on").toBool());
    spin(150);
    QQuickItem* card = dialog->property("contentItem").value<QQuickItem*>();
    REQUIRE(card);
    r.shot(card->parentItem() ? card->parentItem() : card, QStringLiteral("audio-channels.png"), 16);

    click(inDialog(dialog, "audioChannelsOk"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    CHECK(r.undo() == before + 1);
    const AudioChannelMap* map = r.clip("A1@0").channelMap();
    REQUIRE(map);
    CHECK(map->channels[0].sources == std::vector<std::int32_t>{0, 1});
    CHECK(map->channels[1].sources == std::vector<std::int32_t>{0});
    CHECK(map->channels[1].muted);

    // The format: mono, one row; Default clears the map.
    r.channels->openDialog({a1});
    dialog = openDialog(r.shell, "audioChannelsDialogLoader");
    QObject* formats = inDialog(dialog, "audioChannelsFormat");
    REQUIRE(formats);
    REQUIRE(QMetaObject::invokeMethod(formats, "select", Q_ARG(QVariant, 0)));
    CHECK(dialog->property("format").toString() == QStringLiteral("mono"));
    CHECK(dialog->property("routes").toList().size() == 1);
    click(inDialog(dialog, "presetDefault"));
    CHECK(dialog->property("useDefault").toBool());
    click(inDialog(dialog, "audioChannelsOk"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    CHECK(r.clip("A1@0").channelMap() == nullptr);

    // A Library item (the Library's Audio Channels…).
    r.channels->openForMedia({mediaKey(r.tl.media('A'))});
    dialog = openDialog(r.shell, "audioChannelsDialogLoader");
    CHECK(dialog->property("mode").toString() == QStringLiteral("media"));
    click(inDialog(dialog, "presetFillLeft"));
    click(inDialog(dialog, "audioChannelsOk"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    const MediaItem* media = r.controller->snapshot()->findMedia(r.tl.media('A'));
    REQUIRE(media->channels);
    CHECK(*media->channels == AudioChannelMap::fill(1));
    checkNoMessages();
}

TEST_CASE("Clip menu: the Audio Channels submenu on clips with sound; fill, swap and split", "[app][channels][qml]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    QObject* clipMenu = r.shell.window->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(clipMenu);
    QQuickItem* lanes = r.item(QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    QObject* channelsMenu = clipMenu->property("channelsMenu").value<QObject*>();
    REQUIRE(channelsMenu);
    const auto openOn = [&](const QString& clip) {
        r.controller->selectClip(clip);
        REQUIRE(QMetaObject::invokeMethod(clipMenu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                          Q_ARG(QVariant, clip), Q_ARG(QVariant, QPointF(200, 20))));
    };
    const auto row = [&] {
        QVariant at;
        REQUIRE(QMetaObject::invokeMethod(clipMenu, "submenuIndex", Q_RETURN_ARG(QVariant, at),
                                          Q_ARG(QVariant, QVariant::fromValue(channelsMenu))));
        return at.toInt();
    };

    openOn(r.key("A1@0"));
    REQUIRE(row() >= 0);
    QQuickItem* item = nullptr;
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, row())));
    REQUIRE(item);
    CHECK(item->property("text").toString() == QStringLiteral("Audio Channels"));
    QQuickItem* previous = nullptr;
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, previous), Q_ARG(int, row() - 1)));
    REQUIRE(previous);
    CHECK(previous->objectName() == QStringLiteral("synchronizeMenuItem"));

    // Fill Left with Right, Swap Channels: one undo step each.
    std::size_t before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(channelsMenu->findChild<QObject*>(QStringLiteral("fillLeftItem")), "triggered"));
    CHECK(r.undo() == before + 1);
    REQUIRE(r.clip("A1@0").channelMap());
    CHECK(*r.clip("A1@0").channelMap() == AudioChannelMap::fill(1));
    REQUIRE(QMetaObject::invokeMethod(channelsMenu->findChild<QObject*>(QStringLiteral("swapChannelsItem")), "triggered"));
    CHECK(*r.clip("A1@0").channelMap() == AudioChannelMap::swap());
    CHECK(channelsMenu->property("error").toString().isEmpty());
    // Audio Channels… opens the dialog for the clip.
    REQUIRE(QMetaObject::invokeMethod(channelsMenu->findChild<QObject*>(QStringLiteral("audioChannelsOpenItem")), "triggered"));
    QObject* dialog = openDialog(r.shell, "audioChannelsDialogLoader");
    CHECK(dialog->property("keys").toStringList() == QStringList{r.key("A1@0")});
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    QMetaObject::invokeMethod(clipMenu, "close");

    // Split to Mono on the 5.1 clip: five linked mono copies on the tracks below (new ones as needed).
    openOn(r.key("A3@22"));
    before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(channelsMenu->findChild<QObject*>(QStringLiteral("splitToMonoItem")), "triggered"));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller->currentSequence()->audioTracks.size() == 8);
    QMetaObject::invokeMethod(clipMenu, "close");

    // A picture without sound: no submenu.
    openOn(r.key("V2@10"));
    CHECK(row() == -1);
    QMetaObject::invokeMethod(clipMenu, "close");
    // Back on a clip with sound: once again.
    openOn(r.key("A1@0"));
    CHECK(row() >= 0);
    QMetaObject::invokeMethod(clipMenu, "close");
    checkNoMessages();
}

TEST_CASE("Track header: the channel format badge and the format menu; screenshot", "[app][channels][qml][screenshot]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    QQuickItem* badge = r.item(QStringLiteral("trackFormat_A1"));
    QQuickItem* videoBadge = r.item(QStringLiteral("trackFormat_V1"));
    REQUIRE(badge);
    REQUIRE(videoBadge);
    CHECK_FALSE(badge->isVisible()); // stereo: no badge
    CHECK_FALSE(videoBadge->isVisible());

    // The header's format menu (right-click, or the Menu key on the name chip).
    QQuickItem* header = badge->parentItem();
    REQUIRE(header);
    REQUIRE(QMetaObject::invokeMethod(header, "openFormatMenu", Q_ARG(QVariant, QVariant::fromValue<QObject*>(header)),
                                      Q_ARG(QVariant, QPointF(40, 10))));
    QObject* loader = header->findChild<QObject*>(QStringLiteral("trackHeaderMenuLoader"));
    REQUIRE(loader);
    QObject* menu = loader->property("item").value<QObject*>();
    REQUIRE(menu);
    CHECK(menu->objectName() == QStringLiteral("trackHeaderMenu"));
    REQUIRE(waitFor([&] { return menu->property("opened").toBool(); }, 3000));
    QObject* stereo = menu->findChild<QObject*>(QStringLiteral("trackFormat_stereo"));
    QObject* mono = menu->findChild<QObject*>(QStringLiteral("trackFormat_mono"));
    REQUIRE(stereo);
    REQUIRE(mono);
    CHECK(stereo->property("checked").toBool());
    const std::size_t before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(mono, "triggered"));
    CHECK(r.undo() == before + 1);
    CHECK(r.controller->currentSequence()->findTrack(r.tl.track("A1"))->format == ChannelFormat::Mono);
    REQUIRE(waitFor([&] { return badge->isVisible(); }, 2000));
    QQuickItem* glyph = badge->childItems().isEmpty() ? nullptr : badge->childItems().front();
    REQUIRE(glyph);
    CHECK(glyph->property("text").toString() == QStringLiteral("1.0"));
    CHECK(mono->property("checked").toBool());
    CHECK_FALSE(stereo->property("checked").toBool());
    QMetaObject::invokeMethod(menu, "close");
    REQUIRE(r.channels->setTrackFormat(trackKey(r.tl.track("A3")), QStringLiteral("5.1")).isEmpty());
    QQuickItem* a3 = r.item(QStringLiteral("trackFormat_A3"));
    REQUIRE(a3);
    REQUIRE(waitFor([&] { return a3->isVisible(); }, 2000));
    // The timeline's channel badge beside: a swapped clip.
    REQUIRE(r.channels->swap({r.key("A1@0")}).isEmpty());
    spin(300);
    QQuickItem* timeline = r.shell.item("timelinePanel");
    REQUIRE(timeline);
    r.shot(timeline, QStringLiteral("track-format.png"));
    checkNoMessages();
}

TEST_CASE("Mixer: a 5.1 master turns the pan knob into the 5.1 panner; the master meters six channels; screenshot",
          "[app][channels][qml][screenshot]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    auto* mixer = r.shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer);
    mixer->setMeterSource([](int index) -> std::optional<AudioMixerBridge::MeterReading> {
        if (index == -1)
            return AudioMixerBridge::MeterReading{6, {0.7f, 0.66f, 0.5f, 0.2f, 0.35f, 0.3f}, {0.3f, 0.29f, 0.22f, 0.08f, 0.15f, 0.12f}, false};
        if (index == 0)
            return AudioMixerBridge::MeterReading{2, {0.52f, 0.48f}, {0.22f, 0.2f}, false};
        return std::nullopt;
    });
    r.shell.window->setProperty("workspace", 1); // Audio
    spin(500);
    QQuickItem* strip = r.item(QStringLiteral("mixerStrip_A1"));
    REQUIRE(strip);
    QQuickItem* knob = findNamed(strip, QStringLiteral("panKnob"));
    QQuickItem* panner = findNamed(strip, QStringLiteral("surroundPanner"));
    REQUIRE(knob);
    REQUIRE(panner);
    CHECK(knob->isVisible());
    CHECK_FALSE(panner->isVisible());

    // Master: 5.1 (the master strip's format tabs).
    QQuickItem* tabs = r.item(QStringLiteral("masterFormatTabs"));
    REQUIRE(tabs);
    std::size_t before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(tabs, "select", Q_ARG(QVariant, 1)));
    CHECK(r.undo() == before + 1);
    CHECK(r.channels->masterFormat() == QStringLiteral("5.1"));
    REQUIRE(waitFor([&] { return panner->isVisible() && !knob->isVisible(); }, 2000));

    // A drag of the puck and the LFE send: one undo step.
    before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(panner, "moved", Q_ARG(double, -0.3), Q_ARG(double, 0.6), Q_ARG(double, -144.0)));
    REQUIRE(QMetaObject::invokeMethod(panner, "moved", Q_ARG(double, -0.6), Q_ARG(double, 0.2), Q_ARG(double, -18.0)));
    REQUIRE(QMetaObject::invokeMethod(panner, "released"));
    CHECK(r.undo() == before + 1);
    const Track& a1 = *r.controller->currentSequence()->findTrack(r.tl.track("A1"));
    CHECK(a1.surround.x == ParamValue(-0.6));
    CHECK(a1.surround.y == ParamValue(0.2));
    CHECK(a1.surround.lfeDb == ParamValue(-18.0));
    QQuickItem* puck = findNamed(panner, QStringLiteral("surroundPuck"));
    QQuickItem* field = findNamed(panner, QStringLiteral("surroundField"));
    REQUIRE(puck);
    REQUIRE(field);
    // The puck sits left of the centre and in front of it.
    REQUIRE(waitFor([&] { return puck->x() + puck->width() / 2 < field->width() / 2; }, 2000));
    CHECK(puck->y() + puck->height() / 2 < field->height() / 2);

    // A 5.1 track has no panner on a 5.1 master: its knob shows, off.
    REQUIRE(r.channels->setTrackFormat(trackKey(r.tl.track("A1")), QStringLiteral("5.1")).isEmpty());
    REQUIRE(waitFor([&] { return knob->isVisible() && !panner->isVisible(); }, 2000));
    CHECK_FALSE(knob->isEnabled());
    REQUIRE(r.controller->undo());
    REQUIRE(waitFor([&] { return panner->isVisible(); }, 2000));

    // The master meter: six channels.
    auto* masterMeter = qobject_cast<MeterBar*>(r.item(QStringLiteral("masterMeter")));
    REQUIRE(masterMeter);
    REQUIRE(waitFor([&] { return masterMeter->channels() == 6; }, 5000));
    spin(200);
    QQuickItem* mixerPanel = r.shell.item("audioMixerPanel");
    REQUIRE(mixerPanel);
    r.shot(mixerPanel, QStringLiteral("surround-panner.png"));
    checkNoMessages();
}

TEST_CASE("Effect Controls: Channel Volume rows for the selected clip's sound", "[app][channels][qml]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    QQuickItem* section = r.item(QStringLiteral("channelVolumeSection"));
    REQUIRE(section);
    CHECK_FALSE(section->isVisible());
    r.controller->selectClip(r.key("A1@0"));
    REQUIRE(waitFor([&] { return section->isVisible(); }, 3000));
    QQuickItem* row1 = findNamed(section, QStringLiteral("channelVolumeRow1"));
    QQuickItem* row2 = findNamed(section, QStringLiteral("channelVolumeRow2"));
    REQUIRE(row1);
    REQUIRE(row2);
    QPointer<QQuickItem> slider = findNamed(row2, QStringLiteral("channelSlider"));
    REQUIRE(slider);
    CHECK(slider->property("label").toString() == QStringLiteral("R"));

    // A drag on R: one undo step. The slider being dragged lives through it (each step changes the rows' values, not
    // the rows).
    const std::size_t before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(slider, "valueModified", Q_ARG(double, -3.0)));
    spin(50);
    REQUIRE(slider);
    REQUIRE(QMetaObject::invokeMethod(slider, "valueModified", Q_ARG(double, -7.5)));
    spin(50);
    REQUIRE(slider);
    REQUIRE(QMetaObject::invokeMethod(slider, "gestureEnded"));
    CHECK(r.undo() == before + 1);
    REQUIRE(r.clip("A1@0").channelMap());
    CHECK(r.clip("A1@0").channelMap()->channels[1].gainDb == ParamValue(-7.5));
    CHECK(slider->property("value").toDouble() == Catch::Approx(-7.5));
    // The stopwatch on L: its key at the playhead appears.
    row1 = findNamed(section, QStringLiteral("channelVolumeRow1"));
    REQUIRE(row1);
    QPointer<QQuickItem> stopwatch = findNamed(row1, QStringLiteral("channelStopwatch"));
    QPointer<QQuickItem> key = findNamed(row1, QStringLiteral("channelKey"));
    REQUIRE(stopwatch);
    REQUIRE(key);
    CHECK_FALSE(key->isVisible());
    click(stopwatch);
    REQUIRE(waitFor([&] { return key->isVisible() && key->property("checked").toBool(); }, 2000));
    CHECK(r.clip("A1@0").channelMap()->channels[0].gainDb.isCurve());

    // A picture without sound: no Channel Volume.
    r.controller->selectClip(r.key("V2@10"));
    REQUIRE(waitFor([&] { return !section->isVisible(); }, 3000));
    checkNoMessages();
}

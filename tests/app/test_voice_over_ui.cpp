// M5m: the voice-over UI (A.8) in the app, offscreen; no window is shown and no audio device is opened.
//   * record arm: the track header's microphone and the mixer strip's R run track.setRecordArm (one undo step each);
//     a video track is refused;
//   * the count-in overlay over the Program Monitor and the input meter: hidden while the bridge is idle; the count, the
//     pre-roll / recording / post-roll badge, the elapsed time and the meter follow the state they are given (the
//     VoiceOver bridge has no injectable input, so its state cannot be driven here without a microphone);
//   * the takes submenu: a recorded slot's takes, the one heard checked; choosing one is record.useTake;
//   * Settings > Audio: the device list, the voice-over options and the calibration, saved and applied on the next
//     start (VoiceOverSync); screenshots.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "AudioMixerBridge.h"
#include "BridgeIds.h"
#include "ProjectController.h"
#include "VoiceOverBridge.h"

#include <wabcut/audio/edit/VoiceOver.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QColor>
#include <QDir>
#include <QQmlComponent>
#include <QQuickItem>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

constexpr const char* kPicture = R"(
    V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    A2     |..................................................|
)";

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

QObject* child(QObject* root, const char* name) {
    QObject* found = root->findChild<QObject*>(QString::fromLatin1(name));
    INFO(name);
    REQUIRE(found);
    return found;
}

/// An object of the Wabcut module made in the shell's engine (`qml`: its source).
std::unique_ptr<QObject> makeQml(Shell& shell, const QByteArray& qml) {
    QQmlComponent component(&shell.engine);
    component.setData(qml, QUrl(QStringLiteral("qrc:/m5m-voiceover/Host.qml")));
    std::unique_ptr<QObject> object(component.create());
    INFO(component.errorString().toStdString());
    REQUIRE(object);
    return object;
}

/// A visual item of the Wabcut module put over `parent` (filling it), the way Main.qml places it in the app.
std::unique_ptr<QQuickItem> makeItem(Shell& shell, const QByteArray& qml, QQuickItem* parent) {
    std::unique_ptr<QObject> object = makeQml(shell, qml);
    auto* item = qobject_cast<QQuickItem*>(object.get());
    REQUIRE(item);
    object.release();
    item->setParentItem(parent);
    item->setPosition(QPointF(0, 0));
    item->setSize(parent->size());
    item->setZ(1000);
    return std::unique_ptr<QQuickItem>(item);
}

audio::edit::RecordedTake recordedTake(const std::string& name, Ticks start, Ticks duration) {
    audio::edit::RecordedTake t;
    t.path = "C:/Projects/Film/Voice-over/" + name + ".wav";
    t.fingerprint.size = 44 + duration.value / 5'292'000 * 3;
    t.fingerprint.sampleHash = "0123456789abcdef";
    t.start = start;
    t.duration = duration;
    t.device = "Microphone (USB Audio Device)";
    t.createdAt = "2026-10-12T09:30:00Z";
    return t;
}

struct ShellRig {
    Shell shell;
    ProjectController* controller = nullptr;
    VoiceOverBridge* voiceOver = nullptr;
    Timeline tl{kPicture};

    ShellRig() {
        REQUIRE(shell.window);
        shell.resize(kShotWidth, kShotHeight);
        shell.theme->setProperty("reduceMotion", true);
        shell.setAppearance(true, "ultraviolet");
        controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
        voiceOver = shell.engine.singletonInstance<VoiceOverBridge*>("Wabcut", "VoiceOver");
        REQUIRE(controller);
        REQUIRE(voiceOver);
        controller->resetProject(tl.state, tl.seq);
        spin(200);
    }
    std::size_t undo() const { return controller->session().undoCount(); }
    bool armed(std::string_view label) const { return controller->currentSequence()->findTrack(tl.track(label))->recordArmed; }
};

/// The AudioMixer strips carry the model's record arm (the buttons' armed state reads it).
bool stripsCarryArm(Shell& shell) {
    auto* mixer = shell.engine.singletonInstance<AudioMixerBridge*>("Wabcut", "AudioMixer");
    REQUIRE(mixer);
    const QVariantList strips = mixer->strips();
    return !strips.isEmpty() && strips.front().toMap().contains(QStringLiteral("recordArmed"));
}

void checkNoMessages() {
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

} // namespace

TEST_CASE("Voice-over record arm: the track header microphone and the mixer strip R, one undo step each",
          "[app][voiceover][qml]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    REQUIRE(stripsCarryArm(r.shell));

    // The track header: audio rows have the microphone, video rows do not.
    QQuickItem* header = findNamed(r.shell.window->contentItem(), QStringLiteral("recordArm_A2"));
    QQuickItem* video = findNamed(r.shell.window->contentItem(), QStringLiteral("recordArm_V1"));
    REQUIRE(header);
    REQUIRE(video);
    CHECK(header->isVisible());
    CHECK_FALSE(video->isVisible());
    CHECK_FALSE(header->property("armed").toBool());
    CHECK_FALSE(r.armed("A2"));

    std::size_t before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(header, "clicked"));
    INFO("refusal: " << header->property("error").toString().toStdString());
    CHECK(r.armed("A2"));
    CHECK(r.undo() == before + 1);
    CHECK(header->property("error").toString().isEmpty());
    REQUIRE(waitFor([&] { return header->property("armed").toBool(); }, 2000));
    // A second click disarms.
    REQUIRE(QMetaObject::invokeMethod(header, "clicked"));
    CHECK_FALSE(r.armed("A2"));
    CHECK(r.undo() == before + 2);
    REQUIRE(r.controller->undo());
    CHECK(r.armed("A2"));
    REQUIRE(waitFor([&] { return header->property("armed").toBool(); }, 2000));
    // Undo takes the arm back.
    REQUIRE(r.controller->undo());
    CHECK_FALSE(r.armed("A2"));
    REQUIRE(waitFor([&] { return !header->property("armed").toBool(); }, 2000));

    // A video track cannot be armed: the refusal is the button's tooltip, nothing changes.
    before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(video, "clicked"));
    CHECK_FALSE(video->property("error").toString().isEmpty());
    CHECK(r.undo() == before);
    CHECK(video->property("tip").toString() == video->property("error").toString());

    // A locked track can be armed (Premiere).
    r.controller->setTrackFlag(trackKey(r.tl.track("A1")), QStringLiteral("locked"), true);
    REQUIRE(r.controller->currentSequence()->findTrack(r.tl.track("A1"))->locked);
    QQuickItem* a1 = findNamed(r.shell.window->contentItem(), QStringLiteral("recordArm_A1"));
    REQUIRE(a1);
    REQUIRE(QMetaObject::invokeMethod(a1, "clicked"));
    CHECK(r.armed("A1"));
    REQUIRE(r.controller->undo());
    CHECK_FALSE(r.armed("A1"));

    // The mixer strip's R (the strip as the Audio Track Mixer shows it).
    const auto strip = makeItem(r.shell, "import Wabcut\nMixerStrip { strip: AudioMixer.strips[1] }\n", r.shell.window->contentItem());
    QQuickItem* record = findNamed(strip.get(), QStringLiteral("recordToggle"));
    REQUIRE(record);
    CHECK(record->isEnabled());
    CHECK(record->property("letter").toBool());
    CHECK(record->property("trackLabel").toString() == QStringLiteral("A2"));
    before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(record, "clicked"));
    CHECK(r.armed("A2"));
    CHECK(r.undo() == before + 1);
    REQUIRE(waitFor([&] { return record->property("armed").toBool() && header->property("armed").toBool(); }, 2000));
    REQUIRE(QMetaObject::invokeMethod(record, "clicked"));
    CHECK_FALSE(r.armed("A2"));
    CHECK(r.undo() == before + 2);
    // Not recording: the buttons show no pass.
    CHECK_FALSE(record->property("recordingHere").toBool());
    CHECK_FALSE(r.voiceOver->recording());
    checkNoMessages();
}

TEST_CASE("Voice-over overlay: the count-in and the recording badge follow the pass state; screenshot",
          "[app][voiceover][qml][screenshot]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    QQuickItem* well = findNamed(r.shell.window->contentItem(), QStringLiteral("programMonitorWell"));
    REQUIRE(well);

    // Bound to the bridge: idle, so nothing shows.
    const auto idle = makeItem(r.shell, "import Wabcut\nVoiceOverOverlay { anchors.fill: parent }\n", well);
    CHECK(r.voiceOver->state() == QStringLiteral("idle"));
    CHECK(idle->property("passState").toString() == QStringLiteral("idle"));
    CHECK_FALSE(idle->isVisible());
    QQuickItem* idleMeter = findNamed(idle.get(), QStringLiteral("voiceOverMeter"));
    REQUIRE(idleMeter);
    CHECK_FALSE(idleMeter->property("live").toBool()); // the input is closed: no levels
    idle->setVisible(false);

    // The count-in over the Program Monitor.
    const auto overlay = makeItem(r.shell, "import Wabcut\nVoiceOverOverlay { anchors.fill: parent }\n", well);
    overlay->setProperty("trackLabel", QStringLiteral("A2"));
    overlay->setProperty("count", 3);
    overlay->setProperty("passState", QStringLiteral("countIn"));
    spin(100);
    CHECK(overlay->isVisible());
    CHECK(overlay->width() == well->width());
    CHECK(overlay->height() == well->height());
    // The Program Monitor has its own, bound to the bridge (idle: hidden), filling the well too.
    QQuickItem* monitorOverlay = nullptr;
    for (QQuickItem* child : well->childItems()) {
        if (child->objectName() == QStringLiteral("voiceOverOverlay") && child != overlay.get() && child != idle.get())
            monitorOverlay = child;
    }
    REQUIRE(monitorOverlay);
    CHECK_FALSE(monitorOverlay->isVisible());
    CHECK(monitorOverlay->width() == well->width());
    QQuickItem* countIn = findNamed(overlay.get(), QStringLiteral("voiceOverCountIn"));
    QQuickItem* number = findNamed(overlay.get(), QStringLiteral("voiceOverCountNumber"));
    QQuickItem* badge = findNamed(overlay.get(), QStringLiteral("voiceOverBadge"));
    QQuickItem* frame = findNamed(overlay.get(), QStringLiteral("voiceOverRecordingFrame"));
    REQUIRE(countIn);
    REQUIRE(number);
    REQUIRE(badge);
    REQUIRE(frame);
    CHECK(countIn->isVisible());
    CHECK_FALSE(badge->isVisible());
    CHECK(number->property("text").toString() == QStringLiteral("3"));
    const QImage image = r.shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = well->mapRectToScene(QRectF(0, 0, well->width(), well->height()));
    REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("voice-over-countin.png"))));
    overlay->setProperty("count", 2);
    CHECK(number->property("text").toString() == QStringLiteral("2"));

    // Pre-roll: the badge, dimmed (a hollow dot, muted text).
    QQuickItem* label = findNamed(overlay.get(), QStringLiteral("voiceOverLabel"));
    QQuickItem* elapsed = findNamed(overlay.get(), QStringLiteral("voiceOverElapsed"));
    QQuickItem* dot = findNamed(overlay.get(), QStringLiteral("voiceOverDot"));
    QQuickItem* meter = findNamed(overlay.get(), QStringLiteral("voiceOverMeter"));
    REQUIRE(label);
    REQUIRE(elapsed);
    REQUIRE(dot);
    REQUIRE(meter);
    overlay->setProperty("passState", QStringLiteral("preroll"));
    overlay->setProperty("seconds", 0.0);
    CHECK_FALSE(countIn->isVisible());
    CHECK(badge->isVisible());
    CHECK_FALSE(frame->isVisible());
    CHECK(label->property("text").toString() == QStringLiteral("Pre-roll"));
    CHECK(dot->property("color").value<QColor>().alpha() == 0);

    // Recording: the red dot, the time recorded, the red frame round the well.
    overlay->setProperty("passState", QStringLiteral("recording"));
    overlay->setProperty("seconds", 65.34);
    CHECK(label->property("text").toString() == QStringLiteral("Recording"));
    CHECK(elapsed->property("text").toString() == QStringLiteral("1:05.3"));
    CHECK(frame->isVisible());
    CHECK(sameColor(dot->property("color").value<QColor>(), r.shell.themeColor("danger")));

    // The meter follows the levels it is given: RMS -12 dB is in the lime, an over lights the clip light.
    overlay->setProperty("levels", QVariantMap{{QStringLiteral("peakDb"), -3.0}, {QStringLiteral("rmsDb"), -12.0},
                                              {QStringLiteral("clipped"), true}});
    CHECK(meter->property("live").toBool());
    CHECK(meter->property("rmsPosition").toDouble() == Catch::Approx(0.8));
    CHECK(meter->property("peakPosition").toDouble() == Catch::Approx(0.95));
    CHECK(sameColor(meter->property("levelColor").value<QColor>(), r.shell.themeColor("hueLime")));
    CHECK(meter->property("clipHeld").toBool());
    overlay->setProperty("levels", QVariantMap{{QStringLiteral("peakDb"), -30.0}, {QStringLiteral("rmsDb"), -40.0},
                                              {QStringLiteral("clipped"), false}});
    CHECK(meter->property("clipHeld").toBool()); // held until clicked or the input closes
    CHECK(sameColor(meter->property("levelColor").value<QColor>(), r.shell.themeColor("hueMint")));

    // Post-roll dimmed again; the input closing clears the light; idle hides it all.
    overlay->setProperty("passState", QStringLiteral("postroll"));
    CHECK(label->property("text").toString() == QStringLiteral("Post-roll"));
    CHECK_FALSE(frame->isVisible());
    overlay->setProperty("levels", QVariantMap{});
    CHECK_FALSE(meter->property("clipHeld").toBool());
    overlay->setProperty("passState", QStringLiteral("finished"));
    CHECK_FALSE(overlay->isVisible());
    checkNoMessages();
}

TEST_CASE("Voice-over takes submenu: the slot's takes, the one heard checked; choosing one is record.useTake",
          "[app][voiceover][qml]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    // Two passes over the same range of A2 (as the bridge commits them after a pass).
    const TrackId a2 = r.tl.track("A2");
    const ProjectSnapshot start = r.controller->session().snapshot();
    auto first = audio::edit::placeVoiceOverTake(*start, r.tl.seq, a2, recordedTake("take1", f(4), f(40)));
    REQUIRE(first);
    auto second = audio::edit::placeVoiceOverTake(first->state, r.tl.seq, a2, recordedTake("take2", f(4), f(40)));
    REQUIRE(second);
    REQUIRE(r.controller->session().commit("Voice-over Record", second->state));
    REQUIRE(second->placed.size() == 1);
    const QString heard = clipKey(second->placed.front());

    const auto menu = makeQml(r.shell, "import Wabcut\nTakesSubmenu { clipKey: \"" + heard.toUtf8() + "\" }\n");
    CHECK(menu->property("takes").toList().size() == 2);
    CHECK(menu->property("enabled").toBool());
    CHECK(menu->property("title").toString() == QStringLiteral("Takes (2)"));
    REQUIRE(menu->property("count").toInt() == 2);
    const auto itemAt = [&](int i) {
        QQuickItem* item = nullptr;
        REQUIRE(QMetaObject::invokeMethod(menu.get(), "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i)));
        REQUIRE(item);
        return item;
    };
    // Newest first, the one heard checked.
    CHECK(itemAt(0)->objectName() == QStringLiteral("takeItem_2"));
    CHECK(itemAt(0)->property("checked").toBool());
    CHECK(itemAt(0)->property("text").toString().startsWith(QStringLiteral("Take 2 · 1.6 s · ")));
    CHECK(itemAt(0)->property("text").toString().endsWith(QStringLiteral(" · Microphone (USB Audio Device)")));
    CHECK(itemAt(1)->objectName() == QStringLiteral("takeItem_1"));
    CHECK_FALSE(itemAt(1)->property("checked").toBool());

    // Take 1: record.useTake, one undo step; take 1's media is heard over the range.
    const std::size_t before = r.undo();
    REQUIRE(QMetaObject::invokeMethod(itemAt(1), "triggered"));
    CHECK(menu->property("error").toString().isEmpty());
    CHECK(r.undo() == before + 1);
    const ProjectSnapshot after = r.controller->session().snapshot();
    const Track& track = *after->findSequence(r.tl.seq)->findTrack(a2);
    REQUIRE(track.clips.size() == 1);
    const Clip& now = track.clips[0];
    CHECK(now.start() == f(4));
    CHECK(now.duration() == f(40));
    const MediaItem* media = after->findMedia(now.media);
    REQUIRE(media);
    REQUIRE(media->recording);
    CHECK(media->recording->take == 1);
    // The clip was replaced: the host gives the menu the new clip when it opens again.
    menu->setProperty("clipKey", clipKey(now.id));
    REQUIRE(menu->property("count").toInt() == 2);
    CHECK(itemAt(1)->property("checked").toBool());
    CHECK_FALSE(itemAt(0)->property("checked").toBool());

    // The timeline's clip menu: a recorded take's clip has the Takes submenu after the audio tools; other clips do not.
    QObject* clipMenu = r.shell.window->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(clipMenu);
    QQuickItem* lanes = findNamed(r.shell.window->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    const auto openClipMenu = [&](const QString& clip) {
        REQUIRE(QMetaObject::invokeMethod(clipMenu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                          Q_ARG(QVariant, clip), Q_ARG(QVariant, QPointF(200, 20))));
    };
    const auto takesRow = [&] {
        QVariant at;
        REQUIRE(QMetaObject::invokeMethod(clipMenu, "takesIndex", Q_RETURN_ARG(QVariant, at)));
        return at.toInt();
    };
    openClipMenu(clipKey(now.id));
    const int row = takesRow();
    REQUIRE(row >= 0);
    QQuickItem* takesItem = nullptr;
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, takesItem), Q_ARG(int, row)));
    REQUIRE(takesItem);
    CHECK(takesItem->property("text").toString() == QStringLiteral("Takes (2)"));
    QQuickItem* previous = nullptr;
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "itemAt", Q_RETURN_ARG(QQuickItem*, previous), Q_ARG(int, row - 1)));
    REQUIRE(previous);
    CHECK(previous->objectName() == QStringLiteral("synchronizeMenuItem"));
    QMetaObject::invokeMethod(clipMenu, "close");
    openClipMenu(clipKey(r.tl.clip("A1@0")));
    CHECK(takesRow() == -1);
    QMetaObject::invokeMethod(clipMenu, "close");
    // Opening on the take again puts it back once.
    openClipMenu(clipKey(now.id));
    CHECK(takesRow() == row);
    QMetaObject::invokeMethod(clipMenu, "close");

    // A clip that was not recorded has no takes: the submenu is disabled.
    menu->setProperty("clipKey", clipKey(r.tl.clip("A1@0")));
    CHECK(menu->property("takes").toList().isEmpty());
    CHECK_FALSE(menu->property("enabled").toBool());
    CHECK(menu->property("title").toString() == QStringLiteral("Takes"));
    CHECK(r.voiceOver->takesOf(clipKey(r.tl.clip("A1@0"))).isEmpty());
    checkNoMessages();
}

TEST_CASE("Settings Audio: devices, voice-over options and calibration, saved for the next start; screenshot",
          "[app][voiceover][qml][screenshot]") {
    resetSettings();
    clearMessages();
    {
        ShellRig r; // Main's VoiceOverSync loads and saves the options
        // The devices are given (enumerating would start the system's audio); "System default" comes first.
        const auto host = makeItem(r.shell,
                                   "import QtQuick\nimport Wabcut\nItem {\n    property alias dialog: settings\n"
                                   "    AudioSettings {\n        id: settings\n        refreshOnOpen: false\n"
                                   "        devices: [{ id: \"mic-usb\", name: \"USB microphone\", isDefault: true },\n"
                                   "                  { id: \"mic-line\", name: \"Line in\", isDefault: false }]\n    }\n}\n",
                                   r.shell.window->contentItem());
        auto* dialog = host->property("dialog").value<QObject*>();
        REQUIRE(dialog);
        REQUIRE(QMetaObject::invokeMethod(dialog, "open"));
        REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
        const QVariantList options = dialog->property("deviceOptions").toList();
        REQUIRE(options.size() == 3);
        CHECK(options[0].toMap().value(QStringLiteral("id")).toString().isEmpty());
        CHECK(options[0].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("System default"));
        CHECK(options[1].toMap().value(QStringLiteral("name")).toString() == QStringLiteral("USB microphone (default)"));
        QObject* pill = child(dialog, "inputDeviceButton");
        CHECK(pill->property("text").toString() == QStringLiteral("System default"));

        // Choosing a microphone: the bridge records from it (nothing opens), and it is saved.
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "inputDeviceMenu"), "chosen", Q_ARG(QString, QStringLiteral("mic-line"))));
        CHECK(r.voiceOver->inputDevice() == QStringLiteral("mic-line"));
        CHECK_FALSE(r.voiceOver->inputOpen());
        CHECK(pill->property("text").toString() == QStringLiteral("Line in"));
        CHECK(r.shell.preferences->property("voiceOverInput").toString() == QStringLiteral("mic-line"));

        // The options: each field sets the bridge's option, and Preferences keeps it.
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "countInField"), "valueModified", Q_ARG(double, 5.0)));
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "prerollField"), "valueModified", Q_ARG(double, 3.5)));
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "postrollField"), "valueModified", Q_ARG(double, 0.5)));
        QObject* beep = child(dialog, "beepToggle");
        beep->setProperty("checked", false);
        REQUIRE(QMetaObject::invokeMethod(beep, "toggled"));
        QObject* monitor = child(dialog, "monitorToggle");
        CHECK_FALSE(child(dialog, "monitorGainField")->property("enabled").toBool());
        monitor->setProperty("checked", true);
        REQUIRE(QMetaObject::invokeMethod(monitor, "toggled"));
        CHECK(child(dialog, "monitorGainField")->property("enabled").toBool());
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "monitorGainField"), "valueModified", Q_ARG(double, -6.0)));
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "calibrationField"), "valueModified", Q_ARG(double, 12.5)));
        // Out of range is clamped by the bridge (0..10 s).
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "countInField"), "valueModified", Q_ARG(double, 14.0)));
        CHECK(r.voiceOver->options().value(QStringLiteral("countIn")).toInt() == 10);
        REQUIRE(QMetaObject::invokeMethod(child(dialog, "countInField"), "valueModified", Q_ARG(double, 5.0)));
        const QVariantMap o = r.voiceOver->options();
        CHECK(o.value(QStringLiteral("countIn")).toInt() == 5);
        CHECK_FALSE(o.value(QStringLiteral("beep")).toBool());
        CHECK(o.value(QStringLiteral("prerollSeconds")).toDouble() == Catch::Approx(3.5));
        CHECK(o.value(QStringLiteral("postrollSeconds")).toDouble() == Catch::Approx(0.5));
        CHECK(o.value(QStringLiteral("monitor")).toBool());
        CHECK(o.value(QStringLiteral("monitorGainDb")).toDouble() == Catch::Approx(-6.0));
        CHECK(o.value(QStringLiteral("calibrationMs")).toDouble() == Catch::Approx(12.5));
        CHECK(r.voiceOver->latency().value(QStringLiteral("calibrationMs")).toDouble() == Catch::Approx(12.5));
        QObject* prefs = r.shell.preferences;
        CHECK(prefs->property("voiceOverCountIn").toInt() == 5);
        CHECK_FALSE(prefs->property("voiceOverBeep").toBool());
        CHECK(prefs->property("voiceOverPreroll").toDouble() == Catch::Approx(3.5));
        CHECK(prefs->property("voiceOverPostroll").toDouble() == Catch::Approx(0.5));
        CHECK(prefs->property("voiceOverMonitor").toBool());
        CHECK(prefs->property("voiceOverMonitorGainDb").toDouble() == Catch::Approx(-6.0));
        CHECK(prefs->property("voiceOverCalibrationMs").toDouble() == Catch::Approx(12.5));
        // A calibration the bridge sets (a latency measurement) is saved too.
        QVariantMap measured = r.voiceOver->options();
        measured.insert(QStringLiteral("calibrationMs"), 7.25);
        r.voiceOver->setOptions(measured);
        CHECK(prefs->property("voiceOverCalibrationMs").toDouble() == Catch::Approx(7.25));

        // "Test input" is off: the microphone stays closed; the meter shows nothing.
        CHECK_FALSE(child(dialog, "testInputToggle")->property("checked").toBool());
        CHECK_FALSE(dialog->property("testing").toBool());
        QObject* meter = child(dialog, "settingsInputMeter");
        CHECK_FALSE(meter->property("live").toBool());
        CHECK(child(dialog, "roundTripLatency")->property("text").toString() == QStringLiteral("Not measured"));

        // The screenshot, with a level on the meter.
        meter->setProperty("levels", QVariantMap{{QStringLiteral("peakDb"), -8.0}, {QStringLiteral("rmsDb"), -17.0},
                                                {QStringLiteral("clipped"), false}});
        spin(200);
        const QImage image = r.shell.grab();
        REQUIRE_FALSE(image.isNull());
        auto* content = dialog->property("contentItem").value<QQuickItem*>();
        REQUIRE(content);
        QQuickItem* card = content->parentItem() ? content->parentItem() : content;
        const double dpr = image.devicePixelRatio();
        const double margin = 16;
        const QRectF area = card->mapRectToScene(QRectF(0, 0, card->width(), card->height()))
                                .adjusted(-margin, -margin, margin, margin)
                                .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
        REQUIRE(image.copy(QRectF(area.x() * dpr, area.y() * dpr, area.width() * dpr, area.height() * dpr).toRect())
                    .save(QDir(outputDir()).filePath(QStringLiteral("audio-settings.png"))));

        REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
        REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
        CHECK_FALSE(r.voiceOver->inputOpen());
        spin(700); // Settings writes after a short delay
        checkNoMessages();
    }

    // The next start: the saved microphone and options reach the new bridge.
    {
        Shell shell;
        REQUIRE(shell.window);
        auto* voiceOver = shell.engine.singletonInstance<VoiceOverBridge*>("Wabcut", "VoiceOver");
        REQUIRE(voiceOver);
        // Main's VoiceOverSync handed them to the bridge at start-up.
        CHECK(voiceOver->inputDevice() == QStringLiteral("mic-line"));
        const QVariantMap o = voiceOver->options();
        CHECK(o.value(QStringLiteral("countIn")).toInt() == 5);
        CHECK_FALSE(o.value(QStringLiteral("beep")).toBool());
        CHECK(o.value(QStringLiteral("prerollSeconds")).toDouble() == Catch::Approx(3.5));
        CHECK(o.value(QStringLiteral("postrollSeconds")).toDouble() == Catch::Approx(0.5));
        CHECK(o.value(QStringLiteral("monitor")).toBool());
        CHECK(o.value(QStringLiteral("monitorGainDb")).toDouble() == Catch::Approx(-6.0));
        CHECK(o.value(QStringLiteral("calibrationMs")).toDouble() == Catch::Approx(7.25));
        CHECK_FALSE(voiceOver->inputOpen());
        checkNoMessages();
    }
    resetSettings();
}

TEST_CASE("Voice-over in the window: the Program Monitor's record button, the microphone pill, Settings Audio from the rail",
          "[app][voiceover][qml]") {
    resetSettings();
    clearMessages();
    ShellRig r;
    // Record Voice-over beside Lift and Extract (record.toggle); not recording.
    QQuickItem* record = findNamed(r.shell.window->contentItem(), QStringLiteral("programRecordVoiceOver"));
    REQUIRE(record);
    CHECK(record->isVisible());
    CHECK(record->property("command").toString() == QStringLiteral("record.toggle"));
    CHECK(record->property("tooltip").toString() == QStringLiteral("Record Voice-over"));
    CHECK_FALSE(record->property("checked").toBool());
    // The top bar's microphone pill shows only while the input is open.
    QQuickItem* pill = findNamed(r.shell.window->contentItem(), QStringLiteral("microphonePill"));
    REQUIRE(pill);
    CHECK_FALSE(pill->isVisible());

    // Settings > Audio: the rail asks, the window opens the dialog (the devices are not enumerated here).
    QObject* loader = r.shell.object("audioSettingsLoader");
    REQUIRE(loader);
    CHECK_FALSE(loader->property("active").toBool());
    loader->setProperty("active", true);
    auto* dialog = loader->property("item").value<QObject*>();
    REQUIRE(dialog);
    dialog->setProperty("refreshOnOpen", false);
    QObject* rail = r.shell.object("leftRail");
    REQUIRE(rail);
    REQUIRE(QMetaObject::invokeMethod(rail, "audioSettingsRequested"));
    REQUIRE(waitFor([&] { return dialog->property("opened").toBool(); }, 3000));
    CHECK_FALSE(r.voiceOver->inputOpen()); // opening the dialog does not open the microphone
    REQUIRE(QMetaObject::invokeMethod(dialog, "close"));
    REQUIRE(waitFor([&] { return !dialog->property("visible").toBool(); }, 3000));
    checkNoMessages();
}

// P.2 (E5): Premiere's editing and sequence commands in the app (EditingBridge, singleton `Editing`), offscreen; no
// window is shown.
//   * the bridge over the typed commands: Video Options (Scale to Frame Size, Set to Frame Size / Fill Frame, Frame
//     Hold Options), Duplicate, New Sequence From Clip, Sequence Settings, Make Subsequence, Join Through Edits, Go to
//     Gap, Close Gap, Merge Clips, Interpret Footage, Modify > Timecode, Make Offline - one undo step each;
//   * the History steps and going back to one; the Media and Timeline preferences;
//   * in the shell: the File menu's entries, the clip menu's Video Options and Replace With Clip submenus, and the
//     dialogs and the History panel the bridge's requests open.

#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "EditingBridge.h"
#include "ProjectController.h"

#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/model/RawSourceSettings.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>


#include <array>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

struct EditingRig {
    Timeline tl;
    ProjectController controller;
    EditingBridge editing;

    explicit EditingRig(std::string_view picture) : tl(picture) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        editing.setController(&controller);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    const Sequence& sequence() const { return *controller.currentSequence(); }
    /// The clip that started at `ref` in the picture, as it is now.
    Clip now(std::string_view ref) const { return sequence().findClip(tl.clip(ref))->clip(); }
    QString key(std::string_view ref) const { return clipKey(tl.clip(ref)); }
    const MediaItem& media(char letter) const { return *controller.snapshot()->findMedia(tl.media(letter)); }
};

/// `tl` with media `letter` made a ProRes RAW clip (a Bayer stream) at `path`.
void makeRaw(Timeline& tl, char letter, const std::string& path) {
    for (MediaPtr& m : tl.state.media) {
        if (m->id == tl.media(letter)) {
            auto raw = std::make_shared<MediaItem>(*m);
            raw->locator.path = path;
            raw->info.path = path;
            if (raw->info.video.empty())
                raw->info.video.emplace_back();
            raw->info.video.front().codec = "prores_raw";
            raw->info.video.front().pixelFormat = "bayer_rggb16le";
            m = std::move(raw);
        }
    }
}

QObject* findNamed(QObject* root, const QString& name) {
    if (!root)
        return nullptr;
    if (root->objectName() == name)
        return root;
    return root->findChild<QObject*>(name);
}

} // namespace

TEST_CASE("Editing: Video Options scale the selected clip to the frame in one undo step each", "[app][editing]") {
    EditingRig r(R"(
        V1 tgt |A10aaaaaaa|
        A1 tgt |A10aaaaaaa|
    )");
    // A vertical sequence: the 1920x1080 picture fits at 56.25 % and fills at 177.78 %.
    REQUIRE(r.editing.applySequenceSettings(sequenceKey(r.tl.seq), {{QStringLiteral("width"), 1080},
                                                                    {QStringLiteral("height"), 1920}})
                .isEmpty());
    CHECK(r.sequence().width == 1080);
    CHECK(r.sequence().height == 1920);
    r.controller.selectClip(r.key("V1@0"));

    const QVariantMap info = r.editing.clipVideoInfo(r.key("A1@0")); // a linked audio clip reports its picture
    CHECK(info.value(QStringLiteral("video")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("scaleToFrame")).toBool());

    std::size_t before = r.undo();
    REQUIRE(r.editing.scaleToFrameSize(r.key("V1@0"), true).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.now("V1@0").content().scaleToFrame);
    CHECK(r.editing.clipVideoInfo(r.key("V1@0")).value(QStringLiteral("scaleToFrame")).toBool());
    CHECK(scaleToFrameFactor(*r.controller.snapshot(), r.now("V1@0"), 1080, 1920) == Catch::Approx(0.5625));
    // The key toggles it back off.
    REQUIRE(r.editing.runCommand(QStringLiteral("clip.scaleToFrameSize")));
    CHECK_FALSE(r.now("V1@0").content().scaleToFrame);

    before = r.undo();
    REQUIRE(r.editing.setToFrameSize({}).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.now("V1@0").properties()[ParamId::Scale].constant() == Catch::Approx(0.5625));
    REQUIRE(r.editing.runCommand(QStringLiteral("clip.setToFillFrame")));
    CHECK(r.now("V1@0").properties()[ParamId::Scale].constant() == Catch::Approx(1920.0 / 1080.0).epsilon(1e-6));
    CHECK(r.editing.message().isEmpty());

    // Frame Hold Options on the In point, holding the effects too.
    before = r.undo();
    REQUIRE(r.editing
                .applyFrameHoldOptions(r.key("V1@0"), {{QStringLiteral("holdOn"), QStringLiteral("inPoint")},
                                                       {QStringLiteral("holdFilters"), true}})
                .isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.now("V1@0").isFreeze());
    CHECK(r.now("V1@0").sourceIn == f(10));
    CHECK(r.now("V1@0").content().holdFilters);
    CHECK(r.editing.frameHoldInfo(r.key("V1@0")).value(QStringLiteral("frozen")).toBool());
    CHECK_FALSE(r.editing.frameHoldInfo(r.key("A1@0")).value(QStringLiteral("available")).toBool());

    // Nothing selected: the keys say why.
    r.controller.clearSelection();
    CHECK_FALSE(r.editing.setToFrameSize({}).isEmpty());
    CHECK_FALSE(r.editing.message().isEmpty());
}

TEST_CASE("Editing: the History steps list the undo steps and a click goes back to one", "[app][editing][history]") {
    EditingRig r(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    int changed = 0;
    QObject::connect(&r.editing, &EditingBridge::historyChanged, [&] { ++changed; });
    QVariantList steps = r.editing.historySteps();
    REQUIRE(steps.size() == 1);
    CHECK(steps.front().toMap().value(QStringLiteral("state")).toString() == QStringLiteral("current"));

    r.controller.selectClip(r.key("V1@0"));
    REQUIRE(r.editing.scaleToFrameSize({}, true).isEmpty());
    REQUIRE(r.editing.applySequenceSettings(sequenceKey(r.tl.seq), {{QStringLiteral("width"), 1280},
                                                                    {QStringLiteral("height"), 720}})
                .isEmpty());
    REQUIRE(r.editing.setToFrameSize({}).isEmpty());
    CHECK(changed > 0);
    steps = r.editing.historySteps();
    REQUIRE(steps.size() == 4);
    const auto state = [&](int i) { return r.editing.historySteps().at(i).toMap().value(QStringLiteral("state")).toString(); };
    const auto label = [&](int i) { return r.editing.historySteps().at(i).toMap().value(QStringLiteral("label")).toString(); };
    CHECK(label(1) == QStringLiteral("Scale to Frame Size"));
    CHECK(label(3) == QStringLiteral("Set to Frame Size"));
    CHECK(state(3) == QStringLiteral("current"));

    // Back to the first step: two undos at once; the later steps stay, dimmed, and redo.
    REQUIRE(r.editing.goToHistoryStep(1));
    CHECK(r.undo() == 1);
    CHECK(r.sequence().width == 1920);
    CHECK(state(0) == QStringLiteral("done"));
    CHECK(state(1) == QStringLiteral("current"));
    CHECK(state(2) == QStringLiteral("undone"));
    CHECK(state(3) == QStringLiteral("undone"));
    REQUIRE(r.editing.goToHistoryStep(3));
    CHECK(r.undo() == 3);
    CHECK(r.sequence().width == 1280);
    REQUIRE(r.editing.goToHistoryStep(0)); // the project as opened
    CHECK(r.undo() == 0);
    CHECK_FALSE(r.now("V1@0").content().scaleToFrame);
    CHECK_FALSE(r.editing.goToHistoryStep(9));
}

TEST_CASE("Editing: the Media and Timeline preferences are saved and read where they apply", "[app][editing][prefs]") {
    resetSettings();
    EditingBridge editing;
    int changed = 0;
    QObject::connect(&editing, &EditingBridge::preferencesChanged, [&] { ++changed; });
    CHECK(editing.defaultMediaScaling() == QStringLiteral("none"));
    CHECK(EditingBridge::defaultMediaScalingSetting() == MediaScaling::None);
    CHECK(editing.stillDuration() == Catch::Approx(5.0));
    CHECK_FALSE(EditingBridge::stillDurationSetting().has_value());
    CHECK_FALSE(EditingBridge::defaultTransitionsSetting().videoDuration.has_value());

    editing.setDefaultMediaScaling(QStringLiteral("setToFillFrame"));
    CHECK(EditingBridge::defaultMediaScalingSetting() == MediaScaling::SetToFill);
    editing.setDefaultMediaScaling(QStringLiteral("sideways")); // not one: ignored
    CHECK(EditingBridge::defaultMediaScalingSetting() == MediaScaling::SetToFill);
    editing.setStillDuration(3.0);
    REQUIRE(EditingBridge::stillDurationSetting().has_value());
    CHECK(*EditingBridge::stillDurationSetting() == Ticks::fromSeconds(3));
    editing.setVideoTransitionDuration(0.5);
    editing.setAudioTransitionDuration(2.0);
    const edit::DefaultTransitions transitions = EditingBridge::defaultTransitionsSetting();
    REQUIRE(transitions.videoDuration.has_value());
    CHECK(*transitions.videoDuration == Ticks{kTicksPerSecond / 2});
    REQUIRE(transitions.audioDuration.has_value());
    CHECK(*transitions.audioDuration == Ticks::fromSeconds(2));
    CHECK(changed == 4);

    // The keys open the pages.
    QStringList pages;
    QObject::connect(&editing, &EditingBridge::preferencesRequested, [&](const QString& page) { pages << page; });
    REQUIRE(editing.runCommand(QStringLiteral("edit.preferencesMedia")));
    REQUIRE(editing.runCommand(QStringLiteral("edit.preferencesTimeline")));
    CHECK(pages == QStringList{QStringLiteral("media"), QStringLiteral("timeline")});
    CHECK_FALSE(editing.runCommand(QStringLiteral("edit.undo")));
    resetSettings();
}

TEST_CASE("Editing: Duplicate, New Sequence From Clip, Make Subsequence and Join Through Edits", "[app][editing]") {
    EditingRig r(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    const std::size_t sequences = r.controller.snapshot()->sequences.size();
    std::size_t before = r.undo();
    REQUIRE(r.editing.duplicate({sequenceKey(r.tl.seq), mediaKey(r.tl.media('A'))}).isEmpty());
    CHECK(r.undo() == before + 1); // one step for both
    CHECK(r.controller.snapshot()->sequences.size() == sequences + 1);
    CHECK(r.controller.snapshot()->sequences.back()->name == r.sequence().name + " Copy");
    CHECK(r.controller.snapshot()->media.back()->name == "A Copy");
    CHECK_FALSE(r.editing.duplicate({}).isEmpty());

    REQUIRE(r.editing.newSequenceFromClip(mediaKey(r.tl.media('A'))).isEmpty());
    const Sequence& fromClip = *r.controller.snapshot()->sequences.back();
    CHECK(fromClip.name == "A");
    CHECK(fromClip.width == 1920);
    CHECK(fromClip.frameRate == kRate);
    REQUIRE(fromClip.videoTracks[0]->clips.size() == 1);

    int library = 0;
    QObject::connect(&r.editing, &EditingBridge::libraryRequested, [&] { ++library; });
    r.controller.selectClip(r.key("V1@0"));
    before = r.undo();
    REQUIRE(r.editing.makeSubsequence(false).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(library == 1);
    const Sequence& sub = *r.controller.snapshot()->sequences.back();
    CHECK(sub.name == r.sequence().name + " Sub_01");
    CHECK(sub.videoTracks[0]->clips.size() == 1);
    CHECK(sub.audioTracks[0]->clips.size() == 1);

    // A razor cut is a through edit: the clip menu shows Join Through Edits, which joins it.
    const std::array<TrackId, 2> tracks{r.tl.track("V1"), r.tl.track("A1")};
    REQUIRE(r.controller.session().apply("Add Edit", [&](const ProjectState& s) -> std::expected<ProjectState, Error> {
        auto cut = edit::addEdit(s, r.tl.seq, f(4), tracks);
        if (!cut)
            return std::unexpected(cut.error());
        return std::move(cut->state);
    }));
    REQUIRE(r.sequence().videoTracks[0]->clips.size() == 2);
    CHECK(r.editing.clipVideoInfo(r.key("V1@0")).value(QStringLiteral("throughEdit")).toBool());
    REQUIRE(r.editing.joinThroughEdits(r.key("V1@0")).isEmpty());
    CHECK(r.sequence().videoTracks[0]->clips.size() == 1);
    CHECK(r.sequence().audioTracks[0]->clips.size() == 1);
    CHECK_FALSE(r.editing.clipVideoInfo(r.key("V1@0")).value(QStringLiteral("throughEdit")).toBool());
}

TEST_CASE("Editing: Go to Gap moves the playhead and Close Gap closes the gaps", "[app][editing][gaps]") {
    EditingRig r(R"(
        V1 tgt |Aaa...Bbbb..Cc|
    )");
    r.controller.setPlayhead(f(0));
    REQUIRE(r.editing.goToGap(true, true));
    CHECK(r.controller.playhead() == f(3));
    REQUIRE(r.editing.runCommand(QStringLiteral("sequence.nextInSequence")));
    CHECK(r.controller.playhead() == f(10));
    REQUIRE(r.editing.runCommand(QStringLiteral("sequence.previousInTrack")));
    CHECK(r.controller.playhead() == f(3));
    r.controller.setPlayhead(f(12));
    CHECK_FALSE(r.editing.goToGap(true, true)); // none after the last gap
    CHECK(r.controller.playhead() == f(12));

    const std::size_t before = r.undo();
    REQUIRE(r.editing.closeGap().isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.now("V1@0").start() == f(0));
    CHECK(r.now("V1@6").start() == f(3));
    CHECK(r.now("V1@12").start() == f(7));

    // Sequence Settings > Display Format: the app's times show as frames or feet + frames.
    REQUIRE(r.editing.applySequenceSettings(sequenceKey(r.tl.seq), {{QStringLiteral("videoDisplay"), QStringLiteral("frames")}})
                .isEmpty());
    CHECK(r.controller.formatTimecode(f(30)) == QStringLiteral("30"));
    REQUIRE(r.editing.applySequenceSettings(sequenceKey(r.tl.seq), {{QStringLiteral("videoDisplay"), QStringLiteral("feet35mm")}})
                .isEmpty());
    CHECK(r.controller.formatTimecode(f(35)) == QStringLiteral("0002+03"));
}

TEST_CASE("Editing: Merge Clips, Interpret Footage, Timecode and Make Offline on Library items", "[app][editing][footage]") {
    EditingRig r(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
        A2     |Bbbbbbbbbb|
    )");
    // Merge: the first item with video is the camera, the other the recording.
    int library = 0;
    QObject::connect(&r.editing, &EditingBridge::libraryRequested, [&] { ++library; });
    std::size_t before = r.undo();
    REQUIRE(r.editing.mergeClips({mediaKey(r.tl.media('A')), mediaKey(r.tl.media('B'))}, QStringLiteral("inPoints"), true, {})
                .isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(library == 1);
    const Sequence& merged = *r.controller.snapshot()->sequences.back();
    CHECK(merged.name == "A - Merged");
    REQUIRE(merged.multicam);
    CHECK(merged.multicam->merged);
    CHECK(merged.multicam->angles.size() == 2);
    CHECK(merged.videoTracks.size() == 1); // the recording's picture is left out
    CHECK_FALSE(r.editing.mergeClips({mediaKey(r.tl.media('A'))}, QStringLiteral("inPoints"), false, {}).isEmpty());

    // Interpret Footage: 25 fps played as 50 fps runs the clips at 200 % (a conform).
    const QStringList a{mediaKey(r.tl.media('A'))};
    REQUIRE(r.editing.applyInterpretation(a, {{QStringLiteral("frameRate"), 50.0}}).isEmpty());
    REQUIRE(r.media('A').interpretation.has_value());
    CHECK(r.media('A').interpretation->frameRate == Rate{50, 1});
    CHECK(r.now("V1@0").speed == Speed{2, 1});
    QVariantMap interpreted = r.editing.interpretation(a);
    CHECK(interpreted.value(QStringLiteral("frameRate")).toDouble() == Catch::Approx(50.0));
    CHECK(interpreted.value(QStringLiteral("fileRate")).toDouble() == Catch::Approx(25.0));
    // Back to the file's rate.
    REQUIRE(r.editing.applyInterpretation(a, {{QStringLiteral("frameRate"), 0.0}}).isEmpty());
    CHECK(r.now("V1@0").speed == Speed{1, 1});
    CHECK(r.editing.interpretation(a).value(QStringLiteral("frameRate")).toDouble() == 0.0);

    // Modify > Timecode.
    REQUIRE(r.editing.applyMediaTimecode(a, QStringLiteral("01:00:00:00"), QStringLiteral("Reel 7")).isEmpty());
    REQUIRE(r.media('A').timecode.has_value());
    CHECK(r.media('A').timecode->start == Ticks::fromSeconds(3600));
    CHECK(r.media('A').timecode->reelName == "Reel 7");
    const QVariantMap tc = r.editing.mediaTimecode(a);
    CHECK(tc.value(QStringLiteral("timecode")).toString() == QStringLiteral("01:00:00:00"));
    CHECK(tc.value(QStringLiteral("reelName")).toString() == QStringLiteral("Reel 7"));

    // Make Offline.
    REQUIRE(r.editing.makeOffline({mediaKey(r.tl.media('B'))}).isEmpty());
    CHECK(r.media('B').madeOffline);
    CHECK_FALSE(r.media('A').madeOffline);
}

TEST_CASE("Editing: the keymap commands it runs are the ones it says", "[app][editing]") {
    for (const char* id :
         {"clip.scaleToFrameSize", "clip.setToFrameSize", "clip.setToFillFrame", "clip.frameHoldOptions", "clip.addFrameHold",
          "clip.insertFrameHoldSegment", "edit.preferencesMedia", "edit.preferencesTimeline", "edit.duplicate",
          "edit.editOriginal", "file.newSequenceFromClip", "sequence.settings", "sequence.replaceFromSourceMonitor",
          "sequence.replaceFromSourceMonitorMatchFrame", "sequence.replaceFromBin", "sequence.makeSubsequence",
          "sequence.makeSubsequenceFromInToOut", "sequence.joinThroughEdits", "sequence.nextInSequence",
          "sequence.previousInSequence", "sequence.nextInTrack", "sequence.previousInTrack", "sequence.closeGap",
          "clip.mergeClips", "clip.interpretFootage", "clip.modifyTimecode", "clip.replaceFootage", "clip.makeOffline",
          "window.history"}) {
        INFO(id);
        CHECK(EditingBridge::handlesCommand(QString::fromLatin1(id)));
    }
    CHECK_FALSE(EditingBridge::handlesCommand(QStringLiteral("edit.undo")));
    CHECK_FALSE(EditingBridge::handlesCommand(QStringLiteral("clip.speedDuration")));
}

TEST_CASE("Editing: the menus, dialogs and History panel in the shell", "[app][editing][qml]") {
    resetSettings();
    clearMessages();
    Shell shell;
    REQUIRE(shell.window);
    INFO(join(shell.errors));
    REQUIRE(shell.errors.isEmpty());
    shell.resize(kShotWidth, kShotHeight);
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* editing = shell.engine.singletonInstance<EditingBridge*>("Wabcut", "Editing");
    REQUIRE(controller);
    REQUIRE(editing);
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    spin(100);

    // The File menu's entries and the clip menu's submenus.
    for (const char* name : {"newSequenceFromClipMenuItem", "sequenceSettingsMenuItem", "historyMenuItem",
                             "editingPreferencesMenuItem", "videoOptionsSubmenu", "replaceWithClipSubmenu",
                             "makeSubsequenceMenuItem", "joinThroughEditsMenuItem", "scaleToFrameSizeMenuItem",
                             "frameHoldOptionsMenuItem", "replaceMatchFrameMenuItem"}) {
        INFO(name);
        CHECK(shell.window->findChild<QObject*>(QString::fromLatin1(name)));
    }
    QObject* host = shell.window->findChild<QObject*>(QStringLiteral("editingHost"));
    REQUIRE(host);

    // Window > History: the panel lists the steps; a click on the first goes back to it.
    controller->selectClip(clipKey(tl.clip("V1@0")));
    REQUIRE(editing->scaleToFrameSize({}, true).isEmpty());
    REQUIRE(editing->runCommand(QStringLiteral("window.history")));
    QObject* panel = nullptr;
    REQUIRE(waitFor([&] {
        panel = findNamed(host->findChild<QObject*>(QStringLiteral("historyPanelLoader")), QStringLiteral("historyPanel"));
        if (!panel)
            panel = shell.window->findChild<QObject*>(QStringLiteral("historyPanel"));
        return panel && panel->property("opened").toBool();
    }, 5000));
    QObject* list = panel->findChild<QObject*>(QStringLiteral("historySteps"));
    REQUIRE(list);
    CHECK(list->property("count").toInt() == 2);
    REQUIRE(editing->goToHistoryStep(0));
    CHECK_FALSE(controller->currentSequence()->findClip(tl.clip("V1@0"))->clip().content().scaleToFrame);
    REQUIRE(editing->runCommand(QStringLiteral("window.history"))); // again: closes it
    CHECK(waitFor([&] { return !panel->property("opened").toBool(); }, 5000));

    // Sequence > Sequence Settings: a preset, then OK.
    REQUIRE(editing->runCommand(QStringLiteral("sequence.settings")));
    QObject* settings = nullptr;
    const bool settingsOpened = waitFor([&] {
        settings = shell.window->findChild<QObject*>(QStringLiteral("sequenceSettingsDialog"));
        return settings && settings->property("opened").toBool();
    }, 5000);
    INFO(editing->message().toStdString());
    INFO(join(takeMessages()));
    REQUIRE(settingsOpened);
    CHECK(settings->property("frameWidth").toInt() == 1920);
    REQUIRE(QMetaObject::invokeMethod(settings, "choosePreset", Q_ARG(QVariant, QStringLiteral("square1080p30"))));
    REQUIRE(QMetaObject::invokeMethod(settings, "apply"));
    CHECK(settings->property("errorText").toString().isEmpty());
    CHECK(controller->currentSequence()->width == 1080);
    CHECK(controller->currentSequence()->height == 1080);
    CHECK(controller->currentSequence()->frameRate == Rate{30, 1});
    CHECK(waitFor([&] { return !settings->property("opened").toBool(); }, 5000));

    // Preferences and Frame Hold Options open from their commands.
    REQUIRE(editing->runCommand(QStringLiteral("edit.preferencesTimeline")));
    QObject* prefs = nullptr;
    REQUIRE(waitFor([&] {
        prefs = shell.window->findChild<QObject*>(QStringLiteral("editingPreferencesDialog"));
        return prefs && prefs->property("opened").toBool();
    }, 5000));
    QMetaObject::invokeMethod(prefs, "close");
    controller->selectClip(clipKey(tl.clip("V1@0")));
    REQUIRE(editing->runCommand(QStringLiteral("clip.frameHoldOptions")));
    const auto opened = [&](const char* name) {
        QObject* dialog = nullptr;
        const bool open = waitFor([&] {
            dialog = shell.window->findChild<QObject*>(QString::fromLatin1(name));
            return dialog && dialog->property("opened").toBool();
        }, 5000);
        INFO(name << ": " << join(takeMessages()));
        REQUIRE(open);
        return dialog;
    };
    // Frame Hold Options: OK holds the In point frame with its effects.
    QObject* hold = opened("frameHoldOptionsDialog");
    hold->setProperty("holdFilters", true);
    REQUIRE(QMetaObject::invokeMethod(hold, "apply"));
    CHECK(hold->property("errorText").toString().isEmpty());
    const auto video = [&] { return controller->currentSequence()->findClip(tl.clip("V1@0"))->clip(); };
    CHECK(video().isFreeze());
    CHECK(video().content().holdFilters);

    // Modify > Timecode and Interpret Footage on the Library item, then Merge Clips.
    const QString a = mediaKey(tl.media('A'));
    editing->openTimecode({a});
    QObject* timecode = opened("timecodeDialog");
    timecode->setProperty("useFile", false);
    QObject* start = timecode->findChild<QObject*>(QStringLiteral("timecodeStart"));
    QObject* reel = timecode->findChild<QObject*>(QStringLiteral("timecodeReel"));
    REQUIRE(start);
    REQUIRE(reel);
    start->setProperty("text", QStringLiteral("10:00:00:00"));
    reel->setProperty("text", QStringLiteral("B002"));
    REQUIRE(QMetaObject::invokeMethod(timecode, "apply"));
    CHECK(timecode->property("errorText").toString().isEmpty());
    const auto item = [&] { return *controller->snapshot()->findMedia(tl.media('A')); };
    REQUIRE(item().timecode.has_value());
    CHECK(item().timecode->start == Ticks::fromSeconds(36000));
    CHECK(item().timecode->reelName == "B002");

    editing->openInterpretFootage({a});
    QObject* interpret = opened("interpretFootageDialog");
    CHECK(interpret->property("rate").toDouble() == Catch::Approx(25.0)); // the file's rate to start from
    interpret->setProperty("assumeRate", true);
    interpret->setProperty("rate", 50.0);
    interpret->setProperty("ignoreAlpha", true);
    REQUIRE(QMetaObject::invokeMethod(interpret, "apply"));
    CHECK(interpret->property("errorText").toString().isEmpty());
    REQUIRE(item().interpretation.has_value());
    CHECK(item().interpretation->frameRate == Rate{50, 1});
    CHECK(item().interpretation->ignoreAlpha);

    editing->openMergeClips({a});
    CHECK_FALSE(editing->message().isEmpty()); // a merge needs a camera clip and sound
    Timeline two(R"(
        V1 tgt |Aaaaaaaaaa|
        A1     |Bbbbbbbbbb|
    )");
    controller->resetProject(two.state, two.seq);
    editing->openMergeClips({mediaKey(two.media('A')), mediaKey(two.media('B'))});
    QObject* merge = opened("mergeClipsDialog");
    merge->setProperty("removeCameraAudio", true);
    REQUIRE(QMetaObject::invokeMethod(merge, "apply"));
    CHECK(merge->property("errorText").toString().isEmpty());
    CHECK(controller->snapshot()->sequences.size() == two.state.sequences.size() + 1);
    REQUIRE(controller->snapshot()->sequences.back()->multicam);
    CHECK(controller->snapshot()->sequences.back()->multicam->merged);

    // Modify > Source Settings on a camera RAW item: its command opens the dialog; OK develops the item so.
    Timeline raw(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    makeRaw(raw, 'A', "D:/Shoot/A001.mov");
    controller->resetProject(raw.state, raw.seq);
    editing->openRawSourceSettings({mediaKey(raw.media('A'))});
    QObject* source = opened("rawSourceSettingsDialog");
    CHECK(source->property("custom").toBool() == false);
    CHECK(source->findChild<QObject*>(QStringLiteral("rawExposure")));
    source->setProperty("exposure", -1.25);
    source->setProperty("custom", true);
    source->setProperty("temperature", 4300.0);
    REQUIRE(QMetaObject::invokeMethod(source, "apply"));
    CHECK(source->property("errorText").toString().isEmpty());
    CHECK(controller->snapshot()->findMedia(raw.media('A'))->locator.path ==
          "D:/Shoot/A001.mov?raw=exposure:-1.25,white:4300:0");

    const QStringList messages = takeMessages();
    INFO(join(messages));
    for (const QString& m : messages)
        CHECK_FALSE(m.contains(QStringLiteral("qml"), Qt::CaseInsensitive));
    resetSettings();
}

TEST_CASE("Editing: RAW Source Settings on camera RAW Library items, one undo step", "[app][editing][raw]") {
    Timeline tl(R"(
        V1 tgt |Aaaaaaaaaa|
        V2     |Bbbbbbbbbb|
    )");
    makeRaw(tl, 'A', "D:/Shoot/A001.mov");
    ProjectController controller;
    controller.resetProject(tl.state, tl.seq);
    EditingBridge editing;
    editing.setController(&controller);
    const QString a = mediaKey(tl.media('A'));
    const QString b = mediaKey(tl.media('B'));
    CHECK(editing.isRawMedia(a));
    CHECK_FALSE(editing.isRawMedia(b));
    QVariantMap shown = editing.rawSourceSettings({a, b});
    CHECK(shown.value(QStringLiteral("count")).toInt() == 1); // the RAW items among them
    CHECK(shown.value(QStringLiteral("whiteBalance")).toString() == QStringLiteral("asShot"));
    // The dialog opens for RAW items only.
    int requested = 0;
    QObject::connect(&editing, &EditingBridge::rawSourceSettingsRequested, [&] { ++requested; });
    editing.openRawSourceSettings({b});
    CHECK(requested == 0);
    CHECK_FALSE(editing.message().isEmpty());
    editing.openRawSourceSettings({a});
    CHECK(requested == 1);

    const std::size_t before = controller.session().undoCount();
    REQUIRE(editing.applyRawSourceSettings({a, b}, {{QStringLiteral("exposure"), 0.5}, {QStringLiteral("whiteBalance"), QStringLiteral("custom")},
                                                    {QStringLiteral("temperature"), 3200}, {QStringLiteral("tint"), -10}})
                .isEmpty());
    CHECK(controller.session().undoCount() == before + 1);
    const auto path = [&] { return controller.snapshot()->findMedia(tl.media('A'))->locator.path; };
    CHECK(path() == "D:/Shoot/A001.mov?raw=exposure:+0.50,white:3200:-10");
    shown = editing.rawSourceSettings({a});
    CHECK(shown.value(QStringLiteral("exposure")).toDouble() == Catch::Approx(0.5));
    CHECK(shown.value(QStringLiteral("temperature")).toInt() == 3200);
    CHECK(shown.value(QStringLiteral("tint")).toInt() == -10);
    controller.session().undo();
    CHECK(path() == "D:/Shoot/A001.mov");
    CHECK_FALSE(editing.applyRawSourceSettings({b}, {{QStringLiteral("exposure"), 1.0}}).isEmpty());
    CHECK(controller.session().undoCount() == before);
}

TEST_CASE("Editing: a drop with the replace modifier is a Replace Edit; Edit Original needs the file", "[app][editing][replace]") {
    EditingRig r(R"(
        V1 tgt |A10aaaaaaa|
        A1 tgt |A10aaaaaaa|
        A2     |..........Bbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
    )");
    // Alt- or Shift-drag of B from the Library onto the clip: B takes its place from B's start, linked audio too.
    const std::size_t before = r.undo();
    REQUIRE(r.editing.replaceDrop(r.key("V1@0"), mediaKey(r.tl.media('B')), false));
    CHECK(r.undo() == before + 1);
    CHECK(r.now("V1@0").media == r.tl.media('B'));
    CHECK(r.now("V1@0").start() == f(0));
    CHECK(r.now("V1@0").duration() == f(10));
    CHECK(r.now("A1@0").media == r.tl.media('B'));
    // Not onto a clip, or nothing to replace it with: an ordinary drop.
    CHECK_FALSE(r.editing.replaceDrop(QString(), mediaKey(r.tl.media('B')), false));
    CHECK_FALSE(r.editing.replaceDrop(r.key("V1@0"), QString(), true)); // the Source Monitor has nothing loaded
    // Edit Original: the fixture's files are not on this computer, so nothing opens and the message says why.
    CHECK_FALSE(r.editing.editOriginal(mediaKey(r.tl.media('A'))));
    CHECK(r.editing.message().contains(QStringLiteral("offline")));
    CHECK_FALSE(r.editing.editOriginal(QStringLiteral("s1")));
}
// UX rework 2026-10-23, stream T (timeline and monitor QoL), the C++ side: typed playhead entry (absolute times in the
// display format, +/- offsets, the Source Monitor's own), the gap selection (a click in a gap, Delete closes it, the
// right-click menu signal), the track header menu (Rename, Add Tracks with a placement, Delete Track, Delete Empty
// Tracks), the default transitions and the transition menu's facts, Reveal in Project / File Explorer, a Library drop
// that honours the item's In and Out (and the card's marks), the Source Monitor loading a dropped file's item, and the
// marker facts the ruler and Program time bar tooltips show. The QML on top comes in phase 2.

#include "ShellFixture.h"
#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "EditingBridge.h"
#include "LibraryBridge.h"
#include "LibraryModel.h"
#include "ProjectController.h"
#include "ProjectLifecycle.h"
#include "SequenceTimeBar.h"
#include "SourceMonitorBridge.h"
#include "TimelineFixture.h"
#include "TimelineView.h"

#include <wabcut/model/AudioChannels.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/Effects.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/SequenceSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// A project from a timeline picture under a ProjectController (no window).
struct Project {
    Timeline tl;
    ProjectController controller;

    explicit Project(std::string_view picture, const std::map<char, std::int64_t>& media = {}) : tl(picture, media) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
    }
    QString track(std::string_view label) const { return trackKey(tl.track(label)); }
    const Sequence& sequence() const { return *controller.currentSequence(); }
    std::size_t undoSteps() { return controller.session().undoCount(); }
    /// Applies a model edit as one undo step (setup).
    template <class Fn>
    void edit(const char* label, Fn&& fn) {
        REQUIRE(controller.applyEditFn(label, std::forward<Fn>(fn)));
    }
};

/// Points `letter`'s media item at `file` (a relink), as one undo step.
void relink(ProjectController& controller, MediaId media, const QString& file) {
    ProjectState relinked = *controller.snapshot();
    for (MediaPtr& m : relinked.media) {
        if (m->id == media) {
            MediaItem item = *m;
            item.locator.path = file.toStdString();
            item.locator.offline = false;
            m = std::make_shared<const MediaItem>(std::move(item));
        }
    }
    REQUIRE(controller.session().commit("Relink", relinked));
}

/// A file with a few bytes in `dir`.
QString touch(const QTemporaryDir& dir, const QString& name) {
    const QString path = dir.filePath(name);
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly));
    f.write("wabcut");
    return path;
}

QString idOf(const QVariant& entry) { return entry.toMap().value(QStringLiteral("id")).toString(); }

} // namespace

// ---- Typed playhead entry ------------------------------------------------------------------------------------------

TEST_CASE("Typed playhead entry seeks to absolute timecodes and to offsets from the playhead", "[app][timeline][timecodeEntry]") {
    Project p(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    ProjectController& c = p.controller;

    CHECK(c.seekToText(QStringLiteral("00:00:01:10")));
    CHECK(c.playhead() == f(35));
    CHECK(c.seekToText(QStringLiteral("0;0;2;0"))); // any separator
    CHECK(c.playhead() == f(50));
    CHECK(c.seekToText(QStringLiteral("110"))); // packed from the right: 00:00:01:10
    CHECK(c.playhead() == f(35));
    CHECK(c.seekToText(QStringLiteral(" 2. "))); // Premiere's shorthand: an empty field is 00
    CHECK(c.playhead() == f(50));

    // Offsets from the playhead, in frames or timecode.
    CHECK(c.seekToText(QStringLiteral("+15")));
    CHECK(c.playhead() == f(65));
    CHECK(c.seekToText(QStringLiteral("-1:00")));
    CHECK(c.playhead() == f(40));
    CHECK(c.seekToText(QStringLiteral("- 10")));
    CHECK(c.playhead() == f(30));
    CHECK(c.seekToText(QStringLiteral("-99:00"))); // clamped at the start
    CHECK(c.playhead() == Ticks{});
    CHECK(c.seekToText(QStringLiteral("1..")));    // a minute: past the end, clamped to the scrollable extent
    CHECK(c.playhead() == c.contentDuration());

    // What Enter would do, without moving.
    c.setPlayhead(f(25));
    CHECK(c.timeEntryText(QStringLiteral("+5")) == QStringLiteral("00:00:01:05"));
    CHECK(c.timeEntryText(QStringLiteral("nonsense")).isEmpty());
    CHECK(c.playhead() == f(25));

    // Text that is not a time moves nothing.
    for (const char* bad : {"", "+", "abc", "+-3", "1:2:3:4:5", "00:00:01:30", "--5"}) {
        INFO(bad);
        CHECK_FALSE(c.seekToText(QString::fromLatin1(bad)));
        CHECK(c.playhead() == f(25));
    }
}

TEST_CASE("Typed playhead entry follows the Frames and Feet display formats", "[app][timeline][timecodeEntry]") {
    Project p(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    ProjectController& c = p.controller;
    edit::SequenceSettingsChange frames;
    frames.videoDisplay = VideoTimeDisplay::Frames;
    p.edit("Display Format", [&](const ProjectState& s) { return edit::setSequenceSettings(s, p.tl.seq, frames); });
    CHECK(c.seekToText(QStringLiteral("110"))); // a frame count now, not 00:00:01:10
    CHECK(c.playhead() == f(110));
    CHECK(c.playheadText() == QStringLiteral("110"));
    CHECK(c.seekToText(QStringLiteral("+5")));
    CHECK(c.playhead() == f(115));
    CHECK(c.seekToText(QStringLiteral("00:00:01:00"))); // a timecode still reads
    CHECK(c.playhead() == f(25));

    edit::SequenceSettingsChange feet;
    feet.videoDisplay = VideoTimeDisplay::Feet35mm; // 16 frames a foot
    p.edit("Display Format", [&](const ProjectState& s) { return edit::setSequenceSettings(s, p.tl.seq, feet); });
    CHECK(c.seekToText(QStringLiteral("0002+04")));
    CHECK(c.playhead() == f(36));
    CHECK(c.seekToText(QStringLiteral("40")));
    CHECK(c.playhead() == f(40));
}

TEST_CASE("Typed playhead entry in drop-frame goes to the next label for a minute the timecode skips", "[app][timeline][timecodeEntry]") {
    // Two minutes of a clip, then 29.97 drop-frame: 00:01:00;00 and ;01 do not exist (Premiere goes on to ;02).
    const std::string picture = "V1 tgt |A" + std::string(3200, 'a') + "|";
    Project p(picture, {{'A', 3300}});
    ProjectController& c = p.controller;
    edit::SequenceSettingsChange ntsc;
    ntsc.frameRate = Rate{30000, 1001};
    ntsc.dropFrame = true;
    p.edit("Sequence Settings", [&](const ProjectState& s) { return edit::setSequenceSettings(s, p.tl.seq, ntsc); });
    REQUIRE(p.sequence().dropFrame);
    REQUIRE(c.contentDuration() > Ticks::fromSeconds(70));
    const Rate rate{30000, 1001};

    for (const char* minute : {"1..", "00:01:00:00", "00:01:00;01", "10000"}) {
        INFO(minute);
        c.setPlayhead(Ticks{});
        CHECK(c.seekToText(QString::fromLatin1(minute)));
        CHECK(c.playhead() == framesToTicks(1800, rate));
        CHECK(c.playheadText() == QStringLiteral("00:01:00;02"));
    }
    CHECK(c.timeEntryText(QStringLiteral("1..")) == QStringLiteral("00:01:00;02"));
    // Labels that exist read as they are.
    CHECK(c.seekToText(QStringLiteral("00:01:00;02")));
    CHECK(c.playhead() == framesToTicks(1800, rate));
    CHECK(c.seekToText(QStringLiteral("00:00:59;29")));
    CHECK(c.playhead() == framesToTicks(1799, rate));
    CHECK(c.seekToText(QStringLiteral("00:01:00;10")));
    CHECK(c.playhead() == framesToTicks(1808, rate));
    // Offsets count frames, not labels.
    CHECK(c.seekToText(QStringLiteral("+1..")));
    CHECK(c.playhead() == framesToTicks(1808 + 1800, rate));
    // Still not a time: out-of-range fields.
    CHECK_FALSE(c.seekToText(QStringLiteral("00:01:00;30")));
}

TEST_CASE("Typed lengths read back in the sequence display format", "[app][timeline][timecodeEntry][transition]") {
    // Set Duration..., Clip Speed / Duration, caption and marker times are prefilled with formatTimecode (the Display
    // Format) and read with parseTimecodeText: an untouched field keeps its length in every format.
    Project p(R"(
        V1 tgt |Aaaaaaaaaa|
    )");
    ProjectController& c = p.controller;
    CHECK(c.normalizeTimecode(QStringLiteral("120")) == QStringLiteral("00:00:01:20")); // timecode: packed digits
    CHECK(c.normalizeTimecode(QStringLiteral("2.")) == QStringLiteral("00:00:02:00"));   // the shorthand

    edit::SequenceSettingsChange frames;
    frames.videoDisplay = VideoTimeDisplay::Frames;
    p.edit("Display Format", [&](const ProjectState& s) { return edit::setSequenceSettings(s, p.tl.seq, frames); });
    CHECK(c.formatTimecode(f(120)) == QStringLiteral("120"));
    CHECK(c.parseTimecodeText(QStringLiteral("120")) == f(120)); // a frame count, not 00:00:01:20
    CHECK(c.normalizeTimecode(QStringLiteral("120")) == QStringLiteral("120"));
    CHECK(c.parseTimecodeText(QStringLiteral("00:00:01:00")) == f(25)); // a timecode still reads

    edit::SequenceSettingsChange feet;
    feet.videoDisplay = VideoTimeDisplay::Feet35mm; // 16 frames a foot
    p.edit("Display Format", [&](const ProjectState& s) { return edit::setSequenceSettings(s, p.tl.seq, feet); });
    const QString shown = c.formatTimecode(f(36));
    CHECK(shown == QStringLiteral("0002+04"));
    CHECK(c.parseTimecodeText(shown) == f(36));
    CHECK(c.normalizeTimecode(shown) == shown);
    CHECK(c.parseTimecodeText(QStringLiteral("-0002+04")) == Ticks{-f(36).value});
    CHECK(c.normalizeTimecode(QStringLiteral("-0002+04")).isEmpty()); // not a length
    CHECK_FALSE(c.parseTimecodeText(QStringLiteral("nonsense")));
}

TEST_CASE("Typed playhead entry reads audio samples and milliseconds with audio time units", "[app][timeline][timecodeEntry]") {
    Project p(R"(
        V1 tgt |Aaaaaaaaaa|
        A1 tgt |Aaaaaaaaaa|
    )");
    ProjectController& c = p.controller;
    const Rate audio = p.sequence().audioRate;
    REQUIRE(audio == kRate48kHz);
    c.setAudioTimeUnits(true);

    CHECK(c.seekToText(QStringLiteral("00:00:01:24000")));
    CHECK(c.finePlayhead() == Ticks::fromSeconds(1) + framesToTicks(24000, audio));
    CHECK(c.seekToText(QStringLiteral("+480"))); // samples
    CHECK(c.finePlayhead() == Ticks::fromSeconds(1) + framesToTicks(24480, audio));
    CHECK(c.seekToText(QStringLiteral("1:10"))); // a timecode still reads (frames field)
    CHECK(c.finePlayhead() == f(35));
    CHECK_FALSE(c.seekToText(QStringLiteral("00:00:01:48000"))); // past the second's samples

    edit::SequenceSettingsChange ms;
    ms.audioDisplay = AudioTimeDisplay::Milliseconds;
    p.edit("Audio Display Format", [&](const ProjectState& s) { return edit::setSequenceSettings(s, p.tl.seq, ms); });
    CHECK(c.seekToText(QStringLiteral("00:00:02.5")));
    CHECK(c.finePlayhead() == Ticks::fromSeconds(2) + Ticks{kTicksPerSecond / 2});
}

TEST_CASE("The Source Monitor timecode takes typed times and offsets", "[app][source][timecodeEntry]") {
    Project p(R"(
        V1 tgt |Aaaa|
    )", {{'S', 50}});
    SourceMonitorBridge source;
    source.setController(&p.controller);
    CHECK_FALSE(source.seekToText(QStringLiteral("10"))); // nothing loaded

    REQUIRE(source.load(mediaKey(p.tl.media('S'))));
    CHECK(source.seekToText(QStringLiteral("00:00:01:00")));
    CHECK(source.positionTicks() == f(25));
    CHECK(source.seekToText(QStringLiteral("+5")));
    CHECK(source.positionTicks() == f(30));
    CHECK(source.seekToText(QStringLiteral("-1:00")));
    CHECK(source.positionTicks() == f(5));
    CHECK(source.seekToText(QStringLiteral("1.")));
    CHECK(source.positionTicks() == f(25));
    CHECK(source.seekToText(QStringLiteral("99:00"))); // clamped to the last frame
    CHECK(source.positionTicks() == f(49));
    CHECK(source.seekToText(QStringLiteral("-99:00")));
    CHECK(source.positionTicks() == Ticks{});
    CHECK_FALSE(source.seekToText(QStringLiteral("x1")));
    CHECK_FALSE(source.seekToText(QStringLiteral("+")));
    CHECK(source.positionTicks() == Ticks{});
}

// ---- Gap selection --------------------------------------------------------------------------------------------------

TEST_CASE("A click in a gap selects it and Delete closes it in one undo step", "[app][timeline][gap]") {
    Rig rig(R"(
        V1 tgt |Aaaa....Bbbb....|
        A1 tgt |Aaaa....Bbbb....|
    )");
    ProjectController& c = rig.controller;
    const QPointF gap(rig.x(6), rig.y("V1")); // the middle of the gap, away from the clips' trim edges
    rig.press(gap);
    rig.release(gap);
    REQUIRE(c.gapSelection().has_value());
    CHECK(c.gapSelection()->track == rig.tl.track("V1"));
    CHECK(c.gapSelection()->range == TimeRange::fromStartEnd(f(4), f(8)));
    const QVariantMap selected = c.selectedGap();
    CHECK(selected.value(QStringLiteral("track")).toString() == trackKey(rig.tl.track("V1")));
    CHECK(selected.value(QStringLiteral("trackName")).toString() == QStringLiteral("V1"));
    CHECK(selected.value(QStringLiteral("start")).toString() == QStringLiteral("00:00:00:04"));
    CHECK(selected.value(QStringLiteral("end")).toString() == QStringLiteral("00:00:00:08"));
    CHECK(selected.value(QStringLiteral("duration")).toString() == QStringLiteral("00:00:00:04"));
    CHECK(c.selectedClips().isEmpty());
    CHECK(c.commandAvailability(QStringLiteral("edit.clear")).value(QStringLiteral("enabled")).toBool());
    CHECK(c.commandAvailability(QStringLiteral("edit.rippleDelete")).value(QStringLiteral("enabled")).toBool());

    // A click on a clip selects it instead; a click after the last clip selects nothing.
    rig.press({rig.x(1.5), rig.y("V1")});
    rig.release({rig.x(1.5), rig.y("V1")});
    CHECK_FALSE(c.gapSelection().has_value());
    CHECK_FALSE(c.selectedClips().isEmpty());
    rig.press({rig.x(14), rig.y("V1")});
    rig.release({rig.x(14), rig.y("V1")});
    CHECK_FALSE(c.gapSelection().has_value());
    CHECK(c.selectedClips().isEmpty());

    // Delete (Premiere: the selected gap closes, sync-locked tracks lose the same time).
    rig.press(gap);
    rig.release(gap);
    REQUIRE(c.gapSelection().has_value());
    const std::size_t steps = rig.undoSteps();
    CHECK(c.runCommand(QStringLiteral("edit.clear")));
    CHECK(rig.picture() == normalize(R"(
        V1 |AaaaBbbb........|
        A1 |AaaaBbbb........|
    )"));
    CHECK(rig.undoSteps() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Ripple Delete"));
    CHECK_FALSE(c.gapSelection().has_value()); // any edit deselects it
    REQUIRE(c.undo());
    CHECK(rig.picture() == normalize(rig.original));

    // Ripple Delete closes it too; Deselect All and a transition selection deselect it.
    REQUIRE(c.selectGapAt(rig.tl.track("A1"), f(5)));
    c.clearSelection();
    CHECK_FALSE(c.gapSelection().has_value());
    REQUIRE(c.selectGapAt(rig.tl.track("A1"), f(5)));
    c.selectTransition(QString()); // no transition: the gap stays
    CHECK(c.gapSelection().has_value());
    CHECK(c.runCommand(QStringLiteral("edit.rippleDelete")));
    CHECK(rig.picture() == normalize(R"(
        V1 |AaaaBbbb........|
        A1 |AaaaBbbb........|
    )"));
}

TEST_CASE("A gap another sync-locked track fills is not closed and stays selected", "[app][timeline][gap]") {
    Rig rig(R"(
        V1 tgt |Aaaa....Bbbb....|
        A1 tgt |Cccccccccccc....|
    )", 10.0, false);
    ProjectController& c = rig.controller;
    REQUIRE(c.selectGapAt(rig.tl.track("V1"), f(5)));
    const std::size_t steps = rig.undoSteps();
    CHECK_FALSE(c.rippleDeleteGap());
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK(rig.undoSteps() == steps);
    CHECK(rig.picture() == normalize(rig.original));
    CHECK(c.gapSelection().has_value());
    // No gap there: nothing is selected.
    CHECK_FALSE(c.selectGapAt(rig.tl.track("V1"), f(1)));  // inside a clip
    CHECK_FALSE(c.selectGapAt(rig.tl.track("V1"), f(13))); // after the last clip
    CHECK(c.gapSelection().has_value());                    // the earlier one is kept
}

TEST_CASE("Right clicks in a gap and on a transition ask for their menus", "[app][timeline][gap][transition]") {
    resetSettings(); // the default transition
    const auto rightClick = [](Rig& rig, QPointF at) {
        rig.press(at, {}, Qt::RightButton);
        rig.send(QEvent::MouseButtonRelease, at, Qt::RightButton, Qt::NoButton, {});
        rig.harness.frame();
    };
    {
        Rig rig(R"(
            V1 tgt |Aaaa....Bbbb....|
            A1 tgt |Aaaa....Bbbb....|
        )");
        QString gapTrack;
        QObject::connect(rig.view, &TimelineView::gapContextMenuRequested, rig.view,
                         [&](const QString& track, double, double) { gapTrack = track; });
        rightClick(rig, {rig.x(6), rig.y("A1")});
        CHECK(gapTrack == trackKey(rig.tl.track("A1")));
        REQUIRE(rig.controller.gapSelection().has_value());
        CHECK(rig.controller.gapSelection()->track == rig.tl.track("A1"));
        // On a clip: the clip menu as before, and the gap is deselected.
        rightClick(rig, {rig.x(10), rig.y("V1")});
        CHECK_FALSE(rig.controller.gapSelection().has_value());
    }
    // A transition on the cut at frame 16 (Ctrl+D): a right click on it selects it and asks for its menu.
    Rig rig(R"(
        V1 tgt |AaaaaaaaaaaaaaaaB20bbbbbbbbbbbbbbbbb|
    )");
    ProjectController& c = rig.controller;
    QString transition;
    QObject::connect(rig.view, &TimelineView::transitionContextMenuRequested, rig.view,
                     [&](const QString& id, double, double) { transition = id; });
    c.setPlayhead(f(16));
    REQUIRE(c.applyDefaultTransition(false));
    rig.harness.frame();
    const Track& v1 = *c.currentSequence()->findTrack(rig.tl.track("V1"));
    REQUIRE(v1.transitions.size() == 1);
    const Transition made = v1.transitions[0];
    rightClick(rig, {rig.x(16), rig.y("V1")});
    REQUIRE(transition == transitionKey(made.id));
    CHECK(c.selectedTransition() == transition);
    const QVariantMap info = c.transitionInfo(transition);
    CHECK(info.value(QStringLiteral("id")).toString() == transition);
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
    CHECK(info.value(QStringLiteral("effect")).toString() == QStringLiteral("crossDissolve"));
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Cross Dissolve"));
    CHECK(info.value(QStringLiteral("durationText")).toString() == c.formatTimecode(made.duration()));
    CHECK(info.value(QStringLiteral("isDefault")).toBool());
    CHECK(c.transitionInfo(QStringLiteral("x999999")).isEmpty());
}

// ---- Track header menu ----------------------------------------------------------------------------------------------

TEST_CASE("Track header menu: track facts and Rename in one undo step", "[app][timeline][tracks]") {
    Project p(R"(
        V2     |....Bbbb|
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
        A2     |........|
    )");
    ProjectController& c = p.controller;
    const QString v1 = p.track("V1");
    QVariantMap info = c.trackInfo(v1);
    CHECK(info.value(QStringLiteral("kind")).toString() == QStringLiteral("video"));
    CHECK(info.value(QStringLiteral("shortName")).toString() == QStringLiteral("V1"));
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Video 1"));
    CHECK(info.value(QStringLiteral("customName")).toString().isEmpty());
    CHECK(info.value(QStringLiteral("index")).toInt() == 0);
    CHECK(info.value(QStringLiteral("clipCount")).toInt() == 1);
    CHECK(info.value(QStringLiteral("targeted")).toBool());
    CHECK(info.value(QStringLiteral("canDelete")).toBool());
    CHECK(info.value(QStringLiteral("emptyVideoTracks")).toInt() == 0);
    CHECK(info.value(QStringLiteral("emptyAudioTracks")).toInt() == 1);
    CHECK(info.value(QStringLiteral("emptyTracks")).toInt() == 1);
    CHECK(c.trackInfo(QStringLiteral("t999999")).isEmpty());

    const std::size_t steps = p.undoSteps();
    CHECK(c.renameTrack(v1, QStringLiteral("  Cam A  ")));
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->name == "Cam A");
    CHECK(c.undoText() == QStringLiteral("Undo Rename Track"));
    CHECK(p.undoSteps() == steps + 1);
    info = c.trackInfo(v1);
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("Cam A"));
    CHECK(info.value(QStringLiteral("customName")).toString() == QStringLiteral("Cam A"));
    // The header list shows it (display order V2, V1, A1, A2).
    CHECK(c.tracks()->data(c.tracks()->index(1), TrackListModel::NameRole).toString() == QStringLiteral("Cam A"));
    // Locked tracks can be renamed (like the header toggles); "" goes back to the default name.
    c.setTrackFlag(v1, QStringLiteral("locked"), true);
    CHECK(c.renameTrack(v1, QString()));
    CHECK(p.sequence().findTrack(p.tl.track("V1"))->name.empty());
    CHECK_FALSE(c.renameTrack(v1, QString(300, QLatin1Char('x'))));
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK_FALSE(c.renameTrack(QStringLiteral("t999999"), QStringLiteral("Nope")));
}

TEST_CASE("Add Tracks puts the new tracks where asked as one undo step", "[app][timeline][tracks]") {
    Project p(R"(
        V2     |....Bbbb|
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
        A2     |........|
    )");
    ProjectController& c = p.controller;
    const TrackId v1 = p.tl.track("V1");
    const TrackId v2 = p.tl.track("V2");
    const TrackId a1 = p.tl.track("A1");
    const std::size_t steps = p.undoSteps();

    // After V1: the two new tracks become V2 and V3, the old V2 is V4 with its clip.
    const QStringList after = c.addTracks({{QStringLiteral("video"), 2}, {QStringLiteral("videoPlacement"), QStringLiteral("after")},
                                           {QStringLiteral("track"), p.track("V1")}});
    REQUIRE(after.size() == 2);
    {
        const auto& video = p.sequence().videoTracks;
        REQUIRE(video.size() == 4);
        CHECK(video[0]->id == v1);
        CHECK(trackKey(video[1]->id) == after[0]);
        CHECK(trackKey(video[2]->id) == after[1]);
        CHECK(video[3]->id == v2);
        CHECK(video[3]->clips.size() == 1);
    }
    CHECK(p.undoSteps() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Add Tracks"));
    CHECK(c.tracks()->rowCount() == 6);
    REQUIRE(c.undo());
    CHECK(p.sequence().videoTracks.size() == 2);

    // Before the first audio track, mono: the new track is A1.
    const QStringList first = c.addTracks({{QStringLiteral("audio"), 1}, {QStringLiteral("audioPlacement"), QStringLiteral("first")},
                                           {QStringLiteral("audioFormat"), QStringLiteral("mono")}});
    REQUIRE(first.size() == 1);
    CHECK(trackKey(p.sequence().audioTracks[0]->id) == first[0]);
    CHECK(p.sequence().audioTracks[0]->format == ChannelFormat::Mono);
    CHECK(p.sequence().audioTracks[1]->id == a1);
    CHECK(c.undoText() == QStringLiteral("Undo Add Track"));
    REQUIRE(c.undo());

    // "before" without a track: next to the targeted track (V1); the default "last" appends.
    const QStringList before = c.addTracks({{QStringLiteral("video"), 1}, {QStringLiteral("videoPlacement"), QStringLiteral("before")}});
    REQUIRE(before.size() == 1);
    CHECK(trackKey(p.sequence().videoTracks[0]->id) == before[0]);
    REQUIRE(c.undo());
    const QStringList last = c.addTracks({{QStringLiteral("video"), 1}, {QStringLiteral("audio"), 1}});
    REQUIRE(last.size() == 2);
    CHECK(trackKey(p.sequence().videoTracks.back()->id) == last[0]);
    CHECK(trackKey(p.sequence().audioTracks.back()->id) == last[1]);
    REQUIRE(c.undo());

    // Refused, nothing added.
    const std::size_t before2 = p.undoSteps();
    CHECK(c.addTracks({{QStringLiteral("video"), 100}}).isEmpty());
    CHECK_FALSE(c.lastError().isEmpty());
    CHECK(c.addTracks({{QStringLiteral("video"), 1}, {QStringLiteral("videoPlacement"), QStringLiteral("sideways")}}).isEmpty());
    CHECK(c.addTracks({{QStringLiteral("audio"), 1}, {QStringLiteral("audioFormat"), QStringLiteral("7.1")}}).isEmpty());
    CHECK(c.addTracks({}).isEmpty());
    CHECK(p.undoSteps() == before2);
    CHECK(p.sequence().videoTracks.size() == 2);
    CHECK(p.sequence().audioTracks.size() == 2);
}

TEST_CASE("Delete Track and Delete Empty Tracks keep a track of each kind", "[app][timeline][tracks]") {
    Project p(R"(
        V2     |....Bbbb|
        V1 tgt |Aaaa....|
        A1 tgt |Aaaa....|
        A2     |........|
    )");
    ProjectController& c = p.controller;
    const QString v2 = p.track("V2");

    // Locked: refused with the reason trackInfo gave.
    c.setTrackFlag(v2, QStringLiteral("locked"), true);
    QVariantMap info = c.trackInfo(v2);
    CHECK_FALSE(info.value(QStringLiteral("canDelete")).toBool());
    CHECK_FALSE(c.deleteTrack(v2));
    CHECK(c.lastError() == info.value(QStringLiteral("deleteReason")).toString());
    c.setTrackFlag(v2, QStringLiteral("locked"), false);

    // Deleted with its clip, one undo step; then V1 is the only video track and stays.
    const std::size_t steps = p.undoSteps();
    CHECK(c.deleteTrack(v2));
    CHECK(p.sequence().videoTracks.size() == 1);
    CHECK(p.undoSteps() == steps + 1);
    CHECK(c.undoText() == QStringLiteral("Undo Delete Track"));
    info = c.trackInfo(p.track("V1"));
    CHECK_FALSE(info.value(QStringLiteral("canDelete")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("deleteReason")).toString().isEmpty());
    CHECK_FALSE(c.deleteTrack(p.track("V1")));
    REQUIRE(c.undo());
    CHECK(p.sequence().videoTracks.size() == 2);

    // Delete Empty Tracks: the empty ones of both kinds in one step (A2 and two new ones), the rest stay.
    REQUIRE(c.addTracks({{QStringLiteral("video"), 2}, {QStringLiteral("audio"), 1}}).size() == 3);
    const std::size_t added = p.undoSteps();
    CHECK(c.trackInfo(p.track("V1")).value(QStringLiteral("emptyTracks")).toInt() == 4);
    CHECK(c.deleteEmptyTracks(QStringLiteral("video")) == 2);
    CHECK(p.sequence().videoTracks.size() == 2);
    CHECK(c.deleteEmptyTracks() == 2);
    CHECK(p.undoSteps() == added + 2);
    CHECK(c.undoText() == QStringLiteral("Undo Delete Empty Tracks"));
    CHECK(p.sequence().audioTracks.size() == 1);
    CHECK(p.sequence().audioTracks[0]->id == p.tl.track("A1"));
    CHECK(c.deleteEmptyTracks() == 0); // nothing left to delete: no step
    CHECK(p.undoSteps() == added + 2);
    CHECK(c.deleteEmptyTracks(QStringLiteral("titles")) == 0);

    // A kind whose every track is empty keeps its lowest-numbered one.
    Project q(R"(
        V1 |Aaaa|
        A1 |....|
        A2 |....|
    )");
    CHECK(q.controller.deleteEmptyTracks(QStringLiteral("audio")) == 1);
    REQUIRE(q.sequence().audioTracks.size() == 1);
    CHECK(q.sequence().audioTracks[0]->id == q.tl.track("A1"));
}

// ---- Default transitions -------------------------------------------------------------------------------------------

TEST_CASE("Set as Default Transition is remembered and used by Apply Video Transition", "[app][timeline][transition][settings]") {
    resetSettings();
    EditingBridge editing;
    CHECK(editing.defaultVideoTransition() == QStringLiteral("crossDissolve"));
    CHECK(editing.defaultAudioTransition() == QStringLiteral("constantPower"));

    const QVariantList video = editing.transitionChoices(QStringLiteral("video"));
    const QVariantList audio = editing.transitionChoices(QStringLiteral("audio"));
    REQUIRE(video.size() >= 2);
    REQUIRE(audio.size() >= 2);
    CHECK(editing.transitionChoices(QStringLiteral("titles")).isEmpty());
    for (const QVariant& v : video) {
        if (idOf(v) == QStringLiteral("crossDissolve"))
            CHECK(v.toMap().value(QStringLiteral("isDefault")).toBool());
    }
    const auto offers = [](const QVariantList& list, const QString& id) {
        return std::any_of(list.begin(), list.end(), [&](const QVariant& v) { return idOf(v) == id; });
    };
    const QString otherVideo = QStringLiteral("dipToBlack");
    const QString otherAudio = QStringLiteral("constantGain");
    CHECK(offers(video, otherVideo));
    CHECK(offers(audio, otherAudio));
    CHECK_FALSE(offers(video, otherAudio));
    REQUIRE_FALSE(otherVideo.isEmpty());
    REQUIRE_FALSE(otherAudio.isEmpty());

    CHECK(editing.setDefaultTransition(otherVideo));
    CHECK(editing.defaultVideoTransition() == otherVideo);
    CHECK(editing.defaultAudioTransition() == QStringLiteral("constantPower"));
    CHECK(EditingBridge::defaultTransitionsSetting().video == otherVideo.toStdString());
    CHECK(QSettings().value(QStringLiteral("timeline/defaultVideoTransition")).toString() == otherVideo);
    CHECK(editing.isDefaultTransition(otherVideo));
    CHECK_FALSE(editing.isDefaultTransition(QStringLiteral("crossDissolve")));
    CHECK(editing.setDefaultTransition(otherAudio));
    CHECK(editing.defaultAudioTransition() == otherAudio);
    CHECK(EditingBridge::defaultTransitionsSetting().audio == otherAudio.toStdString());

    // Not a transition, or the wrong kind: ignored.
    CHECK_FALSE(editing.setDefaultTransition(QStringLiteral("no.such.transition")));
    CHECK_FALSE(editing.setDefaultTransition(QString::fromLatin1(kBorderEffect.data(), static_cast<qsizetype>(kBorderEffect.size()))));
    editing.setDefaultAudioTransition(otherVideo);
    CHECK(editing.defaultAudioTransition() == otherAudio);

    // Ctrl+D puts the chosen transition on the edit point at the playhead; the transition menu describes it.
    Project p(R"(
        V1 tgt |AaaaaaaaaaaaaaaaB20bbbbbbbbbbbbbbbbb|
    )");
    p.controller.setPlayhead(f(16));
    REQUIRE(p.controller.applyDefaultTransition(false));
    const Track& v1 = *p.sequence().findTrack(p.tl.track("V1"));
    REQUIRE(v1.transitions.size() == 1);
    CHECK(v1.transitions[0].effect == otherVideo.toStdString());
    QVariantMap info = p.controller.transitionInfo(transitionKey(v1.transitions[0].id));
    CHECK(info.value(QStringLiteral("effect")).toString() == otherVideo);
    CHECK(info.value(QStringLiteral("isDefault")).toBool());
    CHECK(info.value(QStringLiteral("defaultEffect")).toString() == otherVideo);
    CHECK(info.value(QStringLiteral("twoSided")).toBool());
    CHECK(info.value(QStringLiteral("alignment")).toString() == QStringLiteral("center"));
    CHECK_FALSE(info.value(QStringLiteral("locked")).toBool());
    // Back to Cross Dissolve: the transition is no longer the default, and Replace with Default would use it.
    editing.setDefaultVideoTransition(QStringLiteral("crossDissolve"));
    info = p.controller.transitionInfo(transitionKey(v1.transitions[0].id));
    CHECK_FALSE(info.value(QStringLiteral("isDefault")).toBool());
    CHECK(info.value(QStringLiteral("defaultEffect")).toString() == QStringLiteral("crossDissolve"));

    // A saved id that no longer resolves (a removed effect) reads as the built-in default.
    QSettings().setValue(QStringLiteral("timeline/defaultVideoTransition"), QStringLiteral("gone.transition"));
    CHECK(editing.defaultVideoTransition() == QStringLiteral("crossDissolve"));
    CHECK(EditingBridge::defaultTransitionsSetting().video == "crossDissolve");
    resetSettings();
}

// ---- Reveal in Project / File Explorer ------------------------------------------------------------------------------

TEST_CASE("Reveal in Project and Reveal in File Explorer from a timeline clip", "[app][timeline][reveal][library]") {
    Project p(R"(
        V1 tgt |AaaaBbbb|
        A1 tgt |AaaaBbbb|
    )");
    LibraryBridge library;
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setAnalysisProbe([](const MediaItem&, const std::filesystem::path&) { return LibraryMediaStatus{}; });
    library.setController(&p.controller);
    EditingBridge editing;
    editing.setController(&p.controller);
    editing.setLibrary(&library);
    int libraryRequests = 0;
    QObject::connect(&editing, &EditingBridge::libraryRequested, &editing, [&] { ++libraryRequests; });

    const QString clipB = clipKey(p.tl.clip("V1@4"));
    const QString mediaB = mediaKey(p.tl.media('B'));
    QVariantMap info = editing.revealInfo(clipB);
    CHECK(info.value(QStringLiteral("item")).toString() == mediaB);
    CHECK(info.value(QStringLiteral("name")).toString() == QStringLiteral("B"));
    CHECK(info.value(QStringLiteral("inProject")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("inExplorer")).toBool()); // the fixture's files do not exist
    CHECK(info.value(QStringLiteral("offline")).toBool());
    CHECK(editing.revealInfo(QStringLiteral("c999999")).isEmpty());

    REQUIRE(editing.revealInProject(clipB));
    CHECK(libraryRequests == 1);
    CHECK(library.selection() == QStringList{mediaB});
    CHECK_FALSE(editing.revealInExplorer(clipB));
    CHECK_FALSE(editing.message().isEmpty());

    // The keymap command (no default key) reveals the primary selected clip.
    CHECK(EditingBridge::handlesCommand(QStringLiteral("clip.revealInProject")));
    p.controller.selectClip(clipKey(p.tl.clip("V1@0")));
    CHECK(editing.runCommand(QStringLiteral("clip.revealInProject")));
    CHECK(library.selection() == QStringList{mediaKey(p.tl.media('A'))});
    CHECK(libraryRequests == 2);

    // A real file: Reveal in File Explorer shows it.
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString file = touch(dir, QStringLiteral("B.mov"));
    relink(p.controller, p.tl.media('B'), file);
    info = editing.revealInfo(clipB);
    CHECK(info.value(QStringLiteral("inExplorer")).toBool());
    CHECK_FALSE(info.value(QStringLiteral("offline")).toBool());
    QString revealed;
    LibraryBridge::setRevealer([&](const QString& path) {
        revealed = path;
        return true;
    });
    CHECK(editing.revealInExplorer(clipB));
    CHECK(QFileInfo(revealed) == QFileInfo(file));
    LibraryBridge::setRevealer({});
}

// ---- Library drag In/Out, the card's marks, the Source Monitor's dropped files ------------------------------------

TEST_CASE("A Library drop edits in the item's marked In to Out and the card shows the marks", "[app][timeline][drop][library]") {
    Project p(R"(
        V2     |............|
        V1 tgt |Aaaa........|
        A1 tgt |Aaaa........|
        A2     |............|
    )", {{'S', 50}});
    ProjectLifecycle lifecycle{&p.controller};
    p.controller.resetProject(p.tl.state, p.tl.seq);
    LibraryBridge library;
    library.setPersistent(false);
    library.setSynchronous(true);
    library.setAnalysisProbe([](const MediaItem&, const std::filesystem::path&) { return LibraryMediaStatus{}; });
    library.setController(&p.controller);
    const MediaId s = p.tl.media('S');
    const QString key = mediaKey(s);
    LibraryModel* items = library.items();
    const auto marksOf = [&] { return items->data(items->index(items->rowOf(key), 0), LibraryModel::MarksRole).toMap(); };
    CHECK(marksOf().isEmpty()); // no marks: no band

    p.edit("Set Source Marks", [&](const ProjectState& state) { return edit::setSourceMarks(state, s, {f(10), f(15)}); });
    const QVariantMap marks = marksOf();
    CHECK(marks.value(QStringLiteral("in")).toDouble() == 0.2);
    CHECK(marks.value(QStringLiteral("out")).toDouble() == 0.3);
    CHECK(marks.value(QStringLiteral("inText")).toString() == QStringLiteral("00:00:00:10"));
    CHECK(marks.value(QStringLiteral("outText")).toString() == QStringLiteral("00:00:00:14")); // the last marked frame
    CHECK(marks.value(QStringLiteral("durationText")).toString() == QStringLiteral("00:00:00:05"));

    // The drop on V2 at frame 5: S's frames 10 to 14, not the whole item.
    REQUIRE(lifecycle.dropMedia(key, QStringLiteral("00:00:00:05"), p.track("V2"), true));
    const Track& v2 = *p.sequence().findTrack(p.tl.track("V2"));
    REQUIRE(v2.clips.size() == 1);
    CHECK(v2.clips[0].start() == f(5));
    CHECK(v2.clips[0].duration() == f(5));
    CHECK(v2.clips[0].sourceIn == f(10));

    // Only an In: from there to the end of the item.
    REQUIRE(p.controller.undo());
    p.edit("Set Source Marks", [&](const ProjectState& state) { return edit::setSourceMarks(state, s, {f(40), std::nullopt}); });
    REQUIRE(lifecycle.dropMedia(key, QStringLiteral("00:00:00:05"), p.track("V2"), true));
    const Track& v2b = *p.sequence().findTrack(p.tl.track("V2"));
    REQUIRE(v2b.clips.size() == 1);
    CHECK(v2b.clips[0].duration() == f(10));
    CHECK(v2b.clips[0].sourceIn == f(40));

    // A subclip (frames 20 to 30 of S; restricted, Make Subclip's default). Its marks are its range: the card shows no
    // band, and the drop is the subclip. A missing mark is the subclip's start or end (as an unmarked three-point edit
    // takes it), not the file's - restricted or not.
    REQUIRE(p.controller.undo()); // the drop
    REQUIRE(p.controller.undo()); // the In
    const auto made = edit::makeSubclip(*p.controller.snapshot(), s, TimeRange{f(20), f(10)}, "Verse");
    REQUIRE(made);
    REQUIRE(p.controller.session().commit("Make Subclip", made->state));
    const MediaId sub = made->id;
    const QString subKey = mediaKey(sub);
    REQUIRE(items->rowOf(subKey) >= 0);
    const auto subMarks = [&] { return items->data(items->index(items->rowOf(subKey), 0), LibraryModel::MarksRole).toMap(); };
    CHECK(subMarks().isEmpty());
    const auto dropSubclip = [&](Ticks in, Ticks length) {
        const bool dropped = lifecycle.dropMedia(subKey, QStringLiteral("00:00:00:05"), p.track("V2"), true);
        INFO("lastError " << lifecycle.lastError().toStdString());
        REQUIRE(dropped);
        const Track& v2 = *p.sequence().findTrack(p.tl.track("V2"));
        REQUIRE(v2.clips.size() == 1);
        CHECK(v2.clips[0].sourceIn == in);
        CHECK(v2.clips[0].duration() == length);
        REQUIRE(p.controller.undo());
    };
    dropSubclip(f(20), f(10));

    // Only an In (its Out cleared in the Source Monitor): to the subclip's end; the card shows the band from the In.
    p.edit("Set Source Marks", [&](const ProjectState& state) { return edit::setSourceMarks(state, sub, {f(22), std::nullopt}); });
    const QVariantMap inOnly = subMarks();
    CHECK(inOnly.value(QStringLiteral("in")).toDouble() == 0.2);
    CHECK(inOnly.value(QStringLiteral("out")).toDouble() == 1.0);
    dropSubclip(f(22), f(8));

    // Both marks cleared: the subclip's range.
    p.edit("Clear Source Marks", [&](const ProjectState& state) { return edit::setSourceMarks(state, sub, {}); });
    CHECK(subMarks().isEmpty());
    dropSubclip(f(20), f(10));

    // Unrestricted (the whole file usable), marks cleared: still its own range, not the whole file.
    const auto open = edit::setSubclip(*p.controller.snapshot(), sub, TimeRange{f(20), f(10)}, false);
    REQUIRE(open);
    REQUIRE(p.controller.session().commit("Edit Subclip", *open));
    dropSubclip(f(20), f(10));
}

TEST_CASE("The Source Monitor loads the project item of a file dropped from File Explorer", "[app][source][drop]") {
    // What the panel does with a file dropped from Explorer: ProjectFile.importFiles, then loadFileItem(first file).
    resetSettings();
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString still = dir.filePath(QStringLiteral("Card.png"));
    {
        QImage picture(64, 36, QImage::Format_RGB32);
        picture.fill(Qt::darkCyan);
        REQUIRE(picture.save(still));
    }
    ProjectController controller;
    ProjectLifecycle lifecycle{&controller};
    SourceMonitorBridge source;
    source.setController(&controller);
    REQUIRE(lifecycle.importFiles({QUrl::fromLocalFile(still)}).value(QStringLiteral("added")).toInt() == 1);
    MediaId card;
    for (const MediaPtr& m : controller.snapshot()->media) {
        if (m->name == "Card.png")
            card = m->id;
    }
    REQUIRE(card);

    CHECK(source.loadFileItem(QUrl::fromLocalFile(still).toString()));
    CHECK(source.mediaKeyText() == mediaKey(card));
    CHECK(source.recent().size() == 1);
    source.unload();
    CHECK(source.loadFileItem(QDir::toNativeSeparators(still))); // a plain path too
    CHECK(source.mediaKeyText() == mediaKey(card));

    // A file the project does not have: nothing loads, the message names it.
    const QString other = touch(dir, QStringLiteral("Other.mov"));
    CHECK_FALSE(source.loadFileItem(QUrl::fromLocalFile(other).toString()));
    CHECK(source.message().contains(QStringLiteral("Other.mov")));
    CHECK(source.mediaKeyText() == mediaKey(card));
}

// ---- Marker tooltips ------------------------------------------------------------------------------------------------

TEST_CASE("Markers carry their name comment time and length for the ruler and time bar tooltips", "[app][timeline][markers]") {
    Rig rig(R"(
        V1 tgt |Aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa|
    )");
    ProjectController& c = rig.controller;
    Marker hook;
    hook.start = f(5);
    hook.name = "Hook";
    hook.comment = "Start the story here";
    Marker chorus;
    chorus.start = f(20);
    chorus.duration = f(25);
    chorus.name = "Chorus";
    chorus.type = MarkerType::Chapter;
    REQUIRE(c.applyEditFn("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, rig.tl.seq, hook); }));
    REQUIRE(c.applyEditFn("Add Marker", [&](const ProjectState& s) { return edit::addMarker(s, rig.tl.seq, chorus); }));
    rig.harness.frame();

    const QVariantList markers = c.markers();
    REQUIRE(markers.size() == 2);
    QVariantMap m0 = markers[0].toMap();
    QVariantMap m1 = markers[1].toMap();
    CHECK(m0.value(QStringLiteral("name")).toString() == QStringLiteral("Hook"));
    CHECK(m0.value(QStringLiteral("comment")).toString() == QStringLiteral("Start the story here"));
    CHECK(m0.value(QStringLiteral("timecode")).toString() == QStringLiteral("00:00:00:05"));
    CHECK(m0.value(QStringLiteral("durationText")).toString().isEmpty());
    CHECK(m0.value(QStringLiteral("type")).toString() == QStringLiteral("comment"));
    CHECK(m1.value(QStringLiteral("durationText")).toString() == QStringLiteral("00:00:01:00"));
    CHECK(m1.value(QStringLiteral("type")).toString() == QStringLiteral("chapter"));

    // The ruler's items: the same facts, and where the name label must stop (the next marker).
    const QVariantList items = rig.view->markerItems();
    REQUIRE(items.size() == 2);
    m0 = items[0].toMap();
    m1 = items[1].toMap();
    CHECK(m0.value(QStringLiteral("comment")).toString() == QStringLiteral("Start the story here"));
    CHECK(m0.value(QStringLiteral("timecode")).toString() == QStringLiteral("00:00:00:05"));
    CHECK(m0.value(QStringLiteral("nextX")).toDouble() == m1.value(QStringLiteral("x")).toDouble());
    CHECK(std::isinf(m1.value(QStringLiteral("nextX")).toDouble()));
    CHECK(m1.value(QStringLiteral("durationText")).toString() == QStringLiteral("00:00:01:00"));
    CHECK(m1.value(QStringLiteral("endX")).toDouble() > m1.value(QStringLiteral("x")).toDouble());

    // The Program Monitor's time bar finds the marker under the pointer for its tooltip.
    SequenceTimeBar bar;
    bar.setController(&c);
    bar.setSize(QSizeF(500, 24));
    const double perSecond = 500.0 / bar.span().toSeconds();
    const QVariantMap atHook = bar.markerAt(perSecond * f(5).toSeconds() + 2);
    CHECK(atHook.value(QStringLiteral("name")).toString() == QStringLiteral("Hook"));
    CHECK(atHook.value(QStringLiteral("comment")).toString() == QStringLiteral("Start the story here"));
    CHECK(atHook.value(QStringLiteral("timecode")).toString() == QStringLiteral("00:00:00:05"));
    const QVariantMap inChorus = bar.markerAt(perSecond * f(30).toSeconds()); // inside the range marker's bar
    CHECK(inChorus.value(QStringLiteral("index")).toInt() == 1);
    CHECK(inChorus.value(QStringLiteral("durationText")).toString() == QStringLiteral("00:00:01:00"));
    CHECK(bar.markerAt(perSecond * f(12).toSeconds()).isEmpty());
}

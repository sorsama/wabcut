// M5o (gaming): one-click "moments" (MomentsBridge, singleton `Moments`), offscreen; no window is shown.
//   * speed presets on the time remapping commands: slow-mo eased down and back around the playhead, speed ramp in,
//     freeze + replay and fast forward; one undo step each, keys on frames, the linked audio following the picture,
//     clamped inside the clip and refused on a clip too short;
//   * zoom presets as Motion keyframes relative to the clip's scale and position: punch in, punch in + hold, shake;
//   * in the shell: the clip menu's Moments submenu and the Tracks header's Moments popover; screenshot.

#include "A11yAudit.h"
#include "ShellFixture.h"
#include "TimelineFixture.h"

#include "BridgeIds.h"
#include "MomentsBridge.h"
#include "ProjectController.h"
#include "TimeRemapBridge.h"

#include <wabcut/command/Engine.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/model/TimeRemap.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QDir>
#include <QQuickItem>

#include <string>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

/// 250 frames of A from its frame 100 on V1 and A1 (linked), then 10 frames of B; C alone on A2 (no picture).
std::string picture() {
    const std::string body = "A100" + std::string(246, 'a') + "Bbbbbbbbbb";
    return "V1 tgt |" + body + "|\nA1 tgt |" + body + "|\nA2     |" + std::string(20, '.') + "Cccccccccc" +
           std::string(230, '.') + "|";
}

struct MomentsRig {
    Timeline tl;
    ProjectController controller;
    MomentsBridge moments;

    explicit MomentsRig(const std::string& text = picture()) : tl(text) {
        tl.linkMatching();
        controller.resetProject(tl.state, tl.seq);
        moments.setController(&controller);
    }
    std::size_t undo() { return controller.session().undoCount(); }
    /// The clip that started at `ref` in the picture, as it is now.
    Clip now(std::string_view ref) const { return controller.currentSequence()->findClip(tl.clip(ref))->clip(); }
    Clip video() const { return now("V1@0"); }
    Clip audio() const { return now("A1@0"); }
    QString key(std::string_view ref) const { return clipKey(tl.clip(ref)); }
};

/// The speed (percent) at timeline frame `frame`.
double speedAt(const Clip& c, std::int64_t frame) { return TimeRemapBridge::speedAt(c, c.curveTimeAt(f(frame))); }
/// The footage frame shown at timeline frame `frame`.
std::int64_t shownAt(const Clip& c, std::int64_t frame) { return ticksToFrames(c.sourceTimeAt(f(frame)), kRate); }
/// The timeline frame of each remap key inside the clip.
std::vector<std::int64_t> keyFrames(const Clip& c) {
    std::vector<std::int64_t> out;
    for (const Keyframe& k : c.timeRemap()->keys) {
        const Ticks t = c.start() + (k.time - c.sourceIn);
        if (t >= c.start() && t < c.end())
            out.push_back(ticksToFrames(t, kRate));
    }
    return out;
}
/// The keyframes of a Motion parameter.
std::vector<Keyframe> keysOf(const Clip& c, ParamId id) {
    const ParamValue v = ProjectController::parameterValue(c, edit::ParamRef::of(id));
    return v.curve() ? v.curve()->keys : std::vector<Keyframe>{};
}
/// The linked audio plays the same remap at the same place: it stays in sync.
void requireInSync(const MomentsRig& r) {
    const Clip v = r.video();
    const Clip a = r.audio();
    REQUIRE(a.isRemapped());
    CHECK(*a.timeRemap() == *v.timeRemap());
    CHECK(a.start() == v.start());
    CHECK(a.duration() == v.duration());
    CHECK(a.sourceIn == v.sourceIn);
}

} // namespace

TEST_CASE("Moments: slow-mo eases down to 25 percent around the playhead in one undo step", "[app][moments]") {
    MomentsRig r;
    r.controller.setPlayhead(f(100));
    const std::size_t before = r.undo();
    REQUIRE(r.moments.slowMo(25.0, 1.0).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Slow Motion"));
    CHECK(r.moments.message().isEmpty());

    // Footage 80..121 (the slow second 88..113 centred on frame 100), on frames: 75 / 60 / 40 % steps, 25 % for
    // 100 frames, and back up. The clip grows by what the remap gains; B moves with it.
    const Clip v = r.video();
    REQUIRE(v.isRemapped());
    CHECK(keyFrames(v) == std::vector<std::int64_t>{80, 84, 89, 94, 194, 199, 204, 208});
    CHECK(speedAt(v, 50) == Catch::Approx(100));
    CHECK(speedAt(v, 82) == Catch::Approx(75));
    CHECK(speedAt(v, 86) == Catch::Approx(60));
    CHECK(speedAt(v, 91) == Catch::Approx(40));
    CHECK(speedAt(v, 144) == Catch::Approx(25));
    CHECK(speedAt(v, 196) == Catch::Approx(40));
    CHECK(speedAt(v, 201) == Catch::Approx(60));
    CHECK(speedAt(v, 206) == Catch::Approx(75));
    CHECK(speedAt(v, 250) == Catch::Approx(100));
    CHECK(v.duration() == f(337));
    CHECK(clipAt(*r.controller.snapshot(), r.tl.seq, "V1@337") == r.tl.clip("V1@250"));
    // The playhead stays on the moment's frame, now in the middle of the slow stretch.
    CHECK(r.controller.playhead() == f(144));
    CHECK(shownAt(v, 144) == 200);
    CHECK(shownAt(v, 50) == 150);
    CHECK(shownAt(v, 250) == 263);
    // The audio follows and is heard where the clip plays at 100 % (before and after the ramp).
    requireInSync(r);
    const std::vector<TimeRange> heard = v.unitySpeedRanges();
    REQUIRE(heard.size() == 2);
    CHECK(heard.front() == TimeRange{f(0), f(80)});
    CHECK(heard.back() == TimeRange{f(208), f(337 - 208)});

    r.controller.undo();
    CHECK_FALSE(r.video().isRemapped());
    CHECK(r.video().duration() == f(250));
    CHECK_FALSE(r.audio().isRemapped());
}

TEST_CASE("Moments: slow-mo is clamped inside the clip and scaled down on a short one", "[app][moments]") {
    SECTION("near the clip's start") {
        MomentsRig r;
        r.controller.setPlayhead(f(3));
        REQUIRE(r.moments.slowMo(50.0, 1.0).isEmpty());
        const Clip v = r.video();
        // The ramp starts a frame in; 50 % steps 80 / 75 / 60 %; the slow stretch 25 frames -> 50.
        CHECK(keyFrames(v).front() == 1);
        CHECK(speedAt(v, 0) == Catch::Approx(100));
        const auto keys = keyFrames(v);
        REQUIRE(keys.size() == 8);
        CHECK(keys[4] - keys[3] == 50);
        CHECK(speedAt(v, keys[3] + 25) == Catch::Approx(50));
    }
    SECTION("near the clip's end") {
        MomentsRig r;
        r.controller.setPlayhead(f(248));
        REQUIRE(r.moments.slowMo(25.0, 1.0).isEmpty());
        const Clip v = r.video();
        // It ends a frame before the clip's end: that last frame still plays at 100 %, with sound.
        const std::vector<TimeRange> heard = v.unitySpeedRanges();
        REQUIRE_FALSE(heard.empty());
        CHECK(heard.back() == TimeRange{v.end() - f(1), f(1)});
        CHECK(keyFrames(v).front() == 208);
    }
    SECTION("a short clip keeps its steps and slows less footage") {
        MomentsRig r("V1 tgt |C" + std::string(29, 'c') + "|");
        r.controller.setPlayhead(f(15));
        REQUIRE(r.moments.slowMo(25.0, 1.0).isEmpty());
        const Clip v = r.now("V1@0");
        const auto keys = keyFrames(v);
        // 0 is the remap's key on the footage's first frame (the clip starts there).
        CHECK(keys == std::vector<std::int64_t>{0, 1, 5, 10, 15, 63, 68, 73, 77});
        CHECK(speedAt(v, 40) == Catch::Approx(25));
    }
    SECTION("a clip too short is refused without an undo step") {
        MomentsRig r("V1 tgt |Ccc|");
        r.controller.setPlayhead(f(1));
        const std::size_t before = r.undo();
        const QString why = r.moments.slowMo(25.0, 1.0);
        CHECK_FALSE(why.isEmpty());
        CHECK(r.moments.message() == why);
        CHECK(r.undo() == before);
        CHECK_FALSE(r.now("V1@0").isRemapped());
    }
}

TEST_CASE("Moments: speed ramp in plays the second before the playhead at 300 percent", "[app][moments]") {
    MomentsRig r;
    r.controller.setPlayhead(f(100));
    const std::size_t before = r.undo();
    REQUIRE(r.moments.rampIn(300.0, 1.0).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Speed Ramp In"));
    const Clip v = r.video();
    // Footage 64..88 at 300 % (8 frames), then 250 / 200 / 150 % for 2 frames each, landing on 100 % at the moment.
    CHECK(keyFrames(v) == std::vector<std::int64_t>{64, 72, 74, 76, 78});
    CHECK(speedAt(v, 68) == Catch::Approx(300));
    CHECK(speedAt(v, 73) == Catch::Approx(250));
    CHECK(speedAt(v, 75) == Catch::Approx(200));
    CHECK(speedAt(v, 77) == Catch::Approx(150));
    CHECK(speedAt(v, 90) == Catch::Approx(100));
    CHECK(v.duration() == f(228));
    CHECK(r.controller.playhead() == f(78));
    CHECK(shownAt(v, 78) == 200);
    requireInSync(r);
}

TEST_CASE("Moments: fast forward plays In to Out or 3 seconds from the playhead faster", "[app][moments]") {
    SECTION("In to Out at 400 percent") {
        MomentsRig r;
        r.controller.setPlayhead(f(40));
        r.controller.markInAtPlayhead();
        r.controller.setPlayhead(f(100));
        r.controller.markOutAtPlayhead();
        REQUIRE(r.controller.markIn() == f(40));
        const std::int64_t out = ticksToFrames(*r.controller.markOut(), kRate);
        const std::int64_t length = (out - 40) / 4 * 4;
        const std::size_t before = r.undo();
        REQUIRE(r.moments.fastForward(400.0).isEmpty());
        CHECK(r.undo() == before + 1);
        CHECK(r.controller.undoText() == QStringLiteral("Undo Fast Forward"));
        const Clip v = r.video();
        CHECK(keyFrames(v) == std::vector<std::int64_t>{40, 40 + length / 4});
        CHECK(speedAt(v, 42) == Catch::Approx(400));
        CHECK(v.duration() == f(250 - length + length / 4));
        requireInSync(r);
    }
    SECTION("3 seconds from the playhead at 200 percent") {
        MomentsRig r;
        r.controller.setPlayhead(f(100));
        REQUIRE(r.moments.fastForward(200.0).isEmpty());
        const Clip v = r.video();
        CHECK(keyFrames(v) == std::vector<std::int64_t>{100, 137});
        CHECK(speedAt(v, 120) == Catch::Approx(200));
        CHECK(v.duration() == f(250 - 37));
    }
}

TEST_CASE("Moments: freeze and replay holds the frame then replays the action at 50 percent", "[app][moments]") {
    MomentsRig r;
    r.controller.setPlayhead(f(100));
    const std::size_t before = r.undo();
    REQUIRE(r.moments.freezeReplay(0.5, 2.0, 50.0).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Freeze and Replay"));
    const Clip v = r.video();
    // Frame 200 holds for 13 frames, the 2 seconds before it play again at 50 %, then the clip carries on.
    CHECK(shownAt(v, 99) == 199);
    CHECK(shownAt(v, 100) == 200);
    CHECK(shownAt(v, 112) == 200);
    CHECK(speedAt(v, 105) == Catch::Approx(0));
    CHECK(shownAt(v, 113) == 150);
    CHECK(speedAt(v, 150) == Catch::Approx(50));
    CHECK(shownAt(v, 212) == 199);
    CHECK(shownAt(v, 213) == 200);
    CHECK(shownAt(v, 214) == 201);
    CHECK(speedAt(v, 250) == Catch::Approx(100));
    CHECK(v.duration() == f(363));
    CHECK(r.controller.playhead() == f(100));
    requireInSync(r);
    const std::vector<TimeRange> heard = v.unitySpeedRanges();
    REQUIRE(heard.size() == 2);
    CHECK(heard.back().start == f(213));
    r.controller.undo();
    CHECK_FALSE(r.video().isRemapped());
}

TEST_CASE("Moments: speed presets act on the linked video of an audio clip and refuse audio alone", "[app][moments]") {
    MomentsRig r;
    r.controller.setPlayhead(f(100));
    REQUIRE(r.moments.slowMo(25.0, 1.0, r.key("A1@0")).isEmpty());
    CHECK(r.video().isRemapped());
    requireInSync(r);

    const std::size_t before = r.undo();
    CHECK_FALSE(r.moments.slowMo(25.0, 1.0, r.key("A2@20")).isEmpty());
    CHECK_FALSE(r.moments.punchIn(130.0, false, r.key("A2@20")).isEmpty());
    CHECK(r.undo() == before);
    CHECK_FALSE(r.moments.message().isEmpty());
    CHECK_FALSE(r.moments.slowMo(25.0, 1.0, QStringLiteral("c999999")).isEmpty());
    CHECK(r.moments.apply(QStringLiteral("nothing")) != QString());
}

TEST_CASE("Moments: punch in keys the scale relative to the clip and comes back out", "[app][moments]") {
    MomentsRig r;
    // The clip is already at 50 %: the punch-in goes to 65 % and back to 50 %.
    {
        command::CommandEngine engine(r.controller.session());
        command::CommandRequest request;
        request.command = "clip.setParam";
        request.params = {{"sequence", r.tl.seq.value},
                          {"clips", command::Json::array({r.tl.clip("V1@0").value})},
                          {"param", "motion.scale"},
                          {"value", 0.5}};
        REQUIRE(engine.execute(request).ok());
    }
    r.controller.setPlayhead(f(100));
    std::size_t before = r.undo();
    REQUIRE(r.moments.punchIn(130.0, false).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Punch In"));
    Clip v = r.video();
    std::vector<Keyframe> keys = keysOf(v, ParamId::Scale);
    REQUIRE(keys.size() == 4);
    CHECK(keys[0].time == v.curveTimeAt(f(100)));
    CHECK(keys[1].time == v.curveTimeAt(f(103)));
    CHECK(keys[2].time == v.curveTimeAt(f(128)));
    CHECK(keys[3].time == v.curveTimeAt(f(133)));
    CHECK(keys[0].value == Catch::Approx(0.5));
    CHECK(keys[1].value == Catch::Approx(0.65));
    CHECK(keys[2].value == Catch::Approx(0.65));
    CHECK(keys[3].value == Catch::Approx(0.5));
    CHECK(keys[0].interpolation == Interpolation::Linear);
    CHECK(keys[1].interpolation == Interpolation::EaseIn);
    CHECK(keys[2].interpolation == Interpolation::EaseOut);
    CHECK(keys[3].interpolation == Interpolation::EaseIn);
    CHECK_FALSE(v.isRemapped());
    r.controller.undo();
    CHECK(keysOf(r.video(), ParamId::Scale).empty());

    // Punch in + hold: in and stays in.
    before = r.undo();
    REQUIRE(r.moments.punchIn(130.0, true).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Punch In and Hold"));
    keys = keysOf(r.video(), ParamId::Scale);
    REQUIRE(keys.size() == 2);
    CHECK(keys[1].value == Catch::Approx(0.65));
    r.controller.undo();

    // Near the clip's end there is no room to come back out: it stays in, inside the clip.
    r.controller.setPlayhead(f(248));
    REQUIRE(r.moments.punchIn(130.0, false).isEmpty());
    v = r.video();
    keys = keysOf(v, ParamId::Scale);
    REQUIRE(keys.size() == 2);
    CHECK(keys[0].time == v.curveTimeAt(f(246)));
    CHECK(keys[1].time == v.curveTimeAt(f(249)));
}

TEST_CASE("Moments: shake wiggles the position and ends where it started", "[app][moments]") {
    MomentsRig r;
    r.controller.setPlayhead(f(100));
    const std::size_t before = r.undo();
    REQUIRE(r.moments.shake(14.0).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(r.controller.undoText() == QStringLiteral("Undo Shake"));
    const Clip v = r.video();
    const std::vector<Keyframe> x = keysOf(v, ParamId::PositionX);
    const std::vector<Keyframe> y = keysOf(v, ParamId::PositionY);
    REQUIRE(x.size() == 9);
    REQUIRE(y.size() == 9);
    CHECK(x.front().time == v.curveTimeAt(f(100)));
    CHECK(x.back().time == v.curveTimeAt(f(108)));
    CHECK(x.front().value == Catch::Approx(0));
    CHECK(x[1].value == Catch::Approx(14));
    CHECK(x[2].value == Catch::Approx(-11.9));
    CHECK(x.back().value == Catch::Approx(0));
    CHECK(y.back().value == Catch::Approx(0));
    // Each swing is smaller than the one before.
    for (std::size_t k = 2; k + 1 < x.size(); ++k)
        CHECK(std::abs(x[k].value) < std::abs(x[k - 1].value));
}

namespace {

/// C.16's clip.instantReplay and sequence.makeVersion as its command docs describe them (the parameters the app sends),
/// stood in for by small edits: a "Replay" marker; a 1080x1920 sequence named after the format.
command::Json g_c16Params;

const command::CommandRegistry& c16Catalog() {
    static const command::CommandRegistry registry = [] {
        command::CommandRegistry r;
        command::CommandSpec replay;
        replay.name = "clip.instantReplay";
        replay.title = "Instant Replay";
        replay.description = "C.16 stand-in";
        replay.capability = command::Capability::EditSequence;
        replay.params = command::Json{{"type", "object"}};
        replay.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
            g_c16Params = c.params();
            Marker marker;
            marker.name = "Replay";
            edit::EditResult edited = c.take(edit::addMarker(c.state(), SequenceId{static_cast<std::uint64_t>(c.integer("sequence"))},
                                                             marker));
            command::CommandOutput out;
            out.state = std::move(edited.state);
            return out;
        };
        (void)r.add(std::move(replay));
        command::CommandSpec version;
        version.name = "sequence.makeVersion";
        version.title = "Make Vertical or Square Version";
        version.description = "C.16 stand-in";
        version.capability = command::Capability::EditProject;
        version.params = command::Json{{"type", "object"}};
        version.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
            g_c16Params = c.params();
            edit::SequenceSettings settings;
            settings.name = c.string("format") == "square" ? "Square" : "Shorts";
            settings.width = 1080;
            settings.height = c.string("format") == "square" ? 1080 : 1920;
            auto created = edit::addSequence(c.state(), settings);
            if (!created)
                command::fail(command::ErrorCode::EditRejected, created.error().message);
            command::CommandOutput out;
            out.data = command::Json{{"sequence", created->id.value}, {"width", settings.width}, {"height", settings.height}};
            out.createdIds = {created->id.value};
            out.state = std::move(created->state);
            return out;
        };
        (void)r.add(std::move(version));
        command::CommandSpec dead;
        dead.name = "sequence.removeDeadTime";
        dead.title = "Remove Dead Time";
        dead.description = "C.16 stand-in";
        dead.capability = command::Capability::EditSequence;
        dead.params = command::Json{{"type", "object"}};
        dead.execute = [](command::ExecutionContext& c) -> command::CommandOutput {
            g_c16Params = c.params();
            command::CommandOutput out;
            if (!c.boolean("apply", false)) { // the preview: two stretches, 9 s in all
                out.state = c.state();
                out.data = command::Json{
                    {"ranges", command::Json::array({command::Json{{"start", "00:00:01:00"}, {"end", "00:00:05:00"},
                                                                   {"startTicks", kTicksPerSecond}, {"endTicks", 5 * kTicksPerSecond},
                                                                   {"seconds", 4.0}},
                                                     command::Json{{"start", "00:00:06:00"}, {"end", "00:00:11:00"},
                                                                   {"startTicks", 6 * kTicksPerSecond},
                                                                   {"endTicks", 11 * kTicksPerSecond}, {"seconds", 5.0}}})},
                    {"removeSeconds", 9.0}};
                return out;
            }
            Marker marker;
            marker.name = "Dead time removed";
            edit::EditResult edited = c.take(edit::addMarker(c.state(), SequenceId{static_cast<std::uint64_t>(c.integer("sequence"))},
                                                             marker));
            out.state = std::move(edited.state);
            return out;
        };
        (void)r.add(std::move(dead));
        return r;
    }();
    return registry;
}

} // namespace

TEST_CASE("Moments: C.16's instant replay and Shorts version run when the catalog has them, one undo step each",
          "[app][moments]") {
    MomentsRig r;
    // The app's catalog without C.16: the entries stay hidden and refuse with a reason.
    static const command::CommandRegistry none;
    r.moments.setRegistry(&none);
    CHECK_FALSE(r.moments.gamingEdits().value(QStringLiteral("replay")).toBool());
    CHECK_FALSE(r.moments.gamingEdits().value(QStringLiteral("version")).toBool());
    CHECK_FALSE(r.moments.instantReplay().isEmpty());
    CHECK_FALSE(r.moments.makeVersion().isEmpty());

    r.moments.setRegistry(&c16Catalog());
    CHECK(r.moments.gamingEdits().value(QStringLiteral("replay")).toBool());
    CHECK(r.moments.gamingEdits().value(QStringLiteral("version")).toBool());
    r.controller.setPlayhead(f(100));
    std::size_t before = r.undo();
    CHECK(r.moments.apply(QStringLiteral("instantReplay")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(g_c16Params.value("clip", std::uint64_t{0}) == r.tl.clip("V1@0").value);
    CHECK(g_c16Params.value("speed", 0.0) == Catch::Approx(0.5));
    CHECK(g_c16Params.value("ramp", false));
    CHECK(g_c16Params.value("label", false));
    // A clip with sound only: refused before the command runs.
    CHECK_FALSE(r.moments.instantReplay(r.key("A2@20")).isEmpty());
    CHECK(r.undo() == before + 1);

    // Make Shorts version: one undo step, and the new 1080x1920 sequence is the one shown.
    const SequenceId original = r.controller.currentSequenceId();
    before = r.undo();
    CHECK(r.moments.apply(QStringLiteral("shortsVersion")).isEmpty());
    CHECK(r.undo() == before + 1);
    CHECK(g_c16Params.value("format", std::string()) == "shorts");
    CHECK(g_c16Params.value("sequence", std::uint64_t{0}) == original.value);
    CHECK(r.controller.currentSequenceId() != original);
    CHECK(r.controller.currentSequence()->height == 1920);

    // Remove dead time: the preview (off the GUI thread in the app; here synchronous) says how much, Remove sends those
    // ranges back and is one undo step.
    r.moments.setSynchronous(true);
    CHECK(r.moments.gamingEdits().value(QStringLiteral("deadTime")).toBool());
    CHECK_FALSE(r.moments.removeDeadTime().isEmpty()); // nothing found yet
    const std::size_t beforeFind = r.undo();
    r.moments.findDeadTime();
    QVariantMap dead = r.moments.deadTime();
    CHECK(dead.value(QStringLiteral("found")).toBool());
    CHECK(dead.value(QStringLiteral("count")).toInt() == 2);
    CHECK(dead.value(QStringLiteral("seconds")).toDouble() == Catch::Approx(9.0));
    CHECK(dead.value(QStringLiteral("text")).toString().contains(QStringLiteral("0:09")));
    CHECK_FALSE(g_c16Params.value("apply", false));
    CHECK(r.undo() == beforeFind); // the preview changes nothing
    CHECK(r.moments.removeDeadTime().isEmpty());
    CHECK(r.undo() == beforeFind + 1);
    CHECK(g_c16Params.value("apply", false));
    REQUIRE(g_c16Params.contains("ranges"));
    CHECK(g_c16Params["ranges"].size() == 2);
    CHECK(g_c16Params["ranges"][0].value("start", std::int64_t{0}) == kTicksPerSecond);
    CHECK(g_c16Params["ranges"][1].value("end", std::int64_t{0}) == 11 * kTicksPerSecond);
    CHECK_FALSE(r.moments.deadTime().value(QStringLiteral("found")).toBool()); // used up
    r.moments.setRegistry(nullptr);
}

TEST_CASE("Moments: the target follows the playhead and the selection and the keymap commands run", "[app][moments]") {
    MomentsRig r;
    r.controller.setPlayhead(f(100));
    QVariantMap target = r.moments.target();
    CHECK(target.value(QStringLiteral("available")).toBool());
    CHECK(target.value(QStringLiteral("clip")).toString() == r.key("V1@0"));
    CHECK(target.value(QStringLiteral("speed")).toBool());
    CHECK(target.value(QStringLiteral("atText")).toString() == r.controller.formatTimecode(f(100)));
    // Past the last clip nothing is under the playhead.
    r.controller.setPlayhead(f(400));
    target = r.moments.target();
    CHECK_FALSE(target.value(QStringLiteral("available")).toBool());
    CHECK_FALSE(target.value(QStringLiteral("reason")).toString().isEmpty());
    // The selected clip, at its middle, when the playhead is elsewhere.
    r.controller.selectClipById(r.tl.clip("V1@250"), ProjectController::Replace);
    target = r.moments.target();
    CHECK(target.value(QStringLiteral("clip")).toString() == r.key("V1@250"));
    CHECK(target.value(QStringLiteral("atText")).toString() == r.controller.formatTimecode(f(255)));
    // A clip with sound only has nothing to retime or zoom.
    CHECK_FALSE(r.moments.targetFor(r.key("A2@20")).value(QStringLiteral("available")).toBool());

    // moments.slowMo (25 %) and moments.punchIn (130 %) at the playhead.
    r.controller.selectClipById(r.tl.clip("V1@0"), ProjectController::Replace);
    r.controller.setPlayhead(f(100));
    std::size_t before = r.undo();
    CHECK(r.moments.runCommand(QStringLiteral("moments.slowMo")));
    CHECK(r.undo() == before + 1);
    CHECK(speedAt(r.video(), ticksToFrames(r.controller.playhead(), kRate)) == Catch::Approx(25));
    before = r.undo();
    CHECK(r.moments.runCommand(QStringLiteral("moments.punchIn")));
    CHECK(r.undo() == before + 1);
    CHECK(keysOf(r.video(), ParamId::Scale).size() == 4);
    CHECK_FALSE(r.moments.runCommand(QStringLiteral("timeline.zoomIn")));
}

// ---- In the shell ---------------------------------------------------------------------------------------------------

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
    for (QObject* o : root->findChildren<QObject*>()) {
        if (o->objectName() == name)
            return o;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Moments: the clip menu submenu and the Tracks popover apply presets; screenshot",
          "[app][moments][qml][screenshot]") {
    resetSettings();
    clearMessages();
    A11yScope a11y;
    Shell shell;
    REQUIRE(shell.window);
    shell.resize(kShotWidth, kShotHeight);
    shell.theme->setProperty("reduceMotion", true);
    shell.setAppearance(true, "ultraviolet");
    auto* controller = shell.engine.singletonInstance<ProjectController*>("Wabcut", "ProjectController");
    auto* moments = shell.engine.singletonInstance<MomentsBridge*>("Wabcut", "Moments");
    REQUIRE(controller);
    REQUIRE(moments);
    Timeline tl(picture());
    tl.linkMatching();
    controller->resetProject(tl.state, tl.seq);
    controller->setPlayhead(f(100));
    spin(200);
    const auto video = [&] { return controller->currentSequence()->findClip(tl.clip("V1@0"))->clip(); };
    const auto undo = [&] { return controller->session().undoCount(); };

    // The clip menu: Moments after Speed/Duration…, its presets on this clip.
    QObject* clipMenu = shell.window->findChild<QObject*>(QStringLiteral("clipContextMenu"));
    REQUIRE(clipMenu);
    QQuickItem* lanes = findNamed(shell.window->contentItem(), QStringLiteral("timelineLanesArea"));
    REQUIRE(lanes);
    QObject* submenu = clipMenu->property("momentsSubmenu").value<QObject*>();
    REQUIRE(submenu);
    CHECK(submenu->property("title").toString() == QStringLiteral("Moments"));
    // Beside it, the layout picker's entries (Fork L): Layout… in the clip menu, the Tracks header's button.
    CHECK(clipMenu->findChild<QObject*>(QStringLiteral("layoutMenuItem")));
    CHECK(findNamed(shell.window->contentItem(), QStringLiteral("timelineLayoutButton")));
    controller->selectClip(clipKey(tl.clip("V1@0")));
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, clipKey(tl.clip("V1@0"))), Q_ARG(QVariant, QPointF(200, 20))));
    CHECK(submenu->property("clipKey").toString() == clipKey(tl.clip("V1@0")));
    for (const char* name : {"momentsSlowMo25", "momentsSlowMo50", "momentsRampIn", "momentsFreezeReplay",
                             "momentsFastForward200", "momentsFastForward300", "momentsFastForward400", "momentsPunchIn",
                             "momentsPunchInHold", "momentsShake"}) {
        INFO(name);
        QObject* item = submenu->findChild<QObject*>(QString::fromLatin1(name));
        REQUIRE(item);
        CHECK(item->property("enabled").toBool());
    }
    std::size_t before = undo();
    REQUIRE(QMetaObject::invokeMethod(submenu->findChild<QObject*>(QStringLiteral("momentsPunchIn")), "triggered"));
    CHECK(undo() == before + 1);
    CHECK(submenu->property("error").toString().isEmpty());
    CHECK(keysOf(video(), ParamId::Scale).size() == 4);
    QMetaObject::invokeMethod(clipMenu, "close");
    // On a clip with sound only, nothing applies.
    REQUIRE(QMetaObject::invokeMethod(clipMenu, "openAt", Q_ARG(QVariant, QVariant::fromValue<QObject*>(lanes)),
                                      Q_ARG(QVariant, clipKey(tl.clip("A2@20"))), Q_ARG(QVariant, QPointF(200, 60))));
    CHECK_FALSE(submenu->findChild<QObject*>(QStringLiteral("momentsSlowMo25"))->property("enabled").toBool());
    CHECK_FALSE(submenu->findChild<QObject*>(QStringLiteral("momentsShake"))->property("enabled").toBool());
    QMetaObject::invokeMethod(clipMenu, "close");

    // The Tracks header's zap button opens the popover on the clip under the playhead.
    QQuickItem* panel = findNamed(shell.window->contentItem(), QStringLiteral("timelinePanel"));
    REQUIRE(panel);
    panel->setProperty("showSpeed", true);
    QQuickItem* button = findNamed(shell.window->contentItem(), QStringLiteral("momentsButton"));
    REQUIRE(button);
    CHECK(button->property("iconName").toString() == QStringLiteral("zap"));
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    QObject* popover = nullptr;
    REQUIRE(waitFor([&] {
        popover = findObject(button, QStringLiteral("momentsPopover"));
        return popover && popover->property("opened").toBool();
    }, 5000));
    CHECK(button->property("checked").toBool());
    auto* content = popover->property("contentItem").value<QQuickItem*>();
    REQUIRE(content);
    {
        const QStringList issues = auditAccessibility(content); // every preset and choice named, with a fitting role
        INFO(issues.join(QStringLiteral("; ")).toStdString());
        CHECK(issues.isEmpty());
    }
    QQuickItem* targetText = findNamed(content, QStringLiteral("momentsTargetText"));
    REQUIRE(targetText);
    CHECK(targetText->property("text").toString() ==
          QStringLiteral("A at %1").arg(controller->formatTimecode(f(100))));
    for (const char* name : {"momentsSlowMoButton", "momentsRampInButton", "momentsFreezeReplayButton",
                             "momentsFastForwardButton", "momentsPunchInButton", "momentsPunchInHoldButton",
                             "momentsShakeButton", "momentsSlowSpeed", "momentsSlowLength", "momentsFastSpeed"}) {
        INFO(name);
        QQuickItem* item = findNamed(content, QString::fromLatin1(name));
        REQUIRE(item);
        CHECK(item->property("enabled").toBool());
    }

    // 50 %, half a second: Slow-mo this moment.
    QQuickItem* speeds = findNamed(content, QStringLiteral("momentsSlowSpeed"));
    REQUIRE(QMetaObject::invokeMethod(speeds, "select", Q_ARG(QVariant, 1)));
    QQuickItem* lengths = findNamed(content, QStringLiteral("momentsSlowLength"));
    REQUIRE(QMetaObject::invokeMethod(lengths, "select", Q_ARG(QVariant, 0)));
    before = undo();
    REQUIRE(QMetaObject::invokeMethod(findNamed(content, QStringLiteral("momentsSlowMoButton")), "clicked"));
    CHECK(undo() == before + 1);
    CHECK(controller->undoText() == QStringLiteral("Undo Slow Motion"));
    Clip v = video();
    REQUIRE(v.isRemapped());
    CHECK(speedAt(v, ticksToFrames(controller->playhead(), kRate)) == Catch::Approx(50));
    // The slow stretch: 13 frames of footage at 50 %.
    const auto keys = keyFrames(v);
    REQUIRE(keys.size() == 8);
    CHECK(keys[4] - keys[3] == 26);
    CHECK(moments->message().isEmpty());

    // Zoomed so the ramp (about 3.4 s to 5.5 s) shows to the right of the popover.
    auto* view = panel->property("view").value<QObject*>();
    REQUIRE(view);
    view->setProperty("pixelsPerSecond", 220.0);
    view->setProperty("scrollX", 0.0);
    spin(300);
    shell.grab(); // lets the timeline draw the ramp before the screenshot frame
    spin(100);
    const QImage image = shell.grab();
    REQUIRE_FALSE(image.isNull());
    const double dpr = image.devicePixelRatio();
    const QRectF area = panel->mapRectToScene(QRectF(0, 0, panel->width(), panel->height()))
                            .intersected(QRectF(0, 0, image.width() / dpr, image.height() / dpr));
    REQUIRE(image.copy(QRectF(area.topLeft() * dpr, area.size() * dpr).toRect())
                .save(QDir(outputDir()).filePath(QStringLiteral("moments-presets.png"))));

    // Shake from the popover; the button closes it again.
    before = undo();
    REQUIRE(QMetaObject::invokeMethod(findNamed(content, QStringLiteral("momentsShakeButton")), "clicked"));
    CHECK(undo() == before + 1);
    REQUIRE(QMetaObject::invokeMethod(button, "clicked"));
    REQUIRE(waitFor([&] { return !popover->property("visible").toBool(); }, 5000));
    const QStringList messages = takeMessages();
    INFO(join(messages));
    CHECK(messages.isEmpty());
}

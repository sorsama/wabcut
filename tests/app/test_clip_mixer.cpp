// p02 audio: Premiere's Audio Clip Mixer (ClipMixerBridge). Offscreen; no window is shown.
//   * one strip per audio track for the clip under the playhead, following the playhead;
//   * paused, the fader and pan set the clip's Volume and Panner (one undo step per drag; the key at the playhead on an
//     animated clip);
//   * Write Keyframes: while playing, the held fader writes keyframes into the clip, committed at stop as one undo
//     step, and is heard at once through the track's live override.

#include "TimelineHarness.h"

#include "BridgeIds.h"
#include "ClipMixerBridge.h"
#include "ProjectController.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <map>
#include <optional>

using namespace wabcut;
using namespace wabcut::app;
using namespace wabcut::test;

namespace {

Ticks seconds(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

struct ClipRig {
    // 25 fps: A1 has two clips (0-2 s, 2-4 s), A2 a gap first.
    Timeline tl{R"(
        V1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A1 tgt |AaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaBbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb|
        A2     |..........................Mmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmmm|
    )"};
    ProjectController controller;
    ClipMixerBridge mixer;
    std::map<std::uint64_t, std::optional<double>> live; ///< what the live override was told, per track

    ClipRig() {
        controller.resetProject(tl.state, tl.seq);
        mixer.setController(&controller);
        mixer.setLiveMixSink([this](TrackId track, std::optional<double> gainDb) {
            live[track.value] = gainDb;
            return true;
        });
    }
    QString a1() const { return trackKey(tl.track("A1")); }
    QString a2() const { return trackKey(tl.track("A2")); }
    QVariantMap strip(int i) const { return mixer.strips().at(i).toMap(); }
    const Clip& clip(const char* ref) {
        return controller.currentSequence()->findClip(clipAt(*controller.session().snapshot(), tl.seq, ref))->clip();
    }
    std::size_t undo() { return controller.session().undoCount(); }
};

} // namespace

TEST_CASE("Audio Clip Mixer strips follow the clip under the playhead", "[app][mixer][clipmixer]") {
    ClipRig rig;
    REQUIRE(rig.mixer.strips().size() == 2);
    rig.controller.setPlayhead(seconds(0.5));
    CHECK(rig.strip(0).value(QStringLiteral("clip")).toString() == clipKey(rig.clip("A1@0").id));
    CHECK(rig.strip(1).value(QStringLiteral("clip")).toString().isEmpty()); // A2 has a gap there
    rig.controller.setPlayhead(seconds(3.0));
    CHECK(rig.strip(0).value(QStringLiteral("clip")).toString() == clipKey(rig.clip("A1@50").id));
    CHECK_FALSE(rig.strip(1).value(QStringLiteral("clip")).toString().isEmpty());
}

TEST_CASE("Audio Clip Mixer: paused, the fader and pan set the clip's volume and pan, one undo step per drag",
          "[app][mixer][clipmixer]") {
    ClipRig rig;
    rig.controller.setPlayhead(seconds(1.0));
    const std::size_t undo = rig.undo();
    for (double db = 0; db >= -9; db -= 1.5)
        REQUIRE(rig.mixer.setGainDb(rig.a1(), db).isEmpty());
    rig.mixer.endGesture();
    CHECK(rig.undo() == undo + 1);
    CHECK(rig.clip("A1@0").properties()[ParamId::VolumeDb] == ParamValue(-9.0));
    CHECK(rig.strip(0).value(QStringLiteral("gainDb")).toDouble() == Catch::Approx(-9.0));
    REQUIRE(rig.mixer.setPan(rig.a1(), -0.5).isEmpty());
    rig.mixer.endGesture();
    CHECK(rig.clip("A1@0").properties()[ParamId::Pan] == ParamValue(-0.5));
    // The other clip of A1 is untouched; a track with no clip there says so.
    CHECK(rig.clip("A1@50").properties()[ParamId::VolumeDb] == ParamValue(0.0));
    CHECK_FALSE(rig.mixer.setGainDb(rig.a2(), -3).isEmpty());
    CHECK(rig.mixer.resetGain(rig.a1()).isEmpty());
    CHECK(rig.clip("A1@0").properties()[ParamId::VolumeDb] == ParamValue(0.0));
}

TEST_CASE("Audio Clip Mixer: Write Keyframes records the held fader into the clip while playing, one undo step",
          "[app][mixer][clipmixer][automation]") {
    ClipRig rig;
    rig.mixer.setWriteKeyframes(rig.a1(), true);
    CHECK(rig.strip(0).value(QStringLiteral("writeKeyframes")).toBool());
    const std::size_t undo = rig.undo();
    rig.controller.setPlayhead(seconds(0.2));
    rig.mixer.setPlayingForTests(true);
    for (double t = 0.2; t <= 1.6 + 1e-9; t += 0.04) {
        rig.controller.setPlayhead(seconds(t));
        if (std::abs(t - 0.6) < 0.02)
            rig.mixer.touch(rig.a1(), QStringLiteral("volume"), true);
        if (t > 0.59 && t < 1.21)
            REQUIRE(rig.mixer.setGainDb(rig.a1(), -12.0 * (t - 0.6) / 0.6).isEmpty());
        if (std::abs(t - 1.2) < 0.02)
            rig.mixer.touch(rig.a1(), QStringLiteral("volume"), false);
        rig.mixer.pollWrite();
        if (std::abs(t - 1.0) < 0.02) {
            // Heard at once: the track's live override is the fader plus the clip's change (-8 dB here).
            REQUIRE(rig.live.contains(rig.tl.track("A1").value));
            REQUIRE(rig.live[rig.tl.track("A1").value].has_value());
            CHECK(*rig.live[rig.tl.track("A1").value] == Catch::Approx(-8.0).margin(0.5));
        }
    }
    rig.mixer.setPlayingForTests(false);
    CHECK(rig.undo() == undo + 1);
    const ParamValue& volume = rig.clip("A1@0").properties()[ParamId::VolumeDb];
    REQUIRE(volume.isCurve());
    const Clip& clip = rig.clip("A1@0");
    CHECK(clip.parameterAt(ParamId::VolumeDb, seconds(0.3)) == Catch::Approx(0.0).margin(0.1));
    CHECK(clip.parameterAt(ParamId::VolumeDb, seconds(1.2)) == Catch::Approx(-12.0).margin(0.6));
    CHECK(clip.parameterAt(ParamId::VolumeDb, seconds(1.5)) == Catch::Approx(0.0).margin(0.1));
    // The override is released when the pass ends.
    CHECK_FALSE(rig.live[rig.tl.track("A1").value].has_value());
}

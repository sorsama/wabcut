// The auto-ducking planner: regions, the envelope and the keyframes it writes on synthetic dialogue and music.

#include "TimelineFixture.h"

#include <wabcut/audio/edit/Ducking.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace wabcut;
using namespace wabcut::audio::edit;
using Catch::Approx;

namespace {

Ticks s(double seconds) { return Ticks{static_cast<std::int64_t>(seconds * static_cast<double>(kTicksPerSecond))}; }
TimeRange span(double from, double to) { return TimeRange::fromStartEnd(s(from), s(to)); }

DuckingOptions options() {
    DuckingOptions o;
    o.amountDb = -12.0;
    o.attack = s(0.2);
    o.release = s(0.5);
    o.hold = s(0.3);
    o.minSpeech = s(0.1);
    return o;
}

} // namespace

TEST_CASE("Duck regions bridge short pauses and drop blips", "[audio][edit][duck]") {
    const DuckingOptions o = options();
    // Phrases at 2-3 s and 3.5-4 s (the music would come back up at 3.8 s but has to go down again at 3.3 s: one
    // region), 8-9 s (far away: its own region), and a 50 ms blip at 6 s (ignored).
    const std::vector<TimeRange> speech{span(8.0, 9.0), span(2.0, 3.0), span(3.5, 4.0), span(6.0, 6.05)};
    const auto regions = duckRegions(speech, o);
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].speech == span(2.0, 4.0));
    CHECK(regions[0].downStart == s(1.8));
    CHECK(regions[0].upStart == s(4.3));
    CHECK(regions[0].upEnd == s(4.8));
    CHECK(regions[1].speech == span(8.0, 9.0));

    // The envelope: 0 dB, linear ramp down over the attack, -12 dB under speech and the hold, linear ramp up.
    CHECK(duckOffsetDb(regions, o, s(1.0)) == 0.0);
    CHECK(duckOffsetDb(regions, o, s(1.9)) == Approx(-6.0));
    CHECK(duckOffsetDb(regions, o, s(2.0)) == -12.0);
    CHECK(duckOffsetDb(regions, o, s(3.25)) == -12.0); // the bridged pause stays down
    CHECK(duckOffsetDb(regions, o, s(4.2)) == -12.0);  // the hold
    CHECK(duckOffsetDb(regions, o, s(4.55)) == Approx(-6.0));
    CHECK(duckOffsetDb(regions, o, s(4.8)) == 0.0);
    CHECK(duckOffsetDb(regions, o, s(6.02)) == 0.0); // the blip did nothing

    // A gap longer than attack + release after the hold keeps the regions apart.
    const auto apart = duckRegions(std::vector<TimeRange>{span(2.0, 3.0), span(4.1, 5.0)}, o);
    CHECK(apart.size() == 2);
}

TEST_CASE("Ducked volume keyframes sit at the envelope corners in curve time", "[audio][edit][duck]") {
    // 100 frames (4 s at 25 fps) of media M from source frame 100 (4 s in); the digits are frames of the clip too.
    test::Timeline tl("A1 music |M100" + std::string(96, 'm') + "|");
    const Clip& music = tl.sequence().audioTracks[0]->clips[0]; // 4 s from source frame 100 (4 s in)
    const DuckingOptions o = options();
    const auto regions = duckRegions(std::vector<TimeRange>{span(1.0, 2.0)}, o);

    SECTION("a constant level") {
        ClipProperties props = music.properties();
        props.set(ParamId::VolumeDb, -3.0);
        Clip c = music;
        c.setProperties(props);
        const auto v = duckedVolume(c, regions, o);
        REQUIRE(v);
        REQUIRE(v->isCurve());
        const auto& keys = v->curve()->keys;
        // Clip start, down start, speech start, up start, up end, clip end.
        REQUIRE(keys.size() == 6);
        const Ticks in = c.sourceIn;
        CHECK(keys[0].time == in);
        CHECK(keys[1].time == in + s(0.8));
        CHECK(keys[2].time == in + s(1.0));
        CHECK(keys[3].time == in + s(2.3));
        CHECK(keys[4].time == in + s(2.8));
        CHECK(keys[5].time == in + s(4.0));
        CHECK(keys[0].value == -3.0);
        CHECK(keys[1].value == -3.0);
        CHECK(keys[2].value == -15.0);
        CHECK(keys[3].value == -15.0);
        CHECK(keys[4].value == -3.0);
        CHECK(keys[5].value == -3.0);
        for (const Keyframe& k : keys) {
            CHECK(k.interpolation == Interpolation::Linear);
        }
        // The curve evaluates to level + duck at any timeline time.
        for (const double t : {0.5, 0.9, 1.5, 2.5, 3.5}) {
            CHECK(v->valueAt(c.curveTimeAt(s(t))) == Approx(-3.0 + duckOffsetDb(regions, o, s(t))));
        }
    }
    SECTION("a range keeps the keys outside it; no speech means the plain level") {
        ClipProperties props = music.properties();
        const Ticks in = music.sourceIn;
        props[ParamId::VolumeDb] = ParamValue(Curve{{Keyframe{in, -60.0}, Keyframe{in + s(0.5), 0.0}, Keyframe{in + s(3.5), 0.0},
                                                     Keyframe{in + s(4.0), -60.0}}});
        Clip c = music;
        c.setProperties(props);
        const auto v = duckedVolume(c, regions, o, span(0.6, 3.4));
        REQUIRE(v);
        const auto& keys = v->curve()->keys;
        CHECK(keys.front().time == in);           // the fade-in kept
        CHECK(keys.front().value == -60.0);
        CHECK(keys.back().time == in + s(4.0));   // the fade-out kept
        CHECK(v->valueAt(c.curveTimeAt(s(1.5))) == Approx(-12.0));
        CHECK(v->valueAt(c.curveTimeAt(s(0.55))) == Approx(0.0).margin(1e-9)); // the old fade's top

        const auto quiet = duckedVolume(c, {}, o, std::nullopt);
        REQUIRE(quiet);
        CHECK(quiet->isConstant());
        CHECK(quiet->constant() == -60.0); // the level where the (whole-clip) range starts
        CHECK_FALSE(duckedVolume(c, regions, o, span(10.0, 11.0)));
    }
}

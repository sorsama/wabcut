// AI-14 matte redactions in evaluation (render/Matte.h): a redaction whose region is an AI object matte asks the matte
// source for the matte at the shown media time, as a redaction (fail closed). Where the matte is known the layer's
// redaction covers its shape; where it is not (outside the propagated span, the cache cleared) it covers the region's
// bounding box grown by a quarter: never nothing. Effect masks ask as effects.

#include "PlaybackTestSupport.h"

#include <wabcut/model/Effects.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/render/Mask.h>
#include <wabcut/render/Matte.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <mutex>

using namespace wabcut;
using namespace wabcut::playback;

namespace {

Ticks sec(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

/// A matte (a disc at (0.5, 0.5), radius 0.1 frame heights) for media times before 2 s; none after.
class FirstTwoSeconds final : public render::MatteSource {
public:
    std::shared_ptr<const render::MatteField> fieldAt(const MaskMatte&, Ticks mediaTime, double aspect, render::MatteUse use) override {
        std::lock_guard lock(mutex);
        uses.push_back(use);
        times.push_back(mediaTime);
        if (mediaTime >= sec(2)) {
            return nullptr;
        }
        auto f = std::make_shared<render::MatteField>();
        f->width = 64;
        f->height = 36;
        f->identity = 7;
        f->distance.resize(64 * 36);
        for (int y = 0; y < 36; ++y) {
            for (int x = 0; x < 64; ++x) {
                const double px = (x + 0.5) / 64 * aspect, py = (y + 0.5) / 36;
                f->distance[static_cast<std::size_t>(y) * 64 + x] = static_cast<float>(0.1 - std::hypot(px - 0.5 * aspect, py - 0.5));
            }
        }
        return f;
    }
    std::mutex mutex;
    std::vector<render::MatteUse> uses;
    std::vector<Ticks> times;
};

struct Installed {
    explicit Installed(std::shared_ptr<render::MatteSource> s) { render::setMatteSource(std::move(s)); }
    ~Installed() { render::setMatteSource(nullptr); }
};

Mask matteRegion() {
    Mask m;
    m.shape = MaskShape::Matte;
    m.matte = MaskMatte{"track", MediaId{1}, 0, TimeRange{Ticks{}, sec(4)}, "test"};
    m.set("center.x", 0.5);
    m.set("center.y", 0.5);
    m.set("size.x", 0.2 * 9.0 / 16.0);
    m.set("size.y", 0.2);
    return m;
}

ProjectState project(Clip& clip, MediaId& media) {
    ProjectState s;
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = 1920;
    v.height = 1080;
    v.frameRate = kRate60;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    media = item->id;
    s.media = {item};
    Redaction r;
    r.media = media;
    r.source = TimeRange{Ticks{}, sec(4)};
    r.style = RedactionStyle::Solid;
    r.region = matteRegion();
    r.region.matte->media = media;
    auto added = edit::addRedaction(s, r);
    REQUIRE(added);
    s = std::move(added->state);
    clip.id = s.ids.allocate<ClipId>();
    clip.media = media;
    clip.timelineRange = {Ticks{}, sec(4)};
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList({clip});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.duration = sec(4);
    seq.videoTracks = {std::make_shared<const Track>(std::move(t))};
    s.sequences = {std::make_shared<const Sequence>(std::move(seq))};
    return s;
}

double at(const render::MaskSpec& spec, double u, double v) {
    constexpr int kW = 320, kH = 180;
    const std::vector<float> c = render::maskCoverageCpu(spec, kW, kH);
    return c[static_cast<std::size_t>(v * kH) * kW + static_cast<std::size_t>(u * kW)];
}

} // namespace

TEST_CASE("Matte redactions: the matte where it is known, the grown bounding box where it is not (fail closed)",
          "[playback][evaluate][redaction][matte]") {
    auto mattes = std::make_shared<FirstTwoSeconds>();
    Installed installed(mattes);
    Clip clip;
    MediaId media;
    const ProjectState s = project(clip, media);

    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, *s.sequences.front(), sec(1), layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].redactions.size() == 1);
    {
        const render::MaskSpec& spec = *layers[0].redactions[0].mask;
        REQUIRE(spec.shapes.size() == 1);
        REQUIRE(spec.shapes[0].matte);                // the matte's own shape
        CHECK(at(spec, 0.5, 0.5) == 1.0);
        CHECK(at(spec, 0.5 + 0.09 * 9 / 16, 0.5 + 0.09) < 0.01); // the box's corner: outside the disc
        std::lock_guard lock(mattes->mutex);
        REQUIRE_FALSE(mattes->uses.empty());
        CHECK(std::ranges::all_of(mattes->uses, [](render::MatteUse u) { return u == render::MatteUse::Redaction; }));
        CHECK(mattes->times.back() >= sec(1) - sec(1.0 / 60));
        CHECK(mattes->times.back() <= sec(1) + sec(1.0 / 60));
    }

    // At 3 s the matte is missing: the redaction covers the bounding box grown by a quarter.
    layers.clear();
    evaluateVideoLayers(s, *s.sequences.front(), sec(3), layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].redactions.size() == 1);
    const render::MaskSpec& spec = *layers[0].redactions[0].mask;
    REQUIRE(spec.shapes.size() == 1);
    CHECK_FALSE(spec.shapes[0].matte);
    CHECK(at(spec, 0.5, 0.5) == 1.0);
    CHECK(at(spec, 0.5 + 0.09 * 9 / 16, 0.5 + 0.09) == 1.0); // the box's corner
    CHECK(at(spec, 0.5, 0.5 + 0.11) == 1.0);                 // past the box's edge: grown
    CHECK(at(spec, 0.5, 0.9) == 0.0);                        // far away: clear

    // Without any matte source (wabcut-cli without the AI libraries, a test host): the same grown box.
    render::setMatteSource(nullptr);
    layers.clear();
    evaluateVideoLayers(s, *s.sequences.front(), sec(1), layers);
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].redactions.size() == 1);
    CHECK(at(*layers[0].redactions[0].mask, 0.5, 0.5 + 0.11) == 1.0);
}

// AI-14 review, finding 2: an effect's matte mask was looked up at the clip's CURVE time, which after a later freeze
// frame (or a time remap) runs on with the timeline instead of staying on the media frame shown: the matte of another
// frame was drawn. Mattes are in media time: they are now looked up at the media time the layer shows.
TEST_CASE("Effect mattes follow the media time of a frozen clip", "[playback][evaluate][matte]") {
    auto mattes = std::make_shared<FirstTwoSeconds>();
    Installed installed(mattes);
    ProjectState s;
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = 1920;
    v.height = 1080;
    v.frameRate = kRate60;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    s.media = {item};
    Clip clip;
    clip.id = s.ids.allocate<ClipId>();
    clip.media = item->id;
    clip.timelineRange = {Ticks{}, sec(4)};
    clip.sourceIn = sec(0.5);
    clip.speed = Speed{0, 1}; // a frame hold on media 0.5 s for 4 s
    EffectInstance blur;
    blur.id = s.ids.allocate<EffectInstanceId>();
    blur.effect = "wabcut.blur.gaussian";
    blur.set("radius", 20.0);
    Mask region = matteRegion();
    region.id = 1;
    region.matte->media = item->id;
    blur.masks = {region};
    clip.setEffects({blur});
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList({clip});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.duration = sec(4);
    seq.videoTracks = {std::make_shared<const Track>(std::move(t))};
    s.sequences = {std::make_shared<const Sequence>(std::move(seq))};

    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, *s.sequences.front(), sec(3), layers); // 3 s into the hold: still media 0.5 s on screen
    REQUIRE(layers.size() == 1);
    REQUIRE(layers[0].effects.size() == 1);
    REQUIRE(layers[0].effects[0].mask);
    std::lock_guard lock(mattes->mutex);
    REQUIRE_FALSE(mattes->times.empty());
    CHECK(mattes->uses.back() == render::MatteUse::Effect);
    CHECK(mattes->times.back() == sec(0.5)); // not 0.5 s + 3 s of curve time (a frame past the matte, none at all)
    CHECK(layers[0].effects[0].mask->shapes.front().matte); // so the effect has the held frame's matte
}

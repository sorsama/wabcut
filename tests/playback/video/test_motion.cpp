// P.2 motion features in evaluation, the preview and export: Time Interpolation's groups (the two frames around the
// source time, mixed by its place between their PTS), Morph Cut's frozen sides and their motion requests, then end to
// end over synthetic moving-texture media (FFV1, lossless) where the true in-between picture is known: Optical Flow
// beats Frame Blending which beats Frame Sampling for 24 fps footage in a 48 fps sequence, a Morph Cut's middle frame
// beats a cross dissolve on a shifted and zoomed second shot, the program monitor shows what export renders, and
// export is deterministic.

#include "PlaybackTestSupport.h"
#include "../../motion/MovingTexture.h"

#include <wabcut/editor/PreviewRender.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/media/Probe.h>
#include <wabcut/media/VideoDecoder.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditingOps.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MotionOps.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/motion/Geometry.h>
#include <wabcut/playback/video/Evaluate.h>
#include <wabcut/playback/video/MotionAnalysis.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;
using Catch::Approx;
namespace fs = std::filesystem;

namespace {

// ---- Fake media (evaluation only: nothing is decoded) ------------------------------------------------------------

std::shared_ptr<MediaItem> fakeMedia(ProjectState& s, Rate rate, int w = 1920, int h = 1080) {
    auto item = std::make_shared<MediaItem>();
    item->id = s.ids.allocate<MediaId>();
    item->locator.path = "fake.mp4";
    VideoStreamInfo v;
    v.index = 0;
    v.width = w;
    v.height = h;
    v.frameRate = rate;
    v.duration = Ticks::fromSeconds(100);
    item->info.video.push_back(v);
    item->info.duration = v.duration;
    return item;
}

Clip videoClip(ProjectState& s, MediaId media, Ticks start, Ticks duration, Ticks sourceIn) {
    Clip c;
    c.id = s.ids.allocate<ClipId>();
    c.media = media;
    c.timelineRange = {start, duration};
    c.sourceIn = sourceIn;
    return c;
}

TrackPtr trackOf(ProjectState& s, std::vector<Clip> clips, std::vector<Transition> transitions = {}) {
    Track t;
    t.id = s.ids.allocate<TrackId>();
    t.clips = ClipList(std::move(clips));
    t.transitions = std::move(transitions);
    return std::make_shared<const Track>(std::move(t));
}

/// A CPU frame (RGBA8, w x h, one grey) covering [pts, pts + duration).
media::VideoFrame cpuFrame(Ticks pts, Ticks duration, int w = 8, int h = 8, std::uint8_t grey = 128) {
    auto pixels = std::make_shared<std::vector<std::uint8_t>>(static_cast<std::size_t>(w) * h * 4, grey);
    media::FrameHandle f;
    f.pts = pts;
    f.duration = duration;
    f.width = w;
    f.height = h;
    f.format = media::PixelFormat::RGBA8;
    media::CpuPlanes cpu;
    cpu.planes[0] = media::Plane{pixels->data(), w * 4, w, h};
    cpu.owner = std::shared_ptr<const void>(pixels, pixels->data());
    f.payload = std::move(cpu);
    return f;
}

class FakeMotion final : public MotionSource {
public:
    std::shared_ptr<const motion::FlowPair> motion(const MotionRequest& request) override {
        asked.push_back(request);
        auto pair = std::make_shared<motion::FlowPair>();
        pair->width = 2;
        pair->height = 2;
        pair->data.assign(16, 0.0f);
        return pair;
    }
    std::vector<MotionRequest> asked;
};

// ---- Real synthetic media ------------------------------------------------------------------------------------------

fs::path motionDir() {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "playback" / "motion";
    fs::create_directories(dir);
    return dir;
}

/// Encodes grey pictures losslessly (FFV1 in Matroska, yuv420p) at `rate` with the vcpkg ffmpeg.
bool encodeGrey(const fs::path& out, const std::vector<motion::Plane>& frames, Rate rate) {
    if (frames.empty()) {
        return false;
    }
    const fs::path raw = fs::path(out).replace_extension(".grey");
    {
        std::ofstream file(raw, std::ios::binary);
        for (const motion::Plane& p : frames) {
            for (const float v : p.px) {
                const auto b = static_cast<char>(static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)));
                file.put(b);
            }
        }
    }
    const std::string command = std::format(
        "\"\"{}\" -y -loglevel error -f rawvideo -pix_fmt gray -s {}x{} -r {}/{} -i \"{}\" -c:v ffv1 -pix_fmt yuv420p \"{}\"\"",
        WABCUT_FFMPEG_EXECUTABLE, frames.front().width, frames.front().height, rate.num, rate.den, raw.string(), out.string());
    const int code = std::system(command.c_str());
    fs::remove(raw);
    return code == 0 && fs::exists(out);
}

/// A probed media item of `file`, added to `state`.
MediaId addFile(ProjectState& state, const fs::path& file) {
    auto info = media::probe(file.string());
    REQUIRE(info);
    auto item = std::make_shared<MediaItem>();
    item->id = state.ids.allocate<MediaId>();
    item->name = file.filename().string();
    item->locator.path = file.string();
    item->info = *info;
    state.media.push_back(item);
    std::sort(state.media.begin(), state.media.end(), [](const MediaPtr& a, const MediaPtr& b) { return a->id < b->id; });
    return item->id;
}

struct MotionProject {
    ProjectState state;
    SequenceId sequence;
};

/// A 320 x 180 sequence at `rate` with `track` on V1.
MotionProject projectWith(ProjectState state, Rate rate, std::vector<Clip> clips, std::vector<Transition> transitions = {}) {
    MotionProject p;
    auto seq = std::make_shared<Sequence>();
    seq->id = state.ids.allocate<SequenceId>();
    seq->name = "Motion";
    seq->width = 320;
    seq->height = 180;
    seq->frameRate = rate;
    seq->videoTracks = {trackOf(state, std::move(clips), std::move(transitions))};
    seq->duration = Ticks::fromSeconds(2);
    p.sequence = seq->id;
    state.sequences.push_back(std::move(seq));
    p.state = std::move(state);
    return p;
}

/// Export's picture of frame `index` (RGBA8, 320 x 180) through the production rendered source.
std::vector<std::uint8_t> exportFrame(const MotionProject& p, Rate rate, std::int64_t index) {
    const Sequence& seq = *p.state.findSequence(p.sequence);
    auto source = exporter::renderedVideoSourceFactory({})(p.state, seq,
                                                           exporter::VideoSourceFormat{320, 180, rate, Ticks{}, index + 1});
    REQUIRE(source);
    REQUIRE((*source)->request(index));
    auto frame = (*source)->next();
    REQUIRE(frame);
    const media::CpuPlanes* cpu = frame->cpu();
    REQUIRE(cpu);
    std::vector<std::uint8_t> rgba(320 * 180 * 4);
    for (int y = 0; y < 180; ++y) {
        std::memcpy(rgba.data() + static_cast<std::size_t>(y) * 320 * 4, cpu->planes[0].data + y * cpu->planes[0].stride, 320 * 4);
    }
    return rgba;
}

/// PSNR of the luma of two RGBA8 pictures over their inner 80 %.
double lumaPsnr(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b, int w = 320, int h = 180) {
    REQUIRE(a.size() == b.size());
    double se = 0.0;
    int n = 0;
    for (int y = h / 10; y < h - h / 10; ++y) {
        for (int x = w / 10; x < w - w / 10; ++x) {
            const std::size_t i = 4 * (static_cast<std::size_t>(y) * w + x);
            const double la = 0.2126 * a[i] + 0.7152 * a[i + 1] + 0.0722 * a[i + 2];
            const double lb = 0.2126 * b[i] + 0.7152 * b[i + 1] + 0.0722 * b[i + 2];
            se += (la - lb) * (la - lb);
            ++n;
        }
    }
    const double mse = se / n;
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// The texture moving 3 px right and 1.5 px down per 1/48 s: frame k of the 48 fps truth.
motion::Plane movingFrame(const motion::test::MovingTexture& t, int k) { return t.picture(320, 180, 3.0 * k, 1.5 * k); }

struct ConformMedia {
    fs::path at24; ///< every other picture at 24 fps
    fs::path at48; ///< every picture at 48 fps: the truth between the 24 fps frames
};

const ConformMedia& conformMedia() {
    static const ConformMedia media = [] {
        const motion::test::MovingTexture texture(31);
        std::vector<motion::Plane> all;
        for (int k = 0; k < 48; ++k) {
            all.push_back(movingFrame(texture, k));
        }
        std::vector<motion::Plane> even;
        for (int k = 0; k < 48; k += 2) {
            even.push_back(all[static_cast<std::size_t>(k)]);
        }
        ConformMedia m{motionDir() / "texture24.mkv", motionDir() / "texture48.mkv"};
        REQUIRE(encodeGrey(m.at24, even, Rate{24, 1}));
        REQUIRE(encodeGrey(m.at48, all, Rate{48, 1}));
        return m;
    }();
    return media;
}

/// 24 fps footage on V1 of a 48 fps sequence, 40 frames from its start, with `mode`.
MotionProject conformProject(TimeInterpolation mode) {
    ProjectState state;
    const MediaId media = addFile(state, conformMedia().at24);
    Clip clip = videoClip(state, media, Ticks{}, framesToTicks(40, Rate{48, 1}), Ticks{});
    ClipContent content;
    content.timeInterpolation = mode;
    clip.setContent(std::move(content));
    return projectWith(std::move(state), Rate{48, 1}, {clip});
}

/// `file` on V1 of a `sequenceRate` sequence, 20 frames from its start, with `mode`, then interpreted at `interpreted`
/// (Interpret Footage, EditingOps.h: the clip plays at its conform speed and grows into the gap after it).
MotionProject interpretedProject(const fs::path& file, Rate interpreted, Rate sequenceRate, TimeInterpolation mode) {
    ProjectState state;
    const MediaId media = addFile(state, file);
    Clip clip = videoClip(state, media, Ticks{}, framesToTicks(20, sequenceRate), Ticks{});
    ClipContent content;
    content.timeInterpolation = mode;
    clip.setContent(std::move(content));
    MotionProject p = projectWith(std::move(state), sequenceRate, {clip});
    MediaInterpretation in;
    in.frameRate = interpreted;
    auto r = edit::interpretFootage(p.state, std::span(&media, 1), in);
    REQUIRE(r);
    p.state = std::move(r->state);
    return p;
}

} // namespace

// ---- Evaluation ---------------------------------------------------------------------------------------------------

TEST_CASE("Time Interpolation makes a group of the two frames around the source time", "[playback][motion]") {
    ProjectState s;
    auto media = fakeMedia(s, kRate30);
    s.media = {media};
    Clip clip = videoClip(s, media->id, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.width = 1920;
    seq.height = 1080;
    for (const TimeInterpolation mode : {TimeInterpolation::FrameBlending, TimeInterpolation::OpticalFlow}) {
        ClipContent content;
        content.timeInterpolation = mode;
        clip.setContent(content);
        seq.videoTracks = {trackOf(s, {clip})};
        std::vector<LayerSpec> layers;
        // 30 fps footage at 60 fps: sequence frame 1 shows source time 1/60 s, half way between frames 0 and 1.
        evaluateVideoLayers(s, seq, framesToTicks(1, kRate60), layers);
        REQUIRE(layers.size() == 3);
        CHECK(layers[0].nested);
        CHECK(layers[0].interpolation == mode);
        CHECK(layers[0].nestedCount == 2);
        CHECK(layers[0].nestedCanvas == render::CanvasSize{1920, 1080});
        CHECK(layers[0].sourceTime == framesToTicks(1, kRate60));
        CHECK_FALSE(needsDecode(layers[0]));
        CHECK(layers[1].sourceTime == framesToTicks(1, kRate60));
        CHECK(layers[2].interpolationSecond);
        CHECK(layers[2].sourceTime == framesToTicks(1, kRate60) + framesToTicks(1, kRate30));
        CHECK(sessionKey(layers[1]) == sessionKey(layers[2]));
        CHECK(needsDecode(layers[1]));
        CHECK(needsDecode(layers[2]));
    }
    // Frame Sampling (the default), or footage at the sequence's rate and speed: the plain layer.
    ClipContent sampling;
    clip.setContent(sampling);
    seq.videoTracks = {trackOf(s, {clip})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, framesToTicks(1, kRate60), layers);
    REQUIRE(layers.size() == 1);
    CHECK_FALSE(layers[0].interpolation);
    auto same = fakeMedia(s, kRate60);
    s.media = {media, same};
    Clip unretimed = videoClip(s, same->id, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    ClipContent flow;
    flow.timeInterpolation = TimeInterpolation::OpticalFlow;
    unretimed.setContent(flow);
    seq.videoTracks = {trackOf(s, {unretimed})};
    evaluateVideoLayers(s, seq, framesToTicks(1, kRate60), layers);
    REQUIRE(layers.size() == 1);
    CHECK_FALSE(layers[0].interpolation);
}

TEST_CASE("An interpolation group mixes its frames by the source time's place between them", "[playback][motion]") {
    ProjectState s;
    auto media = fakeMedia(s, kRate30, 8, 8);
    s.media = {media};
    Clip clip = videoClip(s, media->id, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    ClipContent content;
    content.timeInterpolation = TimeInterpolation::OpticalFlow;
    clip.setContent(content);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.width = 8;
    seq.height = 8;
    seq.videoTracks = {trackOf(s, {clip})};
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, framesToTicks(1, kRate60), layers);
    REQUIRE(layers.size() == 3);
    const Ticks period = framesToTicks(1, kRate30);

    // Half way: a flow mix at 0.5 along the source's motion.
    std::vector<media::VideoFrame> frames{{}, cpuFrame(Ticks{}, period), cpuFrame(period, period)};
    const std::vector<MotionRequest> requests = motionRequestsOf(layers, frames, {8, 8});
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].a.time == Ticks{});
    CHECK(requests[0].b.time == period);
    CHECK(requests[0].canvas == render::CanvasSize{8, 8});
    CHECK(requests[0].a.transform == render::LayerTransform{}); // the frame fills the group
    FakeMotion motionSource;
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out, &motionSource, {8, 8});
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].nested);
    REQUIRE(out[0].nested->layers.size() == 2);
    REQUIRE(out[0].nested->layers[0].transition);
    CHECK(out[0].nested->layers[0].transition->kind == render::TransitionKind::FlowMorph);
    CHECK(out[0].nested->layers[0].transition->progress == Approx(0.5));
    CHECK(out[0].nested->layers[0].transition->motion != nullptr);
    CHECK(motionSource.asked.size() == 1);

    // Without motion: a cross dissolve at the same place.
    frames = {{}, cpuFrame(Ticks{}, period), cpuFrame(period, period)};
    out.clear();
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].nested->layers.size() == 2);
    CHECK(out[0].nested->layers[0].transition->kind == render::TransitionKind::CrossDissolve);

    // On a frame (the source time at its PTS): that frame alone, no motion asked for.
    frames = {{}, cpuFrame(framesToTicks(1, kRate60), period), cpuFrame(framesToTicks(1, kRate60) + period, period)};
    CHECK(motionRequestsOf(layers, frames, {8, 8}).empty());
    out.clear();
    buildCompositeLayers(layers, frames, out, &motionSource, {8, 8});
    REQUIRE(out.size() == 1);
    CHECK(out[0].nested->layers.size() == 1);
    // Past the media's end (no second frame): the first one alone.
    frames = {{}, cpuFrame(Ticks{}, period), {}};
    out.clear();
    buildCompositeLayers(layers, frames, out, &motionSource, {8, 8});
    REQUIRE(out.size() == 1);
    CHECK(out[0].nested->layers.size() == 1);
    CHECK(interpolationWeight(Ticks{period.value / 4}, cpuFrame(Ticks{}, period), cpuFrame(period, period)) == Approx(0.25));
}

TEST_CASE("Morph Cut freezes each side at the transition's end it touches", "[playback][motion]") {
    ProjectState s;
    auto media = fakeMedia(s, kRate60);
    s.media = {media};
    Clip a = videoClip(s, media->id, Ticks{}, framesToTicks(60, kRate60), framesToTicks(100, kRate60));
    Clip b = videoClip(s, media->id, framesToTicks(60, kRate60), framesToTicks(60, kRate60), framesToTicks(300, kRate60));
    Transition morph;
    morph.id = s.ids.allocate<TransitionId>();
    morph.outgoing = a.id;
    morph.incoming = b.id;
    morph.before = framesToTicks(3, kRate60);
    morph.after = framesToTicks(3, kRate60);
    morph.effect = std::string(kMorphCutTransition);
    auto seq = std::make_shared<Sequence>();
    seq->id = s.ids.allocate<SequenceId>();
    seq->frameRate = kRate60;
    seq->width = 1920;
    seq->height = 1080;
    seq->videoTracks = {trackOf(s, {a, b}, {morph})};
    s.sequences.push_back(seq);
    std::vector<LayerSpec> layers;
    for (const std::int64_t frame : {57, 59, 60, 62}) {
        INFO("frame " << frame);
        evaluateVideoLayers(s, *seq, framesToTicks(frame, kRate60), layers);
        REQUIRE(layers.size() == 2);
        // A at the transition's start (frame 57: its source 157), B at its end (frame 63: source 303).
        CHECK(layers[0].sourceTime == framesToTicks(157, kRate60));
        CHECK(layers[1].sourceTime == framesToTicks(303, kRate60));
        REQUIRE(layers[0].transition);
        CHECK(layers[0].transition->kind == render::TransitionKind::FlowMorph);
        CHECK(layers[0].transition->progress == Approx((frame - 57) / 6.0));
    }
    // The motion it needs: the frozen pictures placed on the canvas; the background analysis asks for the same.
    evaluateVideoLayers(s, *seq, framesToTicks(59, kRate60), layers);
    std::vector<media::VideoFrame> frames{cpuFrame(framesToTicks(157, kRate60), framesToTicks(1, kRate60)),
                                          cpuFrame(framesToTicks(303, kRate60), framesToTicks(1, kRate60))};
    const std::vector<MotionRequest> asked = motionRequestsOf(layers, frames, {1920, 1080});
    REQUIRE(asked.size() == 1);
    const std::vector<MotionRequest> prefetched = morphCutRequests(s, *seq);
    REQUIRE(prefetched.size() == 1);
    CHECK(prefetched[0] == asked[0]);
    CHECK(asked[0].a.time == framesToTicks(157, kRate60));
    CHECK(asked[0].b.time == framesToTicks(303, kRate60));
    // Without motion the pair dissolves.
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].transition->kind == render::TransitionKind::CrossDissolve);
}

TEST_CASE("Warp Stabilizer and Rolling Shutter Repair warp the frame inside a group", "[playback][motion][stabilize]") {
    ProjectState s;
    auto media = fakeMedia(s, kRate30);
    s.media = {media};
    Clip clip = videoClip(s, media->id, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate30;
    seq.width = 1920;
    seq.height = 1080;
    const auto evaluateWith = [&](EffectStack stack) {
        Clip c = clip;
        c.setEffects(std::move(stack));
        seq.videoTracks = {trackOf(s, {c})};
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(s, seq, framesToTicks(3, kRate30), layers);
        return layers;
    };
    EffectInstance warp;
    warp.id = EffectInstanceId{1};
    warp.effect = std::string(kWarpStabilizerEffect);
    // Not analysed yet (identity, Stabilize Crop Auto-scale): the plain layer.
    REQUIRE(evaluateWith({warp}).size() == 1);
    // Position, Scale, Rotation: 19.2 px right, framing Stabilize Crop with the analysed crop.
    warp.set("method", ParamValue(1.0));
    warp.set("transform.x", ParamValue(19.2));
    warp.set("framing", ParamValue(1.0));
    warp.set("crop.left", ParamValue(0.02));
    warp.set("crop.right", ParamValue(0.03));
    std::vector<LayerSpec> layers = evaluateWith({warp});
    REQUIRE(layers.size() == 2);
    CHECK(layers[0].nested);
    CHECK(layers[0].nestedCount == 1);
    CHECK(layers[0].nestedCanvas == render::CanvasSize{1920, 1080});
    CHECK_FALSE(needsDecode(layers[0]));
    REQUIRE_FALSE(layers[0].effects.empty());
    CHECK(layers[0].effects.front().type == render::EffectType::Crop);
    CHECK(layers[0].effects.front().params[0] == Approx(0.02));
    CHECK(layers[0].effects.front().params[2] == Approx(0.03));
    CHECK(needsDecode(layers[1]));
    CHECK(layers[1].sourceTime == framesToTicks(3, kRate30));
    CHECK(layers[1].warp[0] == Approx(1.0));
    CHECK(layers[1].warp[2] == Approx(0.01)); // 19.2 / 1920 of the frame
    CHECK(layers[1].warp[5] == Approx(0.0).margin(1e-12));
    // Synthesize Edges: the unwarped frame behind the stabilized one, no crop.
    warp.set("framing", ParamValue(3.0));
    layers = evaluateWith({warp});
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].nestedCount == 2);
    CHECK((layers[0].effects.empty() || layers[0].effects.front().type != render::EffectType::Crop));
    CHECK(layers[1].warp == motion::kIdentity3);
    CHECK(layers[2].warp[2] == Approx(0.01));
    // Rolling Shutter Repair alone: lines read later move back further (top to bottom scan, 50 % rate).
    EffectInstance shutter;
    shutter.id = EffectInstanceId{2};
    shutter.effect = std::string(kRollingShutterRepairEffect);
    shutter.set("motion.x", ParamValue(19.2));
    layers = evaluateWith({shutter});
    REQUIRE(layers.size() == 2);
    CHECK(layers[1].warp[1] == Approx(-0.005)); // x shear by the line's place in the scan
    CHECK(layers[1].warp[2] == Approx(0.0025));
    // A disabled stabilizer does nothing.
    warp.enabled = false;
    REQUIRE(evaluateWith({warp}).size() == 1);
}

// ---- End to end over synthetic media --------------------------------------------------------------------------------

TEST_CASE("Optical Flow beats Frame Blending which beats Frame Sampling on 24 fps footage at 48 fps",
          "[playback][motion][gpu]") {
    // Sequence frame 11 shows source time 11/48 s, between the 24 fps frames 5 and 6; the 48 fps file's frame 11 is
    // the truth there.
    const std::int64_t frame = 11;
    ProjectState truthState;
    const MediaId truthMedia = addFile(truthState, conformMedia().at48);
    const Clip truthClip = videoClip(truthState, truthMedia, Ticks{}, framesToTicks(40, Rate{48, 1}), Ticks{});
    const MotionProject truth = projectWith(std::move(truthState), Rate{48, 1}, {truthClip});
    const std::vector<std::uint8_t> expected = exportFrame(truth, Rate{48, 1}, frame);
    MotionAnalyzer::shared().clear();
    const double sampling = lumaPsnr(expected, exportFrame(conformProject(TimeInterpolation::FrameSampling), Rate{48, 1}, frame));
    const double blending = lumaPsnr(expected, exportFrame(conformProject(TimeInterpolation::FrameBlending), Rate{48, 1}, frame));
    const auto start = std::chrono::steady_clock::now();
    const double flow = lumaPsnr(expected, exportFrame(conformProject(TimeInterpolation::OpticalFlow), Rate{48, 1}, frame));
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    WARN("frame sampling " << sampling << " dB, frame blending " << blending << " dB, optical flow " << flow
                           << " dB (optical flow frame " << ms << " ms incl. analysis)");
    CHECK(blending > sampling + 2.0);
    CHECK(flow > blending + 6.0);
    // A sequence frame on a source frame (frame 10 shows the 24 fps frame 5 exactly) is that frame with any method.
    const std::vector<std::uint8_t> onFrame = exportFrame(truth, Rate{48, 1}, 10);
    CHECK(lumaPsnr(onFrame, exportFrame(conformProject(TimeInterpolation::OpticalFlow), Rate{48, 1}, 10)) > 45.0);
}

TEST_CASE("Export of optical flow is deterministic and the program monitor shows the same frame",
          "[playback][motion][gpu]") {
    const MotionProject project = conformProject(TimeInterpolation::OpticalFlow);
    MotionAnalyzer::shared().clear();
    const std::vector<std::uint8_t> first = exportFrame(project, Rate{48, 1}, 13);
    MotionAnalyzer::shared().clear(); // analysed again from scratch
    const std::vector<std::uint8_t> second = exportFrame(project, Rate{48, 1}, 13);
    CHECK(first == second);

    // The program monitor: the scheduler waits for the motion, then publishes the same composite; rendered for export
    // it is the exported frame.
    MotionAnalyzer::shared().clear();
    EditSession session(project.state);
    PlaybackController controller;
    controller.setSession(&session);
    controller.setSequence(project.sequence);
    const Ticks target = framesToTicks(13, Rate{48, 1});
    controller.seek(target);
    const std::uint64_t epoch = controller.mailbox()->epoch();
    std::shared_ptr<const render::PreparedComposite> shown;
    const auto until = std::chrono::steady_clock::now() + 10s;
    while (!shown && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(16ms);
        const auto sel = controller.mailbox()->acquire(steadyNowNs() + 16'666'667);
        if (sel.composite && sel.composite->seekEpoch == epoch && sel.composite->targetTime == target) {
            shown = sel.composite;
        }
    }
    REQUIRE(shown);
    REQUIRE(shown->layers.size() == 1);
    REQUIRE(shown->layers[0].nested);
    REQUIRE(shown->layers[0].nested->layers.size() == 2);
    CHECK(shown->layers[0].nested->layers[0].transition->kind == render::TransitionKind::FlowMorph);
    auto renderer = render::OffscreenRenderer::create({.role = render::OutputRole::Export});
    REQUIRE(renderer);
    auto image = (*renderer)->render(shown, QSize(320, 180));
    REQUIRE(image);
    CHECK(lumaPsnr(first, image->rgba) > 50.0);
}

TEST_CASE("Render In to Out renders an optical flow section through the export path", "[playback][motion][gpu]") {
    // The render bar shows the retimed clip red; render.previewInOut's renderer (an export of the segment) computes the
    // motion itself, so the preview file holds the full-quality frames.
    const MotionProject project = conformProject(TimeInterpolation::OpticalFlow);
    const TimeRange segment{Ticks{}, framesToTicks(12, Rate{48, 1})};
    const std::vector<RenderSegment> bar = renderSegments(project.state, project.sequence);
    const auto red = std::find_if(bar.begin(), bar.end(), [](const RenderSegment& s) {
        return s.range.contains(framesToTicks(5, Rate{48, 1}));
    });
    REQUIRE(red != bar.end());
    CHECK(red->status == RenderStatus::Red);
    CHECK(red->reason == "optical flow");
    const fs::path file = motionDir() / "preview-optical-flow.mov";
    std::error_code ec;
    fs::remove(file, ec);
    MotionAnalyzer::shared().clear();
    const auto start = std::chrono::steady_clock::now();
    auto bytes = editor::makeExportPreviewRenderer({})->render(project.state, project.sequence, segment, file, {});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    INFO((bytes ? std::string() : bytes.error().message));
    REQUIRE(bytes);
    CHECK(*bytes > 0);
    WARN(std::format("rendered 12 optical flow frames (320 x 180) in {:.2f} s", seconds));
    auto decoder = media::VideoDecoder::open(file.string(), 0, {});
    REQUIRE(decoder);
    auto frame = (*decoder)->frameAt(framesToTicks(11, Rate{48, 1}));
    REQUIRE(frame);
    CHECK(frame->width == 320);
    CHECK(frame->height == 180);
}
TEST_CASE("A Morph Cut's middle frame is closer to the true in-between than a cross dissolve", "[playback][motion][gpu]") {
    // Shot A: a still texture; shot B: the same texture 8 px right, 4 px up and 3 % larger (a jump cut in an interview:
    // the subject moved). Half way through the transition the truth is half of each.
    const motion::test::MovingTexture texture(41);
    const auto still = [&](double dx, double dy, double zoom) {
        return std::vector<motion::Plane>(48, texture.picture(320, 180, dx, dy, zoom, 160.0, 90.0));
    };
    const fs::path shotA = motionDir() / "shotA.mkv";
    const fs::path shotB = motionDir() / "shotB.mkv";
    const fs::path half = motionDir() / "shotHalf.mkv";
    REQUIRE(encodeGrey(shotA, still(0.0, 0.0, 1.0), Rate{48, 1}));
    REQUIRE(encodeGrey(shotB, still(8.0, -4.0, 1.03), Rate{48, 1}));
    REQUIRE(encodeGrey(half, still(4.0, -2.0, 1.015), Rate{48, 1}));
    const auto cutWith = [&](std::string_view effect) {
        ProjectState state;
        const MediaId a = addFile(state, shotA);
        const MediaId b = addFile(state, shotB);
        const Rate r{48, 1};
        Clip ca = videoClip(state, a, Ticks{}, framesToTicks(24, r), Ticks{});
        Clip cb = videoClip(state, b, framesToTicks(24, r), framesToTicks(24, r), framesToTicks(24, r));
        Transition t;
        t.id = state.ids.allocate<TransitionId>();
        t.outgoing = ca.id;
        t.incoming = cb.id;
        t.before = framesToTicks(4, r);
        t.after = framesToTicks(4, r);
        t.effect = std::string(effect);
        return projectWith(std::move(state), r, {ca, cb}, {t});
    };
    ProjectState truthState;
    const MediaId truthMedia = addFile(truthState, half);
    const Clip truthClip = videoClip(truthState, truthMedia, Ticks{}, framesToTicks(48, Rate{48, 1}), Ticks{});
    const MotionProject truth = projectWith(std::move(truthState), Rate{48, 1}, {truthClip});
    const std::vector<std::uint8_t> expected = exportFrame(truth, Rate{48, 1}, 24);
    const double morph = lumaPsnr(expected, exportFrame(cutWith(kMorphCutTransition), Rate{48, 1}, 24));
    const double dissolve = lumaPsnr(expected, exportFrame(cutWith("crossDissolve"), Rate{48, 1}, 24));
    WARN("Morph Cut " << morph << " dB, cross dissolve " << dissolve << " dB");
    CHECK(morph > dissolve + 6.0);
    // Both ends are the shots themselves: continuous with the cut's neighbours.
    const std::vector<std::uint8_t> a = exportFrame(cutWith("crossDissolve"), Rate{48, 1}, 19);
    CHECK(lumaPsnr(a, exportFrame(cutWith(kMorphCutTransition), Rate{48, 1}, 20)) > 40.0);
}

// ---- Interpret Footage with Time Interpolation (p02 integration) ---------------------------------------------------

TEST_CASE("Interpreted footage with Time Interpolation evaluates at its conform speed (P.2 integration)",
          "[playback][motion][interpret]") {
    // A 120 fps capture interpreted as 30 fps plays at 1/4 speed and shows every frame: in a 30 fps sequence nothing is
    // retimed, so Frame Blending and Optical Flow show the plain frame (no group, no render bar). Interpreted as 24 fps
    // it is 24 fps material in a 30 fps sequence: retimed, so the group mixes the two 120 fps frames around the source
    // time, each with the interpretation's pixel aspect.
    ProjectState s;
    auto capture = fakeMedia(s, Rate{120, 1});
    s.media = {capture};
    const MediaId media = capture->id;
    const Clip clip = videoClip(s, media, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    const ClipId id = clip.id;
    auto seq = std::make_shared<Sequence>();
    seq->id = s.ids.allocate<SequenceId>();
    seq->frameRate = kRate30;
    seq->width = 1920;
    seq->height = 1080;
    seq->videoTracks = {trackOf(s, {clip})};
    seq->duration = Ticks::fromSeconds(2);
    s.sequences.push_back(seq);
    const auto interpret = [&](const ProjectState& state, Rate rate, std::optional<Rate> pixelAspect) {
        MediaInterpretation in;
        in.frameRate = rate;
        in.pixelAspect = pixelAspect;
        auto r = edit::interpretFootage(state, std::span(&media, 1), in);
        REQUIRE(r);
        return std::move(r->state);
    };
    const Ticks capturePeriod = framesToTicks(1, Rate{120, 1});

    for (const TimeInterpolation mode : {TimeInterpolation::FrameBlending, TimeInterpolation::OpticalFlow}) {
        INFO("mode " << toString(mode));
        auto withMode = edit::setTimeInterpolation(s, seq->id, std::span(&id, 1), mode);
        REQUIRE(withMode);
        const ProjectState at30 = interpret(withMode->state, Rate{30, 1}, std::nullopt);
        const Sequence& seq30 = *at30.findSequence(seq->id);
        const Clip& conformed = seq30.videoTracks.at(0)->clips.front();
        CHECK(conformed.speed == Speed{1, 4});
        CHECK(conformed.timelineRange.duration == Ticks::fromSeconds(8));
        CHECK(conformed.timeInterpolation() == mode);
        CHECK_FALSE(retimesFrames(conformed, *at30.findMedia(media), kRate30));
        std::vector<LayerSpec> layers;
        evaluateVideoLayers(at30, seq30, framesToTicks(5, kRate30), layers);
        REQUIRE(layers.size() == 1);
        CHECK_FALSE(layers[0].interpolation);
        CHECK(layers[0].sourceTime == framesToTicks(5, Rate{120, 1})); // the capture's frame 5, nothing mixed
        for (const RenderSegment& segment : renderSegments(at30, seq->id)) {
            CHECK(segment.reason.find("optical flow") == std::string::npos);
        }
        // A speed change on top of the conform (50 % of it) retimes again; so does a 60 fps sequence.
        Clip slower = conformed;
        slower.speed = Speed{1, 8};
        CHECK(retimesFrames(slower, *at30.findMedia(media), kRate30));
        CHECK(retimesFrames(conformed, *at30.findMedia(media), kRate60));

        const ProjectState at24 = interpret(at30, Rate{24, 1}, Rate{2, 1});
        const Sequence& seq24 = *at24.findSequence(seq->id);
        const Clip& retimed = seq24.videoTracks.at(0)->clips.front();
        CHECK(retimed.speed == Speed{1, 5});
        CHECK(retimesFrames(retimed, *at24.findMedia(media), kRate30));
        // Sequence frame 1 shows source time 1/150 s: 0.8 of the way from the capture's frame 0 to its frame 1.
        evaluateVideoLayers(at24, seq24, framesToTicks(1, kRate30), layers);
        REQUIRE(layers.size() == 3);
        CHECK(layers[0].interpolation == mode);
        CHECK(layers[0].sourceTime == Ticks{kTicksPerSecond / 150});
        CHECK(layers[0].nestedCanvas == render::CanvasSize{3840, 1080}); // 2:1 pixels as interpreted
        CHECK(layers[1].sourceTime == layers[0].sourceTime);
        CHECK(layers[2].interpolationSecond);
        CHECK(layers[2].sourceTime == layers[0].sourceTime + capturePeriod); // the capture's own next frame
        std::vector<media::VideoFrame> frames{{}, cpuFrame(Ticks{}, capturePeriod), cpuFrame(capturePeriod, capturePeriod)};
        FakeMotion motionSource;
        std::vector<render::CompositeLayer> out;
        buildCompositeLayers(layers, frames, out, &motionSource, {1920, 1080});
        REQUIRE(out.size() == 1);
        REQUIRE(out[0].nested);
        CHECK(out[0].nested->canvas == render::CanvasSize{3840, 1080});
        REQUIRE(out[0].nested->layers.size() == 2);
        REQUIRE(out[0].nested->layers[0].transition);
        CHECK(out[0].nested->layers[0].transition->progress == Approx(0.8));
        CHECK(out[0].nested->layers[0].transition->kind == (mode == TimeInterpolation::OpticalFlow
                                                                ? render::TransitionKind::FlowMorph
                                                                : render::TransitionKind::CrossDissolve));
        CHECK(motionSource.asked.size() == (mode == TimeInterpolation::OpticalFlow ? 1u : 0u));
        for (const render::CompositeLayer& frame : out[0].nested->layers) {
            CHECK(frame.frame.sampleAspect == Rate{2, 1});
        }
    }
}

TEST_CASE("Optical Flow on interpreted footage renders like the same retime of native footage (P.2 integration)",
          "[playback][motion][interpret][gpu]") {
    // The 24 fps file interpreted as 12 fps in a 24 fps sequence: frame 11 shows source time 11/48 s, between its frames
    // 5 and 6, exactly what the uninterpreted file shows at 48 fps frame 11. Same frames, same place, same motion.
    MotionAnalyzer::shared().clear();
    const MotionProject interpreted =
        interpretedProject(conformMedia().at24, Rate{12, 1}, Rate{24, 1}, TimeInterpolation::OpticalFlow);
    const Clip& clip = interpreted.state.findSequence(interpreted.sequence)->videoTracks.at(0)->clips.front();
    CHECK(clip.speed == Speed{1, 2});
    const std::vector<std::uint8_t> conformed = exportFrame(interpreted, Rate{24, 1}, 11);
    const std::vector<std::uint8_t> native = exportFrame(conformProject(TimeInterpolation::OpticalFlow), Rate{48, 1}, 11);
    const double same = lumaPsnr(native, conformed);
    // And against the truth (the 48 fps file's frame 11), better than blending the same two frames.
    ProjectState truthState;
    const MediaId truthMedia = addFile(truthState, conformMedia().at48);
    const Clip truthClip = videoClip(truthState, truthMedia, Ticks{}, framesToTicks(40, Rate{48, 1}), Ticks{});
    const MotionProject truth = projectWith(std::move(truthState), Rate{48, 1}, {truthClip});
    const std::vector<std::uint8_t> expected = exportFrame(truth, Rate{48, 1}, 11);
    const double flow = lumaPsnr(expected, conformed);
    const MotionProject blended =
        interpretedProject(conformMedia().at24, Rate{12, 1}, Rate{24, 1}, TimeInterpolation::FrameBlending);
    const double blending = lumaPsnr(expected, exportFrame(blended, Rate{24, 1}, 11));
    WARN("interpreted optical flow vs native " << same << " dB; vs truth: optical flow " << flow << " dB, frame blending "
                                               << blending << " dB");
    CHECK(same > 45.0);
    CHECK(flow > blending + 6.0);
    // The 48 fps file interpreted as 24 fps in a 24 fps sequence shows every frame as it is, whatever the method.
    const MotionProject everyFrame =
        interpretedProject(conformMedia().at48, Rate{24, 1}, Rate{24, 1}, TimeInterpolation::OpticalFlow);
    CHECK(lumaPsnr(expected, exportFrame(everyFrame, Rate{24, 1}, 11)) > 45.0);
}

TEST_CASE("A clip's input colour space reaches the frames inside its interpolation and stabilizer groups (P.2 integration)",
          "[playback][motion][color]") {
    // The compositor reads each decoded frame through its layer's input colour space (I.9); the groups Time Interpolation
    // and the stabilizers make draw the clip's frames as their own layers, so those carry the clip's space too.
    ProjectState s;
    auto media = fakeMedia(s, kRate30, 8, 8);
    s.media = {media};
    Clip clip = videoClip(s, media->id, Ticks{}, Ticks::fromSeconds(2), Ticks{});
    ClipContent content;
    content.timeInterpolation = TimeInterpolation::FrameBlending;
    content.inputColorSpace = InputColorSpace::SLog3SGamut3;
    clip.setContent(content);
    Sequence seq;
    seq.id = s.ids.allocate<SequenceId>();
    seq.frameRate = kRate60;
    seq.width = 8;
    seq.height = 8;
    seq.videoTracks = {trackOf(s, {clip})};
    const Ticks period = framesToTicks(1, kRate30);

    // 30 fps footage at 60 fps: an interpolation group, both frames read as S-Log3.
    std::vector<LayerSpec> layers;
    evaluateVideoLayers(s, seq, framesToTicks(1, kRate60), layers);
    REQUIRE(layers.size() == 3);
    CHECK(layers[0].inputSpace == InputColorSpace::SLog3SGamut3);
    std::vector<media::VideoFrame> frames{{}, cpuFrame(Ticks{}, period), cpuFrame(period, period)};
    std::vector<render::CompositeLayer> out;
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].nested);
    REQUIRE(out[0].nested->layers.size() == 2);
    for (const render::CompositeLayer& frame : out[0].nested->layers) {
        CHECK(frame.inputSpace == InputColorSpace::SLog3SGamut3);
    }

    // A Warp Stabilizer at the sequence's rate: its group's frame is read as S-Log3 as well.
    seq.frameRate = kRate30;
    EffectInstance warp;
    warp.id = EffectInstanceId{1};
    warp.effect = std::string(kWarpStabilizerEffect);
    warp.set("method", ParamValue(1.0));
    warp.set("transform.x", ParamValue(1.0));
    Clip stabilized = clip;
    stabilized.setEffects({warp});
    seq.videoTracks = {trackOf(s, {stabilized})};
    evaluateVideoLayers(s, seq, framesToTicks(3, kRate30), layers);
    REQUIRE(layers.size() == 2);
    CHECK(layers[1].inputSpace == InputColorSpace::SLog3SGamut3);
    frames = {{}, cpuFrame(framesToTicks(3, kRate30), period)};
    out.clear();
    buildCompositeLayers(layers, frames, out);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].nested);
    REQUIRE(out[0].nested->layers.size() == 1);
    CHECK(out[0].nested->layers[0].inputSpace == InputColorSpace::SLog3SGamut3);
}

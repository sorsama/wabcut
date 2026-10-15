// AI-14 on media (full builds): the decoded pictures the model sees (MediaFrameSource: stored orientation, forward and
// backward without a seek per frame), the compositor's matte source over the analysis cache (CacheMatteSource: the
// matte at a frame for effects; for redactions the nearest present matte grown, never nothing), mask.objectSelect end
// to end on a real file through the scripted wabcut-embed, and the opt-in smoke test of the real SAM 2.1 tiny model
// (WABCUT_TEST_SAM2_DIR = a directory with its six files): IoU against the synthetic ground truth and frames per
// second at a 1080p working resolution on DirectML and on the CPU.

#include "SegmentTestUtil.h"

#include <wabcut/ai/analysis/AnalysisCommands.h>
#include <wabcut/ai/edit/TranscriptCommands.h>
#include <wabcut/ai/segment/MediaSegment.h>
#include <wabcut/command/Engine.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/ProjectIO.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/render/Mask.h>
#include <wabcut/render/Matte.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <span>
#include <vector>

using namespace wabcut;
using namespace wabcut::ai;
using namespace wabcut::ai::segment;
using namespace wabcut::ai::segment::test;
using Catch::Approx;
using Json = nlohmann::json;

namespace {

/// `frames` of the scripted scene at `width` x `height`, 25 fps, H.264 (FFmpeg); false without FFmpeg.
bool makeVideo(const std::filesystem::path& file, int frames, int width, int height, ScriptedScene scene = {}) {
#ifndef WABCUT_SEGMENT_FFMPEG
    (void)file, (void)frames, (void)width, (void)height, (void)scene;
    return false;
#else
    const std::filesystem::path raw = file.parent_path() / (file.stem().string() + ".rgb");
    {
        std::ofstream out(raw, std::ios::binary);
        const ScriptedVideo video(scene);
        for (int n = 0; n < frames; ++n) {
            const search::Image img = video.draw(n, width, height);
            out.write(reinterpret_cast<const char*>(img.rgb.data()), static_cast<std::streamsize>(img.rgb.size()));
        }
    }
    const std::string command = std::string("\"\"") + WABCUT_SEGMENT_FFMPEG + "\" -loglevel error -y -f rawvideo -pix_fmt rgb24 -s " +
                                std::to_string(width) + "x" + std::to_string(height) + " -r 25 -i \"" + pathToUtf8(raw) +
                                "\" -c:v libx264 -preset ultrafast -crf 12 -g 25 -pix_fmt yuv420p \"" + pathToUtf8(file) + "\"\"";
    const bool ok = std::system(command.c_str()) == 0;
    std::filesystem::remove(raw);
    return ok && std::filesystem::exists(file);
#endif
}

MediaId addMedia(ProjectState& state, const std::filesystem::path& file) {
    auto info = media::probe(pathToUtf8(file));
    REQUIRE(info);
    MediaItem item;
    item.id = state.ids.allocate<MediaId>();
    item.name = pathToUtf8(file.filename());
    item.locator.path = pathToUtf8(file);
    item.info = *info;
    const MediaId id = item.id;
    state.media.push_back(std::make_shared<const MediaItem>(std::move(item)));
    return id;
}

/// The mean coverage of `spec` inside and outside `box` (normalised).
std::pair<double, double> coverageInOut(const render::MaskSpec& spec, const MatteBox& box) {
    const int w = 192, h = 108;
    const std::vector<float> c = render::maskCoverageCpu(spec, w, h);
    double in = 0, out = 0;
    int nin = 0, nout = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double u = (x + 0.5) / w, v = (y + 0.5) / h;
            const bool inside = u > box.x + 0.02 && u < box.x + box.width - 0.02 && v > box.y + 0.02 && v < box.y + box.height - 0.02;
            const bool outside = u < box.x - 0.03 || u > box.x + box.width + 0.03 || v < box.y - 0.03 || v > box.y + box.height + 0.03;
            if (inside) {
                in += c[static_cast<std::size_t>(y) * w + x];
                ++nin;
            } else if (outside) {
                out += c[static_cast<std::size_t>(y) * w + x];
                ++nout;
            }
        }
    }
    return {nin ? in / nin : 0.0, nout ? out / nout : 0.0};
}

} // namespace

TEST_CASE("MediaFrameSource: the stored picture at a time, forward and backward in blocks", "[ai][segment][media]") {
    const auto dir = freshDir("media-frames");
    const auto file = dir / "scene.mp4";
    if (!makeVideo(file, 80, 640, 360)) {
        SKIP("no FFmpeg in this build to make the video");
    }
    auto source = MediaFrameSource::open(pathToUtf8(file), -1);
    REQUIRE(source);
    MediaFrameSource& s = **source;
    CHECK(s.frameDuration() == frameTicks(1));
    CHECK(s.frames().duration == frameTicks(80));
    CHECK(s.aspect() == Approx(16.0 / 9.0));
    const ScriptedVideo truth;
    for (const int n : {0, 10, 30}) {
        auto p = s.pictureAt(frameTicks(n) + Ticks{3}, 256);
        REQUIRE(p);
        CHECK(p->width == 256);
        CHECK(p->height == 256);
        CHECK(iouOf(RedSegmenter::segment(*p, 128, 72).matte, RedSegmenter::segment(truth.draw(n, 256, 256), 128, 72).matte) > 0.9);
    }
    // Backward over 24 frames: about two blocks of decoding, not a seek and a GOP per frame.
    const int before = s.decoded();
    for (int n = 70; n > 46; --n) {
        auto p = s.pictureAt(frameTicks(n), 256);
        REQUIRE(p);
        CHECK(iouOf(RedSegmenter::segment(*p, 128, 72).matte, RedSegmenter::segment(truth.draw(n, 256, 256), 128, 72).matte) > 0.9);
    }
    CHECK(s.decoded() - before <= 30);
    CHECK_FALSE(s.pictureAt(frameTicks(500), 256));
}

TEST_CASE("CacheMatteSource: effects get the frame's matte, redactions fail closed", "[ai][segment][media]") {
    const auto dir = freshDir("matte-source");
    // A track of frames 10..19 (the object lost at 15) in the cache.
    MatteTrack t;
    t.fingerprint = 0x1234;
    t.stream = 0;
    t.model = "red-v1";
    t.params = "0123456789abcdef";
    t.width = 192;
    t.height = 108;
    t.aspect = 16.0 / 9.0;
    t.frameDuration = frameTicks(1);
    const ScriptedVideo video(ScriptedScene{100, -1});
    for (int n = 10; n < 20; ++n) {
        MatteFrame f;
        f.time = frameTicks(n);
        Segmentation seg = RedSegmenter::segment(video.draw(n, 192, 108), 192, 108);
        if (n == 15) {
            std::fill(seg.matte.alpha.begin(), seg.matte.alpha.end(), 0);
            seg.present = false;
        }
        f.present = seg.present;
        f.box = boxOf(seg.matte);
        f.rle = encodeRle(seg.matte);
        f.confidence = 0.9f;
        t.frames.push_back(std::move(f));
    }
    REQUIRE(MatteStore(dir).put(t));
    CacheMatteSource source(dir, 8);
    MaskMatte m;
    m.track = t.identity();
    m.media = MediaId{1};
    m.source = t.span();
    m.model = t.model;

    auto effect = source.fieldAt(m, frameTicks(12) + Ticks{5}, 16.0 / 9.0, render::MatteUse::Effect);
    REQUIRE(effect);
    CHECK_FALSE(effect->fallback);
    CHECK(effect->width == 192);
    const MatteBox truth = ScriptedScene{100, -1}.objectAt(12);
    CHECK(render::matteDistance(*effect, render::Vec2{truth.x + truth.width / 2, truth.y + truth.height / 2}) > 0.05);
    CHECK(render::matteDistance(*effect, render::Vec2{0.9, 0.9}) < -0.2);
    CHECK(source.fieldAt(m, frameTicks(40), 16.0 / 9.0, render::MatteUse::Effect) == nullptr);   // outside the track: the box stands in
    auto lostEffect = source.fieldAt(m, frameTicks(15), 16.0 / 9.0, render::MatteUse::Effect);
    REQUIRE(lostEffect);                                                                           // the object not there: an empty matte
    CHECK(render::matteDistance(*lostEffect, render::Vec2{0.5, 0.5}) < 0.0);

    // Redactions: a lost frame or one past the track gets the nearest present matte, grown with the gap.
    auto lost = source.fieldAt(m, frameTicks(15), 16.0 / 9.0, render::MatteUse::Redaction);
    REQUIRE(lost);
    CHECK(lost->fallback);
    auto past = source.fieldAt(m, frameTicks(40), 16.0 / 9.0, render::MatteUse::Redaction);
    REQUIRE(past);
    CHECK(past->fallback);
    const MatteBox last = ScriptedScene{100, -1}.objectAt(19);
    // 21 frames (0.84 s) past the last matte: grown by 1 % + 15 % per second of the frame height (0.136).
    const render::Vec2 nearEdge{last.x + last.width + 0.05, last.y + last.height / 2}; // 0.09 frame heights out
    CHECK(render::matteDistance(*past, nearEdge) > 0.0);
    auto here = source.fieldAt(m, frameTicks(19), 16.0 / 9.0, render::MatteUse::Redaction);
    REQUIRE(here);
    CHECK_FALSE(here->fallback);
    CHECK(render::matteDistance(*here, nearEdge) < 0.0);
    // No such track on this computer: null (the redaction covers its bounding box, grown; render/Mask.h).
    MaskMatte unknown = m;
    unknown.track = "00000000000000ff/0/red-v1/0123456789abcdef";
    CHECK(source.fieldAt(unknown, frameTicks(12), 16.0 / 9.0, render::MatteUse::Redaction) == nullptr);

    // Fields are kept (an LRU): asking again makes nothing new.
    const std::size_t made = source.fieldsMade();
    REQUIRE(source.fieldAt(m, frameTicks(12), 16.0 / 9.0, render::MatteUse::Effect));
    CHECK(source.fieldsMade() == made);

    // Through the mask pipeline: an opacity matte mask covers the object, not the rest; a redaction at a frame with no
    // matte still covers it.
    const auto installed = installMatteSource(dir);
    REQUIRE(installed);
    CHECK(installMatteSource(dir) == installed);
    Mask mask = objectMask(t, MediaId{1}, "Object", 0.0);
    auto spec = render::maskSpecAt(std::span(&mask, 1), frameTicks(12), 16.0 / 9.0);
    REQUIRE(spec);
    const auto [in, out] = coverageInOut(*spec, truth);
    CHECK(in > 0.95);
    CHECK(out < 0.05);
    auto redaction = render::maskSpecAt(std::span(&mask, 1), frameTicks(15), 16.0 / 9.0, render::MatteUse::Redaction);
    REQUIRE(redaction);
    CHECK(coverageInOut(*redaction, ScriptedScene{100, -1}.objectAt(15)).first > 0.95);
    render::setMatteSource(nullptr);
}

TEST_CASE("mask.objectSelect end to end on a real file through wabcut-embed (scripted)", "[ai][segment][media]") {
    const auto dir = freshDir("media-select");
    const auto file = dir / "scene.mp4";
    if (!makeVideo(file, 50, 640, 360, ScriptedScene{50, 30})) {
        SKIP("no FFmpeg in this build to make the video");
    }
    ProjectState state;
    wabcut::edit::SequenceSettings settings;
    settings.frameRate = kTestRate;
    auto seq = wabcut::edit::addSequence(state, settings);
    REQUIRE(seq);
    state = std::move(seq->state);
    const MediaId media = addMedia(state, file);
    const SequenceId sequence = seq->id;
    wabcut::edit::SourceClip source{media, TimeRange{Ticks{}, frameTicks(50)}, wabcut::edit::defaultSourcePatches(*state.findSequence(sequence), *state.findMedia(media))};
    auto inserted = wabcut::edit::insertEdit(state, sequence, source, Ticks{});
    REQUIRE(inserted);
    REQUIRE_FALSE(inserted->placed.empty());
    const ClipId clip = inserted->placed.front();
    EditSession session(inserted->state);

    MediaSegmentOptions o;
    o.projectFile = dir / "project.wabcut";
    o.cacheRoot = dir / "cache";
    o.modelRoot = dir / "models";
    o.helper = WABCUT_FAKE_EMBED;
    o.helperArguments = {"--mode", "normal"};
    o.detector = false;
    auto services = mediaSegmentServices(o);
    const auto installed = installMatteSource(o.cacheRoot);
    analysis::MapAnalysisProvider shots;
    analysis::ShotAnalysis cut;
    cut.cuts.push_back(analysis::ShotCut{frameTicks(30), 30, analysis::CutKind::Cut, 0.9f});
    auto shotProvider = std::make_shared<analysis::MapAnalysisProvider>();
    shotProvider->addShots(media, cut);
    command::CommandEngine engine(session, ai::edit::editingCommands(),
                                  command::EnginePolicy{command::CapabilitySet::all(), 64,
                                                        std::make_shared<command::ServiceBundle>(std::vector<std::shared_ptr<command::CommandServices>>{
                                                            services, std::make_shared<analysis::AnalysisServices>(shotProvider)})});
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = engine.executeJson(Json{{"command", "mask.objectSelect"},
                                           {"params", Json{{"clip", clip.value}, {"time", frameTicks(10).value}, {"points", Json::array({Json{{"x", 0.2}, {"y", 0.4}}})}}}});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(command::toJson(r).dump());
    REQUIRE(r.ok());
    CHECK(r.data["frames"] == 30); // 0..29: the cut at 30
    CHECK(r.data["lost"] == 0);
    CHECK(r.data["device"] == "FakeCPU");
    CHECK(seconds < 60.0);
    // The renderer's source knows the new track at once; the mattes are the object's.
    const Mask* found = nullptr;
    const auto loc = session.snapshot()->findSequence(sequence)->findClip(clip);
    REQUIRE(loc);
    for (const EffectInstance& e : loc->clip().effects()) {
        if (e.effect == "wabcut.mask" && !e.masks.empty()) {
            found = &e.masks.front();
        }
    }
    REQUIRE(found);
    const Mask& m = *found;
    const ScriptedScene scene{50, 30};
    for (const int n : {0, 10, 29}) {
        auto spec = render::maskSpecAt(std::span(&m, 1), frameTicks(n), 16.0 / 9.0);
        REQUIRE(spec);
        const auto [in, out] = coverageInOut(*spec, scene.objectAt(n));
        INFO("frame " << n);
        CHECK(in > 0.9);
        CHECK(out < 0.05);
    }
    render::setMatteSource(nullptr);
}

TEST_CASE("SAM 2.1 tiny, the real model: IoU and speed at 1080p (WABCUT_TEST_SAM2_DIR)", "[ai][segment][model]") {
    const char* modelDir = std::getenv("WABCUT_TEST_SAM2_DIR");
    if (modelDir == nullptr || *modelDir == '\0') {
        SKIP("set WABCUT_TEST_SAM2_DIR to a directory with SAM 2.1 tiny's six files (vision_encoder.onnx, ... constants.json)");
    }
#ifndef WABCUT_EMBED_HELPER
    SKIP("this build has no wabcut-embed");
#else
    const ScriptedScene scene{40, -1};
    ScriptedVideo video(scene);
    // The 1080p working resolution: the frames are drawn at 1920 x 1080 and resized to the model's 1024 x 1024 input,
    // as MediaFrameSource does with a decoded 1080p frame.
    struct Hd final : FrameSource {
        ScriptedVideo& v;
        explicit Hd(ScriptedVideo& v) : v(v) {}
        std::expected<search::Image, Error> pictureAt(Ticks t, int size) override {
            const search::Image hd = v.draw(v.frameOf(t), 1920, 1080);
            search::Image out;
            out.width = out.height = size;
            out.rgb.resize(static_cast<std::size_t>(size) * size * 3);
            for (int y = 0; y < size; ++y) {
                for (int x = 0; x < size; ++x) {
                    const int sx = static_cast<int>((x + 0.5) * 1920 / size), sy = static_cast<int>((y + 0.5) * 1080 / size);
                    std::copy_n(&hd.rgb[(static_cast<std::size_t>(sy) * 1920 + sx) * 3], 3, &out.rgb[(static_cast<std::size_t>(y) * size + x) * 3]);
                }
            }
            return out;
        }
        Ticks frameDuration() const override { return v.frameDuration(); }
        TimeRange frames() const override { return v.frames(); }
        double aspect() const override { return v.aspect(); }
    } hd(video);

    for (const std::string device : {"gpu", "cpu"}) {
        HelperSegmenterOptions o;
        o.executable = WABCUT_EMBED_HELPER;
        o.modelDirectory = modelDir;
        o.modelVersion = segmentModelVersion();
        o.device = device;
        o.workDir = freshDir("sam2-" + device);
        o.log = o.workDir.parent_path() / ("sam2-" + device + ".log");
        o.loadTimeout = std::chrono::minutes(5);
        o.requestTimeout = std::chrono::minutes(2);
        HelperSegmenter model(o);
        PropagationOptions p;
        p.prompt.time = frameTicks(5);
        const MatteBox start = scene.objectAt(5);
        p.prompt.points = {PromptPoint{start.x + start.width / 2, start.y + start.height / 2, true}};
        p.direction = Direction::Forward;
        p.span = TimeRange{Ticks{}, frameTicks(35)};
        // Warm up (sessions created, the first inference), then time the run.
        auto warm = model.prompt(*hd.pictureAt(frameTicks(0), 1024), p.prompt, 0, 1, 64, 36, {});
        INFO(device << ": " << (warm ? std::string("warm") : warm.error().message) << " (helper log: " << pathToUtf8(o.log) << ")");
        REQUIRE(warm);
        auto r = propagate(model, hd, p);
        INFO(device << ": " << (r ? std::string("ok") : r.error().message));
        REQUIRE(r);
        REQUIRE(r->track.frames.size() == 30);
        double worst = 1.0, sum = 0.0;
        for (const MatteFrame& f : r->track.frames) {
            auto got = r->track.imageOf(f);
            REQUIRE(got);
            const int n = video.frameOf(f.time);
            const MatteImage truth = RedSegmenter::segment(video.draw(n, 1920, 1080), got->width, got->height).matte;
            const double iou = iouOf(*got, truth);
            worst = std::min(worst, iou);
            sum += iou;
        }
        const double mean = sum / static_cast<double>(r->track.frames.size());
        std::printf("SAM 2.1 tiny on %s (%s): %.2f frames/s at 1080p working resolution, IoU mean %.4f, worst %.4f over %zu frames\n",
                    device.c_str(), r->device.c_str(), r->framesPerSecond, mean, worst, r->track.frames.size());
        CHECK(mean > 0.9);
        CHECK(worst > 0.8);
        CHECK(r->framesPerSecond > (device == "gpu" ? 0.5 : 0.05)); // fp32 at 1024: DirectML ~3 fps, the CPU ~0.2 (i5-12600K)
    }
#endif
}

namespace {

/// The model files from a local directory, as if downloaded (ModelStore::ensure then checks size and SHA-256 against
/// the pinned manifest, as it does for a real download).
struct LocalModelFiles final : ai::IDownloadTransport {
    std::filesystem::path dir;
    std::expected<Response, Error> get(const std::string& url, std::uint64_t offset, const std::function<bool(const Response&)>& onResponse,
                                       const std::function<bool(std::span<const std::byte>)>& onData, std::stop_token) override {
        const std::filesystem::path file = dir / url.substr(url.rfind('/') + 1);
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(file, ec);
        std::ifstream in(file, std::ios::binary);
        if (ec || !in) {
            return std::unexpected(Error(ErrorCode::IoError, "no " + pathToUtf8(file)));
        }
        in.seekg(static_cast<std::streamoff>(offset));
        const Response r{offset > 0 ? 206 : 200, static_cast<std::uint64_t>(size)};
        if (!onResponse(r)) {
            return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
        }
        std::vector<char> buffer(std::size_t{1} << 20);
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto n = static_cast<std::size_t>(in.gcount());
            if (n == 0) {
                break;
            }
            if (!onData(std::span<const std::byte>(reinterpret_cast<const std::byte*>(buffer.data()), n))) {
                return std::unexpected(Error(ErrorCode::Cancelled, "cancelled"));
            }
        }
        return r;
    }
};

} // namespace

TEST_CASE("mask.objectSelect with the real SAM 2.1 from the model store on a decoded 1080p file (WABCUT_TEST_SAM2_DIR)",
          "[ai][segment][model]") {
    const char* modelDir = std::getenv("WABCUT_TEST_SAM2_DIR");
    if (modelDir == nullptr || *modelDir == '\0') {
        SKIP("set WABCUT_TEST_SAM2_DIR to a directory with SAM 2.1 tiny's six files");
    }
#ifndef WABCUT_EMBED_HELPER
    SKIP("this build has no wabcut-embed");
#else
    const auto dir = freshDir("sam2-store");
    const auto file = dir / "hd.mp4";
    const ScriptedScene scene{40, -1};
    if (!makeVideo(file, 40, 1920, 1080, scene)) {
        SKIP("no FFmpeg in this build to make the video");
    }
    // Installed the way the app installs it: every file through ModelStore::ensure (size and SHA-256 checked).
    LocalModelFiles local;
    local.dir = modelDir;
    ai::ModelStore store(dir / "models");
    for (const ai::ModelInfo* m : segmentModelFiles()) {
        REQUIRE(m != nullptr);
        auto ensured = store.ensure(*m, &local);
        INFO(m->id << ": " << (ensured ? std::string("ok") : ensured.error().message));
        REQUIRE(ensured);
    }
    REQUIRE(segmentModelInstalled(store));

    ProjectState state;
    wabcut::edit::SequenceSettings settings;
    settings.frameRate = kTestRate;
    auto seq = wabcut::edit::addSequence(state, settings);
    REQUIRE(seq);
    state = std::move(seq->state);
    const MediaId media = addMedia(state, file);
    wabcut::edit::SourceClip source{media, TimeRange{Ticks{}, frameTicks(40)},
                                    wabcut::edit::defaultSourcePatches(*state.findSequence(seq->id), *state.findMedia(media))};
    auto inserted = wabcut::edit::insertEdit(state, seq->id, source, Ticks{});
    REQUIRE(inserted);
    const ClipId clip = inserted->placed.front();
    // Saved too: wabcut-cli can run the same request on it (WABCUT_CACHE_DIR = this directory: its models and cache).
    REQUIRE(io::saveProject(inserted->state, dir / "project.wabcut"));
    EditSession session(inserted->state);
    MediaSegmentOptions o;
    o.projectFile = dir / "project.wabcut";
    o.cacheRoot = dir / "analysis";
    o.modelRoot = dir / "models";
    o.helper = WABCUT_EMBED_HELPER;
    o.detector = false;
    command::CommandEngine engine(session, ai::edit::editingCommands(), command::EnginePolicy{command::CapabilitySet::all(), 64, mediaSegmentServices(o)});
    const MatteBox start = scene.objectAt(5);
    const auto t0 = std::chrono::steady_clock::now();
    const auto r = engine.executeJson(Json{{"command", "mask.objectSelect"},
                                           {"params", Json{{"clip", clip.value},
                                                           {"time", frameTicks(5).value},
                                                           {"points", Json::array({Json{{"x", start.x + start.width / 2}, {"y", start.y + start.height / 2}}})}}}});
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    INFO(command::toJson(r).dump());
    REQUIRE(r.ok());
    CHECK(r.data["frames"] == 40);
    auto track = MatteStore(o.cacheRoot).get(r.data["track"].get<std::string>());
    REQUIRE(track);
    REQUIRE(track->has_value());
    const ScriptedVideo video(scene);
    double sum = 0.0, worst = 1.0;
    for (const MatteFrame& f : (*track)->frames) {
        auto got = (*track)->imageOf(f);
        REQUIRE(got);
        const double iou = iouOf(*got, RedSegmenter::segment(video.draw(video.frameOf(f.time), 1920, 1080), got->width, got->height).matte);
        sum += iou;
        worst = std::min(worst, iou);
    }
    const double mean = sum / static_cast<double>((*track)->frames.size());
    std::printf("mask.objectSelect, real SAM 2.1 on %s, 40 frames of a decoded 1080p H.264 file: %.1f s in all (model load included), "
                "%.2f frames/s tracking, IoU mean %.4f, worst %.4f\n",
                r.data["device"].get<std::string>().c_str(), seconds, r.data["framesPerSecond"].get<double>(), mean, worst);
    CHECK(mean > 0.9);
    CHECK(worst > 0.8);
#endif
}

// AI-14 review, findings 3 and 4: a refine or a second propagation keeps a track's identity, and the source used to key
// its fields by that identity and the frame's INDEX - so it went on serving the old matte (possibly smaller than the
// object now is: under-redacted). Fields are now keyed by the track's content and the frame's time. And a field is at
// most 256 texels on its longer side (uploaded per frame by the compositor).
TEST_CASE("CacheMatteSource serves the current version of a track and caps the field size", "[ai][segment][media]") {
    const auto dir = freshDir("matte-source-versions");
    const ScriptedVideo video(ScriptedScene{100, -1});
    MatteTrack t;
    t.fingerprint = 0x5678;
    t.stream = 0;
    t.model = "red-v1";
    t.params = "fedcba9876543210";
    t.width = 512;
    t.height = 288;
    t.aspect = 16.0 / 9.0;
    t.frameDuration = frameTicks(1);
    for (int n = 10; n < 14; ++n) {
        MatteFrame f;
        f.time = frameTicks(n);
        const Segmentation seg = RedSegmenter::segment(video.draw(n, 512, 288), 512, 288);
        f.present = seg.present;
        f.box = boxOf(seg.matte);
        f.rle = encodeRle(seg.matte);
        f.confidence = 0.9f;
        t.frames.push_back(std::move(f));
    }
    CacheMatteSource source(dir, 8);
    source.remember(std::make_shared<const MatteTrack>(t));
    MaskMatte m;
    m.track = t.identity();
    m.media = MediaId{1};
    m.source = t.span();
    m.model = t.model;
    const MatteBox truth = ScriptedScene{100, -1}.objectAt(11);
    const render::Vec2 centre{truth.x + truth.width / 2, truth.y + truth.height / 2};
    auto before = source.fieldAt(m, frameTicks(11), 16.0 / 9.0, render::MatteUse::Effect);
    REQUIRE(before);
    CHECK(std::max(before->width, before->height) <= 256);
    CHECK(render::matteDistance(*before, centre) > 0.05);
    CHECK(before->half.size() == before->distance.size()); // half floats made once, here, not per upload
    CHECK(render::usableField(*before));
    const std::uint64_t firstRevision = source.revision(m);
    CHECK(firstRevision != 0);
    CHECK(source.fieldAt(m, frameTicks(11), 16.0 / 9.0, render::MatteUse::Effect) == before); // cached

    // Refined: the object is not at frame 11 after all (an empty matte). Same identity, new content.
    MatteTrack refined = t;
    MatteImage empty;
    empty.width = 512;
    empty.height = 288;
    empty.alpha.assign(512 * 288, 0);
    refined.frames[1].present = false;
    refined.frames[1].box.reset();
    refined.frames[1].rle = encodeRle(empty);
    REQUIRE(refined.identity() == t.identity());
    source.remember(std::make_shared<const MatteTrack>(refined));
    auto after = source.fieldAt(m, frameTicks(11), 16.0 / 9.0, render::MatteUse::Effect);
    REQUIRE(after);
    CHECK(after != before);
    CHECK(render::matteDistance(*after, centre) < 0.0); // the refined (empty) matte, not the cached old one
    CHECK(source.revision(m) != firstRevision);         // thumbnails key on it (RedactedPictures)

    // Propagated further back: a frame 9 first, so every index moves; frame 10 must not get frame 11's old field.
    MatteTrack longer = refined;
    MatteFrame nine = t.frames[0];
    nine.time = frameTicks(9);
    longer.frames.insert(longer.frames.begin(), nine);
    source.remember(std::make_shared<const MatteTrack>(longer));
    auto ten = source.fieldAt(m, frameTicks(10), 16.0 / 9.0, render::MatteUse::Effect);
    REQUIRE(ten);
    CHECK(ten != before);
    CHECK(ten != after);
}

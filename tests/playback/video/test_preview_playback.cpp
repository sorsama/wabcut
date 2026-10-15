// C.16 preview files through the Scheduler (PreviewSettings::previews): inside a rendered segment whose content hash is
// still the snapshot's, the scheduler shows ONE layer decoded from the preview file at frame (t - segment start)
// instead of compositing the clips. The rendered picture matches the live composite (PSNR, both drawn by the
// compositor); a file rendered from other content but recorded as current really is what plays (the swap happens);
// an edit inside the segment the cache was not told about, and a deleted file, bring the live picture back. Two known
// problems are recorded as [!mayfail] cases: a file the monitor has open is not deleted on invalidation (Windows), and
// an unreadable file shows black rather than the live picture.
//
// A live layer is told from a preview layer by what the clip carries: the clip has a Basic Correction effect and its
// clip id, the preview layer has neither (it is the already rendered picture).

#include "PlaybackTestSupport.h"

#include <wabcut/editor/PreviewRender.h>
#include <wabcut/export/ExportJob.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/FinishingOps.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectOps.h>
#include <wabcut/playback/video/PlaybackController.h>
#include <wabcut/render/OffscreenRenderer.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::playback;
using namespace wabcut::playback::test;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace {

constexpr const char* kClip = "h264_422_10bit.mp4"; // 1280x720 30 fps, 60 frames; barcode = frame number

Ticks frameAt(std::int64_t frame) { return framesToTicks(frame, kRate30); }

/// Seeks `controller` to sequence frame `frame` and returns the composite presented for it.
std::shared_ptr<const render::PreparedComposite> showOn(PlaybackController& controller, std::int64_t frame) {
    const Ticks target = frameAt(frame);
    controller.seek(target);
    const std::uint64_t epoch = controller.mailbox()->epoch();
    const auto until = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(16ms);
        const auto sel = controller.mailbox()->acquire(steadyNowNs() + 16'666'667);
        if (sel.changed && sel.composite && sel.composite->seekEpoch == epoch && sel.composite->targetTime == target) {
            return sel.composite;
        }
    }
    FAIL("no picture for frame " << frame);
    return nullptr;
}

/// One layer decoded from a preview file: no clip, no effects (they are in the rendered picture).
bool fromPreviewFile(const render::PreparedComposite& c) {
    return c.layers.size() == 1 && c.layers.front().clipId == 0 && c.layers.front().effects.empty() &&
           c.layers.front().frame.isValid();
}

/// The clip composited live: its clip id and its Basic Correction.
bool live(const render::PreparedComposite& c) {
    return c.layers.size() == 1 && c.layers.front().clipId != 0 && !c.layers.front().effects.empty() &&
           c.layers.front().frame.isValid();
}

std::int64_t barcodeOfLayer(const render::PreparedComposite& c) {
    return c.layers.empty() ? -1 : barcodeOf(c.layers.front().frame);
}

double psnr(const render::RenderedImage& a, const render::RenderedImage& b) {
    REQUIRE(a.width == b.width);
    REQUIRE(a.height == b.height);
    double se = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            const double d = static_cast<double>(a.rgba[i + c]) - static_cast<double>(b.rgba[i + c]);
            se += d * d;
            ++n;
        }
    }
    const double mse = se / static_cast<double>(n);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// PSNR of the BT.709 luma of the two pictures.
double lumaPsnr(const render::RenderedImage& a, const render::RenderedImage& b) {
    REQUIRE(a.rgba.size() == b.rgba.size());
    const auto luma = [](const std::vector<std::uint8_t>& px, std::size_t i) {
        return 0.2126 * px[i] + 0.7152 * px[i + 1] + 0.0722 * px[i + 2];
    };
    double se = 0.0;
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        const double d = luma(a.rgba, i) - luma(b.rgba, i);
        se += d * d;
    }
    const double mse = se / static_cast<double>(a.rgba.size() / 4);
    return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// The mean of a - b per colour channel (a level or colour shift between the two pipelines shows here).
std::array<double, 3> meanDifference(const render::RenderedImage& a, const render::RenderedImage& b) {
    REQUIRE(a.rgba.size() == b.rgba.size());
    std::array<double, 3> sum{};
    for (std::size_t i = 0; i < a.rgba.size(); i += 4) {
        for (std::size_t c = 0; c < 3; ++c) {
            sum[c] += static_cast<double>(a.rgba[i + c]) - static_cast<double>(b.rgba[i + c]);
        }
    }
    const double pixels = static_cast<double>(a.rgba.size() / 4);
    for (double& s : sum) {
        s /= pixels;
    }
    return sum;
}

/// The clip on V1 over [0, 2 s) (source frame = sequence frame) with a Basic Correction.
ProjectState withCorrection(const TestProject& project) {
    const std::vector<ClipId> clips{project.clips.front()};
    auto corrected = edit::addEffect(project.state, project.sequence, clips, "wabcut.color.basic",
                                     {{"saturation", ParamValue(140.0)}, {"contrast", ParamValue(15.0)}});
    REQUIRE(corrected);
    return std::move(corrected->state);
}

struct Fixture {
    TestProject project;
    EditSession session;
    PlaybackController controller;
    std::shared_ptr<PreviewCache> cache;

    explicit Fixture(const char* name)
        : project(makeProject({{kClip, 0, 0, 60, 0}}, 1280, 720, kRate30)), session(withCorrection(project)) {
        const fs::path folder = fs::path(WABCUT_TEST_OUTPUT_DIR) / "playback" / "previews" / name;
        std::error_code ec;
        fs::remove_all(folder, ec);
        fs::create_directories(folder);
        cache = std::make_shared<PreviewCache>(folder);
        controller.setSession(&session);
        controller.setSequence(project.sequence);
    }

    SequenceId sequence() const { return project.sequence; }

    /// Renders `range` of `state` (default: the session's) into the cache folder and records it with the hash of the
    /// session's current state, so it counts as valid whatever was rendered. Returns the entry.
    PreviewEntry render(TimeRange range, exporter::VideoSourceFactory source = {}, const ProjectState* state = nullptr) {
        const ProjectSnapshot current = session.snapshot();
        const std::string hash = renderContentHash(*current, sequence(), range);
        const std::string file = PreviewCache::fileNameFor(sequence(), range, hash);
        auto bytes = editor::makeExportPreviewRenderer({.videoSource = std::move(source)})
                         ->render(state ? *state : *current, sequence(), range, cache->folder() / file, {});
        INFO((bytes ? std::string() : bytes.error().message));
        REQUIRE(bytes);
        PreviewEntry entry{sequence(), range, hash, file, *bytes, 0};
        cache->record(entry);
        return entry;
    }

    void usePreviews(bool on) {
        controller.scheduler().setPreview({.previews = on ? cache : nullptr});
    }

    std::shared_ptr<const render::PreparedComposite> show(std::int64_t frame) { return showOn(controller, frame); }
};

} // namespace

TEST_CASE("Preview files: playback shows the rendered file inside its segment and it matches the live picture",
          "[playback][preview][gpu]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    auto renderer = render::OffscreenRenderer::create({.role = render::OutputRole::Display});
    REQUIRE(renderer);
    Fixture f("match");
    // A segment that does not start at 0: preview frame k shows sequence frame 10 + k.
    const TimeRange segment = TimeRange::fromStartEnd(frameAt(10), frameAt(40));
    f.render(segment); // the production renderer: the export's offscreen compositor
    f.usePreviews(true);
    for (const std::int64_t frame : {10, 15, 26, 39}) {
        INFO("sequence frame " << frame);
        const auto c = f.show(frame);
        REQUIRE(fromPreviewFile(*c));
        CHECK(c->layers.front().frame.width == 1280);
        CHECK(c->layers.front().frame.height == 720);
        CHECK(barcodeOfLayer(*c) == frame);
    }
    for (const std::int64_t frame : {0, 9, 40, 50}) {
        INFO("sequence frame " << frame << " (outside the segment)");
        const auto c = f.show(frame);
        CHECK(live(*c));
        CHECK(barcodeOfLayer(*c) == frame);
    }

    // The rendered picture is the live one, up to the intermediate codec (ProRes 422 LT, 10-bit 4:2:2).
    const QSize size(1280, 720);
    double worst = 99.0;
    double worstLuma = 99.0;
    double worstBias = 0.0;
    for (const std::int64_t frame : {12, 25, 38}) {
        f.usePreviews(true);
        const auto preview = f.show(frame);
        REQUIRE(fromPreviewFile(*preview));
        f.usePreviews(false);
        const auto composited = f.show(frame);
        REQUIRE(live(*composited));
        auto a = (*renderer)->render(preview, size);
        auto b = (*renderer)->render(composited, size);
        REQUIRE(a);
        REQUIRE(b);
        const double p = psnr(*a, *b);
        const double y = lumaPsnr(*a, *b);
        const std::array<double, 3> bias = meanDifference(*a, *b);
        UNSCOPED_INFO(std::format("frame {}: preview file vs live composite {:.2f} dB (luma {:.2f} dB), mean difference "
                                  "r {:+.3f} g {:+.3f} b {:+.3f}",
                                  frame, p, y, bias[0], bias[1], bias[2]));
        worst = std::min(worst, p);
        worstLuma = std::min(worstLuma, y);
        for (const double channel : bias) {
            worstBias = std::max(worstBias, std::abs(channel));
        }
    }
    // Measured on this test picture (saturated colour bars, a per-pixel chroma-noise checkerboard, the clip at +40 %
    // saturation): ~33 dB RGB, ~40-42 dB luma, no bias. The difference is chroma only, on the bar edges and the noise
    // (4:2:2 subsampled once more, then ProRes LT): the same path without the correction gives ~41.6 dB (next test).
    WARN(std::format("preview file (ProRes 422 LT) vs live composite: worst {:.2f} dB, luma {:.2f} dB, largest mean "
                     "channel difference {:.3f}",
                     worst, worstLuma, worstBias));
    CHECK(worst >= 30.0);
    CHECK(worstLuma >= 37.0);
    CHECK(worstBias < 0.5); // no shift in level or colour: the export and the monitor composite alike
}

TEST_CASE("Preview files: what plays inside the segment is the file even when it was rendered from other content",
          "[playback][preview][gpu]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    auto renderer = render::OffscreenRenderer::create({.role = render::OutputRole::Display});
    REQUIRE(renderer);
    Fixture f("swap");
    // Other content: the clip one second later in its source and without the correction.
    TestProject other = makeProject({{kClip, 0, 0, 30, 60}}, 1280, 720, kRate30);
    REQUIRE(other.sequence == f.sequence());
    const TimeRange segment{Ticks{}, Ticks::fromSeconds(1)};
    f.render(segment, {}, &other.state); // recorded with the real content's hash: the cache takes it as valid
    REQUIRE(f.cache->validAt(*f.session.snapshot(), f.sequence(), frameAt(15)).has_value());
    f.usePreviews(true);
    const auto swapped = f.show(15);
    REQUIRE(fromPreviewFile(*swapped));
    CHECK(barcodeOfLayer(*swapped) == 45); // the other content's frame, not the live frame 15
    CHECK(barcodeOfLayer(*f.show(0)) == 30);
    CHECK(barcodeOfLayer(*f.show(29)) == 59);
    // Outside the segment the clip plays live.
    const auto after = f.show(50);
    CHECK(live(*after));
    CHECK(barcodeOfLayer(*after) == 50);

    // Pictures: far from the live frame, the same as the other content composited live.
    f.usePreviews(false);
    const auto composited = f.show(15);
    REQUIRE(live(*composited));
    EditSession otherSession(other.state);
    PlaybackController otherController;
    otherController.setSession(&otherSession);
    otherController.setSequence(other.sequence);
    const auto otherLive = showOn(otherController, 15);
    REQUIRE(otherLive->layers.size() == 1);
    CHECK(barcodeOf(otherLive->layers.front().frame) == 45);
    const QSize size(640, 360);
    auto s = (*renderer)->render(swapped, size);
    auto l = (*renderer)->render(composited, size);
    auto o = (*renderer)->render(otherLive, size);
    REQUIRE(s);
    REQUIRE(l);
    REQUIRE(o);
    const double vsLive = psnr(*s, *l);
    const double vsOther = psnr(*s, *o);
    WARN(std::format("file rendered from other content at frame 15: {:.2f} dB vs the live picture, {:.2f} dB vs the other "
                     "content composited live",
                     vsLive, vsOther));
    CHECK(vsLive < 25.0);
    CHECK(vsOther >= 35.0);
}

TEST_CASE("Preview files: an edit inside the segment brings the live picture back until it is undone",
          "[playback][preview]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f("edit");
    const TimeRange segment{Ticks{}, Ticks::fromSeconds(1)};
    // The export's synthetic pattern (no GPU needed): its barcode is the sequence frame too.
    f.render(segment, exporter::syntheticVideoSourceFactory());
    f.usePreviews(true);
    auto c = f.show(15);
    REQUIRE(fromPreviewFile(*c));
    CHECK(barcodeOfLayer(*c) == 15);

    // The clip scaled, and the cache not told: the hash memo is per snapshot, so the file stops playing at once.
    auto scaled = f.session.apply("Scale", [&](const ProjectState& s) {
        ClipProperties p = s.findSequence(f.sequence())->findClip(f.project.clips.front())->clip().properties();
        p.set(ParamId::Scale, 0.9);
        edit::ClipAttributeChange change;
        change.properties = p;
        const std::vector<ClipId> one{f.project.clips.front()};
        return edit::setClipAttributes(s, f.sequence(), one, change);
    });
    REQUIRE(scaled);
    CHECK_FALSE(f.cache->validAt(*f.session.snapshot(), f.sequence(), frameAt(15)).has_value());
    CHECK(f.cache->entries().size() == 1); // still listed: only the hash says it is stale
    for (const std::int64_t frame : {15, 16, 0, 29}) {
        INFO("sequence frame " << frame);
        c = f.show(frame);
        REQUIRE(live(*c));
        CHECK(c->layers.front().transform.scale == Catch::Approx(0.9));
        CHECK(barcodeOfLayer(*c) == frame);
    }

    // Undone, the snapshot shows what the file shows again: it plays again.
    REQUIRE(f.session.undo());
    c = f.show(15);
    CHECK(fromPreviewFile(*c));
    CHECK(barcodeOfLayer(*c) == 15);
    c = f.show(45);
    CHECK(live(*c));

    // Redone, and this time the host tells the cache (its EditSession listener): the entry goes, the live picture stays.
    c = f.show(15);
    REQUIRE(fromPreviewFile(*c));
    const std::optional<ChangeSet> redone = f.session.redo();
    REQUIRE(redone);
    CHECK(f.cache->invalidate(*f.session.snapshot(), *redone).size() == 1);
    CHECK(f.cache->entries().empty());
    c = f.show(15);
    CHECK(live(*c));
}

// On Windows a file the monitor's decoder still has open cannot be deleted: the cache keeps its name and deletes it on a
// later operation once the decoder has let go (PreviewCache::retryDeletes; load() also sweeps files the index does not
// name), so nothing is left behind.
TEST_CASE("Preview files: invalidating a file the monitor has just decoded deletes it once the monitor lets go", "[playback][preview]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f("open-delete");
    const PreviewEntry entry = f.render(TimeRange{Ticks{}, Ticks::fromSeconds(1)}, exporter::syntheticVideoSourceFactory());
    const fs::path file = f.cache->folder() / entry.file;
    auto scaled = f.session.apply("Scale", [&](const ProjectState& s) {
        ClipProperties p = s.findSequence(f.sequence())->findClip(f.project.clips.front())->clip().properties();
        p.set(ParamId::Scale, 0.9);
        edit::ClipAttributeChange change;
        change.properties = p;
        const std::vector<ClipId> one{f.project.clips.front()};
        return edit::setClipAttributes(s, f.sequence(), one, change);
    });
    REQUIRE(scaled);
    {
        // A monitor that has the file open while the edit invalidates it.
        REQUIRE(f.session.undo());
        PlaybackController monitor;
        monitor.setSession(&f.session);
        monitor.setSequence(f.sequence());
        monitor.scheduler().setPreview({.previews = f.cache});
        REQUIRE(fromPreviewFile(*showOn(monitor, 15)));
        const std::optional<ChangeSet> redone = f.session.redo();
        REQUIRE(redone);
        REQUIRE(f.cache->invalidate(*f.session.snapshot(), *redone).size() == 1);
        CHECK(live(*showOn(monitor, 15)));
    } // the monitor closes its decoders
    f.controller.setSession(nullptr);
    std::this_thread::sleep_for(200ms);
    CHECK(f.cache->retryDeletes() == 0);
    CHECK_FALSE(fs::exists(file));
    // A file the index does not name (a delete that failed before a restart) goes when the cache loads.
    const fs::path orphan = f.cache->folder() / "53-0-0123456789abcdef.wpv";
    std::ofstream(orphan, std::ios::binary) << "old preview";
    const fs::path partial = f.cache->folder() / "53-0-0123456789abcdef.wabcut-partial-1a2b.wpv";
    std::ofstream(partial, std::ios::binary) << "a render in progress";
    REQUIRE(f.cache->save());
    REQUIRE(f.cache->load());
    CHECK_FALSE(fs::exists(orphan));
    CHECK(fs::exists(partial));
}

// A file that exists but cannot be decoded (damaged, truncated): the scheduler drops it (decoder, entry, file) and
// composites live, the C.15 contract ("on a missing or unreadable file, fall back to live rendering").
TEST_CASE("Preview files: an unreadable file plays the live picture", "[playback][preview]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f("unreadable");
    const TimeRange segment{Ticks{}, Ticks::fromSeconds(1)};
    const std::string hash = renderContentHash(*f.session.snapshot(), f.sequence(), segment);
    const std::string name = PreviewCache::fileNameFor(f.sequence(), segment, hash);
    {
        std::ofstream out(f.cache->folder() / name, std::ios::binary | std::ios::trunc);
        out << std::string(4096, 'x'); // not a movie
    }
    f.cache->record(PreviewEntry{f.sequence(), segment, hash, name, 4096, 0});
    f.usePreviews(true);
    const auto c = f.show(15);
    INFO("layers " << c->layers.size());
    CHECK(live(*c));
    CHECK(barcodeOfLayer(*c) == 15);
    CHECK(f.cache->entries().empty()); // the broken entry is gone
}

TEST_CASE("Preview files: a deleted file plays the live picture without waiting for it", "[playback][preview]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    const std::vector<std::int64_t> frames{15, 3, 29};
    // The same seeks without previews, from a fresh controller (decoders opened, nothing cached): the baseline.
    double baseline = 0.0;
    {
        Fixture plain("deleted-baseline");
        const auto start = std::chrono::steady_clock::now();
        for (const std::int64_t frame : frames) {
            REQUIRE(live(*plain.show(frame)));
        }
        baseline = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    Fixture f("deleted");
    const TimeRange segment{Ticks{}, Ticks::fromSeconds(1)};
    const PreviewEntry entry = f.render(segment, exporter::syntheticVideoSourceFactory());
    const fs::path file = f.cache->folder() / entry.file;
    const fs::path kept = f.cache->folder() / "kept.bin";
    fs::copy_file(file, kept);
    REQUIRE(fs::remove(file));
    f.usePreviews(true);
    const auto start = std::chrono::steady_clock::now();
    for (const std::int64_t frame : frames) {
        INFO("sequence frame " << frame);
        const auto c = f.show(frame); // fails after 5 s without a picture
        REQUIRE(live(*c));
        CHECK(barcodeOfLayer(*c) == frame);
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    WARN(std::format("three seeks into a segment whose file is gone: {:.2f} s (without previews: {:.2f} s)", seconds,
                     baseline));
    CHECK(seconds < 1.5 * baseline + 1.0); // nothing waits for the missing file

    // The file rendered again: setting the previews again (PreviewSettings::previews) picks it up.
    fs::copy_file(kept, file);
    f.usePreviews(true);
    const auto c = f.show(15);
    CHECK(fromPreviewFile(*c));
    CHECK(barcodeOfLayer(*c) == 15);
}

// Integration review (a): a file rendered again under the same name (the same content after its entry went) must play
// the new render, not frames of a decoder the monitor kept for the old one.
TEST_CASE("Preview files: a file rendered again under the same name plays the new render", "[playback][preview]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f("rerender");
    TestProject other = makeProject({{kClip, 0, 0, 30, 60}}, 1280, 720, kRate30);
    REQUIRE(other.sequence == f.sequence());
    const TimeRange segment{Ticks{}, Ticks::fromSeconds(1)};
    const PreviewEntry first = f.render(segment, {}, &other.state);
    f.usePreviews(true);
    REQUIRE(barcodeOfLayer(*f.show(15)) == 45);
    // The entry goes (render.deletePreviewFiles); setting the cache again tells the monitor, which lets go of the file.
    f.cache->remove(f.sequence());
    f.usePreviews(true);
    CHECK(live(*f.show(15)));
    std::this_thread::sleep_for(200ms);
    CHECK(f.cache->retryDeletes() == 0);
    // The same content renders again under the same name.
    const PreviewEntry second = f.render(segment);
    REQUIRE(second.file == first.file);
    f.usePreviews(true);
    const auto c = f.show(15);
    REQUIRE(fromPreviewFile(*c));
    CHECK(barcodeOfLayer(*c) == 15);
}

// Integration review (c): burned-in captions are drawn once over a preview file. Preview files are rendered without
// captions and only the top sequence's caption track is ever burned in (a nested sequence's captions are not), so the
// caption layer on top of the file is the only one.
TEST_CASE("Preview files: burned-in captions are drawn once on top of the file", "[playback][preview]") {
    if (!haveMedia(kClip)) {
        SKIP("test media not generated");
    }
    Fixture f("captions");
    ProjectState next = *f.session.snapshot();
    CaptionTrack track;
    track.name = "Subtitles";
    track.cues = {CaptionCue{Ticks{}, Ticks::fromSeconds(2), "GG WP", {}}};
    auto withCaptions = edit::setCaptionTrack(next, f.sequence(), track);
    REQUIRE(withCaptions);
    REQUIRE(f.session.commit("captions", withCaptions->state).has_value());
    f.controller.scheduler().setCaptions(CaptionSettings{});
    const auto liveFrame = f.show(15);
    REQUIRE(liveFrame->layers.size() == 2); // the clip, the caption
    CHECK(liveFrame->layers.back().view.has_value());
    f.render(TimeRange{Ticks{}, Ticks::fromSeconds(1)});
    f.usePreviews(true);
    const auto c = f.show(15);
    REQUIRE(c->layers.size() == 2); // the file, the caption: once
    CHECK(c->layers.front().clipId == 0);
    CHECK(c->layers.front().frame.isValid());
    CHECK(c->layers.back().view.has_value());
}

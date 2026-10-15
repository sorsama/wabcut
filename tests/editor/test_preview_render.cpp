// C.16 preview files in the editor (PreviewRender.h over the C.15 preview cache, PreviewCache.h): a segment renders
// through the export path into "<project> Preview Files" as ProRes 422 LT (or DNxHR LB) at the sequence's size and
// rate, the cache records it and shows it Rendered, validAt finds it inside and nothing outside, the index survives a
// save and a load; an edit inside the segment drops the entry and its file while an edit outside (and a marker inside)
// keeps it; a cancelled render leaves nothing behind, not even the export's partial file.
//
// The ProRes case renders with the production video source (the offscreen compositor: [gpu]); the others with the
// export's synthetic source, so they run without a GPU.

#include <wabcut/editor/MediaImport.h>
#include <wabcut/editor/PreviewRender.h>
#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/editor/SourceEdit.h>

#include <wabcut/export/ExportJob.h>
#include <wabcut/export/ExportSettings.h>
#include <wabcut/export/VideoFrameSource.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/EditOps.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/model/PreviewCache.h>
#include <wabcut/model/ProjectOps.h>

#include <QGuiApplication>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace wabcut;
using namespace wabcut::editor;
namespace fs = std::filesystem;

namespace {

fs::path testMedia(const char* name) { return fs::path(WABCUT_TEST_MEDIA_DIR) / name; }

bool haveMedia() { return fs::exists(testMedia("h264_1080p60_aac.mp4")); }

fs::path workDir(const char* name) {
    const fs::path dir = fs::path(WABCUT_TEST_OUTPUT_DIR) / "editor" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

void ensureGui() {
    if (!QGuiApplication::instance()) {
        static int argc = 1;
        static char name[] = "wabcut_editor_tests";
        static char* argv[] = {name, nullptr};
        static QGuiApplication app(argc, argv);
    }
}

/// The first segment of the render bar: V1's first clip.
constexpr TimeRange kSegment{Ticks{}, Ticks::fromSeconds(1)};
/// Half way through it.
constexpr Ticks kMiddle{kTicksPerSecond / 2};

/// A saved project (1280x720 30 fps) with two clips of the H.264 clip on V1: [0, 1 s) (source 0..1 s) and [1 s, 2 s)
/// (source 2..3 s), with their audio on A1. The render bar is [0, 1 s), [1 s, 2 s).
struct PreviewProject {
    fs::path file;
    SequenceId sequence;
    ClipId first;
    ClipId second;
};

ClipId videoClipOf(const EditSession& session, SequenceId sequence, const std::vector<ClipId>& placed) {
    for (const ClipId id : placed) {
        const auto loc = session.snapshot()->findSequence(sequence)->findClip(id);
        if (loc && loc->clip().stream.kind == StreamKind::Video) {
            return id;
        }
    }
    return {};
}

PreviewProject makePreviewProject(EditSession& session, ProjectDocument& doc, const fs::path& dir) {
    edit::SequenceSettings settings;
    settings.width = 1280;
    settings.height = 720;
    settings.frameRate = kRate30;
    auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    PreviewProject p;
    p.sequence = created->id;
    session.reset(std::move(created->state));
    p.file = dir / "Preview Test.wabcut";
    REQUIRE(doc.saveAs(p.file));
    const std::vector<fs::path> files{testMedia("h264_1080p60_aac.mp4")};
    const MediaImportResult imported = importMediaFiles(session, files, {.projectFile = p.file});
    REQUIRE(imported.added.size() == 1);
    const MediaId media = imported.added.front();
    auto first = dropMedia(session, {.media = media, .sequence = p.sequence, .at = Ticks{},
                                     .sourceRange = TimeRange{Ticks{}, Ticks::fromSeconds(1)}});
    REQUIRE(first);
    p.first = videoClipOf(session, p.sequence, first->placed);
    auto second = dropMedia(session, {.media = media, .sequence = p.sequence, .at = Ticks::fromSeconds(1),
                                      .sourceRange = TimeRange{Ticks::fromSeconds(2), Ticks::fromSeconds(1)}});
    REQUIRE(second);
    p.second = videoClipOf(session, p.sequence, second->placed);
    REQUIRE(p.first.isValid());
    REQUIRE(p.second.isValid());
    REQUIRE(doc.save());
    return p;
}

/// A clip's properties with one value changed, as one undo step.
void setProperty(EditSession& session, SequenceId sequence, ClipId clip, ParamId param, double value) {
    auto r = session.apply("Set property", [&](const ProjectState& s) {
        ClipProperties p = s.findSequence(sequence)->findClip(clip)->clip().properties();
        p.set(param, value);
        edit::ClipAttributeChange change;
        change.properties = p;
        const std::vector<ClipId> one{clip};
        return edit::setClipAttributes(s, sequence, one, change);
    });
    REQUIRE(r);
}

std::vector<std::string> filesIn(const fs::path& folder) {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec)) {
        names.push_back(pathToUtf8(e.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

/// The export's synthetic test pattern, `delay` slower per frame (so a render takes long enough to be cancelled half
/// way through).
class SlowSource final : public exporter::IVideoFrameSource {
public:
    SlowSource(std::unique_ptr<exporter::IVideoFrameSource> inner, std::chrono::milliseconds delay)
        : m_inner(std::move(inner)), m_delay(delay) {}
    std::string description() const override { return "slow " + m_inner->description(); }
    int maxInFlight() const override { return m_inner->maxInFlight(); }
    std::expected<void, Error> request(std::int64_t frameIndex) override { return m_inner->request(frameIndex); }
    std::expected<media::FrameHandle, Error> next() override {
        std::this_thread::sleep_for(m_delay);
        return m_inner->next();
    }
    void stop() override { m_inner->stop(); }

private:
    std::unique_ptr<exporter::IVideoFrameSource> m_inner;
    std::chrono::milliseconds m_delay;
};

exporter::VideoSourceFactory slowSyntheticSource(std::chrono::milliseconds delay) {
    return [delay](const ProjectState&, const Sequence&, const exporter::VideoSourceFormat& wanted)
               -> std::expected<std::unique_ptr<exporter::IVideoFrameSource>, Error> {
        std::unique_ptr<exporter::IVideoFrameSource> source =
            std::make_unique<SlowSource>(exporter::makeSyntheticVideoSource(wanted), delay);
        return source;
    };
}

/// Renders kSegment of the session's current state into the cache folder under the cache's name for it and records
/// it. Returns the entry.
PreviewEntry renderAndRecord(PreviewCache& cache, const EditSession& session, SequenceId sequence,
                             const PreviewRenderOptions& options) {
    const ProjectSnapshot state = session.snapshot();
    const std::string hash = renderContentHash(*state, sequence, kSegment);
    REQUIRE(hash.size() == 16);
    const std::string name = PreviewCache::fileNameFor(sequence, kSegment, hash);
    auto bytes = makeExportPreviewRenderer(options)->render(*state, sequence, kSegment, cache.folder() / name, {});
    INFO((bytes ? std::string() : bytes.error().message));
    REQUIRE(bytes);
    PreviewEntry entry{sequence, kSegment, hash, name, *bytes, 0};
    CHECK(cache.record(entry).empty());
    return entry;
}

} // namespace

TEST_CASE("Preview render: a segment renders to ProRes LT in the project's preview folder and shows as Rendered",
          "[editor][preview][gpu]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    ensureGui();
    const fs::path dir = workDir("preview_render");
    EditSession session;
    ProjectDocument doc(session);
    const PreviewProject p = makePreviewProject(session, doc, dir);
    const fs::path folder = previewFolderFor(p.file);
    CHECK(folder == dir / "Preview Test Preview Files");
    REQUIRE_FALSE(fs::exists(folder)); // a new project: the first render makes the folder
    PreviewCache cache(folder);
    REQUIRE(cache.load()); // a missing index is an empty cache

    const ProjectSnapshot state = session.snapshot();
    const std::vector<RenderSegment> segments = renderSegments(*state, p.sequence);
    REQUIRE(segments.size() == 2);
    REQUIRE(segments[0].range == kSegment);
    const std::string hash = renderContentHash(*state, p.sequence, kSegment);
    const std::string name = PreviewCache::fileNameFor(p.sequence, kSegment, hash);
    std::vector<double> fractions;
    auto renderer = makeExportPreviewRenderer({.projectFile = p.file});
    const auto bytes = renderer->render(*state, p.sequence, kSegment, folder / name, [&](double fraction) {
        fractions.push_back(fraction);
        return true;
    });
    INFO((bytes ? std::string() : bytes.error().message));
    REQUIRE(bytes);
    REQUIRE(fs::is_regular_file(folder / name));
    CHECK(*bytes == static_cast<std::int64_t>(fs::file_size(folder / name)));
    CHECK(filesIn(folder) == std::vector<std::string>{name}); // no partial file left over
    REQUIRE_FALSE(fractions.empty());
    CHECK(std::ranges::is_sorted(fractions));
    CHECK(fractions.front() >= 0.0);
    CHECK(fractions.back() == Catch::Approx(1.0));

    // The file: ProRes 422 LT, 10-bit 4:2:2, the sequence's size and rate, every frame of the segment, no sound.
    const auto info = media::probe(pathToUtf8(folder / name));
    REQUIRE(info);
    REQUIRE(info->video.size() == 1);
    CHECK(info->audio.empty());
    const VideoStreamInfo& video = info->video.front();
    INFO("profile '" << video.profile << "', pixel format " << video.pixelFormat << ", " << video.frameCount << " frames");
    CHECK(video.codec == "prores");
    CHECK(video.profile == "LT");
    CHECK(video.width == 1280);
    CHECK(video.height == 720);
    CHECK(video.bitDepth == 10);
    CHECK(video.chromaSubsampling == "4:2:2");
    CHECK(video.frameRate == kRate30);
    CHECK(video.frameCount == 30);
    CHECK(video.duration == kSegment.duration);

    // Recorded, it is the green part of the render bar and what playback switches to inside it.
    CHECK(cache.record(PreviewEntry{p.sequence, kSegment, hash, name, *bytes, 0}).empty());
    const std::vector<RenderSegment> bar = cache.renderBar(*state, p.sequence);
    REQUIRE(bar.size() == 2);
    CHECK(bar[0].status == RenderStatus::Rendered);
    CHECK(bar[1].status != RenderStatus::Rendered);
    for (const Ticks t : {Ticks{}, kMiddle, framesToTicks(29, kRate30)}) {
        const auto hit = cache.validAt(*state, p.sequence, t);
        REQUIRE(hit);
        CHECK(hit->file == name);
        CHECK(hit->range == kSegment);
    }
    for (const Ticks t : {Ticks::fromSeconds(1), framesToTicks(45, kRate30), Ticks::fromSeconds(5)}) {
        CHECK_FALSE(cache.validAt(*state, p.sequence, t).has_value());
    }
    CHECK_FALSE(cache.validAt(*state, SequenceId{p.sequence.value + 1000}, Ticks{}).has_value());

    // The index survives a save and a load (a reopened project).
    REQUIRE(cache.save());
    CHECK(fs::exists(folder / std::string(PreviewCache::kIndexFile)));
    PreviewCache reopened(folder);
    REQUIRE(reopened.load());
    const std::vector<PreviewEntry> loadedEntries = reopened.entries();
    REQUIRE(loadedEntries.size() == 1);
    const PreviewEntry& loaded = loadedEntries.front();
    CHECK(loaded.sequence == p.sequence);
    CHECK(loaded.range == kSegment);
    CHECK(loaded.hash == hash);
    CHECK(loaded.file == name);
    CHECK(loaded.bytes == *bytes);
    CHECK(reopened.validAt(*state, p.sequence, kMiddle).has_value());
    CHECK(reopened.renderBar(*state, p.sequence)[0].status == RenderStatus::Rendered);
}

TEST_CASE("Preview render: an edit inside a segment drops its file and an edit outside keeps it", "[editor][preview]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("preview_invalidate");
    EditSession session;
    ProjectDocument doc(session);
    const PreviewProject p = makePreviewProject(session, doc, dir);
    PreviewCache cache(previewFolderFor(p.file));
    const PreviewEntry entry =
        renderAndRecord(cache, session, p.sequence, {.videoSource = exporter::syntheticVideoSourceFactory(), .projectFile = p.file});
    const fs::path file = cache.folder() / entry.file;
    REQUIRE(fs::exists(file));

    // The host keeps the cache in step with every edit, undo and redo (PreviewRender.h).
    std::vector<PreviewEntry> dropped;
    int events = 0;
    const auto listener = session.addListener([&](const ChangeEvent& e) {
        ++events;
        dropped = cache.invalidate(*e.state, e.changes);
    });

    // A clip elsewhere (V1 [1 s, 2 s)) at half opacity: outside the segment.
    setProperty(session, p.sequence, p.second, ParamId::Opacity, 0.5);
    REQUIRE(events == 1);
    CHECK(dropped.empty());
    CHECK(fs::exists(file));
    CHECK(cache.entries().size() == 1);
    CHECK(cache.validAt(*session.snapshot(), p.sequence, kMiddle).has_value());
    CHECK(cache.renderBar(*session.snapshot(), p.sequence)[0].status == RenderStatus::Rendered);

    // A marker inside the segment changes nothing visible.
    auto marked = session.apply("Add Marker", [&](const ProjectState& s) {
        Marker marker;
        marker.start = kMiddle;
        marker.name = "inside";
        return edit::addMarker(s, p.sequence, marker);
    });
    REQUIRE(marked);
    REQUIRE(events == 2);
    CHECK(dropped.empty());
    CHECK(fs::exists(file));
    CHECK(cache.validAt(*session.snapshot(), p.sequence, kMiddle).has_value());

    // The clip under the segment scaled: the entry goes and so does its file.
    setProperty(session, p.sequence, p.first, ParamId::Scale, 0.8);
    REQUIRE(events == 3);
    REQUIRE(dropped.size() == 1);
    CHECK(dropped.front().file == entry.file);
    CHECK_FALSE(fs::exists(file));
    CHECK(cache.entries().empty());
    CHECK_FALSE(cache.validAt(*session.snapshot(), p.sequence, kMiddle).has_value());
    CHECK(cache.renderBar(*session.snapshot(), p.sequence)[0].status != RenderStatus::Rendered);

    // Rendered again at the new content, then an opacity change of that clip drops it too.
    const PreviewEntry again =
        renderAndRecord(cache, session, p.sequence, {.videoSource = exporter::syntheticVideoSourceFactory(), .projectFile = p.file});
    CHECK(again.hash != entry.hash);
    REQUIRE(fs::exists(cache.folder() / again.file));
    setProperty(session, p.sequence, p.first, ParamId::Opacity, 0.5);
    REQUIRE(dropped.size() == 1);
    CHECK_FALSE(fs::exists(cache.folder() / again.file));
    CHECK(cache.entries().empty());
    session.removeListener(listener);
}

TEST_CASE("Preview render: a cancelled render fails as Cancelled and leaves no file", "[editor][preview]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("preview_cancel");
    EditSession session;
    ProjectDocument doc(session);
    const PreviewProject p = makePreviewProject(session, doc, dir);
    const fs::path folder = previewFolderFor(p.file);
    fs::create_directories(folder);
    const TimeRange both{Ticks{}, Ticks::fromSeconds(2)}; // 60 frames, ~20 ms each
    const ProjectSnapshot state = session.snapshot();
    const std::string name = PreviewCache::fileNameFor(p.sequence, both, renderContentHash(*state, p.sequence, both));
    auto renderer = makeExportPreviewRenderer({.videoSource = slowSyntheticSource(std::chrono::milliseconds(20)), .projectFile = p.file});
    bool sawPartial = false;
    double stoppedAt = -1.0;
    const auto result = renderer->render(*state, p.sequence, both, folder / name, [&](double fraction) {
        if (fraction < 0.25) {
            return true;
        }
        for (const std::string& f : filesIn(folder)) {
            sawPartial = sawPartial || f.find(".wabcut-partial-") != std::string::npos;
        }
        stoppedAt = fraction;
        return false;
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().code == ErrorCode::Cancelled);
    CHECK(stoppedAt >= 0.25);
    CHECK(stoppedAt < 1.0);
    CHECK(sawPartial); // the export was writing its temporary file when it was stopped...
    CHECK(filesIn(folder).empty()); // ...and removed it: no preview file, no partial file

    // An empty segment is refused before anything is written.
    const auto empty = renderer->render(*state, p.sequence, TimeRange{Ticks{}, Ticks{}}, folder / name, {});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == ErrorCode::InvalidArgument);
    CHECK(filesIn(folder).empty());
}

TEST_CASE("Preview render: DNxHR LB renders too", "[editor][preview]") {
    if (!haveMedia()) {
        SKIP("test media not generated");
    }
    const fs::path dir = workDir("preview_dnxhr");
    EditSession session;
    ProjectDocument doc(session);
    const PreviewProject p = makePreviewProject(session, doc, dir);
    PreviewCache cache(previewFolderFor(p.file));
    const PreviewEntry entry = renderAndRecord(cache, session, p.sequence,
                                               {.codec = PreviewCodec::DnxhrLb,
                                                .videoSource = exporter::syntheticVideoSourceFactory(),
                                                .projectFile = p.file});
    const auto info = media::probe(pathToUtf8(cache.folder() / entry.file));
    REQUIRE(info);
    REQUIRE(info->video.size() == 1);
    CHECK(info->audio.empty());
    const VideoStreamInfo& video = info->video.front();
    INFO("codec " << video.codec << ", profile '" << video.profile << "', pixel format " << video.pixelFormat);
    CHECK((video.codec == "dnxhd" || video.codec == "dnxhr"));
    CHECK(video.profile == "DNXHR LB");
    CHECK(video.width == 1280);
    CHECK(video.height == 720);
    CHECK(video.bitDepth == 8);
    CHECK(video.chromaSubsampling == "4:2:2");
    CHECK(video.frameRate == kRate30);
    CHECK(video.frameCount == 30);
    CHECK(cache.renderBar(*session.snapshot(), p.sequence)[0].status == RenderStatus::Rendered);
}

TEST_CASE("Preview render: the preview folder name and the export settings of both codecs", "[editor][preview]") {
    CHECK(previewFolderFor(fs::path("C:/Work/My Film.wabcut")) == fs::path("C:/Work/My Film Preview Files"));
    CHECK(previewFolderFor(fs::path("projects/cut.v2.wabcut")) == fs::path("projects/cut.v2 Preview Files"));
    CHECK(previewFolderFor(fs::path("solo.wabcut")) == fs::path("solo Preview Files"));

    edit::SequenceSettings settings;
    settings.width = 1280;
    settings.height = 720;
    settings.frameRate = kRate29_97;
    auto created = edit::addSequence(ProjectState{}, settings);
    REQUIRE(created);
    const Sequence& sequence = *created->state.findSequence(created->id);
    const TimeRange range{framesToTicks(10, kRate29_97), framesToTicks(45, kRate29_97)};
    for (const PreviewCodec codec : {PreviewCodec::ProResLt, PreviewCodec::DnxhrLb}) {
        const exporter::ExportSettings s = previewExportSettings(codec, range);
        INFO(s.presetId);
        const auto valid = exporter::validate(s);
        INFO((valid ? std::string() : valid.error().message));
        CHECK(valid);
        CHECK(s.container == exporter::Container::Mov);
        CHECK(s.video.enabled);
        CHECK_FALSE(s.audio.enabled);
        CHECK(s.captions.mode == exporter::CaptionExport::None);
        CHECK(s.range == exporter::RangeMode::Custom);
        CHECK(s.customRange == range);
        CHECK_FALSE(s.video.width.has_value()); // the sequence's size and rate
        CHECK_FALSE(s.video.height.has_value());
        CHECK_FALSE(s.video.frameRate.has_value());
        if (codec == PreviewCodec::ProResLt) {
            CHECK(s.video.codec == exporter::VideoCodec::ProRes);
            CHECK(s.video.proresProfile == exporter::ProResProfile::Lt);
            CHECK(s.video.pixelFormat == exporter::OutputPixelFormat::Yuv422p10);
        } else {
            CHECK(s.video.codec == exporter::VideoCodec::DnxHR);
            CHECK(s.video.dnxhrProfile == exporter::DnxhrProfile::Lb);
            CHECK(s.video.pixelFormat == exporter::OutputPixelFormat::Yuv422p);
        }
        const auto resolved = exporter::resolve(s, sequence);
        REQUIRE(resolved);
        CHECK(resolved->width == 1280);
        CHECK(resolved->height == 720);
        CHECK(resolved->frameRate == kRate29_97);
        CHECK(resolved->range == range);
        CHECK(resolved->frameCount == 45);
    }
}

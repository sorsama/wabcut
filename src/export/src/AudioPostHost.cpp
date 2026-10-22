// The host side of AAF / OMF export (AudioPostHost.h): clips rendered through the playback path, and the reference
// video through an ExportJob.

#include <wabcut/export/AudioPostHost.h>

#include <wabcut/command/AudioPostServices.h>
#include <wabcut/command/CaptionFileServices.h>
#include <wabcut/export/CaptionSidecar.h>

#include <wabcut/audio/effects/EffectChain.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/MediaLocator.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>
#include <wabcut/playback/audio/TrackAudio.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <algorithm>
#include <format>
#include <vector>

namespace wabcut::exporter {

namespace {

/// `project` with the media `track` plays (through nests too) pointing at their original files, proxies dropped.
/// Errors: NotFound naming what is offline.
std::expected<ProjectState, Error> withOriginals(const ProjectState& project, const Track& track,
                                                 const std::filesystem::path& projectFile) {
    ProjectState out = project;
    const std::vector<MediaId> heard = playback::mediaHeardOn(project, track);
    std::vector<std::string> missing;
    for (MediaPtr& item : out.media) {
        if (!item || item->isGenerator() || !std::binary_search(heard.begin(), heard.end(), item->id)) {
            continue;
        }
        const auto path = item->locator.offline ? std::nullopt : resolveMediaPath(item->locator, projectFile);
        if (!path) {
            missing.push_back(item->locator.path.empty() ? item->name : std::format("{} ({})", item->name, item->locator.path));
            continue;
        }
        auto copy = std::make_shared<MediaItem>(*item);
        copy->locator.path = pathToUtf8(*path);
        copy->locator.proxy.reset();
        item = std::move(copy);
    }
    if (!missing.empty()) {
        std::string list;
        for (const std::string& m : missing) {
            list += (list.empty() ? "" : ", ") + m;
        }
        return std::unexpected(Error{ErrorCode::NotFound, "Original media offline or missing: " + list});
    }
    return out;
}

} // namespace

ClipAudioRenderer::ClipAudioRenderer(std::filesystem::path projectFile) : m_projectFile(std::move(projectFile)) {}

std::expected<void, Error> ClipAudioRenderer::render(const ProjectState& project,
                                                     const interchange::AudioPostRenderRequest& request,
                                                     const std::function<bool(std::span<const float>)>& sink) {
    const Sequence* seq = project.findSequence(request.sequence);
    const Track* track = seq ? seq->findTrack(request.track) : nullptr;
    const Clip* clip = track ? track->findClip(request.clip) : nullptr;
    if (!clip || request.channels < 1 || request.channels > 32 || request.sampleRate <= 0 ||
        request.range.duration.value < 0) {
        return std::unexpected(Error{ErrorCode::NotFound, "The clip to render does not exist"});
    }
    const Rate rate{request.sampleRate, 1};
    const std::int64_t frames = ticksToSamples(request.range.duration, rate);

    // A track holding just this clip, stretched over the range (its media continues into the handles), placed so the
    // range starts at 0: no track effects, fader or panner, no transitions, no fades at its ends.
    Clip c = *clip;
    const Ticks head = clip->start() - request.range.start;
    const Ticks tail = request.range.end() - clip->end();
    if (head.value > 0) {
        c.moveHead(Ticks{-head.value});
    }
    if (tail.value > 0) {
        c.moveTail(tail);
    }
    {
        ClipContent content = c.content();
        content.properties.set(ParamId::VolumeDb, 0.0); // the file carries the clip's gain
        if (!request.clipEffects) {
            content.effects.clear();
        }
        c.setContent(std::move(content));
    }
    c.timelineRange.start = c.timelineRange.start - request.range.start;
    c.linkGroup = {};
    auto solo = std::make_shared<Track>();
    solo->id = track->id;
    solo->kind = TrackKind::Audio;
    solo->format = track->format;
    solo->clips.push_back(std::move(c));

    auto originals = withOriginals(project, *solo, m_projectFile);
    if (!originals) {
        return std::unexpected(originals.error());
    }
    const EditSession session(std::move(*originals));
    playback::MediaAudioFeed feed(session);
    playback::TrackRendererOptions options;
    options.edgeFade = Ticks{};
    options.trackFader = false;
    playback::TrackRenderer renderer(request.channels, rate, options);
    renderer.setProject(session.snapshot());
    renderer.setRedactions(playback::AudioRedactions::of(*session.snapshot()));
    renderer.setAudioTakes(playback::AudioTakes::of(*session.snapshot()));
    const audio::ScopedOfflineRender offline;
    playback::TrackRenderStats stats;
    constexpr std::int64_t kBlock = 4096;
    std::vector<float> block;
    for (std::int64_t at = 0; at < frames; at += kBlock) {
        const std::int64_t n = std::min(kBlock, frames - at);
        block.assign(static_cast<std::size_t>(n * request.channels), 0.0f);
        renderer.render(*solo, samplesToTicks(at, rate), block, feed, stats);
        if (!sink(block)) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Export cancelled"});
        }
    }
    if (stats.feedErrors > 0 && feed.stats().opens == 0) {
        return std::unexpected(Error{ErrorCode::DecoderError, "The clip's media could not be decoded"});
    }
    return {};
}

ExportSettings referenceVideoSettings() {
    ExportSettings s;
    s.presetId = "aaf-reference-video";
    s.container = Container::Mov;
    s.video.codec = VideoCodec::DnxHR;
    s.video.encoder = "auto";
    s.video.dnxhrProfile = DnxhrProfile::Lb;
    s.video.pixelFormat = OutputPixelFormat::Yuv422p;
    s.audio.enabled = true;
    s.audio.codec = AudioCodec::Pcm24;
    s.audio.sampleRate = 48000;
    s.audio.channels = 2;
    s.range = RangeMode::WholeSequence;
    return s;
}

ReferenceVideoRenderer::ReferenceVideoRenderer(std::filesystem::path projectFile, VideoSourceFactory videoSource)
    : m_projectFile(std::move(projectFile)), m_videoSource(std::move(videoSource)) {}

std::expected<std::filesystem::path, Error> ReferenceVideoRenderer::render(const ProjectState& project, SequenceId sequence,
                                                                          const std::filesystem::path& exportFile,
                                                                          const std::function<bool(double)>& progress) {
    ExportRequest request;
    request.snapshot = std::make_shared<const ProjectState>(project);
    request.sequence = sequence;
    request.settings = referenceVideoSettings();
    request.output = exportFile.parent_path() / pathFromUtf8(pathToUtf8(exportFile.stem()) + " Reference.mov");
    request.projectFile = m_projectFile;
    request.videoSource = m_videoSource;
    std::stop_source stop;
    request.onProgress = [&](const ExportProgress& p) {
        if (progress && !progress(p.fraction())) {
            stop.request_stop();
        }
    };
    ExportJob job(std::move(request));
    auto result = job.run(stop.get_token());
    if (!result) {
        return std::unexpected(result.error());
    }
    return result->output;
}

std::shared_ptr<command::CommandServices> interchangeServices(const std::filesystem::path& projectFile) {
    auto audioPost = std::make_shared<command::AudioPostServices>(std::make_shared<ClipAudioRenderer>(projectFile),
                                                                  std::make_shared<ReferenceVideoRenderer>(projectFile));
    auto captions = std::make_shared<command::CaptionFileServices>();
    captions->redact = [](const ProjectState& project, const Sequence& sequence, const CaptionTrack& track) {
        return captionTrackForFile(project, sequence, track);
    };
    return std::make_shared<command::ServiceBundle>(
        std::vector<std::shared_ptr<command::CommandServices>>{std::move(audioPost), std::move(captions)});
}

} // namespace wabcut::exporter

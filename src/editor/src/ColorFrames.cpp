#include <wabcut/editor/ColorFrames.h>

#include <wabcut/editor/ProjectDocument.h>
#include <wabcut/export/RenderedVideoSource.h>
#include <wabcut/model/FrameScaling.h>
#include <wabcut/render/PreparedComposite.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wabcut::editor {

ProjectState soloClipState(const ProjectState& state, SequenceId sequence, ClipId clip, EffectInstanceId stopBefore) {
    ProjectState out = state;
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return out;
    }
    auto solo = std::make_shared<Sequence>(*seq);
    solo->captionTracks.clear();
    if (stopBefore.isValid()) {
        solo->effects.clear(); // the grade's input: nothing after the clip's own stack
        solo->audioEffects.clear();
    }
    for (std::vector<TrackPtr>* list : {&solo->videoTracks, &solo->audioTracks}) {
        for (TrackPtr& trackPtr : *list) {
            auto track = std::make_shared<Track>(*trackPtr);
            track->transitions.clear();
            if (stopBefore.isValid()) {
                track->effects.clear();
            }
            std::vector<Clip> keep;
            for (const Clip& c : trackPtr->clips) {
                if (c.id != clip) {
                    continue;
                }
                Clip copy = c;
                if (stopBefore.isValid()) {
                    EffectStack stack;
                    for (const EffectInstance& e : c.effects()) {
                        if (e.id == stopBefore) {
                            break;
                        }
                        stack.push_back(e);
                    }
                    copy.setEffects(std::move(stack));
                }
                keep.push_back(std::move(copy));
            }
            track->clips = ClipList(std::move(keep));
            trackPtr = std::move(track);
        }
    }
    return out.withSequence(std::move(solo));
}

std::expected<PosterImage, Error> renderSequenceFrame(const ProjectState& state, SequenceId sequence, Ticks time, int width,
                                                      const std::filesystem::path& projectFile) {
    const ProjectState checked = withMediaChecked(state, projectFile);
    const Sequence* seq = checked.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    const Rate rate = seq->frameRate.isValid() ? seq->frameRate : Rate{30, 1};
    PosterImage frame;
    frame.width = std::max(16, std::min(width, seq->width) & ~1);
    frame.height = std::max(2, static_cast<int>(std::lround(static_cast<double>(frame.width) * seq->height / std::max(1, seq->width))) & ~1);
    frame.time = snapToFrame(time, rate);
    exporter::RenderedSourceOptions options;
    options.renderThreads = 1;
    options.maxInFlight = 1;
    auto source = exporter::renderedVideoSourceFactory(options)(checked, *seq,
                                                                exporter::VideoSourceFormat{frame.width, frame.height, rate, frame.time, 1});
    if (!source) {
        return std::unexpected(source.error());
    }
    if (auto r = (*source)->request(0); !r) {
        return std::unexpected(r.error());
    }
    auto picture = (*source)->next();
    (*source)->stop();
    if (!picture) {
        return std::unexpected(picture.error());
    }
    const media::CpuPlanes* cpu = picture->cpu();
    if (!cpu || picture->format != media::PixelFormat::RGBA8) {
        return std::unexpected(Error{ErrorCode::Unsupported, "The renderer delivered no RGBA picture"});
    }
    frame.width = picture->width;
    frame.height = picture->height;
    frame.rgba.resize(static_cast<std::size_t>(frame.width) * frame.height * 4);
    const media::Plane& plane = cpu->planes[0];
    for (int y = 0; y < frame.height; ++y) {
        std::memcpy(frame.rgba.data() + static_cast<std::size_t>(y) * frame.width * 4,
                    plane.data + static_cast<std::ptrdiff_t>(y) * plane.stride, static_cast<std::size_t>(frame.width) * 4);
    }
    return frame;
}

std::expected<color::ColorImage, Error> ColorFrameService::frame(const ProjectState& state, SequenceId sequence,
                                                                 const color::FrameRequest& request) {
    const Sequence* seq = state.findSequence(sequence);
    if (!seq) {
        return std::unexpected(Error{ErrorCode::NotFound, "no such sequence"});
    }
    const ProjectState shown = request.solo.isValid() ? soloClipState(state, sequence, request.solo, request.stopBefore) : state;
    auto rendered = renderSequenceFrame(shown, sequence, request.time, request.maxWidth, m_projectFile);
    if (!rendered) {
        return std::unexpected(rendered.error());
    }
    color::ColorImage image;
    image.width = rendered->width;
    image.height = rendered->height;
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    // Coverage of a solo clip: its frame through its Motion, on the canvas (1 everywhere for the whole sequence).
    std::optional<render::Affine2D> toFrame;
    double frameW = 0, frameH = 0, opacity = 1.0;
    if (request.solo.isValid()) {
        if (const auto loc = seq->findClip(request.solo)) {
            const Clip& clip = loc->clip();
            // The clip's frame: its media stream's picture, or (0.1.2) for a nested or multicam clip the nested
            // sequence's canvas, which the export path draws the active angle into at the clip's source time.
            double aspect = 1.0;
            bool sized = false;
            if (clip.isNested()) {
                if (const Sequence* inner = state.findSequence(clip.sequence); inner && inner->width > 0 && inner->height > 0) {
                    frameW = inner->width;
                    frameH = inner->height;
                    sized = true;
                }
            } else if (const MediaItem* media = state.findMedia(clip.media);
                       media && clip.stream.index >= 0 && clip.stream.index < static_cast<int>(media->info.video.size())) {
                const VideoStreamInfo& v = media->info.video[static_cast<std::size_t>(clip.stream.index)];
                frameW = v.width;
                frameH = v.height;
                aspect = v.sampleAspect.isValid() ? static_cast<double>(v.sampleAspect.num) / v.sampleAspect.den : 1.0;
                sized = true;
            }
            if (sized) {
                const Ticks t = clip.curveTimeAt(std::clamp(request.time, clip.start(), clip.end() - Ticks{1}));
                const ClipProperties& p = clip.properties();
                render::LayerTransform transform;
                transform.position = {p.value(ParamId::PositionX, t), p.value(ParamId::PositionY, t)};
                transform.scale = p.value(ParamId::Scale, t) * scaleToFrameFactor(state, clip, seq->width, seq->height);
                transform.rotationDegrees = p.value(ParamId::Rotation, t);
                transform.anchor = {p.value(ParamId::AnchorX, t), p.value(ParamId::AnchorY, t)};
                opacity = std::clamp(p.value(ParamId::Opacity, t), 0.0, 1.0);
                const render::Affine2D m = render::layerToCanvas(transform, frameW, frameH, aspect, {seq->width, seq->height});
                if (std::abs(m.determinant()) > 1e-12) {
                    toFrame = m.inverted();
                }
            }
        }
    }
    const double sx = static_cast<double>(seq->width) / image.width;
    const double sy = static_cast<double>(seq->height) / image.height;
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * image.width + x) * 4;
            double alpha = 1.0;
            if (request.solo.isValid()) {
                alpha = 0.0;
                if (toFrame) {
                    const render::Vec2 src = toFrame->map({(x + 0.5) * sx, (y + 0.5) * sy});
                    alpha = src.x >= 0 && src.y >= 0 && src.x < frameW && src.y < frameH ? opacity : 0.0;
                }
            }
            image.rgba[i] = rendered->rgba[i] / 255.0f;
            image.rgba[i + 1] = rendered->rgba[i + 1] / 255.0f;
            image.rgba[i + 2] = rendered->rgba[i + 2] / 255.0f;
            image.rgba[i + 3] = static_cast<float>(alpha);
        }
    }
    return image;
}

} // namespace wabcut::editor

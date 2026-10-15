#include <wabcut/playback/audio/TrackAudio.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <wabcut/model/Resolve.h>

#include <algorithm>

namespace wabcut::playback {

namespace {

void collectMedia(const ProjectState& state, const Track& track, std::size_t depth, std::vector<SequenceId>& visited,
                  std::vector<MediaId>& out) {
    for (const Clip& clip : track.clips) {
        if (!clip.isNested()) {
            if (clip.media.isValid()) {
                out.push_back(clip.media);
            }
            continue;
        }
        if (depth >= kMaxNestingDepth || std::find(visited.begin(), visited.end(), clip.sequence) != visited.end()) {
            continue;
        }
        visited.push_back(clip.sequence);
        if (const Sequence* nested = state.findSequence(clip.sequence)) {
            for (const TrackPtr& inner : nested->audioTracks) {
                collectMedia(state, *inner, depth + 1, visited, out);
            }
        }
    }
}

} // namespace

std::vector<MediaId> mediaHeardOn(const ProjectState& state, const Track& track) {
    std::vector<MediaId> out;
    std::vector<SequenceId> visited;
    collectMedia(state, track, 0, visited, out);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool continuesSeamlessly(const Clip& prev, const Clip& next) {
    const ClipProperties& a = prev.properties();
    const ClipProperties& b = next.properties();
    // C.8: a clip of a sequence continues only a clip of the same sequence and angles (another angle is a switch).
    return !prev.isRemapped() && !next.isRemapped() && prev.end() == next.start() && prev.media == next.media &&
           prev.sequence == next.sequence && prev.angles == next.angles && prev.stream == next.stream &&
           prev.speed == next.speed && prev.sourceOut() == next.sourceIn && prev.enabled == next.enabled &&
           a[ParamId::VolumeDb] == b[ParamId::VolumeDb] && a[ParamId::Pan] == b[ParamId::Pan];
}

void renderTrack(const Track& track, Ticks start, std::span<float> out, int channels, Rate rate, AudioFeed& feed,
                 const TrackRenderOptions& options, TrackRenderStats& stats) {
    std::fill(out.begin(), out.end(), 0.0f);
    if (channels < 1 || !rate.isExact()) {
        return;
    }
    TrackRendererOptions renderer;
    renderer.edgeFade = options.edgeFade;
    TrackRenderer(channels, rate, renderer).render(track, start, out, feed, stats);
}

} // namespace wabcut::playback

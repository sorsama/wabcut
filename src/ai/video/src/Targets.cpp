#include <wabcut/ai/video/Targets.h>

#include <wabcut/ai/video/VideoModels.h>
#include <wabcut/model/Clip.h>
#include <wabcut/model/MasterClipEffects.h>
#include <wabcut/model/TimeInterpolation.h>

#include <algorithm>
#include <cmath>

namespace wabcut::ai::video {

std::uint64_t mediaHashOf(const MediaItem& item) {
    if (item.isGenerator()) {
        return 0;
    }
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xFFu;
            h *= 1099511628211ull;
        }
    };
    const SourceFingerprint& f = item.locator.fingerprint;
    if (f.isKnown()) {
        mix(static_cast<std::uint64_t>(f.size));
        for (const char c : f.sampleHash) {
            mix(static_cast<unsigned char>(c));
        }
    } else if (!item.locator.path.empty()) {
        for (const char c : item.locator.path) { // no fingerprint yet: the path (a changed file is not noticed)
            mix(static_cast<unsigned char>(c));
        }
    } else {
        return 0;
    }
    return h == 0 ? 1 : h;
}

CacheIdentity identityOf(const MediaItem& item, int streamIndex, const AiChain& chain) {
    CacheIdentity id;
    if (chain.empty()) {
        return id;
    }
    id.media = mediaHashOf(item);
    id.stream = streamIndex;
    id.chain = aiChainHash(chain);
    id.modelVersion = modelVersion(chain);
    return id;
}

namespace {

/// The stack a clip plays: its own, or (0.6, card #71) the media's Master Clip Effects.
EffectStack stackOf(const ProjectState& state, const Clip& clip) {
    if (playsMasterEffects(state, clip)) {
        return playedEffects(state, clip, EffectKind::Video);
    }
    const auto& own = clip.effects();
    return EffectStack(own.begin(), own.end());
}

} // namespace

int framesIn(TimeRange range, double fps) {
    if (range.duration.value <= 0 || !(fps > 0.0)) {
        return 0;
    }
    const double seconds = static_cast<double>(range.duration.value) / static_cast<double>(Ticks::fromSeconds(1).value);
    return std::max(1, static_cast<int>(std::lround(seconds * fps)));
}

namespace {

/// What every kind of target of a clip shares: where it is, its media and stream, the part of the media it uses. No chain yet.
std::optional<ClipTarget> commonTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex) {
    const MediaItem* item = state.findMedia(clip.media);
    if (item == nullptr || item->isGenerator() || item->info.isStillImage || clip.isNested() || clip.stream.kind != StreamKind::Video ||
        clip.stream.index < 0 || clip.stream.index >= static_cast<int>(item->info.video.size())) {
        return std::nullopt;
    }
    const VideoStreamInfo& stream = item->info.video[static_cast<std::size_t>(clip.stream.index)];
    ClipTarget t;
    t.sequence = sequence.id;
    t.clip = clip.id;
    t.name = clip.name();
    t.track = trackIndex;
    t.timeline = TimeRange{clip.start(), clip.timelineRange.duration};
    t.media = clip.media;
    t.streamIndex = clip.stream.index;
    t.containerStream = stream.index;
    t.path = item->locator.offline ? std::string() : item->locator.path;
    const bool turned = (stream.rotationDegrees / 90) % 2 != 0;
    t.width = turned ? stream.height : stream.width;
    t.height = turned ? stream.width : stream.height;
    t.frame = stream.frameRate.isValid() ? framesToTicks(1, stream.frameRate) : framesToTicks(1, kRate30);
    Ticks start = std::max(Ticks{}, clip.sourceIn);
    Ticks end = clip.sourceOut();
    if (end <= start) {
        end = start + t.frame; // a freeze frame: its one picture
    }
    if (const auto extent = item->sourceExtent(clip.stream); extent && end > *extent) {
        end = std::max(start + t.frame, *extent);
    }
    t.range = TimeRange{start, end - start};
    t.fps = stream.frameRate.isValid() ? stream.frameRate.toDouble() : 0.0;
    t.expected = framesIn(t.range, t.fps);
    return t;
}

} // namespace

std::optional<ClipTarget> clipTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex) {
    auto t = commonTarget(state, sequence, clip, trackIndex);
    if (!t) {
        return std::nullopt;
    }
    const MediaItem* item = state.findMedia(clip.media);
    t->chain = aiProcessingChainOf(stackOf(state, clip), t->height, sequence.height);
    t->identity = identityOf(*item, clip.stream.index, t->chain);
    return t;
}

std::optional<ClipTarget> depthTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex) {
    auto t = commonTarget(state, sequence, clip, trackIndex);
    if (!t) {
        return std::nullopt;
    }
    t->kind = JobKind::Depth;
    t->chain = aiDepthChainOf(stackOf(state, clip));
    if (t->chain.empty()) {
        return std::nullopt;
    }
    t->identity = identityOf(*state.findMedia(clip.media), clip.stream.index, t->chain);
    return t;
}

std::optional<ClipTarget> faceTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex) {
    auto t = commonTarget(state, sequence, clip, trackIndex);
    if (!t) {
        return std::nullopt;
    }
    t->kind = JobKind::Faces;
    t->chain = aiFaceChainOf(stackOf(state, clip));
    if (t->chain.empty()) {
        return std::nullopt;
    }
    t->identity = identityOf(*state.findMedia(clip.media), clip.stream.index, t->chain);
    return t;
}

std::optional<ClipTarget> gazeTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex) {
    auto t = commonTarget(state, sequence, clip, trackIndex);
    if (!t) {
        return std::nullopt;
    }
    t->kind = JobKind::Gaze;
    t->chain = aiGazeChainOf(stackOf(state, clip));
    if (t->chain.empty()) {
        return std::nullopt;
    }
    t->identity = identityOf(*state.findMedia(clip.media), clip.stream.index, t->chain);
    return t;
}

std::optional<ClipTarget> interpolationTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex) {
    if (clip.timeInterpolation() != TimeInterpolation::AiInterpolation) {
        return std::nullopt;
    }
    auto t = commonTarget(state, sequence, clip, trackIndex);
    const MediaItem* item = state.findMedia(clip.media);
    if (!t || item == nullptr || !retimesFrames(clip, *item, sequence.frameRate) || !sequence.frameRate.isValid()) {
        return std::nullopt;
    }
    t->kind = JobKind::Interpolation;
    t->chain = aiInterpolationChain();
    t->identity = identityOf(*item, clip.stream.index, t->chain);
    t->framePeriod = t->frame;
    // The exact source time of every output frame of the clip: what the evaluator asks for (Clip::sourceTimeAt at the frame).
    const Ticks end = clip.end();
    std::int64_t k = std::max<std::int64_t>(0, ticksToFrames(clip.start(), sequence.frameRate)); // the first frame boundary at or after the start
    if (framesToTicks(k, sequence.frameRate) < clip.start()) {
        ++k;
    }
    for (Ticks at = framesToTicks(k, sequence.frameRate); at < end; at = framesToTicks(++k, sequence.frameRate)) {
        t->times.push_back(clip.sourceTimeAt(at));
    }
    std::sort(t->times.begin(), t->times.end());
    t->times.erase(std::unique(t->times.begin(), t->times.end()), t->times.end());
    for (const Ticks time : t->times) {
        t->keys.push_back(Ticks{interpolationKey(time, t->framePeriod)});
    }
    std::sort(t->keys.begin(), t->keys.end());
    t->keys.erase(std::unique(t->keys.begin(), t->keys.end()), t->keys.end());
    t->expected = static_cast<int>(t->keys.size());
    return t;
}

std::size_t coveredCount(ProcessedCache& cache, const ClipTarget& target) {
    if (!target.identity.valid()) {
        return 0;
    }
    if (target.kind == JobKind::Interpolation) {
        return cache.countCovered(target.identity, target.keys, Ticks{0});
    }
    const std::size_t n = cache.countIn(target.identity, Ticks{std::max<std::int64_t>(0, target.range.start.value - target.frame.value / 2)},
                                        target.range.end());
    return std::min<std::size_t>(n, static_cast<std::size_t>(std::max(target.expected, 0)));
}

std::vector<ClipTarget> sequenceTargets(const ProjectState& state, const Sequence& sequence, const std::optional<TimeRange>& within) {
    std::vector<ClipTarget> out;
    int trackIndex = -1;
    for (const TrackPtr& track : sequence.videoTracks) {
        ++trackIndex;
        if (!track || !track->enabled) {
            continue;
        }
        for (const Clip& clip : track->clips) {
            if (!clip.enabled || clip.isNested() || !clip.media.isValid()) {
                continue;
            }
            if (within && (clip.end() <= within->start || clip.start() >= within->end())) {
                continue;
            }
            std::optional<ClipTarget> candidates[5] = {clipTarget(state, sequence, clip, trackIndex),
                                                       depthTarget(state, sequence, clip, trackIndex),
                                                       faceTarget(state, sequence, clip, trackIndex),
                                                       gazeTarget(state, sequence, clip, trackIndex),
                                                       interpolationTarget(state, sequence, clip, trackIndex)};
            for (std::optional<ClipTarget>& target : candidates) {
                if (!target || target->chain.empty()) {
                    continue;
                }
                const auto same = std::ranges::find_if(out, [&](const ClipTarget& o) { return o.identity == target->identity; });
                if (same != out.end()) {
                    if (target->kind == JobKind::Interpolation) {
                        // Interpolation: the union of the times (and keys) the clips of one media ask for.
                        same->times.insert(same->times.end(), target->times.begin(), target->times.end());
                        std::sort(same->times.begin(), same->times.end());
                        same->times.erase(std::unique(same->times.begin(), same->times.end()), same->times.end());
                        same->keys.insert(same->keys.end(), target->keys.begin(), target->keys.end());
                        std::sort(same->keys.begin(), same->keys.end());
                        same->keys.erase(std::unique(same->keys.begin(), same->keys.end()), same->keys.end());
                        same->expected = static_cast<int>(same->keys.size());
                        continue;
                    }
                    const Ticks start = std::min(same->range.start, target->range.start);
                    const Ticks end = std::max(same->range.end(), target->range.end());
                    same->range = TimeRange{start, end - start};
                    same->expected = framesIn(same->range, same->fps);
                    continue;
                }
                out.push_back(std::move(*target));
            }
        }
    }
    return out;
}

} // namespace wabcut::ai::video

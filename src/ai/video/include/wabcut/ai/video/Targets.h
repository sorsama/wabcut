#pragma once

// Which clips have AI video effects and what Process has to make for each (0.7): the chain a clip's AI effects form (Auto
// resolved against its media and the sequence's size, the media's Master Clip Effects included), the cache the chain's
// pictures belong to, and the part of the media the clip uses. Pure model code: the app's header, the Export panel's notice and
// the effect.processAi / effect.aiStatus commands all ask it, so they agree.

#include <wabcut/ai/video/ProcessedCache.h>
#include <wabcut/ai/video/Runner.h>
#include <wabcut/model/AiEffects.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wabcut::ai::video {

/// The media's content identity hashed (size + sample hash of its fingerprint, else its path): 0 for a generated item.
std::uint64_t mediaHashOf(const MediaItem& item);

/// The cache of `chain`'s pictures for stream `streamIndex` (index into MediaInfo::video) of `item`: invalid when the item is
/// generated or the chain is empty.
CacheIdentity identityOf(const MediaItem& item, int streamIndex, const AiChain& chain);

struct ClipTarget {
    /// Pictures: AI Restore / AI Denoise (the chain's processed pictures replace the decoded ones). Depth: AI Refocus's depth maps.
    /// Faces: AI Face Retouch's face maps. Gaze: AI Eye Contact's eye measurements. Interpolation: AI Frame Interpolation's in-between pictures (`times`, `keys`).
    JobKind kind = JobKind::Pictures;
    SequenceId sequence;
    ClipId clip;
    std::string name;       ///< the clip's name
    int track = 0;          ///< its video track, 0 = V1
    TimeRange timeline;     ///< the clip on its sequence
    MediaId media;
    int streamIndex = 0;    ///< into MediaInfo::video
    int containerStream = -1; ///< VideoStreamInfo::index, for the decoder
    std::string path;       ///< the original file ("" = offline)
    int width = 0;          ///< of the displayed source picture (a turned phone clip swaps them)
    int height = 0;
    Ticks frame{};          ///< one frame of the media
    double fps = 0.0;       ///< the media's frame rate
    TimeRange range;        ///< the part of the media the clip uses (a freeze frame: its one picture)
    int expected = 0;       ///< the pictures in `range`
    AiChain chain;          ///< what Process makes (Amount ignored); empty = the clip's settings change nothing
    CacheIdentity identity; ///< invalid while the chain is empty
    /// Interpolation: the exact source time of each of the clip's output frames (ascending, without repeats), the media's frame
    /// period, and the cache keys of those times (interpolationKey, ascending, unique): `expected` is their count.
    std::vector<Ticks> times;
    Ticks framePeriod{};
    std::vector<Ticks> keys;
};

/// How many pictures a media of `fps` frames per second has in `range` (at least 1 for a range with any length, 0 for none).
int framesIn(TimeRange range, double fps);

/// `clip`'s target, or nullopt when AI effects cannot work on it (a title, a graphic, a still image, a nested clip, a stream
/// that does not exist). `trackIndex`: the clip's video track (0 = V1).
std::optional<ClipTarget> clipTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex);

/// 0.7 (AI Refocus): `clip`'s depth-map target (kind Depth), or nullopt when the clip cannot have one or has no enabled AI Refocus.
/// The depth maps belong to the picture, not to the effect's settings: every Refocus of a media shares them.
std::optional<ClipTarget> depthTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex);
/// 0.7 (AI Face Retouch): `clip`'s face-map target (kind Faces), or nullopt when the clip cannot have one or has no enabled AI Face
/// Retouch. The face maps belong to the picture, not to the effect's settings: every Face Retouch of a media shares them.
std::optional<ClipTarget> faceTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex);
/// 0.7 (AI Eye Contact): `clip`'s eye-measurement target (kind Gaze), or nullopt when the clip cannot have one or has no enabled AI Eye
/// Contact. The measurements belong to the picture, not to the effect's settings: every Eye Contact of a media shares them.
std::optional<ClipTarget> gazeTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex);
/// 0.7 (AI Frame Interpolation): `clip`'s interpolation target (kind Interpolation), or nullopt when its Time Interpolation is not
/// AI Frame Interpolation or nothing falls between its media's frames (a speed of 100 % in the media's own rate).
std::optional<ClipTarget> interpolationTarget(const ProjectState& state, const Sequence& sequence, const Clip& clip, int trackIndex);

/// How many of the target's pictures are in the cache (a Pictures / Depth target: frames in its range; Interpolation: its keys).
std::size_t coveredCount(ProcessedCache& cache, const ClipTarget& target);

/// The clips of `sequence`'s enabled video tracks that have a non-empty AI chain, in track then time order; with `within`, only
/// those overlapping that range; a clip can have a Pictures, a Depth, a Faces and an Interpolation target. Clips of one media with the same
/// chain are one target covering the union of what they use (they share their pictures).
std::vector<ClipTarget> sequenceTargets(const ProjectState& state, const Sequence& sequence, const std::optional<TimeRange>& within);

} // namespace wabcut::ai::video

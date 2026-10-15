#pragma once

// One-shot, sample-accurate rendering of one audio track over a timeline range (a fresh TrackRenderer: see
// TrackRenderer.h, which the player's feeder and export use to carry effect state across calls); no threads, no device.

#include <wabcut/playback/audio/AudioFeed.h>

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Track.h>

#include <cstdint>
#include <span>
#include <vector>

namespace wabcut::playback {

struct TrackRenderOptions {
    /// Length of the fades at clip edges that are cuts (2 ms). An edge between two clips that continue each
    /// other exactly (same media and stream, contiguous source, same speed, volume and pan: a through edit) is
    /// not faded. Ticks{} disables fades.
    Ticks edgeFade{kTicksPerSecond / 500};
};

struct TrackRenderStats {
    std::uint64_t feedErrors = 0;     ///< read() calls that failed (rendered as silence)
    std::uint64_t shortReadFrames = 0; ///< frames past the end of a media's data (rendered as silence)
    std::uint64_t takeErrors = 0;     ///< AudioFeed::readTake failures (A.4: the clip played its source)
};

/// Renders `track` at timeline times start + k · ticksPerSample (k = 0 .. frames-1) into `out` (interleaved
/// `channels`, frames = out.size() / channels). Frame k belongs to the clip whose half-open timeline range
/// contains its time, so clip edges are sample-accurate. Gaps, disabled clips, clips with a speed other than
/// 1/1 (no retimed audio yet) and unreadable media are silence; reversed clips play backwards. Applies the clip's
/// volume (dB) and pan (balance), its effect stack, audio transitions, fades at cuts and the track's effect stack
/// (effects settle on a pre-roll before `start`). Track mute/solo/enable are not applied here (the mixer does that).
void renderTrack(const Track& track, Ticks start, std::span<float> out, int channels, Rate rate, AudioFeed& feed,
                 const TrackRenderOptions& options, TrackRenderStats& stats);

/// True when `next` continues `prev` without a cut (see TrackRenderOptions::edgeFade).
bool continuesSeamlessly(const Clip& prev, const Clip& next);

/// C.8: the media `track` plays, also through its nested and multicam clips (the audio tracks of the sequences they
/// play, nested to kMaxNestingDepth), sorted and unique: the media whose redactions or takes change its sound.
std::vector<MediaId> mediaHeardOn(const ProjectState& state, const Track& track);

} // namespace wabcut::playback

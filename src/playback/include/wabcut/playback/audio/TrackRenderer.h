#pragma once

// TrackRenderer: the stateful, sample-accurate render of one audio track, shared by preview (the player's feeder)
// and export (OfflineMixdown), so both run the same DSP on the same samples.
//
// Per clip: source (reversed clips play backwards; with an enabled Enhance Speech effect, the media's enhanced take
// blended in by its Strength, AudioTakes.h; off 100 %, the speed audio below) -> Volume and Panner (curves on the 32-frame parameter grid, see
// audio/effects/EffectChain.h) -> redaction mutes and bleeps (AudioRedactions.h) -> the clip's effect stack -> edge envelope (2 ms fades at cuts, or the audio
// transition's crossfade). Clips that continue each other exactly (a through edit: same media, contiguous source,
// same volume, pan and effects, no transition) share one "voice", so effect state flows across the cut and nothing
// is faded there. Then the clips are summed, the track's effect stack runs on the sum, and the track fader and
// panner (Track::volume / Track::pan, timeline time, on the same parameter grid; A.3) set its level and balance.
// On a 5.1 bus a mono or stereo track renders its clips and both effect stages at its own channel count; its 5.1
// panner (Track::surround, on the parameter grid) then places the result on the bus, after the effects as Premiere
// orders it (review A5-P2-10), and the fader sets the level (its stereo balance gives way to the 5.1 panner).
//
// Nested and multicam clips (C.8, docs/internal/c8-nested-multicam-handoff.md): with the project set (setProject), a clip of a
// sequence plays that sequence's sound as its source, then its own Volume and Panner, effects and envelope like any
// clip. A nested clip: the mix of the tracks resolveLeaves() reads there (nestedTracksRead: output on, not muted,
// solo honoured), each with its clips, effects, faders and pan, summed, then the nested sequence's master stage (its
// master effects and master fader, as Premiere applies a nested sequence's Mix track to the nest). A multicam clip:
// the sum of its active angles' tracks with their effects and faders, no master stage; the angles' sync is where
// their clips lie in the source. Nesting nests (to kMaxNestingDepth), and source-anchored redactions apply to the
// media inside by media time, excepted by the leaf clip or any clip leading to it. Speed: a nested clip is heard at
// 100 % (a remapped one in its 100 % spans); a reversed one plays its nested mix backwards.
// Where a multicam clip is cut to another angle (multicam.switchAngle), the two angles crossfade over
// `angleCrossfade` around the cut instead of the cut fades, so a switch never clicks or dips. Past the nested
// sequence's end the clip is silent.
//
// Speed (A.10, audio/dsp/SpeedRender.h): a media clip plays at its speed. Its 100 % forward spans (a whole clip at
// 100 %, a remap's straight 1x segments) and 100 % reversed clips read the media straight, sample for sample; the
// rest follows the clip's source position frame by frame: a constant speed (forwards or reversed), a remap's ramps,
// other straight speeds and reverse segments, transition handles included (the curve there). Clip::maintainPitch
// picks the renderer: a pitch-keeping time stretch (WSOLA) or varispeed (resampled, pitch moves with the speed). Holds
// and freeze frames are silent, and so are speeds under 0.1x or over 20x (faded at the edges). Where a 100 % span meets
// a speed span the two crossfade (the renderer's coverage), so ramps and joins do not click; a cut into a hold fades
// like a clip edge. Redactions apply to the media time a speed span reads (muted or bleeped at any speed); Channel
// Volume and the enhanced take's Strength hold their value from the start of the span. Nested and multicam clips
// still play at 100 % only (a remapped one in its 100 % spans). A speed render's output depends only on the timeline
// time, so preview and export match sample for sample, also after a seek into a span.
//
// Latency: effects with lookahead (limiter, Reduce Noise) delay their input. The renderer reads each voice's input
// that many frames ahead and the track mix ahead by the track stack's latency, so the output is never late: what is
// rendered for time t is the audio for time t.
//
// State: render() calls that continue each other (same track, next frame) carry effect state on. Any other call (a
// seek, an edit refill, a loop wrap, another track) restarts: every effect is reset and, when the track or a clip
// near the start has effects, the renderer first renders `maxPreroll` of audio before the start and throws it away,
// so the effects settle as if playback had been running. A preview that starts at t and an export that starts at t
// therefore produce identical samples.
//
// Threading: one thread at a time (the feeder, or an export / analysis job). Not real-time: it decodes through the
// AudioFeed and allocates when voices with effects come into view.

#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/TrackAudio.h>

#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Track.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::playback {

struct TrackRendererOptions {
    /// Fades at cuts (clip edges without a transition); Ticks{} disables them.
    Ticks edgeFade{kTicksPerSecond / 500};
    /// Audio rendered (and discarded) before a restart point so stateful effects settle; only when the track has
    /// effects. Preview and export must use the same value to match sample for sample.
    Ticks maxPreroll{kTicksPerSecond / 2};
    /// C.8: the crossfade where a multicam clip switches angle (centred on the cut); Ticks{} cuts with edge fades.
    Ticks angleCrossfade{kTicksPerSecond / 100};
    /// The track's fader and panner (Track::volume / Track::pan) end the render. false leaves them to the caller:
    /// the live player applies them in its audio callback, where an automation pass can override them at once
    /// (TimelineAudioPlayer::setLiveTrackMix). Nested sequences always apply their own tracks' faders.
    bool trackFader = true;
};

class TrackRenderer {
public:
    TrackRenderer(int channels, Rate rate, TrackRendererOptions options = {});
    ~TrackRenderer();
    TrackRenderer(const TrackRenderer&) = delete;
    TrackRenderer& operator=(const TrackRenderer&) = delete;

    int channels() const noexcept { return m_channels; }
    Rate rate() const noexcept { return m_rate; }

    /// Renders `track` at timeline times start + k * ticksPerFrame (k = 0 .. frames - 1) into `out` (interleaved,
    /// frames = out.size() / channels). Track mute/solo/enable are not applied (the mixer does that).
    void render(const Track& track, Ticks start, std::span<float> out, AudioFeed& feed, TrackRenderStats& stats);

    /// The next render() restarts (resets effects, pre-rolls).
    void reset() noexcept;

    /// Redaction mutes and bleeps applied from the next render() on (null: none). Resolved off the audio thread.
    void setRedactions(std::shared_ptr<const AudioRedactions> redactions) noexcept;
    /// The offline takes clips with Enhance Speech play (A.4), from the next render() on (null: none, every clip plays
    /// its source). Resolved off the audio thread.
    void setAudioTakes(std::shared_ptr<const AudioTakes> takes) noexcept;
    /// C.8: the project whose sequences nested and multicam clips play, from the next render() on (null: those clips
    /// are silent). The track given to render() should be of the same revision.
    void setProject(std::shared_ptr<const ProjectState> project) noexcept;
    /// C.8: the nested and multicam clips that lead to the track rendered (outermost first): redactions excepting one of
    /// them do not apply to its clips. Set by the renderer of the nested level above; empty on a top-level track.
    void setNesting(std::vector<ClipId> path);

    /// Voices (clip runs) alive after the last render (diagnostics, tests).
    int activeVoices() const noexcept;
    /// Track stack latency as of the last render, frames.
    int trackLatencyFrames() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_channels;
    Rate m_rate;
};

} // namespace wabcut::playback

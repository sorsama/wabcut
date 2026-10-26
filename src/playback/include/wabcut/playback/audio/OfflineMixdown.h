#pragma once

// OfflineMixdown: a sequence's audio rendered offline through exactly the preview path (preview == export): a
// TrackRenderer per audio track (clip and track effects, transitions, cut fades), the player's mute/solo/enable rules
// in an audio::Mixer, and the master bus (Sequence::audioEffects) with its latency read ahead. Used by export and by
// the loudness analysis jobs.
//
// It renders in whole blocks of kBlockFrames from `start`, like a player started there, so the samples do not depend
// on how the caller slices its reads. Rendered from the same start with the same edge fade, its output equals the
// player's sample for sample (tests/playback/test_effects_render.cpp).

#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/TrackAudio.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Sequence.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace wabcut::playback {

struct OfflineMixdownOptions {
    /// Clip edge fades and effect pre-roll; the player's defaults.
    TrackRendererOptions renderer;
    /// Only these audio tracks (by id), as if the others were muted; nullopt = every track.
    std::optional<std::vector<TrackId>> tracks;
    /// Apply track mute, solo and output (enable). Off: every selected track is heard.
    bool honourMuteSolo = true;
    /// Run the master audio effect stack.
    /// The master bus: Sequence::audioEffects and the master fader (Sequence::masterVolume). Off: the plain track sum.
    bool masterEffects = true;
    /// Redaction mutes and bleeps (AudioRedactions::of the project snapshot), as the player applies them.
    std::shared_ptr<const AudioRedactions> redactions;
    /// The enhanced takes clips with Enhance Speech play (AudioTakes::of the project snapshot; A.4), as the player
    /// plays them. Null: every clip plays its source.
    std::shared_ptr<const AudioTakes> takes;
    /// C.8: the project the sequence is part of, where its nested and multicam clips find the sequences they play
    /// (TrackRenderer::setProject). Null: those clips are silent.
    std::shared_ptr<const ProjectState> project;
};

class OfflineMixdown {
public:
    static constexpr int kBlockFrames = 1024;

    /// `sequence` and `feed` must outlive the mixdown.
    OfflineMixdown(std::shared_ptr<const Sequence> sequence, int channels, int sampleRate, Ticks start, AudioFeed& feed,
                   OfflineMixdownOptions options = {});
    ~OfflineMixdown();
    OfflineMixdown(const OfflineMixdown&) = delete;
    OfflineMixdown& operator=(const OfflineMixdown&) = delete;

    int channels() const noexcept { return m_channels; }
    /// Timeline time of the next frame render() writes.
    Ticks position() const noexcept;

    /// Fills `interleaved` with the next interleaved.size() / channels frames.
    void render(std::span<float> interleaved);
    /// 0.5: goes on from `start` as a mixdown made there would (the master and the submix paths restart and prime there,
    /// every track renderer restarts with its pre-roll), keeping its processors: audio scrubbing renders many short
    /// windows of one sequence revision without building the effect chains again.
    void seek(Ticks start);

    const TrackRenderStats& stats() const noexcept;
    std::uint64_t feedErrors() const noexcept { return stats().feedErrors; }

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    int m_channels;
};

} // namespace wabcut::playback

#pragma once

// Internal: the sound one nested or multicam clip plays (C.8, Nesting.h; docs/internal/c8-nested-multicam-handoff.md "Audio"),
// the source TrackRenderer reads for such a clip instead of media.
//
// A level is the tracks resolveLeaves() reads from the clip's sequence (nestedTracksRead), each rendered by its own
// TrackRenderer (clips, clip effects, transitions, cut fades, track effects, track fader and pan; nested clips inside
// recurse), summed. A nested sequence then runs its master stage, Sequence::audioEffects and masterVolume, with the
// stack's latency read ahead like a track's; a multicam source has none. Outside [0, duration) of the nested sequence
// the clip is silent. The level renders at the parent's rate and channel count.
//
// Nested time advances with the parent at 100 % (the only speed audio plays; a remapped clip is heard in its 100 %
// spans), so a forward read is a run of nested time. A read that continues the previous one carries every inner effect
// on; any other read restarts: the inner renderers reset (they pre-roll their own effects), and a master stack with
// effects is pre-rolled by TrackRendererOptions::maxPreroll. The samples therefore depend only on where the run
// started, as for media, and preview and export agree from the same start.
//
// Reversed clips read backwards, which never continues: readReversed() serves them from fixed pieces of nested time
// (kReversePiece, aligned to the clip), each rendered forwards from a restart, so their samples do not depend on how
// the reads are cut either.
//
// Threading: the TrackRenderer's thread (the feeder, an export or analysis job); never the audio callback.

#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/AudioRedactions.h>
#include <wabcut/playback/audio/AudioTakes.h>
#include <wabcut/playback/audio/TrackRenderer.h>

#include <wabcut/audio/effects/EffectChain.h>

#include <wabcut/model/ProjectState.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::playback::detail {

/// What a level inherits from the renderer above it.
struct NestContext {
    const std::shared_ptr<const ProjectState>* project = nullptr;
    const std::shared_ptr<const AudioRedactions>* redactions = nullptr;
    const std::shared_ptr<const AudioTakes>* takes = nullptr;
    /// The nested clips that lead into the level, outermost first, the clip being read last.
    std::span<const ClipId> path;
};

class NestedMix {
public:
    /// Nested time one piece of a reversed clip covers.
    static constexpr Ticks kReversePiece{kTicksPerSecond};

    NestedMix(int channels, Rate rate, TrackRendererOptions options);
    ~NestedMix();
    NestedMix(const NestedMix&) = delete;
    NestedMix& operator=(const NestedMix&) = delete;

    /// `clip`'s sound at nested times from + k * ticksPerFrame into `out` (interleaved; frames = out.size() / channels).
    void read(const NestContext& context, const Clip& clip, Ticks from, std::span<float> out, AudioFeed& feed,
              TrackRenderStats& stats);
    /// The same for a reversed clip, from pieces of nested time aligned to `anchor` (the clip's source In).
    void readReversed(const NestContext& context, const Clip& clip, Ticks anchor, Ticks from, std::span<float> out,
                      AudioFeed& feed, TrackRenderStats& stats);

private:
    struct Inner {
        TrackId track;
        std::unique_ptr<TrackRenderer> renderer;
        const ProjectState* project = nullptr;
        const AudioRedactions* redactions = nullptr;
        const AudioTakes* takes = nullptr;
        std::vector<ClipId> path;
        bool seen = false;
    };

    std::int64_t frameCount(std::span<float> out) const { return static_cast<std::int64_t>(out.size()) / m_channels; }
    Ticks timeOf(std::int64_t frame) const { return m_origin + Ticks{frame * m_tpf}; }
    Inner& innerFor(TrackId track);
    void configure(Inner& inner, const NestContext& context);
    void restart(const NestContext& context, const Sequence& nested, const Clip& clip, Ticks from, AudioFeed& feed,
                 TrackRenderStats& stats);
    void renderBlock(const NestContext& context, const Sequence& nested, const Clip& clip, std::int64_t f0, std::int64_t n,
                     float* out, AudioFeed& feed, TrackRenderStats& stats);
    void sumTracks(const NestContext& context, const Sequence& nested, const Clip& clip, std::int64_t g0, std::int64_t n,
                   float* dst, AudioFeed& feed, TrackRenderStats& stats);

    const int m_channels;
    const Rate m_rate;
    const std::int64_t m_tpf;
    const TrackRendererOptions m_options;

    std::vector<Inner> m_tracks;
    audio::EffectChain m_master;
    bool m_started = false;
    SequenceId m_sequence;
    Ticks m_next;               ///< the nested time a continuing read() starts at
    Ticks m_origin;             ///< nested time of frame 0 of this run
    std::int64_t m_cursor = 0;  ///< frame of m_next
    std::int64_t m_masterInput = 0; ///< next input frame of the master chain

    std::vector<float> m_mix;
    std::vector<float> m_track;
    std::vector<float> m_discard;

    // readReversed(): the piece last rendered.
    std::int64_t m_piece = std::numeric_limits<std::int64_t>::min();
    Ticks m_pieceBase;
    std::vector<float> m_pieceSamples;
};

} // namespace wabcut::playback::detail

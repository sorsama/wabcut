#pragma once

// The sound of source-anchored redactions (model/Masks.h, I.6): the media ranges a redaction mutes or bleeps.
//
// Resolved from a project snapshot off the audio thread (the player's feeder when the snapshot changes, an export when
// it starts), immutable and shared with the track renderers. TrackRenderer applies it to each clip's source audio
// after the clip's Volume and Panner and before its effects, in preview and export alike (the same code on the same
// samples: offline == preview bit for bit). The audio callback never sees it: it only reads the rendered rings.
//
// Inside a marked range (sample-accurate, in media time) the clip's sound is replaced: silence (Mute) or a 1 kHz sine
// at -12 dBFS (Bleep) whose phase follows media time. 5 ms gain ramps just OUTSIDE the range avoid clicks without
// letting any sample of the range through.

#include <wabcut/model/Ids.h>
#include <wabcut/model/Masks.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace wabcut::playback {

class AudioRedactions {
public:
    static constexpr double kBleepHz = 1000.0;
    static constexpr double kBleepDbfs = -12.0;
    static constexpr Ticks kRamp{kTicksPerSecond / 200}; ///< 5 ms

    struct Range {
        MediaId media; ///< the redaction root of the redaction's media (model/RedactionMedia.h)
        TimeRange source;
        RedactionAudio mode = RedactionAudio::Mute;
        std::vector<ClipId> exceptions; ///< sorted (the redaction's)

        bool operator==(const Range&) const = default;
    };
    /// A media item whose redaction root is another item (a subclip, a stem): (media, root), sorted by media.
    using RootMap = std::vector<std::pair<MediaId, MediaId>>;

    /// The audio ranges of `project`'s enabled redactions (their word ranges, else their whole source range), anchored
    /// at their redaction roots, and the roots of the project's subclips and stems (review wave 5 E5-P1-1, A5-P1-3: a
    /// bleep on a master or source is heard on its subclips and stems, and the reverse); null when none mutes or bleeps.
    static std::shared_ptr<const AudioRedactions> of(const ProjectState& project);

    explicit AudioRedactions(std::vector<Range> ranges, RootMap roots = {});

    /// Applies the ranges covering `media` (except those excepting `clip`) to `frames` interleaved frames at `dst`;
    /// frame k plays media time firstSource + k * step (step = +/- the ticks of one frame: forward or reversed clips).
    /// `path`: the nested and multicam clips that lead to `clip` (C.8): a range that excepts any of them does not apply
    /// either (excepting a nested clip excepts everything inside it; resolveLeaves() applies the same rule).
    void apply(MediaId media, ClipId clip, Ticks firstSource, std::int64_t step, std::int64_t frames, int channels,
               float* dst, std::span<const ClipId> path = {}) const;

    const std::vector<Range>& ranges() const noexcept { return m_ranges; }
    /// The redaction root of `media` (itself unless it is a subclip or a stem). No allocation.
    MediaId rootOf(MediaId media) const noexcept;
    bool operator==(const AudioRedactions& o) const;

    /// The media whose ranges differ between two tables (either may be null: no ranges), sorted: what an edit of the
    /// project's redactions changes in the sound. Renaming a redaction, moving its picture region or any video-only
    /// redaction changes nothing.
    static std::vector<MediaId> changedMedia(const AudioRedactions* before, const AudioRedactions* after);

    /// The bleep's sample at media time `t` (the same in preview and export).
    static float bleepAt(Ticks t);

private:
    std::vector<Range> m_ranges; ///< sorted by media, then start
    RootMap m_roots;
};

} // namespace wabcut::playback

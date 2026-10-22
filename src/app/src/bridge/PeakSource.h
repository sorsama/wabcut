#pragma once

// PeakSource: where TimelineView gets audio waveform envelopes.
//
// SEAM for media: `media::WaveformPeaks` (background job + disk cache, min/max pairs at several levels of
// detail) will implement this. Until then PlaceholderPeakSource returns a deterministic envelope derived from
// the media id and the *source* time, so a waveform stays put when a clip is moved, split or trimmed, the way
// real peaks do.
//
// Threading: peaks() is called from TimelineView::updatePaintNode (render thread, GUI thread blocked). An
// implementation must not block on decoding or IO: return false when the peaks are not ready yet and call
// the invalidation callback once they are.

#include <wabcut/model/Clip.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace wabcut::app {

class PeakSource {
public:
    virtual ~PeakSource() = default;

    /// Fills `out` with peak magnitudes in [0, 1] for `out.size()` equal slices of the source range
    /// [sourceStart, sourceEnd) of `stream` in `media`. Returns false (leaving `out` unspecified) when peaks
    /// are not available yet.
    virtual bool peaks(MediaId media, StreamRef stream, Ticks sourceStart, Ticks sourceEnd,
                       std::span<float> out) const = 0;

    /// p02 audio: while the peaks of `stream` are still being decoded, the source time the decoded part reaches (the
    /// timeline shades the rest of its clips); nullopt once they are complete, or when the source does not say.
    virtual std::optional<Ticks> buildingUntil(MediaId /*media*/, StreamRef /*stream*/) const { return std::nullopt; }

    /// p02 audio: the media (ids) whose peaks changed after `serial` (0, or a `now` an earlier call returned), so a view
    /// rebuilds only their waveforms. nullopt when the source cannot tell: everything may have changed. `now` receives
    /// the serial to pass next time. Any thread.
    virtual std::optional<std::vector<std::uint64_t>> mediaChangedSince(std::uint64_t serial, std::uint64_t& now) const {
        now = serial;
        return std::nullopt;
    }
};

class PlaceholderPeakSource final : public PeakSource {
public:
    bool peaks(MediaId media, StreamRef stream, Ticks sourceStart, Ticks sourceEnd,
               std::span<float> out) const override;
};

} // namespace wabcut::app

#pragma once

// Private: the offline audio mix of an export, through the playback path (playback::OfflineMixdown: the player's
// TrackRenderer per audio track with clip/track effects and transitions, mute/solo, the mixer and the master bus),
// with MediaAudioFeed for decoding. Preview and export run the same DSP on the same samples.

#include <wabcut/model/EditSession.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>
#include <wabcut/playback/audio/OfflineMixdown.h>

#include <cstdint>
#include <span>

namespace wabcut::exporter::detail {

/// Renders a sequence's mix from `start` on, sample by sample in order. Internally always mixes whole blocks
/// from `start` (like a player rendering from that point), so the samples do not depend on how the caller slices
/// its reads.
class OfflineMix {
public:
    static constexpr int kBlockFrames = playback::OfflineMixdown::kBlockFrames;

    /// `project`: the snapshot with locators pointing at the original files.
    OfflineMix(ProjectState project, SequencePtr sequence, int channels, int sampleRate, Ticks start);

    /// Fills `interleaved` with the next interleaved.size() / channels sample frames.
    void render(std::span<float> interleaved) { m_mix.render(interleaved); }

    std::uint64_t feedErrors() const { return m_mix.feedErrors(); }

private:
    EditSession m_session;
    playback::MediaAudioFeed m_feed; // after m_session: it reads the session
    playback::OfflineMixdown m_mix;  // after m_feed
};

} // namespace wabcut::exporter::detail

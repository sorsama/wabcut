#pragma once

// MediaAudioFeed: the AudioFeed that decodes real media files through wabcut_media.

#include <wabcut/playback/audio/AudioFeed.h>

#include <wabcut/model/EditSession.h>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace wabcut::playback {

struct MediaAudioFeedStats {
    std::uint64_t opens = 0;         ///< decoders opened
    std::uint64_t evictions = 0;     ///< decoders closed to stay within the cache size
    std::uint64_t invalidations = 0; ///< decoders closed because their media item was relinked or removed
    std::uint64_t errors = 0;        ///< reads that failed (unknown media/stream, offline file, decode error)
};

/// Resolves a clip's MediaId through the session's current snapshot (MediaItem::locator.path and
/// MediaInfo::audio[streamOrdinal].index) and decodes with media::AudioDecoder at the player's rate and
/// channel count. Open decoders are kept in an LRU keyed by (media, stream ordinal, rate, channels); one whose
/// media item changed its locator (relink, proxy switch) or disappeared is closed and reopened on next use.
///
/// Threading: like every AudioFeed, only the player's feeder thread calls read() (one at a time); it refuses
/// to run on the audio callback. The session must outlive the feed.
class MediaAudioFeed final : public AudioFeed {
public:
    explicit MediaAudioFeed(const EditSession& session, std::size_t maxOpenDecoders = 16);
    ~MediaAudioFeed() override;

    MediaAudioFeed(const MediaAudioFeed&) = delete;
    MediaAudioFeed& operator=(const MediaAudioFeed&) = delete;

    /// A.4: the take file at `path` through its own decoders (a few, least recently used closed first).
    std::expected<std::size_t, Error> readTake(const std::string& path, Ticks sourceTime, std::span<float> interleaved,
                                               int channels, Rate rate) override;
    std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime,
                                           std::span<float> interleaved, int channels, Rate rate) override;

    std::size_t openDecoders() const noexcept;
    MediaAudioFeedStats stats() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace wabcut::playback

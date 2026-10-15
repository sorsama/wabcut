#pragma once

// AudioFeed: the boundary between timeline audio playback and media decoding.

#include <wabcut/model/Error.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/Time.h>

#include <cstddef>
#include <expected>
#include <span>
#include <string>

namespace wabcut::playback {

/// Supplies decoded PCM for the timeline audio player.
///
/// Threading: called on the player's feeder thread (or the thread calling TimelineAudioPlayer::feedOnce()),
/// never on the audio callback, so implementations may decode, block on IO and allocate. One player calls a
/// feed from one thread at a time; a feed shared by several players must be thread-safe.
///
/// Implementations: MediaAudioFeed (real files through media::AudioDecoder) and SyntheticAudioFeed (tests).
class AudioFeed {
public:
    virtual ~AudioFeed() = default;

    /// Decode the audio stream `streamOrdinal` (an index into MediaInfo::audio, i.e. Clip::stream.index) of
    /// `media`, starting at source time `sourceTime`, resampled to `rate` and mixed to `channels`, into the
    /// whole of `interleaved` (frames = interleaved.size() / channels).
    ///
    /// Returns the number of frames written from the start of the span. Fewer than requested means the media
    /// ended (or is not available yet); the caller zero-fills the rest. An error means the stream cannot be read
    /// at all (missing media, decode failure): the caller renders silence and counts it.
    virtual std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime,
                                                   std::span<float> interleaved, int channels, Rate rate) = 0;

    /// A.4: decodes an offline take of a stream (AudioTakes.h: a file of the stream's sound from source time 0) at
    /// source time `sourceTime`, like read(). A feed that cannot read takes reports Unsupported; the clip then plays
    /// its source.
    virtual std::expected<std::size_t, Error> readTake(const std::string& path, Ticks sourceTime, std::span<float> interleaved,
                                                       int channels, Rate rate) {
        (void)path;
        (void)sourceTime;
        (void)interleaved;
        (void)channels;
        (void)rate;
        return std::unexpected(Error{ErrorCode::Unsupported, "this audio feed plays no takes"});
    }
};

} // namespace wabcut::playback

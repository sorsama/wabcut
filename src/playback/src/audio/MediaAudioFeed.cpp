#include <wabcut/playback/audio/MediaAudioFeed.h>

#include <wabcut/audio/RealTime.h>
#include <wabcut/media/AudioDecoder.h>

#include <wabcut/model/ProjectState.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

namespace wabcut::playback {

namespace {

/// The single place this adapter talks to media::AudioDecoder::read. AudioDecoder fills the whole span (silence
/// before the stream starts and past its end) and returns the frames that came from the stream; the AudioFeed
/// contract counts frames written from the start of the span, so the whole span is reported, as it holds
/// exactly what the timeline should play there.
/// When AudioDecoder::read returns AudioReadResult{framesFromStream, leadingSilence, endOfStream}, this becomes
/// `leadingSilence + framesFromStream` (or the whole span while !endOfStream), with nothing else to change.
std::expected<std::size_t, Error> decodeInto(media::AudioDecoder& decoder, Ticks sourceTime, std::span<float> interleaved,
                                             int channels) {
    auto result = decoder.read(sourceTime, interleaved);
    if (!result) {
        return std::unexpected(std::move(result.error()));
    }
    return interleaved.size() / static_cast<std::size_t>(channels);
}

} // namespace

struct MediaAudioFeed::Impl {
    struct Key {
        std::uint64_t media = 0;
        int ordinal = 0;
        std::int32_t rateNum = 0;
        std::int32_t rateDen = 1;
        int channels = 0;
        bool operator==(const Key&) const = default;
    };
    struct Entry {
        Key key;
        MediaLocator locator; ///< what the decoder was opened from (relink detection)
        int streamIndex = -1;
        std::unique_ptr<media::AudioDecoder> decoder;
        std::uint64_t lastUse = 0;
    };

    Impl(const EditSession& s, std::size_t max) : session(s), maxOpen(std::max<std::size_t>(max, 1)) {}

    const EditSession& session;
    const std::size_t maxOpen;
    std::vector<Entry> entries; ///< feeder-thread only
    std::uint64_t useCounter = 0;
    std::atomic<std::size_t> open{0};
    std::atomic<std::uint64_t> opens{0};
    std::atomic<std::uint64_t> evictions{0};
    std::atomic<std::uint64_t> invalidations{0};
    std::atomic<std::uint64_t> errors{0};

    struct TakeEntry {
        std::string path;
        std::int32_t rateNum = 0;
        std::int32_t rateDen = 1;
        int channels = 0;
        std::unique_ptr<media::AudioDecoder> decoder;
        std::uint64_t lastUse = 0;
    };
    static constexpr std::size_t kMaxTakes = 8;
    std::vector<TakeEntry> takes; ///< feeder-thread only

    std::expected<std::size_t, Error> readTake(const std::string& path, Ticks sourceTime, std::span<float> interleaved,
                                               int channels, Rate rate) {
        if (audio::rt::inRealtimeSection()) {
            return fail(Error{ErrorCode::InvalidArgument, "MediaAudioFeed::readTake called on the audio callback"});
        }
        if (channels < 1 || interleaved.size() % static_cast<std::size_t>(channels) != 0) {
            return fail(Error{ErrorCode::InvalidArgument, "bad channel count or span size"});
        }
        auto it = std::ranges::find_if(takes, [&](const TakeEntry& e) {
            return e.path == path && e.rateNum == rate.num && e.rateDen == rate.den && e.channels == channels;
        });
        if (it == takes.end()) {
            // A take is a WAV of the stream alone: its one audio stream (-1: the best).
            auto decoder = media::AudioDecoder::open(path, -1, rate, channels);
            if (!decoder) {
                return fail(std::move(decoder.error()));
            }
            opens.fetch_add(1, std::memory_order_relaxed);
            if (takes.size() >= kMaxTakes) {
                takes.erase(std::ranges::min_element(takes, {}, &TakeEntry::lastUse));
                evictions.fetch_add(1, std::memory_order_relaxed);
            }
            takes.push_back({path, rate.num, rate.den, channels, std::move(*decoder), 0});
            it = takes.end() - 1;
        }
        it->lastUse = ++useCounter;
        auto decoded = decodeInto(*it->decoder, sourceTime, interleaved, channels);
        if (!decoded) {
            return fail(std::move(decoded.error()));
        }
        return decoded;
    }

    std::expected<std::size_t, Error> fail(Error error) {
        errors.fetch_add(1, std::memory_order_relaxed);
        return std::unexpected(std::move(error));
    }

    void erase(std::vector<Entry>::iterator it) {
        entries.erase(it);
        open.store(entries.size(), std::memory_order_relaxed);
    }

    std::expected<std::size_t, Error> read(MediaId media, int ordinal, Ticks sourceTime, std::span<float> interleaved,
                                           int channels, Rate rate) {
        if (audio::rt::inRealtimeSection()) {
            return fail(Error{ErrorCode::InvalidArgument, "MediaAudioFeed::read called on the audio callback"});
        }
        if (channels < 1 || interleaved.size() % static_cast<std::size_t>(channels) != 0) {
            return fail(Error{ErrorCode::InvalidArgument, "bad channel count or span size"});
        }
        // Resolve through the current snapshot: the item's locator and the container index of the stream.
        const ProjectSnapshot snapshot = session.snapshot();
        const MediaItem* item = snapshot ? snapshot->findMedia(media) : nullptr;
        const Key key{media.value, ordinal, rate.num, rate.den, channels};
        auto it = std::ranges::find_if(entries, [&](const Entry& e) { return e.key == key; });
        if (item == nullptr || ordinal < 0 || static_cast<std::size_t>(ordinal) >= item->info.audio.size()) {
            if (it != entries.end()) {
                erase(it); // removed from the project: drop its decoder
                invalidations.fetch_add(1, std::memory_order_relaxed);
            }
            return fail(Error{ErrorCode::NotFound, "media item or audio stream not in the project"});
        }
        if (item->locator.offline) {
            return fail(Error{ErrorCode::NotFound, "media is offline: " + item->locator.path});
        }
        const int streamIndex = item->info.audio[static_cast<std::size_t>(ordinal)].index;
        if (it != entries.end() && (it->locator != item->locator || it->streamIndex != streamIndex)) {
            erase(it); // relinked (or the stream mapping changed): reopen from the new file
            invalidations.fetch_add(1, std::memory_order_relaxed);
            it = entries.end();
        }
        if (it == entries.end()) {
            auto decoder = media::AudioDecoder::open(item->locator.path, streamIndex, rate, channels);
            if (!decoder) {
                return fail(std::move(decoder.error()));
            }
            opens.fetch_add(1, std::memory_order_relaxed);
            if (entries.size() >= maxOpen) {
                const auto oldest = std::ranges::min_element(entries, {}, &Entry::lastUse);
                erase(oldest);
                evictions.fetch_add(1, std::memory_order_relaxed);
            }
            entries.push_back({key, item->locator, streamIndex, std::move(*decoder), 0});
            open.store(entries.size(), std::memory_order_relaxed);
            it = entries.end() - 1;
        }
        it->lastUse = ++useCounter;
        auto decoded = decodeInto(*it->decoder, sourceTime, interleaved, channels);
        if (!decoded) {
            return fail(std::move(decoded.error()));
        }
        return decoded;
    }
};

MediaAudioFeed::MediaAudioFeed(const EditSession& session, std::size_t maxOpenDecoders)
    : m_impl(std::make_unique<Impl>(session, maxOpenDecoders)) {}

MediaAudioFeed::~MediaAudioFeed() = default;

std::expected<std::size_t, Error> MediaAudioFeed::read(MediaId media, int streamOrdinal, Ticks sourceTime,
                                                       std::span<float> interleaved, int channels, Rate rate) {
    return m_impl->read(media, streamOrdinal, sourceTime, interleaved, channels, rate);
}

std::expected<std::size_t, Error> MediaAudioFeed::readTake(const std::string& path, Ticks sourceTime,
                                                           std::span<float> interleaved, int channels, Rate rate) {
    return m_impl->readTake(path, sourceTime, interleaved, channels, rate);
}

std::size_t MediaAudioFeed::openDecoders() const noexcept { return m_impl->open.load(std::memory_order_relaxed); }

MediaAudioFeedStats MediaAudioFeed::stats() const noexcept {
    MediaAudioFeedStats s;
    s.opens = m_impl->opens.load(std::memory_order_relaxed);
    s.evictions = m_impl->evictions.load(std::memory_order_relaxed);
    s.invalidations = m_impl->invalidations.load(std::memory_order_relaxed);
    s.errors = m_impl->errors.load(std::memory_order_relaxed);
    return s;
}

} // namespace wabcut::playback

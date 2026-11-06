#pragma once

// The sound the Spectral tab's spectrogram is made of (internal to SpectralRepairBridge; header-only so the tests can use them):
//   FeedSource      the audio stream of a media item as one mono signal (all channels mixed, or one channel), read through
//                   any playback::AudioFeed in blocks; positions outside the media read as silence.
//   ProjectFeed     the player's own decoders (playback::MediaAudioFeed) over a copy of the project.
//   RepairedSource  another source as a clip's spectral edits repair it: the code the player runs (dsp::SpectralRepair), over
//                   that one signal, with the STFT grid anchored to the sample number in the media so every read of the same
//                   stretch agrees, wherever it starts.

#include <wabcut/audio/dsp/SpectralRepair.h>
#include <wabcut/audio/dsp/Spectrogram.h>
#include <wabcut/model/EditSession.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/SpectralRepair.h>
#include <wabcut/playback/audio/AudioFeed.h>
#include <wabcut/playback/audio/MediaAudioFeed.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace wabcut::app {

/// The player's own decoders over a copy of the project's state (MediaAudioFeed reads locators through an EditSession).
class ProjectFeed final : public playback::AudioFeed {
public:
    explicit ProjectFeed(const ProjectState& state) : m_session(state), m_feed(m_session) {}
    std::expected<std::size_t, Error> read(MediaId media, int streamOrdinal, Ticks sourceTime, std::span<float> interleaved,
                                           int channels, Rate rate) override {
        return m_feed.read(media, streamOrdinal, sourceTime, interleaved, channels, rate);
    }

private:
    EditSession m_session;
    playback::MediaAudioFeed m_feed; // after m_session: it reads the session
};

class FeedSource final : public audio::dsp::SampleSource {
public:
    /// `channels` of the stream are read; `channel` < 0 mixes them to the mean, else that channel (clamped).
    FeedSource(std::shared_ptr<playback::AudioFeed> feed, MediaId media, int streamOrdinal, int sampleRate, std::int64_t length,
               int channels, int channel)
        : m_feed(std::move(feed)), m_media(media), m_stream(streamOrdinal), m_rate(sampleRate), m_length(length),
          m_channels(std::clamp(channels, 1, 8)), m_channel(channel < 0 ? -1 : std::min(channel, std::clamp(channels, 1, 8) - 1)) {}

    std::int64_t length() const override { return m_length; }

    void read(std::int64_t start, float* out, std::int64_t count) override {
        std::fill(out, out + count, 0.0f);
        const std::int64_t lo = std::max<std::int64_t>(start, 0);
        const std::int64_t hi = std::min(start + count, m_length);
        if (hi <= lo) {
            return;
        }
        constexpr std::int64_t kChunk = 8192;
        const auto ch = static_cast<std::size_t>(m_channels);
        m_buffer.resize(static_cast<std::size_t>(kChunk) * ch);
        for (std::int64_t at = lo; at < hi; at += kChunk) {
            const std::int64_t n = std::min(kChunk, hi - at);
            std::span<float> span(m_buffer.data(), static_cast<std::size_t>(n) * ch);
            std::fill(span.begin(), span.end(), 0.0f);
            const auto got = m_feed->read(m_media, m_stream, samplesToTicks(at, Rate{m_rate, 1}), span, m_channels, Rate{m_rate, 1});
            if (!got) {
                continue; // an unreadable stretch is silence
            }
            const auto frames = static_cast<std::int64_t>(std::min<std::size_t>(*got, static_cast<std::size_t>(n)));
            for (std::int64_t f = 0; f < frames; ++f) {
                const float* frame = m_buffer.data() + static_cast<std::size_t>(f) * ch;
                float v = 0.0f;
                if (m_channel >= 0) {
                    v = frame[m_channel];
                } else {
                    for (std::size_t c = 0; c < ch; ++c) {
                        v += frame[c];
                    }
                    v /= static_cast<float>(ch);
                }
                out[at - start + f] = v;
            }
        }
    }

private:
    std::shared_ptr<playback::AudioFeed> m_feed;
    MediaId m_media;
    int m_stream;
    int m_rate;
    std::int64_t m_length;
    int m_channels;
    int m_channel;
    std::vector<float> m_buffer;
};

class RepairedSource final : public audio::dsp::SampleSource {
public:
    RepairedSource(std::shared_ptr<audio::dsp::SampleSource> base, std::vector<SpectralEdit> edits, int rate, int fftSize)
        : m_base(std::move(base)), m_edits(std::move(edits)), m_rate(rate), m_fft(fftSize) {}

    std::int64_t length() const override { return m_base->length(); }

    void read(std::int64_t start, float* out, std::int64_t count) override {
        audio::dsp::SpectralRepair repair(1, m_rate, m_edits, audio::dsp::SpectralRepairOptions{m_fft, audio::dsp::SpectralListen::Off});
        if (!repair.active()) {
            m_base->read(start, out, count);
            return;
        }
        const std::int64_t pre = repair.settleFrames();
        const std::int64_t post = repair.latencyFrames();
        std::vector<float> buffer(static_cast<std::size_t>(pre + count + post), 0.0f);
        m_base->read(start - pre, buffer.data(), pre + count + post);
        const audio::dsp::IdentityTimeMap map(m_rate, static_cast<double>(start - pre) / m_rate);
        constexpr std::int64_t kBlock = 4096;
        for (std::int64_t done = 0; done < pre + count + post; done += kBlock) {
            const auto n = static_cast<int>(std::min(kBlock, pre + count + post - done));
            repair.process(buffer.data() + done, n, done, start - pre, map);
        }
        std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(pre + post), static_cast<std::ptrdiff_t>(count), out);
    }

private:
    std::shared_ptr<audio::dsp::SampleSource> m_base;
    std::vector<SpectralEdit> m_edits;
    int m_rate;
    int m_fft;
};

} // namespace wabcut::app

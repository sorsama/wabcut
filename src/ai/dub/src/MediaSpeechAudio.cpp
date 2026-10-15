#include <wabcut/ai/dub/MediaSpeechAudio.h>

#include <wabcut/ai/audio/WavFile.h>
#include <wabcut/audio/dsp/TimeStretch.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Probe.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>

namespace wabcut::ai::dub {

namespace {

constexpr std::int64_t kMaxLineSeconds = 600;

std::string extensionOf(std::string_view mediaType) {
    if (mediaType == "audio/wav" || mediaType == "audio/x-wav" || mediaType == "audio/wave") {
        return ".wav";
    }
    if (mediaType == "audio/ogg" || mediaType == "audio/opus") {
        return ".ogg";
    }
    if (mediaType == "audio/aac") {
        return ".aac";
    }
    if (mediaType == "audio/flac") {
        return ".flac";
    }
    return ".mp3";
}

/// Everything a decoder delivers from `from` on, at most `limit` frames (mono).
std::expected<std::vector<float>, Error> readAll(media::AudioDecoder& decoder, int rate, Ticks from, std::int64_t limit) {
    std::vector<float> out;
    std::vector<float> block(static_cast<std::size_t>(rate));
    std::int64_t frames = 0;
    while (frames < limit) {
        const std::size_t want = static_cast<std::size_t>(std::min<std::int64_t>(rate, limit - frames));
        auto read = decoder.read(from + samplesToTicks(frames, Rate{rate, 1}), std::span<float>(block.data(), want));
        if (!read) {
            return std::unexpected(read.error());
        }
        const std::size_t got = read->leadingSilence + read->framesFromStream;
        out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(std::min(got, want)));
        frames += static_cast<std::int64_t>(want);
        if (read->endOfStream || got == 0) {
            break;
        }
    }
    return out;
}

class MediaSpeechAudio final : public SpeechAudio {
public:
    explicit MediaSpeechAudio(std::filesystem::path scratch) : m_scratch(std::move(scratch)) {}

    std::expected<std::vector<float>, Error> decode(const SpeechResult& speech, int rate) override {
        std::error_code ec;
        std::filesystem::create_directories(m_scratch, ec);
        static std::atomic<std::uint64_t> counter{0};
        const std::filesystem::path file = m_scratch / ("tts-" + std::to_string(++counter) + extensionOf(speech.mediaType));
        {
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(speech.audio.data()), static_cast<std::streamsize>(speech.audio.size()));
            if (!out) {
                return std::unexpected(Error{ErrorCode::IoError, "cannot write " + pathToUtf8(file)});
            }
        }
        struct Remove {
            const std::filesystem::path& f;
            ~Remove() {
                std::error_code e;
                std::filesystem::remove(f, e);
            }
        } remove{file};
        auto decoder = media::AudioDecoder::open(pathToUtf8(file), -1, Rate{rate, 1}, 1);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        return readAll(**decoder, rate, Ticks{}, kMaxLineSeconds * rate);
    }

    std::vector<float> stretch(std::span<const float> samples, int rate, double speed) override {
        return stretchOffline(samples, rate, speed);
    }

    std::expected<MediaItem, Error> write(const std::filesystem::path& file, std::span<const float> samples, int rate) override {
        std::error_code exists;
        if (std::filesystem::exists(file, exists)) {
            // Never over an earlier file: clips of another run (or a decoder holding it open) use it (A5-P1-6).
            return std::unexpected(Error{ErrorCode::IoError, pathToUtf8(file) + " already exists"});
        }
        // Appended to the path, not to its narrow string(), which throws on Windows for names outside the code page.
        std::filesystem::path part = file;
        part += ".part";
        auto writer = ai::audio::WavWriter::create(part, 1, rate);
        if (!writer) {
            return std::unexpected(writer.error());
        }
        if (auto written = writer->write(samples); !written) {
            return std::unexpected(written.error());
        }
        if (auto closed = writer->close(); !closed) {
            return std::unexpected(closed.error());
        }
        std::error_code ec;
        std::filesystem::rename(part, file, ec);
        if (ec) {
            return std::unexpected(Error{ErrorCode::IoError, "cannot move " + pathToUtf8(part) + ": " + ec.message()});
        }
        auto info = media::probe(pathToUtf8(file));
        if (!info) {
            return std::unexpected(info.error());
        }
        auto fingerprint = fingerprintFile(file);
        if (!fingerprint) {
            return std::unexpected(fingerprint.error());
        }
        MediaItem item;
        item.name = pathToUtf8(file.stem());
        item.locator.path = pathToUtf8(file);
        item.locator.fingerprint = std::move(*fingerprint);
        item.info = std::move(*info);
        return item;
    }

    std::expected<std::filesystem::path, Error> extractSample(const MediaItem& media, int stream, std::span<const TimeRange> ranges,
                                                              const std::filesystem::path& file) override {
        if (stream < 0 || static_cast<std::size_t>(stream) >= media.info.audio.size()) {
            return std::unexpected(Error{ErrorCode::NotFound, "'" + media.name + "' has no audio stream " + std::to_string(stream)});
        }
        constexpr int rate = 44100;
        auto decoder = media::AudioDecoder::open(media.locator.path, media.info.audio[static_cast<std::size_t>(stream)].index, Rate{rate, 1}, 1);
        if (!decoder) {
            return std::unexpected(decoder.error());
        }
        std::vector<float> sample;
        const std::vector<float> pause(static_cast<std::size_t>(rate / 4), 0.0f);
        for (const TimeRange& r : ranges) {
            const std::int64_t frames = r.duration.value * rate / kTicksPerSecond;
            auto read = readAll(**decoder, rate, r.start, frames);
            if (!read) {
                return std::unexpected(read.error());
            }
            sample.insert(sample.end(), read->begin(), read->end());
            sample.insert(sample.end(), pause.begin(), pause.end());
        }
        auto writer = ai::audio::WavWriter::create(file, 1, rate);
        if (!writer) {
            return std::unexpected(writer.error());
        }
        if (auto written = writer->write(sample); !written) {
            return std::unexpected(written.error());
        }
        if (auto closed = writer->close(); !closed) {
            return std::unexpected(closed.error());
        }
        return file;
    }

private:
    std::filesystem::path m_scratch;
};

} // namespace

std::vector<float> stretchOffline(std::span<const float> samples, int rate, double speed) {
    if (samples.empty() || !(speed > 0.0) || std::abs(speed - 1.0) < 1e-6) {
        return {samples.begin(), samples.end()};
    }
    const auto target = static_cast<std::size_t>(std::llround(static_cast<double>(samples.size()) / speed));
    wabcut::audio::dsp::TimeStretch stretcher(1, static_cast<double>(rate), std::max(4.0, speed), 8192);
    stretcher.setSpeed(speed);
    std::vector<float> out;
    out.reserve(target + 8192);
    std::vector<float> block(8192);
    const std::vector<float> zeros(4096, 0.0f);
    std::size_t at = 0;
    std::size_t flushed = 0;
    const std::size_t flush = static_cast<std::size_t>(stretcher.latencyInputFrames()) + static_cast<std::size_t>(rate / 10);
    const auto drain = [&] {
        int got = 0;
        while ((got = stretcher.pull(block.data(), static_cast<int>(block.size()))) > 0) {
            out.insert(out.end(), block.begin(), block.begin() + got);
        }
    };
    while ((at < samples.size() || flushed < flush) && out.size() < target) {
        if (at < samples.size()) {
            const std::size_t n = std::min<std::size_t>(4096, samples.size() - at);
            stretcher.push(samples.data() + at, static_cast<int>(n));
            at += n;
        } else {
            const std::size_t n = std::min(zeros.size(), flush - flushed);
            stretcher.push(zeros.data(), static_cast<int>(n));
            flushed += n;
        }
        drain();
    }
    out.resize(target, 0.0f);
    return out;
}

std::shared_ptr<SpeechAudio> makeMediaSpeechAudio(std::filesystem::path scratch) {
    return std::make_shared<MediaSpeechAudio>(std::move(scratch));
}

} // namespace wabcut::ai::dub

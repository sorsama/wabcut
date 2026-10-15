#include "Background.h"
#include "FfmpegUtil.h"

#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/media/WaveformPeaks.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>

namespace wabcut::media {

using namespace detail;

namespace {

constexpr std::uint32_t kPeaksMagic = 0x4b504357; // "WCPK"
constexpr std::uint32_t kPeaksVersion = 2;        // 2: keyed by source fingerprint
/// Values of the finest level reserved up front from the container's duration (16 MB of int16: a 2-hour stereo
/// stream at 48 kHz needs 5.4 M). Bounded, since the duration is only the file's claim.
constexpr std::int64_t kMaxReservedValues = std::int64_t{1} << 23;

std::int16_t quantizeMin(float v) {
    return static_cast<std::int16_t>(std::clamp(std::floor(v * 32767.0f), -32768.0f, 32767.0f));
}
std::int16_t quantizeMax(float v) {
    return static_cast<std::int16_t>(std::clamp(std::ceil(v * 32767.0f), -32768.0f, 32767.0f));
}

std::expected<void, Error> validate(const WaveformOptions& o) {
    if (o.samplesPerBin.empty() || o.samplesPerBin.size() > 8) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "1..8 peak levels required"});
    }
    for (std::size_t i = 0; i < o.samplesPerBin.size(); ++i) {
        const int spb = o.samplesPerBin[i];
        if (spb < 16 || (i > 0 && (spb <= o.samplesPerBin[i - 1] || spb % o.samplesPerBin[i - 1] != 0))) {
            return std::unexpected(
                Error{ErrorCode::InvalidArgument,
                      "Peak levels must be >= 16 samples and multiples of the previous level"});
        }
    }
    return {};
}

std::string peaksKey(std::uint64_t fingerprint, const WaveformOptions& o) {
    std::string levels;
    for (const int spb : o.samplesPerBin) {
        levels += std::format("{},", spb);
    }
    return std::format("peaks/v{}|fp={:016x}|stream={}|levels={}", kPeaksVersion, fingerprint, o.stream,
                       levels);
}

std::filesystem::path peaksFile(const std::filesystem::path& cacheDir, const std::string& key) {
    const auto* data = reinterpret_cast<const std::uint8_t*>(key.data());
    return cacheDir / "peaks" / (hex64(fnv1a({data, key.size()})) + ".wpeaks");
}

std::optional<WaveformPeaks> loadPeaks(const std::filesystem::path& file, const std::string& key) {
    const auto bytes = readWholeFile(file);
    if (!bytes || !ByteReader::verifyChecksum(*bytes)) {
        return std::nullopt;
    }
    ByteReader in(*bytes);
    std::uint32_t magic = 0;
    std::uint32_t version = 0;
    std::string storedKey;
    WaveformPeaks peaks;
    std::int32_t rate = 0;
    std::uint32_t levelCount = 0;
    if (!in.pod(magic) || magic != kPeaksMagic || !in.pod(version) || version != kPeaksVersion ||
        !in.string(storedKey) || storedKey != key || !in.pod(rate) || rate <= 0 || !in.pod(peaks.channels) ||
        peaks.channels <= 0 || peaks.channels > 16 || !in.pod(peaks.sampleCount) || peaks.sampleCount < 0 ||
        !in.pod(levelCount) || levelCount > 8) {
        return std::nullopt;
    }
    peaks.sampleRate = Rate{rate, 1};
    peaks.levels.resize(levelCount);
    for (PeakLevel& level : peaks.levels) {
        if (!in.pod(level.samplesPerBin) || level.samplesPerBin <= 0 || !in.pod(level.binCount) ||
            level.binCount < 0) {
            return std::nullopt;
        }
        const auto values = static_cast<std::uint64_t>(level.binCount) * peaks.channels * 2;
        if (values * sizeof(std::int16_t) > in.remaining()) {
            return std::nullopt;
        }
        level.peaks.resize(static_cast<std::size_t>(values));
        in.raw(level.peaks.data(), level.peaks.size() * sizeof(std::int16_t));
    }
    peaks.fromCache = true;
    return peaks;
}

bool savePeaks(const std::filesystem::path& file, const std::string& key, const WaveformPeaks& peaks) {
    ByteWriter out;
    out.pod(kPeaksMagic);
    out.pod(kPeaksVersion);
    out.string(key);
    out.pod(static_cast<std::int32_t>(peaks.sampleRate.num));
    out.pod(static_cast<std::int32_t>(peaks.channels));
    out.pod(peaks.sampleCount);
    out.pod(static_cast<std::uint32_t>(peaks.levels.size()));
    for (const PeakLevel& level : peaks.levels) {
        out.pod(static_cast<std::int32_t>(level.samplesPerBin));
        out.pod(level.binCount);
        out.raw(level.peaks.data(), level.peaks.size() * sizeof(std::int16_t));
    }
    out.checksum();
    return writeFileAtomic(file, out.bytes());
}

} // namespace

const PeakLevel* WaveformPeaks::levelFor(double samplesPerPixel) const {
    const PeakLevel* best = levels.empty() ? nullptr : &levels.front();
    for (const PeakLevel& level : levels) {
        if (level.samplesPerBin <= samplesPerPixel) {
            best = &level;
        }
    }
    return best;
}

std::expected<WaveformPeaks, Error> computeWaveformPeaks(const std::string& path,
                                                         const WaveformOptions& options, std::stop_token stop,
                                                         const std::function<void(float)>& progress,
                                                         const WaveformPartials& partials) {
    if (auto valid = validate(options); !valid) {
        return std::unexpected(valid.error());
    }
    // Resolve the stream and its native format first; the decoder then runs without resampling or remixing.
    AudioStreamInfo info;
    {
        auto fmt = openInput(path);
        if (!fmt) {
            return std::unexpected(fmt.error());
        }
        auto stream = selectStream(fmt->get(), AVMEDIA_TYPE_AUDIO, options.stream);
        if (!stream) {
            return std::unexpected(stream.error());
        }
        info = describeAudioStream(fmt->get(), *stream);
    }
    if (info.sampleRate.num <= 0 || info.channels < 1 || info.channels > 16) {
        return std::unexpected(Error{ErrorCode::Unsupported, "Unsupported audio format for peaks"});
    }
    auto decoder = AudioDecoder::open(path, info.index, Rate{info.sampleRate.num, 1}, info.channels);
    if (!decoder) {
        return std::unexpected(decoder.error());
    }

    const int channels = info.channels;
    const Rate rate{info.sampleRate.num, 1};
    const int finest = options.samplesPerBin.front();
    const std::int64_t expected = std::max<std::int64_t>(ticksToSamples(info.duration, rate), 1);
    const std::int64_t giveUpAt = expected + 10LL * rate.num; // safety net: no audio ever appears

    WaveformPeaks peaks;
    peaks.sampleRate = rate;
    peaks.channels = channels;

    // Level 0 is built while decoding, and every coarser level from level 0's quantised bins as they complete: a
    // coarse bin is the min/max of `factor` consecutive finest bins (the last one may be partial), exactly what
    // reducing the finished level 0 gives. Built along the way, a partial snapshot is a copy, not a recomputation.
    PeakLevel level0;
    level0.samplesPerBin = finest;
    const std::int64_t expectedValues = (expected + finest - 1) / finest * channels * 2;
    level0.peaks.reserve(static_cast<std::size_t>(std::min(expectedValues, kMaxReservedValues)));
    struct CoarseLevel {
        PeakLevel level;              ///< its complete bins
        std::int64_t factor = 1;      ///< finest bins per bin
        std::int64_t fill = 0;        ///< finest bins in the open bin
        std::vector<std::int16_t> lo; ///< the open bin, per channel
        std::vector<std::int16_t> hi;
    };
    std::vector<CoarseLevel> coarse;
    for (std::size_t li = 1; li < options.samplesPerBin.size(); ++li) {
        CoarseLevel k;
        k.level.samplesPerBin = options.samplesPerBin[li];
        k.factor = k.level.samplesPerBin / finest;
        k.lo.assign(static_cast<std::size_t>(channels), std::numeric_limits<std::int16_t>::max());
        k.hi.assign(static_cast<std::size_t>(channels), std::numeric_limits<std::int16_t>::min());
        coarse.push_back(std::move(k));
    }
    const auto closeCoarseBin = [&](CoarseLevel& k) {
        for (std::size_t c = 0; c < static_cast<std::size_t>(channels); ++c) {
            k.level.peaks.push_back(k.lo[c]);
            k.level.peaks.push_back(k.hi[c]);
            k.lo[c] = std::numeric_limits<std::int16_t>::max();
            k.hi[c] = std::numeric_limits<std::int16_t>::min();
        }
        ++k.level.binCount;
        k.fill = 0;
    };
    std::vector<float> binMin(static_cast<std::size_t>(channels), std::numeric_limits<float>::max());
    std::vector<float> binMax(static_cast<std::size_t>(channels), std::numeric_limits<float>::lowest());
    int binFill = 0;
    const auto flushBin = [&] {
        for (std::size_t c = 0; c < static_cast<std::size_t>(channels); ++c) {
            const std::int16_t lo = quantizeMin(binMin[c]);
            const std::int16_t hi = quantizeMax(binMax[c]);
            level0.peaks.push_back(lo);
            level0.peaks.push_back(hi);
            for (CoarseLevel& k : coarse) {
                k.lo[c] = std::min(k.lo[c], lo);
                k.hi[c] = std::max(k.hi[c], hi);
            }
            binMin[c] = std::numeric_limits<float>::max();
            binMax[c] = std::numeric_limits<float>::lowest();
        }
        ++level0.binCount;
        binFill = 0;
        for (CoarseLevel& k : coarse) {
            if (++k.fill == k.factor) {
                closeCoarseBin(k);
            }
        }
    };

    // Partial snapshots: the complete bins so far. Level 0 holds only complete bins until the end (the partial last
    // bin is flushed after the loop), and each coarse level's open bin lives outside its vector.
    using Clock = std::chrono::steady_clock;
    auto lastPartial = Clock::now();
    std::int64_t partialBins = 0; // finest bins in the last snapshot
    const auto deliverPartial = [&] {
        auto snapshot = std::make_shared<WaveformPeaks>();
        snapshot->sampleRate = rate;
        snapshot->channels = channels;
        snapshot->sampleCount = level0.binCount * finest;
        snapshot->levels.reserve(1 + coarse.size());
        snapshot->levels.push_back(level0);
        for (const CoarseLevel& k : coarse) {
            snapshot->levels.push_back(k.level);
        }
        partialBins = level0.binCount;
        partials.onPartial(std::shared_ptr<const WaveformPeaks>(std::move(snapshot)));
    };

    const std::int64_t blockFrames = static_cast<std::int64_t>(finest) * 256;
    std::vector<float> block(static_cast<std::size_t>(blockFrames * channels));
    std::int64_t position = 0;
    for (;;) {
        if (stop.stop_requested()) {
            return std::unexpected(Error{ErrorCode::Cancelled, "Cancelled"});
        }
        auto got = (*decoder)->read(samplesToTicks(position, rate), block);
        if (!got) {
            return std::unexpected(got.error());
        }
        // Silence before the stream's audio (it starts after the media origin) is part of the waveform; the
        // silence after the end of the stream is not.
        const AudioReadResult& r = *got;
        std::int64_t valid = blockFrames;
        bool finished = false;
        if (r.endOfStream) {
            valid = static_cast<std::int64_t>(r.leadingSilence + r.framesFromStream);
            finished = true;
        } else if (r.framesFromStream == 0 && position >= giveUpAt) {
            break;
        }
        for (std::int64_t i = 0; i < valid; ++i) {
            const float* frame = block.data() + i * channels;
            for (int c = 0; c < channels; ++c) {
                binMin[static_cast<std::size_t>(c)] = std::min(binMin[static_cast<std::size_t>(c)], frame[c]);
                binMax[static_cast<std::size_t>(c)] = std::max(binMax[static_cast<std::size_t>(c)], frame[c]);
            }
            if (++binFill == finest) {
                flushBin();
            }
        }
        position += valid;
        if (progress) {
            progress(std::min(0.99f, static_cast<float>(position) / static_cast<float>(expected)));
        }
        if (finished) {
            break; // the final result follows: no snapshot of the last block
        }
        if (partials.onPartial && level0.binCount > partialBins) {
            if (const auto now = Clock::now(); now - lastPartial >= partials.interval) {
                lastPartial = now;
                deliverPartial();
            }
        }
    }
    if (binFill > 0) {
        flushBin();
    }
    peaks.sampleCount = position;
    if (level0.peaks.capacity() - level0.peaks.size() > level0.peaks.size() / 8) {
        level0.peaks.shrink_to_fit(); // the container claimed a longer stream than it holds
    }

    peaks.levels.reserve(1 + coarse.size());
    peaks.levels.push_back(std::move(level0));
    for (CoarseLevel& k : coarse) {
        if (k.fill > 0) {
            closeCoarseBin(k); // the partial last bin
        }
        peaks.levels.push_back(std::move(k.level));
    }
    if (progress) {
        progress(1.0f);
    }
    return peaks;
}

// ---- WaveformPeaksService
// ----------------------------------------------------------------------------------------

struct WaveformPeaksService::Workers {
    explicit Workers(int threads) : pool(threads) {}
    WorkerPool pool;
};

WaveformPeaksService::WaveformPeaksService(std::filesystem::path cacheDir, int workerThreads)
    : m_cacheDir(std::move(cacheDir)), m_workers(std::make_unique<Workers>(std::max(1, workerThreads))) {}

WaveformPeaksService::~WaveformPeaksService() = default;

std::filesystem::path WaveformPeaksService::cacheFileFor(const std::string& path,
                                                         const WaveformOptions& options) const {
    const auto fingerprint = sourceFingerprint(path);
    return fingerprint ? peaksFile(m_cacheDir, peaksKey(*fingerprint, options)) : std::filesystem::path{};
}

std::expected<WaveformPeaks, Error>
WaveformPeaksService::getOrCompute(const std::string& path, const WaveformOptions& options,
                                   std::stop_token stop, const std::function<void(float)>& progress,
                                   const WaveformPartials& partials) {
    if (auto valid = validate(options); !valid) {
        return std::unexpected(valid.error());
    }
    const auto fingerprint = sourceFingerprint(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    const std::string key = peaksKey(*fingerprint, options);
    const std::filesystem::path file = peaksFile(m_cacheDir, key);
    if (auto cached = loadPeaks(file, key)) {
        return std::move(*cached);
    }
    auto peaks = computeWaveformPeaks(path, options, stop, progress, partials);
    if (peaks && !savePeaks(file, key, *peaks)) {
        log(LogLevel::Warning, "Could not write the peaks cache file {}", pathToUtf8(file));
    }
    return peaks;
}

Job<WaveformPeaks> WaveformPeaksService::request(const std::string& path, const WaveformOptions& options,
                                                 Callback onDone, WaveformPartials partials) {
    return submitJob<WaveformPeaks>(
        m_workers->pool, std::move(onDone),
        [this, path, options, partials = std::move(partials)](std::stop_token stop,
                                                              const std::function<void(float)>& progress) {
            return getOrCompute(path, options, stop, progress, partials);
        });
}

} // namespace wabcut::media

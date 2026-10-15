#include <wabcut/ai/edit/MediaServices.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MediaLocator.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace wabcut::ai::edit {

namespace {

constexpr Rate kProbeRate{16000, 1};

std::optional<std::string> mediaPath(const MediaItem& media, const std::filesystem::path& projectFile) {
    if (auto path = resolveMediaPath(media.locator, projectFile)) {
        return pathToUtf8(*path);
    }
    if (!media.info.path.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(pathFromUtf8(media.info.path), ec)) {
            return media.info.path;
        }
    }
    return std::nullopt;
}

/// Container index of the media's audio stream `audioStream`, -1 when the probe did not record one.
int containerStream(const MediaItem& media, int audioStream) {
    if (audioStream >= 0 && static_cast<std::size_t>(audioStream) < media.info.audio.size()) {
        return media.info.audio[static_cast<std::size_t>(audioStream)].index;
    }
    return -1;
}

} // namespace

std::optional<std::string> mediaFilePath(const MediaItem& media, const std::filesystem::path& projectFile) {
    return mediaPath(media, projectFile);
}

int audioContainerStream(const MediaItem& media, int audioStream) { return containerStream(media, audioStream); }

// --- CachedTranscriptProvider --------------------------------------------------------------------------------

CachedTranscriptProvider::CachedTranscriptProvider(std::filesystem::path cacheRoot, std::filesystem::path projectFile)
    : m_root(std::move(cacheRoot)), m_projectFile(std::move(projectFile)) {}

std::shared_ptr<const Transcript> CachedTranscriptProvider::transcript(const MediaItem& media, int audioStream) const {
    const auto path = mediaPath(media, m_projectFile);
    if (!path) {
        return nullptr;
    }
    std::lock_guard lock(m_mutex);
    const auto key = std::pair{*path, audioStream};
    // What was found (or not) holds until this process stores another analysis entry: a transcription may have
    // just made this stream's transcript, or a newer one.
    const std::uint64_t writes = AnalysisCache::writeCount();
    if (const auto it = m_found.find(key); it != m_found.end() && it->second.writes == writes) {
        return it->second.transcript;
    }
    Found& slot = m_found[key];
    slot.writes = writes;
    const auto fingerprint = media::sourceFingerprint(*path);
    if (!fingerprint) {
        slot = Found{nullptr, writes, {}, {}};
        return nullptr;
    }
    // <root>/v1/<fingerprint>/<stream>/transcript/<model version>-<params hash>.json: the newest entry of the
    // stream (any model, any settings). Without a recorded container index, the audioStream-th stream that has
    // transcripts.
    const AnalysisCache cache(m_root);
    const std::filesystem::path sourceDir =
        cache.pathFor(AnalysisKey{*fingerprint, 0, "transcript", "m", "p"}).parent_path().parent_path().parent_path();
    std::vector<int> streams;
    if (const int index = containerStream(media, audioStream); index >= 0) {
        streams.push_back(index);
    } else {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(sourceDir, ec)) {
            try {
                streams.push_back(std::stoi(pathToUtf8(entry.path().filename())));
            } catch (...) {
            }
        }
        std::ranges::sort(streams);
        if (audioStream >= 0 && static_cast<std::size_t>(audioStream) < streams.size()) {
            streams = {streams[static_cast<std::size_t>(audioStream)]};
        } else {
            streams.clear();
        }
    }
    for (const int stream : streams) {
        const std::filesystem::path dir = sourceDir / std::to_string(stream) / "transcript";
        std::error_code ec;
        std::optional<std::filesystem::path> newest;
        std::filesystem::file_time_type newestTime{};
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() != ".json") {
                continue;
            }
            const auto time = entry.last_write_time(ec);
            if (!newest || time > newestTime || (time == newestTime && entry.path() > *newest)) {
                newest = entry.path();
                newestTime = time;
            }
        }
        if (newest) {
            if (slot.transcript && slot.file == *newest && slot.fileTime == newestTime) {
                return slot.transcript; // still the newest: no need to read it again
            }
            if (auto t = readTranscriptFile(*newest)) {
                slot = Found{std::make_shared<const Transcript>(std::move(*t)), writes, *newest, newestTime};
                return slot.transcript;
            }
        }
    }
    slot = Found{nullptr, writes, {}, {}};
    return nullptr;
}

// --- DecoderEnergyProbe --------------------------------------------------------------------------------------

struct DecoderEnergyProbe::Decoders {
    std::mutex mutex;
    std::map<std::pair<std::string, int>, std::unique_ptr<media::AudioDecoder>> open; ///< by file and stream
};

DecoderEnergyProbe::DecoderEnergyProbe(std::filesystem::path projectFile)
    : m_projectFile(std::move(projectFile)), m_decoders(std::make_unique<Decoders>()) {}

DecoderEnergyProbe::~DecoderEnergyProbe() = default;

std::optional<double> DecoderEnergyProbe::rmsDb(const MediaItem& media, int audioStream, TimeRange source) const {
    if (source.duration <= Ticks{}) {
        return std::nullopt;
    }
    const auto path = mediaPath(media, m_projectFile);
    if (!path) {
        return std::nullopt;
    }
    std::lock_guard lock(m_decoders->mutex);
    auto& decoder = m_decoders->open[{*path, audioStream}];
    if (!decoder) {
        auto opened = media::AudioDecoder::open(*path, containerStream(media, audioStream), kProbeRate,
                                                media::ChannelLayout::mono());
        if (!opened) {
            return std::nullopt;
        }
        decoder = std::move(*opened);
    }
    const std::int64_t total = ticksToSamples(source.duration, kProbeRate);
    if (total <= 0) {
        return std::nullopt;
    }
    std::vector<float> buffer(4096);
    double sum = 0.0;
    std::int64_t counted = 0;
    for (std::int64_t done = 0; done < total;) {
        const auto n = static_cast<std::size_t>(std::min<std::int64_t>(total - done, static_cast<std::int64_t>(buffer.size())));
        const auto read = decoder->read(source.start + samplesToTicks(done, kProbeRate), std::span<float>(buffer.data(), n));
        if (!read) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < n; ++i) {
            sum += static_cast<double>(buffer[i]) * static_cast<double>(buffer[i]);
        }
        counted += static_cast<std::int64_t>(n);
        done += static_cast<std::int64_t>(n);
        if (read->endOfStream) {
            break;
        }
    }
    if (counted == 0) {
        return std::nullopt;
    }
    const double rms = std::sqrt(sum / static_cast<double>(counted));
    return rms > 0.0 ? 20.0 * std::log10(rms) : -std::numeric_limits<double>::infinity();
}

// --- ServiceMediaTranscriber ---------------------------------------------------------------------------------

ServiceMediaTranscriber::ServiceMediaTranscriber(std::shared_ptr<TranscribeService> service, std::filesystem::path projectFile,
                                                 bool allowOnce)
    : m_service(std::move(service)), m_projectFile(std::move(projectFile)), m_allowOnce(allowOnce) {}

std::expected<MediaTranscriber::Result, Error> ServiceMediaTranscriber::transcribe(const MediaItem& media, int audioStream,
                                                                                   const Options& options,
                                                                                   const std::function<bool(double)>& progress) {
    const auto path = mediaPath(media, m_projectFile);
    if (!path) {
        return std::unexpected(Error{ErrorCode::NotFound, "the media file of '" + media.name + "' is missing (relink it)"});
    }
    TranscribeJobOptions job;
    job.provider = options.provider;
    job.language = options.language;
    job.useCache = !options.force;
    job.allowOnce = m_allowOnce;
    std::stop_source stop;
    auto outcome = m_service->run(TranscribeSource{*path, containerStream(media, audioStream), options.redacted}, job,
                                  stop.get_token(),
                                  [&](float p) {
                                      if (progress && !stop.stop_requested() && !progress(p)) {
                                          stop.request_stop();
                                      }
                                  });
    if (!outcome) {
        return std::unexpected(outcome.error());
    }
    return Result{outcome->provider, outcome->fromCache, outcome->transcript.words.size(), outcome->transcript.language};
}

bool ServiceMediaTranscriber::hasTranscript(const MediaItem& media, int audioStream) const {
    const auto path = mediaPath(media, m_projectFile);
    return path && m_service->cached(TranscribeSource{*path, containerStream(media, audioStream)}) != nullptr;
}

std::vector<MediaTranscriber::Provider> ServiceMediaTranscriber::providers() const {
    std::vector<Provider> out;
    for (const TranscribeProviderInfo& p : m_service->providers()) {
        out.push_back(Provider{p.id, p.label, p.available, p.reason});
    }
    return out;
}

std::expected<std::string, Error> ServiceMediaTranscriber::resolve(std::string_view provider) const {
    return m_service->resolveProvider(provider);
}

// --- ServiceEnergyProbe --------------------------------------------------------------------------------------

ServiceEnergyProbe::ServiceEnergyProbe(std::shared_ptr<AudioEnergyService> service, std::filesystem::path projectFile)
    : m_service(std::move(service)), m_projectFile(std::move(projectFile)) {}

std::optional<double> ServiceEnergyProbe::rmsDb(const MediaItem& media, int audioStream, TimeRange source) const {
    if (source.duration <= Ticks{}) {
        return std::nullopt;
    }
    const auto path = mediaPath(media, m_projectFile);
    if (!path) {
        return std::nullopt;
    }
    const int stream = containerStream(media, audioStream);
    if (const auto known = m_service->cached(*path, stream, source)) {
        return known->rmsDb;
    }
    auto measured = m_service->run(EnergyRequest{*path, stream, {source}, false});
    if (!measured || measured->ranges.empty()) {
        return std::nullopt;
    }
    return measured->ranges.front().rmsDb;
}

void ServiceEnergyProbe::prefetch(std::span<const Range> ranges) const {
    std::map<std::pair<std::string, int>, std::vector<TimeRange>> byStream;
    for (const Range& r : ranges) {
        if (r.media == nullptr || r.source.duration <= Ticks{}) {
            continue;
        }
        if (const auto path = mediaPath(*r.media, m_projectFile)) {
            const int stream = containerStream(*r.media, r.audioStream);
            if (!m_service->cached(*path, stream, r.source)) {
                byStream[{*path, stream}].push_back(r.source);
            }
        }
    }
    for (auto& [key, list] : byStream) {
        [[maybe_unused]] const auto measured = m_service->run(EnergyRequest{key.first, key.second, std::move(list), false});
    }
}

} // namespace wabcut::ai::edit

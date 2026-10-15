#include <wabcut/ai/highlights/MediaHighlights.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/media/AudioDecoder.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MediaLocator.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include <algorithm>
#include <cmath>
#include <fstream>

namespace wabcut::ai::highlights {

namespace {

constexpr std::uintmax_t kMaxMarkerFileBytes = 4u << 20;

const std::string kAudioAnalyzer = "highlights.audio";
const std::string kVideoAnalyzer = "highlights.video";
const std::string kMomentsAnalyzer = "highlights";
const std::string kChatAnalyzer = "highlights.chat";

std::string version(std::string_view what) { return std::string(what) + "-" + std::to_string(kVersion); }

Ticks ticksOf(double s) { return Ticks{static_cast<std::int64_t>(std::llround(s * static_cast<double>(kTicksPerSecond)))}; }

} // namespace

std::expected<AudioFeatures, Error> measureAudio(const std::string& path, int containerStream, const analysis::AnalysisProgress& progress) {
    const Rate rate{kAudioRate, 1};
    auto decoder = media::AudioDecoder::open(path, containerStream, rate, media::ChannelLayout::mono());
    if (!decoder) {
        return std::unexpected(decoder.error());
    }
    const Ticks total = (*decoder)->streamInfo().duration;
    AudioFeatureExtractor extractor(kAudioRate);
    std::vector<float> chunk(static_cast<std::size_t>(kAudioRate)); // one second
    std::int64_t position = 0;
    for (int guard = 0; guard < 7 * 24 * 3600; ++guard) {
        auto read = (*decoder)->read(samplesToTicks(position, rate), chunk);
        if (!read) {
            return std::unexpected(read.error());
        }
        const std::size_t used = read->endOfStream ? read->leadingSilence + read->framesFromStream : chunk.size();
        extractor.add(std::span<const float>(chunk.data(), std::min(used, chunk.size())));
        position += static_cast<std::int64_t>(chunk.size());
        if (read->endOfStream || (read->framesFromStream == 0 && read->leadingSilence == 0)) {
            break;
        }
        if (progress && total.value > 0 && guard % 30 == 0 &&
            !progress(std::clamp(static_cast<double>(samplesToTicks(position, rate).value) / static_cast<double>(total.value), 0.0, 1.0))) {
            return std::unexpected(Error{ErrorCode::Cancelled, "measuring the audio was stopped"});
        }
    }
    return extractor.finish();
}

std::expected<VisualFeatures, Error> measureVideo(const std::string& path, int containerStream, int width,
                                                  const analysis::AnalysisProgress& progress) {
    auto source = analysis::VideoFrameSource::open(path, containerStream, width);
    if (!source) {
        return std::unexpected(source.error());
    }
    const std::optional<Ticks> total = (*source)->duration();
    VisualFeatureExtractor extractor;
    for (std::int64_t frames = 0;; ++frames) {
        auto next = (*source)->next();
        if (!next) {
            return std::unexpected(next.error());
        }
        if (!*next) {
            break;
        }
        extractor.add(**next);
        if (progress && total && total->value > 0 && frames % 120 == 0 &&
            !progress(std::clamp(static_cast<double>((*next)->pts.value) / static_cast<double>(total->value), 0.0, 1.0))) {
            return std::unexpected(Error{ErrorCode::Cancelled, "measuring the picture was stopped"});
        }
    }
    return extractor.finish();
}

std::expected<std::vector<Hint>, Error> readChapters(const std::string& path) {
    AVFormatContext* raw = nullptr;
    if (const int r = avformat_open_input(&raw, path.c_str(), nullptr, nullptr); r < 0) {
        return std::unexpected(Error(ErrorCode::IoError, "cannot open the recording to read its chapters"));
    }
    std::vector<Hint> hints;
    for (unsigned i = 0; i < raw->nb_chapters && hints.size() < 10000; ++i) {
        const AVChapter* chapter = raw->chapters[i];
        if (!chapter || chapter->time_base.den == 0) {
            continue;
        }
        const double start = static_cast<double>(chapter->start) * chapter->time_base.num / chapter->time_base.den;
        if (!(start >= 0.0) || !std::isfinite(start)) {
            continue;
        }
        std::string title;
        if (const AVDictionaryEntry* e = av_dict_get(chapter->metadata, "title", nullptr, 0); e && e->value) {
            title = std::string(e->value).substr(0, 200);
        }
        hints.push_back({ticksOf(start), std::move(title), "chapter"});
    }
    avformat_close_input(&raw);
    return hints;
}

std::vector<Hint> findMarkerFiles(const std::filesystem::path& recording, std::vector<std::string>* notes) {
    std::vector<Hint> hints;
    const std::filesystem::path dir = recording.parent_path();
    const std::filesystem::path stem = recording.stem();
    std::vector<std::filesystem::path> candidates;
    for (const char* ext : {".txt", ".csv", ".json"}) {
        candidates.push_back(std::filesystem::path(recording).concat(std::string(".markers") + ext));
        candidates.push_back(dir / std::filesystem::path(stem).concat(std::string(".markers") + ext));
        candidates.push_back(dir / std::filesystem::path(stem).concat(std::string("_markers") + ext));
    }
    for (const std::filesystem::path& file : candidates) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            continue;
        }
        const auto size = std::filesystem::file_size(file, ec);
        if (ec || size > kMaxMarkerFileBytes) {
            if (notes) {
                notes->push_back("marker file " + pathToUtf8(file.filename()) + " skipped (larger than 4 MB or unreadable)");
            }
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        std::string text(static_cast<std::size_t>(size), '\0');
        if (!in.read(text.data(), static_cast<std::streamsize>(size))) {
            continue;
        }
        auto parsed = parseMarkerFile(text);
        if (!parsed) {
            if (notes) {
                notes->push_back("marker file " + pathToUtf8(file.filename()) + ": " + parsed.error().message);
            }
            continue;
        }
        if (notes) {
            notes->push_back(std::to_string(parsed->size()) + " markers from " + pathToUtf8(file.filename()));
        }
        hints.insert(hints.end(), parsed->begin(), parsed->end());
        break; // the first marker file found
    }
    return hints;
}

std::optional<MediaActivity> cachedActivity(const MediaItem& media, const std::filesystem::path& cacheRoot,
                                            const std::filesystem::path& projectFile, int frameWidth) {
    if (cacheRoot.empty() || media.isGenerator()) {
        return std::nullopt;
    }
    const auto path = resolveMediaPath(media.locator, projectFile);
    if (!path) {
        return std::nullopt;
    }
    const auto fingerprint = media::sourceFingerprint(pathToUtf8(*path));
    if (!fingerprint) {
        return std::nullopt;
    }
    const AnalysisCache cache(cacheRoot);
    MediaActivity out;
    out.step = featureTime(1);
    out.source = "analysis";
    bool found = false;
    for (const AudioStreamInfo& stream : media.info.audio) {
        const AnalysisKey key{*fingerprint, stream.index, kAudioAnalyzer, version("highlights-audio"),
                              hashParams({{"rate", std::to_string(kAudioRate)}})};
        auto hit = cache.get(key);
        if (!hit || !*hit) {
            continue;
        }
        auto features = audioFeaturesFromJson(**hit);
        if (!features) {
            continue;
        }
        found = true;
        if (out.audio.size() < features->size()) {
            out.audio.resize(features->size(), 0.0f);
        }
        for (std::size_t i = 0; i < features->size(); ++i) {
            const float level = std::clamp((features->levelDb[i] + 60.0f) / 60.0f, 0.0f, 1.0f);
            out.audio[i] = std::max(out.audio[i], level);
        }
    }
    if (!media.info.video.empty()) {
        const AnalysisKey key{*fingerprint, media.info.video.front().index, kVideoAnalyzer, version("highlights-video"),
                              hashParams({{"width", std::to_string(frameWidth)}})};
        if (auto hit = cache.get(key); hit && *hit) {
            if (auto features = visualFeaturesFromJson(**hit)) {
                found = true;
                out.motion.reserve(features->size());
                for (float m : features->motion) {
                    out.motion.push_back(std::isfinite(m) ? std::clamp(m, 0.0f, 1.0f) : 0.0f);
                }
            }
        }
    }
    if (!found || mediaActivityProblem(out)) {
        return std::nullopt;
    }
    return out;
}

// --- MediaHighlightProvider ------------------------------------------------------------------------------------------

MediaHighlightProvider::MediaHighlightProvider(MediaHighlightOptions options, std::filesystem::path projectFile)
    : m_options(std::move(options)), m_projectFile(std::move(projectFile)) {}

std::expected<std::string, Error> MediaHighlightProvider::pathOf(const MediaItem& media) const {
    if (media.isGenerator()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "'" + media.name + "' is generated: there is no recording to analyse"});
    }
    if (auto path = resolveMediaPath(media.locator, m_projectFile)) {
        return pathToUtf8(*path);
    }
    return std::unexpected(Error{ErrorCode::NotFound, "'" + media.name + "' is offline"});
}

std::expected<Measurement, Error> MediaHighlightProvider::measure(const MediaItem& media, bool video, const analysis::AnalysisProgress& progress) {
    auto path = pathOf(media);
    if (!path) {
        return std::unexpected(path.error());
    }
    std::lock_guard lock(m_mutex);
    const auto fingerprint = media::sourceFingerprint(*path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    std::optional<AnalysisCache> cache;
    if (!m_options.cacheRoot.empty()) {
        cache.emplace(m_options.cacheRoot);
    }
    Measurement out;
    bool allCached = true;
    // Progress: the audio streams share the first part, the picture (much slower) the rest.
    const std::size_t streams = media.info.audio.size();
    const double audioShare = video && !media.info.video.empty() ? 0.15 : 1.0;
    for (std::size_t i = 0; i < streams; ++i) {
        const int container = media.info.audio[i].index;
        const AnalysisKey key{*fingerprint, container, kAudioAnalyzer, version("highlights-audio"), hashParams({{"rate", std::to_string(kAudioRate)}})};
        if (cache && m_options.useCache) {
            if (auto hit = cache->get(key); hit && *hit) {
                if (auto cached = audioFeaturesFromJson(**hit)) {
                    out.audio.push_back(std::move(*cached));
                    continue;
                }
            }
        }
        allCached = false;
        const double base = audioShare * static_cast<double>(i) / static_cast<double>(streams);
        auto measured = measureAudio(*path, container, progress ? analysis::AnalysisProgress([&](double f) {
            return progress(base + f * audioShare / static_cast<double>(streams));
        })
                                                                 : analysis::AnalysisProgress{});
        if (!measured) {
            return std::unexpected(measured.error());
        }
        if (cache) {
            (void)cache->put(key, toJson(*measured));
        }
        out.audio.push_back(std::move(*measured));
    }
    if (video && !media.info.video.empty()) {
        const int container = media.info.video.front().index;
        const AnalysisKey key{*fingerprint, container, kVideoAnalyzer, version("highlights-video"),
                              hashParams({{"width", std::to_string(m_options.frameWidth)}})};
        bool found = false;
        if (cache && m_options.useCache) {
            if (auto hit = cache->get(key); hit && *hit) {
                if (auto cached = visualFeaturesFromJson(**hit)) {
                    out.video = std::move(*cached);
                    found = true;
                }
            }
        }
        if (!found) {
            allCached = false;
            auto measured = measureVideo(*path, container, m_options.frameWidth, progress ? analysis::AnalysisProgress([&](double f) {
                return progress(audioShare + f * (1.0 - audioShare));
            })
                                                                                           : analysis::AnalysisProgress{});
            if (!measured) {
                return std::unexpected(measured.error());
            }
            if (cache) {
                (void)cache->put(key, toJson(*measured));
            }
            out.video = std::move(*measured);
        }
    }
    if (auto chapters = readChapters(*path); chapters && !chapters->empty()) {
        out.notes.push_back(std::to_string(chapters->size()) + (chapters->size() == 1 ? " chapter marker" : " chapter markers"));
        out.hints = std::move(*chapters);
    }
    std::vector<Hint> files = findMarkerFiles(pathFromUtf8(*path), &out.notes);
    out.hints.insert(out.hints.end(), files.begin(), files.end());
    out.fromCache = allCached;
    if (progress) {
        (void)progress(1.0);
    }
    return out;
}

void MediaHighlightProvider::remember(const MediaItem& media, const std::vector<Moment>& moments) {
    if (m_options.cacheRoot.empty()) {
        return;
    }
    auto path = pathOf(media);
    if (!path) {
        return;
    }
    std::lock_guard lock(m_mutex);
    const auto fingerprint = media::sourceFingerprint(*path);
    if (!fingerprint) {
        return;
    }
    AnalysisCache cache(m_options.cacheRoot);
    (void)cache.put(AnalysisKey{*fingerprint, -1, kMomentsAnalyzer, version("highlights"), hashParams({})}, toJson(moments));
}

std::expected<AnalysisKey, Error> MediaHighlightProvider::chatKey(const MediaItem& media) const {
    auto path = pathOf(media);
    if (!path) {
        return std::unexpected(path.error());
    }
    const auto fingerprint = media::sourceFingerprint(*path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    return AnalysisKey{*fingerprint, -1, kChatAnalyzer, version("highlights-chat"), hashParams({})};
}

std::expected<void, Error> MediaHighlightProvider::keepChat(const MediaItem& media, const ChatLog& chat, double offsetSeconds) {
    if (m_options.cacheRoot.empty()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "there is no analysis cache to keep the chat log in"});
    }
    std::lock_guard lock(m_mutex);
    auto key = chatKey(media);
    if (!key) {
        return std::unexpected(key.error());
    }
    AnalysisCache cache(m_options.cacheRoot);
    return cache.put(*key, toJson(chat, offsetSeconds));
}

std::optional<HighlightProvider::KeptChat> MediaHighlightProvider::chatFor(const MediaItem& media) {
    if (m_options.cacheRoot.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(m_mutex);
    auto key = chatKey(media);
    if (!key) {
        return std::nullopt;
    }
    const AnalysisCache cache(m_options.cacheRoot);
    auto hit = cache.get(*key);
    if (!hit || !*hit) {
        return std::nullopt;
    }
    auto kept = chatFromJson(**hit);
    if (!kept) {
        return std::nullopt;
    }
    return KeptChat{std::move(kept->first), kept->second};
}

} // namespace wabcut::ai::highlights

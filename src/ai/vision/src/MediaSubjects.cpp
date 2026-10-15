#include <wabcut/ai/vision/MediaSubjects.h>

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/ai/analysis/MediaAnalysis.h>
#include <wabcut/ai/search/MediaSearchAccess.h>
#include <wabcut/media/Fingerprint.h>
#include <wabcut/model/MediaLocator.h>

#include <chrono>
#include <cstdio>

namespace wabcut::ai::vision {

namespace {

/// The decoded pictures, in increasing time order.
class ReaderSource : public TimedFrameSource {
public:
    explicit ReaderSource(search::VideoPictureReader& reader) : m_reader(reader) {}
    std::expected<Image, Error> frameAt(Ticks time) override { return m_reader.at(time); }

private:
    search::VideoPictureReader& m_reader;
};

std::string number(double value) {
    char text[32];
    std::snprintf(text, sizeof text, "%.4g", value);
    return text;
}

std::string optionsHash(const MediaSubjectOptions& o) {
    const SubjectOptions& a = o.analysis;
    const auto tracker = [](const TrackerOptions& t) {
        return number(t.highScore) + "/" + number(t.lowScore) + "/" + number(t.matchIou) + "/" + number(t.lowMatchIou) + "/" +
               std::to_string(t.maxLost) + "/" + std::to_string(t.minHits);
    };
    return hashParams({{"width", std::to_string(o.width)},
                       {"fps", number(a.baseFps) + "-" + number(a.maxFps)},
                       {"motion", number(a.motionThreshold)},
                       {"maxSamples", std::to_string(a.maxSamples)},
                       {"faces", tracker(a.faceTracker)},
                       {"people", tracker(a.personTracker)}});
}

AnalysisKey keyFor(std::uint64_t fingerprint, int containerStream, const SubjectDetector* detector, const MediaSubjectOptions& options) {
    const std::string version = detector ? detector->modelVersion() : std::string("saliency");
    return AnalysisKey{fingerprint, containerStream, std::string(SubjectAnalysis::kAnalyzer),
                       version + "-s" + std::to_string(SubjectAnalysis::kVersion), optionsHash(options)};
}

} // namespace

std::optional<SubjectAnalysis> cachedFileSubjects(const std::string& path, int containerStream, const SubjectDetector* detector,
                                                  const MediaSubjectOptions& options) {
    if (options.cacheRoot.empty()) {
        return std::nullopt;
    }
    const auto fingerprint = media::sourceFingerprint(path);
    if (!fingerprint) {
        return std::nullopt;
    }
    const AnalysisCache cache(options.cacheRoot);
    if (auto hit = cache.get(keyFor(*fingerprint, containerStream, detector, options)); hit && *hit) {
        return subjectsFromJson(**hit);
    }
    return std::nullopt;
}

std::expected<SubjectAnalysis, Error> analyzeFileSubjects(const std::string& path, int containerStream, SubjectDetector* detector,
                                                          const MediaSubjectOptions& options, const SubjectProgress& progress,
                                                          SubjectStats* stats, std::stop_token stop) {
    SubjectStats local;
    SubjectStats& s = stats ? *stats : local;
    s = SubjectStats{};
    const auto fingerprint = media::sourceFingerprint(path);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto reader = search::VideoPictureReader::open(path, containerStream, options.width);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    s.backend = (*reader)->backend();
    containerStream = (*reader)->streamIndex(); // -1 resolved: the same cache entry whoever asks

    // The shots first (cached by C.5's analyser): tracks reset at every cut. They take the first 30% of progress
    // when they are not cached yet.
    analysis::MediaAnalyzerOptions analyzerOptions;
    analyzerOptions.cacheRoot = options.cacheRoot;
    analyzerOptions.useCache = !options.cacheRoot.empty();
    analysis::MediaAnalyzer analyzer(analyzerOptions);
    analysis::MediaAnalyzer::Stats shotStats;
    auto shots = analyzer.shots(path, containerStream, analysis::ShotOptions{},
                                [&](double fraction) { return !stop.stop_requested() && (!progress || progress(fraction * 0.3)); },
                                &shotStats);
    if (!shots) {
        return std::unexpected(shots.error());
    }
    const double base = shotStats.fromCache ? 0.0 : 0.3;

    const AnalysisKey key = keyFor(*fingerprint, containerStream, detector, options);
    std::optional<AnalysisCache> cache;
    if (!options.cacheRoot.empty()) {
        cache.emplace(options.cacheRoot);
        if (auto hit = cache->get(key); hit && *hit) {
            if (auto cached = subjectsFromJson(**hit)) {
                s.fromCache = true;
                s.samples = cached->samples.size();
                s.analysedSeconds = (cached->end - cached->start).toSeconds();
                return std::move(*cached);
            }
        }
    }

    ReaderSource source(**reader);
    const auto started = std::chrono::steady_clock::now();
    auto result = analyzeSubjects(
        source, detector, *shots, options.analysis,
        [&](double fraction) { return !progress || progress(base + (1.0 - base) * fraction); }, stop);
    s.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (!result) {
        return result;
    }
    s.samples = result->samples.size();
    s.analysedSeconds = (result->end - result->start).toSeconds();
    if (const auto* helper = dynamic_cast<HelperDetector*>(detector)) {
        s.device = helper->device();
    }
    if (cache) {
        (void)cache->put(key, subjectsToJson(*result));
    }
    return result;
}

MediaSubjectProvider::MediaSubjectProvider(MediaSubjectOptions options, std::shared_ptr<SubjectDetector> detector)
    : m_options(std::move(options)), m_detector(std::move(detector)) {}

void MediaSubjectProvider::setProjectFile(std::filesystem::path projectFile) {
    std::lock_guard lock(m_mutex);
    m_options.projectFile = std::move(projectFile);
}

std::expected<std::string, Error> MediaSubjectProvider::pathOf(const MediaItem& media) const {
    if (media.isGenerator()) {
        return std::unexpected(Error{ErrorCode::Unsupported, "'" + media.name + "' is generated: it has no file to analyse"});
    }
    std::filesystem::path projectFile;
    {
        std::lock_guard lock(m_mutex);
        projectFile = m_options.projectFile;
    }
    if (auto path = resolveMediaPath(media.locator, projectFile)) {
        return pathToUtf8(*path);
    }
    return std::unexpected(Error{ErrorCode::NotFound, "'" + media.name + "' is offline"});
}

std::expected<SubjectAnalysis, Error> MediaSubjectProvider::subjects(const MediaItem& media, int videoStream,
                                                                     const std::function<bool(double)>& progress) {
    if (media.info.video.empty()) {
        return std::unexpected(Error{ErrorCode::InvalidArgument, "'" + media.name + "' has no video"});
    }
    auto path = pathOf(media);
    if (!path) {
        return std::unexpected(path.error());
    }
    const int container = videoStream >= 0 && static_cast<std::size_t>(videoStream) < media.info.video.size()
                              ? media.info.video[static_cast<std::size_t>(videoStream)].index
                              : media.info.video.front().index;
    MediaSubjectOptions options;
    {
        std::lock_guard lock(m_mutex);
        options = m_options;
    }
    SubjectStats stats;
    auto result = analyzeFileSubjects(*path, container, m_detector.get(), options, progress, &stats);
    std::lock_guard lock(m_mutex);
    m_stats = stats;
    return result;
}

std::optional<SubjectAnalysis> MediaSubjectProvider::cached(const MediaItem& media, int videoStream) {
    if (media.info.video.empty()) {
        return std::nullopt;
    }
    auto path = pathOf(media);
    if (!path) {
        return std::nullopt;
    }
    const int container = videoStream >= 0 && static_cast<std::size_t>(videoStream) < media.info.video.size()
                              ? media.info.video[static_cast<std::size_t>(videoStream)].index
                              : media.info.video.front().index;
    MediaSubjectOptions options;
    {
        std::lock_guard lock(m_mutex);
        options = m_options;
    }
    return cachedFileSubjects(*path, container, m_detector.get(), options);
}

SubjectStats MediaSubjectProvider::lastStats() const {
    std::lock_guard lock(m_mutex);
    return m_stats;
}

SubjectDetectorChoice openSubjectDetector(const std::filesystem::path& modelRoot, const std::filesystem::path& helper, int threads) {
    SubjectDetectorChoice choice;
    const std::filesystem::path executable = helper.empty() ? search::defaultEmbedExecutable() : helper;
    std::error_code ec;
    if (!std::filesystem::exists(executable, ec)) {
        choice.unavailableReason = "this build has no detection helper (wabcut-embed is built with WABCUT_WITH_ONNX)";
        return choice;
    }
    const ModelStore store(modelRoot.empty() ? ModelStore::defaultRoot() : modelRoot);
    if (!subjectModelsInstalled(store)) {
        choice.unavailableReason = "the face and person detectors are not downloaded";
        return choice;
    }
    HelperDetectorOptions options = subjectDetectorOptions(store);
    options.executable = executable;
    options.threads = threads;
    choice.detector = std::make_shared<HelperDetector>(std::move(options));
    return choice;
}

} // namespace wabcut::ai::vision

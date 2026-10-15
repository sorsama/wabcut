#pragma once

// Subject analysis for auto reframe and the shorts finder (AI-9, docs/ai.md "Auto reframe"): where the faces, people
// and salient things of a video are, over time.
//
// analyzeSubjects() samples each shot (C.5 cuts) at `baseFps`, denser (`maxFps`) right after the picture moved, sends
// the frames to a SubjectDetector in batches (YuNet faces and YOLOX people in wabcut-embed), tracks faces and people
// per shot (ByteTracker, reset at every cut), measures each face's activity (motion in its lower half: a talking
// mouth) and, for every sample, the most salient region (spectral residual + motion), the fallback when nothing is
// detected. The result is plain data, cached in the AnalysisCache by source fingerprint and detector version.

#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/ai/vision/Detection.h>
#include <wabcut/ai/vision/Tracker.h>
#include <wabcut/model/Error.h>
#include <wabcut/model/ProjectState.h>
#include <wabcut/model/Time.h>

#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::vision {

struct SubjectBox {
    Box box;               ///< normalised 0..1 in the picture
    float score = 0.0f;
    int track = -1;        ///< track id (unique within one analysis); -1 = untracked
    float activity = 0.0f; ///< faces: motion in the lower half since the previous sample (0..1), a speaking cue

    bool operator==(const SubjectBox&) const = default;
};

struct SubjectSample {
    Ticks time; ///< source time
    std::vector<SubjectBox> faces;
    std::vector<SubjectBox> people;
    std::optional<Box> salient; ///< the most salient region (normalised)
    float motion = 0.0f;        ///< mean picture change since the previous sample of the shot (0..1)

    bool operator==(const SubjectSample&) const = default;
};

struct SubjectAnalysis {
    static constexpr std::string_view kAnalyzer = "subjects";
    static constexpr int kVersion = 1;

    int frameWidth = 0;  ///< display size of the analysed video (its aspect)
    int frameHeight = 0;
    Ticks start;
    Ticks end;
    std::vector<Ticks> cuts;            ///< shot cuts inside [start, end): tracks reset there
    std::vector<SubjectSample> samples; ///< in time order
    std::string models;                 ///< the detector's version ("" = saliency only)

    /// Index of the shot that contains `time` (0 = before the first cut).
    std::size_t shotOf(Ticks time) const;
    /// [start, end) of shot `index`.
    TimeRange shot(std::size_t index) const;
    std::size_t shotCount() const { return cuts.size() + 1; }

    bool operator==(const SubjectAnalysis&) const = default;
};

/// How many face / person tracks appear in at least `minSamples` samples (a detection that flickers for one sample
/// starts a track that never lasts: it is not a subject).
std::pair<std::size_t, std::size_t> countTracks(const SubjectAnalysis& analysis, std::size_t minSamples = 2);

/// The cache payload (compact JSON, numbers rounded to 1e-4), and back. Nullopt for anything malformed.
std::string subjectsToJson(const SubjectAnalysis& analysis);
std::optional<SubjectAnalysis> subjectsFromJson(std::string_view json);

/// What a detector found in one picture, in its pixels.
struct FrameDetections {
    std::vector<Detection> faces;
    std::vector<Detection> people;
    /// Every COCO class (people too, classId = the class), when asked for (HelperDetectorOptions::allClasses: AI-14's
    /// object prompts); else empty.
    std::vector<Detection> objects;
};

/// Faces and people in pictures (wabcut-embed's YuNet + YOLOX, or a fixture).
class SubjectDetector {
public:
    virtual ~SubjectDetector() = default;
    /// One result per picture, in order. Errors: the engine's, Cancelled.
    virtual std::expected<std::vector<FrameDetections>, Error> detect(std::span<const Image> frames, std::stop_token stop) = 0;
    /// Identity of the models (cache keys).
    virtual std::string modelVersion() const = 0;
};

/// The pictures of a video at chosen times, asked for in increasing order.
class TimedFrameSource {
public:
    virtual ~TimedFrameSource() = default;
    virtual std::expected<Image, Error> frameAt(Ticks time) = 0;
};

struct SubjectOptions {
    double baseFps = 5.0;
    double maxFps = 10.0;           ///< after a sample whose motion reaches motionThreshold
    float motionThreshold = 0.035f;
    std::size_t batch = 16;         ///< pictures per detector call (bounds memory)
    std::size_t maxSamples = 50'000;
    TrackerOptions faceTracker;
    TrackerOptions personTracker;
};

/// Progress 0..1; returning false cancels.
using SubjectProgress = std::function<bool(double)>;

/// Analyses [shots.start, shots.end) of `source`. `detector` may be null (saliency only). Errors: the source's and
/// the detector's, Cancelled.
std::expected<SubjectAnalysis, Error> analyzeSubjects(TimedFrameSource& source, SubjectDetector* detector,
                                                      const analysis::ShotAnalysis& shots, const SubjectOptions& options = {},
                                                      const SubjectProgress& progress = {}, std::stop_token stop = {});

/// Subjects of media items (decoding them, or reading the cache): the commands' services.
class SubjectProvider {
public:
    virtual ~SubjectProvider() = default;
    /// `videoStream` indexes MediaInfo::video.
    virtual std::expected<SubjectAnalysis, Error> subjects(const MediaItem& media, int videoStream,
                                                           const std::function<bool(double)>& progress) = 0;
    /// The analysis when it is already at hand (cached), without analysing; nullopt otherwise.
    virtual std::optional<SubjectAnalysis> cached(const MediaItem& media, int videoStream) {
        (void)media;
        (void)videoStream;
        return std::nullopt;
    }
};

/// Analyses handed over in memory (tests, fixtures), by media id.
class MapSubjectProvider : public SubjectProvider {
public:
    void add(MediaId media, SubjectAnalysis analysis) { m_analyses[media.value] = std::move(analysis); }
    std::expected<SubjectAnalysis, Error> subjects(const MediaItem& media, int videoStream,
                                                   const std::function<bool(double)>& progress) override;
    std::optional<SubjectAnalysis> cached(const MediaItem& media, int videoStream) override;

private:
    std::map<std::uint64_t, SubjectAnalysis> m_analyses;
};

} // namespace wabcut::ai::vision

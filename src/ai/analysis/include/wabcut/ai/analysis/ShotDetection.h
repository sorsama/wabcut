#pragma once

// Shot (scene-cut) detection on small frames, after the detectors of PySceneDetect (BSD-3-Clause; the algorithms are
// reimplemented here from their documentation, no code is taken):
//
//   ContentDetector    the mean absolute difference of hue, saturation and value between consecutive frames
//                      (OpenCV's 8-bit HSV: H 0..179, S and V 0..255); a cut where it reaches `contentThreshold`
//                      (27 by default).
//   AdaptiveDetector   the same content value divided by the mean of its neighbours (`adaptiveWindow` frames each
//                      side); a cut where that ratio reaches `adaptiveThreshold` (3) and the value itself reaches
//                      `minContentValue` (15): robust against fast motion, which raises every frame's value.
//   ThresholdDetector  the mean pixel intensity; a fade out ends below `fadeThreshold` (12) and the next fade in
//                      starts above it; the cut goes between them (`fadeBias` 0 = the middle).
//
// Wabcut adds what editors expect from scene-edit detection:
//   * flashes (a strobe, a camera flash, a white frame) are not cuts: a change that returns to the previous picture
//     within `flashFrames` frames is ignored, at both of its edges;
//   * dissolves: over a window of `dissolveFrames`, a middle frame that is the average of the window's end frames,
//     which differ by `dissolveThreshold` (mean RGB difference), is a blend; the cut goes where the blend is half
//     way. Pans and zooms are not blends, so they do not count;
//   * cuts closer than `minShotFrames` keep the strongest (hard cut > fade > dissolve).
//
// Cuts are frame-exact: a cut's time is the start of the first frame of the new shot. Everything is deterministic.

#include <wabcut/model/Error.h>
#include <wabcut/model/Time.h>

#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wabcut::ai::analysis {

/// Reports progress (0..1); returning false stops the analysis (it then fails with ErrorCode::Cancelled).
using AnalysisProgress = std::function<bool(double fraction)>;

/// A small picture of one frame: packed 8-bit RGB, row by row.
struct AnalysisFrame {
    Ticks pts;      ///< start of the frame, source time
    Ticks duration;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgb; ///< width x height x 3
};

/// The frames of a video in order (decoded and made small).
class FrameSource {
public:
    virtual ~FrameSource() = default;
    /// The next frame, nullopt at the end. Errors stop the analysis.
    virtual std::expected<std::optional<AnalysisFrame>, Error> next() = 0;
    /// The source's length, for progress; nullopt when unknown.
    virtual std::optional<Ticks> duration() const { return std::nullopt; }
};

enum class ShotDetector : std::uint8_t {
    Combined, ///< hard cuts (content + adaptive), fades and dissolves, flashes ignored (the default)
    Content,  ///< ContentDetector only (flash filter applies)
    Adaptive, ///< AdaptiveDetector only (flash filter applies)
    Threshold ///< ThresholdDetector only (fades through black)
};
std::string_view toString(ShotDetector detector);
std::optional<ShotDetector> shotDetectorFromString(std::string_view name);

struct ShotOptions {
    ShotDetector detector = ShotDetector::Combined;
    double contentThreshold = 27.0;
    double adaptiveThreshold = 3.0;
    int adaptiveWindow = 2;
    double minContentValue = 15.0;
    double fadeThreshold = 12.0;
    double fadeBias = 0.0; ///< -1 = at the end of the fade out, 0 = middle, +1 = at the start of the fade in
    int flashFrames = 4;
    int dissolveFrames = 12;
    double dissolveThreshold = 25.0;
    int minShotFrames = 15;

    /// Every setting that changes the result, for cache keys.
    std::string fingerprint() const;
};

enum class CutKind : std::uint8_t { Cut, Fade, Dissolve };
std::string_view toString(CutKind kind);
std::optional<CutKind> cutKindFromString(std::string_view name);

struct ShotCut {
    Ticks time;               ///< start of the first frame of the new shot (source time)
    std::int64_t frame = 0;   ///< that frame's index in the stream
    CutKind kind = CutKind::Cut;
    float confidence = 0.0f;  ///< 0..1
    bool operator==(const ShotCut&) const = default;
};

struct ShotAnalysis {
    static constexpr std::string_view kAnalyzer = "wabcut-shots";
    static constexpr int kVersion = 1;
    std::vector<ShotCut> cuts;
    std::int64_t frames = 0;   ///< frames analysed
    Ticks start;               ///< first frame's start
    Ticks end;                 ///< last frame's end
    std::string detector = std::string(kAnalyzer) + "-" + std::to_string(kVersion);
    bool operator==(const ShotAnalysis&) const = default;
};

/// Runs the detectors over every frame of `source`. Errors: the source's errors, Cancelled.
std::expected<ShotAnalysis, Error> detectShots(FrameSource& source, const ShotOptions& options = {},
                                               const AnalysisProgress& progress = {});

/// Frames handed over in memory (tests, generated pictures).
class VectorFrameSource : public FrameSource {
public:
    explicit VectorFrameSource(std::vector<AnalysisFrame> frames) : m_frames(std::move(frames)) {}
    std::expected<std::optional<AnalysisFrame>, Error> next() override;

private:
    std::vector<AnalysisFrame> m_frames;
    std::size_t m_next = 0;
};

} // namespace wabcut::ai::analysis

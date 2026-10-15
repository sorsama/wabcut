#pragma once

// The media-analysis commands of the catalog (C.5):
//
//   analysis.detectShots   cuts of a clip's (or media item's) video, as markers in the clip
//   clip.splitAtShots      razor clips at the cuts in their video (one undo step; dryRun previews the split)
//   analysis.detectBeats   beats of a clip's (or media item's) music: markers labelled 1-2-3-4 (downbeats red) and
//                          the sequence beat grid (Sequence::beatGrid), which snapping, edit.snapToBeats and
//                          edit.cutToBeats use
//
// They need the analysis of the project's media, which the host provides as AnalysisServices (alone or in a
// command::ServiceBundle; wabcut-cli's comes from wabcut_ai_analysis_media, cached per source fingerprint);
// without it they fail with InvalidRequest.

#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/ai/analysis/ShotDetection.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/ProjectState.h>

#include <map>
#include <memory>

namespace wabcut::ai::analysis {

/// Analyses media items (decoding them, or reading a cache).
class AnalysisProvider {
public:
    virtual ~AnalysisProvider() = default;
    /// `videoStream` / `audioStream` index MediaInfo::video / ::audio (like StreamRef::index).
    virtual std::expected<ShotAnalysis, Error> shots(const MediaItem& media, int videoStream, const ShotOptions& options,
                                                     const AnalysisProgress& progress) = 0;
    virtual std::expected<BeatAnalysis, Error> beats(const MediaItem& media, int audioStream, const BeatOptions& options,
                                                     const AnalysisProgress& progress) = 0;
};

/// Results handed over in memory (tests, an app that already has them), by media id.
class MapAnalysisProvider : public AnalysisProvider {
public:
    void addShots(MediaId media, ShotAnalysis shots) { m_shots[media.value] = std::move(shots); }
    void addBeats(MediaId media, BeatAnalysis beats) { m_beats[media.value] = std::move(beats); }
    std::expected<ShotAnalysis, Error> shots(const MediaItem& media, int, const ShotOptions&, const AnalysisProgress&) override;
    std::expected<BeatAnalysis, Error> beats(const MediaItem& media, int, const BeatOptions&, const AnalysisProgress&) override;

private:
    std::map<std::uint64_t, ShotAnalysis> m_shots;
    std::map<std::uint64_t, BeatAnalysis> m_beats;
};

class AnalysisServices : public command::CommandServices {
public:
    explicit AnalysisServices(std::shared_ptr<AnalysisProvider> provider) : provider(std::move(provider)) {}
    std::shared_ptr<AnalysisProvider> provider;
};

/// Adds analysis.detectShots, clip.splitAtShots and analysis.detectBeats.
void registerAnalysisCommands(command::CommandRegistry& registry);

/// Where a clip shows source time `source` on the timeline (speed and direction applied; snapped to the nearest
/// frame of `snapTo` when given), or nullopt when the clip does not show it (trimmed away, a frame hold).
std::optional<Ticks> timelineTimeOfSource(const Clip& clip, Ticks source, std::optional<Rate> snapTo = std::nullopt);

} // namespace wabcut::ai::analysis

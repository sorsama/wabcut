#pragma once

// The stabilization analysis commands (C.15 parked, finished in P.2; docs/motion.md "Warp Stabilizer"):
//
//   effect.analyzeStabilization  Warp Stabilizer's Analyze. Tracks a clip's media (Stabilize.h), smooths the camera path
//                                with the effect's settings and keys the compensating transform into the clip's
//                                "wabcut.stabilize.warp" instance (added first in the stack when the clip has none).
//   effect.analyzeRollingShutter Rolling Shutter Repair's analysis: keys the picture's motion per frame (motion.x,
//                                motion.y) into the clip's "wabcut.stabilize.rollingShutter" instance.
//   effect.analyzeAll            0.7 (card #128): every Warp Stabilizer and Rolling Shutter Repair of the sequence (and
//                                the sequences nested in it) that is not analysed or out of date, in one job and one undo
//                                step. Morph Cut is not one of them: it analyses itself in the background.
//
// 0.7 (card #107): a time-remapped clip is analysed too: the frames its remap shows are tracked and the keys go at the
// curve time of each frame the clip shows on the timeline (speed ramps, holds and reverse segments), the compensation
// interpolated between the analysed frames; a clip at a constant speed (reversed or not) keys at the media times as
// before, which are its curve times.
//
// The raw motion is cached per media fingerprint, so changing Smoothness or Framing and running it again re-smooths
// without tracking (as Warp Stabilizer does). Both are jobs (progress through ExecutionContext::checkpoint, cancel) and
// one undo step each.
//
// Services (findService): masks::TrackingServices gives the frames (the editor's MediaTrackingService in the app; a fake
// in tests); StabilizationServices, when present, the analysis cache. Without frames the commands are refused.

#include <wabcut/ai/AnalysisCache.h>
#include <wabcut/command/ExecutionContext.h>
#include <wabcut/command/Registry.h>
#include <wabcut/model/Ids.h>
#include <wabcut/model/ProjectState.h>

#include <memory>
#include <string>
#include <vector>

namespace wabcut::ai::stabilize {

class StabilizationServices : public command::CommandServices {
public:
    explicit StabilizationServices(std::shared_ptr<AnalysisCache> cache) : m_cache(std::move(cache)) {}
    AnalysisCache* cache() const { return m_cache.get(); }

private:
    std::shared_ptr<AnalysisCache> m_cache;
};

/// Width the frames are analysed at (pixels; the tracker's frames are downsized to it).
inline constexpr int kAnalysisWidth = 640;

/// 0.7 (cards #107, #128): where one Warp Stabilizer or Rolling Shutter Repair stands.
enum class StabilizerNeed {
    Current,     ///< analysed, and the analysis still matches the clip's frames and the effect's settings
    NotAnalyzed, ///< never analysed (or the analysis failed)
    OutOfDate,   ///< analysed, but the clip shows other frames now or a setting changed
    Skipped,     ///< cannot be analysed (switched off, a freeze frame, no camera footage ...): `reason` says why
};

struct StabilizerEntry {
    SequenceId sequence; ///< the sequence its clip is in (a nested sequence's own, for a clip inside a nest)
    ClipId clip;
    EffectInstanceId effect;
    bool warp = true;    ///< Warp Stabilizer; else Rolling Shutter Repair
    StabilizerNeed need = StabilizerNeed::Current;
    std::string reason;  ///< for Skipped
    std::size_t frames = 0; ///< the frames an analysis goes through
    TimeRange where;     ///< the clip's range on its sequence's timeline
    int depth = 0;       ///< 0: the sequence asked about; 1 and up: nested in it
    bool master = false; ///< a Master Clip Effect of the clip's media (met on the first clip that plays it)
};

/// Every Warp Stabilizer and Rolling Shutter Repair of `sequence` - and of the sequences nested in it when `nested` - with
/// whether its analysis is current: the clips' own effects and their media's Master Clip Effects (each once). The command
/// effect.analyzeAll runs the ones that are not Current; the Effect Controls banner and the export warning read the rest.
std::vector<StabilizerEntry> surveyStabilizers(const ProjectState& state, SequenceId sequence, bool nested = true);

/// Adds effect.analyzeStabilization, effect.analyzeRollingShutter and effect.analyzeAll.
void registerStabilizeCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::stabilize

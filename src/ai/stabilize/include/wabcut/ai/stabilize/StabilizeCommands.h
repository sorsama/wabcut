#pragma once

// The stabilization analysis commands (C.15 parked, finished in P.2; docs/motion.md "Warp Stabilizer"):
//
//   effect.analyzeStabilization  Warp Stabilizer's Analyze. Tracks a clip's media (Stabilize.h), smooths the camera path
//                                with the effect's settings and keys the compensating transform into the clip's
//                                "wabcut.stabilize.warp" instance (added first in the stack when the clip has none).
//   effect.analyzeRollingShutter Rolling Shutter Repair's analysis: keys the picture's motion per frame (motion.x,
//                                motion.y) into the clip's "wabcut.stabilize.rollingShutter" instance.
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

#include <memory>

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

/// Adds effect.analyzeStabilization and effect.analyzeRollingShutter.
void registerStabilizeCommands(command::CommandRegistry& registry);

} // namespace wabcut::ai::stabilize

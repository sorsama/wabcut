#pragma once

// Media activity for dead-time removal (C.16, sequence.removeDeadTime; model/GamingOps.h MediaActivity): a cheap pass
// over a media item. Motion is the mean absolute difference of the luma of consecutive samples, each frame shrunk to a
// small grid (the proxy is decoded when there is one: ProRes Proxy is intra-only and small). Loudness is the peak level of
// the audio's waveform bins in dB, mapped from -60 dBFS..0 dBFS to 0..1. Both are sampled `samplesPerSecond` times a
// second from the media's origin.
//
// When the AI engineer's signal curves exist (an analysis cache with an "activity" entry for the media), the host's
// lookup passes them first and nothing is decoded.

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/model/GamingOps.h>
#include <wabcut/model/ProjectState.h>

#include <expected>
#include <filesystem>
#include <functional>
#include <optional>

namespace wabcut::editor {

struct ActivityOptions {
    int samplesPerSecond = 5; ///< 1..30
    int gridWidth = 64;       ///< luma grid the frames shrink to (its height follows the aspect), 8..256
    bool preferProxy = true;  ///< decode the proxy when the media has one
};

/// Measures `item` (see the file comment). `progress` gets 0..1 and returns false to stop (ErrorCode::Cancelled).
/// Errors: NotFound (offline, no file), the decoders' errors. Media without video measures audio only and the other
/// way round.
std::expected<MediaActivity, Error> measureMediaActivity(const MediaItem& item, const std::filesystem::path& projectFile,
                                                         const std::function<bool(double)>& progress = {},
                                                         const ActivityOptions& options = {});

/// The command layer's activity measure (command::ActivityServices) over measureMediaActivity: results are kept in
/// memory per media file (path and fingerprint), so a preview and its apply measure once. `fromAnalysis`, when given,
/// is asked first (the AI engineer's signal curves from the analysis cache).
command::ActivityServices::Measure activityMeasure(
    std::filesystem::path projectFile, std::function<std::optional<MediaActivity>(const MediaItem&)> fromAnalysis = {},
    ActivityOptions options = {});

} // namespace wabcut::editor

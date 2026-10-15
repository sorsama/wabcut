#pragma once

// Versioned JSON of analysis results: the payload of their AnalysisCache entries (wabcut/ai/AnalysisCache.h) and
// what `wabcut-cli analyze --json` prints. Times are integer ticks.
//
//   {"schema": "wabcut.shots", "version": 1, "detector": "wabcut-shots-1", "frames": 250, "start": 0, "end": ...,
//    "cuts": [{"t": ticks, "frame": 50, "kind": "cut", "confidence": 0.98}, ...]}
//   {"schema": "wabcut.beats", "version": 1, "tracker": "dsp-1", "bpm": 120.0, "confidence": 0.93,
//    "beatsPerBar": 4, "duration": ticks, "beats": [ticks, ...], "downbeats": [ticks, ...]}

#include <wabcut/ai/analysis/BeatDetection.h>
#include <wabcut/ai/analysis/ShotDetection.h>

#include <expected>
#include <string>
#include <string_view>

namespace wabcut::ai::analysis {

/// `indent` < 0: one line.
std::string toJson(const ShotAnalysis& shots, int indent = -1);
std::string toJson(const BeatAnalysis& beats, int indent = -1);

/// Errors: ParseError (not JSON, another schema or version, wrong types).
std::expected<ShotAnalysis, Error> shotsFromJson(std::string_view json);
std::expected<BeatAnalysis, Error> beatsFromJson(std::string_view json);

} // namespace wabcut::ai::analysis

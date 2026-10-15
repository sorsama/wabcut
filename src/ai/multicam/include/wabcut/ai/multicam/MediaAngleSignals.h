#pragma once

// The media-backed AngleSignals (AI-13): microphone levels decoded with wabcut_media (16 kHz mono, RMS per frame,
// remembered per media file and range for the session) and faces from the AI-9 subject provider (cached on disk by
// it). Full builds only (wabcut_ai_multicam_media).

#include <wabcut/ai/multicam/Evidence.h>

#include <filesystem>
#include <memory>

namespace wabcut::ai::multicam {

/// `faces` may be null (levels only); `projectFile` resolves project-relative media paths.
std::shared_ptr<AngleSignals> makeMediaAngleSignals(std::shared_ptr<vision::SubjectProvider> faces, std::filesystem::path projectFile);

/// RMS level (dBFS) of every `frame` of mono samples at `rate`; -100 for silence.
std::vector<float> rmsLevels(std::span<const float> samples, int rate, Ticks frame);

} // namespace wabcut::ai::multicam

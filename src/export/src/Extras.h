#pragma once

// Private: checks of the 0.2 export extras (ExportEffects.h) that validate() runs.

#include <wabcut/export/ExportSettings.h>

#include <expected>

namespace wabcut::exporter {

/// Effects, metadata, the subtitle track and alpha: value ranges, string bounds, and what the format can carry.
/// Errors: InvalidArgument naming the problem.
std::expected<void, Error> validateExtras(const ExportSettings& settings);

/// Strings of presets and requests are untrusted: the bound on a path (image overlay, LUT).
inline constexpr std::size_t kMaxExportPathBytes = 4096;

} // namespace wabcut::exporter

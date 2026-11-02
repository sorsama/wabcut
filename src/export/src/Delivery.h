#pragma once

// Private (0.6, #73): the settings rules of broadcast delivery - MXF OP1a / OP-Atom, the XDCAM HD422 and AVC-Intra 100
// codecs, and embedded CEA-608 / 708 captions - kept out of ExportSettings.cpp, which calls them from validate() and
// resolve().

#include <wabcut/export/ExportSettings.h>
#include <wabcut/model/Error.h>

#include <expected>
#include <optional>
#include <string>

namespace wabcut::exporter::detail {

/// Container / codec / audio / caption rules of MXF and the broadcast codecs that need no sequence (validate()).
std::expected<void, Error> validateDelivery(const ExportSettings& settings);

/// What the resolved export breaks of those rules once the frame size and rate are known (resolve()): MXF frame rates,
/// XDCAM HD422 and AVC-Intra 100 rasters, the 608 / 708 data rate. nullopt = fine.
std::optional<std::string> deliveryProblem(const ExportSettings& settings, const ResolvedExport& resolved);

/// The XDCAM HD422 GOP length: 12 at 24 / 25 / 50 Hz, 15 at 30 / 60 Hz (M = 3).
int xdcamGopLength(Rate rate);

} // namespace wabcut::exporter::detail

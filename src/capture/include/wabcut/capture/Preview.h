#pragma once

// The live picture of the Capture panel (0.7, docs/capture.md): a captured picture converted for display. Runs on whatever
// thread asks (the panel's image provider), never on the GUI thread or the sources' threads.

#include <wabcut/capture/CaptureTypes.h>

#include <wabcut/model/Error.h>

#include <cstdint>
#include <expected>
#include <vector>

namespace wabcut::capture {

struct PreviewImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba; ///< tightly packed RGBA, 8 bits, opaque
};

/// `frame` as an RGBA picture no wider than `maxWidth` (never enlarged), in the shape it is shown in: `pixelAspect` applied
/// (a 720 x 480 DV picture with 10:11 pixels is shown 654 wide). Rec.709 for 720 lines and up, Rec.601 below, limited range.
/// Errors: InvalidArgument (an unusable frame), EncoderError.
std::expected<PreviewImage, Error> makePreview(const CaptureFrame& frame, Rate pixelAspect, int maxWidth);

} // namespace wabcut::capture

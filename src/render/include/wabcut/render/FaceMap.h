#pragma once

// The face map of AI Face Retouch (0.7) as the picture the compositor reads: three coverage planes (skin, the inside of the
// mouth, the eyes; 0..255) as the red, green and blue of an opaque raster. The raster is read with the sRGB curve, so the bytes
// are the sRGB encoding of the coverage: the effect's shader then sees the coverage itself as linear red, green and blue.

#include <wabcut/render/Title.h>

#include <cstdint>
#include <memory>
#include <span>

namespace wabcut::render {

/// width x height RGBA8 (alpha 255) from `coverage` (width x height x 3 bytes: skin, mouth, eyes; rows top-down). A size that does
/// not match makes an empty (all 0) map.
std::shared_ptr<const TitleRaster> makeFaceMapRaster(int width, int height, std::span<const std::uint8_t> coverage);

/// The byte whose sRGB decoding is coverage `c` / 255 (the encoding makeFaceMapRaster applies).
std::uint8_t faceMapByte(std::uint8_t coverage);

} // namespace wabcut::render

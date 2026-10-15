#pragma once

// Reference pictures of the synthetic media items (C.15, model/Synthetic.h): docs/internal/c15-parity-handoff.md, "Synthetic
// media and adjustment items".

#include <wabcut/model/Synthetic.h>
#include <wabcut/render/reference/Image.h>

namespace wabcut::render::reference {

/// Color Matte: every pixel (toLinear(r), toLinear(g), toLinear(b), 1). The colour is in the sequence's encoding (0..1,
/// clamped), converted with the working space's BT.709 curve (toLinear), so an export shows exactly `encodedColor`.
Image colorMatte(int width, int height, const Rgb& encodedColor);
/// Black Video: every pixel (0, 0, 0, 1).
Image blackVideo(int width, int height);
/// Transparent Video: every pixel (0, 0, 0, 0); only its effects draw.
Image transparentVideo(int width, int height);

/// The picture of a synthetic media item (syntheticOf): Color Matte with its color.r/g/b (syntheticParam: the defaults
/// when missing), Black Video, Transparent Video. An empty Image for any other item, adjustment layers included (they
/// have no picture of their own).
Image syntheticPicture(const MediaItem& item, int width, int height);

} // namespace wabcut::render::reference

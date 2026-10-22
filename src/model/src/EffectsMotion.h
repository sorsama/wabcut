#pragma once

// The motion features' descriptors (P.2): kept apart from Effects.cpp's catalog so the two grow without touching each
// other; EffectRegistry::builtin() appends them.

#include <wabcut/model/Effects.h>

#include <vector>

namespace wabcut::detail {

/// Morph Cut (video transition), Warp Stabilizer and Rolling Shutter Repair (Distort).
void appendMotionDescriptors(std::vector<EffectDescriptor>& out);

} // namespace wabcut::detail

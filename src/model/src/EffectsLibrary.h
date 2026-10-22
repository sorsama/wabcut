#pragma once

// Premiere Pro's effect and transition library (0.2, docs/graphics.md "Effects and transitions library"): the
// descriptors of the video transitions and video effects beyond the finishing shelf, appended to the built-in registry
// (Effects.cpp). Data only: the compositor implements them (render/EffectLibrary.h).

#include <wabcut/model/Effects.h>

#include <vector>

namespace wabcut::detail {

/// Premiere's video transitions by bin (Dissolve, Iris, Page Peel, Slide, Wipe, Zoom) and the creator transitions.
std::vector<EffectDescriptor> libraryTransitionDescriptors();
/// Premiere's video effects by bin (Adjust, Blur & Sharpen, Channel, Color Correction, Distort, ...).
std::vector<EffectDescriptor> libraryVideoEffectDescriptors();

} // namespace wabcut::detail

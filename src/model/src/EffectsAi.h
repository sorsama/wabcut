#pragma once

// The AI video effects' descriptors (0.7, wabcut/model/AiEffects.h): kept apart from Effects.cpp's catalog;
// EffectRegistry::builtin() appends them.

#include <wabcut/model/Effects.h>

#include <vector>

namespace wabcut::detail {

/// Restore and Denoise (category "AI").
void appendAiDescriptors(std::vector<EffectDescriptor>& out);

} // namespace wabcut::detail

#pragma once

// Internal (p02 audio): Premiere Pro's audio effects in the built-in EffectRegistry, in three parts so each family's
// descriptors live next to each other (Effects.cpp appends them to the catalog). Every descriptor and parameter table
// must have static storage: the registry keeps spans and string views into them.
//
// Ids are "wabcut.audio.<name>" and never change; names, categories, parameter names, units, ranges and defaults follow
// Premiere Pro 26's Effects panel (Audio Effects). The DSP is in wabcut_audio (audio/effects/EffectChain.h and
// src/effects/*.cpp); docs/audio.md lists them.

#include <wabcut/model/Effects.h>

#include <vector>

namespace wabcut::audiocatalog {

/// Amplitude and Compression, Special (dynamics, distortion, channel tools), Time and Pitch (Pitch Shifter).
void appendAmplitudeEffects(std::vector<EffectDescriptor>& out);
/// Filter and EQ, Stereo Imagery and the simple channel effects (Balance, Bass, Treble, ...).
void appendFilterEffects(std::vector<EffectDescriptor>& out);
/// Delay and Echo, Modulation, Reverb, Noise Reduction/Restoration.
void appendTimeEffects(std::vector<EffectDescriptor>& out);

} // namespace wabcut::audiocatalog

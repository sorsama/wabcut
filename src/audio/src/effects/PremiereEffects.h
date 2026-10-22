#pragma once

// Internal (p02 audio): the processors of Premiere Pro's audio effects (model: src/model/src/AudioEffectCatalog.h), in
// three families, each implemented in its own file. EffectChain.cpp asks each family whether it runs an effect id and
// to build its processor. Processors follow EffectChain.h's rules: construction allocates (and fixes the
// non-animatable parameters), process() and reset() are RT-safe, latencyFrames() is constant for the processor's life.

#include <wabcut/audio/effects/EffectChain.h>

#include <memory>
#include <span>
#include <string_view>

namespace wabcut::audio::premiere {

/// Amplitude and Compression, Special, Time and Pitch (AmplitudeEffects.cpp).
bool isAmplitudeEffect(std::string_view id) noexcept;
std::unique_ptr<EffectProcessor> createAmplitudeProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                          std::span<const double> values);

/// Filter and EQ, Stereo Imagery, the channel effects (FilterEffects.cpp).
bool isFilterEffect(std::string_view id) noexcept;
std::unique_ptr<EffectProcessor> createFilterProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                       std::span<const double> values);

/// Delay and Echo, Modulation, Reverb, Noise Reduction/Restoration (TimeEffects.cpp).
bool isTimeEffect(std::string_view id) noexcept;
std::unique_ptr<EffectProcessor> createTimeProcessor(const EffectDescriptor& descriptor, int channels, int sampleRate,
                                                     std::span<const double> values);

/// Index of a parameter in the descriptor's order (the descriptors are fixed data: a miss is a programming error the
/// tests catch; it reads the first parameter rather than out of bounds).
inline int paramIndex(const EffectDescriptor& d, std::string_view key) noexcept {
    for (std::size_t i = 0; i < d.params.size(); ++i) {
        if (d.params[i].key == key) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

} // namespace wabcut::audio::premiere

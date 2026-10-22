// Fuzz harness: effect presets (0.2, model/EffectPresets.h) - the user's effects library file and exported preset files
// from anyone, so untrusted. The first byte picks the reader: '0' the effects library, '1' a presets file, '2' one
// preset. Whatever reads passes its check (effectsLibraryProblem, effectPresetProblem) and comes back the same through
// its writer.

#include "FuzzSupport.h"

#include <wabcut/model/EffectPresets.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::string_view text = fuzz::text(data + 1, size - 1);
    switch (data[0] % 3) {
    case 0: {
        const auto library = io::effectsLibraryFromJson(text);
        if (library) {
            WABCUT_FUZZ_CHECK(!effectsLibraryProblem(*library), effectsLibraryProblem(*library).value_or(""));
            const auto again = io::effectsLibraryFromJson(io::effectsLibraryToJson(*library));
            WABCUT_FUZZ_CHECK(again && *again == *library, again ? std::string("library round trip") : again.error().message);
        }
        break;
    }
    case 1: {
        const auto presets = io::effectPresetsFileFromJson(text);
        if (presets) {
            for (const EffectPreset& p : *presets) {
                WABCUT_FUZZ_CHECK(!effectPresetProblem(p), effectPresetProblem(p).value_or(""));
            }
            const auto again = io::effectPresetsFileFromJson(io::effectPresetsFileToJson(*presets));
            WABCUT_FUZZ_CHECK(again && *again == *presets, again ? std::string("presets round trip") : again.error().message);
        }
        break;
    }
    default: {
        const auto preset = io::effectPresetFromJson(text);
        if (preset) {
            WABCUT_FUZZ_CHECK(!effectPresetProblem(*preset), effectPresetProblem(*preset).value_or(""));
            const auto again = io::effectPresetFromJson(io::effectPresetToJson(*preset));
            WABCUT_FUZZ_CHECK(again && *again == *preset, again ? std::string("preset round trip") : again.error().message);
        }
        break;
    }
    }
    return 0;
}

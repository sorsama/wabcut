// Wabcut effects in FCP7 XML (EffectMap.h).

#include "EffectMap.h"
#include "TextUtil.h"

namespace wabcut::interchange::detail {

const NativeEffect* nativeEffectFromFcp(std::string_view effectId, std::string_view name) {
    for (const NativeEffect& e : kNativeEffects) {
        if (iequals(effectId, e.fcpId) || iequals(name, e.fcpName)) {
            return &e;
        }
    }
    // Premiere's own names for the same filters.
    if (iequals(name, "Gaussian Blur") || iequals(effectId, "gaussian blur")) {
        return &kNativeEffects[0];
    }
    return nullptr;
}

} // namespace wabcut::interchange::detail

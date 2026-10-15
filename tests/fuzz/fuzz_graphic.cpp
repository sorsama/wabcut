// Fuzz harness: motion graphics JSON (C.13) - a clip's graphic, a graphic command's layer, a brand kit or a template
// library file, chosen by the first byte. What reads must pass the model's checks, come back the same through its JSON,
// and resolve to a draw list of finite numbers at any time.

#include "FuzzSupport.h"

#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicResolve.h>

#include <cmath>

using namespace wabcut;

namespace {

bool finite(const Affine2D& m) {
    for (const double v : {m.a, m.b, m.c, m.d, m.e, m.f}) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

void resolveAt(const Graphic& g, const BrandKit* kit) {
    ResolveOptions options;
    options.brandKit = kit;
    for (const Ticks t : {Ticks{}, Ticks{kTicksPerSecond / 3}, Ticks::fromSeconds(5), Ticks{-kTicksPerSecond}}) {
        const DrawList list = resolveGraphic(g, t, options);
        for (const DrawItem& item : list.items) {
            WABCUT_FUZZ_CHECK(finite(item.transform), "a draw item's transform is not finite");
            WABCUT_FUZZ_CHECK(std::isfinite(item.opacity) && item.opacity >= 0.0 && item.opacity <= 1.0, "opacity");
            for (const DrawCluster& c : item.text.clusters) {
                WABCUT_FUZZ_CHECK(std::isfinite(c.opacity) && std::isfinite(c.scale) && std::isfinite(c.offsetX), "cluster");
            }
        }
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::string_view text = fuzz::text(data + 1, size - 1);
    switch (data[0] % 4) {
    case 0: {
        const auto g = io::graphicFromJson(text);
        if (!g) {
            return 0;
        }
        WABCUT_FUZZ_CHECK(!graphicProblem(*g), graphicProblem(*g).value_or(""));
        const auto again = io::graphicFromJson(io::graphicToJson(*g));
        WABCUT_FUZZ_CHECK(again && *again == *g, again ? std::string("graphic round trip") : again.error().message);
        resolveAt(*g, nullptr);
        break;
    }
    case 1: {
        const auto layer = io::layerFromJson(text);
        if (!layer) {
            return 0;
        }
        const auto again = io::layerFromJson(io::layerToJson(*layer));
        WABCUT_FUZZ_CHECK(again && *again == *layer, again ? std::string("layer round trip") : again.error().message);
        Graphic g;
        g.layers.push_back(*layer);
        g.layers.back().id = 1;
        g.layers.back().pin.reset();
        if (!graphicProblem(g)) {
            resolveAt(g, nullptr);
        }
        break;
    }
    case 2: {
        const auto kit = io::brandKitFromJson(text);
        if (!kit) {
            return 0;
        }
        WABCUT_FUZZ_CHECK(!brandKitProblem(*kit), brandKitProblem(*kit).value_or(""));
        const auto again = io::brandKitFromJson(io::brandKitToJson(*kit));
        WABCUT_FUZZ_CHECK(again && *again == *kit, again ? std::string("brand kit round trip") : again.error().message);
        break;
    }
    default: {
        const auto tmpl = io::templateFileFromJson(text);
        if (!tmpl) {
            return 0;
        }
        WABCUT_FUZZ_CHECK(!templateProblem(*tmpl), templateProblem(*tmpl).value_or(""));
        const auto again = io::templateFileFromJson(io::templateFileToJson(*tmpl));
        WABCUT_FUZZ_CHECK(again && *again == *tmpl, again ? std::string("template round trip") : again.error().message);
        resolveAt(tmpl->graphic, nullptr);
        break;
    }
    }
    return 0;
}

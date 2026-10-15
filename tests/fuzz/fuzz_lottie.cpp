// Fuzz harness: Lottie files (C.13, interchange/Lottie.h) - downloaded animations, so untrusted. An import either fails
// with a ParseError that is not an importer bug ("internal: ..."), or gives a graphic that passes graphicProblem, comes
// back the same through the graphic JSON and resolves to finite draw lists.

#include "FuzzSupport.h"

#include <wabcut/interchange/Lottie.h>
#include <wabcut/model/GraphicJson.h>
#include <wabcut/model/GraphicResolve.h>

#include <cmath>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto imported = interchange::importLottie(fuzz::text(data, size), "fuzz.json");
    if (!imported) {
        WABCUT_FUZZ_CHECK(!imported.error().message.starts_with("internal"), imported.error().message);
        return 0;
    }
    const Graphic& g = imported->graphic;
    WABCUT_FUZZ_CHECK(!graphicProblem(g), graphicProblem(g).value_or(""));
    WABCUT_FUZZ_CHECK(imported->duration.value > 0 && imported->frameRate.isValid(), "duration and rate");
    const auto again = io::graphicFromJson(io::graphicToJson(g));
    WABCUT_FUZZ_CHECK(again && *again == g, again ? std::string("graphic round trip") : again.error().message);
    for (const Ticks t : {Ticks{}, Ticks{imported->duration.value / 2}, imported->duration}) {
        for (const DrawItem& item : resolveGraphic(g, t).items) {
            for (const double v : {item.transform.a, item.transform.b, item.transform.c, item.transform.d, item.transform.e,
                                   item.transform.f, item.opacity}) {
                WABCUT_FUZZ_CHECK(std::isfinite(v), "a draw item has a number that is not finite");
            }
        }
    }
    return 0;
}

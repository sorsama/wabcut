// Fuzz harness: a .cube LUT (color/Lut.h; owner: colour/integration). Harness only: crashes go to the owner through the
// coordinator. writeCube promises that parseCube reads its output back exactly, so a parsed LUT written, parsed and
// written again gives the same text.

#include "FuzzSupport.h"

#include <wabcut/color/Lut.h>

using namespace wabcut;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto lut = color::parseCube(fuzz::text(data, size));
    if (!lut) {
        return 0;
    }
    const std::string written = color::writeCube(*lut);
    const auto again = color::parseCube(written);
    WABCUT_FUZZ_CHECK(again, (again ? std::string() : again.error().message) + "\n" + written.substr(0, 2000));
    WABCUT_FUZZ_CHECK(color::writeCube(*again) == written, written.substr(0, 2000));
    return 0;
}

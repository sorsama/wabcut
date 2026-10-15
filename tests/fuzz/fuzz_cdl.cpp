// Fuzz harness: ASC CDL files (.cc, .ccc, .cdl XML; color/Cdl.h; owner: colour/integration). Harness only: crashes go
// to the owner through the coordinator. Corrections that parse are written as .ccc and must parse back to the same.

#include "FuzzSupport.h"

#include <wabcut/color/Cdl.h>

#include <cmath>

using namespace wabcut;

namespace {

bool finite(const color::Cdl& c) {
    for (const double v : {c.slope[0], c.slope[1], c.slope[2], c.offset[0], c.offset[1], c.offset[2], c.power[0],
                           c.power[1], c.power[2], c.saturation}) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto parsed = color::parseCdl(fuzz::text(data, size));
    if (!parsed || parsed->empty()) {
        return 0;
    }
    for (const color::Cdl& c : *parsed) {
        WABCUT_FUZZ_CHECK(finite(c), "a parsed CDL value is not finite: " + c.id);
    }
    const std::string written = color::writeCdl(*parsed, color::CdlFormat::Ccc);
    const auto again = color::parseCdl(written);
    WABCUT_FUZZ_CHECK(again, (again ? std::string() : again.error().message) + "\n" + written.substr(0, 2000));
    WABCUT_FUZZ_CHECK(color::writeCdl(*again, color::CdlFormat::Ccc) == written, written.substr(0, 2000));
    return 0;
}

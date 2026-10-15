// Fuzz harness: a Premiere Pro keyboard shortcut set (.kys XML, keymap/KysImport.h; owner: keymap). Harness only:
// crashes go to the keymap owner through the coordinator.

#include "FuzzSupport.h"

#include <wabcut/keymap/KysImport.h>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto parsed = wabcut::keymap::parseKys(wabcut::fuzz::text(data, size));
    (void)parsed;
    return 0;
}
